#include "stream_player_controller.hpp"

#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <thread>
#include <utility>

#include "ac3/audio/monitor.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/encoder/plan.hpp"
#include "ac3/io/wav.hpp"
#include "channel_geometry.hpp"

using splayer_detail::RawResult;

namespace {

QString to_qstring(std::string_view sv) {
    return QString::fromUtf8(sv.data(), static_cast<qsizetype>(sv.size()));
}

// -60 dBFS, not ac3::analysis::kFloorDb's -120: the same GUI meter-bar floor
// EncoderController::kMeterFloorDb uses, so a bar on this dialog and a bar on
// the encode workbench read the same level the same way.
constexpr double kMeterFloorDb = -60.0;

// Chunk size for both the playback worker's submit() calls and its metering
// - small enough for a responsive seek/pause, the same 2048 frames every
// other MonitorSink submit loop in this window already chunks by
// (ObjectDecodeController::auditionObject).
constexpr std::size_t kChunkFrames = 2048;

// ~30 Hz, matching EncoderController's own kPublishInterval - fast enough to
// read as live, slow enough not to flood the GUI thread with invokeMethod
// calls.
constexpr auto kPublishInterval = std::chrono::milliseconds(33);

bool layout_has_lfe(const ac3::eac3::chanmap::Layout& layout) {
    using ac3::eac3::chanmap::Location;
    return layout.index_of(Location::kLfe) >= 0 || layout.index_of(Location::kLfe2) >= 0;
}

QString channel_label(const std::vector<ac3::eac3::chanmap::Location>& locations, std::size_t at) {
    if (at < locations.size()) {
        return to_qstring(ac3::eac3::chanmap::name(locations[at]));
    }
    // Dual mono: no Table E2.5 location, so no name() to borrow - Ch1/Ch2,
    // the same coded-order labels the rest of this window uses for 1+1.
    return QStringLiteral("Ch%1").arg(at + 1);
}

// Appends one access unit/frame's planar channels into `dst`, permuted by
// `order` (order[i] names which of `src`'s channels belongs at position i) -
// the accumulate-as-you-go counterpart of apps/cli/support.hpp's
// interleave_reordered, kept planar here rather than interleaved so the
// result is already write_wav_f32's own input shape.
void append_planar(std::vector<std::vector<float>>& dst,
                    const std::vector<std::vector<float>>& src, std::span<const std::size_t> order) {
    if (dst.empty()) {
        dst.assign(order.size(), {});
    }
    for (std::size_t i = 0; i < order.size() && i < dst.size(); ++i) {
        const auto& channel = src[order[i]];
        dst[i].insert(dst[i].end(), channel.begin(), channel.end());
    }
}

struct DecodeOutcome {
    QString error;  // empty on success
    std::shared_ptr<RawResult> result;
};

// Reads the whole file, dispatches on bsid and decodes every frame/access
// unit into whole-file planar buffers - the GUI twin of `ac3cli monitor`
// (bed playback) fused with `ac3cli decode`'s object export (objects_dir),
// since this dialog offers both from one decode pass rather than two. Runs
// entirely on the calling thread; openFile() is what moves this off the GUI
// thread, mirroring ObjectDecodeController::inspectFile/
// QcController::measureFile.
DecodeOutcome decode_stream_to_memory(const QString& path) {
    DecodeOutcome outcome;
    std::ifstream in{path.toStdString(), std::ios::binary};
    if (!in) {
        outcome.error = QStringLiteral("Could not open %1.").arg(path);
        return outcome;
    }
    const std::vector<char> raw{std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>()};
    if (raw.empty()) {
        outcome.error = QStringLiteral("%1 is empty.").arg(path);
        return outcome;
    }
    std::vector<std::byte> stream(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        stream[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
    }

    const auto bsid = ac3::stream_bsid(stream);
    if (!bsid) {
        outcome.error = QStringLiteral("%1 is too short to hold a syncframe.").arg(path);
        return outcome;
    }

    auto result = std::make_shared<RawResult>();

    if (*bsid > 8) {
        const auto units = ac3::split_access_units(stream);
        if (!units || units->empty()) {
            outcome.error = QStringLiteral("%1 is not a valid E-AC-3 stream.").arg(path);
            return outcome;
        }
        result->codec_label = QStringLiteral("E-AC-3");
        ac3::Eac3Decoder decoder;
        std::vector<std::size_t> order;
        bool have_first = false;
        double time_s = 0.0;

        const auto ingest = [&](const ac3::DecodedAccessUnit& out) {
            if (!have_first) {
                have_first = true;
                result->sample_rate_hz = ac3::sample_rate_hz(out.sample_rate);
                result->acmod = out.acmod;
                result->lfe = layout_has_lfe(out.layout);
                result->layout_label =
                    to_qstring(ac3::analysis::layout_name(result->acmod, result->lfe));
                if (out.layout.count > 0) {
                    result->locations.assign(out.layout.begin(), out.layout.end());
                    order = ac3::plan::monitor_order(
                        std::span{out.layout.items}.first(static_cast<std::size_t>(out.layout.count)),
                        out.channels.size());
                } else {
                    // Dual mono: no Table E2.5 layout - identity, coded order.
                    order.resize(out.channels.size());
                    for (std::size_t i = 0; i < order.size(); ++i) {
                        order[i] = i;
                    }
                }
            }
            if (result->sample_rate_hz > 0) {
                time_s += static_cast<double>(ac3::kSamplesPerFrame) /
                          static_cast<double>(result->sample_rate_hz);
            }
            ++result->unit_count;
            append_planar(result->channels, out.channels, order);

            if (out.object_metadata && !out.object_audio.empty()) {
                result->has_objects = true;
                result->object_count = static_cast<int>(out.object_audio.size());
                if (result->object_audio.size() != out.object_audio.size()) {
                    result->object_audio.assign(out.object_audio.size(), {});
                }
                for (std::size_t i = 0; i < out.object_audio.size(); ++i) {
                    auto& dst = result->object_audio[i];
                    dst.insert(dst.end(), out.object_audio[i].begin(), out.object_audio[i].end());
                }
            }
        };

        for (const auto& unit : *units) {
            const auto decoded = decoder.decode_access_unit(unit);
            if (!decoded) {
                outcome.error = QStringLiteral("Decode failed (code %1).")
                                     .arg(static_cast<int>(decoded.error()));
                return outcome;
            }
            if (decoded->has_value()) {
                ingest(**decoded);
            }
        }
        for (const auto& sub : decoder.flush()) {
            // flush() returns raw per-substream results, not an assembled
            // DecodedAccessUnit (Eac3Decoder::flush's own header comment) -
            // only the independent substream carries the bed audio the
            // player needs; a dependent's trailing tail with nothing to
            // extend is not something this whole-file-in-memory player
            // reconstructs a partial programme from. Also skipped whenever
            // the flushed substream's own width does not match what every
            // other append_planar call this file already committed to
            // (e.g. a wide programme whose dependents never separately
            // flush): every entry of result->channels must stay the same
            // length, since playback indexes them in lockstep, and one
            // held-back frame is not worth risking that for.
            if (sub.strmtyp == ac3::eac3::StreamType::kDependent || !have_first ||
                sub.channels.size() != result->channels.size()) {
                continue;
            }
            append_planar(result->channels, sub.channels, order);
        }

        if (!have_first) {
            outcome.error = QStringLiteral("%1 carried no independent substream.").arg(path);
            return outcome;
        }
        result->duration_seconds = time_s;
    } else {
        const auto frames = ac3::split_frames(stream);
        if (!frames || frames->empty()) {
            outcome.error = QStringLiteral("%1 is not a valid AC-3 stream.").arg(path);
            return outcome;
        }
        result->codec_label = QStringLiteral("AC-3");
        ac3::FrameDecoder decoder;
        std::vector<std::size_t> order;
        bool have_first = false;
        double time_s = 0.0;

        for (const auto& frame : *frames) {
            const auto decoded = decoder.decode_frame(frame);
            if (!decoded) {
                outcome.error =
                    QStringLiteral("%1: %2").arg(path, to_qstring(ac3::describe(decoded.error())));
                return outcome;
            }
            if (!have_first) {
                have_first = true;
                result->sample_rate_hz = ac3::sample_rate_hz(decoded->sample_rate);
                result->acmod = decoded->acmod;
                result->lfe = decoded->lfe;
                result->layout_label =
                    to_qstring(ac3::analysis::layout_name(result->acmod, result->lfe));
                result->locations = ac3gui::ac3_bed_locations(result->acmod, result->lfe);
                if (decoded->acmod == ac3::Acmod::kDualMono) {
                    order.resize(decoded->channels.size());
                    for (std::size_t i = 0; i < order.size(); ++i) {
                        order[i] = i;
                    }
                } else {
                    order = ac3::io::wav_channel_order(decoded->acmod, decoded->lfe);
                }
            }
            if (result->sample_rate_hz > 0) {
                time_s += static_cast<double>(ac3::kSamplesPerFrame) /
                          static_cast<double>(result->sample_rate_hz);
            }
            ++result->unit_count;
            append_planar(result->channels, decoded->channels, order);
        }
        if (!have_first) {
            outcome.error = QStringLiteral("%1 carried no frames.").arg(path);
            return outcome;
        }
        result->duration_seconds = time_s;
    }

    result->frame_count = result->channels.empty() ? 0 : result->channels.front().size();
    outcome.result = std::move(result);
    return outcome;
}

}  // namespace

StreamPlayerController::StreamPlayerController(QObject* parent) : QObject(parent) {}

// Out-of-line even though it is just = default: sink_ is a
// unique_ptr<ac3::audio::MonitorSink>, forward-declared in the header - see
// ObjectDecodeController's own identical ~ObjectDecodeController() comment.
StreamPlayerController::~StreamPlayerController() = default;

QString StreamPlayerController::summaryLine() const {
    if (!result_) {
        return QString();
    }
    return QStringLiteral("%1 · %2 · %3 Hz · %4 frame(s) · %5 s")
        .arg(result_->codec_label, result_->layout_label)
        .arg(result_->sample_rate_hz)
        .arg(result_->unit_count)
        .arg(result_->duration_seconds, 0, 'f', 2);
}

double StreamPlayerController::positionSeconds() const {
    if (!result_ || result_->sample_rate_hz == 0) {
        return 0.0;
    }
    return static_cast<double>(read_frame_.load(std::memory_order_relaxed)) /
           static_cast<double>(result_->sample_rate_hz);
}

QVariantList StreamPlayerController::channelMeta() const {
    QVariantList out;
    if (!result_) {
        return out;
    }
    out.reserve(static_cast<qsizetype>(result_->channels.size()));
    for (std::size_t at = 0; at < result_->channels.size(); ++at) {
        const bool has_location = at < result_->locations.size();
        const auto azimuth =
            has_location ? ac3gui::location_azimuth_deg(result_->locations[at]) : std::nullopt;
        out.append(QVariantMap{
            {QStringLiteral("name"), channel_label(result_->locations, at)},
            {QStringLiteral("azimuthDeg"), azimuth.value_or(0.0)},
            {QStringLiteral("directional"), azimuth.has_value()},
            {QStringLiteral("ceiling"),
             has_location && ac3gui::is_ceiling_location(result_->locations[at])},
            {QStringLiteral("replaced"), false},
            {QStringLiteral("fed"), true},
        });
    }
    return out;
}

double StreamPlayerController::meterFloorDb() const { return kMeterFloorDb; }

void StreamPlayerController::publishLevels(const splayer_detail::RawResult& source,
                                           std::span<const ac3::analysis::ChannelLevel> levels) {
    clip_latched_.resize(levels.size(), false);

    QVariantList entries;
    entries.reserve(static_cast<qsizetype>(levels.size()));
    for (std::size_t ch = 0; ch < levels.size(); ++ch) {
        const auto& level = levels[ch];
        const bool has_location = ch < source.locations.size();
        const auto location =
            has_location ? source.locations[ch] : ac3::eac3::chanmap::Location::kLeft;
        const auto azimuth = has_location ? ac3gui::location_azimuth_deg(location) : std::nullopt;
        const bool ceiling = has_location && ac3gui::is_ceiling_location(location);
        clip_latched_[ch] = clip_latched_[ch] || level.clipped;
        entries.append(QVariantMap{
            {QStringLiteral("peakDb"), level.peak_db},
            {QStringLiteral("rmsDb"), level.rms_db},
            {QStringLiteral("holdDb"), level.hold_db},
            {QStringLiteral("clipped"), static_cast<bool>(clip_latched_[ch])},
            {QStringLiteral("peak"), ac3::analysis::meter_fraction(level.peak_db, kMeterFloorDb)},
            {QStringLiteral("rms"), ac3::analysis::meter_fraction(level.rms_db, kMeterFloorDb)},
            {QStringLiteral("hold"), ac3::analysis::meter_fraction(level.hold_db, kMeterFloorDb)},
            {QStringLiteral("azimuthDeg"), azimuth.value_or(0.0)},
            {QStringLiteral("directional"), azimuth.has_value()},
            {QStringLiteral("ceiling"), ceiling},
            {QStringLiteral("replaced"), false},
            {QStringLiteral("fed"), true},
        });
    }
    channel_levels_ = std::move(entries);

    const auto field = ac3::analysis::energy_vector(levels, source.acmod);
    soundfield_ = QVariantMap{
        {QStringLiteral("azimuthDeg"), field.azimuth_deg},
        {QStringLiteral("magnitude"), field.magnitude},
        {QStringLiteral("levelDb"), field.level_db},
        {QStringLiteral("active"), field.magnitude > 0.0},
    };
    emit levelsChanged();
}

void StreamPlayerController::openFile(const QUrl& url) {
    if (busy_) {
        return;
    }
    pause();
    const QString path = url.toLocalFile();
    if (path.isEmpty()) {
        return;
    }
    file_path_ = path;
    emit filePathChanged();
    busy_ = true;
    emit busyChanged();

    std::ignore = QtConcurrent::run([this, path] {
        auto outcome = decode_stream_to_memory(path);
        QMetaObject::invokeMethod(this, [this, outcome = std::move(outcome)]() mutable {
            busy_ = false;
            error_ = outcome.error;
            result_ = outcome.error.isEmpty() ? std::move(outcome.result) : nullptr;
            read_frame_.store(0, std::memory_order_relaxed);
            clip_latched_.clear();
            channel_levels_ = QVariantList();
            soundfield_ = QVariantMap();
            emit busyChanged();
            emit resultChanged();
            emit positionChanged();
            emit levelsChanged();
        });
    });
}

void StreamPlayerController::play() {
    if (!result_ || busy_ || result_->channels.empty() || worker_active_) {
        return;
    }
    if (!sink_) {
        sink_ = std::make_unique<ac3::audio::MonitorSink>();
        const auto started = sink_->start(std::string{}, result_->sample_rate_hz,
                                          static_cast<std::uint16_t>(result_->channels.size()));
        if (!started) {
            const auto why = ac3::audio::describe(started.error());
            sink_.reset();
            error_ = QStringLiteral("Could not open the playback output: %1").arg(to_qstring(why));
            emit resultChanged();
            return;
        }
    }

    should_play_.store(true, std::memory_order_relaxed);
    worker_active_ = true;
    playing_ = true;
    emit playingChanged();

    // The worker keeps its own shared_ptr to the result it started with -
    // see splayer_detail::RawResult's own header comment on why a raw
    // reference into result_ here would be a dangling-reference hazard if
    // openFile() loaded a second file while this worker is still mid-flight.
    std::ignore = QtConcurrent::run([this, result = result_] {
        std::uint64_t at = read_frame_.load(std::memory_order_relaxed);
        const std::size_t channels = result->channels.size();
        ac3::analysis::LevelMeter meter{result->acmod, result->lfe, result->sample_rate_hz,
                                        static_cast<int>(channels)};
        auto published_at = std::chrono::steady_clock::now() - kPublishInterval;
        std::vector<float> chunk(kChunkFrames * channels);
        std::vector<std::span<const float>> views(channels);

        while (should_play_.load(std::memory_order_relaxed)) {
            if (seek_pending_.exchange(false, std::memory_order_relaxed)) {
                at = seek_target_.load(std::memory_order_relaxed);
            }
            if (at >= result->frame_count) {
                break;
            }
            const auto len = std::min<std::uint64_t>(kChunkFrames, result->frame_count - at);
            chunk.resize(len * channels);
            for (std::size_t ch = 0; ch < channels; ++ch) {
                views[ch] = std::span{result->channels[ch]}.subspan(at, len);
                for (std::uint64_t f = 0; f < len; ++f) {
                    chunk[f * channels + ch] = result->channels[ch][at + f];
                }
            }
            meter.process(views);

            bool submitted = false;
            while (!submitted && should_play_.load(std::memory_order_relaxed)) {
                submitted = sink_->submit(chunk);
                if (!submitted) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(4));
                }
            }
            if (!submitted) {
                break;
            }
            at += len;
            read_frame_.store(at, std::memory_order_relaxed);

            const auto now = std::chrono::steady_clock::now();
            if (now - published_at >= kPublishInterval) {
                published_at = now;
                std::vector<ac3::analysis::ChannelLevel> snapshot(meter.levels().begin(),
                                                                   meter.levels().end());
                QMetaObject::invokeMethod(this, [this, result, snapshot = std::move(snapshot)] {
                    // result_ may have moved on to a different file while
                    // this was queued (openFile() only asks should_play_ to
                    // go false - it does not wait for this worker to notice
                    // before starting its own decode) - a superseded
                    // worker's stale position/levels must never overwrite
                    // the new file's own, so this checks identity against
                    // the exact shared_ptr this worker started with rather
                    // than trusting result_ still means the same file.
                    if (result_ != result) {
                        return;
                    }
                    emit positionChanged();
                    publishLevels(*result, snapshot);
                });
            }
        }

        const bool reached_end = at >= result->frame_count;
        QMetaObject::invokeMethod(this, [this, result, reached_end] {
            // sink_/worker_active_ are exclusively this worker's to tear
            // down regardless of which file is current: play() refuses to
            // spawn a second worker while worker_active_ is true, so sink_
            // is still exactly the MonitorSink this worker itself opened.
            worker_active_ = false;
            should_play_.store(false, std::memory_order_relaxed);
            if (sink_) {
                sink_->stop();
                sink_.reset();
            }
            // Position/playing state, on the other hand, belongs to
            // whichever file is current - skip it entirely for a superseded
            // worker, same reasoning as the periodic publish above.
            if (result_ != result) {
                return;
            }
            playing_ = false;
            emit playingChanged();
            if (reached_end) {
                read_frame_.store(0, std::memory_order_relaxed);
            }
            emit positionChanged();
        });
    });
}

void StreamPlayerController::pause() {
    should_play_.store(false, std::memory_order_relaxed);
    if (playing_) {
        playing_ = false;
        emit playingChanged();
    }
}

void StreamPlayerController::seek(double seconds) {
    if (!result_ || result_->sample_rate_hz == 0) {
        return;
    }
    const double clamped = std::clamp(seconds, 0.0, result_->duration_seconds);
    const auto target =
        static_cast<std::uint64_t>(clamped * static_cast<double>(result_->sample_rate_hz));
    seek_target_.store(target, std::memory_order_relaxed);
    seek_pending_.store(true, std::memory_order_relaxed);
    if (!worker_active_) {
        // Nothing will consume seek_pending_ while paused - reflect the new
        // position immediately instead of waiting for a play() that may
        // never come.
        read_frame_.store(target, std::memory_order_relaxed);
        seek_pending_.store(false, std::memory_order_relaxed);
        emit positionChanged();
    }
}

void StreamPlayerController::clearClipLatch(int channel) {
    if (channel < 0 || static_cast<std::size_t>(channel) >= clip_latched_.size()) {
        return;
    }
    clip_latched_[static_cast<std::size_t>(channel)] = false;
    if (!channel_levels_.isEmpty() && channel < channel_levels_.size()) {
        auto entry = channel_levels_[channel].toMap();
        entry[QStringLiteral("clipped")] = false;
        channel_levels_[channel] = entry;
        emit levelsChanged();
    }
}

void StreamPlayerController::exportDecodedWav(const QUrl& url) {
    if (!result_ || exporting_) {
        return;
    }
    const QString path = url.toLocalFile();
    if (path.isEmpty()) {
        return;
    }
    exporting_ = true;
    export_error_.clear();
    emit exportingChanged();

    std::ignore = QtConcurrent::run([this, path, result = result_] {
        const auto written = ac3::io::write_wav_f32(path.toStdString(), result->channels,
                                                     result->sample_rate_hz);
        QString error;
        if (!written) {
            error = QStringLiteral("Could not write %1: %2")
                        .arg(path, to_qstring(ac3::io::describe(written.error())));
        }
        QMetaObject::invokeMethod(this, [this, error] {
            exporting_ = false;
            export_error_ = error;
            emit exportingChanged();
            emit exportFinished();
        });
    });
}

void StreamPlayerController::exportObjects(const QUrl& url) {
    if (!result_ || exporting_) {
        return;
    }
    if (!result_->has_objects || result_->object_audio.empty()) {
        export_error_ = QStringLiteral("This stream carries no Dolby Atmos objects to export.");
        emit exportFinished();
        return;
    }
    const QString dir = url.toLocalFile();
    if (dir.isEmpty()) {
        return;
    }
    exporting_ = true;
    export_error_.clear();
    emit exportingChanged();

    std::ignore = QtConcurrent::run([this, dir, result = result_] {
        QString error;
        std::filesystem::create_directories(dir.toStdString());
        for (std::size_t i = 0; i < result->object_audio.size(); ++i) {
            const auto object_path =
                QStringLiteral("%1/object_%2.wav")
                    .arg(dir, QString::number(i).rightJustified(2, QLatin1Char('0')));
            const std::vector<std::vector<float>> mono{result->object_audio[i]};
            const auto written =
                ac3::io::write_wav_f32(object_path.toStdString(), mono, result->sample_rate_hz);
            if (!written) {
                error = QStringLiteral("Could not write %1: %2")
                            .arg(object_path, to_qstring(ac3::io::describe(written.error())));
                break;
            }
        }
        QMetaObject::invokeMethod(this, [this, error] {
            exporting_ = false;
            export_error_ = error;
            emit exportingChanged();
            emit exportFinished();
        });
    });
}
