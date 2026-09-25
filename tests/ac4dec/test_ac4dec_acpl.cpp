// The decoder's A-CPL stage (src/ac4dec/src/pcm/acpl.hpp): differential
// decoding and dequantisation of parsed acpl_data_1ch() and acpl_data_2ch()
// across frames (ETSI TS 103 190-1 V1.4.1 clause 5.7.7.7), and the modules of
// Pseudocodes 115 to 119 on QMF matrices where the answer can be worked by
// hand: steep interpolation between two parameter sets, and ASPX_ACPL_3's
// centre channel from gamma5 and gamma6.

#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ac4dec/decoder.hpp"
#include "pcm/acpl.hpp"
#include "syntax/acpl.hpp"
#include "syntax/channel_elements.hpp"

namespace {

using ac4::Speaker;
using ac4::detail::AcplData1ch;
using ac4::detail::AcplDataType;
using ac4::detail::AcplFrameValues;
using ac4::detail::AcplParams;
using ac4::detail::AcplQuantHistory;
using ac4::detail::ChannelElement;
using ac4::detail::ElementKind;
using ac4::detail::QmfValue;
using Catch::Approx;
namespace codec_mode = ac4::detail::codec_mode;

constexpr int kSlots = 32;
constexpr std::size_t kValues = static_cast<std::size_t>(kSlots) * 64;

// One parameter set of `bands` bands: along frequency, the first band's F0
// index and DF indices at cb_off (no change) after it; along time, DT
// indices of cb_off plus `delta`.
AcplParams params(AcplDataType type, int bands, int f0_index, int df_off, bool diff_time, int dt_index) {
    AcplParams p;
    p.data_type = type;
    p.quant_mode = 0;
    p.sets[0].diff_type = diff_time ? 1 : 0;
    for (int i = 0; i < bands; ++i) {
        const auto index = static_cast<std::uint16_t>(diff_time ? dt_index : (i == 0 ? f0_index : df_off));
        p.sets[0].huff_index[static_cast<std::size_t>(i)] = index;
    }
    return p;
}

ChannelElement pair_element(const AcplData1ch& data) {
    ChannelElement e;
    e.kind = ElementKind::kPair;
    e.codec_mode = codec_mode::kAspxAcpl2;
    e.acpl_1ch = {data};
    return e;
}

std::vector<QmfValue> matrix(double scale) {
    std::vector<QmfValue> out(kValues);
    for (std::size_t i = 0; i < kValues; ++i) {
        out[i] = scale * std::polar(1.0, 0.37 * static_cast<double>(i));
    }
    return out;
}

}  // namespace

TEST_CASE("acpl_values decodes alpha and beta along frequency, then along time from the frame before",
          "[ac4dec][acpl]") {
    // Fine alpha: F0 has cb_off 0, DF and DT 32; fine beta: F0 0, DF and DT 8.
    AcplData1ch data;
    data.framing.num_param_sets = 1;
    data.num_bands = 7;
    data.alpha1 = params(AcplDataType::kAlpha, 7, 24, 32, false, 0);  // alpha_q 24 in every band: 1.0
    data.beta1 = params(AcplDataType::kBeta, 7, 2, 8, false, 0);      // beta_q 2 at ibeta 8: 0.1375
    AcplQuantHistory history;
    AcplFrameValues values;
    REQUIRE(ac4::detail::acpl_values(pair_element(data), history, values));
    REQUIRE(values.module_count == 1);
    for (std::size_t pb = 0; pb < 7; ++pb) {
        CHECK(values.modules[0].alpha[0][pb] == 1.0);
        CHECK(values.modules[0].beta[0][pb] == 0.1375);
    }
    // The next frame adds 1 to every alpha_q along time: 25, 1.059375, ibeta 7.
    data.alpha1 = params(AcplDataType::kAlpha, 7, 0, 0, true, 33);
    data.beta1 = params(AcplDataType::kBeta, 7, 0, 0, true, 8);
    REQUIRE(ac4::detail::acpl_values(pair_element(data), history, values));
    CHECK(values.modules[0].alpha[0][6] == 1.059375);
    CHECK(values.modules[0].beta[0][6] == 0.1619922);
    // Eight more steps of 1 leave the table (32 is its last value).
    for (int step = 0; step < 7; ++step) {
        REQUIRE(ac4::detail::acpl_values(pair_element(data), history, values));
    }
    CHECK(values.modules[0].alpha[0][0] == 2.0);
    const auto refused = ac4::detail::acpl_values(pair_element(data), history, values);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().error == ac4::DecodeError::kInvalidStream);
}

TEST_CASE("acpl_values leaves the bands below acpl_param_band at 0", "[ac4dec][acpl]") {
    AcplData1ch data;
    data.framing.num_param_sets = 1;
    data.num_bands = 15;
    data.start_band = 4;
    data.qmf_band = 4;
    data.alpha1 = params(AcplDataType::kAlpha, 15, 8, 32, false, 0);  // -1.0 from band 4
    data.beta1 = params(AcplDataType::kBeta, 15, 0, 8, false, 0);
    // The parser leaves the bands below start_band unsent, index 0.
    for (std::size_t i = 0; i < 4; ++i) {
        data.alpha1.sets[0].huff_index[i] = 0;
        data.beta1.sets[0].huff_index[i] = 0;
    }
    data.alpha1.sets[0].huff_index[4] = 8;
    ChannelElement e = pair_element(data);
    e.codec_mode = codec_mode::kAspxAcpl1;
    AcplQuantHistory history;
    AcplFrameValues values;
    REQUIRE(ac4::detail::acpl_values(e, history, values));
    CHECK(values.modules[0].qmf_band == 4);
    CHECK(values.modules[0].alpha[0][3] == 0.0);
    CHECK(values.modules[0].alpha[0][4] == -1.0);
    CHECK(values.modules[0].alpha[0][14] == -1.0);
}

TEST_CASE("acpl_values dequantises acpl_data_2ch()'s beta3 and gammas by their steps", "[ac4dec][acpl]") {
    ac4::detail::AcplData2ch data;
    data.framing.num_param_sets = 1;
    data.num_bands = 9;
    for (auto& alpha : data.alpha) {
        alpha = params(AcplDataType::kAlpha, 9, 16, 32, false, 0);
    }
    for (auto& beta : data.beta) {
        beta = params(AcplDataType::kBeta, 9, 0, 8, false, 0);
    }
    data.beta3 = params(AcplDataType::kBeta3, 9, 12, 16, false, 0);  // 12 steps of 0.125
    for (std::size_t k = 0; k < 6; ++k) {
        // Fine gamma: F0 has cb_off 20, DF 40; gamma_q -3 + k.
        data.gamma[k] = params(AcplDataType::kGamma, 9, 17 + static_cast<int>(k), 40, false, 0);
    }
    ChannelElement e;
    e.kind = ElementKind::k5X;
    e.codec_mode = codec_mode::kAspxAcpl3;
    e.acpl_2ch = data;
    AcplQuantHistory history;
    AcplFrameValues values;
    REQUIRE(ac4::detail::acpl_values(e, history, values));
    REQUIRE(values.coupling.has_value());
    CHECK(values.coupling->beta3[0][8] == 1.5);
    for (std::size_t k = 0; k < 6; ++k) {
        CHECK(values.coupling->gamma[k][0][8] == (-3.0 + static_cast<double>(k)) * 1638.0 / 16384.0);
    }
}

TEST_CASE("steep interpolation switches a pair between its outputs at each set's time slot", "[ac4dec][acpl]") {
    // ASPX_ACPL_2 with two parameter sets: alpha 1 from slot 8 and -1 from
    // slot 20, beta 0; acpl_param_prev 0 (alpha 0) before slot 8.
    ac4::detail::AcplModuleValues module;
    module.framing = {.steep = true, .num_param_sets = 2, .param_timeslot = {8, 20}};
    module.num_bands = 15;
    for (std::size_t pb = 0; pb < 15; ++pb) {
        module.alpha[0][pb] = 1.0;
        module.alpha[1][pb] = -1.0;
    }
    AcplFrameValues values;
    values.modules[0] = module;
    values.module_count = 1;
    std::vector<QmfValue> left = matrix(1.0);
    const std::vector<QmfValue> x0 = left;
    std::vector<QmfValue> right(kValues);
    const std::array<Speaker, 2> speakers = {Speaker::kLeft, Speaker::kRight};
    const std::array<std::vector<QmfValue>*, 2> matrices = {&left, &right};
    ac4::detail::AcplStage stage;
    stage.apply(ac4::detail::ch_mode::kStereo, false, ElementKind::kPair, codec_mode::kAspxAcpl2, values, kSlots,
                {.speakers = speakers, .matrices = matrices});
    for (std::size_t ts = 0; ts < static_cast<std::size_t>(kSlots); ++ts) {
        for (const std::size_t sb : {0U, 30U, 63U}) {
            const std::size_t i = ts * 64 + sb;
            CAPTURE(ts, sb);
            if (ts < 8) {  // alpha 0: both the downmix
                CHECK(std::abs(left[i] - x0[i]) < 1e-12);
                CHECK(std::abs(right[i] - x0[i]) < 1e-12);
            } else if (ts < 20) {  // alpha 1: all in L
                CHECK(std::abs(left[i] - 2.0 * x0[i]) < 1e-12);
                CHECK(std::abs(right[i]) < 1e-12);
            } else {  // alpha -1: all in R
                CHECK(std::abs(left[i]) < 1e-12);
                CHECK(std::abs(right[i] - 2.0 * x0[i]) < 1e-12);
            }
        }
    }
}

TEST_CASE("ASPX_ACPL_3 makes the centre of gamma5 and gamma6", "[ac4dec][acpl]") {
    // Only gamma5 and gamma6 set, at 0.5: C = sqrt 2 (1 + sqrt 2) (x0 + x1) / 2
    // (Pseudocode 118's ACplModule2() on (z4, z5) and its sqrt 2), and the
    // other four channels 0. Two frames, so that the second has no ramp from
    // acpl_param_prev.
    ac4::detail::AcplCouplingValues coupling;
    coupling.num_bands = 7;
    for (std::size_t pb = 0; pb < 7; ++pb) {
        coupling.gamma[4][0][pb] = 0.5;
        coupling.gamma[5][0][pb] = 0.5;
    }
    AcplFrameValues values;
    values.coupling = coupling;
    const std::array<Speaker, 5> speakers = {Speaker::kLeft, Speaker::kRight, Speaker::kCentre,
                                             Speaker::kLeftSurround, Speaker::kRightSurround};
    ac4::detail::AcplStage stage;
    const double gain = std::numbers::sqrt2 * (1.0 + std::numbers::sqrt2) * 0.5;
    for (int frame = 0; frame < 2; ++frame) {
        std::vector<QmfValue> l = matrix(1.0);
        std::vector<QmfValue> r = matrix(0.5);
        const std::vector<QmfValue> x0 = l;
        const std::vector<QmfValue> x1 = r;
        std::vector<QmfValue> c(kValues);
        std::vector<QmfValue> ls(kValues);
        std::vector<QmfValue> rs(kValues);
        const std::array<std::vector<QmfValue>*, 5> matrices = {&l, &r, &c, &ls, &rs};
        stage.apply(ac4::detail::ch_mode::k5_0, false, ElementKind::k5X, codec_mode::kAspxAcpl3, values, kSlots,
                    {.speakers = speakers, .matrices = matrices});
        if (frame == 0) {
            continue;
        }
        for (std::size_t i = 0; i < kValues; i += 97) {
            CAPTURE(i);
            CHECK(std::abs(c[i] - gain * (x0[i] + x1[i])) < 1e-12);
            CHECK(std::abs(l[i]) < 1e-12);
            CHECK(std::abs(r[i]) < 1e-12);
            CHECK(std::abs(ls[i]) < 1e-12);
            CHECK(std::abs(rs[i]) < 1e-12);
        }
    }
}
