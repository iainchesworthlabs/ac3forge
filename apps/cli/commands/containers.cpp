#include "containers.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <print>
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
#include "mp4/dash.hpp"
#include "mp4/hls.hpp"
#include "mp4/mp4.hpp"
#include "mpegts/mpegts.hpp"
#include "../support.hpp"

namespace ac3cli::commands {

namespace {

bool write_bytes_to_path(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream out{path, std::ios::binary};
    if (!out) {
        std::println(stderr, "error: cannot open {} for writing", path.string());
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        std::println(stderr, "error: write failed for {}", path.string());
        return false;
    }
    return true;
}

bool write_text_to_path(const std::filesystem::path& path, std::string_view text) {
    return write_bytes_to_path(
        path, std::as_bytes(std::span{reinterpret_cast<const char*>(text.data()), text.size()}));
}

// A minimal but complete DASH MPD document wrapped around
// mp4::build_dash_adaptation_set()'s <AdaptationSet> snippet - the library
// stops at the snippet (mp4.hpp/dash.hpp's own scope: single-representation
// audio, no opinion on the surrounding document), the CLI front end supplies
// the rest, the same boundary mp4::mux() not doing file I/O already draws.
// profiles="isoff-live" is what a SegmentTemplate-based MPD declares
// regardless of static/live (ISO/IEC 23009-1 Annex A.3) - "isoff-on-demand"
// instead mandates a single SegmentBase/index-range layout this module does
// not produce.
std::string build_dash_mpd(const mp4::AudioTrack& track,
                           std::span<const mp4::MediaSegment> segments,
                           std::string_view adaptation_set) {
    std::uint64_t total_samples = 0;
    for (const auto& segment : segments) {
        total_samples += segment.duration_samples;
    }
    const double total_seconds =
        static_cast<double>(total_samples) / static_cast<double>(track.sample_rate);
    return std::format(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" type=\"static\" "
        "mediaPresentationDuration=\"PT{:.3f}S\" minBufferTime=\"PT2S\" "
        "profiles=\"urn:mpeg:dash:profile:isoff-live:2011\">\n"
        "  <Period>\n"
        "{}"
        "  </Period>\n"
        "</MPD>\n",
        total_seconds, adaptation_set);
}

}  // namespace

int run_mkv(std::string_view in_path, std::string_view out_path) {
    const auto raw = read_all(in_path);
    if (raw.empty()) {
        std::println(stderr, "error: cannot open {}", in_path);
        return 1;
    }
    // Everything the container needs to declare comes out of the bitstream:
    // the format, the access-unit boundaries, the sample rate and the channel
    // count. This used to take a layout argument to learn the channel count,
    // which meant a wrong one silently produced a file that misdescribed
    // itself - and nothing could catch it.
    const auto scanned = ac3::io::scan(raw);
    if (!scanned) {
        std::println(stderr, "error: {}", ac3::io::describe(scanned.error()));
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
        std::println(stderr, "error: {}", matroska::describe(file.error()));
        return 1;
    }
    std::ofstream out{std::string{out_path}, std::ios::binary};
    if (!out) {
        std::println(stderr, "error: cannot write {}", out_path);
        return 1;
    }
    out.write(reinterpret_cast<const char*>(file->data()),
              static_cast<std::streamsize>(file->size()));
    if (!out) {
        std::println(stderr, "error: write failed");
        return 1;
    }
    // Name the layout only when one substream carries the whole thing. With
    // dependents the acmod describes the BED, so printing it beside a wider
    // rendered channel count would just contradict itself.
    const std::string shape =
        scanned->substreams_per_unit > 1
            ? std::format("{} substreams", scanned->substreams_per_unit)
            : std::string{ac3::analysis::layout_name(scanned->acmod, scanned->lfe)};
    std::println("wrote {} {} access units ({}, {} channels, {} bytes) to {}",
                 units.size(), eac3 ? "E-AC-3" : "AC-3", shape, track.channels,
                 file->size(), out_path);
    return 0;
}

int run_mp4(std::string_view in_path, std::string_view out_path) {
    const auto raw = read_all(in_path);
    if (raw.empty()) {
        std::println(stderr, "error: cannot open {}", in_path);
        return 1;
    }
    const auto scanned = ac3::io::scan(raw);
    if (!scanned) {
        std::println(stderr, "error: {}", ac3::io::describe(scanned.error()));
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
        std::println(stderr, "error: {}", mp4::describe(file.error()));
        return 1;
    }
    std::ofstream out{std::string{out_path}, std::ios::binary};
    if (!out) {
        std::println(stderr, "error: cannot write {}", out_path);
        return 1;
    }
    out.write(reinterpret_cast<const char*>(file->data()),
              static_cast<std::streamsize>(file->size()));
    if (!out) {
        std::println(stderr, "error: write failed");
        return 1;
    }
    const std::string shape =
        scanned->substreams_per_unit > 1
            ? std::format("{} substreams", scanned->substreams_per_unit)
            : std::string{ac3::analysis::layout_name(scanned->acmod, scanned->lfe)};
    const std::string atmos =
        scanned->oba_complexity_index
            ? std::format(", Atmos complexity {}", *scanned->oba_complexity_index)
            : std::string{};
    std::println("wrote {} {} access units ({}, {} channels{}, {} bytes) to {}",
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
        std::println(stderr, "error: cannot create directory {} ({})", dir.string(), ec.message());
        return false;
    }
    if (!write_bytes_to_path(dir / "init.mp4", rendition.fragmented.init_segment)) {
        return false;
    }
    for (const auto& segment : rendition.fragmented.media_segments) {
        const auto name = std::format("segment{}.m4s", segment.sequence_number);
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
    mp4::AudioTrack track{.codec_id = std::string{eac3 ? mp4::kCodecEac3 : mp4::kCodecAc3},
                          .sample_rate = ac3::sample_rate_hz(scanned.sample_rate),
                          .channels = scanned.channels,
                          .samples_per_frame = ac3::kSamplesPerFrame,
                          .codec_config = ac3::io::build_codec_config_box(scanned)};
    auto fragmented = mp4::fragment(
        track, scanned.access_units,
        mp4::FragmentOptions{.frames_per_fragment = frames_per_fragment});
    if (!fragmented) {
        std::println(stderr, "error: {}", mp4::describe(fragmented.error()));
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
                                  ? std::format("{}/JOC", *scanned.oba_complexity_index)
                                  : std::string{}};
}

}  // namespace

int run_fmp4(std::string_view in_path, std::string_view out_dir,
             std::uint32_t frames_per_fragment, const Options& meta) {
    const auto raw = read_all(in_path);
    if (raw.empty()) {
        std::println(stderr, "error: cannot open {}", in_path);
        return 1;
    }
    const auto scanned = ac3::io::scan(raw);
    if (!scanned) {
        std::println(stderr, "error: {}", ac3::io::describe(scanned.error()));
        return 1;
    }
    const bool eac3 = scanned->kind == ac3::io::StreamKind::kEac3;
    const auto primary = build_rendition(*scanned, frames_per_fragment);
    if (!primary) {
        return 1;
    }

    const std::filesystem::path dir{std::string{out_dir}};
    if (!write_rendition(dir, *primary)) {
        return 1;
    }

    std::vector<mp4::HlsRendition> renditions;
    renditions.push_back(mp4::HlsRendition{.track = primary->track,
                                           .segments = primary->fragmented.media_segments,
                                           .media_playlist_uri = "audio.m3u8",
                                           .name = scanned->oba_complexity_index
                                                       ? "Dolby Atmos"
                                                       : "Audio",
                                           .channels_attribute = primary->channels_attribute,
                                           .is_default = true});

    // The 5.1 companion: the SAME bed audio, bit for bit, with the object
    // layer taken out (ac3::io::strip_objects). Its bytes have to outlive the
    // scan that views them, hence the two locals here rather than a block.
    std::vector<std::byte> stripped_bytes;
    ac3::io::ScannedStream stripped_scan;
    std::optional<RenditionFiles> companion;
    if (meta.hls_fallback_51 && scanned->oba_complexity_index) {
        auto stripped = ac3::io::strip_objects(raw);
        if (!stripped) {
            std::println(stderr, "error: {}", ac3::io::describe(stripped.error()));
            return 1;
        }
        stripped_bytes = std::move(stripped->bytes);
        const auto rescanned = ac3::io::scan(stripped_bytes);
        if (!rescanned) {
            std::println(stderr, "error: stripped stream did not scan: {}",
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
        renditions.push_back(mp4::HlsRendition{.track = companion->track,
                                               .segments = companion->fragmented.media_segments,
                                               .media_playlist_uri = "bed51/audio.m3u8",
                                               .name = "5.1",
                                               .channels_attribute =
                                                   companion->channels_attribute,
                                               .is_default = false});
    } else if (meta.hls_fallback_51) {
        std::println("note: fallback-51 ignored - {} carries no object layer to strip", in_path);
    }

    if (!write_text_to_path(dir / "master.m3u8", mp4::build_hls_master_playlist(renditions))) {
        return 1;
    }

    // The MPD stays single-representation: mp4/dash.hpp builds one
    // <AdaptationSet> for one track by design, and DASH's own JOC signalling
    // is ROADMAP.md's IO5, not this. It describes the primary rendition.
    const auto adaptation_set =
        mp4::build_dash_adaptation_set(primary->track, primary->fragmented.media_segments);
    const auto mpd =
        build_dash_mpd(primary->track, primary->fragmented.media_segments, adaptation_set);
    if (!write_text_to_path(dir / "manifest.mpd", mpd)) {
        return 1;
    }

    const std::string shape =
        scanned->substreams_per_unit > 1
            ? std::format("{} substreams", scanned->substreams_per_unit)
            : std::string{ac3::analysis::layout_name(scanned->acmod, scanned->lfe)};
    const std::string atmos =
        scanned->oba_complexity_index
            ? std::format(", Atmos complexity {}", *scanned->oba_complexity_index)
            : std::string{};
    const std::string companion_note =
        companion ? std::format(", and bed51/ with the same {} channels and the objects stripped",
                                companion->track.channels)
                  : std::string{};
    std::println(
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
        std::println(stderr, "error: unknown TS profile '{}' (expected dvb or atsc)", profile_name);
        return 1;
    }
    const auto raw = read_all(in_path);
    if (raw.empty()) {
        std::println(stderr, "error: cannot open {}", in_path);
        return 1;
    }
    const auto scanned = ac3::io::scan(raw);
    if (!scanned) {
        std::println(stderr, "error: {}", ac3::io::describe(scanned.error()));
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
        .samples_per_frame = ac3::kSamplesPerFrame,
        .service = service_info_from(*scanned, meta)};
    const auto file = mpegts::mux(track, units, mpegts::MuxOptions{.profile = profile});
    if (!file) {
        std::println(stderr, "error: {}", mpegts::describe(file.error()));
        return 1;
    }
    std::ofstream out{std::string{out_path}, std::ios::binary};
    if (!out) {
        std::println(stderr, "error: cannot write {}", out_path);
        return 1;
    }
    out.write(reinterpret_cast<const char*>(file->data()),
              static_cast<std::streamsize>(file->size()));
    if (!out) {
        std::println(stderr, "error: write failed");
        return 1;
    }
    const std::string shape =
        scanned->substreams_per_unit > 1
            ? std::format("{} substreams", scanned->substreams_per_unit)
            : std::string{ac3::analysis::layout_name(scanned->acmod, scanned->lfe)};
    std::println("wrote {} {} access units ({}, {} channels, {} bytes) to {} ({} profile)",
                 units.size(), eac3 ? "E-AC-3" : "AC-3", shape, track.channels,
                 file->size(), out_path,
                 profile == mpegts::BroadcastProfile::kAtsc ? "ATSC" : "DVB");
    return 0;
}

}  // namespace ac3cli::commands
