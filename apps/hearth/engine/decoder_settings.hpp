#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "ac3/decoder/decoder.hpp"
#include "ac3/decoder/output.hpp"
#include "ac3/render/layout.hpp"
#include "ac3/render/serving.hpp"

// The decoder configuration model (planning/hearth-reference-player.md,
// "Decoder configuration"): what the app's decoder controls hold, and the
// library configuration they become. One place, so the settings page, the
// engine and the tests agree on what each control means.
//
// The controls are the plan's table, row for row. The operating mode chooses
// one of §7.7's two canonical modes or the custom switches under it; the
// stereo fold is what a two-speaker layout gets (a one-speaker layout folds to
// mono, a wider one is rendered); the mix levels replace the stream's in any
// fold. Dual mono is the one control no library setting holds - the output
// stage leaves the choice of programme to its caller - so StreamDecoder
// applies it after decoding.

namespace ac3::hearth {

// Which of dual mono's (acmod 1+1) two unrelated programmes is heard.
enum class DualMonoChoice : std::uint8_t {
    // Channel 1 on the left and channel 2 on the right.
    kBoth,
    // Channel 1 on both.
    kFirst,
    // Channel 2 on both.
    kSecond,
};

struct DecoderSettings {
    // §7.7's operating mode. kLine and kRf each fix the dynamic range
    // handling and normalise dialogue; kCustom uses the four switches below.
    OperatingMode mode = OperatingMode::kLine;
    // kCustom only: §7.7.1's partial compression, as the share of each dynrng
    // cut and boost applied, 0 to 1.
    double drc_cut = 1.0;
    double drc_boost = 1.0;
    // kCustom only: §7.7.2's compr in place of dynrng where a frame has one.
    bool heavy_compression = false;
    // kCustom only: §5.4.2.8's normalisation onto -31 dBFS.
    bool normalise_dialogue = true;
    // The fold for a two-speaker layout: kLtRt, or kLoRo for anything else.
    DownmixTarget stereo_fold = DownmixTarget::kLoRo;
    bool ltrt_phase_shift = true;
    // Whether the LFE joins a fold (where the stream allows it).
    bool mix_lfe = false;
    // Levels to fold with in place of the stream's.
    MixLevelOverride mix_levels{};
    DualMonoChoice dual_mono = DualMonoChoice::kBoth;
    // The programme of a multi-programme E-AC-3 stream to play, by its
    // independent substream id; unset plays the first. A session applies it
    // when an item opens, by choosing that programme's units, so it is not
    // part of the decoder configuration.
    std::optional<int> programme = std::nullopt;
    render::ObjectsPolicy objects = render::ObjectsPolicy::kAuto;
    // §7.10. A player keeps a stream continuous through a damaged frame.
    ConcealmentPolicy concealment = ConcealmentPolicy::kRepeatFade;

    friend bool operator==(const DecoderSettings&, const DecoderSettings&) = default;
};

// What `settings` make of the decoders that serve `layout`: which fold the
// decoder does and whether it reconstructs objects, and the configuration to
// build them with.
struct DecoderSetup {
    render::Serving serving{};
    DecoderConfig config{};
};

[[nodiscard]] DecoderSetup decoder_setup(const DecoderSettings& settings,
                                         const render::OutputLayout& layout);

// Every control's value on one line, for the diagnostics file: "line mode,
// stereo fold Lo/Ro, no LFE in folds, the stream's mix levels, ...".
[[nodiscard]] std::string describe(const DecoderSettings& settings);

}  // namespace ac3::hearth
