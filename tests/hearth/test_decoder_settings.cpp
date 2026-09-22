#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <optional>

#include "ac3/decoder/decoder.hpp"
#include "ac3/decoder/output.hpp"
#include "ac3/render/layout.hpp"
#include "ac3/render/serving.hpp"
#include "decoder_settings.hpp"

// ac3::hearth::decoder_setup (apps/hearth/engine/decoder_settings.cpp): every
// decoder control lands where the library reads it, for the layout it serves.
// What each setting then does to the audio is the library's to test, and
// tests/meta/test_drc.cpp and tests/decoder/test_output_stage.cpp do.

namespace {

using ac3::hearth::DecoderSettings;
using ac3::hearth::decoder_setup;

ac3::render::OutputLayout layout(const char* name) {
    const auto parsed = ac3::render::OutputLayout::parse(name);
    REQUIRE(parsed.has_value());
    return *parsed;
}

}  // namespace

TEST_CASE("decoder settings: the defaults are line mode, a Lo/Ro fold for two speakers, "
          "and concealment",
          "[hearth][decoder-settings]") {
    const auto stereo = decoder_setup(DecoderSettings{}, layout("2.0"));
    CHECK(stereo.config.output.mode == ac3::OperatingMode::kLine);
    CHECK(stereo.config.output.rf_ceiling == 1.0);
    CHECK(stereo.serving.fold == ac3::DownmixTarget::kLoRo);
    CHECK(stereo.config.output.target == ac3::DownmixTarget::kLoRo);
    CHECK(stereo.config.output.ltrt_phase_shift);
    CHECK_FALSE(stereo.config.output.mix_lfe);
    CHECK(stereo.config.output.mix_override == ac3::MixLevelOverride{});
    CHECK(stereo.config.skip_object_reconstruction);
    CHECK_FALSE(stereo.config.programme.has_value());
    CHECK(stereo.config.concealment == ac3::ConcealmentPolicy::kRepeatFade);

    // A wider layout is rendered, not folded; objects are reconstructed only
    // where there is height to place them in.
    const auto wide = decoder_setup(DecoderSettings{}, layout("5.1"));
    CHECK_FALSE(wide.serving.fold.has_value());
    CHECK(wide.config.output.target == ac3::DownmixTarget::kAsCoded);
    CHECK(wide.config.skip_object_reconstruction);
    const auto height = decoder_setup(DecoderSettings{}, layout("5.1.2"));
    CHECK_FALSE(height.config.skip_object_reconstruction);
}

TEST_CASE("decoder settings: every control reaches the configuration", "[hearth][decoder-settings]") {
    DecoderSettings settings;
    settings.mode = ac3::OperatingMode::kCustom;
    settings.rf_ceiling_db = -20.0;
    settings.drc_cut = 0.25;
    settings.drc_boost = 0.75;
    settings.heavy_compression = true;
    settings.normalise_dialogue = false;
    settings.stereo_fold = ac3::DownmixTarget::kLtRt;
    settings.ltrt_phase_shift = false;
    settings.mix_lfe = true;
    settings.mix_levels.loro_clev = 0.5;
    settings.mix_levels.lfe_mix_level_db = 3.0;
    settings.programme = 2;
    settings.objects = ac3::render::ObjectsPolicy::kNever;
    settings.concealment = ac3::ConcealmentPolicy::kMute;

    const auto setup = decoder_setup(settings, layout("2.0"));
    const ac3::DecoderConfig& config = setup.config;
    CHECK(config.output.mode == ac3::OperatingMode::kCustom);
    // Reaches OutputConfig whatever the mode - it just has no effect outside
    // kRf, the same as every other custom-only field has no effect here.
    CHECK(config.output.rf_ceiling == Catch::Approx(0.1).margin(1e-9));
    CHECK(config.drc_scale == 0.25);
    REQUIRE(config.drc_boost_scale.has_value());
    CHECK(*config.drc_boost_scale == 0.75);
    CHECK(config.heavy_compression);
    CHECK_FALSE(config.output.apply_dialnorm);
    CHECK(config.output.target == ac3::DownmixTarget::kLtRt);
    CHECK_FALSE(config.output.ltrt_phase_shift);
    CHECK(config.output.mix_lfe);
    CHECK(config.output.mix_override.loro_clev == std::optional<double>{0.5});
    CHECK(config.output.mix_override.lfe_mix_level_db == std::optional<double>{3.0});
    CHECK_FALSE(config.output.mix_override.ltrt_clev.has_value());
    // The programme is the session's choice of units, never a decoder filter:
    // a decoder told to skip other programmes would skip an item still
    // playing the old one when the setting changes.
    CHECK_FALSE(config.programme.has_value());
    CHECK(config.concealment == ac3::ConcealmentPolicy::kMute);
    CHECK(config.skip_object_reconstruction);
}

TEST_CASE("decoder settings: shares outside 0 to 1 are held to it", "[hearth][decoder-settings]") {
    DecoderSettings settings;
    settings.mode = ac3::OperatingMode::kCustom;
    settings.drc_cut = -0.5;
    settings.drc_boost = 4.0;
    const auto setup = decoder_setup(settings, layout("5.1"));
    CHECK(setup.config.drc_scale == 0.0);
    REQUIRE(setup.config.drc_boost_scale.has_value());
    CHECK(*setup.config.drc_boost_scale == 1.0);
}

TEST_CASE("decoder settings: the RF ceiling never exceeds full scale", "[hearth][decoder-settings]") {
    DecoderSettings settings;
    settings.mode = ac3::OperatingMode::kRf;
    settings.rf_ceiling_db = 6.0;  // above full scale - held to it instead
    CHECK(decoder_setup(settings, layout("5.1")).config.output.rf_ceiling == 1.0);
    // Below full scale is not held to anything: more headroom than asked
    // for is a quieter fold, not an invalid one (output.hpp's own comment).
    settings.rf_ceiling_db = -20.0;
    CHECK(decoder_setup(settings, layout("5.1")).config.output.rf_ceiling ==
          Catch::Approx(0.1).margin(1e-9));
}

TEST_CASE("decoder settings: the fold follows the layout, and only the two stereo folds are "
          "a stereo fold",
          "[hearth][decoder-settings]") {
    DecoderSettings settings;
    settings.stereo_fold = ac3::DownmixTarget::kLtRt;
    // One speaker folds to mono whatever the stereo fold says.
    CHECK(decoder_setup(settings, layout("1.0")).config.output.target ==
          ac3::DownmixTarget::kMono);
    CHECK(decoder_setup(settings, layout("2.0")).config.output.target ==
          ac3::DownmixTarget::kLtRt);
    // Mono and "as coded" are not stereo folds; two speakers get Lo/Ro.
    settings.stereo_fold = ac3::DownmixTarget::kMono;
    CHECK(decoder_setup(settings, layout("2.0")).config.output.target ==
          ac3::DownmixTarget::kLoRo);
    settings.stereo_fold = ac3::DownmixTarget::kAsCoded;
    CHECK(decoder_setup(settings, layout("2.0")).config.output.target ==
          ac3::DownmixTarget::kLoRo);
}

TEST_CASE("decoder settings: the object policy decides reconstruction for rendered layouts",
          "[hearth][decoder-settings]") {
    DecoderSettings settings;
    settings.objects = ac3::render::ObjectsPolicy::kAlways;
    CHECK_FALSE(decoder_setup(settings, layout("5.1")).config.skip_object_reconstruction);
    // A folded layout never reconstructs, whatever the policy.
    CHECK(decoder_setup(settings, layout("2.0")).config.skip_object_reconstruction);
    settings.objects = ac3::render::ObjectsPolicy::kNever;
    CHECK(decoder_setup(settings, layout("7.1.4")).config.skip_object_reconstruction);
}

TEST_CASE("decoder settings: equal settings compare equal, and any control tells them apart",
          "[hearth][decoder-settings]") {
    const DecoderSettings base;
    DecoderSettings changed = base;
    CHECK(changed == base);
    changed.mix_levels.ltrt_slev = 0.25;
    CHECK_FALSE(changed == base);
    changed = base;
    changed.dual_mono = ac3::hearth::DualMonoChoice::kSecond;
    CHECK_FALSE(changed == base);
    changed = base;
    changed.rf_ceiling_db = -3.0;
    CHECK_FALSE(changed == base);
}

TEST_CASE("decoder settings: a transcode decodes the programme as coded, whatever the listener "
          "chose",
          "[hearth][decoder-settings][transcode]") {
    DecoderSettings listener;
    listener.mode = ac3::OperatingMode::kRf;
    listener.rf_ceiling_db = -6.0;
    listener.drc_cut = 0.5;
    listener.heavy_compression = true;
    listener.stereo_fold = ac3::DownmixTarget::kLtRt;
    listener.mix_lfe = true;
    listener.mix_levels.loro_clev = 0.5;
    listener.objects = ac3::render::ObjectsPolicy::kAlways;
    // What a receiver cannot choose for itself is kept.
    listener.dual_mono = ac3::hearth::DualMonoChoice::kSecond;
    listener.concealment = ac3::ConcealmentPolicy::kMute;
    listener.programme = 3;

    const DecoderSettings neutral = ac3::hearth::transcode_settings(listener);
    CHECK(neutral.rf_ceiling_db == 0.0);
    CHECK(neutral.dual_mono == ac3::hearth::DualMonoChoice::kSecond);
    CHECK(neutral.concealment == ac3::ConcealmentPolicy::kMute);
    CHECK(neutral.programme == std::optional<int>{3});
    CHECK(neutral.mix_levels == ac3::MixLevelOverride{});
    CHECK_FALSE(neutral.mix_lfe);

    const auto setup = decoder_setup(neutral, layout("5.1"));
    const ac3::DecoderConfig& config = setup.config;
    CHECK(config.output.mode == ac3::OperatingMode::kCustom);
    CHECK(config.drc_scale == 0.0);
    REQUIRE(config.drc_boost_scale.has_value());
    CHECK(*config.drc_boost_scale == 0.0);
    CHECK_FALSE(config.heavy_compression);
    CHECK_FALSE(config.output.apply_dialnorm);
    CHECK(config.output.target == ac3::DownmixTarget::kAsCoded);
    CHECK_FALSE(setup.serving.fold.has_value());
    CHECK(config.skip_object_reconstruction);
    CHECK(config.concealment == ac3::ConcealmentPolicy::kMute);
}
