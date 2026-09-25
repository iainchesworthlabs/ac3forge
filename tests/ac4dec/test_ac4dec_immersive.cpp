// The immersive element's tools of ETSI TS 103 190-2 V1.3.1 in the decoder
// (src/ac4dec/src/pcm): Table 20's prediction gains (clause 5.2.3.2, step 5),
// S-CPL's Tables 23 and 24 (clause 5.3), the gains the QMF domain applies after
// A-SPX (clauses 4.8.3.11, 4.8.3.14 and 5.4), A-CPL's four modules (clause
// 5.5.2, Table 25 and Pseudocode 2), and A-JCC (clause 5.6): its differential
// decoding and dequantisation, its pre-modification, and its full and core
// reconstructions against Pseudocodes 8 to 14 worked here from their printed
// sums, on known QMF input.

#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "ac4dec/decoder.hpp"
#include "acpl/acpl.hpp"
#include "ajcc/ajcc.hpp"
#include "pcm/acpl.hpp"
#include "pcm/ajcc.hpp"
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

namespace {

namespace acpl = ac4::detail::acpl;
namespace ajcc = ac4::detail::ajcc;

// ajcc_data()'s fourteen parameters, quantised, in syntax order: alpha1,
// alpha2, beta1, beta2, dry1 to dry4, wet1 to wet6. Fine quantisation: alpha
// 0.65 and -0.65 (ibeta 4 both), beta 0.4804688 and 0.2818750, dry 0.3, 0.2,
// 0.4 and 0.0, wet 0.5, 0.3, 0.2, 0.1, 0.4 and -0.2.
constexpr std::array<int, 14> kAjccQ = {20, 12, 3, 2, 9, 8, 10, 6, 25, 23, 22, 21, 24, 18};

// A frame of one parameter set, smooth, every band at kAjccQ.
ac4::detail::AjccFrameValues ajcc_frame(int core_mode) {
    ac4::detail::AjccFrameValues v;
    v.core_mode = core_mode;
    v.num_bands = 15;
    for (std::size_t p = 0; p < kAjccQ.size(); ++p) {
        for (auto& band : v.q[p][0]) {
            band = static_cast<std::int8_t>(kAjccQ[p]);
        }
    }
    return v;
}

// One side's dequantised parameters, by Part 1 Tables 203 and 204 and Part 2
// Pseudocodes 4 and 5 as printed.
struct Side {
    double alpha = 0.0;
    double beta = 0.0;
    double dry1 = 0.0;
    double dry2 = 0.0;
    double wet1 = 0.0;
    double wet2 = 0.0;
    double wet3 = 0.0;
};

Side side_of(std::size_t side) {
    const auto q = [&](std::size_t left, std::size_t right) {
        return kAjccQ[side == 0 ? left : right];
    };
    const acpl::AlphaValue alpha = acpl::dequantise_alpha(q(0, 1), acpl::Quant::kFine);
    return {.alpha = alpha.alpha,
            .beta = acpl::dequantise_beta(q(2, 3), alpha.ibeta, acpl::Quant::kFine),
            .dry1 = q(4, 6) * 0.1 - 0.6,
            .dry2 = q(5, 7) * 0.1 - 0.6,
            .wet1 = q(8, 11) * 0.1 - 2.0,
            .wet2 = q(9, 12) * 0.1 - 2.0,
            .wet3 = q(10, 13) * 0.1 - 2.0};
}

// Pseudocode 8 and 12's decorrelated inputs, worked apart from the stage: one
// decorrelator and ducker per call site.
struct Decorrelated {
    acpl::Decorrelator<double> decorrelator;
    acpl::TransientDucker<double> ducker{};

    std::vector<QmfValue> operator()(const std::vector<QmfValue>& in) {
        std::vector<QmfValue> out(in.size());
        decorrelator.process(in, out, kSlots);
        ducker.process(out, kSlots);
        return out;
    }
};

std::vector<QmfValue> scaled(const std::vector<QmfValue>& x, double gain) {
    std::vector<QmfValue> out(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
        out[i] = gain * x[i];
    }
    return out;
}

// Runs the A-JCC stage for three frames of fresh input at one core mode, and
// checks the second and third, past the first frame's ramp from
// ajcc_param_prev, against `expected`, which works each channel's value from
// the inputs as the pseudocode prints it.
using Channels = std::vector<std::vector<QmfValue>>;
using Expect = std::function<void(std::size_t side, const std::vector<QmfValue>& x0,
                                  const std::vector<QmfValue>& x1, std::array<Decorrelated*, 3> d,
                                  std::array<std::vector<QmfValue>, 5>& z)>;

void check_ajcc(DecodingMode decoding, int core_mode, std::array<Decorrelated, 6>& reference,
                const Expect& expected) {
    const auto speakers = ac4::detail::speakers_of(ac4::detail::ch_mode::k7_0_4, decoding);
    const bool full = decoding == DecodingMode::kFull;
    const double gain = 2.0 + 1.0 / std::numbers::sqrt2;
    ac4::detail::AjccStage stage;
    const ac4::detail::AjccFrameValues values = ajcc_frame(core_mode);
    for (int frame = 0; frame < 3; ++frame) {
        CAPTURE(frame);
        Channels channels(speakers.size(), std::vector<QmfValue>(kValues));
        const std::array<Speaker, 5> core = {S::kLeft, S::kRight, S::kCentre, S::kLeftSurround,
                                             S::kRightSurround};
        for (std::size_t k = 0; k < core.size(); ++k) {
            channels[index_of(speakers, core[k])] =
                matrix(0.5 + 0.1 * static_cast<double>(k),
                       0.13 + 0.07 * static_cast<double>(k) + 0.05 * frame);
        }
        const Channels in = channels;
        std::vector<std::vector<QmfValue>*> matrices;
        for (auto& m : channels) {
            matrices.push_back(&m);
        }
        stage.apply(decoding, values, kSlots, {.speakers = speakers, .matrices = matrices});

        // The expected outputs, each side from its inputs times the gain.
        std::array<std::array<std::vector<QmfValue>, 5>, 2> z;
        for (std::size_t side = 0; side < 2; ++side) {
            const auto x0 = scaled(in[index_of(speakers, side == 0 ? S::kLeft : S::kRight)], gain);
            const auto x1 = scaled(
                in[index_of(speakers, side == 0 ? S::kLeftSurround : S::kRightSurround)], gain);
            const std::size_t first = side * 3;
            expected(side, x0, x1,
                     {&reference[first], &reference[first + 1], &reference[first + 2]}, z[side]);
        }
        if (frame == 0) {
            continue;
        }
        const std::array<std::array<Speaker, 5>, 2> full_out = {
            {{S::kLeft, S::kLeftSurround, S::kLeftBack, S::kTopFrontLeft, S::kTopBackLeft},
             {S::kRight, S::kRightSurround, S::kRightBack, S::kTopFrontRight, S::kTopBackRight}}};
        const std::array<std::array<Speaker, 3>, 2> core_out = {
            {{S::kLeft, S::kLeftSurround, S::kTopSideLeft},
             {S::kRight, S::kRightSurround, S::kTopSideRight}}};
        for (std::size_t i = 0; i < kValues; i += 89) {
            CAPTURE(i);
            CHECK(std::abs(channels[index_of(speakers, S::kCentre)][i] -
                           gain * in[index_of(speakers, S::kCentre)][i]) < 1e-9);
            for (std::size_t side = 0; side < 2; ++side) {
                for (std::size_t o = 0; o < (full ? 5U : 3U); ++o) {
                    CAPTURE(side, o);
                    const Speaker s = full ? full_out[side][o] : core_out[side][o];
                    // Pseudocode 8's sqrt 2 on every output but L, R and C.
                    const double out_gain = full && o > 0 ? kSqrt2 : 1.0;
                    CHECK(std::abs(channels[index_of(speakers, s)][i] - out_gain * z[side][o][i]) <
                          1e-9);
                }
            }
        }
    }
}

std::vector<QmfValue> sum(
    std::initializer_list<std::pair<double, const std::vector<QmfValue>*>> terms) {
    std::vector<QmfValue> out(kValues);
    for (const auto& [w, x] : terms) {
        for (std::size_t i = 0; i < kValues; ++i) {
            out[i] += w * (*x)[i];
        }
    }
    return out;
}

// Pseudocode 8's decorrelators by call site, left then right: D0, D2, D1.
std::array<Decorrelated, 6> full_decorrelators() {
    return {
        Decorrelated{acpl::Decorrelator<double>(0)}, Decorrelated{acpl::Decorrelator<double>(2)},
        Decorrelated{acpl::Decorrelator<double>(1)}, Decorrelated{acpl::Decorrelator<double>(0)},
        Decorrelated{acpl::Decorrelator<double>(2)}, Decorrelated{acpl::Decorrelator<double>(1)}};
}

// Pseudocode 12's: D0 and D2 on each side (the third unused).
std::array<Decorrelated, 6> core_decorrelators() {
    return {
        Decorrelated{acpl::Decorrelator<double>(0)}, Decorrelated{acpl::Decorrelator<double>(2)},
        Decorrelated{acpl::Decorrelator<double>(1)}, Decorrelated{acpl::Decorrelator<double>(0)},
        Decorrelated{acpl::Decorrelator<double>(2)}, Decorrelated{acpl::Decorrelator<double>(1)}};
}

}  // namespace

TEST_CASE("A-JCC's values decode and dequantise by Pseudocodes 3 to 5",
          "[ac4dec][immersive][ajcc]") {
    using ajcc::Kind;
    CHECK(ajcc::dequantise(Kind::kDry, 9, acpl::Quant::kFine) == 9 * 0.1 - 0.6);
    CHECK(ajcc::dequantise(Kind::kDry, 4, acpl::Quant::kCoarse) == 4 * 0.2 - 0.6);
    CHECK(ajcc::dequantise(Kind::kWet, 25, acpl::Quant::kFine) == 25 * 0.1 - 2.0);
    CHECK(ajcc::dequantise(Kind::kWet, 12, acpl::Quant::kCoarse) == 12 * 0.2 - 2.0);
    CHECK(ajcc::quantised_range(Kind::kDry, acpl::Quant::kFine).max == 22);
    CHECK(ajcc::quantised_range(Kind::kWet, acpl::Quant::kCoarse).max == 20);
    CHECK(ajcc::num_param_bands(2) == 9);

    // Along frequency the first value as it is and each next one added to the
    // band below; along time each added to the set before.
    std::array<int, 15> coded{};
    coded[0] = 3;
    coded[1] = 1;
    coded[2] = -2;
    std::array<int, 15> previous{};
    previous.fill(5);
    std::array<int, 15> out{};
    REQUIRE(ajcc::differential_decode({0, 22}, false, 4, coded, previous, out));
    CHECK(out == std::array<int, 15>{3, 4, 2, 2});
    REQUIRE(ajcc::differential_decode({0, 22}, true, 4, coded, previous, out));
    CHECK(out == std::array<int, 15>{8, 6, 3, 5});
    coded[3] = 30;
    CHECK_FALSE(ajcc::differential_decode({0, 22}, false, 4, coded, previous, out));

    // ajcc_values() over an ajcc_data() of codebook indices: F0 and DF along
    // frequency, then DT along time, with each parameter's cb_off.
    ac4::detail::AjccData data;
    data.num_param_bands_id = 3;  // 7 bands
    data.num_bands = 7;
    data.core_mode = 1;
    auto params = [&](std::size_t p) -> ac4::detail::AjccParams& {
        if (p < 2) {
            return data.alpha[p];
        }
        if (p < 4) {
            return data.beta[p - 2];
        }
        if (p < 8) {
            return data.dry[p - 4];
        }
        return data.wet[p - 8];
    };
    const auto type_of = [](std::size_t p) {
        using T = ac4::detail::AjccDataType;
        return p < 2 ? T::kAlpha : p < 4 ? T::kBeta : p < 8 ? T::kDry : T::kWet;
    };
    using ac4::detail::AjccHcbType;
    for (std::size_t p = 0; p < 14; ++p) {
        ac4::detail::AjccParams& param = params(p);
        param.data_type = type_of(p);
        const int f0 = ac4::detail::ajcc_codebook(param.data_type, 0, AjccHcbType::kF0).cb_off;
        const int df = ac4::detail::ajcc_codebook(param.data_type, 0, AjccHcbType::kDf).cb_off;
        param.sets[0].huff_index.fill(static_cast<std::uint16_t>(df));
        param.sets[0].huff_index[0] = static_cast<std::uint16_t>(kAjccQ[p] + f0);
    }
    ac4::detail::AjccQuantHistory history;
    ac4::detail::AjccFrameValues values;
    REQUIRE(ac4::detail::ajcc_values(data, history, values));
    CHECK(values.core_mode == 1);
    CHECK(values.num_bands == 7);
    for (std::size_t p = 0; p < 14; ++p) {
        CAPTURE(p);
        CHECK(values.q[p][0][0] == kAjccQ[p]);
        CHECK(values.q[p][0][6] == kAjccQ[p]);
    }
    // The next frame one step up along time, then out of range.
    for (std::size_t p = 0; p < 14; ++p) {
        ac4::detail::AjccParams& param = params(p);
        const int dt = ac4::detail::ajcc_codebook(param.data_type, 0, AjccHcbType::kDt).cb_off;
        param.sets[0].diff_type = 1;
        param.sets[0].huff_index.fill(static_cast<std::uint16_t>(dt + 1));
    }
    REQUIRE(ac4::detail::ajcc_values(data, history, values));
    CHECK(values.q[4][0][3] == kAjccQ[4] + 1);
    for (int step = 0; step < 40; ++step) {
        if (!ac4::detail::ajcc_values(data, history, values)) {
            SUCCEED("a value left its range");
            return;
        }
    }
    FAIL("no value ever left its range");
}

TEST_CASE("A-JCC's pre-modification follows ajcc_core_mode (Pseudocode 9)",
          "[ac4dec][immersive][ajcc]") {
    ajcc::PreModification<double> pre;
    const std::vector<QmfValue> in1 = matrix(1.0, 0.3);
    const std::vector<QmfValue> in2 = matrix(2.0, 0.5);
    const std::vector<QmfValue> in3 = matrix(3.0, 0.7);
    const std::vector<QmfValue> in4 = matrix(4.0, 0.9);
    std::vector<QmfValue> out1(kValues);
    std::vector<QmfValue> out2(kValues);
    // g at slot ts of each frame: mode 0 from the first frame (1), then to 1
    // (falling), 1 again (0), back to 0 (rising).
    const std::array<int, 4> modes = {0, 1, 1, 0};
    for (std::size_t f = 0; f < modes.size(); ++f) {
        CAPTURE(f);
        pre.process(modes[f], kSlots, in1, in2, in3, in4, out1, out2);
        for (std::size_t ts = 0; ts < static_cast<std::size_t>(kSlots); ++ts) {
            const double step = static_cast<double>(ts + 1) / kSlots;
            const double g = f == 0 ? 1.0 : f == 1 ? 1.0 - step : f == 2 ? 0.0 : step;
            const std::size_t i = ts * 64 + 17;
            CAPTURE(ts);
            CHECK(std::abs(out1[i] - (g * in2[i] + (1.0 - g) * in1[i])) < 1e-12);
            CHECK(std::abs(out2[i] - (g * in4[i] + (1.0 - g) * in3[i])) < 1e-12);
        }
    }
}

TEST_CASE("A-JCC full decoding makes Pseudocode 8's eleven channels from known QMF input",
          "[ac4dec][immersive][ajcc]") {
    const double r = 1.0 / std::numbers::sqrt2;
    SECTION("ajcc_core_mode 0: the core is L, R, C, Ls and Rs") {
        auto reference = full_decorrelators();
        check_ajcc(
            DecodingMode::kFull, 0, reference,
            [&](std::size_t side, const std::vector<QmfValue>& x0, const std::vector<QmfValue>& x1,
                std::array<Decorrelated*, 3> d, std::array<std::vector<QmfValue>, 5>& z) {
                const Side p = side_of(side);
                // Pseudocode 9 at mode 0: w_in is x3in (x1 here).
                const std::vector<QmfValue> y0 = (*d[0])(x0);
                const std::vector<QmfValue> y1 = (*d[1])(x1);
                const std::vector<QmfValue> y2 = (*d[2])(x1);
                z[0] = sum({{(1.0 + p.alpha) / 2.0, &x0}, {p.beta / 2.0, &y0}});
                z[1] = sum(
                    {{p.dry1, &x1}, {(p.wet1 + p.wet3) * r, &y1}, {(p.wet3 + p.wet2) * r, &y2}});
                z[2] = sum({{p.dry2, &x1}, {-p.wet3 * r, &y1}, {-p.wet2 * r, &y2}});
                z[3] = sum({{(1.0 - p.alpha) / 2.0, &x0}, {-p.beta / 2.0, &y0}});
                z[4] = sum({{1.0 - p.dry1 - p.dry2, &x1}, {-p.wet1 * r, &y1}, {-p.wet3 * r, &y2}});
            });
    }
    SECTION("ajcc_core_mode 1: the core is L, R, C, Tfl and Tfr") {
        auto reference = full_decorrelators();
        check_ajcc(
            DecodingMode::kFull, 1, reference,
            [&](std::size_t side, const std::vector<QmfValue>& x0, const std::vector<QmfValue>& x1,
                std::array<Decorrelated*, 3> d, std::array<std::vector<QmfValue>, 5>& z) {
                const Side p = side_of(side);
                // Pseudocode 9 at mode 1: w_in is x0in.
                const std::vector<QmfValue> y0 = (*d[0])(x0);
                const std::vector<QmfValue> y1 = (*d[1])(x0);
                const std::vector<QmfValue> y2 = (*d[2])(x1);
                z[0] = sum(
                    {{p.dry1, &x0}, {(p.wet1 + p.wet3) * r, &y0}, {(p.wet3 + p.wet2) * r, &y1}});
                z[1] = sum({{p.dry2, &x0}, {-p.wet3 * r, &y0}, {-p.wet2 * r, &y1}});
                z[2] = sum({{1.0 - p.dry1 - p.dry2, &x0}, {-p.wet1 * r, &y0}, {-p.wet3 * r, &y1}});
                z[3] = sum({{(1.0 + p.alpha) / 2.0, &x1}, {p.beta / 2.0, &y2}});
                z[4] = sum({{(1.0 - p.alpha) / 2.0, &x1}, {-p.beta / 2.0, &y2}});
            });
    }
}

TEST_CASE("A-JCC core decoding makes Pseudocode 12's seven channels from known QMF input",
          "[ac4dec][immersive][ajcc]") {
    SECTION("ajcc_core_mode 0") {
        auto reference = core_decorrelators();
        check_ajcc(
            DecodingMode::kCore, 0, reference,
            [&](std::size_t side, const std::vector<QmfValue>& x0, const std::vector<QmfValue>& x1,
                std::array<Decorrelated*, 3> d, std::array<std::vector<QmfValue>, 5>& z) {
                const Side p = side_of(side);
                const std::vector<QmfValue> y0 = (*d[0])(x0);
                const std::vector<QmfValue> y1 = (*d[1])(x1);
                const double w = std::sqrt(0.5 * p.wet1 * p.wet1 + 0.5 * p.wet3 * p.wet3);
                z[0] = sum({{(1.0 + p.alpha) / 2.0, &x0}, {p.beta / 2.0, &y0}});
                z[1] = sum({{p.dry1 + p.dry2, &x1}, {-w, &y1}});
                z[2] = sum({{(1.0 - p.alpha) / 2.0, &x0},
                            {1.0 - p.dry1 - p.dry2, &x1},
                            {-p.beta / 2.0, &y0},
                            {w, &y1}});
            });
    }
    SECTION("ajcc_core_mode 1") {
        auto reference = core_decorrelators();
        check_ajcc(
            DecodingMode::kCore, 1, reference,
            [&](std::size_t side, const std::vector<QmfValue>& x0, const std::vector<QmfValue>& x1,
                std::array<Decorrelated*, 3> d, std::array<std::vector<QmfValue>, 5>& z) {
                const Side p = side_of(side);
                const std::vector<QmfValue> y0 = (*d[0])(x0);
                (void)(*d[1])(x1);  // D2 runs on x3in, whose output mode 1 does not weight
                const double front = p.wet1 + p.wet3;
                const double back = p.wet3 + p.wet2;
                const double v = std::sqrt(0.5 * front * front + 0.5 * back * back);
                z[0] = sum({{p.dry1, &x0}, {v, &y0}});
                z[1] = sum({{1.0 - p.dry1, &x0}, {-v, &y0}});
                z[2] = x1;
            });
    }
}
