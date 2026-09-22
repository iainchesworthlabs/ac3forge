#pragma once

#include <immintrin.h>

// ---------------------------------------------------------------------------
// The AVX2 tier's own tiny SIMD type, same shape as
// src/forge/src/internal/arch/x86_64/ac3/internal/arch/simd.hpp's f64x2 -
// only ever included by a .cpp file compiled into forge_simd_avx2 (the one
// AVX2-flagged object library, see src/forge/CMakeLists.txt), never by
// forge_objects. That split is why this lives in src/internal/avx2/ rather
// than alongside f64x2 in the arch/ tree: arch/'s directory-selection
// mechanism picks exactly one SIMD width for the WHOLE codec at compile
// time, but AVX2 is a second, narrower, runtime-gated tier that coexists
// with whichever arch/ tier is active - a kernel that dispatches to AVX2
// still falls back to arch/'s own f64x2 (or scalar) on a CPU that can't run
// this. See src/forge/src/internal/cpu/cpu_features.hpp for the dispatch
// contract and docs/building.md's "Runtime AVX2 dispatch" section.
//
// SAME bit-exactness argument as f64x2: every operation below is exactly
// one IEEE-754 add, subtract or multiply per lane, so widening from 2 lanes
// to 4 cannot change a result - each lane performs the identical operation
// sequence the scalar/SSE2 form did, just four at a time instead of two.
// No FMA intrinsic is used anywhere in this file, matching the project-wide
// -ffp-contract=off policy this seam depends on (see the top-level
// CMakeLists.txt's own comment).
// ---------------------------------------------------------------------------

namespace ac3::internal::avx2 {

struct f64x4 {
    __m256d v;

    [[nodiscard]] static f64x4 load(const double* p) { return f64x4{_mm256_loadu_pd(p)}; }
    // Lane 0 is `a`: _mm256_set_pd takes its arguments highest-lane first,
    // the same convention f64x2::set already documents for _mm_set_pd.
    [[nodiscard]] static f64x4 set(double a, double b, double c, double d) {
        return f64x4{_mm256_set_pd(d, c, b, a)};
    }
    [[nodiscard]] static f64x4 broadcast(double a) { return f64x4{_mm256_set1_pd(a)}; }

    void store(double* p) const { _mm256_storeu_pd(p, v); }

    // Lane extraction for the gather/scatter kernels' scalar scatter ends -
    // same reasoning as f64x2::lane0/lane1: the seam carries no scatter-store
    // instruction, so a kernel that writes to a permuted destination (the
    // FFT's bitrev table) extracts each lane individually rather than
    // vector-storing to a contiguous range it does not actually have.
    [[nodiscard]] double lane0() const { return _mm256_cvtsd_f64(v); }
    [[nodiscard]] double lane1() const {
        return _mm256_cvtsd_f64(_mm256_permute4x64_pd(v, 0b01'01'01'01));
    }
    [[nodiscard]] double lane2() const {
        return _mm256_cvtsd_f64(_mm256_permute4x64_pd(v, 0b10'10'10'10));
    }
    [[nodiscard]] double lane3() const {
        return _mm256_cvtsd_f64(_mm256_permute4x64_pd(v, 0b11'11'11'11));
    }
};

[[nodiscard]] inline f64x4 operator+(f64x4 a, f64x4 b) {
    return f64x4{_mm256_add_pd(a.v, b.v)};
}
[[nodiscard]] inline f64x4 operator-(f64x4 a, f64x4 b) {
    return f64x4{_mm256_sub_pd(a.v, b.v)};
}
[[nodiscard]] inline f64x4 operator*(f64x4 a, f64x4 b) {
    return f64x4{_mm256_mul_pd(a.v, b.v)};
}
// Sign-bit flip, not 0.0 - a: same reasoning as f64x2's unary minus - so
// -0.0 negates to +0.0 the way the scalar unary operator does.
[[nodiscard]] inline f64x4 operator-(f64x4 a) {
    return f64x4{_mm256_xor_pd(a.v, _mm256_set1_pd(-0.0))};
}

// Broadcast-scalar multiply (ROADMAP PF5's batch-axis follow-on): a batched
// transform's DATA is one f64x4 per lane-of-independent-transform-instances,
// but its TWIDDLES are a plain `double` - identical for every instance in
// the batch, since they only depend on which bin/stage, not which instance
// - so fft_kernel.hpp's templated `VecType * double` twiddle multiplies
// need this pair to compile at `VecType = f64x4`. Broadcasting into all 4
// lanes and then multiplying is exactly the operation a scalar-times-vector
// product already is, so this changes no result `f64x4::broadcast(s) * v`
// would not already give - it exists only so callers don't have to spell
// that out themselves at every twiddle multiply.
[[nodiscard]] inline f64x4 operator*(f64x4 a, double b) {
    return f64x4{_mm256_mul_pd(a.v, _mm256_set1_pd(b))};
}
[[nodiscard]] inline f64x4 operator*(double a, f64x4 b) {
    return f64x4{_mm256_mul_pd(_mm256_set1_pd(a), b.v)};
}

// In-place 4x4 transpose: on return, r0 holds the four inputs' lane 0, r1
// their lane 1, and so on. Eight shuffles for sixteen doubles - the whole
// reason a batched kernel can afford to change data layout at its own
// boundary instead of asking the caller to store everything interleaved:
// f64x4::set from four scalar loads costs a serial insert chain per
// vector, and lane0()..lane3() extraction the mirror image of one, so
// moving 16 doubles across the layout seam that way is ~28 dependent
// instructions where this is 8 independent ones (ROADMAP PF5's batch-axis
// follow-on measured both earlier shapes losing to plain scalar because
// of exactly that tax - see mdct_avx2.hpp's imdct512_windowed_batch4).
// Pure data movement, no arithmetic, so it cannot perturb bit-exactness.
inline void transpose4x4(f64x4& r0, f64x4& r1, f64x4& r2, f64x4& r3) {
    const __m256d t0 = _mm256_unpacklo_pd(r0.v, r1.v);  // r0[0] r1[0] r0[2] r1[2]
    const __m256d t1 = _mm256_unpackhi_pd(r0.v, r1.v);  // r0[1] r1[1] r0[3] r1[3]
    const __m256d t2 = _mm256_unpacklo_pd(r2.v, r3.v);  // r2[0] r3[0] r2[2] r3[2]
    const __m256d t3 = _mm256_unpackhi_pd(r2.v, r3.v);  // r2[1] r3[1] r2[3] r3[3]
    r0.v = _mm256_permute2f128_pd(t0, t2, 0x20);        // r0[0] r1[0] r2[0] r3[0]
    r1.v = _mm256_permute2f128_pd(t1, t3, 0x20);        // r0[1] r1[1] r2[1] r3[1]
    r2.v = _mm256_permute2f128_pd(t0, t2, 0x31);        // r0[2] r1[2] r2[2] r3[2]
    r3.v = _mm256_permute2f128_pd(t1, t3, 0x31);        // r0[3] r1[3] r2[3] r3[3]
}

}  // namespace ac3::internal::avx2
