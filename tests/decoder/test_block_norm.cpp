// The fixed-point tier's block exponent (src/forge/src/decoder/block_norm.hpp):
// the exponent a stream is stored under, the bounds the tools lower it by,
// the exact AHT exponent, the on-demand renormalisation, and the overlap-add
// that aligns two exponents and applies the result exactly.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

#include "block_norm.hpp"
#include "fixed32.hpp"

using ac3::internal::Fixed32;
namespace bn = ac3::internal;

TEST_CASE("the store exponent follows the smallest coded exponent, one bit down", "[fixed32]") {
    CHECK(bn::store_norm(bn::kNoExponent) == 0);
    CHECK(bn::store_norm(5) == 4);
    CHECK(bn::store_norm(0) == -1);
    CHECK(bn::store_norm(1) == 0);
    CHECK(bn::store_norm(24) == 23);
    CHECK(bn::store_norm(30) == bn::kNormCeiling);
    CHECK(bn::store_norm(-9) == bn::kNormFloor);
    const std::vector<std::uint8_t> exps{7, 3, 9, 12};
    CHECK(bn::min_exponent(exps, 0, 4) == 3);
    CHECK(bn::min_exponent(exps, 2, 4) == 9);
    CHECK(bn::min_exponent(exps, 3, 3) == bn::kNoExponent);
    const std::vector<int> wide{bn::kNoExponent, 11, bn::kNoExponent};
    CHECK(bn::min_exponent(wide, 0, 3) == 11);
    CHECK(bn::coupled_exponent(bn::kNoExponent, 4) == bn::kNoExponent);
    CHECK(bn::coupled_exponent(10, 7) == 14);
    CHECK(bn::raw_width(0) == 0);
    CHECK(bn::raw_width(1) == 1);
    CHECK(bn::raw_width(Fixed32::kOne) == 25);
    CHECK(bn::raw_width(Fixed32::kOne / 2) == 24);
    CHECK(bn::raw_width(Fixed32::kOne / 2 - 1) == 23);
}

TEST_CASE("an AHT bin's effective exponent comes from its reconstructed peak", "[fixed32]") {
    std::array<Fixed32, 6> blocks{};
    CHECK(bn::aht_effective_exponent(blocks, 9) == bn::kNoExponent);
    blocks[2] = Fixed32{0.75};  // in [0.5, 1): the coded exponent holds
    CHECK(bn::aht_effective_exponent(blocks, 9) == 9);
    blocks[4] = Fixed32{-1.5};  // above unity: one bit less
    CHECK(bn::aht_effective_exponent(blocks, 9) == 8);
    blocks.fill(Fixed32{0.3});  // below one half: one bit more
    CHECK(bn::aht_effective_exponent(blocks, 9) == 10);
    // The floating tiers keep the coded exponent.
    std::array<double, 6> wide{};
    wide.fill(1.5);
    CHECK(bn::aht_effective_exponent(wide, 9) == 9);
}

TEST_CASE("the spectral extension room comes from the copy source's peak", "[fixed32]") {
    std::array<Fixed32, 256> coeffs{};
    CHECK(bn::spx_room(coeffs, 10, 40, 3) == 0);  // nothing to copy
    coeffs[20] = Fixed32::from_raw(1 << 22);      // width 23
    // 23 + 1 + 5 - e against the 23 bits the format has below one half.
    CHECK(bn::spx_room(coeffs, 10, 40, 6) == 0);
    CHECK(bn::spx_room(coeffs, 10, 40, 5) == 1);
    CHECK(bn::spx_room(coeffs, 10, 40, 3) == 3);
    CHECK(bn::spx_room(coeffs, 10, 40, bn::kNoExponent) == 0);
    // Outside the copy source, the peak does not count.
    CHECK(bn::spx_room(coeffs, 21, 40, 0) == 0);
    std::array<double, 256> wide{};
    wide[20] = 0.25;
    CHECK(bn::spx_room(wide, 10, 40, 0) == 0);
}

TEST_CASE("renormalise shifts a stream down to a lower exponent and not up", "[fixed32]") {
    std::array<Fixed32, 256> coeffs{};
    coeffs[0] = Fixed32{0.25};
    coeffs[1] = Fixed32{-0.25};
    coeffs[2] = Fixed32::from_raw(3);  // rounds half up on the way down
    int norm = 6;
    bn::renormalise(coeffs, norm, 8);  // higher: nothing happens
    CHECK(norm == 6);
    CHECK(coeffs[0].raw == Fixed32{0.25}.raw);
    bn::renormalise(coeffs, norm, 4);
    CHECK(norm == 4);
    CHECK(coeffs[0].raw == Fixed32{0.0625}.raw);
    CHECK(coeffs[1].raw == Fixed32{-0.0625}.raw);
    CHECK(coeffs[2].raw == 1);  // 3/4 rounds to 1
    std::array<double, 256> wide{};
    wide[0] = 0.25;
    int wide_norm = 0;
    bn::renormalise(wide, wide_norm, -3);
    CHECK(wide_norm == 0);
    CHECK(wide[0] == 0.25);
}

TEST_CASE("the overlap-add aligns two exponents and applies the result exactly", "[fixed32]") {
    std::mt19937 rng(0x0a1d);
    std::uniform_real_distribution<double> dist(-0.4, 0.4);
    for (const auto& [x_norm, delay_norm] : {std::pair{3, 3}, std::pair{2, 9}, std::pair{11, 4}}) {
        std::array<double, 512> x_true{};
        std::array<double, 256> delay_true{};
        std::array<Fixed32, 512> x{};
        std::array<Fixed32, 256> delay{};
        for (std::size_t i = 0; i < 512; ++i) {
            x[i] = Fixed32{dist(rng)};
            x_true[i] = std::ldexp(static_cast<double>(x[i]), -x_norm);
        }
        for (std::size_t i = 0; i < 256; ++i) {
            delay[i] = Fixed32{dist(rng)};
            delay_true[i] = std::ldexp(static_cast<double>(delay[i]), -delay_norm);
        }
        std::array<float, 256> pcm{};
        int history_norm = delay_norm;
        bn::overlap_add_normalised(x, delay, history_norm, x_norm, pcm);
        // The sum sits under the smaller exponent, so the quieter half gives
        // up the bits below that scale: within a raw unit of it, twice.
        const double tolerance = 2.0 * std::ldexp(1.0, -24 - std::min(x_norm, delay_norm));
        for (std::size_t i = 0; i < 256; ++i) {
            const double expected = 2.0 * (x_true[i] + delay_true[i]);
            CHECK_THAT(static_cast<double>(pcm[i]),
                       Catch::Matchers::WithinAbs(expected, tolerance + 1e-7 * std::abs(expected)));
        }
        CHECK(history_norm == x_norm);
        for (std::size_t i = 0; i < 256; ++i) {
            CHECK(delay[i].raw == x[i + 256].raw);
        }
    }
    // A hostile sum clips to the format rather than wrapping.
    std::array<Fixed32, 512> loud{};
    std::array<Fixed32, 256> louder{};
    loud.fill(Fixed32{100});
    louder.fill(Fixed32{100});
    std::array<float, 256> pcm{};
    int history_norm = 0;
    bn::overlap_add_normalised(loud, louder, history_norm, 0, pcm);
    CHECK(pcm[0] > 0.0F);
    CHECK_THAT(static_cast<double>(pcm[0]), Catch::Matchers::WithinRel(2.0 * 128.0, 1e-6));
    // The floating tiers' instantiation is the plain loop.
    std::array<double, 512> xw{};
    std::array<double, 256> dw{};
    xw[0] = 0.25;
    dw[0] = 0.125;
    xw[256] = 0.5;
    std::array<float, 256> pcm_wide{};
    int wide_norm = 0;
    bn::overlap_add_normalised(xw, dw, wide_norm, 0, pcm_wide);
    CHECK(pcm_wide[0] == 0.75F);
    CHECK(dw[0] == 0.5);
}

TEST_CASE("a fresh delay half and a widened value", "[fixed32]") {
    constexpr auto norms = bn::fresh_delay_norms<6>();
    for (const int n : norms) {
        CHECK(n == bn::kNormCeiling);
    }
    constexpr auto slots = bn::fresh_delay_norm_slots<3, 6>();
    CHECK(slots[2][5] == bn::kNormCeiling);
    CHECK(bn::widen_stored(Fixed32{0.25}, 2) == 0.0625);
    CHECK(bn::widen_stored(Fixed32{-0.5}, -1) == -1.0);
    CHECK(bn::widen_stored(0.25, 2) == 0.25);
    CHECK(bn::widen_stored(0.25F, 2) == 0.25);
}
