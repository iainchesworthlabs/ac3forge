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
// Every step in between the two layout seams (the FFT itself, the
// negate-copy, the post-twiddle) is unit-stride f64x4 arithmetic with
// nothing to gather or scatter, since the bin axis stays natural-order/
// bitrev-permuted the whole way through exactly as it does for one object.
//
// The two seams themselves - object-major spans in, object-major spans out,
// f64x4-per-bin in the middle - are paid in 4x4 block transposes (eight
// shuffles per sixteen doubles, see simd_avx2.hpp's transpose4x4): the
// spectra are interleaved once up front into an 8KB dense scratch, and the
// step-5 windowing transposes each region's four consecutive output rows
// straight into four contiguous per-object stores. This is the THIRD shape
// this function has had, and the reason is measured, not aesthetic: the
// first paid the seam in f64x4::set serial-insert gathers and
// lane0()..lane3() extraction scatters inside the kernel (~1% over plain
// scalar on a real joc-domain=mdct decode); the second moved the whole
// interleaving out into ReconstructionState so the kernel boundary was
// free, which made the kernel itself faster but the CALLER ~8% slower -
// its accumulation and overlap-add passes then strode through [bin][slot]
// memory at a cache line per double (net ~7% worse than the first). The
// transposes keep the caller's layout natural AND the kernel boundary
// cheap; git history holds both earlier designs and their numbers.
//
// coeffs0..3 and x0..3 are the same 256/512-long spans
// ac3::imdct512_windowed itself takes; cos1/sin1 are imdct512_windowed's
// own twiddle table (kQuarter = 128 long); fft is the P = 128 FFT table
// the long transform's own fast fold already shares
// (fast_mdct_tables<512>().fft) - passed in rather than looked up here
// because both live in mdct.cpp's anonymous namespace, invisible outside
// that translation unit; this is a plain-old-data struct (no AVX2 type),
// so passing a reference across the object-library boundary is safe.
void imdct512_windowed_batch4(std::span<const double> coeffs0, std::span<const double> coeffs1,
                              std::span<const double> coeffs2, std::span<const double> coeffs3,
                              std::span<const double> cos1, std::span<const double> sin1,
                              const ac3::internal::FftTables<128>& fft, std::span<double> x0,
                              std::span<double> x1, std::span<double> x2, std::span<double> x3);

// ac3::mdct512_forward_batch4's AVX2 body (mdct.hpp): the forward twin of
// imdct512_windowed_batch4 above, and the same three-part shape - transpose
// the four windowed blocks in at the boundary, run every interior step as
// f64x4-per-index arithmetic, transpose the coefficients back out - for the
// same measured reason (see that function's own comment).
//
// The interior is mdct.cpp's mdct_forward_fast_core<512> followed by
// dct4_scaled<512>, transcribed operation for operation: the quarter fold
// (u[i] = -w[3Q-1-i] - w[3Q+i], u[Q+j] = w[j] - w[2Q-1-j], Q = 128) is
// folded INTO the entry transposes rather than run as its own pass, since
// each of its four index walks is contiguous in groups of four - two
// ascending, two descending, and a descending run is just the same
// contiguous load with the four RESULT vectors taken in reverse order,
// which costs nothing. Then the pre-twiddle (u at 2m / M-1-2m, complex
// multiply by pre_re[m]/pre_im[m], bitrev scatter), the P = 128 FFT, and
// the post-twiddle (complex multiply by post_re[k]/post_im[k], times
// `scale`, out at 2k / M-1-2k) - all of which become plain indexed vector
// reads and writes once the data is interleaved, exactly as they do on the
// inverse side.
//
// w0..w3 are 512 long, c0..c3 256; pre_re/pre_im/post_re/post_im are the
// kP = 128 twiddle tables and fft the P = 128 FFT table, all four living in
// mdct.cpp's anonymous namespace and so passed in rather than looked up
// here; `scale` is dct4_scaled's own -2/NLen.
void mdct512_forward_batch4(std::span<const double> w0, std::span<const double> w1,
                            std::span<const double> w2, std::span<const double> w3,
                            std::span<const double> pre_re, std::span<const double> pre_im,
                            std::span<const double> post_re, std::span<const double> post_im,
                            const ac3::internal::FftTables<128>& fft, double scale,
                            std::span<double> c0, std::span<double> c1, std::span<double> c2,
                            std::span<double> c3);

}  // namespace ac3::internal::avx2
