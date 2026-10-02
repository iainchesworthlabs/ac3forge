// MantExp (src/arithmetic/include/iclforge/arithmetic/mant_exp.hpp), the mantissa and power of
// two the AC-4 decoder's fixed-point tier carries its energies, gains and scale factors in
// (planning/ac4.md, D14d): its representation and rounding held exactly, and its arithmetic,
// root, log2 and exp2 held to double within its thirty-bit mantissa.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>

#include "iclforge/arithmetic/fixed32.hpp"
#include "iclforge/arithmetic/mant_exp.hpp"

using iclforge::internal::Fixed32;
using iclforge::internal::MantExp;

namespace {

// A result within `units` of 2^-29 of the double's magnitude: one rounding of a thirty-bit
// mantissa is half of 2^-29 relative at worst.
bool near(MantExp got, double want, double units) {
    const double tolerance = units * std::ldexp(std::abs(want), -29);
    return std::abs(got.to_double() - want) <= tolerance;
}

double random_value(std::mt19937& rng) {
    std::uniform_real_distribution<double> mantissa(0.5, 1.0);
    std::uniform_int_distribution<int> exponent(-80, 80);
    std::bernoulli_distribution negative(0.5);
    const double v = std::ldexp(mantissa(rng), exponent(rng));
    return negative(rng) ? -v : v;
}

}  // namespace

TEST_CASE("MantExp holds one normalised representation of each value", "[ac4][fixed][mant_exp]") {
    STATIC_CHECK(MantExp{}.is_zero());
    STATIC_CHECK(MantExp{0}.is_zero());
    STATIC_CHECK(MantExp{1}.m == (1 << 29));
    STATIC_CHECK(MantExp{1}.e == 1);
    STATIC_CHECK(MantExp{-1}.m == -(1 << 29));
    STATIC_CHECK(MantExp{0.75}.m == 3 * (1 << 28));
    STATIC_CHECK(MantExp{0.75}.e == 0);
    STATIC_CHECK(MantExp{1.0} == MantExp{1});
    STATIC_CHECK(MantExp{3}.scaled_by_pow2(-5) == MantExp{3.0 / 32.0});
    STATIC_CHECK(MantExp{Fixed32{0.5}} == MantExp{0.5});
    // Thirty bits: 1 + 2^-29 is held, 1 + 2^-31 rounds to 1, and 1 + 2^-30, a half, away from it.
    STATIC_CHECK(MantExp{1.0 + 0x1p-29}.m == (1 << 29) + 1);
    STATIC_CHECK(MantExp{1.0 + 0x1p-31} == MantExp{1});
    STATIC_CHECK(MantExp{1.0 + 0x1p-30}.m == (1 << 29) + 1);
    STATIC_CHECK(MantExp{-(1.0 + 0x1p-30)}.m == -((1 << 29) + 1));
}

TEST_CASE("MantExp's arithmetic is double's to its mantissa", "[ac4][fixed][mant_exp]") {
    std::mt19937 rng(20261002);
    for (int i = 0; i < 20000; ++i) {
        const double a = random_value(rng);
        const double b = random_value(rng);
        const MantExp x{a};
        const MantExp y{b};
        // Each operand is a's or b's double rounded to thirty bits, which the tolerances allow for.
        CHECK(near(x * y, a * b, 3));
        CHECK(near(x / y, a / b, 3));
        if (std::abs(a + b) > std::ldexp(std::max(std::abs(a), std::abs(b)), -8)) {
            CHECK(near(x + y, a + b, 2 * 256 + 2));
            CHECK(near(x - (-y), a + b, 2 * 256 + 2));
        }
        CHECK((x < y) == (a < b));
        CHECK((x == y) == (a == b));
        CHECK(near(scalar_sqrt(abs(x)), std::sqrt(std::abs(a)), 2));
    }
    CHECK((MantExp{5} + MantExp{-5}).is_zero());
    CHECK(MantExp{1} + MantExp{1}.scaled_by_pow2(-100) == MantExp{1});
    CHECK((MantExp{3} / MantExp{}).e == MantExp::kMaxExponent);
    CHECK(scalar_sqrt(MantExp{-4}).is_zero());
    CHECK(scalar_sqrt(MantExp{4}) == MantExp{2});
}

TEST_CASE("MantExp's log2 and exp2 are double's to a few units of 2^-30", "[ac4][fixed][mant_exp]") {
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> argument(-60.0, 60.0);
    for (int i = 0; i < 5000; ++i) {
        const double x = argument(rng);
        const MantExp e = scalar_exp2(MantExp{x});
        CHECK(near(e, std::exp2(x), 64.0 + std::abs(x) * 8.0));
        const double positive = std::exp2(x);
        const MantExp l = scalar_log2(MantExp{positive});
        CHECK(std::abs(l.to_double() - std::log2(positive)) <= 8.0 * std::ldexp(1.0, -30) + std::ldexp(std::abs(x), -29));
    }
    CHECK(scalar_exp2(MantExp{}) == MantExp{1});
    CHECK(scalar_exp2(MantExp{10}) == MantExp{1024});
    CHECK(scalar_log2(MantExp{1024}) == MantExp{10});
    CHECK(scalar_log2(MantExp{}) == MantExp::largest(true));
}

TEST_CASE("MantExp converts to Fixed32 by rounding half away from zero, saturating", "[ac4][fixed][mant_exp]") {
    STATIC_CHECK(MantExp{0.5}.to_fixed().raw == Fixed32::kOne / 2);
    STATIC_CHECK(MantExp{-3}.to_fixed().raw == -3 * Fixed32::kOne);
    STATIC_CHECK(MantExp{0x1p-25}.to_fixed().raw == 1);
    STATIC_CHECK(MantExp{-0x1p-25}.to_fixed().raw == -1);
    STATIC_CHECK(MantExp{0x1p-26}.to_fixed().raw == 0);
    STATIC_CHECK(MantExp{200}.to_fixed().raw == std::numeric_limits<std::int32_t>::max());
    STATIC_CHECK(MantExp{-200}.to_fixed().raw == std::numeric_limits<std::int32_t>::min());
    std::mt19937 rng(11);
    std::uniform_int_distribution<std::int32_t> raw(std::numeric_limits<std::int32_t>::min() / 2,
                                                    std::numeric_limits<std::int32_t>::max() / 2);
    for (int i = 0; i < 10000; ++i) {
        const Fixed32 f = Fixed32::from_raw(raw(rng));
        CHECK(MantExp{f}.to_fixed() == f);
    }
}
