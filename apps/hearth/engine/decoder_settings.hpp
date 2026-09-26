#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "ac3/decoder/decoder.hpp"
#include "ac3/decoder/output.hpp"
#include "ac3/render/layout.hpp"
#include "ac3/render/serving.hpp"
#include "ac4dec/decoder.hpp"

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
//
// AC-4 (planning/ac4.md, I2, and its "One control for both formats") shares
// the stereo fold, the LFE in a fold, the concealment and the layout with
// AC-3 and E-AC-3, and has controls of its own in Ac4Settings: the
// presentation, its DRC decoder mode and output level, which decision 12
// keeps apart from E-AC-3's operating mode, dialogue enhancement, and the
// dialogue and associated audio levels.

namespace ac3::hearth {

// AC-4's own controls, each a field of ac4::DecoderConfig (ac4dec/decoder.hpp)
// once decoder_setup() has made one of them.
struct Ac4Settings {
    // The presentation to play: the one that carries this presentation_id,
    // else the one at this position of the table of contents, else the
    // decoder's choice by the preferences below (ETSI TS 103 190-2 clause
    // 4.8.2). A choice the stream cannot meet falls to the next.
    std::optional<int> presentation_id = std::nullopt;
    std::optional<int> presentation_index = std::nullopt;
    // The listener's language, a BCP 47 tag: a presentation in it plays
    // first. The window sets it from its own language.
    std::string language{};
    // Audio description. On, a presentation that carries it plays first and
    // its associated audio is mixed in at associated_db; off, a presentation
    // without associated audio plays first, and the associated audio of one
    // chosen by hand is not heard.
    bool audio_description = false;
    // g_assoc (ETSI TS 103 190-1 clause 6.2.16.2), 0 dB or less.
    double associated_db = 0.0;
    // g_dialog (6.2.16.1): a presentation's dialogue against its music and
    // effects, up to the maximum the stream allows.
    double dialogue_db = 0.0;
    // G_DE (5.7.8), 0 to 12 dB, up to the stream's own cap.
    double dialogue_enhancement_db = 0.0;
    // Dialogue normalisation to the output level Lout (5.7.9.3.3), which cuts
    // or boosts; off, the stream plays at its coded level and the DRC
    // compresses nothing.
    bool normalise = true;
    double output_level_dbfs = -31.0;
    // Table 161's DRC decoder mode. kDefault takes the mode the output level
    // falls in; kPortableHeadphones also prefers a presentation made for
    // headphones (b_pre_virtualized).
    ac4::DrcMode drc = ac4::DrcMode::kDefault;
    // A stereo fold by the stream's preferred_dmx_method (Table 150), Lo/Ro
    // where it names none, rather than by DecoderSettings::stereo_fold.
    bool preferred_downmix = false;

    friend bool operator==(const Ac4Settings&, const Ac4Settings&) = default;
};

// The output levels Table 161's modes cover, which the page's control spans.
inline constexpr double kAc4MinOutputLevelDbfs = -31.0;
inline constexpr double kAc4MaxOutputLevelDbfs = 0.0;

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
    // kRf only: OutputConfig::rf_ceiling, in dBFS rather than the library's
    // own linear magnitude - decoder_setup() converts it, held to full scale.
    // 0.0 is OutputConfig's own default, and this has no effect outside kRf.
    double rf_ceiling_db = 0.0;
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
    // Whether the LFE joins a fold (where the stream allows it). One control
    // for both formats with a default for each until the listener sets it:
    // off for AC-3 and E-AC-3, whose §7.8 fold leaves it out unless asked,
    // and on for AC-4, whose downmix takes it at lfe_mixgain (ETSI TS 103
    // 190-1 clause 6.2.17).
    std::optional<bool> mix_lfe = std::nullopt;
    // Levels to fold with in place of the stream's.
    MixLevelOverride mix_levels{};
    DualMonoChoice dual_mono = DualMonoChoice::kBoth;
    // The programme of a multi-programme E-AC-3 stream to play, by its
    // independent substream id; unset plays the first. A session applies it
    // when an item opens, by choosing that programme's units, so it is not
    // part of the decoder configuration.
    std::optional<int> programme = std::nullopt;
    render::ObjectsPolicy objects = render::ObjectsPolicy::kAuto;
    // Which domain a reconstruction runs the JOC matrix in (joc.hpp): kQmf,
    // TS 103 420's own domain, or the cheaper kMdctBand, which also lags the
    // bed less (256 samples against 576). Only reaches anything when objects
    // are reconstructed at all.
    oba::joc::Domain joc_domain = oba::joc::Domain::kQmf;
    // §7.10. A player keeps a stream continuous through a damaged frame.
    ConcealmentPolicy concealment = ConcealmentPolicy::kRepeatFade;
    // Passed straight to DecoderConfig::fast_imdct: the FFT evaluation of
    // §7.9.4 step 3's inverse transform, against its reference direct form.
    bool fast_inverse_transform = true;
    Ac4Settings ac4{};

    friend bool operator==(const DecoderSettings&, const DecoderSettings&) = default;
};

// What `settings` make of the decoders that serve `layout`: which fold the
// decoder does and whether it reconstructs objects, and the configuration to
// build them with - AC-3's and E-AC-3's, and AC-4's, whose downmix is the
// same fold.
struct DecoderSetup {
    render::Serving serving{};
    DecoderConfig config{};
    ac4::DecoderConfig ac4{};
};

[[nodiscard]] DecoderSetup decoder_setup(const DecoderSettings& settings,
                                         const render::OutputLayout& layout);

// The presentation an AC-4 decoder is asked for: DecoderSetup::ac4's
// presentation, which does not depend on the layout.
[[nodiscard]] ac4::PresentationChoice presentation_choice(const DecoderSettings& settings);

// Every control's value on one line, for the diagnostics file: "line mode,
// stereo fold Lo/Ro, no LFE in folds, the stream's mix levels, ...".
[[nodiscard]] std::string describe(const DecoderSettings& settings);

// What a decode for re-encoding uses: the programme as coded, with no dynamic
// range gain, no dialogue normalisation and no objects - the receiver applies
// its own from the metadata carried across, and a JOC stream's bed is the
// mix its objects were coded against. Only the listener's choices a receiver
// cannot make are kept: which half of a dual mono programme is heard, how a
// damaged frame is concealed, and the programme, which is the session's
// choice of units in any case.
[[nodiscard]] DecoderSettings transcode_settings(const DecoderSettings& listener);

}  // namespace ac3::hearth
