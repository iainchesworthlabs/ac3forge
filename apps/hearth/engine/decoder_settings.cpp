#include "decoder_settings.hpp"

#include <algorithm>

// See decoder_settings.hpp.

namespace ac3::hearth {

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

}  // namespace ac3::hearth
