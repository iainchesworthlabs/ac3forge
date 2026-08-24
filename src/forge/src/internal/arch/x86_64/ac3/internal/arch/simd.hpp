#pragma once

#include <emmintrin.h>  // SSE2

#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
// The x86-64 (SSE2) member of the arch seam. See
// src/forge/src/internal/arch/generic/ac3/internal/arch/simd.hpp for what the
// seam is, how CMake selects between the three directories, and why no
// header here needs a preprocessor conditional to name its architecture.
//
// SSE2 AND ONLY SSE2, deliberately: it is part of the x86-64 architecture
// itself, so every operation below is available on every 64-bit x86 CPU ever
// made, needs no -march= flag, and needs no runtime cpuid dispatch. Scalar
// double arithmetic on x86-64 already goes through these same SSE2 units, so
// _mm_add_pd/_mm_sub_pd/_mm_mul_pd are the identical IEEE-754 operations the
// scalar code was performing, two at a time - which is the whole
// bit-exactness argument, and why this file uses no approximate reciprocal,
// no rsqrt, and no min/max instruction (whose NaN and signed-zero rules
// differ from the C++ operators the scalar reference uses).
//
// Everything wider - AVX's 256-bit ymm, AVX-512, and the FMA3 instructions
// that would let a butterfly fuse its multiply and add - is a CPU-FEATURE
// question rather than an architecture one, and turning any of them on at
// compile time would produce a binary that faults on hardware without them.
// That needs runtime dispatch, which this seam does not have; see
// docs/building.md.
// ---------------------------------------------------------------------------

namespace ac3::internal::arch {

inline constexpr const char* kSimdName = "x86_64-sse2";

struct f64x2 {
    __m128d v;

    [[nodiscard]] static f64x2 load(const double* p) { return f64x2{_mm_loadu_pd(p)}; }
    // Lane 0 is `a`, matching the generic header's {lo, hi} - note that
    // _mm_set_pd takes its arguments high-lane first.
    [[nodiscard]] static f64x2 set(double a, double b) { return f64x2{_mm_set_pd(b, a)}; }
    [[nodiscard]] static f64x2 broadcast(double a) { return f64x2{_mm_set1_pd(a)}; }

    void store(double* p) const { _mm_storeu_pd(p, v); }

    [[nodiscard]] double lane0() const { return _mm_cvtsd_f64(v); }
    [[nodiscard]] double lane1() const { return _mm_cvtsd_f64(_mm_unpackhi_pd(v, v)); }
};

[[nodiscard]] inline f64x2 operator+(f64x2 a, f64x2 b) { return f64x2{_mm_add_pd(a.v, b.v)}; }
[[nodiscard]] inline f64x2 operator-(f64x2 a, f64x2 b) { return f64x2{_mm_sub_pd(a.v, b.v)}; }
[[nodiscard]] inline f64x2 operator*(f64x2 a, f64x2 b) { return f64x2{_mm_mul_pd(a.v, b.v)}; }
// Sign-bit flip rather than 0.0 - a, so -0.0 negates to +0.0 the way the
// unary operator on a double does, and not to -0.0 the way a subtraction
// from positive zero would.
[[nodiscard]] inline f64x2 operator-(f64x2 a) {
    return f64x2{_mm_xor_pd(a.v, _mm_set1_pd(-0.0))};
}

// IEEE-754 roundToIntegralTiesAway - exactly std::round(), for every finite
// input, with NaN propagated. SSE2 has no rounding instruction at all
// (roundpd arrives with SSE4.1, and even that rounds ties to EVEN, which is
// not what std::round does), so this is built out of arithmetic. The
// alternative it replaces is a call to libm's round(), which MSVC and the
// System V libraries both leave out of line - about 9,100 of them per
// encoded frame (ROADMAP PF2).
//
// The construction, on the MAGNITUDE a = |x| so the tie case has only one
// direction to worry about:
//
//   t = (a + 2^52) - 2^52
//       The classic magic-number round. For a < 2^52 the addition forces
//       every fractional bit off the bottom of the significand and the
//       subtraction is exact, so t = a rounded to an integer under the
//       CURRENT rounding mode - round-to-nearest-EVEN, which is the mode
//       this project never changes and every other double in it assumes.
//   a - t == 0.5  <=>  a was an exact tie that round-to-even took DOWNWARDS
//       (2.5 -> 2). Ties it took upwards (1.5 -> 2) are already the
//       ties-away answer, and non-ties are unaffected. Both a and t are
//       exactly representable and within a factor of two of each other, so
//       the difference is exact.
//   t + 1 in that case; then re-apply the sign bit of x.
//
//   a >= 2^52 (which includes +/-inf) means x is already an integer and the
//   magic-number step would be a no-op at best, so those lanes select x
//   unchanged. A NaN compares unordered, takes the arithmetic path, and
//   propagates through it.
//
// tests/core/test_simd_kernels.cpp pins this against std::round() over the
// tie ladder, the powers of two either side of the magic number, denormals,
// both zeros, infinities and a large pseudorandom spread.
[[nodiscard]] inline f64x2 round_ties_away(f64x2 x) {
    const __m128d sign_mask = _mm_set1_pd(-0.0);
    const __m128d magic = _mm_set1_pd(4503599627370496.0);  // 2^52
    const __m128d half = _mm_set1_pd(0.5);
    const __m128d one = _mm_set1_pd(1.0);

    const __m128d sign = _mm_and_pd(x.v, sign_mask);
    const __m128d a = _mm_andnot_pd(sign_mask, x.v);  // |x|

    const __m128d t = _mm_sub_pd(_mm_add_pd(a, magic), magic);
    const __m128d tie_down = _mm_cmpeq_pd(_mm_sub_pd(a, t), half);
    const __m128d rounded = _mm_add_pd(t, _mm_and_pd(tie_down, one));

    const __m128d already_integral = _mm_cmpge_pd(a, magic);
    const __m128d small = _mm_andnot_pd(already_integral, _mm_or_pd(rounded, sign));
    const __m128d large = _mm_and_pd(already_integral, x.v);
    return f64x2{_mm_or_pd(small, large)};
}

struct i32x4 {
    __m128i v;

    [[nodiscard]] static i32x4 load_u8_widen(const std::uint8_t* p) {
        // Four bytes in, four dwords out: two unpacks against zero. Loaded
        // through a 32-bit scalar rather than _mm_loadu_si128 so nothing is
        // read past the four bytes actually asked for.
        std::int32_t packed = 0;
        std::memcpy(&packed, p, sizeof(packed));
        const __m128i zero = _mm_setzero_si128();
        const __m128i bytes = _mm_cvtsi32_si128(packed);
        return i32x4{_mm_unpacklo_epi16(_mm_unpacklo_epi8(bytes, zero), zero)};
    }
    [[nodiscard]] static i32x4 broadcast(std::int32_t a) { return i32x4{_mm_set1_epi32(a)}; }

    // memcpy rather than _mm_storeu_si128 through a cast pointer: the
    // store is unaligned either way, and this needs no reinterpret_cast to
    // express. Every compiler here lowers it to the same single movdqu.
    void store(std::int32_t* p) const { std::memcpy(p, &v, sizeof(v)); }
};

[[nodiscard]] inline i32x4 operator-(i32x4 a, i32x4 b) { return i32x4{_mm_sub_epi32(a.v, b.v)}; }

template <int Bits>
[[nodiscard]] i32x4 shift_left(i32x4 a) {
    return i32x4{_mm_slli_epi32(a.v, Bits)};
}

}  // namespace ac3::internal::arch
