#include "containers.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fmt/base.h>
#include <fmt/format.h>
#include <fstream>
#include <ios>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "ac3/analysis/levels.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/io/dec3.hpp"
#include "ac3/io/elementary.hpp"
#include "matroska/matroska.hpp"
#include "matroska/reader.hpp"
#include "mp4/dash.hpp"
#include "mp4/hls.hpp"
#include "mp4/mp4.hpp"
#include "mpegts/mpegts.hpp"
#include "../platform/stdio_binary.hpp"
#include "../support.hpp"

namespace ac3cli::commands {

namespace {

bool write_bytes_to_path(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream out{path, std::ios::binary};
    if (!out) {
        fmt::println(stderr, "error: cannot open {} for writing", path.string());
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        fmt::println(stderr, "error: write failed for {}", path.string());
        return false;
    }
    return true;
}

bool write_text_to_path(const std::filesystem::path& path, std::string_view text) {
    return write_bytes_to_path(
        path, std::as_bytes(std::span{reinterpret_cast<const char*>(text.data()), text.size()}));
}

// §E2.3.1.2's legacy-core delivery - an AC-3 bed with Annex E dependent
// substreams extending it - has no codec-config box defined for it in any of
// these containers: 'dac3' cannot mention the dependents and 'dec3' would
// have to call the AC-3 core Annex E syntax (ac3::io::build_codec_config_box
// declines it for exactly that reason, returning an empty payload). Refused
// here, where the message can name the file and point somewhere useful,
// rather than written into a file whose header contradicts its own mdat.
[[nodiscard]] bool reject_legacy_core(const ac3::io::ScannedStream& scanned,
                                      std::string_view in_path, std::string_view container) {
    if (scanned.kind != ac3::io::StreamKind::kAc3CoreEac3Extension) {
        return false;
    }
    fmt::println(stderr,
                "error: {} is an AC-3 core with E-AC-3 extension substreams (A/52 §E2.3.1.2); "
                "{} has no codec-config box that can describe that arrangement. "
                "`ac3cli decode` reads the stream itself.",
                in_path, container);
    return true;
}
}  // namespace

int run_mkv(std::string_view in_path, std::string_view out_path) {
    const auto raw = read_all(in_path);
    if (raw.empty()) {
        fmt::println(stderr, "error: cannot open {}", in_path);
        return 1;
    }
    // Everything the container needs to declare comes out of the bitstream:
    // the format, the access-unit boundaries, the sample rate and the channel
    // count. This used to take a layout argument to learn the channel count,
    // which meant a wrong one silently produced a file that misdescribed
    // itself - and nothing could catch it.
    const auto scanned = ac3::io::scan(raw);
    if (!scanned) {
        fmt::println(stderr, "error: {}", ac3::io::describe(scanned.error()));
        return 1;
    }
    if (reject_legacy_core(*scanned, in_path, "Matroska")) {
        return 1;
    }
    const bool eac3 = scanned->kind == ac3::io::StreamKind::kEac3;

    // scan()'s access units pass to the muxer as the views they already are
    // - the whole-stream copy that satisfied the old parameter type is gone.
    const auto& units = scanned->access_units;

    const matroska::AudioTrack track{
        .codec_id = std::string{eac3 ? matroska::kCodecEac3 : matroska::kCodecAc3},
        .sample_rate = ac3::sample_rate_hz(scanned->sample_rate),
        .channels = scanned->channels,
        .samples_per_frame = ac3::kSamplesPerFrame};
    const auto file = matroska::mux(track, units);
    if (!file) {
        fmt::println(stderr, "error: {}", matroska::describe(file.error()));
        return 1;
    }
    std::ofstream out{std::string{out_path}, std::ios::binary};
    if (!out) {
        fmt::println(stderr, "error: cannot write {}", out_path);
        return 1;
    }
    out.write(reinterpret_cast<const char*>(file->data()),
              static_cast<std::streamsize>(file->size()));
    if (!out) {
        fmt::println(stderr, "error: write failed");
        return 1;
    }
    // Name the layout only when one substream carries the whole thing. With
    // dependents the acmod describes the BED, so printing it beside a wider
    // rendered channel count would just contradict itself.
    const std::string shape =
        scanned->substreams_per_unit > 1
            ? fmt::format("{} substreams", scanned->substreams_per_unit)
            : std::string{ac3::analysis::layout_name(scanned->acmod, scanned->lfe)};
    fmt::println("wrote {} {} access units ({}, {} channels, {} bytes) to {}",
                 units.size(), eac3 ? "E-AC-3" : "AC-3", shape, track.channels,
                 file->size(), out_path);
    return 0;
}

int run_mp4(std::string_view in_path, std::string_view out_path) {
    const auto raw = read_all(in_path);
    if (raw.empty()) {
        fmt::println(stderr, "error: cannot open {}", in_path);
        return 1;
    }
    const auto scanned = ac3::io::scan(raw);
    if (!scanned) {
        fmt::println(stderr, "error: {}", ac3::io::describe(scanned.error()));
        return 1;
    }
    if (reject_legacy_core(*scanned, in_path, "MP4")) {
        return 1;
    }
    const bool eac3 = scanned->kind == ac3::io::StreamKind::kEac3;

    // scan()'s access units pass to the muxer as the views they already are
    // - the whole-stream copy that satisfied the old parameter type is gone.
    const auto& units = scanned->access_units;

    const mp4::AudioTrack track{
        .codec_id = std::string{eac3 ? mp4::kCodecEac3 : mp4::kCodecAc3},
        .sample_rate = ac3::sample_rate_hz(scanned->sample_rate),
        .channels = scanned->channels,
        .samples_per_frame = ac3::kSamplesPerFrame,
        .codec_config = ac3::io::build_codec_config_box(*scanned)};
    const auto file = mp4::mux(track, units);
    if (!file) {
        fmt::println(stderr, "error: {}", mp4::describe(file.error()));
        return 1;
    }
    std::ofstream out{std::string{out_path}, std::ios::binary};
    if (!out) {
        fmt::println(stderr, "error: cannot write {}", out_path);
        return 1;
    }
    out.write(reinterpret_cast<const char*>(file->data()),
              static_cast<std::streamsize>(file->size()));
    if (!out) {
        fmt::println(stderr, "error: write failed");
        return 1;
    }
    const std::string shape =
        scanned->substreams_per_unit > 1
            ? fmt::format("{} substreams", scanned->substreams_per_unit)
            : std::string{ac3::analysis::layout_name(scanned->acmod, scanned->lfe)};
    const std::string atmos =
        scanned->oba_complexity_index
            ? fmt::format(", Atmos complexity {}", *scanned->oba_complexity_index)
            : std::string{};
    fmt::println("wrote {} {} access units ({}, {} channels{}, {} bytes) to {}",
                 units.size(), eac3 ? "E-AC-3" : "AC-3", shape, track.channels, atmos,
                 file->size(), out_path);
    return 0;
}

int run_fmp4(std::string_view in_path, std::string_view out_dir,
             std::uint32_t frames_per_fragment) {
    const auto raw = read_all(in_path);
    if (raw.empty()) {
        fmt::println(stderr, "error: cannot open {}", in_path);
        return 1;
    }
    const auto scanned = ac3::io::scan(raw);
    if (!scanned) {
        fmt::println(stderr, "error: {}", ac3::io::describe(scanned.error()));
        return 1;
    }
    if (reject_legacy_core(*scanned, in_path, "fragmented MP4")) {
        return 1;
    }
    const bool eac3 = scanned->kind == ac3::io::StreamKind::kEac3;

    // scan()'s access units pass to the muxer as the views they already are
    // - the whole-stream copy that satisfied the old parameter type is gone.
    const auto& units = scanned->access_units;

    const mp4::AudioTrack track{.codec_id = std::string{eac3 ? mp4::kCodecEac3 : mp4::kCodecAc3},
                                .sample_rate = ac3::sample_rate_hz(scanned->sample_rate),
                                .channels = scanned->channels,
                                .samples_per_frame = ac3::kSamplesPerFrame,
                                .codec_config = ac3::io::build_codec_config_box(*scanned)};

    // ETSI TS 103 420 §E.5's 'ceao' compatibility brand, which DASH-IF IOP
    // Part 8 v5.0.0 §5.3.3 asks for on a backward-compatible object-audio
    // E-AC-3 track: mp4:: never reads the object layer itself, so this front
    // end - which already read oba_complexity_index to build the dec3 box
    // above - is the one that says so.
    const auto fragmented = mp4::fragment(
        track, units,
        mp4::FragmentOptions{.frames_per_fragment = frames_per_fragment,
                             .object_audio_brand = scanned->oba_complexity_index.has_value()});
    if (!fragmented) {
        fmt::println(stderr, "error: {}", mp4::describe(fragmented.error()));
        return 1;
    }

    std::error_code ec;
    const std::filesystem::path dir{std::string{out_dir}};
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        fmt::println(stderr, "error: cannot create directory {} ({})", out_dir, ec.message());
        return 1;
    }

    if (!write_bytes_to_path(dir / "init.mp4", fragmented->init_segment)) {
        return 1;
    }
    for (const auto& segment : fragmented->media_segments) {
        const auto name = fmt::format("segment{}.m4s", segment.sequence_number);
        if (!write_bytes_to_path(dir / name, segment.bytes)) {
            return 1;
        }
    }

    // Dolby Digital Plus with Atmos objects needs CHANNELS="<N>/JOC" instead
    // of a plain channel count (see mp4/hls.hpp's own citations) - N is the
    // same decodable-object count ac3::io::scan already read off the
    // bitstream to build the dec3 box above (TS 103 420
    // §8.3.2's complexity_index_type_a). mp4:: itself never reads that
    // field; only this CLI front end, which already has it, does.
    const mp4::HlsOptions hls_options{
        .channels_attribute = scanned->oba_complexity_index
                                  ? fmt::format("{}/JOC", *scanned->oba_complexity_index)
                                  : std::string{}};
    const auto media_playlist =
        mp4::build_hls_media_playlist(track, fragmented->media_segments, hls_options);
    const auto master_playlist = mp4::build_hls_master_playlist(track, fragmented->media_segments,
                                                                "audio.m3u8", hls_options);
    if (!write_text_to_path(dir / "audio.m3u8", media_playlist) ||
        !write_text_to_path(dir / "master.m3u8", master_playlist)) {
        return 1;
    }

    // The DASH side of the same two facts: TS 103 420 §D.2's JOC extension
    // type and complexity index (DASH-IF IOP Part 8 §5.3.2), and the
    // AudioChannelConfiguration @value TS 102 366 clause I.1.2.1 defines -
    // ac3::io::dash_channel_configuration is the one place that word is
    // derived from the bitstream (ac3/io/dec3.hpp).
    const mp4::DashOptions dash_options{
        .joc_complexity_index = scanned->oba_complexity_index,
        .dolby_channel_configuration = ac3::io::dash_channel_configuration(*scanned)};
    const auto adaptation_set =
        mp4::build_dash_adaptation_set(track, fragmented->media_segments, dash_options);
    const auto mpd = mp4::build_dash_mpd(track, fragmented->media_segments, adaptation_set);
    if (!write_text_to_path(dir / "manifest.mpd", mpd)) {
        return 1;
    }

    const std::string shape =
        scanned->substreams_per_unit > 1
            ? fmt::format("{} substreams", scanned->substreams_per_unit)
            : std::string{ac3::analysis::layout_name(scanned->acmod, scanned->lfe)};
    const std::string atmos =
        scanned->oba_complexity_index
            ? fmt::format(", Atmos complexity {}", *scanned->oba_complexity_index)
            : std::string{};
    fmt::println(
        "wrote {} {} access units ({}, {} channels{}) as {} fragment(s) to {} "
        "(init.mp4, segment*.m4s, audio.m3u8, master.m3u8, manifest.mpd)",
        units.size(), eac3 ? "E-AC-3" : "AC-3", shape, track.channels, atmos,
        fragmented->media_segments.size(), out_dir);
    return 0;
}

int run_ts(std::string_view in_path, std::string_view out_path) {
    const auto raw = read_all(in_path);
    if (raw.empty()) {
        fmt::println(stderr, "error: cannot open {}", in_path);
        return 1;
    }
    const auto scanned = ac3::io::scan(raw);
    if (!scanned) {
        fmt::println(stderr, "error: {}", ac3::io::describe(scanned.error()));
        return 1;
    }
    if (reject_legacy_core(*scanned, in_path, "MPEG-TS")) {
        return 1;
    }
    const bool eac3 = scanned->kind == ac3::io::StreamKind::kEac3;

    // scan()'s access units pass to the muxer as the views they already are
    // - the whole-stream copy that satisfied the old parameter type is gone.
    const auto& units = scanned->access_units;

    const mpegts::AudioTrack track{
        .codec = eac3 ? mpegts::AudioCodec::kEac3 : mpegts::AudioCodec::kAc3,
        .sample_rate = ac3::sample_rate_hz(scanned->sample_rate),
        .channels = scanned->channels,
        .samples_per_frame = ac3::kSamplesPerFrame};
    const auto file = mpegts::mux(track, units);
    if (!file) {
        fmt::println(stderr, "error: {}", mpegts::describe(file.error()));
        return 1;
    }
    std::ofstream out{std::string{out_path}, std::ios::binary};
    if (!out) {
        fmt::println(stderr, "error: cannot write {}", out_path);
        return 1;
    }
    out.write(reinterpret_cast<const char*>(file->data()),
              static_cast<std::streamsize>(file->size()));
    if (!out) {
        fmt::println(stderr, "error: write failed");
        return 1;
    }
    const std::string shape =
        scanned->substreams_per_unit > 1
            ? fmt::format("{} substreams", scanned->substreams_per_unit)
            : std::string{ac3::analysis::layout_name(scanned->acmod, scanned->lfe)};
    fmt::println("wrote {} {} access units ({}, {} channels, {} bytes) to {}",
                 units.size(), eac3 ? "E-AC-3" : "AC-3", shape, track.channels,
                 file->size(), out_path);
    return 0;
}

// --- container input (ROADMAP.md's IO2) -------------------------------------

namespace {

// Which container a file actually is, decided by its first bytes rather than
// its name. A rip is as likely to be called "title00.mkv" when it is not one
// as it is to have no extension at all, and the failure a wrong guess
// produces ("no EBML header") reads like a corrupt file rather than like the
// wrong parser - so the name is never consulted.
enum class ContainerKind : std::uint8_t { kUnknown, kMatroska };

// EBML's own magic: the four bytes of the EBML header id every Matroska and
// WebM file opens with - the same kEbmlHeader constant
// src/matroska/src/ebml_detail.hpp holds, written out big-endian.
constexpr std::array<std::byte, 4> kEbmlMagic{std::byte{0x1A}, std::byte{0x45}, std::byte{0xDF},
                                              std::byte{0xA3}};

ContainerKind sniff_container(std::span<const std::byte> head) {
    if (head.size() >= kEbmlMagic.size() &&
        std::equal(kEbmlMagic.begin(), kEbmlMagic.end(), head.begin())) {
        return ContainerKind::kMatroska;
    }
    return ContainerKind::kUnknown;
}

// How much of the file is read at a time. Big enough that a whole cluster
// usually lands in one or two reads, small enough that this is the memory
// figure for a two-hour rip as much as for a ten-second clip.
constexpr std::size_t kDemuxChunkBytes = 64 * 1024;

}  // namespace

int run_demux(std::string_view in_path, std::string_view out_path) {
    std::ifstream file;
    std::istream* in = &std::cin;
    if (is_stdio_path(in_path)) {
        // Binary mode before the first byte, the same rule read_all and the
        // sinks already follow - see platform/stdio_binary.hpp.
        ac3::cli::platform::set_stdio_binary();
    } else {
        file.open(std::string{in_path}, std::ios::binary);
        if (!file) {
            fmt::println(stderr, "error: cannot open {}", in_path);
            return 1;
        }
        in = &file;
    }

    std::vector<std::byte> chunk(kDemuxChunkBytes);
    const auto read_chunk = [&in, &chunk]() -> std::span<const std::byte> {
        in->read(reinterpret_cast<char*>(chunk.data()),
                 static_cast<std::streamsize>(chunk.size()));
        return std::span<const std::byte>{chunk}.first(static_cast<std::size_t>(in->gcount()));
    };

    const auto first = read_chunk();
    if (sniff_container(first) != ContainerKind::kMatroska) {
        fmt::println(stderr,
                     "error: {} is not a container this build reads (expected Matroska/WebM)",
                     in_path);
        return 1;
    }

    matroska::Reader reader{};
    EncodedStreamSink sink;
    if (!sink.open(out_path, /*keep_partial=*/false)) {
        return 1;
    }
    // A write failure is latched rather than thrown out of the callback: the
    // reader cannot be told to stop mid-chunk, and unwinding through it would
    // leave its parse state undefined.
    bool write_failed = false;
    const auto on_frame = [&sink, &write_failed](std::span<const std::byte> frame) {
        if (!write_failed && !sink.push(frame)) {
            write_failed = true;
        }
    };

    const auto fail = [&sink](std::string_view message) {
        fmt::println(stderr, "error: {}", message);
        sink.abort();
        return 1;
    };

    for (auto bytes = first; !bytes.empty(); bytes = read_chunk()) {
        const auto pushed = reader.push(bytes, on_frame);
        if (!pushed) {
            return fail(matroska::describe(pushed.error()));
        }
        if (write_failed) {
            return fail("write failed");
        }
    }
    const auto finished = reader.finish();
    if (!finished) {
        return fail(matroska::describe(finished.error()));
    }
    if (write_failed) {
        return fail("write failed");
    }
    if (sink.frames() == 0) {
        return fail("the container holds no access units on its audio track");
    }
    if (!sink.close()) {
        return 1;
    }

    // The container declares the codec; this command never looks inside an
    // access unit, which is exactly why it can hand one back untouched.
    const auto& track = reader.track();
    fmt::println(status_stream(out_path),
                 "wrote {} access units ({}, {} Hz, {} channels, {} bytes) to {}", sink.frames(),
                 track.codec_id, track.sample_rate, track.channels, sink.total_bytes(), out_path);
    return 0;
}

}  // namespace ac3cli::commands
