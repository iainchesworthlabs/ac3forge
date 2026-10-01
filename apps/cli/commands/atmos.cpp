#include "atmos.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <limits>
#include <numbers>
#include <optional>
#include <fmt/base.h>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../exit_codes.hpp"
#include "../multi_source.hpp"
#include "../support.hpp"
#include "iclforge/ac3/analysis/levels.hpp"
#include "iclforge/ac3/core/tables.hpp"
#include "iclforge/ac3/encoder/assignment.hpp"
#include "iclforge/ac3/core/eac3_tables.hpp"  // blocks_per_syncframe
#include "iclforge/ac3/io/elementary.hpp"
#include "iclforge/ac3/io/object_strip.hpp"
#include "iclforge/ac3/io/wav.hpp"
#include "iclforge/ac3/oba/atmos.hpp"
#include "iclforge/objects/motion.hpp"
#include "iclforge/objects/oamd.hpp"
#include "iclforge/objects/scene.hpp"
#include "iclforge/signing/emdf_atmos_signer.hpp"
#include "iclforge/signing/signing_key.hpp"
#include "iclforge/ac4/ac4.hpp"
#include "ac4_encode_core.hpp"
#include "ac4_objects_core.hpp"
#include "iclforge/ac4enc/encoder.hpp"
#include "../adm/atmos_adm.hpp"
#include "../adm/atmos_iab.hpp"

namespace forge_cli::commands {

namespace plan = iclforge::ac3::plan;

namespace {

// Applies EMDF object signing to freshly-encoded Atmos units when the operator
// asked for it (sign-objects) and supplied a key. Returns the number of frames
// signed, or nullopt if signing was requested but the key could not be loaded
// (the message is already printed). Not requested -> 0, units untouched. The
// key comes from the operator at runtime (signing-key=<path> or the
// ICLFORGE_SIGNING_KEY[_FILE] env vars) and is never stored - see
// docs/concepts/object-signing.md.
std::optional<int> apply_object_signing(std::vector<std::vector<std::byte>>& units,
                                        const Options& meta) {
    if (!meta.sign_objects) {
        return 0;
    }
    const auto key = iclforge::signing::load_signing_key(meta.signing_key.value_or(""));
    if (!key.has_value()) {
        if (key.error().kind == iclforge::signing::KeyErrorKind::kAbsent) {
            fmt::println(stderr,
                         "error: sign-objects needs a key — pass signing-key=<path>, or set "
                         "ICLFORGE_SIGNING_KEY_FILE / ICLFORGE_SIGNING_KEY");
        } else {
            fmt::println(stderr, "error: {}", key.error().message);
        }
        return std::nullopt;
    }
    int signed_count = 0;
    for (auto& unit : units) {
        signed_count += iclforge::signing::sign_atmos_stream(unit, *key);
    }
    return signed_count;
}

// Reads a scene file: either the hand-authored keyframe grammar this command
// has always taken ("object_index time_s x y z gain lfe_send" per line, '#'
// comments, blank lines skipped) or the JSON object-scene form, told apart by
// their first character. Both are iclforge::oba's now - see ac3/oba/scene.hpp -
// so the GUI, the examples and this share one reader rather than three.
//
// Returns the file's objects and orientation without filling in the indices a
// keyframe file skipped: what those should be is this command's policy and
// each caller below applies its own.
std::optional<iclforge::oba::SceneContents> read_scene_file(std::string_view path) {
    std::ifstream in{std::string{path}, std::ios::binary};
    if (!in) {
        fmt::println(stderr, "error: cannot open {}", path);
        return std::nullopt;
    }
    const std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    auto contents = iclforge::oba::read_scene(text);
    if (!contents.has_value()) {
        // Line 0 means the format had no line to point at (a JSON-level
        // complaint about the scene as a whole); everything else keeps the
        // path:line: prefix this command has always printed.
        if (contents.error().line != 0) {
            fmt::println(stderr, "error: {}:{}: {}", path, contents.error().line,
                         contents.error().message);
        } else {
            fmt::println(stderr, "error: {}: {}", path, contents.error().message);
        }
        return std::nullopt;
    }
    return std::move(*contents);
}

// The objects a scene file described, padded out to `count` with `fallback`
// (an index the file skipped, or one past its end), then validated. `fallback`
// is asked for an index because atmos-encode's default placement differs per
// object where atmos-path's does not.
std::optional<iclforge::oba::ObjectScene> scene_of(std::string_view path,
                                                   iclforge::oba::SceneContents contents,
                                                   std::size_t count, const auto& fallback) {
    contents.objects.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (contents.objects[i].automation.empty()) {
            const iclforge::oba::ObjectPlacement rest = fallback(i);
            contents.objects[i].automation.push_back({.time_s = 0.0,
                                                      .position = rest.position,
                                                      .gain = rest.gain,
                                                      .lfe_send = rest.lfe_send});
        }
    }
    auto scene =
        iclforge::oba::ObjectScene::create(std::move(contents.objects), contents.orientation);
    if (!scene.has_value()) {
        fmt::println(stderr, "error: {}: {}", path, scene.error().message);
        return std::nullopt;
    }
    return std::move(*scene);
}

// atmos-cbi's named layouts. DEE's own --input-format cbi_wav channel order
// (measured against a real Dolby Encoding Engine 5.1.4 stream - see
// tools/generators/gen_object_fixture.py and tests/ac3/oba/test_dee_joc_fixture.cpp)
// is exactly iclforge::oba::bed_labels()'s Table 12 order for that bed, so this
// table names each layout only by its bed flags and lets bed_labels() derive
// the channel order AtmosEncoder::encode_bed_frame expects - no separate,
// hand-maintained channel list to keep in sync with it. Only the 5.1.4 row has
// been checked against a real DEE-produced stream; 7.1.4 and 9.1.6 extend it
// by Table 12's own channel order, unverified against DEE itself - see
// docs/concepts/atmos-joc.md.
struct CbiLayout {
    std::string_view name;
    std::uint16_t bed;
};

constexpr std::array<CbiLayout, 3> kCbiLayouts{{
    {"5.1.4", iclforge::oba::bed::kLR | iclforge::oba::bed::kC | iclforge::oba::bed::kLfe |
                  iclforge::oba::bed::kLsRs | iclforge::oba::bed::kTflTfr |
                  iclforge::oba::bed::kTblTbr},
    {"7.1.4", iclforge::oba::bed::kLR | iclforge::oba::bed::kC | iclforge::oba::bed::kLfe |
                  iclforge::oba::bed::kLsRs | iclforge::oba::bed::kLbRb |
                  iclforge::oba::bed::kTflTfr | iclforge::oba::bed::kTblTbr},
    {"9.1.6", iclforge::oba::bed::kLR | iclforge::oba::bed::kC | iclforge::oba::bed::kLfe |
                  iclforge::oba::bed::kLsRs | iclforge::oba::bed::kLbRb |
                  iclforge::oba::bed::kLwRw | iclforge::oba::bed::kTflTfr |
                  iclforge::oba::bed::kTslTsr | iclforge::oba::bed::kTblTbr},
}};

[[nodiscard]] std::optional<std::uint16_t> resolve_cbi_layout(std::string_view name) {
    for (const auto& layout : kCbiLayouts) {
        if (layout.name == name) {
            return layout.bed;
        }
    }
    return std::nullopt;
}

// The layout whose channel count matches, when the caller didn't name one -
// unambiguous because 10 (5.1.4), 12 (7.1.4) and 16 (9.1.6) are all distinct.
[[nodiscard]] std::optional<std::uint16_t> cbi_layout_for_channel_count(std::size_t channels) {
    for (const auto& layout : kCbiLayouts) {
        if (static_cast<std::size_t>(iclforge::oba::bed::channel_count(layout.bed)) == channels) {
            return layout.bed;
        }
    }
    return std::nullopt;
}

// atmos-adm/atmos-iab with codec=ac4 (planning/ac4.md, I5): iclforge::oba::ObjectPlacement (this
// project's E-AC-3/Atmos object model) and iclforge::ac4::ObjectProperties (TS 103 190-2 Annex F)
// share one room coordinate system - X 0 (left wall) to 1 (right), Y 0 (front) to 1 (back), Z -1
// (floor) to 1 (ceiling), confirmed against apps/common/ac4_object_render.hpp's own header comment
// - so position carries over unconverted; gain does not, since oba's is linear and AC-4's is dB
// (Table 108-adjacent range +15 to -49, or -infinity for silence).
// iclforge::apps::ac4_object_properties does both.
//
// The AC-4 branch of run_atmos_adm/run_atmos_iab (codec=ac4): every bed/object channel the source
// names becomes a dynamic AC-4 object driven by its own ObjectPath, the same treatment the E-AC-3
// branches beside this function give a bed channel (panned by position, no speaker-anchored
// iclforge::ac4::BedChannel assigned) - is_bed is reported in the summary line and nothing else,
// exactly as it already is for E-AC-3 above. AC-4's object substream is frame_rate_index 13 only
// (ac4enc/encoder.hpp, SubstreamConfig::objects), so metadata updates land on that fixed
// 2048-sample grid: one update per object per frame, ramped over the whole frame from the previous
// one, evaluated at the frame's END time - the convention every Atmos-encode command in this file
// uses. The steps themselves are apps/common/ac4_objects_core.cpp's, which forge-gui's AC-4 objects
// take too.
int run_atmos_objects_to_ac4(std::string_view source_kind, std::uint32_t sample_rate,
                             const std::vector<bool>& is_bed,
                             const std::vector<iclforge::oba::ObjectPath>& paths,
                             const std::vector<std::span<const float>>& pcm,
                             std::string_view in_path, std::string_view out_path,
                             std::uint32_t bitrate, const Options& meta) {
    if (sample_rate != 48000 && sample_rate != 44100) {
        fmt::println(stderr,
                     "error: {} objects need a 48 or 44.1 kHz source for AC-4 (its object "
                     "substream is frame_rate_index 13 only); {} is {} Hz",
                     source_kind, in_path, sample_rate);
        return kExitInput;
    }
    const std::size_t count = paths.size();
    if (count < 1) {
        fmt::println(stderr, "error: {} names no bed/object channel", in_path);
        return kExitInput;
    }

    const iclforge::apps::Ac4ObjectsParams params{
        .sample_rate_hz = sample_rate,
        .bitrate_kbps = static_cast<int>(bitrate),
        .dialnorm_db = static_cast<double>(meta.p.dialnorm),
        .coding = meta.ac4_atmos_coding.value_or(iclforge::ac4::ObjectCoding::kAjoc)};
    const auto encoded = iclforge::apps::encode_ac4_objects(
        params, std::vector<bool>{}, pcm,
        [&paths](double time_s) { return iclforge::oba::evaluate_placements(paths, time_s); });
    if (!encoded.has_value()) {
        switch (encoded.error().kind) {
            case iclforge::apps::Ac4ObjectsError::Kind::kRefused:
                fmt::println(stderr, "error: the encoder refuses {} objects at {} kbps ({})",
                             source_kind, bitrate, encoded.error().message);
                return kExitUsage;
            case iclforge::apps::Ac4ObjectsError::Kind::kEncode:
                fmt::println(stderr, "error: {}: {}", in_path, encoded.error().message);
                return kExitInput;
            case iclforge::apps::Ac4ObjectsError::Kind::kFlush:
                break;
        }
        fmt::println(stderr, "error: {}", encoded.error().message);
        return kExitInput;
    }
    std::vector<std::vector<std::byte>> bytes;
    bytes.reserve(encoded->frames.size());
    for (const iclforge::ac4::EncodedFrame& frame : encoded->frames) {
        bytes.push_back(iclforge::ac4::sync_frame(frame.raw_ac4_frame, /*crc=*/true));
    }
    if (!write_frames(out_path, bytes)) {
        return kExitOutput;
    }
    std::size_t bed_count = 0;
    for (const bool b : is_bed) {
        bed_count += b ? 1U : 0U;
    }
    const auto status = status_stream(out_path);
    status_println(status, "encoded {} AC-4 frames ({} kbps, {} Hz) from {} to {}",
                   encoded->frames.size(), bitrate, sample_rate, in_path, out_path);
    status_println(status, "  {} bed speaker feed(s) + {} dynamic object(s) = {} objects, {}-coded",
                   bed_count, count - bed_count, count,
                   params.coding == iclforge::ac4::ObjectCoding::kAjoc ? "A-JOC" : "direct");
    status_println(status, "  the decoder's output lags the input by {} samples",
                   encoded->lag_samples);
    return kExitOk;
}

}  // namespace

int run_atmos(std::string_view out_path, std::uint32_t seconds, std::uint32_t bitrate,
              std::uint32_t objects, std::uint32_t orbit_seconds, std::string_view mode,
              const Options& meta) {
    if (objects < 1 || objects > 15) {
        fmt::println(stderr, "error: 1 to 15 objects (the bed's LFE is the 16th, "
                             "and TS 103 420 §8.3.2.2 caps the total at 16)");
        return kExitUsage;
    }
    // "objects" emits the JOC + OAMD container; "bed51" omits it so the stream
    // degrades to a plain 5.1 bed on a decoder that refuses an unvalidated
    // object container instead of falling back (see AtmosConfig).
    if (mode != "objects" && mode != "bed51") {
        fmt::println(stderr, "error: mode is 'objects' (default) or 'bed51'");
        return kExitUsage;
    }
    const bool emit_objects = mode != "bed51";
    const auto count = static_cast<std::size_t>(objects);
    // One frame of input per encode_frame call - kSamplesPerFrame at the
    // default numblkscod, 256/512/768 under a short syncframe. Derived
    // once, next to the config that set it, so the feed loop below and
    // the encoder can never disagree about a frame's length.
    const std::size_t frame_samples =
        static_cast<std::size_t>(iclforge::ac3::eac3::blocks_per_syncframe(meta.atmos_numblkscod) *
                                 iclforge::ac3::kSamplesPerBlock);
    iclforge::ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = bitrate,
                                    .dialnorm = meta.p.dialnorm,
                                    .num_bands_idx = 4,
                                    .emit_object_metadata = emit_objects,
                                    .fast_mdct = meta.fast_mdct,
                                    .joc_domain = meta.joc_domain,
                                    .numblkscod = meta.atmos_numblkscod},
                                   static_cast<int>(objects)};

    // Distinct tones so the objects are separable in the first place, and a
    // reader with an object renderer can tell which one ended up where.
    std::vector<double> tone_hz(count);
    std::vector<iclforge::oba::ObjectPath> paths;
    paths.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        tone_hz[i] = 220.0 * std::pow(2.0, static_cast<double>(i) * 0.45);
        // Rates that are not simple ratios of each other, so the objects do
        // not lock into formation and stay separable.
        const double rate = 1.0 / (static_cast<double>(orbit_seconds) *
                                   (1.0 + 0.31 * static_cast<double>(i)));
        // Spread around the ring to begin with, or a short clip would show
        // them all bunched in the same quadrant - and objects that share a
        // direction are exactly the ones JOC cannot separate.
        const double phase = 2.0 * std::numbers::pi * static_cast<double>(i) /
                             static_cast<double>(count);
        const double height = count == 1 ? 0.5
                                         : -1.0 + 2.0 * static_cast<double>(i) /
                                                      static_cast<double>(count - 1);
        paths.push_back(iclforge::oba::make_orbit_path(
            rate, phase, height, 0.7 / std::sqrt(static_cast<double>(count)),
            // Only the lowest object feeds the LFE, and only a little: it is
            // the one channel JOC never touches.
            i == 0 ? 0.2 : 0.0));
    }

    const std::uint64_t frames = (static_cast<std::uint64_t>(seconds) * 48000 + static_cast<std::uint64_t>(frame_samples) - 1) /
                                 static_cast<std::uint64_t>(frame_samples);
    std::vector<std::vector<float>> essences(count,
                                             std::vector<float>(frame_samples));
    std::vector<std::span<const float>> views(count);
    // Streamed out as encoded unless sign-objects defers them (the signing
    // pass below rewrites every frame after the loop). keep_partial is
    // hard-off: this command has never honoured keep-partial - its output
    // is synthetic and regenerable - so a mid-run failure must keep
    // leaving no file behind, which is exactly what abort() then does.
    EncodedStreamSink out_sink;
    if (!out_sink.open(out_path, /*keep_partial=*/false, /*defer=*/meta.sign_objects)) {
        return kExitOutput;
    }

    std::uint64_t n0 = 0;
    for (std::uint64_t f = 0; f < frames; ++f) {
        // The placement is the object's position at the END of the frame,
        // because that is where both metadata layers interpolate to: OAMD's
        // ramp and the JOC matrix both finish there.
        const double t = static_cast<double>(n0 + frame_samples) / 48000.0;
        const auto placement = iclforge::oba::evaluate_placements(paths, t);
        for (std::size_t i = 0; i < count; ++i) {
            for (std::size_t n = 0; n < frame_samples; ++n) {
                essences[i][static_cast<std::size_t>(n)] = static_cast<float>(
                    std::sin(2.0 * std::numbers::pi * tone_hz[i] *
                             static_cast<double>(n0 + static_cast<std::uint64_t>(n)) / 48000.0));
            }
            views[i] = essences[i];
        }
        n0 += frame_samples;

        auto unit = encoder.encode_frame(views, placement);
        if (!unit.has_value()) {
            fmt::println(stderr,
                         "error: cannot encode {} objects at {} kbps — the metadata and "
                         "the mantissas share one frame, so try a higher bit rate",
                         objects, bitrate);
            out_sink.abort();
            return kExitUsage;
        }
        if (!out_sink.push(std::move(unit->bytes))) {
            out_sink.abort();
            return kExitOutput;
        }
    }
    // Optional object signing: writes the keyed EMDF-protection tag so a
    // decoder that validates it accepts the JOC objects instead of falling
    // back to the 5.1 bed. Off unless the operator passes sign-objects with a
    // key; the algorithm is in-tree (clean-room), only the key is supplied.
    // A key failure discards everything, as it always has - nothing is on
    // disk in defer mode, so a plain return leaves exactly no file.
    const auto signed_count = apply_object_signing(out_sink.deferred(), meta);
    if (!signed_count.has_value()) {
        return kExitRuntime;
    }
    if (*signed_count > 0) {
        status_println(status_stream(),
                       "  signed {} frames' EMDF object container with the supplied key",
                       *signed_count);
    }
    if (!out_sink.close()) {
        return kExitOutput;
    }
    status_println(status_stream(), "wrote {} E-AC-3 access units to {}", frames, out_path);
    if (emit_objects) {
        status_println(status_stream(),
                       "  {} dynamic objects + the bed's LFE = {} objects, JOC over a 5.1 downmix",
                       objects, iclforge::oba::object_count(encoder.program()));
    } else {
        status_println(status_stream(),
                       "  bed51: 5.1 bed only, no object container — plays as 5.1 on a decoder "
                       "that rejects an unvalidated one ({} objects were panned into the bed)",
                       objects);
    }
    return kExitOk;
}

int run_atmos_path(std::string_view out_path, std::string_view paths_path, std::uint32_t seconds,
                   std::uint32_t bitrate, std::uint32_t objects_arg, const Options& meta) {
    auto contents = read_scene_file(paths_path);
    if (!contents.has_value()) {
        return kExitInput;
    }
    const auto described = contents->objects.size();
    const auto objects = objects_arg != 0 ? static_cast<std::size_t>(objects_arg) : described;
    if (objects < 1 || objects > 15) {
        fmt::println(stderr, "error: 1 to 15 objects (the bed's LFE is the 16th, "
                             "and TS 103 420 §8.3.2.2 caps the total at 16)");
        return kExitUsage;
    }
    if (described > objects) {
        fmt::println(stderr,
                     "error: {} has keyframes up to object index {}, more than the {} objects "
                     "requested",
                     paths_path, described - 1, objects);
        return kExitUsage;
    }

    // An object the file never mentions sits still at room centre under the
    // same inverse-root gain law 'atmos' and the GUI use, exactly as before.
    const auto scene = scene_of(paths_path, std::move(*contents), objects, [objects](std::size_t) {
        return iclforge::oba::ObjectPlacement{.position = {.x = 0.5, .y = 0.5, .z = 0.0},
                                         .gain = 0.7 / std::sqrt(static_cast<double>(objects)),
                                         .lfe_send = 0.0};
    });
    if (!scene.has_value()) {
        return kExitInput;
    }

    // One frame of input per encode_frame call - kSamplesPerFrame at the
    // default numblkscod, 256/512/768 under a short syncframe. Derived
    // once, next to the config that set it, so the feed loop below and
    // the encoder can never disagree about a frame's length.
    const std::size_t frame_samples =
        static_cast<std::size_t>(iclforge::ac3::eac3::blocks_per_syncframe(meta.atmos_numblkscod) *
                                 iclforge::ac3::kSamplesPerBlock);
    iclforge::ac3::oba::AtmosEncoder encoder{
        {.bitrate_kbps = bitrate, .dialnorm = meta.p.dialnorm, .num_bands_idx = 4,
         .fast_mdct = meta.fast_mdct,
         .joc_domain = meta.joc_domain,
         .numblkscod = meta.atmos_numblkscod},
        static_cast<int>(objects)};

    // Distinct tones purely for audibility, same as 'atmos'.
    std::vector<double> tone_hz(objects);
    for (std::size_t i = 0; i < objects; ++i) {
        tone_hz[i] = 220.0 * std::pow(2.0, static_cast<double>(i) * 0.45);
    }

    const std::uint64_t frames = (static_cast<std::uint64_t>(seconds) * 48000 + static_cast<std::uint64_t>(frame_samples) - 1) /
                                 static_cast<std::uint64_t>(frame_samples);
    std::vector<std::vector<float>> essences(objects,
                                             std::vector<float>(frame_samples));
    std::vector<std::span<const float>> views(objects);
    // Same output arrangement as 'atmos' above, keep_partial hard-off for
    // the same synthetic-and-regenerable reason.
    EncodedStreamSink out_sink;
    if (!out_sink.open(out_path, /*keep_partial=*/false, /*defer=*/meta.sign_objects)) {
        return kExitOutput;
    }

    // Reused every frame rather than reallocated: evaluate_into fills it in
    // place, which is the whole reason it exists alongside the vector form.
    std::vector<iclforge::oba::ObjectPlacement> placement(objects);
    std::uint64_t n0 = 0;
    for (std::uint64_t f = 0; f < frames; ++f) {
        const double t = static_cast<double>(n0 + frame_samples) / 48000.0;
        scene->evaluate_into(t, placement);
        for (std::size_t i = 0; i < objects; ++i) {
            for (std::size_t n = 0; n < frame_samples; ++n) {
                essences[i][static_cast<std::size_t>(n)] = static_cast<float>(
                    std::sin(2.0 * std::numbers::pi * tone_hz[i] *
                             static_cast<double>(n0 + static_cast<std::uint64_t>(n)) / 48000.0));
            }
            views[i] = essences[i];
        }
        n0 += frame_samples;

        auto unit = encoder.encode_frame(views, placement);
        if (!unit.has_value()) {
            fmt::println(stderr,
                         "error: cannot encode {} objects at {} kbps — the metadata and "
                         "the mantissas share one frame, so try a higher bit rate",
                         objects, bitrate);
            out_sink.abort();
            return kExitUsage;
        }
        if (!out_sink.push(std::move(unit->bytes))) {
            out_sink.abort();
            return kExitOutput;
        }
    }
    // Optional object signing, same as 'atmos' - see the comments at its
    // call site there, the key-failure plain return included.
    const auto signed_count = apply_object_signing(out_sink.deferred(), meta);
    if (!signed_count.has_value()) {
        return kExitRuntime;
    }
    if (*signed_count > 0) {
        status_println(status_stream(),
                       "  signed {} frames' EMDF object container with the supplied key",
                       *signed_count);
    }
    if (!out_sink.close()) {
        return kExitOutput;
    }
    status_println(status_stream(), "wrote {} E-AC-3 access units to {} ({} objects from {})",
                   frames, out_path, objects, paths_path);
    return kExitOk;
}

namespace {

// atmos-encode's default placement of `count` objects with no direction of their own (a src=/map=
// run has no source layout to take one from): an even fan around the room at ear height, each at
// the inverse-root gain 'atmos' and the GUI use, so that objects panned into the same five channels
// add to about unity.
std::vector<iclforge::oba::ObjectPlacement> fan_placements(std::size_t count) {
    std::vector<iclforge::oba::ObjectPlacement> placement(count);
    for (std::size_t i = 0; i < count; ++i) {
        const double azimuth = 360.0 * static_cast<double>(i) / static_cast<double>(count);
        const double radians = azimuth * std::numbers::pi / 180.0;
        placement[i] = {.position = {.x = 0.5 - 0.5 * std::sin(radians),
                                     .y = 0.5 - 0.5 * std::cos(radians),
                                     .z = 0.0},
                        .gain = 0.7 / std::sqrt(static_cast<double>(count)),
                        .lfe_send = 0.0};
    }
    return placement;
}

// The same for the first `count` channels of one file of `src_channels`: a channel that already
// has a direction keeps it, the rest fan out evenly.
std::vector<iclforge::oba::ObjectPlacement> layout_placements(std::size_t src_channels,
                                                         std::size_t count) {
    std::vector<iclforge::oba::ObjectPlacement> placement(count);
    const auto layout = iclforge::ac3::io::ac3_layout_for(src_channels);
    for (std::size_t i = 0; i < count; ++i) {
        double azimuth = 0.0;
        if (layout.has_value()) {
            // wav_index maps a coded channel to a WAV one; this needs the
            // inverse, so the channel is found rather than indexed.
            for (std::size_t k = 0; k < layout->wav_index.size(); ++k) {
                if (layout->wav_index[k] != i) {
                    continue;
                }
                azimuth = iclforge::ac3::analysis::channel_azimuth_deg(layout->acmod, layout->lfe,
                                                             static_cast<int>(k))
                              .value_or(0.0);
            }
        } else {
            azimuth = 360.0 * static_cast<double>(i) / static_cast<double>(count);
        }
        const double radians = azimuth * std::numbers::pi / 180.0;
        placement[i] = {.position = {.x = 0.5 - 0.5 * std::sin(radians),
                                     .y = 0.5 - 0.5 * std::cos(radians),
                                     .z = 0.0},
                        // Every object is panned into the same five channels,
                        // so their contributions add there. The same
                        // inverse-root law 'atmos' and the GUI use, so a file
                        // encoded either way comes out at the same level.
                        .gain = 0.7 / std::sqrt(static_cast<double>(count)),
                        .lfe_send = 0.0};
    }
    return placement;
}

}  // namespace

// atmos-encode with src=/map= (wide-layout record/live paths): several sources, and an explicit
// statement of which of their channels become which objects, instead of the
// single-file "every channel is an object, in file order" default.
//
// Deliberately a second function rather than a branch inside run_atmos_encode,
// for the same reason run_encode_multi is (see multi_source.hpp's own header):
// the two have genuinely different data shapes - one WavData with an optional
// streaming reader versus several whole files gathered per frame - and the
// small amount that does overlap costs far less duplicated than a shared
// abstraction would risk. No streaming path here: load_sources opens whole
// files, exactly as the multi-source encode path does.
int run_atmos_encode_multi(std::string_view in_path, std::string_view out_path,
                           std::uint32_t bitrate, const Options& meta,
                           std::string_view paths_path) {
    auto sources = load_sources(in_path, meta.sources, meta.offsets);
    if (!sources.has_value()) {
        return kExitInput;
    }
    const auto sr = wav_sample_rate(sources->sample_rate, "E-AC-3", true);
    if (!sr.has_value()) {
        return kExitInput;
    }
    std::size_t total_channels = 0;
    for (const auto& shape : sources->shapes) {
        total_channels += shape.channels;
    }

    // map= is what makes a multi-source object encode mean anything: with
    // several files there is no "file order" for channels to become objects
    // in. One source without map= keeps the classic behaviour (below), so
    // this is only reachable with src= present or map= given explicitly.
    plan::Assignment assignment;
    if (meta.map_spec.has_value()) {
        if (!plan::parse_assignment(*meta.map_spec, sources->shapes, assignment)) {
            fmt::println(stderr, "error: bad map= spec ({})", plan::kAssignmentSyntax);
            return kExitUsage;
        }
    } else {
        // src= without map=: every loaded channel becomes its own object, in
        // load order - the natural generalisation of what one file does, and
        // the only reading that does not silently drop somebody's second file.
        for (std::size_t s = 0; s < sources->shapes.size(); ++s) {
            for (std::size_t c = 0; c < sources->shapes[s].channels; ++c) {
                assignment.set(s, c, {.kind = plan::DestinationKind::kObject});
            }
        }
    }

    const auto slots = object_slots_from_assignment(assignment, sources->shapes);
    if (slots.empty()) {
        fmt::println(stderr,
                     "error: map= names no obj/objm destination, so this encode would carry no "
                     "objects at all - 'eac3-encode' is the command for a purely "
                     "channel-mapped programme");
        return kExitUsage;
    }
    const std::size_t count = slots.size();
    if (count > 15) {
        fmt::println(stderr,
                     "error: 1 to 15 objects (the bed's LFE is the 16th, and TS 103 420 "
                     "8.3.2.2 caps the total at 16); this map= resolves {}",
                     count);
        return kExitUsage;
    }

    const auto status = status_stream(out_path);
    int dialnorm = meta.p.dialnorm;
    if (meta.p.measure_dialnorm) {
        fmt::println(stderr,
                     "error: dialnorm=auto is not supported alongside src=/map= on "
                     "atmos-encode - object channels have no single fixed layout to measure "
                     "loudness against; pass dialnorm=<1..31> explicitly");
        return kExitUsage;
    }

    // One frame of input per encode_frame call - kSamplesPerFrame at the
    // default numblkscod, 256/512/768 under a short syncframe. Derived
    // once, next to the config that set it, so the feed loop below and
    // the encoder can never disagree about a frame's length.
    const std::size_t frame_samples =
        static_cast<std::size_t>(iclforge::ac3::eac3::blocks_per_syncframe(meta.atmos_numblkscod) *
                                 iclforge::ac3::kSamplesPerBlock);
    iclforge::ac3::oba::AtmosEncoder encoder{
        {.sample_rate = *sr, .bitrate_kbps = bitrate, .dialnorm = dialnorm, .num_bands_idx = 4,
         .fast_mdct = meta.fast_mdct, .numblkscod = meta.atmos_numblkscod},
        static_cast<int>(count)};

    // Objects that reach the bed by the same route are exactly the ones JOC
    // cannot pull apart again, so they are fanned out evenly around the room
    // rather than stacked at one point. A multi-source map= has no source
    // layout to take a direction from the way one file does, so this is the
    // even fan every time.
    const std::vector<iclforge::oba::ObjectPlacement> placement = fan_placements(count);

    // Authored motion, keyed by OBJECT index (the order map= produced them
    // in), not by channel: with several sources a channel index alone would
    // not identify anything.
    std::optional<iclforge::oba::ObjectScene> scene;
    if (!paths_path.empty()) {
        auto contents = read_scene_file(paths_path);
        if (!contents.has_value()) {
            return kExitInput;
        }
        scene = scene_of(paths_path, std::move(*contents), count,
                         [&placement](std::size_t i) { return placement[i]; });
        if (!scene.has_value()) {
            return kExitInput;
        }
    }

    status_println(status, "  {} sources, {} channels -> {} objects (map= order)",
                   sources->shapes.size(), total_channels, count);
    if (verbose_mode()) {
        for (std::size_t i = 0; i < count; ++i) {
            std::string taps;
            for (const auto& [flat, gain] : slots[i].taps) {
                taps += taps.empty() ? "" : " + ";
                taps += std::format("ch{}", flat);
                if (gain != 1.0) {
                    taps += std::format(" x{:.3f}", gain);
                }
            }
            status_println(status, "    object {}: {}", i, taps.empty() ? "silent" : taps);
        }
    }

    iclforge::ac3::analysis::LevelMeter meter{iclforge::ac3::Acmod::k3_2, true,
                                              sources->sample_rate};
    const std::size_t total = sources->total_frames;
    std::vector<std::vector<float>> gathered(total_channels,
                                             std::vector<float>(frame_samples));
    std::vector<std::vector<float>> block(count, std::vector<float>(frame_samples));
    std::vector<std::span<const float>> views(count);
    std::vector<std::span<const float>> metered(6);
    for (std::size_t i = 0; i < count; ++i) {
        views[i] = block[i];
    }
    EncodedStreamSink out_sink;
    if (!out_sink.open(out_path, meta.keep_partial, /*defer=*/meta.sign_objects)) {
        return kExitOutput;
    }
    Progress progress;
    progress.start("encoding", (total + frame_samples - 1) / frame_samples);
    for (std::size_t start = 0; start < total; start += frame_samples) {
        gather_frame(*sources, start, gathered);
        for (std::size_t i = 0; i < count; ++i) {
            for (std::size_t n = 0; n < frame_samples; ++n) {
                float sum = 0.0F;
                for (const auto& [flat, gain] : slots[i].taps) {
                    if (flat < gathered.size()) {
                        sum += gathered[flat][static_cast<std::size_t>(n)] *
                               static_cast<float>(gain);
                    }
                }
                block[i][static_cast<std::size_t>(n)] = sum;
            }
        }
        auto unit = scene ? encoder.encode_frame(
                                views, scene->evaluate(
                                           static_cast<double>(start + frame_samples) /
                                           static_cast<double>(sources->sample_rate)))
                          : encoder.encode_frame(views, placement);
        if (!unit.has_value()) {
            fmt::println(stderr,
                         "error: cannot encode {} objects at {} kbps - the metadata and the "
                         "mantissas share one frame, so try a higher bit rate",
                         count, bitrate);
            out_sink.abort();
            return kExitUsage;
        }
        for (std::size_t ch = 0; ch < 6; ++ch) {
            metered[ch] = std::span{encoder.bed()[ch]};
        }
        meter.process(metered);
        if (!out_sink.push(std::move(unit->bytes))) {
            out_sink.abort();
            return kExitOutput;
        }
        progress.tick(start / frame_samples + 1);
    }
    progress.finish();
    const auto signed_count = apply_object_signing(out_sink.deferred(), meta);
    if (!signed_count.has_value()) {
        return kExitRuntime;
    }
    if (*signed_count > 0) {
        status_println(status, "  signed {} frames' EMDF object container with the supplied key",
                       *signed_count);
    }
    if (!out_sink.close()) {
        return kExitOutput;
    }
    status_println(status, "encoded {} E-AC-3 access units ({} kbps, {} Hz) to {}",
                   out_sink.frames(), bitrate, sources->sample_rate, out_path);
    status_println(status,
                   "  {} objects + the bed's LFE = {} objects, JOC over a 5.1 downmix", count,
                   iclforge::oba::object_count(encoder.program()));
    print_channel_summary(meter, status);
    return kExitOk;
}

namespace {

// atmos-encode with codec=ac4 (planning/ac4.md, I5b): the source's channels as AC-4 objects, A-JOC
// coded by default or, with coding=direct, direct-coded, written as a raw stream or, for an
// .mp4/.m4a/.mov name, an MP4 file. Which channels are objects follows src=/map= as it does for
// E-AC-3 (object_slots_from_assignment, with a channel mapped to a speaker a dynamic object held
// there and one mapped to an LFE the LFE object - iclforge::apps::ac4_object_slots), and an
// authored scene file moves the dynamic objects, in that order; without one each keeps
// atmos-encode's default placement. The steps from there are apps/common/ac4_objects_core.cpp's and
// ac4_encode_core.cpp's, which forge-gui's AC-4 objects take too, so the line the GUI echoes writes
// the bytes the GUI does.
int run_atmos_encode_ac4(std::string_view in_path, std::string_view out_path, std::uint32_t bitrate,
                         std::uint32_t objects, const Options& meta, std::string_view paths_path) {
    if (meta.sign_objects) {
        fmt::println(stderr,
                     "error: sign-objects signs E-AC-3's EMDF object container; an AC-4 object "
                     "stream carries no such signature");
        return kExitUsage;
    }
    if (meta.p.measure_dialnorm) {
        fmt::println(stderr,
                     "error: dialnorm=auto measures a bed's loudness, which an AC-4 object stream "
                     "has none of; pass dialnorm=<1..31> explicitly");
        return kExitUsage;
    }
    const bool to_mp4 = iclforge::apps::ac4_output_names_mp4(out_path);
    if (meta.ac4enc.crc && to_mp4) {
        fmt::println(stderr,
                     "error: crc= is a raw stream's sync frames' CRC; an MP4 sample is the raw "
                     "frame alone, with no sync word and no CRC");
        return kExitUsage;
    }
    const bool routed = !meta.sources.empty() || meta.map_spec.has_value();
    if (routed && objects != 0) {
        fmt::println(stderr,
                     "error: [objects] counts the source channels to turn into objects, "
                     "which map= states instead - give one or the other");
        return kExitUsage;
    }

    // The sources, whole: src=/map= go through load_sources as everywhere, the single file the
    // classic way (so "-" reads stdin), with offset= on it either way.
    std::vector<iclforge::ac3::io::WavData> wavs;
    std::vector<plan::SourceShape> shapes;
    std::vector<std::size_t> offsets;
    if (routed) {
        auto loaded = load_sources(in_path, meta.sources, meta.offsets);
        if (!loaded.has_value()) {
            return kExitInput;
        }
        wavs = std::move(loaded->wavs);
        shapes = std::move(loaded->shapes);
        offsets = std::move(loaded->offset_samples);
    } else {
        auto wav = read_wav_arg(in_path);
        if (!wav.has_value()) {
            fmt::println(stderr, "error: {}: {}", in_path,
                         iclforge::ac3::io::describe(wav.error()));
            return kExitInput;
        }
        shapes.push_back({.channels = wav->channels.size(), .label = std::string{in_path}});
        offsets.push_back(offset_samples_for(meta.offsets, 0, wav->sample_rate));
        wavs.push_back(std::move(*wav));
    }
    const std::uint32_t sample_rate = wavs.front().sample_rate;

    // What each channel is. Without map= every channel is an object - the first `objects` of a
    // single file when a count is given.
    plan::Assignment assignment;
    if (meta.map_spec.has_value()) {
        if (!plan::parse_assignment(*meta.map_spec, shapes, assignment)) {
            fmt::println(stderr, "error: bad map= spec ({})", plan::kAssignmentSyntax);
            return kExitUsage;
        }
    } else {
        std::size_t rows = 0;
        for (const plan::SourceShape& shape : shapes) {
            rows += shape.channels;
        }
        if (!routed && objects != 0) {
            rows = std::min<std::size_t>(objects, rows);
        }
        std::size_t flat = 0;
        for (std::size_t s = 0; s < shapes.size(); ++s) {
            for (std::size_t c = 0; c < shapes[s].channels; ++c, ++flat) {
                if (flat < rows) {
                    assignment.set(s, c, {.kind = plan::DestinationKind::kObject});
                }
            }
        }
    }
    const std::vector<iclforge::apps::Ac4ObjectSlot> slots =
        iclforge::apps::ac4_object_slots(assignment, shapes);
    const iclforge::apps::Ac4ObjectsParams params{
        .sample_rate_hz = sample_rate,
        .bitrate_kbps = static_cast<int>(bitrate),
        .dialnorm_db = static_cast<double>(meta.p.dialnorm),
        .coding = meta.ac4_atmos_coding.value_or(iclforge::ac4::ObjectCoding::kAjoc)};
    if (const auto refused = iclforge::apps::ac4_objects_refusal(slots, params)) {
        fmt::println(stderr, "error: {}: {}", in_path, *refused);
        return kExitUsage;
    }
    const auto dynamic =
        static_cast<std::size_t>(std::ranges::count_if(slots, [](const auto& slot) {
            return slot.kind == iclforge::apps::Ac4ObjectSlot::Kind::kDynamic;
        }));

    // The dynamic objects' motion: the scene file's, and the default placement of any it does not
    // mention (or of all, without one).
    const std::vector<iclforge::oba::ObjectPlacement> placement =
        routed ? fan_placements(dynamic) : layout_placements(shapes.front().channels, dynamic);
    iclforge::oba::SceneContents contents;
    if (!paths_path.empty()) {
        auto read = read_scene_file(paths_path);
        if (!read.has_value()) {
            return kExitInput;
        }
        contents = std::move(*read);
    }
    const auto scene = scene_of(paths_path, std::move(contents), dynamic,
                                [&placement](std::size_t i) { return placement[i]; });
    if (!scene.has_value()) {
        return kExitInput;
    }

    std::vector<iclforge::apps::Ac4SourceView> views;
    views.reserve(wavs.size());
    for (std::size_t i = 0; i < wavs.size(); ++i) {
        views.push_back({.channels = wavs[i].channels, .offset_samples = offsets[i]});
    }
    const std::vector<std::vector<float>> flat = iclforge::apps::ac4_flat_planes(views);
    const auto encoded = iclforge::apps::encode_ac4_scene(params, slots, flat, *scene);
    if (!encoded.has_value()) {
        switch (encoded.error().kind) {
            case iclforge::apps::Ac4ObjectsError::Kind::kRefused:
                fmt::println(stderr, "error: the encoder refuses {} objects at {} kbps ({})",
                             slots.size(), bitrate, encoded.error().message);
                return kExitUsage;
            case iclforge::apps::Ac4ObjectsError::Kind::kEncode:
                fmt::println(stderr, "error: {}: {}", in_path, encoded.error().message);
                return kExitInput;
            case iclforge::apps::Ac4ObjectsError::Kind::kFlush:
                break;
        }
        fmt::println(stderr, "error: {}", encoded.error().message);
        return kExitInput;
    }
    const bool crc = meta.ac4enc.crc.value_or(true);
    const auto packaged = iclforge::apps::package_ac4(encoded->frames, encoded->toc, to_mp4, crc);
    if (!packaged.has_value()) {
        fmt::println(stderr, "error: {}", packaged.error().message);
        return packaged.error().usage ? kExitUsage : kExitOutput;
    }
    if (!write_frames(out_path, packaged->chunks)) {
        return kExitOutput;
    }
    const auto status = status_stream(out_path);
    status_println(status, "encoded {} AC-4 frames ({} kbps, {} Hz) from {} to {}",
                   encoded->frames.size(), bitrate, sample_rate, in_path, out_path);
    status_println(status, "  {} objects ({} dynamic), {}-coded, {}", slots.size(), dynamic,
                   params.coding == iclforge::ac4::ObjectCoding::kAjoc ? "A-JOC" : "direct",
                   to_mp4 ? fmt::format("MP4, codecs {}", packaged->rfc6381)
                          : std::string{crc ? "raw with CRC" : "raw without CRC"});
    status_println(status, "  the decoder's output lags the input by {} samples",
                   encoded->lag_samples);
    return kExitOk;
}

}  // namespace

int run_atmos_encode(std::string_view in_path, std::string_view out_path,
                     std::uint32_t bitrate, std::uint32_t objects,
                     const Options& meta, std::string_view paths_path) {
    // codec=ac4 takes the AC-4 objects path above, src=/map= and all.
    if (meta.take_codec == iclforge::ac3::plan::Codec::kAc4) {
        return run_atmos_encode_ac4(in_path, out_path, bitrate, objects, meta, paths_path);
    }
    // coding= and crc= are AC-4 objects' options; an E-AC-3 stream has neither, and a flag that
    // is dropped reads exactly like one that did not work.
    if (meta.ac4_atmos_coding.has_value() || meta.ac4enc.crc.has_value()) {
        fmt::println(stderr,
                     "error: coding= and crc= are the options of AC-4 objects: add codec=ac4");
        return kExitUsage;
    }
    // src=/map= route to the multi-source path above, which is what makes
    // obj/objm real destinations on this command (wide-layout record/live paths - they parsed
    // and did nothing here before). Without either, everything below is
    // byte-identical to what this command always did.
    if (!meta.sources.empty() || meta.map_spec.has_value()) {
        if (objects != 0) {
            fmt::println(stderr,
                         "error: [objects] counts the source channels to turn into objects, "
                         "which map= states instead - give one or the other");
            return kExitUsage;
        }
        return run_atmos_encode_multi(in_path, out_path, bitrate, meta, paths_path);
    }
    // The same streaming-vs-whole-file split as run_encode - see its
    // comment. This command has no dual-mono merge, so only stdin and
    // dialnorm=auto (whole-programme BS.1770) force the whole-file read.
    iclforge::ac3::io::WavStreamReader stream_in;
    const bool streaming = !is_stdio_path(in_path) && !meta.p.measure_dialnorm &&
                           stream_in.open(std::string{in_path}).has_value();
    std::expected<iclforge::ac3::io::WavData, iclforge::ac3::io::WavError> wav =
        std::unexpected(iclforge::ac3::io::WavError::kCannotOpen);
    if (!streaming) {
        wav = read_wav_arg(in_path);
        if (!wav.has_value()) {
            fmt::println(stderr, "error: {}: {}", in_path,
                         iclforge::ac3::io::describe(wav.error()));
            return kExitInput;        }
    }
    const std::uint32_t src_rate = streaming ? stream_in.sample_rate() : wav->sample_rate;
    const std::size_t src_channels =
        streaming ? stream_in.channels() : wav->channels.size();
    const auto sr = wav_sample_rate(src_rate, "E-AC-3", true);
    if (!sr.has_value()) {
        return kExitInput;
    }
    // One object per source channel unless told otherwise; more objects than
    // the file has channels would leave some carrying nothing.
    const auto count = objects == 0 ? src_channels
                                    : std::min<std::size_t>(objects, src_channels);
    if (count < 1 || count > 15) {
        fmt::println(stderr,
                     "error: 1 to 15 objects (the bed's LFE is the 16th, and TS 103 420 "
                     "§8.3.2.2 caps the total at 16); this file has {} channels",
                     src_channels);
        return kExitUsage;
    }

    // status_stream(out_path): stderr instead of stdout when out_path is "-"
    // - the E-AC-3 bytes this function writes below already own stdout in
    // that case, and no human-readable report (the dialnorm=auto measurement
    // just below included) may land in the middle of them.
    const auto status = status_stream(out_path);
    int dialnorm = meta.p.dialnorm;
    if (meta.p.measure_dialnorm) {
        const auto layout = iclforge::ac3::io::ac3_layout_for(src_channels);
        const auto measured = layout
                                  ? measured_dialnorm(*wav, *sr, layout->acmod, layout->lfe, status)
                                  : std::nullopt;
        if (!measured.has_value()) {
            fmt::println(stderr, "error: cannot measure loudness for this file; "
                                 "pass dialnorm=<1..31> explicitly");
            return kExitRuntime;
        }
        dialnorm = *measured;
    }

    // One frame of input per encode_frame call - kSamplesPerFrame at the
    // default numblkscod, 256/512/768 under a short syncframe. Derived
    // once, next to the config that set it, so the feed loop below and
    // the encoder can never disagree about a frame's length.
    const std::size_t frame_samples =
        static_cast<std::size_t>(iclforge::ac3::eac3::blocks_per_syncframe(meta.atmos_numblkscod) *
                                 iclforge::ac3::kSamplesPerBlock);
    iclforge::ac3::oba::AtmosEncoder encoder{
        {.sample_rate = *sr, .bitrate_kbps = bitrate, .dialnorm = dialnorm, .num_bands_idx = 4,
         .fast_mdct = meta.fast_mdct,
         .joc_domain = meta.joc_domain,
         .numblkscod = meta.atmos_numblkscod},
        static_cast<int>(count)};

    // Objects that reach the bed by the same route are exactly the ones JOC
    // cannot pull apart again, so the source's channels are spread across the
    // room rather than stacked at one point. A channel that already has a
    // direction keeps it; the rest fan out evenly.
    const std::vector<iclforge::oba::ObjectPlacement> placement =
        layout_placements(src_channels, count);

    // An authored scene file (same format/addressing as atmos-path, object
    // index == this WAV channel index) drives motion instead of the static
    // placement above; empty (the default) leaves that placement reused
    // unchanged every frame, exactly as before this argument existed - see
    // the per-frame loop below.
    std::optional<iclforge::oba::ObjectScene> scene;
    if (!paths_path.empty()) {
        auto contents = read_scene_file(paths_path);
        if (!contents.has_value()) {
            return kExitInput;
        }
        // Not mentioned in the file: keep exactly the placement this object
        // has today, just re-expressed as a (never-moving) automation point.
        scene = scene_of(paths_path, std::move(*contents), count,
                         [&placement](std::size_t i) { return placement[i]; });
        if (!scene.has_value()) {
            return kExitInput;
        }
    }

    iclforge::ac3::analysis::LevelMeter meter{iclforge::ac3::Acmod::k3_2, true, src_rate};
    const std::size_t total =
        streaming ? static_cast<std::size_t>(stream_in.frame_count()) : wav->frame_count();
    std::vector<std::vector<float>> block(count, std::vector<float>(frame_samples));
    std::vector<std::span<const float>> views(count);
    std::vector<std::span<const float>> metered(6);
    // Streamed out as encoded - except under sign-objects, where the frames
    // defer inside the sink because the signing pass below rewrites every
    // one of them after this loop.
    EncodedStreamSink out_sink;
    if (!out_sink.open(out_path, meta.keep_partial, /*defer=*/meta.sign_objects)) {
        return kExitOutput;
    }
    // Streaming reads every file channel (read_planar's contract), but only
    // the first `count` become objects - the extras land in one shared
    // discard buffer whose contents nothing reads.
    std::vector<float> stream_discard(streaming ? frame_samples : 0);
    std::vector<std::span<float>> stream_dst(streaming ? src_channels : 0);

    Progress progress;
    progress.start("encoding", (total + frame_samples - 1) / frame_samples);
    for (std::size_t start = 0; start < total; start += frame_samples) {
        const auto valid = std::min<std::size_t>(frame_samples, total - start);
        if (streaming) {
            for (std::size_t ch = 0; ch < src_channels; ++ch) {
                stream_dst[ch] = ch < count ? std::span{block[ch]}.first(valid)
                                            : std::span{stream_discard}.first(valid);
            }
            const auto got = stream_in.read_planar(stream_dst, valid);
            if (!got || *got != valid) {
                fmt::println(stderr, "error: {}: {}", in_path,
                             iclforge::ac3::io::describe(
                                 got ? iclforge::ac3::io::WavError::kTruncated : got.error()));
                out_sink.abort();
                return kExitInput;
            }
            for (std::size_t ch = 0; ch < count; ++ch) {
                // The tail frame zero-pads past the file's end, exactly as
                // the whole-file loop below writes 0.0f there.
                std::fill(block[ch].begin() + static_cast<std::ptrdiff_t>(valid),
                          block[ch].end(), 0.0f);
                views[ch] = block[ch];
            }
        } else {
            for (std::size_t ch = 0; ch < count; ++ch) {
                for (std::size_t i = 0; i < frame_samples; ++i) {
                    const std::size_t at = start + static_cast<std::size_t>(i);
                    block[ch][static_cast<std::size_t>(i)] =
                        at < total ? wav->channels[ch][at] : 0.0f;
                }
                views[ch] = block[ch];
            }
        }
        // With paths_path, the object placement moves - evaluated at the
        // frame's END time, the same convention run_atmos_path and the GUI's
        // encodeObjects use. Without it, every frame reuses the one static
        // placement computed above, byte-identical to before this argument
        // existed.
        auto unit = scene ? encoder.encode_frame(
                                views, scene->evaluate(
                                           static_cast<double>(start + frame_samples) /
                                           static_cast<double>(src_rate)))
                          : encoder.encode_frame(views, placement);
        if (!unit.has_value()) {
            fmt::println(stderr,
                         "error: cannot encode {} objects at {} kbps — the metadata and the "
                         "mantissas share one frame, so try a higher bit rate",
                         count, bitrate);
            out_sink.abort();
            return kExitUsage;
        }
        // The bed exists only once the frame is encoded, so it is metered
        // afterwards - and it is the bed, not the source, that a legacy
        // decoder plays.
        for (std::size_t ch = 0; ch < metered.size(); ++ch) {
            metered[ch] = std::span{encoder.bed()[ch]}.first(valid);
        }
        meter.process(metered);
        if (!out_sink.push(std::move(unit->bytes))) {
            out_sink.abort();
            return kExitOutput;
        }
        progress.tick(start / frame_samples + 1);
    }
    progress.finish();
    // Optional object signing, same as 'atmos' - see the comments at its
    // call site there, the key-failure plain return included. Goes through
    // status_stream() like the report below: with out_path == "-" the
    // E-AC-3 bytes about to be written own stdout.
    const auto signed_count = apply_object_signing(out_sink.deferred(), meta);
    if (!signed_count.has_value()) {
        return kExitRuntime;
    }
    if (*signed_count > 0) {
        status_println(status_stream(out_path),
                       "  signed {} frames' EMDF object container with the supplied key",
                       *signed_count);
    }
    if (!out_sink.close()) {
        return kExitOutput;
    }
    status_println(status, "encoded {} E-AC-3 access units ({} kbps, {} Hz) to {}",
                   out_sink.frames(), bitrate, src_rate, out_path);
    status_println(status,
                   "  {} objects from {} source channels + the bed's LFE = {} objects, "
                   "JOC over a 5.1 downmix",
                   count, src_channels, iclforge::oba::object_count(encoder.program()));
    print_channel_summary(meter, status);
    return kExitOk;
}

int run_atmos_adm(std::string_view in_path, std::string_view out_path, std::uint32_t bitrate,
                  const Options& meta, std::string_view programme_id) {
    // No fixed source layout to measure a pre-encode loudness figure against the way
    // atmos-encode's WAV input has (iclforge::ac3::io::ac3_layout_for) - an ADM document's channels
    // are an arbitrary mix of bed speaker feeds and dynamic objects, not one of the handful of
    // layouts that function maps. Refusing clearly beats silently keeping the fixed default
    // dialnorm: "a silently ignored metadata flag looks exactly like metadata that did not work"
    // (see parse_options's own comment above).
    if (meta.p.measure_dialnorm) {
        fmt::println(stderr,
                     "error: dialnorm=auto is not supported by atmos-adm - an ADM document's bed/"
                     "object channels have no single fixed layout to measure loudness against the "
                     "way atmos-encode's WAV input does; pass dialnorm=<1..31> explicitly");
        return kExitUsage;
    }

    auto source = forge_cli::load_adm_atmos_source(in_path, programme_id);
    if (!source.has_value()) {
        fmt::println(stderr, "error: {}: {}", in_path, source.error());
        return kExitInput;
    }

    // codec=ac4 (planning/ac4.md, I5): AC-4 as this command's output codec, the ADM master's
    // bed/object channels going into E9's object encoder rather than AtmosEncoder below.
    if (meta.take_codec == iclforge::ac3::plan::Codec::kAc4) {
        return run_atmos_objects_to_ac4("ADM BWF", source->sample_rate, source->is_bed,
                                        source->paths, source->pcm, in_path, out_path, bitrate,
                                        meta);
    }

    const auto sr = wav_sample_rate(source->sample_rate, "E-AC-3", true);
    if (!sr.has_value()) {
        return kExitInput;
    }

    const auto count = source->channel_count();
    if (count < 1 || count > 15) {
        fmt::println(stderr,
                     "error: 1 to 15 bed/object channels (the bed's LFE is the 16th, and TS 103 "
                     "420 §8.3.2.2 caps the total at 16); {} resolved {} channel(s)",
                     in_path, count);
        return kExitInput;
    }

    // One frame of input per encode_frame call - kSamplesPerFrame at the
    // default numblkscod, 256/512/768 under a short syncframe. Derived
    // once, next to the config that set it, so the feed loop below and
    // the encoder can never disagree about a frame's length.
    const std::size_t frame_samples =
        static_cast<std::size_t>(iclforge::ac3::eac3::blocks_per_syncframe(meta.atmos_numblkscod) *
                                 iclforge::ac3::kSamplesPerBlock);
    iclforge::ac3::oba::AtmosEncoder encoder{
        {.sample_rate = *sr, .bitrate_kbps = bitrate, .dialnorm = meta.p.dialnorm,
         .num_bands_idx = 4, .fast_mdct = meta.fast_mdct,
         .joc_domain = meta.joc_domain,
         .numblkscod = meta.atmos_numblkscod},
        static_cast<int>(count)};

    // Metered the same way run_atmos_encode meters its own bed: 3/2 + LFE is AtmosEncoder's own
    // fixed bed layout regardless of how many dynamic objects/bed feeds fed it.
    iclforge::ac3::analysis::LevelMeter meter{iclforge::ac3::Acmod::k3_2, true,
                                              source->sample_rate};
    const std::size_t total = source->pcm.empty() ? 0 : source->pcm.front().size();
    std::vector<std::vector<float>> block(count, std::vector<float>(frame_samples));
    std::vector<std::span<const float>> views(count);
    std::vector<std::span<const float>> metered(6);
    // Streamed out as encoded - no sign-objects on this command, so no
    // defer case either.
    EncodedStreamSink out_sink;
    if (!out_sink.open(out_path, meta.keep_partial)) {
        return kExitOutput;
    }

    Progress progress;
    progress.start("encoding", (total + frame_samples - 1) / frame_samples);
    for (std::size_t start = 0; start < total; start += frame_samples) {
        const auto valid = std::min<std::size_t>(frame_samples, total - start);
        for (std::size_t ch = 0; ch < count; ++ch) {
            for (std::size_t i = 0; i < frame_samples; ++i) {
                const std::size_t at = start + static_cast<std::size_t>(i);
                block[ch][static_cast<std::size_t>(i)] =
                    at < source->pcm[ch].size() ? source->pcm[ch][at] : 0.0f;
            }
            views[ch] = block[ch];
        }
        // Evaluated at the frame's END time, the same convention run_atmos_path/run_atmos_encode
        // use.
        const auto placement = iclforge::oba::evaluate_placements(
            source->paths, static_cast<double>(start + frame_samples) /
                                static_cast<double>(source->sample_rate));
        auto unit = encoder.encode_frame(views, placement);
        if (!unit.has_value()) {
            fmt::println(stderr,
                         "error: cannot encode {} channels at {} kbps — the metadata and the "
                         "mantissas share one frame, so try a higher bit rate",
                         count, bitrate);
            out_sink.abort();
            return kExitUsage;
        }
        // The bed exists only once the frame is encoded, so it is metered afterwards - and it is
        // the bed, not the source, that a legacy decoder plays.
        for (std::size_t ch = 0; ch < metered.size(); ++ch) {
            metered[ch] = std::span{encoder.bed()[ch]}.first(valid);
        }
        meter.process(metered);
        if (!out_sink.push(std::move(unit->bytes))) {
            out_sink.abort();
            return kExitOutput;
        }
        progress.tick(start / frame_samples + 1);
    }
    progress.finish();
    if (!out_sink.close()) {
        return kExitOutput;
    }

    std::size_t bed_count = 0;
    for (const bool is_bed : source->is_bed) {
        bed_count += is_bed ? 1 : 0;
    }
    // See run_encode's identical status_stream() comment: out_path == "-" means the E-AC-3 bytes
    // just written own stdout, so this report goes to stderr instead.
    const auto status = status_stream(out_path);
    status_println(status, "encoded {} E-AC-3 access units ({} kbps, {} Hz) from {} to {}",
                   out_sink.frames(), bitrate, source->sample_rate, in_path, out_path);
    status_println(status,
                   "  {} bed speaker feed(s) + {} dynamic object(s) + the bed's LFE = {} objects, "
                   "JOC over a 5.1 downmix",
                   bed_count, count - bed_count, iclforge::oba::object_count(encoder.program()));
    print_channel_summary(meter, status);
    return kExitOk;
}

int run_atmos_iab(std::string_view in_path, std::string_view out_path, std::uint32_t bitrate,
                  const Options& meta) {
    // Same refusal, same reason as run_atmos_adm's own: an IAB file's Bed/Object channels are an
    // arbitrary mix, not one of iclforge::ac3::io::ac3_layout_for's fixed layouts - see that
    // function's own comment above.
    if (meta.p.measure_dialnorm) {
        fmt::println(stderr,
                     "error: dialnorm=auto is not supported by atmos-iab - an IAB file's Bed/"
                     "Object channels have no single fixed layout to measure loudness against the "
                     "way atmos-encode's WAV input does; pass dialnorm=<1..31> explicitly");
        return kExitUsage;
    }

    auto source = forge_cli::load_iab_atmos_source(in_path);
    if (!source.has_value()) {
        fmt::println(stderr, "error: {}: {}", in_path, source.error());
        return kExitInput;
    }

    // codec=ac4 (planning/ac4.md, I5): AC-4 as this command's output codec, the IAB file's
    // Bed/Object channels going into E9's object encoder rather than AtmosEncoder below.
    if (meta.take_codec == iclforge::ac3::plan::Codec::kAc4) {
        return run_atmos_objects_to_ac4("IAB", source->sample_rate, source->is_bed, source->paths,
                                        source->pcm, in_path, out_path, bitrate, meta);
    }

    const auto sr = wav_sample_rate(source->sample_rate, "E-AC-3", true);
    if (!sr.has_value()) {
        return kExitInput;
    }

    const auto count = source->channel_count();
    if (count < 1 || count > 15) {
        fmt::println(stderr,
                     "error: 1 to 15 Bed/Object channels (the bed's LFE is the 16th, and TS 103 "
                     "420 §8.3.2.2 caps the total at 16); {} resolved {} channel(s)",
                     in_path, count);
        return kExitInput;
    }

    // One frame of input per encode_frame call - kSamplesPerFrame at the
    // default numblkscod, 256/512/768 under a short syncframe. Derived
    // once, next to the config that set it, so the feed loop below and
    // the encoder can never disagree about a frame's length.
    const std::size_t frame_samples =
        static_cast<std::size_t>(iclforge::ac3::eac3::blocks_per_syncframe(meta.atmos_numblkscod) *
                                 iclforge::ac3::kSamplesPerBlock);
    iclforge::ac3::oba::AtmosEncoder encoder{
        {.sample_rate = *sr, .bitrate_kbps = bitrate, .dialnorm = meta.p.dialnorm,
         .num_bands_idx = 4, .fast_mdct = meta.fast_mdct,
         .joc_domain = meta.joc_domain,
         .numblkscod = meta.atmos_numblkscod},
        static_cast<int>(count)};

    // Metered the same way run_atmos_adm meters its own bed.
    iclforge::ac3::analysis::LevelMeter meter{iclforge::ac3::Acmod::k3_2, true,
                                              source->sample_rate};
    const std::size_t total = source->pcm.empty() ? 0 : source->pcm.front().size();
    std::vector<std::vector<float>> block(count, std::vector<float>(frame_samples));
    std::vector<std::span<const float>> views(count);
    std::vector<std::span<const float>> metered(6);
    EncodedStreamSink out_sink;
    if (!out_sink.open(out_path, meta.keep_partial)) {
        return kExitOutput;
    }

    Progress progress;
    progress.start("encoding", (total + frame_samples - 1) / frame_samples);
    for (std::size_t start = 0; start < total; start += frame_samples) {
        const auto valid = std::min<std::size_t>(frame_samples, total - start);
        for (std::size_t ch = 0; ch < count; ++ch) {
            for (std::size_t i = 0; i < frame_samples; ++i) {
                const std::size_t at = start + static_cast<std::size_t>(i);
                block[ch][static_cast<std::size_t>(i)] =
                    at < source->pcm[ch].size() ? source->pcm[ch][at] : 0.0f;
            }
            views[ch] = block[ch];
        }
        // Evaluated at the frame's END time, the same convention every other Atmos-encode command
        // uses.
        const auto placement = iclforge::oba::evaluate_placements(
            source->paths, static_cast<double>(start + frame_samples) /
                                static_cast<double>(source->sample_rate));
        auto unit = encoder.encode_frame(views, placement);
        if (!unit.has_value()) {
            fmt::println(stderr,
                         "error: cannot encode {} channels at {} kbps — the metadata and the "
                         "mantissas share one frame, so try a higher bit rate",
                         count, bitrate);
            out_sink.abort();
            return kExitUsage;
        }
        for (std::size_t ch = 0; ch < metered.size(); ++ch) {
            metered[ch] = std::span{encoder.bed()[ch]}.first(valid);
        }
        meter.process(metered);
        if (!out_sink.push(std::move(unit->bytes))) {
            out_sink.abort();
            return kExitOutput;
        }
        progress.tick(start / frame_samples + 1);
    }
    progress.finish();
    if (!out_sink.close()) {
        return kExitOutput;
    }

    std::size_t bed_count = 0;
    for (const bool is_bed : source->is_bed) {
        bed_count += is_bed ? 1 : 0;
    }
    const auto status = status_stream(out_path);
    status_println(status, "encoded {} E-AC-3 access units ({} kbps, {} Hz) from {} to {}",
                   out_sink.frames(), bitrate, source->sample_rate, in_path, out_path);
    status_println(status,
                   "  {} bed channel(s) + {} dynamic object(s) + the bed's LFE = {} objects, "
                   "JOC over a 5.1 downmix",
                   bed_count, count - bed_count, iclforge::oba::object_count(encoder.program()));
    print_channel_summary(meter, status);
    return kExitOk;
}

int run_atmos_cbi(std::string_view in_path, std::string_view out_path, std::uint32_t bitrate,
                  std::string_view layout_arg, const Options& meta) {
    if (!meta.sources.empty() || meta.map_spec.has_value()) {
        fmt::println(stderr,
                     "error: src=/map= are not supported by atmos-cbi - a CBI bed's channel order "
                     "IS its layout (see layout=), with no per-channel destination to state");
        return kExitUsage;
    }
    // Same refusal, same reason as run_atmos_adm's/run_atmos_iab's own: a bed
    // this wide has no single fixed layout iclforge::ac3::io::ac3_layout_for maps, so
    // there is nothing for dialnorm=auto to measure against - see that
    // function's own comment above.
    if (meta.p.measure_dialnorm) {
        fmt::println(stderr,
                     "error: dialnorm=auto is not supported by atmos-cbi - a channel-based-"
                     "immersive bed has no single fixed layout to measure loudness against the "
                     "way atmos-encode's plain WAV input does; pass dialnorm=<1..31> explicitly");
        return kExitUsage;
    }

    // The same streaming-vs-whole-file split as run_atmos_encode - see its
    // comment. dialnorm=auto is already refused above, so only stdin forces
    // the whole-file read here.
    iclforge::ac3::io::WavStreamReader stream_in;
    const bool streaming = !is_stdio_path(in_path) && stream_in.open(std::string{in_path}).has_value();
    std::expected<iclforge::ac3::io::WavData, iclforge::ac3::io::WavError> wav =
        std::unexpected(iclforge::ac3::io::WavError::kCannotOpen);
    if (!streaming) {
        wav = read_wav_arg(in_path);
        if (!wav.has_value()) {
            fmt::println(stderr, "error: {}: {}", in_path,
                         iclforge::ac3::io::describe(wav.error()));
            return kExitInput;
        }
    }
    const std::uint32_t src_rate = streaming ? stream_in.sample_rate() : wav->sample_rate;
    const std::size_t src_channels = streaming ? stream_in.channels() : wav->channels.size();
    const auto sr = wav_sample_rate(src_rate, "E-AC-3", true);
    if (!sr.has_value()) {
        return kExitInput;
    }

    std::optional<std::uint16_t> bed_flags;
    if (!layout_arg.empty()) {
        bed_flags = resolve_cbi_layout(layout_arg);
        if (!bed_flags.has_value()) {
            fmt::println(stderr, "error: layout must be one of 5.1.4, 7.1.4, 9.1.6 (got '{}')",
                         layout_arg);
            return kExitUsage;
        }
        const auto expected =
            static_cast<std::size_t>(iclforge::oba::bed::channel_count(*bed_flags));
        if (expected != src_channels) {
            fmt::println(stderr, "error: {} is a {}-channel bed, but {} has {} channel(s)",
                         layout_arg, expected, in_path, src_channels);
            return kExitUsage;
        }
    } else {
        bed_flags = cbi_layout_for_channel_count(src_channels);
        if (!bed_flags.has_value()) {
            fmt::println(stderr,
                         "error: {} has {} channel(s), which is none of the 10 (5.1.4), 12 "
                         "(7.1.4) or 16 (9.1.6) this command recognizes - pass layout= explicitly",
                         in_path, src_channels);
            return kExitUsage;
        }
    }

    // One frame of input per encode_bed_frame call - kSamplesPerFrame at the
    // default numblkscod, 256/512/768 under a short syncframe. Derived once,
    // next to the config that set it, the same convention every other Atmos-
    // encode command here uses.
    const std::size_t frame_samples =
        static_cast<std::size_t>(iclforge::ac3::eac3::blocks_per_syncframe(meta.atmos_numblkscod) *
                                 iclforge::ac3::kSamplesPerBlock);
    iclforge::ac3::oba::AtmosEncoder encoder{
        {.sample_rate = *sr, .bitrate_kbps = bitrate, .dialnorm = meta.p.dialnorm,
         .num_bands_idx = 4, .fast_mdct = meta.fast_mdct, .joc_domain = meta.joc_domain,
         .numblkscod = meta.atmos_numblkscod},
        iclforge::ac3::oba::BedProgram{.bed = *bed_flags}};

    iclforge::ac3::analysis::LevelMeter meter{iclforge::ac3::Acmod::k3_2, true, src_rate};
    const std::size_t total =
        streaming ? static_cast<std::size_t>(stream_in.frame_count()) : wav->frame_count();
    std::vector<std::vector<float>> block(src_channels, std::vector<float>(frame_samples));
    std::vector<std::span<const float>> views(src_channels);
    std::vector<std::span<const float>> metered(6);
    // Streamed out as encoded - except under sign-objects, where the frames
    // defer inside the sink because the signing pass below rewrites every one
    // of them after this loop, same as run_atmos_encode.
    EncodedStreamSink out_sink;
    if (!out_sink.open(out_path, meta.keep_partial, /*defer=*/meta.sign_objects)) {
        return kExitOutput;
    }
    // Streaming reads every file channel (read_planar's contract) - unlike
    // run_atmos_encode, every one of them is a bed channel here, so there is
    // no discard buffer for channels beyond some smaller object count.
    std::vector<std::span<float>> stream_dst(streaming ? src_channels : 0);

    Progress progress;
    progress.start("encoding", (total + frame_samples - 1) / frame_samples);
    for (std::size_t start = 0; start < total; start += frame_samples) {
        const auto valid = std::min<std::size_t>(frame_samples, total - start);
        if (streaming) {
            for (std::size_t ch = 0; ch < src_channels; ++ch) {
                stream_dst[ch] = std::span{block[ch]}.first(valid);
            }
            const auto got = stream_in.read_planar(stream_dst, valid);
            if (!got || *got != valid) {
                fmt::println(stderr, "error: {}: {}", in_path,
                             iclforge::ac3::io::describe(
                                 got ? iclforge::ac3::io::WavError::kTruncated : got.error()));
                out_sink.abort();
                return kExitInput;
            }
            for (std::size_t ch = 0; ch < src_channels; ++ch) {
                // The tail frame zero-pads past the file's end, exactly as
                // the whole-file loop below writes 0.0f there.
                std::fill(block[ch].begin() + static_cast<std::ptrdiff_t>(valid), block[ch].end(),
                          0.0f);
                views[ch] = block[ch];
            }
        } else {
            for (std::size_t ch = 0; ch < src_channels; ++ch) {
                for (std::size_t i = 0; i < frame_samples; ++i) {
                    const std::size_t at = start + i;
                    block[ch][i] = at < total ? wav->channels[ch][at] : 0.0f;
                }
                views[ch] = block[ch];
            }
        }

        auto unit = encoder.encode_bed_frame(views);
        if (!unit.has_value()) {
            fmt::println(stderr,
                         "error: cannot encode a {}-channel bed at {} kbps - the metadata and the "
                         "mantissas share one frame, so try a higher bit rate",
                         src_channels, bitrate);
            out_sink.abort();
            return kExitUsage;
        }
        // The bed exists only once the frame is encoded, so it is metered
        // afterwards - and it is the bed, not the source, that a legacy
        // decoder plays.
        for (std::size_t ch = 0; ch < metered.size(); ++ch) {
            metered[ch] = std::span{encoder.bed()[ch]}.first(valid);
        }
        meter.process(metered);
        if (!out_sink.push(std::move(unit->bytes))) {
            out_sink.abort();
            return kExitOutput;
        }
        progress.tick(start / frame_samples + 1);
    }
    progress.finish();
    // Optional object signing, same as atmos-encode - see the comments at its
    // call site there, the key-failure plain return included.
    const auto signed_count = apply_object_signing(out_sink.deferred(), meta);
    if (!signed_count.has_value()) {
        return kExitRuntime;
    }
    if (*signed_count > 0) {
        status_println(status_stream(out_path),
                       "  signed {} frames' EMDF object container with the supplied key",
                       *signed_count);
    }
    if (!out_sink.close()) {
        return kExitOutput;
    }
    const auto status = status_stream(out_path);
    status_println(status, "encoded {} E-AC-3 access units ({} kbps, {} Hz) from {} to {}",
                   out_sink.frames(), bitrate, src_rate, in_path, out_path);
    status_println(status,
                   "  {}-channel channel-based-immersive bed, 0 dynamic objects -> {} objects "
                   "total, {} of them JOC-reconstructed from a 5.1 downmix",
                   src_channels, iclforge::oba::object_count(encoder.program()),
                   iclforge::oba::joc_object_count(encoder.program()));
    print_channel_summary(meter, status);
    return kExitOk;
}

int run_strip_objects(std::string_view in_path, std::string_view out_path,
                      const forge_cli::Options& meta) {
    const auto raw = read_all(in_path);
    if (raw.empty()) {
        fmt::println(stderr, "error: cannot open {}", in_path);
        return kExitInput;
    }
    const auto stripped = iclforge::ac3::io::strip_objects(raw);
    if (!stripped.has_value()) {
        fmt::println(stderr, "error: {}", iclforge::ac3::io::describe(stripped.error()));
        return kExitInput;
    }
    // Re-scan before writing: it costs one cheap walk and it is the check
    // that matters here - a rewrite that re-derives frmsiz and re-stamps crc2
    // either still frames as an elementary stream or the whole exercise
    // failed, and finding that out from the file afterwards is worse.
    const auto rescanned = iclforge::ac3::io::scan(stripped->bytes);
    if (!rescanned.has_value()) {
        fmt::println(stderr, "error: the stripped stream no longer scans: {}",
                     iclforge::ac3::io::describe(rescanned.error()));
        return kExitInternal;
    }
    if (rescanned->oba_complexity_index.has_value()) {
        fmt::println(stderr,
                     "error: the stripped stream still declares an object layer (complexity {})",
                     *rescanned->oba_complexity_index);
        return kExitInternal;
    }

    EncodedStreamSink out_sink;
    if (!out_sink.open(out_path, meta.keep_partial)) {
        return kExitOutput;
    }
    for (const auto& unit : rescanned->access_units) {
        if (!out_sink.push(unit)) {
            out_sink.abort();
            return kExitOutput;
        }
    }
    if (!out_sink.close()) {
        return kExitOutput;
    }

    // See run_encode's identical status_stream() comment: out_path == "-" means the E-AC-3 bytes
    // just written own stdout, so this report goes to stderr instead.
    const auto status = status_stream(out_path);
    status_println(status, "stripped {} of {} frame(s) in {} -> {} ({} bytes removed, {} left)",
                   stripped->frames_stripped, stripped->frames_total, in_path, out_path,
                   stripped->bytes_removed, stripped->bytes.size());
    status_println(status, "  {} at {} Hz, no object metadata remains",
                   iclforge::ac3::analysis::layout_name(rescanned->acmod, rescanned->lfe),
                   iclforge::ac3::sample_rate_hz(rescanned->sample_rate));
    return kExitOk;
}

}  // namespace forge_cli::commands
