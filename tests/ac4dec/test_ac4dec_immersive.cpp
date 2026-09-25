// The immersive element's tools of ETSI TS 103 190-2 V1.3.1 in the decoder
// (src/ac4dec/src/pcm): Table 20's prediction gains (clause 5.2.3.2, step 5),
// S-CPL's Tables 23 and 24 (clause 5.3), the gains the QMF domain applies after
// A-SPX (clauses 4.8.3.11, 4.8.3.14 and 5.4), and A-CPL's four modules
// (clause 5.5.2, Table 25 and Pseudocode 2), each on inputs whose outputs can
// be worked by hand.

#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4dec/decoder.hpp"
#include "acpl/acpl.hpp"
#include "pcm/acpl.hpp"
#include "pcm/immersive.hpp"
#include "pcm/routing.hpp"
#include "pcm/stereo.hpp"
#include "syntax/asf.hpp"
#include "syntax/channel_elements.hpp"
#include "syntax/context.hpp"

namespace {

using ac4::DecodingMode;
using ac4::Speaker;
using ac4::detail::QmfValue;
namespace immersive = ac4::detail::immersive_mode;
using S = Speaker;

constexpr double kSqrt2 = std::numbers::sqrt2;
constexpr int kSlots = 32;
constexpr std::size_t kValues = static_cast<std::size_t>(kSlots) * 64;

// The index of `speaker` in `speakers`.
std::size_t index_of(std::span<const Speaker> speakers, Speaker speaker) {
    for (std::size_t c = 0; c < speakers.size(); ++c) {
        if (speakers[c] == speaker) {
            return c;
        }
    }
    FAIL("no such speaker");
    return 0;
}

std::vector<QmfValue> matrix(double scale, double step) {
    std::vector<QmfValue> out(kValues);
    for (std::size_t i = 0; i < kValues; ++i) {
        out[i] = scale * std::polar(1.0, step * static_cast<double>(i));
    }
    return out;
}

}  // namespace

TEST_CASE("Table 20's prediction gains are sap_gain in full SAP's coded bands and 0 elsewhere",
          "[ac4dec][immersive]") {
    ac4::detail::SubstreamContext ctx;
    ac4::detail::SfInfo info;
    info.psy.max_sfb = {8, 0};
    ac4::detail::ChparamInfo chparam;
    chparam.sap_mode = 3;
    // Bands 0 and 1 alpha_q 5, 2 and 3 uncoded, 4 to 7 alpha_q -3 (band 4's
    // difference from band 2, whose alpha_q is 0, and band 6's 0 from band 4).
    for (std::size_t sfb = 0; sfb < 8; ++sfb) {
        chparam.sap_coeff_used[0][sfb] = sfb < 2 || sfb >= 4;
    }
    chparam.dpcm_alpha_q[0][0] = 60 + 5;
    chparam.dpcm_alpha_q[0][4] = 60 - 3;
    chparam.dpcm_alpha_q[0][6] = 60;
    const auto gain = [](int alpha_q) {
        return static_cast<double>(static_cast<float>(alpha_q) * 0.1f);
    };
    using ac4::detail::StereoUse;
    const auto prediction =
        ac4::detail::stereo_parameters(ctx, info, chparam, StereoUse::kPrediction);
    const std::array<double, 8> expected = {gain(5),  gain(5),  0.0,      0.0,
                                            gain(-3), gain(-3), gain(-3), gain(-3)};
    for (std::size_t sfb = 0; sfb < 8; ++sfb) {
        CAPTURE(sfb);
        const auto [a, b, c, d] = prediction.abcd[0][sfb];
        CHECK(a == 1.0);
        CHECK(b == 0.0);
        CHECK(c == expected[sfb]);
        CHECK(d == 1.0);
    }
    // The same chparam_info() as a 2 x 2 step: Pseudocode 59's (1 + g, 1, 1 - g, -1).
    const auto pair = ac4::detail::stereo_parameters(ctx, info, chparam);
    CHECK(pair.abcd[0][0] == std::array{1.0 + gain(5), 1.0, 1.0 - gain(5), -1.0});
    CHECK(pair.abcd[0][2] == std::array{1.0, 0.0, 0.0, 1.0});

    // No other sap_mode predicts: M/S bands included, a'_j is 0.
    for (const int mode : {0, 1, 2}) {
        CAPTURE(mode);
        chparam.sap_mode = mode;
        chparam.ms_used[0].fill(true);
        const auto none =
            ac4::detail::stereo_parameters(ctx, info, chparam, StereoUse::kPrediction);
        for (std::size_t sfb = 0; sfb < 8; ++sfb) {
            CHECK(none.abcd[0][sfb] == std::array{1.0, 0.0, 0.0, 1.0});
        }
    }
}

TEST_CASE("S-CPL makes the channels of Tables 23 and 24", "[ac4dec][immersive]") {
    const auto full = ac4::detail::speakers_of(ac4::detail::ch_mode::k7_1_4);
    const auto core = ac4::detail::speakers_of(ac4::detail::ch_mode::k7_1_4, DecodingMode::kCore);
    // Channel c holds the constant c + 1: A'' to K'' in the channels
    // pcm/routing.hpp gives them.
    const auto signals = [](std::size_t count) {
        std::vector<std::vector<double>> time(count);
        for (std::size_t c = 0; c < count; ++c) {
            time[c].assign(16, static_cast<double>(c + 1));
        }
        return time;
    };
    for (const int mode : {immersive::kScpl, immersive::kAspxScpl}) {
        CAPTURE(mode);
        const double c_gain = mode == immersive::kScpl ? 2.0 : 1.0;
        const double m_gain = mode == immersive::kScpl ? kSqrt2 : 1.0;
        std::vector<std::vector<double>> time = signals(full.size());
        const std::vector<std::vector<double>> in = time;
        ac4::detail::apply_scpl(mode, DecodingMode::kFull, full, time);
        const auto at = [&](const std::vector<std::vector<double>>& t, Speaker s) {
            return t[index_of(full, s)][7];
        };
        for (const Speaker front : {S::kLeft, S::kRight, S::kCentre}) {
            CHECK(at(time, front) == c_gain * at(in, front));
        }
        CHECK(at(time, S::kLfe) == at(in, S::kLfe));
        const std::array<std::array<Speaker, 2>, 4> coupled = {
            {{S::kLeftSurround, S::kLeftBack},
             {S::kRightSurround, S::kRightBack},
             {S::kTopFrontLeft, S::kTopBackLeft},
             {S::kTopFrontRight, S::kTopBackRight}}};
        for (const auto& [x, y] : coupled) {
            // Ls = m_gain (D'' + H''), Lb = m_gain (D'' - H''), and alike.
            CHECK(std::abs(at(time, x) - m_gain * (at(in, x) + at(in, y))) < 1e-12);
            CHECK(std::abs(at(time, y) - m_gain * (at(in, x) - at(in, y))) < 1e-12);
        }

        // Core decoding: c_gain on the seven core channels, the LFE as it is.
        std::vector<std::vector<double>> core_time = signals(core.size());
        ac4::detail::apply_scpl(mode, DecodingMode::kCore, core, core_time);
        for (std::size_t c = 0; c < core.size(); ++c) {
            CAPTURE(c);
            const double gain = core[c] == S::kLfe ? 1.0 : c_gain;
            CHECK(core_time[c][3] == gain * static_cast<double>(c + 1));
        }
    }
    // Nothing in the modes without S-CPL.
    std::vector<std::vector<double>> time = signals(full.size());
    const std::vector<std::vector<double>> in = time;
    ac4::detail::apply_scpl(immersive::kAspxAcpl2, DecodingMode::kFull, full, time);
    CHECK(time == in);
}

TEST_CASE("the immersive element's gains after A-SPX follow Tables 9 and 10 and clause 4.8.3.14",
          "[ac4dec][immersive]") {
    using ac4::detail::immersive_gains;
    const auto gains = [](int mode, DecodingMode decoding, Speaker speaker) {
        const auto g = immersive_gains(mode, decoding, speaker);
        return std::array{g.low, g.high};
    };
    constexpr auto kFull = DecodingMode::kFull;
    constexpr auto kCore = DecodingMode::kCore;
    // Table 10: 2 for L, C and R, the square root of 2 for the coupled pairs.
    for (const Speaker s : {S::kLeft, S::kRight, S::kCentre}) {
        CHECK(gains(immersive::kAspxScpl, kFull, s) == std::array{2.0, 2.0});
    }
    for (const Speaker s :
         {S::kLeftSurround, S::kLeftBack, S::kRightSurround, S::kRightBack, S::kTopFrontLeft,
          S::kTopBackLeft, S::kTopFrontRight, S::kTopBackRight}) {
        CHECK(gains(immersive::kAspxScpl, kFull, s) == std::array{kSqrt2, kSqrt2});
    }
    // Core decoding in ASPX_SCPL: 2 everywhere, and Table 9's channels 0.841395
    // from sbx on.
    for (const Speaker s :
         {S::kLeftSurround, S::kRightSurround, S::kTopSideLeft, S::kTopSideRight}) {
        CHECK(gains(immersive::kAspxScpl, kCore, s) == std::array{2.0, 2.0 * 0.841395});
    }
    CHECK(gains(immersive::kAspxScpl, kCore, S::kLeft) == std::array{2.0, 2.0});
    // Core decoding in the A-CPL modes: 2 in place of A-CPL; full decoding's
    // A-CPL applies its own.
    for (const int mode : {immersive::kAspxAcpl1, immersive::kAspxAcpl2}) {
        CHECK(gains(mode, kCore, S::kTopSideRight) == std::array{2.0, 2.0});
        CHECK(gains(mode, kFull, S::kLeft) == std::array{1.0, 1.0});
    }
    // Never the LFE, and nothing in SCPL.
    CHECK(gains(immersive::kAspxAcpl2, kCore, S::kLfe) == std::array{1.0, 1.0});
    CHECK(gains(immersive::kScpl, kFull, S::kLeft) == std::array{1.0, 1.0});

    // apply_band_gains splits each slot at sbx.
    std::vector<QmfValue> m(kValues, QmfValue{1.0, -1.0});
    ac4::detail::apply_band_gains(m, kSlots, 20, {.low = 2.0, .high = 3.0});
    CHECK(m[5 * 64 + 19] == QmfValue{2.0, -2.0});
    CHECK(m[5 * 64 + 20] == QmfValue{3.0, -3.0});
}

TEST_CASE("A-CPL's four immersive modules take Table 25's channels and Pseudocode 2's gains",
          "[ac4dec][immersive]") {
    const auto speakers = ac4::detail::speakers_of(ac4::detail::ch_mode::k7_0_4);
    const auto run = [&](int mode, const ac4::detail::AcplFrameValues& values,
                         ac4::detail::AcplStage& stage,
                         std::vector<std::vector<QmfValue>>& channels) {
        std::vector<std::vector<QmfValue>*> matrices;
        for (auto& m : channels) {
            matrices.push_back(&m);
        }
        stage.apply(ac4::detail::ch_mode::k7_0_4, false, ac4::detail::ElementKind::kImmersive, mode,
                    values, kSlots, {.speakers = speakers, .matrices = matrices});
    };
    // Each channel its own signal.
    const auto inputs = [&]() {
        std::vector<std::vector<QmfValue>> channels;
        for (std::size_t c = 0; c < speakers.size(); ++c) {
            channels.push_back(
                matrix(1.0 + 0.25 * static_cast<double>(c), 0.1 + 0.03 * static_cast<double>(c)));
        }
        return channels;
    };
    const std::array<std::array<Speaker, 2>, 4> pairs = {{{S::kLeftSurround, S::kLeftBack},
                                                          {S::kRightSurround, S::kRightBack},
                                                          {S::kTopFrontLeft, S::kTopBackLeft},
                                                          {S::kTopFrontRight, S::kTopBackRight}}};

    SECTION("ASPX_ACPL_2 at alpha 1 and -1, beta 0, and ASPX_ACPL_1's residuals") {
        // Modules 1 and 3 alpha 1 (all in the first channel), 2 and 4 alpha -1
        // (all in the second); the second frame, past the ramp from
        // acpl_param_prev.
        ac4::detail::AcplFrameValues values;
        values.module_count = 4;
        for (std::size_t m = 0; m < 4; ++m) {
            values.modules[m].num_bands = 15;
            for (auto& band : values.modules[m].alpha[0]) {
                band = m % 2 == 0 ? 1.0 : -1.0;
            }
        }
        ac4::detail::AcplStage stage;
        std::vector<std::vector<QmfValue>> channels = inputs();
        const std::vector<std::vector<QmfValue>> in = channels;
        run(immersive::kAspxAcpl2, values, stage, channels);
        channels = in;
        run(immersive::kAspxAcpl2, values, stage, channels);
        const auto value = [&](const std::vector<std::vector<QmfValue>>& t, Speaker s,
                               std::size_t i) { return t[index_of(speakers, s)][i]; };
        for (std::size_t i = 0; i < kValues; i += 131) {
            CAPTURE(i);
            // z0, z2 and z4: L, R and C doubled.
            for (const Speaker front : {S::kLeft, S::kRight, S::kCentre}) {
                CHECK(std::abs(value(channels, front, i) - 2.0 * value(in, front, i)) < 1e-12);
            }
            for (std::size_t m = 0; m < 4; ++m) {
                // x_in = 2 x, and every output times the square root of 2.
                const QmfValue full = 2.0 * kSqrt2 * value(in, pairs[m][0], i);
                const QmfValue first = m % 2 == 0 ? full : QmfValue{};
                const QmfValue second = m % 2 == 0 ? QmfValue{} : full;
                CHECK(std::abs(value(channels, pairs[m][0], i) - first) < 1e-12);
                CHECK(std::abs(value(channels, pairs[m][1], i) - second) < 1e-12);
            }
        }

        // ASPX_ACPL_1 below acpl_qmf_band: (x + r, x - r), the residual r in
        // the second channel, each doubled and times the square root of 2.
        for (std::size_t m = 0; m < 4; ++m) {
            values.modules[m].qmf_band = 64;
        }
        ac4::detail::AcplStage residual;
        channels = in;
        run(immersive::kAspxAcpl1, values, residual, channels);
        for (std::size_t i = 0; i < kValues; i += 131) {
            CAPTURE(i);
            for (const auto& [x, r] : pairs) {
                CHECK(std::abs(value(channels, x, i) -
                               kSqrt2 * (value(in, x, i) + value(in, r, i))) < 1e-12);
                CHECK(std::abs(value(channels, r, i) -
                               kSqrt2 * (value(in, x, i) - value(in, r, i))) < 1e-12);
            }
        }
    }

    SECTION("the decorrelators are D0, D0, D1 and D1, one instance each") {
        // alpha 0 and beta 1: z0 = (x_in + y) / 2, z1 = (x_in - y) / 2, so
        // z0 - z1 = y, the decorrelated and ducked x_in, times the square root
        // of 2. Ls and Rs carry the same signal, as do Tfl and Tfr: a shared
        // instance would filter the second from the first's history.
        ac4::detail::AcplFrameValues values;
        values.module_count = 4;
        for (auto& module : values.modules) {
            module.num_bands = 15;
            for (auto& band : module.beta[0]) {
                band = 1.0;
            }
        }
        ac4::detail::AcplStage stage;
        std::array<ac4::detail::acpl::Decorrelator<double>, 2> reference = {
            ac4::detail::acpl::Decorrelator<double>(0), ac4::detail::acpl::Decorrelator<double>(1)};
        std::array<ac4::detail::acpl::TransientDucker<double>, 2> duckers{};
        for (int frame = 0; frame < 3; ++frame) {
            CAPTURE(frame);
            std::vector<std::vector<QmfValue>> channels = inputs();
            const std::vector<QmfValue> surround = matrix(0.8, 0.21 + 0.1 * frame);
            const std::vector<QmfValue> top = matrix(0.6, 0.47 + 0.1 * frame);
            for (const Speaker s : {S::kLeftSurround, S::kRightSurround}) {
                channels[index_of(speakers, s)] = surround;
            }
            for (const Speaker s : {S::kTopFrontLeft, S::kTopFrontRight}) {
                channels[index_of(speakers, s)] = top;
            }
            run(immersive::kAspxAcpl2, values, stage, channels);
            std::array<std::vector<QmfValue>, 2> y;
            for (std::size_t d = 0; d < 2; ++d) {
                std::vector<QmfValue> x_in(kValues);
                const std::vector<QmfValue>& source = d == 0 ? surround : top;
                for (std::size_t i = 0; i < kValues; ++i) {
                    x_in[i] = 2.0 * source[i];
                }
                y[d].resize(kValues);
                reference[d].process(x_in, y[d], kSlots);
                duckers[d].process(y[d], kSlots);
            }
            if (frame == 0) {
                continue;  // beta ramps from acpl_param_prev
            }
            for (std::size_t i = 0; i < kValues; i += 97) {
                CAPTURE(i);
                for (std::size_t m = 0; m < 4; ++m) {
                    const QmfValue difference = channels[index_of(speakers, pairs[m][0])][i] -
                                                channels[index_of(speakers, pairs[m][1])][i];
                    CHECK(std::abs(difference - kSqrt2 * y[m < 2 ? 0 : 1][i]) < 1e-9);
                }
            }
        }
    }
}
