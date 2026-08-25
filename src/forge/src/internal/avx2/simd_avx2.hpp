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

}  // namespace ac3::internal::avx2
