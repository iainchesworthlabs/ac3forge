// The mathematical functions of the converter's filter design
// (src/ac4core/include/iclforge/ac4core/dsp/portable_math.hpp): a series or an iteration each, in
// plain double arithmetic with no library call, so that the compiler's constant evaluator, a host
// and a part with a soft-float double all give the same bits. Each is held to its definition, to
// the C library's value to a few units in the last place, and to itself at compile time.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "iclforge/ac4core/dsp/kbd.hpp"
#include "iclforge/ac4core/dsp/portable_math.hpp"

namespace {

namespace portable = iclforge::ac4::detail::dsp::portable;

// The distance of `got` from `want` in units of the last place of `want`.
double ulps(double got, double want) {
    const double magnitude = std::abs(want);
    if (magnitude == 0.0) {
        return got == 0.0 ? 0.0 : std::numeric_limits<double>::infinity();
    }
    const double spacing =
        std::nextafter(magnitude, std::numeric_limits<double>::infinity()) - magnitude;
    return std::abs(got - want) / spacing;
}

std::uint64_t bits(double v) {
    return std::bit_cast<std::uint64_t>(v);
}

// A fixed sequence of pseudo-random numbers in [0, 1): a linear congruential generator, so that
// the arguments are the same on every platform.
struct Sequence {
    std::uint64_t state = 0x9E3779B97F4A7C15ULL;
    double next() {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>(state >> 11U) / 9007199254740992.0;
    }
};

// What these functions are for: at compile time. Each asserts the value its definition gives.
static_assert(portable::ceil(0.0) == 0.0);
static_assert(portable::ceil(2.0) == 2.0);
static_assert(portable::ceil(2.25) == 3.0);
static_assert(portable::ceil(-2.25) == -2.0);
static_assert(portable::ceil(92.6 / 2.0) == 47.0);
static_assert(portable::sqrt(0.0) == 0.0);
static_assert(portable::sqrt(-4.0) == 0.0);
static_assert(portable::sqrt(1.0) == 1.0);
static_assert(portable::sqrt(4.0) == 2.0);
static_assert(portable::sqrt(2.25) == 1.5);
static_assert(portable::sqrt(0.0625) == 0.25);
static_assert(portable::sin(0.0) == 0.0);
static_assert(portable::cos(0.0) == 1.0);
static_assert(portable::sin(-0.5) == -portable::sin(0.5));
static_assert(portable::cos(-0.5) == portable::cos(0.5));
static_assert(iclforge::ac4::detail::dsp::bessel_i0(0.0) == 1.0);

}  // namespace

TEST_CASE("the portable ceil rounds up to a whole number", "[ac4core][dsp][portable]") {
    CHECK(portable::ceil(0.5) == 1.0);
    CHECK(portable::ceil(1.0) == 1.0);
    CHECK(portable::ceil(1.0000000000000002) == 2.0);
    CHECK(portable::ceil(46.5) == 47.0);
    CHECK(portable::ceil(1e15 + 0.5) == 1e15 + 1.0);
    CHECK(portable::ceil(-0.5) == 0.0);
    CHECK(portable::ceil(-3.0) == -3.0);
    CHECK(portable::ceil(-3.5) == -3.0);
    Sequence random;
    for (int i = 0; i < 10000; ++i) {
        const double x = (random.next() - 0.5) * 2000.0;
        CHECK(portable::ceil(x) == std::ceil(x));
    }
}

TEST_CASE("the portable square root is the double nearest the root", "[ac4core][dsp][portable]") {
    // Exact roots.
    for (std::int64_t n = 1; n <= 100000; ++n) {
        const auto root = static_cast<double>(n);
        REQUIRE(portable::sqrt(root * root) == root);
    }
    // The arguments next to a square, and next to a power of four, where the double below the root
    // is half as far away as the double above it and a root can lie a billionth of a unit from a
    // midpoint between two doubles: what the correction step on the exact residual is for.
    for (std::int64_t n = 1; n <= 20000; ++n) {
        const auto square = static_cast<double>(n) * static_cast<double>(n);
        double x = square;
        for (int i = 0; i < 3; ++i) {
            x = std::nextafter(x, 0.0);
        }
        for (int i = 0; i < 7;
             ++i, x = std::nextafter(x, std::numeric_limits<double>::infinity())) {
            REQUIRE(bits(portable::sqrt(x)) == bits(std::sqrt(x)));
        }
    }
    for (int k = -480; k <= 480; ++k) {
        double x = std::ldexp(1.0, 2 * k);
        for (int i = 0; i < 4; ++i) {
            x = std::nextafter(x, 0.0);
        }
        for (int i = 0; i < 9;
             ++i, x = std::nextafter(x, std::numeric_limits<double>::infinity())) {
            REQUIRE(bits(portable::sqrt(x)) == bits(std::sqrt(x)));
        }
    }
    // Arguments over a range of 2^400, and the design's: in [0, 1].
    Sequence random;
    for (int i = 0; i < 200000; ++i) {
        const double x =
            std::ldexp(1.0 + random.next(), static_cast<int>(random.next() * 400.0) - 200);
        REQUIRE(bits(portable::sqrt(x)) == bits(std::sqrt(x)));
        const double unit = random.next();
        REQUIRE(bits(portable::sqrt(unit)) == bits(std::sqrt(unit)));
    }
    // Not a positive number: 0.
    CHECK(portable::sqrt(0.0) == 0.0);
    CHECK(portable::sqrt(-1.0) == 0.0);
}

TEST_CASE(
    "the portable sin and cos keep to the C library's values to a few units in the last place",
    "[ac4core][dsp][portable]") {
    // The classics: the reduction of the double nearest pi and pi / 2 leaves what is left of them.
    CHECK(portable::sin(std::numbers::pi) == 1.2246467991473532e-16);
    CHECK(portable::cos(std::numbers::pi / 2.0) == 6.123233995736766e-17);
    CHECK(portable::sin(std::numbers::pi / 6.0) == Catch::Approx(0.5).margin(1e-16));
    CHECK(portable::cos(std::numbers::pi / 3.0) == Catch::Approx(0.5).margin(1e-16));
    CHECK(portable::sin(1.0) == Catch::Approx(0.8414709848078965).margin(2e-16));
    CHECK(portable::cos(1.0) == Catch::Approx(0.5403023058681398).margin(2e-16));

    // What the design needs: arguments up to 140, in steps no finer than its own; then the range of
    // the reduction (2^20), at arguments of every size.
    double worst_sin = 0.0;
    double worst_cos = 0.0;
    for (int i = -300000; i <= 300000; ++i) {
        const double x = static_cast<double>(i) * 0.001;
        worst_sin = std::max(worst_sin, ulps(portable::sin(x), std::sin(x)));
        worst_cos = std::max(worst_cos, ulps(portable::cos(x), std::cos(x)));
    }
    Sequence random;
    for (int i = 0; i < 200000; ++i) {
        const double x = (random.next() - 0.5) * 2.0 * 1048576.0;
        worst_sin = std::max(worst_sin, ulps(portable::sin(x), std::sin(x)));
        worst_cos = std::max(worst_cos, ulps(portable::cos(x), std::cos(x)));
    }
    CAPTURE(worst_sin, worst_cos);
    CHECK(worst_sin <= 3.0);
    CHECK(worst_cos <= 3.0);

    // sin^2 + cos^2 = 1, and the symmetries.
    for (int i = 0; i < 2000; ++i) {
        const double x = (random.next() - 0.5) * 280.0;
        const double s = portable::sin(x);
        const double c = portable::cos(x);
        CHECK(std::abs(s * s + c * c - 1.0) < 1e-15);
        CHECK(portable::sin(-x) == -s);
        CHECK(portable::cos(-x) == c);
    }
}

TEST_CASE("the portable sin and cos give no number outside the range of their reduction",
          "[ac4core][dsp][portable]") {
    constexpr double kInfinity = std::numeric_limits<double>::infinity();
    CHECK(std::isnan(portable::sin(2097152.0)));
    CHECK(std::isnan(portable::cos(-2097152.0)));
    CHECK(std::isnan(portable::sin(kInfinity)));
    CHECK(std::isnan(portable::cos(-kInfinity)));
    CHECK(std::isnan(portable::sin(std::numeric_limits<double>::quiet_NaN())));
    CHECK(!std::isnan(portable::sin(1048576.0)));
    CHECK(!std::isnan(portable::cos(-1048576.0)));
}

TEST_CASE("the portable functions give at run time the bits they give at compile time",
          "[ac4core][dsp][portable]") {
    // A table of values the compiler's constant evaluator computes, and the same values computed
    // here by the machine the test runs on, each from an argument the optimiser cannot see.
    constexpr std::size_t kCount = 48;
    struct Values {
        std::array<double, kCount> sin{};
        std::array<double, kCount> cos{};
        std::array<double, kCount> sqrt{};
        std::array<double, kCount> i0{};
    };
    constexpr auto at_compile_time = [] {
        Values v;
        for (std::size_t i = 0; i < kCount; ++i) {
            const double x = (static_cast<double>(i) - 24.0) * 5.9137 + 0.0123;
            v.sin[i] = portable::sin(x);
            v.cos[i] = portable::cos(x);
            v.sqrt[i] = portable::sqrt(x * x + 0.5);
            v.i0[i] = iclforge::ac4::detail::dsp::bessel_i0(x / 14.0);
        }
        return v;
    }();
    volatile double step = 5.9137;
    for (std::size_t i = 0; i < kCount; ++i) {
        const double x = (static_cast<double>(i) - 24.0) * step + 0.0123;
        CHECK(bits(portable::sin(x)) == bits(at_compile_time.sin[i]));
        CHECK(bits(portable::cos(x)) == bits(at_compile_time.cos[i]));
        CHECK(bits(portable::sqrt(x * x + 0.5)) == bits(at_compile_time.sqrt[i]));
        CHECK(bits(iclforge::ac4::detail::dsp::bessel_i0(x / 14.0)) == bits(at_compile_time.i0[i]));
    }
}
