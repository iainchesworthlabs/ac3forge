#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../core/fft_kernel.hpp"

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

// dct4_scaled<NLen>'s pre-twiddle (mdct.cpp), four m at a time instead of
// two: gathers u at stride +-2, complex-multiplies by pre_re[m]/pre_im[m],
// scatters the result to z_re[bitrev[m]]/z_im[bitrev[m]]. Not templated on
// NLen - P (pre_re.size() == pre_im.size() == bitrev.size() ==
// z_re.size() == z_im.size()) is a runtime span length instead, since a
// template instantiated from mdct.cpp (no AVX2 flag) could not itself use
// AVX2 intrinsics; P must be a multiple of 4 (true at both call sites, 128
// and 64). u.size() must be 2*P (M).
void dct4_pre_twiddle(std::span<const double> u, std::span<const double> pre_re,
                      std::span<const double> pre_im, std::span<const std::uint16_t> bitrev,
                      std::span<double> z_re, std::span<double> z_im);

// dct4_scaled<NLen>'s post-twiddle: unit-stride read of z_re/z_im/post_re/
// post_im (all P long), scaled complex multiply, scatter to out (2*P = M
// long) at stride +-2. P must be a multiple of 4.
void dct4_post_twiddle(std::span<const double> z_re, std::span<const double> z_im,
                       std::span<const double> post_re, std::span<const double> post_im,
                       double scale, std::span<double> out);

// imdct512_windowed's step 2-3 pre-twiddle (fast branch), four k at a time:
// gathers coeffs at two different descending/ascending stride-2 walks,
// complex-multiplies by cos1[k]/sin1[k] with imdct512_windowed's OWN sign
// convention (zi negated, NOT the same as dct4_pre_twiddle's), scatters to
// z_re[bitrev[k]]/z_im[bitrev[k]]. kQuarter (cos1.size() == sin1.size() ==
// bitrev.size() == z_re.size() == z_im.size()) is 128, a multiple of 4.
// coeffs.size() must be kHalfN (512).
void imdct512_pre_twiddle(std::span<const double> coeffs, std::span<const double> cos1,
                          std::span<const double> sin1, std::span<const std::uint16_t> bitrev,
                          std::span<double> z_re, std::span<double> z_im);

// imdct512_windowed's post-FFT copy-and-negate (fast branch): t_re = z_re,
// t_im = -z_im, unit stride, four at a time.
void imdct512_negate_copy(std::span<const double> z_re, std::span<const double> z_im,
                          std::span<double> t_re, std::span<double> t_im);

// imdct512_windowed's step 4 post-twiddle: unit stride throughout (cos1,
// sin1, t_re, t_im, y_re, y_im all kQuarter = 128 long), four at a time.
void imdct512_post_twiddle(std::span<const double> cos1, std::span<const double> sin1,
                           std::span<const double> t_re, std::span<const double> t_im,
                           std::span<double> y_re, std::span<double> y_im);

// imdct256_pair_windowed's step 4 post-twiddle: the same shape as
// imdct512_post_twiddle but over BOTH half-block sets (1 and 2) at once,
// unit stride throughout, kEighth = 64 long, four at a time.
void imdct256_post_twiddle(std::span<const double> cos2, std::span<const double> sin2,
                           std::span<const double> t1_re, std::span<const double> t1_im,
                           std::span<const double> t2_re, std::span<const double> t2_im,
                           std::span<double> y1_re, std::span<double> y1_im,
                           std::span<double> y2_re, std::span<double> y2_im);

// ROADMAP PF5's batch-axis follow-on (ac3::imdct512_windowed_batch4's own
// AVX2 body, see mdct.hpp): runs the SAME steps 2-5 imdct512_windowed's
// fast branch does - pre-twiddle, in-place FFT, negate-copy, post-twiddle,
// windowing/de-interleave - but with one f64x4 per BIN holding all four
// objects' values for that bin, instead of one f64x2/f64x4 per group of
// bins within a single object's own spectrum (contrast with
// imdct512_pre_twiddle/imdct512_post_twiddle above, which batch ACROSS
// bins of ONE transform; this batches ACROSS four INDEPENDENT transforms).
//
// A first version of this function took four SEPARATE per-object spans and
// gathered/scattered between them and the f64x4-per-bin working arrays -
// correct, but measurably SLOWER than four scalar calls (a real, measured
// regression: docs/building.md's own "Runtime AVX2 dispatch" section, and
// ROADMAP PF5's own history), because `f64x4::set` from four separate
// scalar reads and `lane0()..lane3()` extraction back out are both several
// real instructions apiece, run 128 and 512 times a call respectively -
// overhead that swamped the vectorised arithmetic's own savings at this
// transform size. `spectra`/`pcm_out` fix that by requiring the CALLER to
// hold four objects' data already interleaved (four objects' values at
// the same bin/sample index are four ADJACENT doubles), so both the
// pre-twiddle gather and the step-5 scatter become a single f64x4 load/
// store instead: `spectra` is row-major [256][stride], `pcm_out` row-major
// [512][stride] - stride is the caller's own per-row width (its widest
// legal value, not fixed to 4), and `group_start` (a multiple of 4, with
// group_start + 4 <= stride) names which four ADJACENT columns this call
// reads/writes; every other column is left untouched. spectra.size() must
// be 256 * stride, pcm_out.size() 512 * stride.
//
// cos1/sin1 are imdct512_windowed's own twiddle table (kQuarter = 128
// long); fft is the P = 128 FFT table the long transform's own fast fold
// already shares (fast_mdct_tables<512>().fft) - passed in rather than
// looked up here because both live in mdct.cpp's anonymous namespace,
// invisible outside that translation unit; this is a plain-old-data
// struct (no AVX2 type), so passing a reference across the object-library
// boundary is safe.
void imdct512_windowed_batch4(std::span<const double> spectra, std::size_t stride,
                              std::size_t group_start, std::span<const double> cos1,
                              std::span<const double> sin1,
                              const ac3::internal::FftTables<128>& fft, std::span<double> pcm_out);

}  // namespace ac3::internal::avx2
