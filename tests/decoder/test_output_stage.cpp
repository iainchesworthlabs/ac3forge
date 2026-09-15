#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/decoder/output.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/io/metadata_edit.hpp"
#include "ac3/meta/bsi.hpp"
#include "ac3/meta/mixing.hpp"

// The §7.8 output stage (ac3/decoder/output.hpp): dialnorm normalisation, the
// three folds, LFE mixing, and §7.7's line and RF operating modes.
//
// The matrix tests here drive the stage directly with impulses rather than
// through an encode/decode round trip, because a matrix is exactly the thing
// an impulse reads off unambiguously: put 1.0 in one coded channel and the
// output IS that channel's coefficient. The round-trip tests further down
// then check that the same fold survives real coded audio over several
// frames, which is what CONTRIBUTING.md's validation discipline asks for and
// what an impulse cannot tell you.

namespace {

std::vector<std::vector<float>> impulse_at(std::size_t channels, std::size_t channel,
                                           std::size_t length = 64) {
    std::vector<std::vector<float>> pcm(channels, std::vector<float>(length, 0.0F));
    pcm[channel][0] = 1.0F;
    return pcm;
}

// A distinct tone per channel over several frames, so a fold that drops or
// swaps a channel is distinguishable from one that is merely scaled - the
// same reasoning tests/decoder/test_live_downmix.cpp's own bed_frame uses.
std::vector<std::vector<float>> tones(std::span<const double> hz, std::uint64_t start,
                                      int samples, double amplitude = 0.3) {
    std::vector<std::vector<float>> pcm(hz.size(),
                                        std::vector<float>(static_cast<std::size_t>(samples)));
    for (std::size_t ch = 0; ch < hz.size(); ++ch) {
        for (int i = 0; i < samples; ++i) {
            const auto n = static_cast<double>(start + static_cast<std::uint64_t>(i));
            pcm[ch][static_cast<std::size_t>(i)] = static_cast<float>(
                amplitude * std::sin(2.0 * std::numbers::pi * hz[ch] * n / 48000.0));
        }
    }
    return pcm;
}

double peak_of(const std::vector<std::vector<float>>& channels) {
    double peak = 0.0;
    for (const auto& channel : channels) {
        for (const float sample : channel) {
            peak = std::max(peak, std::abs(static_cast<double>(sample)));
        }
    }
    return peak;
}

}  // namespace

TEST_CASE("the output stage is a bit-exact no-op until it is asked for something",
          "[decoder][output]") {
    // The whole premise of the decoders as a reference: a caller that
    // configures nothing gets the coded channels back untouched. Not
    // "approximately" - the same floats.
    ac3::OutputStage stage;
    const std::array<double, 5> hz = {200.0, 300.0, 500.0, 700.0, 1100.0};
    auto channels = tones(hz, 0, 1536);
    const auto before = channels;
    stage.apply(channels, ac3::Acmod::k3_2, false, ac3::MixLevels{}, 20);
    REQUIRE(channels.size() == before.size());
    for (std::size_t ch = 0; ch < channels.size(); ++ch) {
        for (std::size_t i = 0; i < channels[ch].size(); ++i) {
            REQUIRE(channels[ch][i] == before[ch][i]);
        }
    }
    CHECK(stage.latency_samples() == 0);
}

TEST_CASE("Lo/Ro reproduces the section 7.8 coefficients, normalised", "[decoder][output]") {
    // 3/2 with clev -3 dB and slev -6 dB: left takes L at 1, C at 0.7071 and
    // Ls at 0.5, so §7.8.1's divisor is 1 + 0.7071 + 0.5 = 2.2071. Reading
    // the coefficients back off an impulse is the only test that distinguishes
    // "the right matrix" from "a matrix that happens to sound plausible".
    const ac3::MixLevels levels{.loro_clev = ac3::meta::level::kMinus3dB,
                                .loro_slev = ac3::meta::level::kMinus6dB};
    const double divisor = 1.0 + ac3::meta::level::kMinus3dB + ac3::meta::level::kMinus6dB;

    struct Expect {
        std::size_t channel;
        double left;
        double right;
    };
    const std::array<Expect, 5> expected = {
        Expect{.channel = 0, .left = 1.0, .right = 0.0},                                 // L
        Expect{.channel = 1,
               .left = ac3::meta::level::kMinus3dB,
               .right = ac3::meta::level::kMinus3dB},                                    // C
        Expect{.channel = 2, .left = 0.0, .right = 1.0},                                 // R
        Expect{.channel = 3, .left = ac3::meta::level::kMinus6dB, .right = 0.0},         // Ls
        Expect{.channel = 4, .left = 0.0, .right = ac3::meta::level::kMinus6dB},         // Rs
    };
    for (const auto& e : expected) {
        ac3::OutputStage stage{{.target = ac3::DownmixTarget::kLoRo}};
        auto channels = impulse_at(5, e.channel);
        stage.apply(channels, ac3::Acmod::k3_2, false, levels, 31);
        REQUIRE(channels.size() == 2);
        CHECK(static_cast<double>(channels[0][0]) ==
              Catch::Approx(e.left / divisor).margin(1e-6));
        CHECK(static_cast<double>(channels[1][0]) ==
              Catch::Approx(e.right / divisor).margin(1e-6));
    }
}

TEST_CASE("section 7.8.1 normalisation bounds a REAL matrix by the loudest coded sample",
          "[decoder][output]") {
    // The clause exists to prevent overload, and for a matrix of plain
    // coefficients this is the claim it makes: with every channel
    // simultaneously at full scale - the worst case a matrix can be handed -
    // the fold still does not exceed full scale.
    //
    // Lt/Rt with its phase shift is deliberately NOT in this list, and the
    // next test says why.
    for (const auto target : {ac3::DownmixTarget::kLoRo, ac3::DownmixTarget::kMono}) {
        ac3::OutputStage stage{{.target = target}};
        std::vector<std::vector<float>> channels(5, std::vector<float>(256, 1.0F));
        stage.apply(channels, ac3::Acmod::k3_2, false, ac3::MixLevels{}, 31);
        INFO("target " << static_cast<int>(target));
        CHECK(peak_of(channels) <= 1.0 + 1e-6);
    }
    // Lt/Rt's sign-only matrix is a matrix of plain coefficients like the
    // other two, and is bounded like them.
    ac3::OutputStage sign_only{
        {.target = ac3::DownmixTarget::kLtRt, .ltrt_phase_shift = false}};
    std::vector<std::vector<float>> channels(5, std::vector<float>(256, 1.0F));
    sign_only.apply(channels, ac3::Acmod::k3_2, false, ac3::MixLevels{}, 31);
    CHECK(peak_of(channels) <= 1.0 + 1e-6);
}

TEST_CASE("Lt/Rt's phase shift can overshoot, and RF mode is what bounds it",
          "[decoder][output]") {
    // A Hilbert transformer preserves ENERGY, not peak: its response at a
    // discontinuity is unbounded, so §7.8.1's coefficient normalisation -
    // which bounds a sum of plain coefficients - cannot bound the shifted
    // path the way it bounds the other folds. That is a property of what
    // §7.8.2 asks for rather than a defect in how it is done here, and the
    // honest thing is to state it and point at the tool that does bound it.
    std::vector<std::vector<float>> stepped(5, std::vector<float>(256, 1.0F));
    ac3::OutputStage shifted{{.target = ac3::DownmixTarget::kLtRt}};
    shifted.apply(stepped, ac3::Acmod::k3_2, false, ac3::MixLevels{}, 31);
    CHECK(peak_of(stepped) > 1.0);

    // RF mode is the mode whose whole promise is that nothing clips, and it
    // holds for exactly the same input.
    std::vector<std::vector<float>> guarded(5, std::vector<float>(256, 1.0F));
    ac3::OutputStage rf{
        {.target = ac3::DownmixTarget::kLtRt, .mode = ac3::OperatingMode::kRf}};
    rf.apply(guarded, ac3::Acmod::k3_2, false, ac3::MixLevels{}, 31);
    CHECK(peak_of(guarded) <= 1.0 + 1e-6);
    CHECK(rf.rf_protection_db() < 0.0);
}

TEST_CASE("a mono fold takes the section 7.8 1/0 branch", "[decoder][output]") {
    ac3::OutputStage stage{{.target = ac3::DownmixTarget::kMono}};
    auto channels = impulse_at(5, 1);  // the centre channel
    const ac3::MixLevels levels{.loro_clev = ac3::meta::level::kMinus3dB,
                                .loro_slev = ac3::meta::level::kMinus6dB};
    stage.apply(channels, ac3::Acmod::k3_2, false, levels, 31);
    REQUIRE(channels.size() == 1);
    // "mix center into center using clev and +3 dB gain", then normalised by
    // the same sum the builder computes.
    const auto coeffs = ac3::meta::mono_downmix(ac3::Acmod::k3_2, levels.loro_clev,
                                                levels.loro_slev);
    CHECK(static_cast<double>(channels[0][0]) == Catch::Approx(coeffs[1]).margin(1e-6));
}

TEST_CASE("Lt/Rt puts the surround sum into the two outputs in opposite polarity",
          "[decoder][output]") {
    // §7.8.2's defining property, and the one a Dolby Surround decoder
    // recovers the surround channel from. Checked with the phase shift OFF so
    // the claim under test is the matrix's sign and nothing else.
    ac3::OutputStage stage{{.target = ac3::DownmixTarget::kLtRt, .ltrt_phase_shift = false}};
    auto channels = impulse_at(5, 3);  // Ls
    stage.apply(channels, ac3::Acmod::k3_2, false, ac3::MixLevels{}, 31);
    REQUIRE(channels.size() == 2);
    CHECK(static_cast<double>(channels[0][0]) < 0.0);
    CHECK(static_cast<double>(channels[1][0]) ==
          Catch::Approx(-static_cast<double>(channels[0][0])).margin(1e-6));
    CHECK(stage.latency_samples() == 0);
}

TEST_CASE("Lt/Rt's surround really is phase shifted 90 degrees, and the direct path is "
          "delayed to match",
          "[decoder][output]") {
    // Two separate claims, both worth stating, because getting one right and
    // the other wrong is the plausible failure - and one of them WAS wrong
    // when this test was first written (the convolution walked the kernel's
    // zero taps, so the "shift" was no shift at all, which sounds entirely
    // plausible and is simply not Lt/Rt).
    //
    // The two are checked with different signals on purpose. Delay is checked
    // with an impulse, because a delayed sine is just a phase-shifted sine and
    // correlating one against the other proves nothing. Quadrature is checked
    // with a tone, because that is the only thing quadrature means.
    constexpr int kLength = 8192;
    ac3::OutputStage stage{{.target = ac3::DownmixTarget::kLtRt}};
    const int latency = stage.latency_samples();
    REQUIRE(latency > 0);

    SECTION("the direct path comes out delayed by exactly the filter's group delay") {
        // L and R together, so the fold is symmetric and the surround path
        // contributes nothing: whatever comes out is the direct path alone.
        std::vector<std::vector<float>> channels(5, std::vector<float>(kLength, 0.0F));
        channels[0][100] = 1.0F;  // L
        channels[2][100] = 1.0F;  // R
        stage.apply(channels, ac3::Acmod::k3_2, false, ac3::MixLevels{}, 31);
        REQUIRE(channels.size() == 2);
        std::size_t peak_at = 0;
        double best = 0.0;
        for (std::size_t i = 0; i < channels[0].size(); ++i) {
            if (std::abs(static_cast<double>(channels[0][i])) > best) {
                best = std::abs(static_cast<double>(channels[0][i]));
                peak_at = i;
            }
        }
        CHECK(peak_at == 100U + static_cast<std::size_t>(latency));
        CHECK(best > 0.0);
    }

    SECTION("the surround sum comes out in quadrature with where it went in") {
        // Ls alone: Lt = -shift(Ls), Rt = +shift(Ls), so the difference IS
        // the shifted surround with nothing else mixed into it.
        constexpr double kHz = 1000.0;
        std::vector<std::vector<float>> channels(5, std::vector<float>(kLength, 0.0F));
        for (int i = 0; i < kLength; ++i) {
            channels[3][static_cast<std::size_t>(i)] = static_cast<float>(
                0.5 * std::sin(2.0 * std::numbers::pi * kHz * static_cast<double>(i) / 48000.0));
        }
        stage.apply(channels, ac3::Acmod::k3_2, false, ac3::MixLevels{}, 31);
        REQUIRE(channels.size() == 2);

        // Well past the filter's transient, and away from the end.
        const auto start = static_cast<std::size_t>(latency) + 512;
        const auto stop = static_cast<std::size_t>(kLength) - 512;
        double with_sine = 0.0;
        double with_cosine = 0.0;
        for (std::size_t i = start; i < stop; ++i) {
            const double difference =
                0.5 * (static_cast<double>(channels[1][i]) - static_cast<double>(channels[0][i]));
            const double phase = 2.0 * std::numbers::pi * kHz *
                                 static_cast<double>(i - static_cast<std::size_t>(latency)) /
                                 48000.0;
            with_sine += difference * std::sin(phase);
            with_cosine += difference * std::cos(phase);
        }
        // In quadrature with the input tone at the filter's own delay: the
        // cosine component is everything, the sine component is nothing.
        INFO("sine " << with_sine << ", cosine " << with_cosine);
        CHECK(std::abs(with_cosine) > 20.0 * std::abs(with_sine));
        // And it is a shift, not an attenuation to nothing - the failure the
        // zero-tap bug produced would satisfy a quadrature test on its own.
        CHECK(std::abs(with_cosine) > 100.0);
    }

    SECTION("the two outputs still take the surround in opposite polarity") {
        std::vector<std::vector<float>> channels(5, std::vector<float>(kLength, 0.0F));
        channels[3][100] = 1.0F;
        stage.apply(channels, ac3::Acmod::k3_2, false, ac3::MixLevels{}, 31);
        for (std::size_t i = 0; i < channels[0].size(); ++i) {
            REQUIRE(static_cast<double>(channels[0][i]) ==
                    Catch::Approx(-static_cast<double>(channels[1][i])).margin(1e-9));
        }
    }
}

TEST_CASE("dialnorm normalises onto the -31 dBFS reference and never boosts",
          "[decoder][output]") {
    // dialnorm 20 means dialogue sits at -20 dBFS, 11 dB hotter than the
    // reference, so the decoder attenuates by 11 dB. 31 is the reference
    // itself and must be EXACTLY unity - a reference decoder that quietly
    // multiplied by 0.9999 would fail the round-trip identity tests
    // elsewhere for a reason nobody would find.
    CHECK(ac3::meta::dialnorm_gain(31) == 1.0);
    CHECK(ac3::meta::to_db(ac3::meta::dialnorm_gain(20)) == Catch::Approx(-11.0).margin(1e-9));
    CHECK(ac3::meta::dialnorm_gain(1) < 1.0);
    // §5.4.2.8 reserves 0; a decoder has no better reading of it than "no
    // information", and leaving the audio alone is what that means.
    CHECK(ac3::meta::dialnorm_gain(0) == 1.0);

    ac3::OutputStage stage{{.apply_dialnorm = true}};
    std::vector<std::vector<float>> channels(2, std::vector<float>(64, 1.0F));
    stage.apply(channels, ac3::Acmod::k2_0, false, ac3::MixLevels{}, 20);
    // No fold was asked for, so the channel count is untouched and only the
    // level moved.
    REQUIRE(channels.size() == 2);
    CHECK(static_cast<double>(channels[0][0]) ==
          Catch::Approx(ac3::meta::dialnorm_gain(20)).margin(1e-6));
}

TEST_CASE("dual mono normalises Ch2 by its own dialnorm2, not Ch1's", "[decoder][output][dual-mono]") {
    // §5.4.2.16: dialnorm2 is Ch2's OWN reference. Dual mono's two channels
    // are unrelated programmes (this file's own class comment on apply()), so
    // a stage that scaled both by Ch1's dialnorm - the bug this guards
    // against - would leave Ch2 audibly off level whenever the two differ, as
    // they do here.
    ac3::OutputStage stage{{.apply_dialnorm = true}};
    std::vector<std::vector<float>> channels(2, std::vector<float>(64, 1.0F));
    stage.apply(channels, ac3::Acmod::kDualMono, false, ac3::MixLevels{}, 27, 18);
    // Dual mono is never folded (OutputStage refuses it outright - see
    // apply()'s own comment), so the channel count is untouched and only the
    // level moved, on each channel by its own reference.
    REQUIRE(channels.size() == 2);
    CHECK(static_cast<double>(channels[0][0]) ==
          Catch::Approx(ac3::meta::dialnorm_gain(27)).margin(1e-6));
    CHECK(static_cast<double>(channels[1][0]) ==
          Catch::Approx(ac3::meta::dialnorm_gain(18)).margin(1e-6));
    // The two gains actually differ - proof this isn't passing by coincidence
    // because dialnorm_gain(27) and dialnorm_gain(18) happen to agree.
    CHECK(ac3::meta::dialnorm_gain(27) != ac3::meta::dialnorm_gain(18));

    // Without a dialnorm2 to give, Ch2 falls back to Ch1's dialnorm - the
    // pre-existing behaviour every other acmod already relies on, and the
    // only sane default for a caller with no second word.
    std::vector<std::vector<float>> fallback(2, std::vector<float>(64, 1.0F));
    stage.apply(fallback, ac3::Acmod::kDualMono, false, ac3::MixLevels{}, 27);
    CHECK(static_cast<double>(fallback[0][0]) ==
          Catch::Approx(ac3::meta::dialnorm_gain(27)).margin(1e-6));
    CHECK(static_cast<double>(fallback[1][0]) ==
          Catch::Approx(ac3::meta::dialnorm_gain(27)).margin(1e-6));
}

TEST_CASE("the LFE joins a fold only when asked, and never against the stream's wishes",
          "[decoder][output]") {
    const auto fold = [](bool mix_lfe, std::optional<double> lfe_level) {
        ac3::OutputStage stage{{.target = ac3::DownmixTarget::kLoRo, .mix_lfe = mix_lfe}};
        std::vector<std::vector<float>> channels(6, std::vector<float>(16, 0.0F));
        channels[5][0] = 1.0F;  // the LFE, and nothing else
        ac3::MixLevels levels;
        levels.lfe_mix_level_db = lfe_level;
        stage.apply(channels, ac3::Acmod::k3_2, true, levels, 31);
        return static_cast<double>(channels[0][0]);
    };
    // §7.8 makes the LFE's contribution optional and this decoder drops it by
    // default: the channel most likely to overload a fold, least likely to be
    // missed.
    CHECK(fold(false, ac3::meta::lfe_mix_level_db(0)) == 0.0);
    // Asked for, with §7.8's stated +10 dB ideal.
    CHECK(fold(true, ac3::meta::lfe_mix_level_db(0)) ==
          Catch::Approx(ac3::meta::lfe_mix_gain(10.0)).margin(1e-6));
    // §E2.3.1.10: an absent lfemixlevcod means the stream DISABLED LFE
    // mixing, which mix_lfe deliberately cannot talk it out of.
    CHECK(fold(true, std::nullopt) == 0.0);
}

TEST_CASE("RF mode holds the fold under its ceiling and says that it did",
          "[decoder][output]") {
    // §7.8.1's normalisation alone cannot overload, so the case worth testing
    // is the one that can: an LFE folded in at +10 dB on top of full-scale
    // fronts. Both claims matter - the ceiling holds, AND the limiter is what
    // held it (silence would satisfy the first claim on its own).
    ac3::OutputStage stage{{.target = ac3::DownmixTarget::kLoRo,
                            .mode = ac3::OperatingMode::kRf,
                            .mix_lfe = true}};
    std::vector<std::vector<float>> channels(6, std::vector<float>(1536, 0.0F));
    for (auto& channel : channels) {
        std::fill(channel.begin(), channel.end(), 0.9F);
    }
    stage.apply(channels, ac3::Acmod::k3_2, true, ac3::MixLevels{}, 31);
    CHECK(peak_of(channels) <= 1.0 + 1e-6);
    CHECK(stage.rf_protection_db() < 0.0);

    // And a fold that never approaches the ceiling is left alone entirely -
    // a limiter that attenuated quiet material would be a bug, not caution.
    ac3::OutputStage quiet{{.target = ac3::DownmixTarget::kLoRo,
                            .mode = ac3::OperatingMode::kRf,
                            .mix_lfe = true}};
    std::vector<std::vector<float>> soft(6, std::vector<float>(1536, 0.01F));
    quiet.apply(soft, ac3::Acmod::k3_2, true, ac3::MixLevels{}, 31);
    CHECK(quiet.rf_protection_db() == Catch::Approx(0.0).margin(1e-9));
}

TEST_CASE("mix_levels resolves both generations' downmix syntax", "[decoder][output]") {
    // AC-3: absent fields take §7.8's own defaults rather than zero, which is
    // the difference between "the stream said nothing" and "the stream said
    // silence".
    const auto ac3_default = ac3::mix_levels(std::nullopt, std::nullopt);
    CHECK(ac3_default.loro_clev == ac3::meta::level::kMinus4_5dB);
    CHECK(ac3_default.loro_slev == ac3::meta::level::kMinus6dB);

    const auto ac3_stated = ac3::mix_levels(ac3::meta::CentreMixLevel::kMinus3dB,
                                            ac3::meta::SurroundMixLevel::kMinus3dB);
    CHECK(ac3_stated.loro_clev == ac3::meta::level::kMinus3dB);
    CHECK(ac3_stated.loro_slev == ac3::meta::level::kMinus3dB);

    // §5.4.2.5's '10' drops the surrounds from the downmix, and it means that
    // for the Lt/Rt fold too - AC-3 has no separate Lt/Rt level to say it
    // with, and putting the channels back would undo a deliberate decision.
    const auto dropped = ac3::mix_levels(ac3::meta::CentreMixLevel::kMinus3dB,
                                         ac3::meta::SurroundMixLevel::kSilent);
    CHECK(dropped.loro_slev == 0.0);
    CHECK(dropped.ltrt_slev == 0.0);

    // E-AC-3: mixmdate carries all four levels separately.
    ac3::meta::MixMetadata mix;
    mix.dmixmod = ac3::meta::DownmixMode::kLtRt;
    mix.lorocmixlev = ac3::meta::MixLevel::kMinus6dB;
    mix.ltrtcmixlev = ac3::meta::MixLevel::kUnity;
    mix.lorosurmixlev = ac3::meta::MixLevel::kMinus6dB;
    mix.ltrtsurmixlev = ac3::meta::MixLevel::kMinus1_5dB;
    mix.lfemixlevcod = 5;
    const auto eac3 = ac3::mix_levels(std::optional{mix});
    CHECK(eac3.loro_clev == ac3::meta::level::kMinus6dB);
    CHECK(eac3.ltrt_clev == ac3::meta::level::kUnity);
    CHECK(eac3.ltrt_slev == ac3::meta::level::kMinus1_5dB);
    CHECK(eac3.preferred == ac3::meta::DownmixMode::kLtRt);
    REQUIRE(eac3.lfe_mix_level_db.has_value());
    CHECK(*eac3.lfe_mix_level_db == Catch::Approx(5.0));

    // An absent lfemixlevcod is a decision, not a missing default.
    mix.lfemixlevcod = std::nullopt;
    CHECK_FALSE(ac3::mix_levels(std::optional{mix}).lfe_mix_level_db.has_value());
    // No mixmdate at all falls back on the AC-3 defaults rather than zero.
    CHECK(ac3::mix_levels(std::optional<ac3::meta::MixMetadata>{}).loro_clev ==
          ac3::meta::level::kMinus4_5dB);

    // Table D2.2's reserved '11' is passed on as sent, not folded into '00'.
    mix.dmixmod = ac3::meta::DownmixMode::kReserved;
    CHECK(ac3::mix_levels(std::optional{mix}).preferred == ac3::meta::DownmixMode::kReserved);
}

TEST_CASE("automatic_stereo_target follows Lt/Rt and folds everything else Lo/Ro",
          "[decoder][output]") {
    // §D3.1.1's automatic selection between the two folds dmixmod can name,
    // at an acmod Table D2.2 defines the field for.
    CHECK(ac3::automatic_stereo_target(ac3::Acmod::k3_2, ac3::meta::DownmixMode::kLtRt) ==
          ac3::DownmixTarget::kLtRt);
    CHECK(ac3::automatic_stereo_target(ac3::Acmod::k3_2, ac3::meta::DownmixMode::kLoRo) ==
          ac3::DownmixTarget::kLoRo);
    // No preference, and the reserved code §D2.3.1.2 lets a decoder read as
    // "not indicated", both take the plain fold.
    CHECK(ac3::automatic_stereo_target(ac3::Acmod::k3_2, ac3::meta::DownmixMode::kNotIndicated) ==
          ac3::DownmixTarget::kLoRo);
    CHECK(ac3::automatic_stereo_target(ac3::Acmod::k3_2, ac3::meta::DownmixMode::kReserved) ==
          ac3::DownmixTarget::kLoRo);
    // A MixLevels nobody filled in says nothing, so it folds Lo/Ro as well.
    CHECK(ac3::automatic_stereo_target(ac3::Acmod::k3_2, ac3::MixLevels{}.preferred) ==
          ac3::DownmixTarget::kLoRo);

    // Table D2.2's own note leaves dmixmod's meaning reserved below acmod
    // 3/0 - at 1+1, 1/0 and 2/0 the field is reserved whatever code it
    // carries, so a preference that would choose Lt/Rt at a wider acmod
    // still folds Lo/Ro at each of these three.
    for (const auto acmod : {ac3::Acmod::kDualMono, ac3::Acmod::k1_0, ac3::Acmod::k2_0}) {
        CHECK(ac3::automatic_stereo_target(acmod, ac3::meta::DownmixMode::kLtRt) ==
              ac3::DownmixTarget::kLoRo);
    }
    // k3_0 is the narrowest acmod the note DOES define the field for - the
    // sharp edge of that boundary, not just one more wide case.
    CHECK(ac3::automatic_stereo_target(ac3::Acmod::k3_0, ac3::meta::DownmixMode::kLtRt) ==
          ac3::DownmixTarget::kLtRt);
}

TEST_CASE("a plain 5.1 layout folds identically through the acmod and the layout forms",
          "[decoder][output]") {
    // The Table E2.5 reduction is an extension beyond §7.8 and has to be an
    // exact identity for every layout §7.8 already covers - otherwise every
    // ordinary E-AC-3 stream would fold differently from the AC-3 of the same
    // programme, which is the one outcome that would make the extension
    // indefensible.
    const std::array<double, 5> hz = {200.0, 300.0, 500.0, 700.0, 1100.0};
    const auto source = tones(hz, 0, 1536);
    const auto layout = ac3::eac3::chanmap::expand(
        ac3::eac3::chanmap::acmod_map(ac3::Acmod::k3_2, false));
    REQUIRE(layout.count == 5);

    for (const auto target :
         {ac3::DownmixTarget::kLoRo, ac3::DownmixTarget::kLtRt, ac3::DownmixTarget::kMono}) {
        auto by_acmod = source;
        ac3::OutputStage plain{{.target = target}};
        plain.apply(by_acmod, ac3::Acmod::k3_2, false, ac3::MixLevels{}, 31);

        auto by_layout = source;
        std::vector<std::span<float>> views;
        for (auto& channel : by_layout) {
            views.emplace_back(channel);
        }
        ac3::OutputStage rendered{{.target = target}};
        rendered.apply(views, layout, ac3::Acmod::k3_2, false, ac3::MixLevels{}, 31);

        INFO("target " << static_cast<int>(target));
        const auto width = by_acmod.size();
        for (std::size_t ch = 0; ch < width; ++ch) {
            for (std::size_t i = 0; i < by_acmod[ch].size(); ++i) {
                REQUIRE(by_acmod[ch][i] == by_layout[ch][i]);
            }
        }
    }
}

TEST_CASE("a 7.1.4 layout folds without discarding its height layer", "[decoder][output]") {
    // §7.8 has no fold for a layout Annex E's chanmap can express, so the
    // reduction seats the extra locations first. What must not happen is the
    // other available answer: dropping every channel §7.8 cannot name, which
    // would silently lose the whole height layer.
    const auto map = static_cast<std::uint16_t>(
        ac3::eac3::chanmap::acmod_map(ac3::Acmod::k3_2, true) | ac3::eac3::chanmap::kLrsRrsBit |
        ac3::eac3::chanmap::kTopQuad);
    const auto layout = ac3::eac3::chanmap::expand(map);
    REQUIRE(layout.count == 12);

    // One height channel carrying signal, everything else silent: if the
    // reduction dropped it, the fold would be silence.
    const int height = layout.index_of(ac3::eac3::chanmap::Location::kVhl);
    REQUIRE(height >= 0);
    std::vector<std::vector<float>> channels(static_cast<std::size_t>(layout.count),
                                             std::vector<float>(64, 0.0F));
    channels[static_cast<std::size_t>(height)][0] = 1.0F;

    std::vector<std::span<float>> views;
    for (auto& channel : channels) {
        views.emplace_back(channel);
    }
    ac3::OutputStage stage{{.target = ac3::DownmixTarget::kLoRo}};
    stage.apply(views, layout, ac3::Acmod::k3_2, true, ac3::MixLevels{}, 31);
    // A front-left height seats left, so it reaches Lo and not Ro.
    CHECK(static_cast<double>(channels[0][0]) > 0.0);
    CHECK(static_cast<double>(channels[1][0]) == Catch::Approx(0.0).margin(1e-9));
}

TEST_CASE("a folded decode of real coded audio keeps every channel's content",
          "[decoder][output]") {
    // The round trip the impulse tests cannot stand in for: real encoded
    // audio over more than three frames, so the MDCT overlap is genuine by
    // the last one, checking that the fold carries each channel's own tone
    // rather than silently dropping one - and that the frame-by-frame stage
    // produces the same thing as folding the whole decode at once, which is
    // what its carried filter state is for.
    ac3::EncoderConfig config;
    config.acmod = ac3::Acmod::k3_2;
    config.lfe = false;
    config.bitrate_kbps = 448;
    const std::array<double, 5> hz = {200.0, 400.0, 800.0, 1600.0, 3200.0};

    ac3::FrameEncoder encoder{config};
    std::vector<std::vector<std::byte>> frames;
    std::uint64_t n0 = 0;
    for (int f = 0; f < 5; ++f) {
        const auto pcm = tones(hz, n0, ac3::kSamplesPerFrame);
        n0 += ac3::kSamplesPerFrame;
        std::vector<std::span<const float>> views;
        for (const auto& channel : pcm) {
            views.emplace_back(channel);
        }
        auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        frames.push_back(std::move(*frame));
    }

    ac3::FrameDecoder folding{
        {.output = {.target = ac3::DownmixTarget::kLoRo, .apply_dialnorm = true}}};
    ac3::FrameDecoder plain;
    ac3::OutputStage after{{.target = ac3::DownmixTarget::kLoRo, .apply_dialnorm = true}};
    for (const auto& frame : frames) {
        const auto in_decoder = folding.decode_frame(frame);
        REQUIRE(in_decoder.has_value());
        REQUIRE(in_decoder->channels.size() == 2);

        auto separate = plain.decode_frame(frame);
        REQUIRE(separate.has_value());
        REQUIRE(separate->channels.size() == 5);
        after.apply(separate->channels, separate->acmod, separate->lfe,
                    ac3::mix_levels(separate->cmixlev, separate->surmixlev),
                    separate->dialnorm);
        REQUIRE(separate->channels.size() == 2);
        for (std::size_t ch = 0; ch < 2; ++ch) {
            for (std::size_t i = 0; i < separate->channels[ch].size(); ++i) {
                REQUIRE(in_decoder->channels[ch][i] == separate->channels[ch][i]);
            }
        }
    }

    // Every channel's tone survives the fold. `hz` is in Table 5.8 coded
    // order, so 200 Hz is L, 400 Hz is C, 800 Hz is R, 1600 Hz is Ls and
    // 3200 Hz is Rs - and Lo takes L, C and Ls but not R, which is what
    // makes the last check below a real claim rather than a restatement.
    const auto last = folding.decode_frame(frames.back());
    REQUIRE(last.has_value());
    const auto energy_at = [&](double target_hz) {
        double real = 0.0;
        double imag = 0.0;
        const auto& pcm = last->channels[0];
        for (std::size_t i = 0; i < pcm.size(); ++i) {
            const double phase =
                2.0 * std::numbers::pi * target_hz * static_cast<double>(i) / 48000.0;
            real += static_cast<double>(pcm[i]) * std::cos(phase);
            imag += static_cast<double>(pcm[i]) * std::sin(phase);
        }
        return std::sqrt(real * real + imag * imag) / static_cast<double>(pcm.size());
    };
    CHECK(energy_at(200.0) > 0.01);    // L, at unity into Lo
    CHECK(energy_at(400.0) > 0.005);   // C, at clev into both
    CHECK(energy_at(1600.0) > 0.002);  // Ls, at slev into Lo only
    // R goes to Ro and not to Lo, so its near-absence here is what says the
    // two outputs are not simply the same sum twice.
    CHECK(energy_at(800.0) < 0.2 * energy_at(200.0));
}

// --- Annex D: the xbsi1 group's own downmix levels ---------------------------
//
// §D3.1.2: once a two-channel downmix is selected, a compliant decoder uses
// the xbsi1 levels for that downmix - ltrtcmixlev/ltrtsurmixlev for Lt/Rt,
// lorocmixlev/lorosurmixlev for Lo/Ro - and without them downmixes as the
// original specification defines.

namespace {

// Four xbsi1 levels that differ from bsi's -3 dB pair in annex_d_config() and
// from §7.8.2's Lt/Rt -3 dB, so a fold that takes a level from the wrong
// place cannot match by coincidence.
ac3::meta::MixMetadata annex_d_levels() {
    return ac3::meta::MixMetadata{
        .dmixmod = ac3::meta::DownmixMode::kLtRt,
        .ltrtcmixlev = ac3::meta::MixLevel::kUnity,
        .lorocmixlev = ac3::meta::MixLevel::kMinus6dB,
        .ltrtsurmixlev = ac3::meta::MixLevel::kMinus6dB,
        .lorosurmixlev = ac3::meta::MixLevel::kMinus1_5dB,
    };
}

// The same levels as coefficients, written out from Tables D2.3-D2.6 rather
// than through mix_levels(), so a wrong conversion there cannot pass by
// agreeing with itself.
const ac3::MixLevels kAnnexDLevels{.loro_clev = ac3::meta::level::kMinus6dB,
                                   .loro_slev = ac3::meta::level::kMinus1_5dB,
                                   .ltrt_clev = ac3::meta::level::kUnity,
                                   .ltrt_slev = ac3::meta::level::kMinus6dB};

// What a decoder that ignored xbsi1 folds this programme with: bsi's two
// -3 dB levels for Lo/Ro and mono, §7.8.2's -3 dB for Lt/Rt.
const ac3::MixLevels kBsiLevels{.loro_clev = ac3::meta::level::kMinus3dB,
                                .loro_slev = ac3::meta::level::kMinus3dB};

// 3/2 at bsid 6: bsi carries -3 dB for both levels, as §D4.2.1 requires a
// bsid-6 encoder to keep sending for legacy decoders, and xbsi1 carries `mix`.
ac3::EncoderConfig annex_d_config(const ac3::meta::MixMetadata& mix) {
    ac3::EncoderConfig config;
    config.acmod = ac3::Acmod::k3_2;
    config.lfe = false;
    config.bitrate_kbps = 448;
    config.cmixlev = ac3::meta::CentreMixLevel::kMinus3dB;
    config.surmixlev = ac3::meta::SurroundMixLevel::kMinus3dB;
    config.alternate_bsi = ac3::meta::AlternateBsi{.mix = mix};
    return config;
}

// `count` syncframes of a distinct tone per channel, as the round-trip test
// above encodes them.
std::vector<std::vector<std::byte>> encode_tones(const ac3::EncoderConfig& config, int count) {
    const std::array<double, 5> hz = {200.0, 400.0, 800.0, 1600.0, 3200.0};
    ac3::FrameEncoder encoder{config};
    std::vector<std::vector<std::byte>> frames;
    std::uint64_t n0 = 0;
    for (int f = 0; f < count; ++f) {
        const auto pcm = tones(hz, n0, ac3::kSamplesPerFrame);
        n0 += ac3::kSamplesPerFrame;
        std::vector<std::span<const float>> views;
        for (const auto& channel : pcm) {
            views.emplace_back(channel);
        }
        auto frame = encoder.encode_frame(views);
        REQUIRE(frame.has_value());
        frames.push_back(std::move(*frame));
    }
    return frames;
}

struct FoldCheck {
    std::size_t matched = 0;    // frames whose fold was the hand fold, sample for sample
    std::size_t concealed = 0;  // frames the folding decoder concealed
};

// Decodes `frames` through a FrameDecoder folding to `target`, and through a
// plain one whose coded channels are then folded by hand with `levels`.
// `concealment` goes to both decoders, so a frame they conceal is concealed
// the same way before either fold sees it.
FoldCheck check_fold(std::span<const std::vector<std::byte>> frames, ac3::DownmixTarget target,
                     const ac3::MixLevels& levels,
                     ac3::ConcealmentPolicy concealment = ac3::ConcealmentPolicy::kNone) {
    ac3::FrameDecoder folding{{.output = {.target = target}, .concealment = concealment}};
    ac3::FrameDecoder plain{{.concealment = concealment}};
    ac3::OutputStage by_hand{{.target = target}};
    FoldCheck check;
    for (const auto& frame : frames) {
        const auto folded = folding.decode_frame(frame);
        auto coded = plain.decode_frame(frame);
        REQUIRE(folded.has_value());
        REQUIRE(coded.has_value());
        by_hand.apply(coded->channels, coded->acmod, coded->lfe, levels, coded->dialnorm);
        if (folded->channels == coded->channels) {
            ++check.matched;
        }
        if (folded->concealed.has_value()) {
            ++check.concealed;
        }
    }
    return check;
}

// Every bit two syncframes of one size disagree on, past crc1 and short of
// crc2, as offsets from the start of the frame.
std::vector<std::size_t> differing_bits(std::span<const std::byte> a,
                                        std::span<const std::byte> b) {
    REQUIRE(a.size() == b.size());
    std::vector<std::size_t> out;
    for (std::size_t byte = 4; byte + 2 < a.size(); ++byte) {
        const auto diff = std::to_integer<unsigned>(a[byte] ^ b[byte]);
        for (std::size_t bit = 0; bit < 8; ++bit) {
            if ((diff & (0x80U >> bit)) != 0) {
                out.push_back(byte * 8 + bit);
            }
        }
    }
    return out;
}

// `frame` with the 3-bit field starting at bit `first` set to `code`, and its
// CRC words re-stamped.
std::vector<std::byte> with_field(std::vector<std::byte> frame, std::size_t first,
                                  unsigned code) {
    for (std::size_t i = 0; i < 3; ++i) {
        const std::size_t bit = first + i;
        const auto mask = static_cast<std::byte>(0x80U >> (bit % 8));
        if (((code >> (2 - i)) & 1U) != 0) {
            frame[bit / 8] |= mask;
        } else {
            frame[bit / 8] &= ~mask;
        }
    }
    REQUIRE(ac3::io::restamp_crc(frame).has_value());
    return frame;
}

}  // namespace

TEST_CASE("mix_levels takes Annex D's xbsi1 levels in place of bsi's", "[decoder][output]") {
    const ac3::meta::AlternateBsi annex_d{.mix = annex_d_levels()};
    // bsi's surmixlev '10' would silence the Lt/Rt surrounds on its own (see
    // the bsid-8 case above); xbsi1 states that level itself, so it is xbsi1's.
    const auto levels = ac3::mix_levels(ac3::Acmod::k3_2, ac3::meta::CentreMixLevel::kMinus3dB,
                                        ac3::meta::SurroundMixLevel::kSilent, annex_d);
    CHECK(levels.loro_clev == kAnnexDLevels.loro_clev);
    CHECK(levels.loro_slev == kAnnexDLevels.loro_slev);
    CHECK(levels.ltrt_clev == kAnnexDLevels.ltrt_clev);
    CHECK(levels.ltrt_slev == kAnnexDLevels.ltrt_slev);
    CHECK(levels.preferred == ac3::meta::DownmixMode::kLtRt);
    // Annex D has no LFE mix level, and an AC-3 fold keeps §7.8's +10 dB
    // ideal. mixmdate's reading of an absent lfemixlevcod as "disabled" is
    // Annex E's rule and does not carry over.
    CHECK(levels.lfe_mix_level_db == ac3::MixLevels{}.lfe_mix_level_db);

    // Table D2.2's note defines dmixmod for 3/0 and wider only.
    CHECK(ac3::mix_levels(ac3::Acmod::k3_0, std::nullopt, std::nullopt, annex_d).preferred ==
          ac3::meta::DownmixMode::kLtRt);
    for (const auto acmod : {ac3::Acmod::kDualMono, ac3::Acmod::k1_0, ac3::Acmod::k2_0}) {
        INFO("acmod " << static_cast<int>(acmod));
        CHECK(ac3::mix_levels(acmod, std::nullopt, std::nullopt, annex_d).preferred ==
              ac3::meta::DownmixMode::kNotIndicated);
    }

    // No xbsi1 group - bsid 8, or bsid 6 with xbsi1e clear - is the bsid-8
    // conversion, field for field, the surmixlev '10' carry-across included.
    const auto same = [](const ac3::MixLevels& a, const ac3::MixLevels& b) {
        return a.loro_clev == b.loro_clev && a.loro_slev == b.loro_slev &&
               a.ltrt_clev == b.ltrt_clev && a.ltrt_slev == b.ltrt_slev &&
               a.lfe_mix_level_db == b.lfe_mix_level_db && a.preferred == b.preferred;
    };
    const auto bsid8 = ac3::mix_levels(ac3::meta::CentreMixLevel::kMinus3dB,
                                       ac3::meta::SurroundMixLevel::kSilent);
    CHECK(same(ac3::mix_levels(ac3::Acmod::k3_2, ac3::meta::CentreMixLevel::kMinus3dB,
                               ac3::meta::SurroundMixLevel::kSilent, std::nullopt),
               bsid8));
    CHECK(same(ac3::mix_levels(ac3::Acmod::k3_2, ac3::meta::CentreMixLevel::kMinus3dB,
                               ac3::meta::SurroundMixLevel::kSilent, ac3::meta::AlternateBsi{}),
               bsid8));
}

TEST_CASE("an Annex D stream folds to Lt/Rt with its own Lt/Rt levels", "[decoder][output]") {
    // Five frames, so the MDCT overlap and the phase shifter's history are
    // both carrying real audio by the end.
    const auto frames = encode_tones(annex_d_config(annex_d_levels()), 5);
    const auto stream = check_fold(frames, ac3::DownmixTarget::kLtRt, kAnnexDLevels);
    CHECK(stream.matched == frames.size());
    // And the levels a decoder ignoring xbsi1 would use give a different fold
    // on every frame, which is what makes the match above mean something.
    CHECK(check_fold(frames, ac3::DownmixTarget::kLtRt, kBsiLevels).matched == 0);
}

TEST_CASE("an Annex D stream folds to Lo/Ro and mono with its own Lo/Ro levels",
          "[decoder][output]") {
    // Mono is included because §7.8.2 defines it as Lo/Ro summed, so it takes
    // lorocmixlev/lorosurmixlev too.
    const auto frames = encode_tones(annex_d_config(annex_d_levels()), 5);
    for (const auto target : {ac3::DownmixTarget::kLoRo, ac3::DownmixTarget::kMono}) {
        INFO("target " << static_cast<int>(target));
        CHECK(check_fold(frames, target, kAnnexDLevels).matched == frames.size());
        CHECK(check_fold(frames, target, kBsiLevels).matched == 0);
    }
}

TEST_CASE("a concealed Annex D frame folds with the last good frame's xbsi1 levels",
          "[decoder][output]") {
    // §7.10's repeat-and-fade reports the last good frame's metadata, so the
    // fold of the frame it reconstructs takes that frame's xbsi1 levels too.
    auto frames = encode_tones(annex_d_config(annex_d_levels()), 6);
    frames[3][frames[3].size() / 2] ^= std::byte{0xFF};  // so its CRC fails
    const auto stream = check_fold(frames, ac3::DownmixTarget::kLtRt, kAnnexDLevels,
                                   ac3::ConcealmentPolicy::kRepeatFade);
    CHECK(stream.concealed == 1);
    CHECK(stream.matched == frames.size());
}

TEST_CASE("xbsi1's reserved surround levels fold as -1.5 dB", "[decoder][output]") {
    // Tables D2.4/D2.6 reserve '000'..'010' for both surround levels, and
    // §D2.3.1.4/§D2.3.1.6 have a decoder use 0.841 for one. The encoder
    // refuses to write a reserved code, so these frames are made the way a
    // third-party stream would arrive. Encoded once at -1.5 dB ('011') and
    // once at -inf ('111'), the two differ in each field's first bit alone,
    // which says where to patch.
    auto mix = annex_d_levels();
    mix.ltrtsurmixlev = ac3::meta::MixLevel::kMinus1_5dB;
    mix.lorosurmixlev = ac3::meta::MixLevel::kMinus1_5dB;
    const auto frames = encode_tones(annex_d_config(mix), 5);
    mix.ltrtsurmixlev = ac3::meta::MixLevel::kSilent;
    mix.lorosurmixlev = ac3::meta::MixLevel::kSilent;
    const auto silent = encode_tones(annex_d_config(mix), 1);
    const auto fields = differing_bits(frames.front(), silent.front());
    // Table D2.1's order: ltrtsurmixlev first, then lorosurmixlev six bits
    // on, past the three of lorocmixlev.
    REQUIRE(fields.size() == 2);
    REQUIRE(fields[1] == fields[0] + 6);

    std::vector<std::vector<std::byte>> reserved;
    for (const auto& frame : frames) {
        reserved.push_back(with_field(with_field(frame, fields[0], 0b000), fields[1], 0b010));
    }

    // The patches moved nothing else: the coded audio is the original's.
    ac3::FrameDecoder original;
    ac3::FrameDecoder patched;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const auto a = original.decode_frame(frames[i]);
        const auto b = patched.decode_frame(reserved[i]);
        REQUIRE(a.has_value());
        REQUIRE(b.has_value());
        const bool same_audio = a->channels == b->channels;
        CHECK(same_audio);
    }

    // The report gives the level a decoder uses in place of each code...
    ac3::FrameDecoder decoder;
    const auto decoded = decoder.decode_frame(reserved.front());
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->alternate_bsi.has_value());
    REQUIRE(decoded->alternate_bsi->mix.has_value());
    CHECK(decoded->alternate_bsi->mix->ltrtsurmixlev == ac3::meta::MixLevel::kMinus1_5dB);
    CHECK(decoded->alternate_bsi->mix->lorosurmixlev == ac3::meta::MixLevel::kMinus1_5dB);

    // ...and both folds use it.
    auto levels = kAnnexDLevels;
    levels.ltrt_slev = ac3::meta::level::kMinus1_5dB;
    levels.loro_slev = ac3::meta::level::kMinus1_5dB;
    for (const auto target : {ac3::DownmixTarget::kLtRt, ac3::DownmixTarget::kLoRo}) {
        INFO("target " << static_cast<int>(target));
        CHECK(check_fold(reserved, target, levels).matched == reserved.size());
    }
}
