#pragma once

#include <span>

// The REFERENCE (direct-form) halves of §8.2.3.2's forward MDCT and §7.9.4.2
// step 3's inverse inner sums, behind a declaration so their tables can be
// left out of a build entirely (roadmap PF7).
//
// Why this exists as a seam at all: each of the five entry points below is
// backed by a full (k, n) cosine/sine matrix, lazily constructed but
// STATICALLY allocated - the storage is reserved by the linker whether or not
// the function is ever called. Measured on the object file
// (dumpbin /HEADERS over mdct.cpp.obj):
//
//     ForwardCosTable<512>      1,048,576 B   forward, long
//     ForwardCosTable<256> x 2    524,288 B   forward, the two short halves
//     InnerSumTable               262,144 B   inverse, long
//     InnerSumPairTable            65,536 B   inverse, short
//                                ----------
//                                1,900,544 B  (1.81 MiB) of .bss
//
// - against about 12 KiB for every table the FAST paths need. A set-top box
// or DSP port paying 1.81 MiB of RAM for the arithmetic it is not running is
// the whole of what PF7's table ROM budget is about.
//
// So the definitions live in ONE of two CMake-selected translation units -
// src/core/transform/{reference,stub}/reference_transform.cpp - exactly as
// ac3/internal/profiling.hpp and src/audio's platform backends are selected,
// and for the same reason (tools/checks/check_platform_macros.ps1: no
// #ifdef). ac3::internal::kReferenceTransformAvailable
// (ac3/internal/profile.hpp, selected the same way) says which one is in the
// build, and the public API refuses a configuration that would need the
// missing one rather than silently substituting the fast path - see
// DecoderConfig::fast_imdct and EncoderConfig::fast_mdct.
//
// The stub variant's bodies are unreachable by construction, not merely
// unused: every caller checks kReferenceTransformAvailable first.

namespace ac3::internal {

// §8.2.3.2 direct form, alpha = 0 (long), -1 and +1 (the two halves of a
// block-switched block). Same phase formula, same std::cos, same accumulation
// order as the fast paths' oracle has always used - moving the loop here
// changes where the code lives, not what it computes.
void reference_mdct512_forward(std::span<const double, 512> windowed,
                               std::span<double, 256> coeffs);
void reference_mdct256_forward_first(std::span<const double, 256> windowed,
                                     std::span<double, 128> coeffs);
void reference_mdct256_forward_second(std::span<const double, 256> windowed,
                                      std::span<double, 128> coeffs);

// §7.9.4.2 step 3's N/4-point (long) and N/8-point (short) complex "IFFT"
// sums, evaluated as the pseudocode writes them:
//   t[n] = sum_k z[k] * (cos(2*pi*k*n/P) + j*sin(2*pi*k*n/P)).
// The short form is called twice per block-switched block, once per half -
// the two sums are independent and share the one table, so running them as
// two passes rather than one interleaved loop leaves every accumulation
// order, and therefore every bit, exactly as it was.
void reference_inner_sum_128(std::span<const double, 128> z_re,
                             std::span<const double, 128> z_im, std::span<double, 128> t_re,
                             std::span<double, 128> t_im);
void reference_inner_sum_64(std::span<const double, 64> z_re, std::span<const double, 64> z_im,
                            std::span<double, 64> t_re, std::span<double, 64> t_im);

}  // namespace ac3::internal
