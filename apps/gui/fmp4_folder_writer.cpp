#include "fmp4_folder_writer.hpp"

#include <chrono>
#include <cstdint>
#include <format>
#include <fstream>
#include <ios>
#include <string_view>
#include <system_error>
#include <utility>

#include "ac3/core/tables.hpp"
#include "ac3/io/dec3.hpp"
#include "ac3/io/elementary.hpp"

namespace {

constexpr const char* kCannotOpen = "Could not open the output file for writing.";
constexpr const char* kWriteFailed = "Writing the fragmented MP4 folder failed.";

bool write_bytes(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream out{path, std::ios::binary};
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

bool write_text(const std::filesystem::path& path, std::string_view text) {
    return write_bytes(
        path, std::as_bytes(std::span{reinterpret_cast<const char*>(text.data()), text.size()}));
}

}  // namespace

std::string Fmp4FolderWriter::open(const std::string& directory) {
    dir_ = std::filesystem::path{directory};
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    if (ec) {
        return kCannotOpen;
    }
    return {};
}

std::string Fmp4FolderWriter::start(std::span<const std::byte> first_frame) {
    // One access unit carries everything the track needs: kind, sample rate,
    // rendered channel count, the dac3/dec3 payload, the channel map and the
    // TS 103 420 object marker. Exactly the re-scan
    // EncoderController::writeOutput and ac3cli's own fmp4 already do before
    // wrapping frames they just encoded - done here on the first frame
    // instead, because a live session has no finished stream to scan.
    const auto scanned = ac3::io::scan(first_frame);
    if (!scanned) {
        return "Could not describe the encoded stream for the fragmented MP4 folder.";
    }
    const bool eac3 = scanned->kind == ac3::io::StreamKind::kEac3;
    track_ = mp4::AudioTrack{.codec_id = std::string{eac3 ? mp4::kCodecEac3 : mp4::kCodecAc3},
                             .sample_rate = ac3::sample_rate_hz(scanned->sample_rate),
                             .channels = scanned->channels,
                             .samples_per_frame = ac3::kSamplesPerFrame,
                             .codec_config = ac3::io::build_codec_config_box(*scanned)};
    // The Atmos/JOC signalling, identical to what
    // EncoderController::writeOutput's own fMP4 branch and ac3cli's fmp4
    // build: CHANNELS="<N>/JOC" for HLS (mp4/hls.hpp), TS 103 420 §D.2's two
    // SupplementalProperty descriptors and TS 102 366 clause I.1.2.1's
    // AudioChannelConfiguration for DASH (mp4/dash.hpp), and §E.5's 'ceao'
    // brand on the segments themselves.
    hls_ = mp4::HlsOptions{.channels_attribute =
                               scanned->oba_complexity_index
                                   ? std::format("{}/JOC", *scanned->oba_complexity_index)
                                   : std::string{}};
    dash_ = mp4::DashOptions{
        .joc_complexity_index = scanned->oba_complexity_index,
        .dolby_channel_configuration = ac3::io::dash_channel_configuration(*scanned)};

    auto writer = mp4::FragmentWriter::create(
        track_,
        mp4::FragmentOptions{.object_audio_brand = scanned->oba_complexity_index.has_value()});
    if (!writer) {
        return std::string{mp4::describe(writer.error())};
    }
    writer_.emplace(std::move(*writer));
    if (!write_bytes(dir_ / "init.mp4", writer_->init_segment())) {
        return kWriteFailed;
    }
    availability_start_ = std::format(
        "{:%FT%TZ}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
    return {};
}

std::string Fmp4FolderWriter::write_manifests(const mp4::FragmentWriter& writer,
                                             bool finished) {
    const auto window = writer.window();
    auto hls = hls_;
    hls.vod = finished;
    if (!write_text(dir_ / "audio.m3u8", mp4::build_hls_media_playlist(track_, window, hls)) ||
        !write_text(dir_ / "master.m3u8",
                    mp4::build_hls_master_playlist(track_, window, "audio.m3u8", hls))) {
        return kWriteFailed;
    }
    const auto adaptation_set = mp4::build_dash_adaptation_set(track_, window, dash_);
    // Dynamic while the take runs, static once it stops - the MPD's half of
    // the before/after the HLS playlist's #EXT-X-ENDLIST makes. No rolling
    // window is configured here (every segment stays on disk and stays
    // listed), so the time-shift buffer is the whole take so far.
    const double window_seconds =
        window.empty() ? 0.0
                       : static_cast<double>(window.back().base_media_decode_time +
                                             window.back().duration_samples) /
                             static_cast<double>(track_.sample_rate);
    const mp4::MpdOptions mpd_options{.is_static = finished,
                                      .availability_start_time = availability_start_,
                                      .time_shift_buffer_depth_seconds = window_seconds};
    if (!write_text(dir_ / "manifest.mpd",
                    mp4::build_dash_mpd(track_, window, adaptation_set, mpd_options))) {
        return kWriteFailed;
    }
    return {};
}

std::string Fmp4FolderWriter::push(std::span<const std::byte> frame) {
    if (!writer_) {
        if (auto problem = start(frame); !problem.empty()) {
            return problem;
        }
    }
    // start() above engages writer_ on every path that returns empty, but
    // clang-tidy's bugprone-unchecked-optional-access does not trace an
    // optional's engagement across a member-function call - the same false
    // positive gui/encoder_controller.cpp already works around by binding
    // the optional's value once.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    auto& writer = *writer_;
    auto segment = writer.push(frame);
    if (!segment) {
        return std::string{mp4::describe(segment.error())};
    }
    if (!*segment) {
        return {};
    }
    const auto name = std::format("segment{}.m4s", (*segment)->sequence_number);
    if (!write_bytes(dir_ / name, (*segment)->bytes)) {
        return kWriteFailed;
    }
    ++segments_;
    return write_manifests(writer, false);
}

std::string Fmp4FolderWriter::close() {
    if (!writer_) {
        return {};
    }
    auto& writer = *writer_;
    auto segment = writer.finalize();
    if (!segment) {
        return std::string{mp4::describe(segment.error())};
    }
    if (*segment) {
        const auto name = std::format("segment{}.m4s", (*segment)->sequence_number);
        if (!write_bytes(dir_ / name, (*segment)->bytes)) {
            return kWriteFailed;
        }
        ++segments_;
    }
    return write_manifests(writer, true);
}
