// The float encode path's own log2 / log / exp (src/forge/src/encoder/
// scalar_math.hpp) against libm's double forms: accuracy bounds sized to what
// the callers quantise to, exactness at the powers of two the analyses lean
// on, and the double overloads being libm itself.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstdint>
#include <limits>

#include "scalar_math.hpp"

using ac3::internal::scalar_exp;
using ac3::internal::scalar_log;
using ac3::internal::scalar_log2;

TEST_CASE("float scalar_log2 tracks std::log2 across the normal range", "[encoder][scalar_math]") {
    // Log-spaced sweep over ~60 decades, with a prime step so the mantissa
    // is well covered rather than landing on the same few patterns.
    double worst_abs = 0.0;
    double worst_rel = 0.0;
    for (int i = -3700; i <= 3700; i += 7) {
        const auto x = static_cast<float>(std::pow(10.0, i / 100.0));
        const double reference = std::log2(static_cast<double>(x));
        const double got = static_cast<double>(scalar_log2(x));
        const double abs_err = std::abs(got - reference);
        worst_abs = std::max(worst_abs, abs_err);
        if (std::abs(reference) > 1.0) {
            worst_rel = std::max(worst_rel, abs_err / std::abs(reference));
        }
    }
    CHECK(worst_abs < 2e-5);
    CHECK(worst_rel < 2e-6);
}

TEST_CASE("float scalar_log2 is exact at powers of two", "[encoder][scalar_math]") {
    for (int k = -120; k <= 120; ++k) {
        const float x = std::ldexp(1.0f, k);
        CHECK(scalar_log2(x) == static_cast<float>(k));
    }
}

TEST_CASE("float scalar_log2 accepts subnormal input", "[encoder][scalar_math]") {
    const float tiny = 1e-40f;
    REQUIRE(tiny > 0.0f);
    REQUIRE(tiny < std::numeric_limits<float>::min());
    CHECK_THAT(scalar_log2(tiny), Catch::Matchers::WithinAbs(std::log2(1e-40), 1e-4));
}

TEST_CASE("float scalar_log is ln 2 times scalar_log2", "[encoder][scalar_math]") {
    for (const float x : {1e-30f, 1e-9f, 0.001f, 0.5f, 1.0f, 3.0f, 1e6f, 3e30f}) {
        CHECK_THAT(scalar_log(x), Catch::Matchers::WithinRel(std::log(static_cast<double>(x)), 3e-6));
    }
    CHECK(scalar_log(1.0f) == 0.0f);
}

TEST_CASE("float scalar_exp tracks std::exp over the arguments the analyses reach",
          "[encoder][scalar_math]") {
    // A mean of logs of squared coefficients: silence at the 1e-30 floor is
    // ln 1e-30 = -69, and nothing above a few units; -85 is the last value
    // whose exp is a normal float, which is where the accuracy claim ends.
    double worst_rel = 0.0;
    for (int i = -8500; i <= 800; i += 13) {
        const auto x = static_cast<float>(i / 100.0);
        const double reference = std::exp(static_cast<double>(x));
        const double got = static_cast<double>(scalar_exp(x));
        worst_rel = std::max(worst_rel, std::abs(got - reference) / reference);
    }
    CHECK(worst_rel < 2e-6);
    CHECK(scalar_exp(0.0f) == 1.0f);
}

TEST_CASE("float scalar_exp clamps rather than overflowing", "[encoder][scalar_math]") {
    CHECK(std::isfinite(scalar_exp(1000.0f)));
    CHECK(scalar_exp(1000.0f) == std::numeric_limits<float>::max());
    CHECK(scalar_exp(-1000.0f) > 0.0f);
    CHECK(scalar_exp(-1000.0f) == std::numeric_limits<float>::min());
    // Just inside the clamps the value is still the real one.
    CHECK_THAT(scalar_exp(88.0f), Catch::Matchers::WithinRel(std::exp(88.0), 2e-6));
    CHECK_THAT(scalar_exp(-86.0f), Catch::Matchers::WithinRel(std::exp(-86.0), 2e-6));
}

TEST_CASE("the double overloads are libm", "[encoder][scalar_math]") {
    for (const double x : {1e-30, 0.001, 0.7, 1.0, 2.5, 1e6}) {
        CHECK(scalar_log2(x) == std::log2(x));
        CHECK(scalar_log(x) == std::log(x));
    }
    for (const double x : {-70.0, -3.2, 0.0, 0.5, 4.0}) {
        CHECK(scalar_exp(x) == std::exp(x));
    }
}
