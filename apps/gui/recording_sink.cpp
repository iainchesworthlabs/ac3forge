#include "recording_sink.hpp"

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <system_error>
#include <utility>

#include "ac3/core/tables.hpp"
#include "ac3/io/dec3.hpp"
#include "ac3/io/elementary.hpp"

namespace {

// The user-facing strings match EncoderController::writeOutput's for the
// same failures, so a streamed take and a whole-buffer one report a broken
// disk in the same words.
constexpr const char* kCannotOpen = "Could not open the output file for writing.";
constexpr const char* kCannotWrap = "Could not wrap the stream into IEC 61937 bursts.";
constexpr const char* kNothingEncoded = "Nothing was encoded.";

const char* write_failed_for(RecordingSink::Container container) {
    switch (container) {
        case RecordingSink::Container::kMatroska:
            return "Writing the Matroska file failed.";
        case RecordingSink::Container::kMpegts:
            return "Writing the MPEG-TS file failed.";
        case RecordingSink::Container::kSpdif:
            return "Writing the WAV carrier failed.";
        case RecordingSink::Container::kFmp4:
            return "Writing the fragmented MP4 folder failed.";
        case RecordingSink::Container::kElementary:
            break;
    }
    return "Writing the stream failed.";
}

// kFmp4's own file writers - it writes several named files into a folder
// rather than appending to the one std::ofstream every other container here
// shares.
bool write_fmp4_bytes(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream out{path, std::ios::binary};
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

bool write_fmp4_text(const std::filesystem::path& path, std::string_view text) {
    return write_fmp4_bytes(
        path, std::as_bytes(std::span{reinterpret_cast<const char*>(text.data()), text.size()}));
}

}  // namespace

std::string RecordingSink::open(const std::string& path, const Config& config) {
    config_ = config;
    path_ = path;
    frames_ = 0;

    if (config.container == Container::kSpdif) {
        // The carrier runs at 4x the content rate for E-AC-3 - see
        // ac3cli's own run_spdif (apps/cli/main.cpp) for the citation.
        const auto carrier_rate =
            config.eac3 ? config.sample_rate * 4 : config.sample_rate;
        if (!wav_.open(path, carrier_rate, 2)) {
            return kCannotOpen;
        }
        packer_ = {};
        open_ = true;
        return {};
    }

    if (config.container == Container::kFmp4) {
        // A folder, not a file - and nothing is written into it yet: the
        // fragmenter's track needs a bitstream scan, so it waits for the
        // first frame (see start_fmp4). Creating the folder here still means
        // an unwritable destination refuses the take before capture starts,
        // which is what open()'s contract above promises.
        fmp4_dir_ = std::filesystem::path{path};
        std::error_code ec;
        std::filesystem::create_directories(fmp4_dir_, ec);
        if (ec) {
            return kCannotOpen;
        }
        open_ = true;
        return {};
    }

    if (config.container == Container::kMatroska) {
        auto writer = matroska::Writer::create(matroska::AudioTrack{
            .codec_id = std::string{config.eac3 ? matroska::kCodecEac3 : matroska::kCodecAc3},
            .sample_rate = config.sample_rate,
            .channels = config.channels,
            .samples_per_frame = ac3::kSamplesPerFrame});
        if (!writer) {
            return std::string{matroska::describe(writer.error())};
        }
        matroska_.emplace(std::move(*writer));
    } else if (config.container == Container::kMpegts) {
        auto writer = mpegts::Writer::create(mpegts::AudioTrack{
            .codec = config.eac3 ? mpegts::AudioCodec::kEac3 : mpegts::AudioCodec::kAc3,
            .sample_rate = config.sample_rate,
            .channels = config.channels,
            .samples_per_frame = ac3::kSamplesPerFrame});
        if (!writer) {
            return std::string{mpegts::describe(writer.error())};
        }
        mpegts_.emplace(std::move(*writer));
    }

    file_.open(path, std::ios::binary);
    if (!file_) {
        return kCannotOpen;
    }
    if (matroska_ && !write_file(matroska_->header())) {
        return write_failed_for(config_.container);
    }
    open_ = true;
    return {};
}

std::string RecordingSink::push(std::span<const std::byte> frame) {
    switch (config_.container) {
        case Container::kElementary:
            if (!write_file(frame)) {
                return write_failed_for(config_.container);
            }
            break;
        case Container::kMatroska: {
            auto closed = matroska_->push(frame);
            if (!closed) {
                return std::string{matroska::describe(closed.error())};
            }
            if (!closed->empty() && !write_file(*closed)) {
                return write_failed_for(config_.container);
            }
            break;
        }
        case Container::kMpegts: {
            auto packets = mpegts_->push(frame);
            if (!packets) {
                return std::string{mpegts::describe(packets.error())};
            }
            if (!write_file(*packets)) {
                return write_failed_for(config_.container);
            }
            break;
        }
        case Container::kFmp4:
            if (const auto problem = push_fmp4(frame); !problem.empty()) {
                return problem;
            }
            break;
        case Container::kSpdif: {
            if (config_.eac3) {
                auto burst = packer_.push(frame);
                if (!burst) {
                    return kCannotWrap;
                }
                if (*burst && !wav_.write(**burst)) {
                    return write_failed_for(config_.container);
                }
            } else {
                auto burst = ac3::iec61937::wrap_frame(frame);
                if (!burst) {
                    return kCannotWrap;
                }
                if (!wav_.write(*burst)) {
                    return write_failed_for(config_.container);
                }
            }
            // One frame is 32 ms, so this patches the carrier's header
            // about once a second - WavPcm16StreamWriter::flush_header's
            // own crash-worst-case rationale.
            if (frames_ % 32 == 31) {
                wav_.flush_header();
            }
            break;
        }
    }
    ++frames_;
    return {};
}

std::string RecordingSink::close() {
    if (!open_) {
        return {};
    }
    open_ = false;
    if (frames_ == 0) {
        // The whole-buffer path never created a file for an empty take;
        // matching that means removing the one open() already created. kFmp4
        // never wrote anything into its folder (start_fmp4 waits for a first
        // frame that never came), so removing the folder is the same
        // gesture - and remove(), not remove_all(), so a folder the user
        // pointed at that already had something in it is left alone.
        wav_.close();
        file_.close();
        std::error_code ec;
        if (config_.container == Container::kFmp4) {
            std::filesystem::remove(fmp4_dir_, ec);
        } else {
            std::filesystem::remove(std::filesystem::path{path_}, ec);
        }
        return kNothingEncoded;
    }
    if (config_.container == Container::kFmp4) {
        auto segment = fmp4_->finalize();
        if (!segment) {
            return std::string{mp4::describe(segment.error())};
        }
        if (*segment) {
            const auto name = std::format("segment{}.m4s", (*segment)->sequence_number);
            if (!write_fmp4_bytes(fmp4_dir_ / name, (*segment)->bytes)) {
                return write_failed_for(config_.container);
            }
            ++fmp4_segments_;
        }
        return write_fmp4_manifests(true);
    }
    if (config_.container == Container::kSpdif) {
        // An E-AC-3 tail that never completed a burst is dropped, exactly
        // as the one-shot wrap_stream drops it.
        wav_.close();
        return {};
    }
    if (matroska_ && !write_file(matroska_->finalize())) {
        return write_failed_for(config_.container);
    }
    if (mpegts_) {
        // Always empty by contract; called so the two writers age uniformly.
        std::ignore = mpegts_->finalize();
    }
    file_.close();
    if (file_.fail()) {
        return write_failed_for(config_.container);
    }
    return {};
}

std::string RecordingSink::start_fmp4(std::span<const std::byte> first_frame) {
    const auto scanned = ac3::io::scan(first_frame);
    if (!scanned) {
        return "Could not describe the encoded stream for the fragmented MP4 folder.";
    }
    const bool eac3 = scanned->kind == ac3::io::StreamKind::kEac3;
    fmp4_track_ = mp4::AudioTrack{.codec_id = std::string{eac3 ? mp4::kCodecEac3 : mp4::kCodecAc3},
                                  .sample_rate = ac3::sample_rate_hz(scanned->sample_rate),
                                  .channels = scanned->channels,
                                  .samples_per_frame = ac3::kSamplesPerFrame,
                                  .codec_config = ac3::io::build_codec_config_box(*scanned)};
    // The Atmos/JOC signalling, identical to what
    // EncoderController::writeOutput's own fMP4 branch and ac3cli's fmp4
    // build: CHANNELS="<N>/JOC" for HLS, TS 103 420 §D.2's two
    // SupplementalProperty descriptors and TS 102 366 clause I.1.2.1's
    // AudioChannelConfiguration for DASH, and §E.5's 'ceao' brand on the
    // segments themselves.
    fmp4_hls_ = mp4::HlsOptions{.channels_attribute =
                                    scanned->oba_complexity_index
                                        ? std::format("{}/JOC", *scanned->oba_complexity_index)
                                        : std::string{}};
    fmp4_dash_ = mp4::DashOptions{
        .joc_complexity_index = scanned->oba_complexity_index,
        .dolby_channel_configuration = ac3::io::dash_channel_configuration(*scanned)};

    auto writer = mp4::FragmentWriter::create(
        fmp4_track_,
        mp4::FragmentOptions{.object_audio_brand = scanned->oba_complexity_index.has_value()});
    if (!writer) {
        return std::string{mp4::describe(writer.error())};
    }
    fmp4_.emplace(std::move(*writer));
    if (!write_fmp4_bytes(fmp4_dir_ / "init.mp4", fmp4_->init_segment())) {
        return write_failed_for(config_.container);
    }
    // Read once, when the first segment's timeline starts - a live MPD's
    // @availabilityStartTime must not move as the session runs, and mp4::
    // has no clock of its own to read (mp4::MpdOptions).
    fmp4_availability_start_ = std::format(
        "{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
    return {};
}

std::string RecordingSink::write_fmp4_manifests(bool finished) {
    const auto window = fmp4_->window();
    auto hls = fmp4_hls_;
    hls.vod = finished;
    if (!write_fmp4_text(fmp4_dir_ / "audio.m3u8",
                         mp4::build_hls_media_playlist(fmp4_track_, window, hls)) ||
        !write_fmp4_text(fmp4_dir_ / "master.m3u8",
                         mp4::build_hls_master_playlist(fmp4_track_, window, "audio.m3u8", hls))) {
        return write_failed_for(config_.container);
    }
    const auto adaptation_set = mp4::build_dash_adaptation_set(fmp4_track_, window, fmp4_dash_);
    // Dynamic while the take runs, static once it stops - the MPD's half of
    // the same before/after the HLS playlist's #EXT-X-ENDLIST makes. Every
    // segment stays on disk here (no rolling window is configured), so the
    // time-shift buffer is the whole take so far.
    const double window_seconds =
        window.empty() ? 0.0
                       : static_cast<double>(window.back().base_media_decode_time +
                                             window.back().duration_samples) /
                             static_cast<double>(fmp4_track_.sample_rate);
    const mp4::MpdOptions mpd_options{.is_static = finished,
                                      .availability_start_time = fmp4_availability_start_,
                                      .time_shift_buffer_depth_seconds = window_seconds};
    if (!write_fmp4_text(fmp4_dir_ / "manifest.mpd",
                         mp4::build_dash_mpd(fmp4_track_, window, adaptation_set, mpd_options))) {
        return write_failed_for(config_.container);
    }
    return {};
}

std::string RecordingSink::push_fmp4(std::span<const std::byte> frame) {
    if (!fmp4_) {
        if (auto problem = start_fmp4(frame); !problem.empty()) {
            return problem;
        }
    }
    auto segment = fmp4_->push(frame);
    if (!segment) {
        return std::string{mp4::describe(segment.error())};
    }
    if (!*segment) {
        return {};
    }
    const auto name = std::format("segment{}.m4s", (*segment)->sequence_number);
    if (!write_fmp4_bytes(fmp4_dir_ / name, (*segment)->bytes)) {
        return write_failed_for(config_.container);
    }
    ++fmp4_segments_;
    return write_fmp4_manifests(false);
}

bool RecordingSink::write_file(std::span<const std::byte> bytes) {
    if (bytes.empty()) {
        return static_cast<bool>(file_);
    }
    file_.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(file_);
}
