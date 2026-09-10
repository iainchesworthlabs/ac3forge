// The fixed-point tier's scalar (src/forge/src/core/fixed32.hpp): the
// arithmetic rules it states, held exactly; the square root against the
// double one; and the two shared tables the decoders read through it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>

#include "ac3/core/coupling.hpp"
#include "ac3/core/eac3_tools.hpp"
#include "ac3/core/exponents.hpp"
#include "ac3/core/mantissas.hpp"
#include "fixed32.hpp"

using ac3::internal::Fixed32;

namespace {

constexpr double kUlp = 1.0 / 16777216.0;  // one raw unit

}  // namespace

TEST_CASE("Fixed32 constructs from integers and floating values exactly where it can",
          "[fixed32]") {
    STATIC_CHECK(Fixed32{0}.raw == 0);
    STATIC_CHECK(Fixed32{1}.raw == Fixed32::kOne);
    STATIC_CHECK(Fixed32{2}.raw == 2 * Fixed32::kOne);
    STATIC_CHECK(Fixed32{-1}.raw == -Fixed32::kOne);
    STATIC_CHECK(Fixed32{0.5}.raw == Fixed32::kOne / 2);
    STATIC_CHECK(Fixed32{-0.5}.raw == -Fixed32::kOne / 2);
    STATIC_CHECK(Fixed32{0.5F}.raw == Fixed32::kOne / 2);
    // Half a raw unit rounds away from zero.
    STATIC_CHECK(Fixed32{kUlp * 0.5}.raw == 1);
    STATIC_CHECK(Fixed32{-kUlp * 0.5}.raw == -1);
    STATIC_CHECK(Fixed32{kUlp * 0.49}.raw == 0);
    // Saturation: 128 is one past the format.
    STATIC_CHECK(Fixed32{128}.raw == std::numeric_limits<std::int32_t>::max());
    STATIC_CHECK(Fixed32{-129}.raw == std::numeric_limits<std::int32_t>::min());
    STATIC_CHECK(Fixed32{1e9}.raw == std::numeric_limits<std::int32_t>::max());
    STATIC_CHECK(static_cast<std::size_t>(1536) > 127);
    STATIC_CHECK(Fixed32{static_cast<std::size_t>(1536)}.raw ==
                 std::numeric_limits<std::int32_t>::max());
    CHECK(static_cast<double>(Fixed32{0.75}) == 0.75);
    CHECK(static_cast<float>(Fixed32{-0.25}) == -0.25F);
    CHECK(static_cast<int>(Fixed32{2.75}) == 2);
    CHECK(static_cast<int>(Fixed32{-2.75}) == -2);
}

TEST_CASE("Fixed32 from a double is the arithmetic definition, on the bits", "[fixed32]") {
    // The constructor works on the double's bits; this is the arithmetic it
    // stands for - value x 2^24, rounded half away from zero, saturated -
    // evaluated here in long double so the check itself carries no rounding.
    std::mt19937_64 rng(0xd0b1);
    std::uniform_real_distribution<double> wide(-127.9, 127.9);
    std::uniform_real_distribution<double> small(-1e-3, 1e-3);
    std::uniform_real_distribution<double> tiny(-1e-7, 1e-7);
    const auto reference = [](double v) -> std::int32_t {
        const long double scaled = static_cast<long double>(v) * 16777216.0L;
        const long double rounded = scaled >= 0 ? std::floor(scaled + 0.5L) : std::ceil(scaled - 0.5L);
        if (rounded > 2147483647.0L) {
            return std::numeric_limits<std::int32_t>::max();
        }
        if (rounded < -2147483648.0L) {
            return std::numeric_limits<std::int32_t>::min();
        }
        return static_cast<std::int32_t>(rounded);
    };
    for (int i = 0; i < 30000; ++i) {
        const double v = i % 3 == 0 ? small(rng) : i % 3 == 1 ? tiny(rng) : wide(rng);
        CHECK(Fixed32{v}.raw == reference(v));
    }
    // Exact ties, both signs, and the format's edges.
    for (const double v : {0.5 * kUlp, -0.5 * kUlp, 1.5 * kUlp, -1.5 * kUlp, 0.25 * kUlp,
                           127.0, -128.0, 127.99999994, 1e-300, -1e-300, 1e300, -1e300}) {
        CHECK(Fixed32{v}.raw == reference(v));
    }
    STATIC_CHECK(Fixed32{std::numeric_limits<double>::infinity()}.raw ==
                 std::numeric_limits<std::int32_t>::max());
    STATIC_CHECK(Fixed32{-std::numeric_limits<double>::infinity()}.raw ==
                 std::numeric_limits<std::int32_t>::min());
    STATIC_CHECK(Fixed32{-0.0}.raw == 0);
    STATIC_CHECK(Fixed32{5e-324}.raw == 0);  // the smallest denormal
}

TEST_CASE("Fixed32 from a float is the double route's value, bit for bit", "[fixed32]") {
    // The float constructor works on the float's own bits; the double one is
    // the arithmetic definition. Same rounding (half away from zero), same
    // saturation, and zero below the floor.
    std::mt19937 rng(0xf10a);
    std::uniform_real_distribution<float> wide(-127.9F, 127.9F);
    std::uniform_real_distribution<float> small(-1e-3F, 1e-3F);
    std::uniform_real_distribution<float> tiny(-1e-7F, 1e-7F);
    for (int i = 0; i < 30000; ++i) {
        const float v = i % 3 == 0 ? small(rng) : i % 3 == 1 ? tiny(rng) : wide(rng);
        CHECK(Fixed32{v}.raw == Fixed32{static_cast<double>(v)}.raw);
    }
    STATIC_CHECK(Fixed32{0.0F}.raw == 0);
    STATIC_CHECK(Fixed32{-0.0F}.raw == 0);
    STATIC_CHECK(Fixed32{1e-30F}.raw == 0);
    STATIC_CHECK(Fixed32{200.0F}.raw == std::numeric_limits<std::int32_t>::max());
    STATIC_CHECK(Fixed32{-200.0F}.raw == std::numeric_limits<std::int32_t>::min());
    STATIC_CHECK(Fixed32{std::numeric_limits<float>::infinity()}.raw ==
                 std::numeric_limits<std::int32_t>::max());
    STATIC_CHECK(Fixed32{-std::numeric_limits<float>::infinity()}.raw ==
                 std::numeric_limits<std::int32_t>::min());
    const auto nan = Fixed32{std::numeric_limits<float>::quiet_NaN()}.raw;
    CHECK((nan == std::numeric_limits<std::int32_t>::max() ||
           nan == std::numeric_limits<std::int32_t>::min()));
    // Half a raw unit rounds away from zero, as the double route does.
    STATIC_CHECK(Fixed32{0.5F / 16777216.0F}.raw == 1);
    STATIC_CHECK(Fixed32{-0.5F / 16777216.0F}.raw == -1);
    STATIC_CHECK(Fixed32{0.25F / 16777216.0F}.raw == 0);
    STATIC_CHECK(Fixed32{127.0F}.raw == 127 * Fixed32::kOne);
}

TEST_CASE("Fixed32 sums wrap and products round half up and saturate", "[fixed32]") {
    STATIC_CHECK((Fixed32{0.25} + Fixed32{0.5}).raw == Fixed32{0.75}.raw);
    STATIC_CHECK((Fixed32{0.25} - Fixed32{0.5}).raw == Fixed32{-0.25}.raw);
    STATIC_CHECK((-Fixed32{0.25}).raw == Fixed32{-0.25}.raw);
    // Wrap, not UB: the largest value plus one raw unit is the smallest.
    STATIC_CHECK((Fixed32::from_raw(std::numeric_limits<std::int32_t>::max()) +
                  Fixed32::from_raw(1)).raw == std::numeric_limits<std::int32_t>::min());
    // Products: exact where the result is representable.
    STATIC_CHECK((Fixed32{0.5} * Fixed32{0.5}).raw == Fixed32{0.25}.raw);
    STATIC_CHECK((Fixed32{-0.5} * Fixed32{0.5}).raw == Fixed32{-0.25}.raw);
    STATIC_CHECK((Fixed32{3} * Fixed32{4}).raw == Fixed32{12}.raw);
    // One rounding rule: the product's discarded half rounds up.
    // 2^-24 * 0.5 = 2^-25, half a raw unit -> rounds to one raw unit.
    STATIC_CHECK((Fixed32::from_raw(1) * Fixed32{0.5}).raw == 1);
    // -2^-25 rounds half UP, i.e. toward +inf: to zero.
    STATIC_CHECK((Fixed32::from_raw(-1) * Fixed32{0.5}).raw == 0);
    // Saturation on a product that leaves the format.
    STATIC_CHECK((Fixed32{100} * Fixed32{100}).raw == std::numeric_limits<std::int32_t>::max());
    STATIC_CHECK((Fixed32{-100} * Fixed32{100}).raw == std::numeric_limits<std::int32_t>::min());
    // Division: exact for powers of two, truncating otherwise, saturating on zero.
    STATIC_CHECK((Fixed32{1} / Fixed32{2}).raw == Fixed32{0.5}.raw);
    STATIC_CHECK((Fixed32{1} / Fixed32{3}).raw == Fixed32::kOne / 3);
    STATIC_CHECK((Fixed32{1} / Fixed32{0}).raw == std::numeric_limits<std::int32_t>::max());
    STATIC_CHECK((Fixed32{-1} / Fixed32{0}).raw == std::numeric_limits<std::int32_t>::min());
    // Comparisons are the raw ones.
    STATIC_CHECK(Fixed32{-0.5} < Fixed32{0.25});
    STATIC_CHECK(Fixed32{0.25} == Fixed32{0.25});
    STATIC_CHECK(ac3::internal::abs(Fixed32{-0.75}).raw == Fixed32{0.75}.raw);
    // Powers of two: shifts, with the saturation at the top and the floor at
    // the bottom.
    STATIC_CHECK(Fixed32{0.75}.scaled_by_pow2(1).raw == Fixed32{1.5}.raw);
    STATIC_CHECK(Fixed32{0.75}.scaled_by_pow2(-1).raw == Fixed32{0.375}.raw);
    STATIC_CHECK(Fixed32{1}.scaled_by_pow2(-24).raw == 1);
    // Half a raw unit rounds up, the product's rule; below half rounds to 0.
    STATIC_CHECK(Fixed32{1}.scaled_by_pow2(-25).raw == 1);
    STATIC_CHECK(Fixed32{1}.scaled_by_pow2(-26).raw == 0);
    STATIC_CHECK(Fixed32{-1}.scaled_by_pow2(-25).raw == 0);
    STATIC_CHECK(Fixed32{0.75}.scaled_by_pow2(-40).raw == 0);
    STATIC_CHECK(Fixed32{100}.scaled_by_pow2(3).raw == std::numeric_limits<std::int32_t>::max());
    STATIC_CHECK(ac3::internal::scalar_ldexp(Fixed32{0.5}, 2).raw == Fixed32{2}.raw);
}

TEST_CASE("Fixed32 square root is the rounded-down root of the double value", "[fixed32]") {
    std::mt19937 rng(0x51ed);
    std::uniform_real_distribution<double> dist(0.0, 127.0);
    for (int i = 0; i < 5000; ++i) {
        const double v = i < 100 ? i * 1e-6 : dist(rng);
        const Fixed32 x{v};
        const double truth = std::sqrt(static_cast<double>(x));
        const double got = static_cast<double>(ac3::internal::scalar_sqrt(x));
        CHECK(got <= truth + 1e-12);
        CHECK(got > truth - kUlp - 1e-12);
    }
    STATIC_CHECK(ac3::internal::scalar_sqrt(Fixed32{4}).raw == Fixed32{2}.raw);
    STATIC_CHECK(ac3::internal::scalar_sqrt(Fixed32{0.25}).raw == Fixed32{0.5}.raw);
    STATIC_CHECK(ac3::internal::scalar_sqrt(Fixed32{0}).raw == 0);
    STATIC_CHECK(ac3::internal::scalar_sqrt(Fixed32{-1}).raw == 0);
    STATIC_CHECK(ac3::internal::scalar_sqrt(Fixed32{127}).raw ==
                 Fixed32{11.269427669584644}.raw);
}

TEST_CASE("the exponent scale and the mantissa tables read exactly through Fixed32",
          "[fixed32]") {
    for (int exp = 0; exp <= 24; ++exp) {
        CHECK(ac3::exponent_scale<Fixed32>(exp).raw == (Fixed32::kOne >> exp));
    }
    for (int exp = 25; exp < 32; ++exp) {
        CHECK(ac3::exponent_scale<Fixed32>(exp).raw == 0);  // below the format's floor
    }
    // Every symmetric and asymmetric reconstruction value within a raw unit of
    // the double one: the table is the double division rounded once.
    for (int bap = 1; bap <= 15; ++bap) {
        const int codes = bap <= 5 ? ac3::kSymmetricLevels[static_cast<std::size_t>(bap)]
                                   : 1 << ac3::kBapBits[static_cast<std::size_t>(bap)];
        for (int code = 0; code < codes; ++code) {
            const double wide = ac3::dequantize_mantissa_as<double>(static_cast<std::uint32_t>(code), bap);
            const double fixed = static_cast<double>(
                ac3::dequantize_mantissa_as<Fixed32>(static_cast<std::uint32_t>(code), bap));
            CHECK(std::abs(fixed - wide) <= kUlp);
        }
    }
    // A coupling coordinate, likewise.
    const ac3::coupling::Coordinate coordinate{.exp = 3, .mant = 9};
    const double wide = ac3::coupling::decode_coordinate_as<double>(coordinate, 1);
    const double fixed = static_cast<double>(ac3::coupling::decode_coordinate_as<Fixed32>(coordinate, 1));
    CHECK(std::abs(fixed - wide) <= 2 * kUlp);
}

TEST_CASE("the noise generators draw in Fixed32 from the same state sequence", "[fixed32]") {
    ac3::DitherGenerator dither;
    ac3::DitherGenerator dither_wide;
    for (int i = 0; i < 100; ++i) {
        const Fixed32 f = dither.next_as<Fixed32>();
        const double d = dither_wide.next_as<double>();
        CHECK(dither.state == dither_wide.state);  // the same sequence
        CHECK(std::abs(static_cast<double>(f)) <= 0.707 + kUlp);
        // The fixed mapping reads the top 24 bits of the same state, so it
        // sits within a 2^-24 unit-interval step of the double one, scaled.
        CHECK(std::abs(static_cast<double>(f) - d) < 2.0 * 0.707 * 2.0 / 16777216.0 + 2 * kUlp);
    }
    ac3::eac3::SpxNoise spx;
    for (int i = 0; i < 100; ++i) {
        const Fixed32 f = spx.next_as<Fixed32>();
        CHECK(std::abs(static_cast<double>(f)) <= 1.7320508075688772 + kUlp);
    }
    STATIC_CHECK(Fixed32::unit_from_state(0xFFFFFFFFU).raw == Fixed32::kOne - 1);
    STATIC_CHECK(Fixed32::unit_from_state(0).raw == 0);
    STATIC_CHECK(ac3::eac3::ecpl_rand_notrans_as<Fixed32>(0, 0) >= Fixed32{-1});
    STATIC_CHECK(ac3::eac3::ecpl_rand_notrans_as<Fixed32>(0, 0) < Fixed32{1});
}
