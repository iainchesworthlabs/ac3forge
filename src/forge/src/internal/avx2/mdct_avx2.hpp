#pragma once

#include <span>

// ---------------------------------------------------------------------------
// AVX2 kernel bodies for src/forge/src/core/mdct.cpp, dispatched behind
// ac3::internal::cpu::has_avx2() at each call site in that file. Declared
// here with PLAIN std::span/double signatures - no AVX2 type ever appears
// outside mdct_avx2.cpp itself - so mdct.cpp (compiled without any AVX2
// flag, see src/forge/CMakeLists.txt's forge_simd_avx2 target) can call
// these across the object-library boundary without ever seeing an
// intrinsic. See simd_avx2.hpp for the f64x4 type these are built from and
// docs/building.md's "Runtime AVX2 dispatch" section for the mechanism.
//
// Each function here is a straight four-lanes-instead-of-two transliteration
// of its f64x2 counterpart in mdct.cpp: identical operations in identical
// order, so the result is bit-identical, not merely close - the same
// argument mdct.cpp's own comments make for the SSE2 seam.
// ---------------------------------------------------------------------------

namespace ac3::internal::avx2 {

// mdct.cpp's apply_analysis_window, four samples per iteration instead of
// two. Unit stride throughout, nothing to gather or scatter.
void apply_analysis_window(std::span<const double, 512> x, std::span<double, 512> windowed);

}  // namespace ac3::internal::avx2
