#pragma once

#include <bit>
#include <cmath>
#include <cstdint>
#include <numbers>

// The transcendentals the encoders' content analyses call, in the scalar of
// the coefficient store (ac3/internal/encode_scalar.hpp), as one overload set.
//
// The double overloads are libm's own functions, called exactly as the
// encoders called them before this header existed, so the default build's
// fifteen bitstream hashes (tests/golden/bitstream-hashes.json) hold.
//
// The float overloads are the project's own. Not for speed alone - though on
// the one platform whose encode scalar is float, the ESP32-S3, a call into
// newlib's logf is a large multiple of the polynomial below - but for a
// property libm cannot give: the same float out of the same float in on every
// platform. The minimum-footprint profile's encode fixtures
// (apps/baremetal/encode_fixture.hpp) pin one hash per stream and check it on
// the x86 host, the Cortex-M3 leg under QEMU and the ESP32-S3, and three C
// libraries' logf differ in their last bit on some inputs. The bit-level
// decomposition and the short series below are plain float multiplies and
// adds, which every one of those legs rounds identically.
//
// Accuracy is what the callers need and no more. extension_content and
// spx_blend take a mean of logs and exponentiate it back into a spectral
// flatness in [0, 1] that is then quantised to five bits or compared with a
// threshold; choose_delta_segments rounds 128 * log2 |c| to an integer psd
// unit (1/128 of one exponent step). A few float ulps of error in log2 move
// none of those by a visible amount; tests/encoder/test_scalar_math.cpp pins
// the bounds.

namespace ac3::internal {

// --- log2 --------------------------------------------------------------------

inline double scalar_log2(double x) { return std::log2(x); }

// x = m * 2^e with m in [sqrt(1/2), sqrt(2)), then log2(m) from the series
// 2 * log2(e) * atanh(u) with u = (m - 1) / (m + 1), |u| <= 0.1716. Five
// terms leave a truncation error below 4e-10 relative to the log, under a
// float ulp; the float arithmetic itself contributes a few ulps.
//
// Requires x > 0: every caller either guards the magnitude or has added a
// positive floor. Subnormals are lifted by 2^24 first (an exact multiply) so
// the exponent field is meaningful; zero would reach the series as a
// subnormal with an empty mantissa and give -inf's neighbourhood rather than
// -inf, which no caller depends on.
inline float scalar_log2(float x) {
    constexpr std::uint32_t kExponentMask = 0x7f800000U;
    constexpr std::uint32_t kMantissaMask = 0x007fffffU;
    constexpr std::uint32_t kHalfBits = 0x3f000000U;  // 0.5f
    int exponent = 0;
    if ((std::bit_cast<std::uint32_t>(x) & kExponentMask) == 0) {
        x *= 16777216.0f;  // 2^24, exact
        exponent -= 24;
    }
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(x);
    exponent += static_cast<int>((bits & kExponentMask) >> 23) - 126;
    float m = std::bit_cast<float>((bits & kMantissaMask) | kHalfBits);  // [0.5, 1)
    if (m < 0.70710678118654752f) {
        m *= 2.0f;
        exponent -= 1;
    }
    const float u = (m - 1.0f) / (m + 1.0f);
    const float u2 = u * u;
    // 2 / ln 2, then the odd series in u.
    constexpr float kTwoLog2e = 2.8853900817779268f;
    const float series =
        u * (1.0f + u2 * (1.0f / 3.0f + u2 * (1.0f / 5.0f + u2 * (1.0f / 7.0f + u2 * (1.0f / 9.0f)))));
    return static_cast<float>(exponent) + kTwoLog2e * series;
}

// --- natural log -------------------------------------------------------------

inline double scalar_log(double x) { return std::log(x); }

inline float scalar_log(float x) { return std::numbers::ln2_v<float> * scalar_log2(x); }

// --- exp ---------------------------------------------------------------------

inline double scalar_exp(double x) { return std::exp(x); }

// e^x = 2^n * e^r with n the nearest integer to x / ln 2 and r = x - n ln 2
// in [-0.35, 0.35]. ln 2 is split into a 15-bit head and a tail (fdlibm's
// pair) so that n * head is exact for every n a float exponent can hold and
// the reduction loses nothing - reducing the scaled argument x * log2(e)
// instead would carry that product's rounding, some 1e-6 relative at |x| of
// 70, into the result. e^r is then a degree-6 Taylor series, whose
// truncation at |r| = 0.35 is 1.2e-7, and 2^n is built directly as an
// exponent field. Arguments past the float range clamp to its ends rather
// than overflowing to inf or reaching a zero exponent field; the callers
// exponentiate a mean of logs of floored powers (>= ln 1e-30 = -69) and never
// get there.
inline float scalar_exp(float x) {
    if (x >= 88.7f) {  // e^88.72 is the largest float
        return 3.4028235e38f;
    }
    if (x <= -87.3f) {  // e^-87.34 is the smallest normal one
        return 1.17549435e-38f;
    }
    const float n = std::floor(x * std::numbers::log2e_v<float> + 0.5f);
    constexpr float kLn2Head = 0.693145751953125f;  // 0x3f317200
    constexpr float kLn2Tail = 1.4286067653e-06f;   // 0x35bfbe8e
    const float r = (x - n * kLn2Head) - n * kLn2Tail;
    const float p =
        1.0f + r * (1.0f + r * (1.0f / 2.0f +
                               r * (1.0f / 6.0f +
                                    r * (1.0f / 24.0f + r * (1.0f / 120.0f + r * (1.0f / 720.0f))))));
    const auto exponent = static_cast<std::int32_t>(n);  // [-126, 127]
    const auto scale_bits = static_cast<std::uint32_t>(exponent + 127) << 23;
    return p * std::bit_cast<float>(scale_bits);
}

}  // namespace ac3::internal
