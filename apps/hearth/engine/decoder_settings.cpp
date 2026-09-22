#include "decoder_settings.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
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

}  // namespace

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
    config.output.mode = settings.mode;
    // dB to the linear magnitude OutputConfig wants, held to full scale: a
    // ceiling above it would let kRf's fold clip, which is the one thing
    // kRf promises not to do (output.hpp's own comment on rf_ceiling).
    config.output.rf_ceiling = std::pow(10.0, std::min(settings.rf_ceiling_db, 0.0) / 20.0);
    config.output.apply_dialnorm = settings.normalise_dialogue;
    config.output.ltrt_phase_shift = settings.ltrt_phase_shift;
    config.output.mix_lfe = settings.mix_lfe;
    config.output.mix_override = settings.mix_levels;
    // Not settings.programme: which programme plays is which units a session
    // feeds (Session::open), and a decoder told to skip every other
    // programme would skip the whole of an item still playing the old one
    // when the setting changes under it.
    config.concealment = settings.concealment;
    render::configure_decoder(setup.serving, config);
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
    parts.emplace_back(settings.mix_lfe ? "LFE in folds" : "no LFE in folds");

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
    switch (settings.concealment) {
        case ConcealmentPolicy::kNone: parts.emplace_back("no concealment"); break;
        case ConcealmentPolicy::kRepeatFade: parts.emplace_back("concealment: repeat and fade"); break;
        case ConcealmentPolicy::kMute: parts.emplace_back("concealment: mute"); break;
    }
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
