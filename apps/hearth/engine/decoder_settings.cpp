#include "decoder_settings.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

// See decoder_settings.hpp.

namespace ac3::hearth {

namespace {

// The parts, comma-separated.
[[nodiscard]] std::string joined(const std::vector<std::string>& parts) {
    std::string out;
    for (const std::string& part : parts) {
        if (!out.empty()) {
            out += ", ";
        }
        out += part;
    }
    return out;
}

// Part 1 Table 91's content_classifier of a service for the visually
// impaired, which audio description is.
constexpr int kVisuallyImpaired = 0b010;

// AC-4's downmix for the fold the layout gets: the same target, or the
// stream's preferred method for a stereo fold the listener has left to it.
[[nodiscard]] ac4::DownmixTarget ac4_downmix(const std::optional<DownmixTarget>& fold,
                                             bool preferred) {
    if (!fold) {
        return ac4::DownmixTarget::kAsCoded;
    }
    switch (*fold) {
        case DownmixTarget::kMono:
            return ac4::DownmixTarget::kMono;
        case DownmixTarget::kLtRt:
            return preferred ? ac4::DownmixTarget::kStereo : ac4::DownmixTarget::kLtRt;
        case DownmixTarget::kLoRo:
            return preferred ? ac4::DownmixTarget::kStereo : ac4::DownmixTarget::kLoRo;
        case DownmixTarget::kAsCoded:
            break;
    }
    return ac4::DownmixTarget::kAsCoded;
}

// The same policies under AC-4's names.
[[nodiscard]] ac4::ConcealmentPolicy ac4_concealment(ConcealmentPolicy policy) {
    switch (policy) {
        case ConcealmentPolicy::kNone:
            return ac4::ConcealmentPolicy::kNone;
        case ConcealmentPolicy::kRepeatFade:
            return ac4::ConcealmentPolicy::kRepeatFade;
        case ConcealmentPolicy::kMute:
            return ac4::ConcealmentPolicy::kMute;
    }
    return ac4::ConcealmentPolicy::kNone;
}

// AC-4's configuration, for the fold `serving` asks of the decoder.
[[nodiscard]] ac4::DecoderConfig ac4_setup(const DecoderSettings& settings,
                                           const render::Serving& serving) {
    const Ac4Settings& ac4 = settings.ac4;
    ac4::DecoderConfig config;
    const bool headphones = ac4.drc == ac4::DrcMode::kPortableHeadphones;
    config.output.output_level_dbfs =
        ac4.normalise ? std::optional<double>{std::clamp(
                            ac4.output_level_dbfs, kAc4MinOutputLevelDbfs, kAc4MaxOutputLevelDbfs)}
                      : std::nullopt;
    config.output.drc = ac4.drc;
    config.output.headphones = headphones;
    config.output.dialogue_enhancement_db = std::clamp(ac4.dialogue_enhancement_db, 0.0, 12.0);
    config.output.downmix = ac4_downmix(serving.fold, ac4.preferred_downmix);
    config.output.mix_lfe = settings.mix_lfe.value_or(true);
    config.output.dialogue_gain_db = ac4.dialogue_db;
    // Below -120 dB the decoder silences it, which is what "not mixed in" is.
    config.output.associated_gain_db = ac4.audio_description
                                           ? std::min(ac4.associated_db, 0.0)
                                           : -std::numeric_limits<double>::infinity();
    config.concealment = ac4_concealment(settings.concealment);
    config.presentation = presentation_choice(settings);
    return config;
}

[[nodiscard]] std::string_view drc_mode_words(ac4::DrcMode mode) {
    // clang-format off
    switch (mode) {
        case ac4::DrcMode::kOff: return "no compression";
        case ac4::DrcMode::kDefault: return "the DRC mode for the output level";
        case ac4::DrcMode::kHomeTheatre: return "home theatre DRC";
        case ac4::DrcMode::kFlatPanelTv: return "flat panel TV DRC";
        case ac4::DrcMode::kPortableSpeakers: return "portable speakers DRC";
        case ac4::DrcMode::kPortableHeadphones: return "portable headphones DRC";
    }
    // clang-format on
    return "an unknown DRC mode";
}

// Ac4Settings on one line, for describe().
[[nodiscard]] std::string describe_ac4(const Ac4Settings& ac4) {
    std::vector<std::string> parts;
    if (ac4.presentation_id) {
        parts.push_back(fmt::format("presentation_id {}", *ac4.presentation_id));
    } else if (ac4.presentation_index) {
        parts.push_back(fmt::format("presentation {}", *ac4.presentation_index));
    } else {
        parts.push_back(ac4.language.empty() ? std::string{"the stream's presentation"}
                                             : fmt::format("a presentation in {}", ac4.language));
    }
    parts.push_back(ac4.normalise ? fmt::format("dialogue to {:.0f} dBFS, {}",
                                                ac4.output_level_dbfs, drc_mode_words(ac4.drc))
                                  : std::string{"the coded level, no compression"});
    parts.push_back(fmt::format("dialogue enhancement {:.0f} dB", ac4.dialogue_enhancement_db));
    parts.push_back(fmt::format("dialogue {:+.1f} dB", ac4.dialogue_db));
    parts.push_back(ac4.audio_description
                        ? fmt::format("audio description at {:+.1f} dB", ac4.associated_db)
                        : std::string{"no audio description"});
    parts.emplace_back(ac4.preferred_downmix ? "the stream's preferred downmix"
                                             : "the stereo fold's downmix");
    return fmt::format("AC-4: {}", joined(parts));
}

}  // namespace

ac4::PresentationChoice presentation_choice(const DecoderSettings& settings) {
    const Ac4Settings& ac4 = settings.ac4;
    ac4::PresentationChoice choice;
    choice.presentation_id = ac4.presentation_id;
    if (ac4.presentation_index && *ac4.presentation_index >= 0) {
        choice.index = static_cast<std::size_t>(*ac4.presentation_index);
    }
    choice.language = ac4.language;
    if (ac4.audio_description) {
        choice.associated = kVisuallyImpaired;
        choice.associated_type = ac4::AssociatedType::kAudioDescription;
    }
    choice.headphones = ac4.drc == ac4::DrcMode::kPortableHeadphones;
    return choice;
}

DecoderSetup decoder_setup(const DecoderSettings& settings, const render::OutputLayout& layout) {
    DecoderSetup setup;
    // render::serve takes the two stereo folds only; mono is the one-speaker
    // layout's, and "as coded" is not a fold.
    const DownmixTarget stereo = settings.stereo_fold == DownmixTarget::kLtRt
                                     ? DownmixTarget::kLtRt
                                     : DownmixTarget::kLoRo;
    setup.serving = render::serve(layout, stereo, settings.objects);

    DecoderConfig& config = setup.config;
    // The custom switches go in whatever the mode: the decoders ignore them
    // under kLine and kRf (internal::resolve_operating_mode), so a settings
    // page can keep them while another mode is chosen.
    config.drc_scale = std::clamp(settings.drc_cut, 0.0, 1.0);
    config.drc_boost_scale = std::clamp(settings.drc_boost, 0.0, 1.0);
    config.heavy_compression = settings.heavy_compression;
    config.fast_imdct = settings.fast_inverse_transform;
    config.output.mode = settings.mode;
    // dB to the linear magnitude OutputConfig wants, held to full scale: a
    // ceiling above it would let kRf's fold clip, which is the one thing
    // kRf promises not to do (output.hpp's own comment on rf_ceiling).
    config.output.rf_ceiling = std::pow(10.0, std::min(settings.rf_ceiling_db, 0.0) / 20.0);
    config.output.apply_dialnorm = settings.normalise_dialogue;
    config.output.ltrt_phase_shift = settings.ltrt_phase_shift;
    config.output.mix_lfe = settings.mix_lfe.value_or(false);
    config.output.mix_override = settings.mix_levels;
    config.joc_domain = settings.joc_domain;
    // Not settings.programme: which programme plays is which units a session
    // feeds (Session::open), and a decoder told to skip every other
    // programme would skip the whole of an item still playing the old one
    // when the setting changes under it.
    config.concealment = settings.concealment;
    render::configure_decoder(setup.serving, config);
    setup.ac4 = ac4_setup(settings, setup.serving);
    return setup;
}

std::string describe(const DecoderSettings& settings) {
    std::vector<std::string> parts;
    switch (settings.mode) {
        case OperatingMode::kLine: parts.emplace_back("line mode"); break;
        case OperatingMode::kRf:
            parts.push_back(fmt::format("RF mode (ceiling {:.1f} dBFS)", settings.rf_ceiling_db));
            break;
        case OperatingMode::kCustom:
            parts.push_back(fmt::format("custom mode (cut {:.2f}, boost {:.2f}, compr {}, dialogue {})",
                                        settings.drc_cut, settings.drc_boost,
                                        settings.heavy_compression ? "on" : "off",
                                        settings.normalise_dialogue ? "normalised" : "as coded"));
            break;
    }
    parts.push_back(settings.stereo_fold == DownmixTarget::kLtRt
                        ? fmt::format("stereo fold Lt/Rt (phase shift {})",
                                      settings.ltrt_phase_shift ? "on" : "off")
                        : std::string{"stereo fold Lo/Ro"});
    parts.emplace_back(!settings.mix_lfe   ? "LFE in AC-4's folds only"
                       : *settings.mix_lfe ? "LFE in folds"
                                           : "no LFE in folds");

    const MixLevelOverride& levels = settings.mix_levels;
    std::vector<std::string> set;
    if (levels.loro_clev) {
        set.push_back(fmt::format("Lo/Ro centre {:.3f}", *levels.loro_clev));
    }
    if (levels.loro_slev) {
        set.push_back(fmt::format("Lo/Ro surround {:.3f}", *levels.loro_slev));
    }
    if (levels.ltrt_clev) {
        set.push_back(fmt::format("Lt/Rt centre {:.3f}", *levels.ltrt_clev));
    }
    if (levels.ltrt_slev) {
        set.push_back(fmt::format("Lt/Rt surround {:.3f}", *levels.ltrt_slev));
    }
    if (levels.lfe_mix_level_db) {
        set.push_back(fmt::format("LFE {:+.1f} dB", *levels.lfe_mix_level_db));
    }
    parts.push_back(set.empty() ? std::string{"the stream's mix levels"}
                                : "mix levels " + joined(set));

    switch (settings.dual_mono) {
        case DualMonoChoice::kBoth: parts.emplace_back("dual mono: both"); break;
        case DualMonoChoice::kFirst: parts.emplace_back("dual mono: channel 1"); break;
        case DualMonoChoice::kSecond: parts.emplace_back("dual mono: channel 2"); break;
    }
    parts.push_back(settings.programme ? fmt::format("programme {}", *settings.programme)
                                       : std::string{"the first programme"});
    switch (settings.objects) {
        case render::ObjectsPolicy::kAuto: parts.emplace_back("objects for height layouts"); break;
        case render::ObjectsPolicy::kNever: parts.emplace_back("objects never"); break;
        case render::ObjectsPolicy::kAlways: parts.emplace_back("objects always"); break;
    }
    switch (settings.joc_domain) {
        case oba::joc::Domain::kQmf: parts.emplace_back("objects reconstructed in the QMF domain"); break;
        case oba::joc::Domain::kMdctBand:
            parts.emplace_back("objects reconstructed in the MDCT-band domain");
            break;
    }
    switch (settings.concealment) {
        case ConcealmentPolicy::kNone: parts.emplace_back("no concealment"); break;
        case ConcealmentPolicy::kRepeatFade: parts.emplace_back("concealment: repeat and fade"); break;
        case ConcealmentPolicy::kMute: parts.emplace_back("concealment: mute"); break;
    }
    parts.emplace_back(settings.fast_inverse_transform ? "fast inverse transform"
                                                        : "reference inverse transform");
    parts.push_back(describe_ac4(settings.ac4));
    return joined(parts);
}

DecoderSettings transcode_settings(const DecoderSettings& listener) {
    DecoderSettings neutral;
    neutral.mode = OperatingMode::kCustom;
    neutral.rf_ceiling_db = 0.0;
    neutral.drc_cut = 0.0;
    neutral.drc_boost = 0.0;
    neutral.heavy_compression = false;
    neutral.normalise_dialogue = false;
    neutral.objects = render::ObjectsPolicy::kNever;
    neutral.dual_mono = listener.dual_mono;
    neutral.concealment = listener.concealment;
    neutral.programme = listener.programme;
    return neutral;
}

}  // namespace ac3::hearth
