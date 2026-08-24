#include "containers.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>
#include <cstdint>
#include <filesystem>
#include <fmt/base.h>
#include <fmt/format.h>
#include <fstream>
#include <ios>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "ac3/analysis/levels.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/io/dec3.hpp"
#include "ac3/io/elementary.hpp"
#include "ac3/io/object_strip.hpp"
#include "matroska/matroska.hpp"
#include "matroska/reader.hpp"
#include "mp4/dash.hpp"
#include "mp4/hls.hpp"
#include "mp4/mp4.hpp"
#include "mp4/reader.hpp"
#include "mpegts/mpegts.hpp"
#include "mpegts/reader.hpp"
#include "../platform/stdio_binary.hpp"
#include "../support.hpp"

namespace ac3cli::commands {

namespace {

// Every container writer here holds ONE samples_per_frame for the whole
// track (mp4::AudioTrack, mpegts::AudioTrack, matroska::AudioTrack), so a
// stream whose access units differ in length cannot be described to any of
// them. That was invisible while this passed ac3::kSamplesPerFrame outright:
// an E-AC-3 stream coding fewer than six blocks per syncframe (numblkscod
// 0/1/2, §E2.3.1.4 - legal, and nothing this project's own encoders emit)
// got a track claiming 1536 samples a frame when its units really carry 256,
// 512 or 768, and every timestamp downstream was wrong by the ratio.
//
// ac3::io::uniform_access_unit_samples answers the question these writers
// can actually act on. Nothing means the units genuinely differ from each
// other, which no fixed-duration track models at all - refused with a real
// reason rather than muxed to a silently wrong timeline.
std::optional<std::uint32_t> track_samples_per_frame(const ac3::io::ScannedStream& scanned) {
    const auto uniform = ac3::io::uniform_access_unit_samples(scanned);
    if (!uniform) {
        fmt::println(stderr,
                     "error: this stream's access units are not all the same length, which no "
                     "fixed-duration container track can express");
    }
    return uniform;
}

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

    const auto samples_per_frame = track_samples_per_frame(*scanned);
    if (!samples_per_frame) {
        return 1;
    }

    const matroska::AudioTrack track{
        .codec_id = std::string{eac3 ? matroska::kCodecEac3 : matroska::kCodecAc3},
        .sample_rate = ac3::sample_rate_hz(scanned->sample_rate),
        .channels = scanned->channels,
        .samples_per_frame = *samples_per_frame};
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

    const auto samples_per_frame = track_samples_per_frame(*scanned);
    if (!samples_per_frame) {
        return 1;
    }

    const mp4::AudioTrack track{
        .codec_id = std::string{eac3 ? mp4::kCodecEac3 : mp4::kCodecAc3},
        .sample_rate = ac3::sample_rate_hz(scanned->sample_rate),
        .channels = scanned->channels,
        .samples_per_frame = *samples_per_frame,
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

namespace {

// One CMAF rendition on disk: the init segment, the media segments and the
// media playlist that names them, all inside `dir` and all referring to each
// other by names relative to it - which is what lets a second rendition live
// in a subdirectory beside the first without either one's playlist changing.
struct RenditionFiles {
    mp4::AudioTrack track;
    mp4::FragmentedOutput fragmented;
    std::string channels_attribute;
};

bool write_rendition(const std::filesystem::path& dir, const RenditionFiles& rendition) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        fmt::println(stderr, "error: cannot create directory {} ({})", dir.string(), ec.message());
        return false;
    }
    if (!write_bytes_to_path(dir / "init.mp4", rendition.fragmented.init_segment)) {
        return false;
    }
    for (const auto& segment : rendition.fragmented.media_segments) {
        const auto name = fmt::format("segment{}.m4s", segment.sequence_number);
        if (!write_bytes_to_path(dir / name, segment.bytes)) {
            return false;
        }
    }
    const mp4::HlsOptions options{.channels_attribute = rendition.channels_attribute};
    return write_text_to_path(
        dir / "audio.m3u8",
        mp4::build_hls_media_playlist(rendition.track, rendition.fragmented.media_segments,
                                      options));
}

// Everything mp4::fragment needs about one elementary stream, read off the
// bitstream rather than taken on trust - the same derivation for the JOC
// rendition and for its stripped companion.
std::optional<RenditionFiles> build_rendition(const ac3::io::ScannedStream& scanned,
                                              std::uint32_t frames_per_fragment) {
    const bool eac3 = scanned.kind == ac3::io::StreamKind::kEac3;
    const auto samples_per_frame = track_samples_per_frame(scanned);
    if (!samples_per_frame) {
        return std::nullopt;
    }
    mp4::AudioTrack track{.codec_id = std::string{eac3 ? mp4::kCodecEac3 : mp4::kCodecAc3},
                          .sample_rate = ac3::sample_rate_hz(scanned.sample_rate),
                          .channels = scanned.channels,
                          .samples_per_frame = *samples_per_frame,
                          .codec_config = ac3::io::build_codec_config_box(scanned)};
    // ETSI TS 103 420 §E.5's 'ceao' compatibility brand, which DASH-IF IOP
    // Part 8 v5.0.0 §5.3.3 asks for on a backward-compatible object-audio
    // E-AC-3 track: mp4:: never reads the object layer itself, so this front
    // end - which already has oba_complexity_index from the same scan that
    // built the dec3 box above - is the one that says so. A stripped
    // companion's own scan carries no such marker, so this naturally comes
    // out false for it without a separate branch.
    auto fragmented = mp4::fragment(
        track, scanned.access_units,
        mp4::FragmentOptions{.frames_per_fragment = frames_per_fragment,
                             .object_audio_brand = scanned.oba_complexity_index.has_value()});
    if (!fragmented) {
        fmt::println(stderr, "error: {}", mp4::describe(fragmented.error()));
        return std::nullopt;
    }
    // Dolby Digital Plus with Atmos objects needs CHANNELS="<N>/JOC" instead
    // of a plain channel count (see mp4/hls.hpp's own citations) - N is the
    // same decodable-object count ac3::io::scan already read off the
    // bitstream to build the dec3 box above (TS 103 420 §8.3.2's
    // complexity_index_type_a). mp4:: itself never reads that field; only
    // this CLI front end, which already has it, does. A stripped stream has
    // no such marker left, so its companion falls through to the plain
    // channel count on exactly the same code path.
    return RenditionFiles{.track = std::move(track),
                          .fragmented = std::move(*fragmented),
                          .channels_attribute =
                              scanned.oba_complexity_index
                                  ? fmt::format("{}/JOC", *scanned.oba_complexity_index)
                                  : std::string{}};
}

// HlsRendition::segments is span<const SegmentInfo> - a manifest only ever
// reads a segment's bookkeeping, never its bytes (mp4.hpp's SegmentInfo) -
// while RenditionFiles keeps the real MediaSegment list (write_rendition
// needs the bytes). The vector this returns has to outlive the HlsRendition
// built over it, hence a named local at each call site rather than a
// temporary.
std::vector<mp4::SegmentInfo> segment_infos_of(const RenditionFiles& rendition) {
    std::vector<mp4::SegmentInfo> out;
    out.reserve(rendition.fragmented.media_segments.size());
    for (const auto& segment : rendition.fragmented.media_segments) {
        out.push_back(mp4::segment_info(segment));
    }
    return out;
}

}  // namespace

int run_fmp4(std::string_view in_path, std::string_view out_dir,
             std::uint32_t frames_per_fragment, const Options& meta) {
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
    const auto primary = build_rendition(*scanned, frames_per_fragment);
    if (!primary) {
        return 1;
    }

    const std::filesystem::path dir{std::string{out_dir}};
    if (!write_rendition(dir, *primary)) {
        return 1;
    }

    const std::vector<mp4::SegmentInfo> primary_segments = segment_infos_of(*primary);
    std::vector<mp4::HlsRendition> renditions;
    renditions.push_back(mp4::HlsRendition{.track = primary->track,
                                           .segments = primary_segments,
                                           .media_playlist_uri = "audio.m3u8",
                                           .name = scanned->oba_complexity_index
                                                       ? "Dolby Atmos"
                                                       : "Audio",
                                           .channels_attribute = primary->channels_attribute,
                                           .is_default = true});

    // The 5.1 companion: the SAME bed audio, bit for bit, with the object
    // layer taken out (ac3::io::strip_objects). Its bytes have to outlive the
    // scan that views them, hence the locals here rather than a block.
    std::vector<std::byte> stripped_bytes;
    ac3::io::ScannedStream stripped_scan;
    std::optional<RenditionFiles> companion;
    std::vector<mp4::SegmentInfo> companion_segments;
    if (meta.hls_fallback_51 && scanned->oba_complexity_index) {
        auto stripped = ac3::io::strip_objects(raw);
        if (!stripped) {
            fmt::println(stderr, "error: {}", ac3::io::describe(stripped.error()));
            return 1;
        }
        stripped_bytes = std::move(stripped->bytes);
        const auto rescanned = ac3::io::scan(stripped_bytes);
        if (!rescanned) {
            fmt::println(stderr, "error: stripped stream did not scan: {}",
                         ac3::io::describe(rescanned.error()));
            return 1;
        }
        stripped_scan = *rescanned;
        companion = build_rendition(stripped_scan, frames_per_fragment);
        if (!companion) {
            return 1;
        }
        if (!write_rendition(dir / "bed51", *companion)) {
            return 1;
        }
        companion_segments = segment_infos_of(*companion);
        renditions.push_back(mp4::HlsRendition{.track = companion->track,
                                               .segments = companion_segments,
                                               .media_playlist_uri = "bed51/audio.m3u8",
                                               .name = "5.1",
                                               .channels_attribute =
                                                   companion->channels_attribute,
                                               .is_default = false});
    } else if (meta.hls_fallback_51) {
        fmt::println("note: fallback-51 ignored - {} carries no object layer to strip", in_path);
    }

    if (!write_text_to_path(dir / "master.m3u8", mp4::build_hls_master_playlist(renditions))) {
        return 1;
    }

    // The DASH side of the same two facts: TS 103 420 §D.2's JOC extension
    // type and complexity index (DASH-IF IOP Part 8 §5.3.2), and the
    // AudioChannelConfiguration @value TS 102 366 clause I.1.2.1 defines -
    // ac3::io::dash_channel_configuration is the one place that word is
    // derived from the bitstream (ac3/io/dec3.hpp).
    //
    // The MPD stays single-representation: mp4/dash.hpp builds one
    // <AdaptationSet> for one track by design, so this describes the primary
    // rendition only - the 5.1 companion has no DASH representation.
    const mp4::DashOptions dash_options{
        .joc_complexity_index = scanned->oba_complexity_index,
        .dolby_channel_configuration = ac3::io::dash_channel_configuration(*scanned)};
    const auto adaptation_set = mp4::build_dash_adaptation_set(
        primary->track, primary->fragmented.media_segments, dash_options);
    const auto mpd =
        mp4::build_dash_mpd(primary->track, primary->fragmented.media_segments, adaptation_set);
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
    const std::string companion_note =
        companion ? fmt::format(", and bed51/ with the same {} channels and the objects stripped",
                                companion->track.channels)
                  : std::string{};
    const bool eac3 = scanned->kind == ac3::io::StreamKind::kEac3;
    fmt::println(
        "wrote {} {} access units ({}, {} channels{}) as {} fragment(s) to {} "
        "(init.mp4, segment*.m4s, audio.m3u8, master.m3u8, manifest.mpd{})",
        scanned->access_units.size(), eac3 ? "E-AC-3" : "AC-3", shape, primary->track.channels,
        atmos, primary->fragmented.media_segments.size(), out_dir, companion_note);
    return 0;
}

namespace {

// mpegts::ServiceInfo is plain A/52 field values (see its own header comment
// on why that module maps them onto each registry's tables rather than being
// handed finished descriptor bytes), so this is a field-for-field copy out of
// what ac3::io::scan already read off the bitstream - no derivation here, and
// nothing invented. The two values that are NOT in any bitstream, because
// they describe how services in a multiplex relate rather than what one
// stream contains, come from the operator via mainid=/asvc= and stay unset
// otherwise.
mpegts::ServiceInfo service_info_from(const ac3::io::ScannedStream& scanned,
                                      const Options& meta) {
    mpegts::ServiceInfo service{
        .bsmod = scanned.bsmod,
        .bsmod_present = scanned.bsmod_present,
        .acmod = static_cast<int>(scanned.acmod),
        .lfe = scanned.lfe,
        .channels = scanned.channels,
        .bsid = scanned.bsid,
        .dsurmod = scanned.dsurmod,
        .bit_rate_code = scanned.bit_rate_code,
        .sample_rate_code = static_cast<int>(scanned.sample_rate),
        .mix_metadata = scanned.mix_metadata,
        .independent_substreams = scanned.independent_substreams,
    };
    for (std::size_t i = 0; i < service.associated_substreams.size(); ++i) {
        const auto& from = scanned.associated_substreams[i];
        service.associated_substreams[i] =
            mpegts::SubstreamService{.present = from.present,
                                     .bsmod = from.bsmod,
                                     .bsmod_present = from.bsmod_present,
                                     .acmod = static_cast<int>(from.acmod),
                                     .lfe = from.lfe,
                                     .dsurmod = 0,
                                     .mix_metadata = from.mix_metadata};
    }
    service.mainid = meta.mainid;
    if (meta.mainid) {
        // A/52 Table A4.6: with a main-service number given and nothing said
        // about ranking, "primary audio" is what a lone main service is.
        service.priority = 1;
    }
    if (meta.asvc) {
        service.asvc = static_cast<std::uint8_t>(*meta.asvc);
    }
    return service;
}

}  // namespace

int run_ts(std::string_view in_path, std::string_view out_path, std::string_view profile_name,
           const Options& meta) {
    mpegts::BroadcastProfile profile = mpegts::BroadcastProfile::kDvb;
    if (profile_name == "atsc") {
        profile = mpegts::BroadcastProfile::kAtsc;
    } else if (!profile_name.empty() && profile_name != "dvb") {
        fmt::println(stderr, "error: unknown TS profile '{}' (expected dvb or atsc)", profile_name);
        return 1;
    }
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

    const auto samples_per_frame = track_samples_per_frame(*scanned);
    if (!samples_per_frame) {
        return 1;
    }

    const mpegts::AudioTrack track{
        .codec = eac3 ? mpegts::AudioCodec::kEac3 : mpegts::AudioCodec::kAc3,
        .sample_rate = ac3::sample_rate_hz(scanned->sample_rate),
        .channels = scanned->channels,
        .samples_per_frame = *samples_per_frame,
        .service = service_info_from(*scanned, meta)};
    const auto file = mpegts::mux(track, units, mpegts::MuxOptions{.profile = profile});
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
    fmt::println("wrote {} {} access units ({}, {} channels, {} bytes) to {} ({} profile)",
                 units.size(), eac3 ? "E-AC-3" : "AC-3", shape, track.channels,
                 file->size(), out_path,
                 profile == mpegts::BroadcastProfile::kAtsc ? "ATSC" : "DVB");
    return 0;
}

// --- container input (ROADMAP.md's IO2) -------------------------------------

namespace {

// Which container a file actually is, decided by its first bytes rather than
// its name. A rip is as likely to be called "title00.mkv" when it is not one
// as it is to have no extension at all, and the failure a wrong guess
// produces ("no EBML header") reads like a corrupt file rather than like the
// wrong parser - so the name is never consulted.
enum class ContainerKind : std::uint8_t { kUnknown, kMatroska, kMp4, kMpegTs };

// EBML's own magic: the four bytes of the EBML header id every Matroska and
// WebM file opens with - the same kEbmlHeader constant
// src/matroska/src/ebml_detail.hpp holds, written out big-endian.
constexpr std::array<std::byte, 4> kEbmlMagic{std::byte{0x1A}, std::byte{0x45}, std::byte{0xDF},
                                              std::byte{0xA3}};

// ISOBMFF has no magic at offset 0 - it opens with a box, whose first four
// bytes are a LENGTH. The type is what identifies it, four bytes in, and
// 'ftyp' is what a well-formed file leads with (ISO/IEC 14496-12 4.3 says it
// "should be placed as early as possible"). 'styp' is a bare CMAF media
// segment, and a plain 'moov'/'mdat'/'moof' opener occurs in files written
// by tools that skipped ftyp - all of them are what a reader is handed in
// practice.
constexpr std::array<std::string_view, 5> kIsobmffLeadingTypes{"ftyp", "styp", "moov", "moof",
                                                               "mdat"};

[[nodiscard]] bool has_isobmff_box_at_start(std::span<const std::byte> head) {
    if (head.size() < 8) {
        return false;
    }
    const std::string_view type{reinterpret_cast<const char*>(head.data()) + 4, 4};
    return std::ranges::find(kIsobmffLeadingTypes, type) != kIsobmffLeadingTypes.end();
}

// A transport stream has no header at all - it is a bare repeating grid of
// 188-byte packets, each starting with 0x47, and a capture may begin
// anywhere in it. So the test is the grid itself: a sync byte that recurs at
// one of the three strides in the wild (188, M2TS's 192, or 204 with parity)
// several times over. A lone 0x47 proves nothing; five in a row exactly a
// stride apart is not a coincidence.
//
// Checked LAST, after the two formats that do have magic: an MP4 or Matroska
// file can easily contain a 0x47 pattern by chance somewhere in its audio,
// and the grid test is the loosest of the three.
constexpr std::array<std::size_t, 3> kTsStrides{188, 192, 204};
constexpr int kTsSyncRuns = 5;

[[nodiscard]] bool has_ts_packet_grid(std::span<const std::byte> head) {
    for (std::size_t at = 0; at < head.size(); ++at) {
        if (std::to_integer<std::uint8_t>(head[at]) != 0x47) {
            continue;
        }
        for (const auto stride : kTsStrides) {
            int seen = 1;
            for (int i = 1; i < kTsSyncRuns; ++i) {
                const std::size_t next = at + (stride * static_cast<std::size_t>(i));
                if (next >= head.size() ||
                    std::to_integer<std::uint8_t>(head[next]) != 0x47) {
                    break;
                }
                ++seen;
            }
            if (seen >= kTsSyncRuns) {
                return true;
            }
        }
    }
    return false;
}

ContainerKind sniff_container(std::span<const std::byte> head) {
    if (head.size() >= kEbmlMagic.size() &&
        std::equal(kEbmlMagic.begin(), kEbmlMagic.end(), head.begin())) {
        return ContainerKind::kMatroska;
    }
    if (has_isobmff_box_at_start(head)) {
        return ContainerKind::kMp4;
    }
    if (has_ts_packet_grid(head)) {
        return ContainerKind::kMpegTs;
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
    const auto kind = sniff_container(first);
    if (kind == ContainerKind::kUnknown) {
        fmt::println(
            stderr,
            "error: {} is not a container this build reads (expected Matroska/WebM, MP4 or "
            "MPEG-2 Transport Stream)",
            in_path);
        return 1;
    }

    EncodedStreamSink sink;
    if (!sink.open(out_path, /*keep_partial=*/false)) {
        return 1;
    }
    // A write failure is latched rather than thrown out of the callback: a
    // reader cannot be told to stop mid-chunk, and unwinding through one
    // would leave its parse state undefined.
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

    // The two readers have the same shape but no common base class - the
    // modules are deliberately independent of each other, not just of
    // ac3::forge - so the drive loop is written once against whichever one
    // the sniff picked, as a template over the pair.
    std::string codec_id;
    std::uint32_t sample_rate = 0;
    int channels = 0;
    int status = 0;
    const auto drive = [&]<typename Reader, typename Describe>(Reader& reader,
                                                               Describe describe) {
        for (auto bytes = first; !bytes.empty(); bytes = read_chunk()) {
            const auto pushed = reader.push(bytes, on_frame);
            if (!pushed) {
                status = fail(describe(pushed.error()));
                return;
            }
            if (write_failed) {
                status = fail("write failed");
                return;
            }
        }
        // mpegts::Reader::finish() takes the callback and the other two do
        // not, because only a transport stream can have a packet that ends
        // at end-of-input (the unbounded PES length form). The difference is
        // real, so it is dispatched on rather than papered over.
        const auto finished = [&] {
            if constexpr (requires { reader.finish(on_frame); }) {
                return reader.finish(on_frame);
            } else {
                return reader.finish();
            }
        }();
        if (!finished) {
            status = fail(describe(finished.error()));
            return;
        }
        if (write_failed) {
            status = fail("write failed");
            return;
        }
    };

    if (kind == ContainerKind::kMatroska) {
        matroska::Reader reader{};
        drive(reader, [](matroska::DemuxError e) { return matroska::describe(e); });
        codec_id = std::string{reader.track().codec_id};
        sample_rate = reader.track().sample_rate;
        channels = reader.track().channels;
    } else if (kind == ContainerKind::kMp4) {
        mp4::Reader reader{};
        drive(reader, [](mp4::DemuxError e) { return mp4::describe(e); });
        codec_id = reader.track().codec_id;
        sample_rate = reader.track().sample_rate;
        channels = reader.track().channels;
    } else {
        mpegts::Reader reader{};
        drive(reader, [](mpegts::DemuxError e) { return mpegts::describe(e); });
        // A transport stream's PMT names the codec but carries no sample
        // rate or channel count - those live in the bitstream, which this
        // command deliberately never looks inside. Reported as absent
        // rather than guessed.
        codec_id = reader.stream().eac3 ? "E-AC-3" : "AC-3";
    }
    if (status != 0) {
        return status;
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
    if (sample_rate != 0) {
        fmt::println(status_stream(out_path),
                     "wrote {} access units ({}, {} Hz, {} channels, {} bytes) to {}",
                     sink.frames(), codec_id, sample_rate, channels, sink.total_bytes(),
                     out_path);
    } else {
        // MPEG-TS: the container named the codec and nothing else. 'probe'
        // or 'levels' on the result reads the rest off the bitstream.
        fmt::println(status_stream(out_path), "wrote {} PES payloads ({}, {} bytes) to {}",
                     sink.frames(), codec_id, sink.total_bytes(), out_path);
    }
    return 0;
}

}  // namespace ac3cli::commands
