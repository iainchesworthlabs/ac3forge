#include "object_decode_controller.hpp"

#include <QVariantMap>
#include <QtConcurrent/QtConcurrentRun>

#include <chrono>
#include <cstddef>
#include <fstream>
#include <ios>
#include <iterator>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/oba/oamd.hpp"
#include "ac3/audio/monitor.hpp"
#include "ac3/core/eac3_tables.hpp"
#include "ac4/ac4.hpp"
#include "ac4_channels.hpp"
#include "ac4_presentations.hpp"
#include "ac4_sync_word.hpp"
#include "ac4dec/decoder.hpp"
#include "container_input.hpp"

using objdec_detail::RawFrame;
using objdec_detail::RawResult;

namespace {

QString to_qstring(std::string_view sv) {
    return QString::fromUtf8(sv.data(), static_cast<qsizetype>(sv.size()));
}

// The outcome of one inspectFile() attempt, handed back from the worker
// thread to the GUI thread in one QMetaObject::invokeMethod call - mirrors
// qc_controller.cpp's own MeasureOutcome.
struct InspectOutcome {
    QString error = QString();  // empty on success
    RawResult result = RawResult();
};

// Every access unit's own independent substream, whether or not it carried
// OAMD - object_metadata is std::nullopt for any frame arriving before the
// EMDF container is fully assembled or for a plain (non-Atmos) stream, and
// `ingest` simply contributes no RawFrame for those, exactly the way
// qc_controller.cpp's own dual-mono split contributes to only the
// programme(s) actually present.
std::optional<RawResult> measure_eac3_objects(std::span<const std::byte> stream, QString& error) {
    const auto frames = ac3::split_frames(stream);
    if (!frames || frames->empty()) {
        error = QStringLiteral("Not a valid E-AC-3 stream.");
        return std::nullopt;
    }
    ac3::Eac3Decoder decoder;
    RawResult result;
    result.codec_label = QStringLiteral("E-AC-3");

    bool have_first = false;
    double time_s = 0.0;

    const auto ingest = [&](const ac3::DecodedSubstream& sub) {
        if (sub.strmtyp == ac3::eac3::StreamType::kDependent) {
            return;  // object audio only ever rides in the independent bed
        }
        if (!have_first) {
            have_first = true;
            result.sample_rate_hz = sample_rate_hz(sub.sample_rate);
        }
        if (result.sample_rate_hz > 0) {
            time_s += static_cast<double>(ac3::kSamplesPerFrame) /
                      static_cast<double>(result.sample_rate_hz);
        }
        if (!sub.object_metadata) {
            return;
        }
        const auto& program = sub.object_metadata->program;
        // Every JOC output, not just the dynamic objects: a bed programme has
        // none of the latter and eleven of the former, and used to show as an
        // empty dialog.
        const auto described = ac3::oba::describe_objects(*sub.object_metadata);

        RawFrame f;
        f.time_s = time_s;
        f.x.reserve(described.size());
        f.y.reserve(described.size());
        f.z.reserve(described.size());
        f.gain_db.reserve(described.size());
        f.width.reserve(described.size());
        f.depth.reserve(described.size());
        f.height.reserve(described.size());
        f.snap.reserve(described.size());
        f.labels.reserve(described.size());
        for (const auto& object : described) {
            f.x.push_back(object.position.x);
            f.y.push_back(object.position.y);
            f.z.push_back(object.position.z);
            f.gain_db.push_back(object.gain_db);
            f.width.push_back(object.size.width);
            f.depth.push_back(object.size.depth);
            f.height.push_back(object.size.height);
            f.snap.push_back(object.snap);
            f.labels.push_back(QString::fromUtf8(object.label.data(),
                                                 static_cast<qsizetype>(object.label.size())));
        }
        result.frames.push_back(std::move(f));

        result.dynamic_object_count = static_cast<int>(described.size());
        result.dynamic_only = program.dynamic_only;
        result.has_lfe = ac3::oba::has_lfe(program);

        if (result.object_audio.size() != described.size()) {
            result.object_audio.assign(described.size(), {});
        }
        if (sub.object_audio.size() == described.size()) {
            for (std::size_t i = 0; i < described.size(); ++i) {
                auto& dst = result.object_audio[i];
                dst.insert(dst.end(), sub.object_audio[i].begin(), sub.object_audio[i].end());
            }
        }
    };

    for (const auto& frame : *frames) {
        const auto decoded = decoder.decode_substream(frame);
        if (!decoded) {
            error = QStringLiteral("Decode failed (code %1).")
                        .arg(static_cast<int>(decoded.error()));
            return std::nullopt;
        }
        if (decoded->has_value()) {
            ingest(**decoded);
        }
    }
    for (const auto& sub : decoder.flush()) {
        ingest(sub);
    }

    if (!have_first) {
        error = QStringLiteral("Stream carried no independent substream.");
        return std::nullopt;
    }
    if (result.frames.empty()) {
        error = QStringLiteral(
            "No Dolby Atmos object metadata (OAMD) found — this is a plain E-AC-3 stream.");
        return std::nullopt;
    }

    // dynamic_object_count is really "objects the dialog shows", which for a
    // bed programme is its channels - so the label has to say which it is
    // rather than call eleven bed channels eleven dynamic objects.
    if (result.dynamic_only) {
        result.layout_label = result.has_lfe
                                   ? QStringLiteral("%1 dynamic object(s) + LFE")
                                         .arg(result.dynamic_object_count)
                                   : QStringLiteral("%1 dynamic object(s)")
                                         .arg(result.dynamic_object_count);
    } else {
        result.layout_label =
            QStringLiteral("bed of %1 channel(s)").arg(result.dynamic_object_count);
    }
    result.duration_seconds = time_s;
    return result;
}

// AC-4: what ac4::Decoder reports of the stream, read-only - the table of
// contents' presentations, and for the one it chooses with no preference its
// channels and, frame by frame, its objects (Part 2 Annex F's properties, as
// DecodedFrame::objects gives them), a bed object at its speaker and labelled
// with it. Every frame's objects are recorded, and their audio kept for the
// dialog's audition as E-AC-3's JOC objects' is.
std::optional<RawResult> measure_ac4_objects(std::span<const std::byte> stream, QString& error) {
    const ac4::ScanResult scan = ac4::scan(stream);
    if (scan.frames.empty()) {
        error = QStringLiteral("Not a valid AC-4 stream.");
        return std::nullopt;
    }
    RawResult result;
    result.codec_label = QStringLiteral("AC-4");
    result.ac4 = true;
    for (const auto& row : ac3gui::ac4_presentation_rows(scan.frames)) {
        result.presentations.append(QString::fromStdString(row.label));
    }
    ac4::Decoder decoder;
    double time_s = 0.0;
    std::size_t number = 0;
    bool have_first = false;
    for (const ac4::SyncFrame& frame : scan.frames) {
        ++number;
        const auto decoded = decoder.decode(frame.raw_ac4_frame);
        if (!decoded.has_value()) {
            error = QStringLiteral("Frame %1: %2")
                        .arg(number)
                        .arg(to_qstring(decoder.refusal_reason()));
            return std::nullopt;
        }
        if (!decoded->has_value()) {
            continue;  // waiting for an I-frame
        }
        const ac4::DecodedFrame& pcm = **decoded;
        if (!have_first) {
            have_first = true;
            result.sample_rate_hz = static_cast<std::uint32_t>(pcm.sample_rate_hz);
            result.presentation = pcm.presentation;
            result.layout_label = to_qstring(ac3gui::ac4_speaker_names(pcm.speakers));
            result.has_lfe =
                std::ranges::find(pcm.speakers, ac4::Speaker::kLfe) != pcm.speakers.end();
        }
        time_s += static_cast<double>(pcm.samples) / static_cast<double>(pcm.sample_rate_hz);
        ++result.unit_count;
        if (pcm.objects.empty()) {
            continue;
        }
        RawFrame f;
        f.time_s = time_s;
        int beds = 0;
        for (const ac4::DecodedObject& object : pcm.objects) {
            const ac4::ObjectProperties& p = object.properties;
            f.x.push_back(p.position[0]);
            f.y.push_back(p.position[1]);
            f.z.push_back(p.position[2]);
            f.gain_db.push_back(p.gain_db);
            f.width.push_back(p.width[0]);
            f.depth.push_back(p.width[1]);
            f.height.push_back(p.width[2]);
            f.snap.push_back(p.snap);
            const bool bed = object.kind == ac4::ObjectKind::kBed;
            beds += bed ? 1 : 0;
            f.labels.push_back(bed && object.speaker
                                   ? to_qstring(ac3::eac3::chanmap::name(
                                         ac3::apps::ac4_location(*object.speaker)))
                                   : QString());
        }
        result.ac4_bed_objects = beds;
        result.ac4_dynamic_objects = static_cast<int>(pcm.objects.size()) - beds;
        result.dynamic_object_count = static_cast<int>(pcm.objects.size());
        result.dynamic_only = beds == 0;
        if (result.object_audio.size() != pcm.objects.size()) {
            result.object_audio.assign(pcm.objects.size(), {});
        }
        for (std::size_t i = 0; i < pcm.objects.size(); ++i) {
            auto& dst = result.object_audio[i];
            dst.insert(dst.end(), pcm.objects[i].samples.begin(), pcm.objects[i].samples.end());
        }
        result.frames.push_back(std::move(f));
    }
    if (!have_first) {
        error = QStringLiteral("No frame decoded; the stream sent no I-frame.");
        return std::nullopt;
    }
    result.duration_seconds = time_s;
    return result;
}

// Reads the whole file, dispatches on the AC-4 sync word and then on bsid
// (AC-3's bsid <= 8 never carries OAMD - it is an Annex E / E-AC-3-only tool)
// and decodes it. Runs entirely
// on the calling thread - inspectFile() below is what moves this off the GUI
// thread, mirroring qc_controller.cpp's own measure_file().
InspectOutcome inspect_file(const QString& path) {
    InspectOutcome outcome;
    std::ifstream in{path.toStdString(), std::ios::binary};
    if (!in) {
        outcome.error = QStringLiteral("Could not open %1.").arg(path);
        return outcome;
    }
    const std::vector<char> raw{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (raw.empty()) {
        outcome.error = QStringLiteral("%1 is empty.").arg(path);
        return outcome;
    }
    std::vector<std::byte> file_bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        file_bytes[i] = static_cast<std::byte>(static_cast<unsigned char>(raw[i]));
    }

    // container readers (mkv/mp4/ts): the file itself unchanged if it is not a container this
    // build reads, or the first AC-3/E-AC-3 track demuxed out of one - the
    // same sniff-and-demux ac3cli's own decode/qc/levels/play/monitor use.
    auto demuxed = ac3::apps::elementary_stream_from_bytes(file_bytes);
    if (!demuxed.error.empty()) {
        outcome.error =
            QStringLiteral("%1 is a %2").arg(path, QString::fromStdString(demuxed.error));
        return outcome;
    }
    const auto stream = std::move(demuxed.bytes);

    if (ac3::apps::is_ac4_stream(stream)) {
        QString error;
        auto measured = measure_ac4_objects(stream, error);
        if (!measured) {
            outcome.error = error;
            return outcome;
        }
        outcome.result = std::move(*measured);
        return outcome;
    }
    const auto bsid = ac3::stream_bsid(stream);
    if (!bsid) {
        outcome.error = QStringLiteral("%1 is too short to hold a syncframe.").arg(path);
        return outcome;
    }
    if (*bsid <= 8) {
        outcome.error = QStringLiteral(
            "AC-3 (bsid %1) carries no object metadata — Dolby Atmos rides only in E-AC-3.")
                             .arg(*bsid);
        return outcome;
    }

    QString error;
    auto measured = measure_eac3_objects(stream, error);
    if (!measured) {
        outcome.error = error;
        return outcome;
    }
    outcome.result = std::move(*measured);
    return outcome;
}

}  // namespace

ObjectDecodeController::ObjectDecodeController(QObject* parent) : QObject(parent) {}

// Out-of-line even though it is just = default: audition_sink_ is a
// unique_ptr<ac3::audio::MonitorSink>, and MonitorSink is only
// forward-declared in the header (see its own comment on why) - the
// destructor needs the complete type, which this translation unit's
// #include "ac3/audio/monitor.hpp" above provides. Same shape as
// EncoderController's own out-of-line ~EncoderController() = default for its
// analogous motion_preview_monitor_sink_.
ObjectDecodeController::~ObjectDecodeController() = default;

QString ObjectDecodeController::summaryLine() const {
    if (!result_) {
        return QString();
    }
    return QStringLiteral("%1 · %2 · %3 Hz · %4 frame(s) · %5 s")
        .arg(result_->codec_label, result_->layout_label)
        .arg(result_->sample_rate_hz)
        .arg(result_->ac4 ? result_->unit_count : result_->frames.size())
        .arg(result_->duration_seconds, 0, 'f', 2);
}

QVariantList ObjectDecodeController::frames() const {
    QVariantList out;
    if (!result_) {
        return out;
    }
    out.reserve(static_cast<qsizetype>(result_->frames.size()));
    for (const auto& f : result_->frames) {
        QVariantList objects;
        objects.reserve(static_cast<qsizetype>(f.x.size()));
        for (std::size_t i = 0; i < f.x.size(); ++i) {
            QVariantMap obj;
            obj[QStringLiteral("x")] = f.x[i];
            obj[QStringLiteral("y")] = f.y[i];
            obj[QStringLiteral("z")] = f.z[i];
            obj[QStringLiteral("gainDb")] = f.gain_db[i];
            obj[QStringLiteral("width")] = f.width[i];
            obj[QStringLiteral("depth")] = f.depth[i];
            obj[QStringLiteral("height")] = f.height[i];
            obj[QStringLiteral("snap")] = static_cast<bool>(f.snap[i]);
            obj[QStringLiteral("label")] = f.labels[i];
            objects.append(obj);
        }
        QVariantMap row;
        row[QStringLiteral("time")] = f.time_s;
        row[QStringLiteral("objects")] = objects;
        out.append(row);
    }
    return out;
}

int ObjectDecodeController::frameCount() const {
    return result_ ? static_cast<int>(result_->frames.size()) : 0;
}

void ObjectDecodeController::inspectFile(const QUrl& url) {
    if (busy_) {
        return;
    }
    stopAudition();
    const QString path = url.toLocalFile();
    if (path.isEmpty()) {
        return;
    }
    file_path_ = path;
    emit filePathChanged();
    busy_ = true;
    emit busyChanged();

    std::ignore = QtConcurrent::run([this, path] {
        auto outcome = inspect_file(path);
        QMetaObject::invokeMethod(this, [this, outcome = std::move(outcome)]() mutable {
            busy_ = false;
            error_ = outcome.error;
            result_ = outcome.error.isEmpty() ? std::make_optional(std::move(outcome.result))
                                              : std::nullopt;
            emit busyChanged();
            emit resultChanged();
        });
    });
}

void ObjectDecodeController::auditionObject(int index) {
    if (index == auditioning_index_) {
        stopAudition();
        return;
    }
    if (auditioning_index_ != -1 || busy_ || !result_) {
        return;
    }
    if (index < 0 || static_cast<std::size_t>(index) >= result_->object_audio.size()) {
        return;
    }
    const auto& samples = result_->object_audio[static_cast<std::size_t>(index)];
    if (samples.empty()) {
        return;
    }

    audition_sink_ = std::make_unique<ac3::audio::MonitorSink>();
    const auto started =
        audition_sink_->start(std::string{}, result_->sample_rate_hz, /*channels=*/1);
    if (!started) {
        const auto why = ac3::audio::describe(started.error());
        audition_sink_.reset();
        error_ = QStringLiteral("Could not open the audition output: %1").arg(to_qstring(why));
        emit resultChanged();
        return;
    }

    stop_audition_.store(false, std::memory_order_relaxed);
    auditioning_index_ = index;
    emit auditionChanged();

    std::ignore = QtConcurrent::run([this, samples] {
        std::size_t at = 0;
        QString error;
        while (at < samples.size()) {
            if (stop_audition_.load(std::memory_order_relaxed)) {
                break;
            }
            // running() turns false, not just submit() false-forever, once
            // the device goes away under the stream - without this check
            // the loop below retries forever instead of stopping.
            if (!audition_sink_->running()) {
                error = QStringLiteral("The audition output device disappeared.");
                break;
            }
            const auto chunk_len = std::min<std::size_t>(2048, samples.size() - at);
            const auto chunk = std::span{samples}.subspan(at, chunk_len);
            // submit()'s own non-blocking contract (see monitor.hpp): a full
            // queue means this is running ahead of real time, so wait
            // rather than spin - identical pacing to EncoderController's
            // own motion-preview submit loop.
            if (audition_sink_->submit(chunk)) {
                at += chunk_len;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
        }
        QMetaObject::invokeMethod(this, [this, error] {
            if (audition_sink_) {
                audition_sink_->stop();
                audition_sink_.reset();
            }
            auditioning_index_ = -1;
            if (!error.isEmpty()) {
                error_ = error;
                emit resultChanged();
            }
            emit auditionChanged();
        });
    });
}

void ObjectDecodeController::stopAudition() {
    stop_audition_.store(true, std::memory_order_relaxed);
}
