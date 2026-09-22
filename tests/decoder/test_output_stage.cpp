#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <optional>
#include <span>
#include <vector>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/decoder/decoder.hpp"
#include "ac3/decoder/output.hpp"
#include "ac3/encoder/encoder.hpp"
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
