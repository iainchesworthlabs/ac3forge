#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <utility>

#include "ac3/internal/arch/simd.hpp"

// The iterative radix-2 decimation-in-time FFT core shared by the §7.9.4
// fast MDCT fold (mdct.cpp, P = 128) and dft512 (fft.cpp, P = 512, the
// enhanced-coupling spectrum both encoder and decoder consume). Extracted
// from mdct.cpp's fast path verbatim - same tables, same butterfly loop,
// same evaluation order - so hoisting it here changed nothing numerically
// for the MDCT; dft512's move from its direct-form sum onto this core is
// that file's own, separately-verified change.
//
// Internal to src/forge/src/core/ on purpose - transform plumbing between
// translation units, not library surface.

namespace ac3::internal {

// One-time tables for a P-point transform: everything angle-dependent,
// computed once - the same treatment Twiddles/InnerSumTable/ForwardCosTable
// give every other transform in this library. The stage table stores
// std::cos/std::sin of each exact angle -2*pi*j/len rather than generating
// twiddles by iterated complex multiply (which carries j-1 accumulated
// rounding steps by its j-th butterfly).
template <std::size_t P>
struct FftRadix2Tables {
    static_assert((P & (P - 1)) == 0 && P >= 2, "radix-2 needs a power of two");
    // Bit-reversal permutation of 0..P-1 for the decimation-in-time FFT.
    std::array<std::uint16_t, P> bitrev{};
    // Stage twiddles exp(-2*pi*i*j/len) for len = 2, 4, ..., P and
    // j < len/2, flattened at offset len/2 - 1: stage `len` holds len/2
    // entries, so the stages pack exactly into P - 1 slots.
    std::array<double, P - 1> stage_re{};
    std::array<double, P - 1> stage_im{};
    FftRadix2Tables() {
        for (std::size_t i = 1; i < P; ++i) {
            bitrev[i] = static_cast<std::uint16_t>(
                (bitrev[i >> 1] >> 1) | ((i & 1) != 0 ? P / 2 : 0));
        }
        for (std::size_t len = 2; len <= P; len <<= 1) {
            const std::size_t half = len / 2;
            for (std::size_t j = 0; j < half; ++j) {
                const double ang = -2.0 * std::numbers::pi * static_cast<double>(j) /
                                   static_cast<double>(len);
                stage_re[half - 1 + j] = std::cos(ang);
                stage_im[half - 1 + j] = std::sin(ang);
            }
        }
    }
};

// The bit-reversal permutation both forms below open with. Pure data
// movement, no arithmetic, so there is nothing here for a vector unit to do
// and nothing that could differ between the two.
template <std::size_t P>
void fft_radix2_bitreverse(const FftRadix2Tables<P>& t, std::span<double, P> re,
                           std::span<double, P> im) {
    for (std::size_t i = 1; i < P; ++i) {
        const std::size_t j = t.bitrev[i];
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
}

// The scalar butterfly loop: both the REFERENCE the vector form below is
// checked against, and what a build with no vector unit actually runs (see
// fft_radix2_forward's own comment for why those are the same choice).
//
// Deliberately still the original loop, unchanged and unclever: the value of
// a reference is that it is obviously right, not that it is fast.
// tests/core/test_simd_kernels.cpp asserts the two forms agree bit-for-bit,
// which on an x86_64 or aarch64 build is the whole claim the PF5 work rests
// on, and on a generic build is a tautology that costs one cheap test.
template <std::size_t P>
void fft_radix2_forward_reference(const FftRadix2Tables<P>& t, std::span<double, P> re,
                                  std::span<double, P> im) {
    fft_radix2_bitreverse<P>(t, re, im);
    for (std::size_t len = 2; len <= P; len <<= 1) {
        const std::size_t half = len / 2;
        for (std::size_t i = 0; i < P; i += len) {
            for (std::size_t j = 0; j < half; ++j) {
                const double wr = t.stage_re[half - 1 + j];
                const double wi = t.stage_im[half - 1 + j];
                const double xr = re[i + j + half];
                const double xi = im[i + j + half];
                const double vr = xr * wr - xi * wi;
                const double vi = xr * wi + xi * wr;
                const double ur = re[i + j];
                const double ui = im[i + j];
                re[i + j] = ur + vr;
                im[i + j] = ui + vi;
                re[i + j + half] = ur - vr;
                im[i + j + half] = ui - vi;
            }
        }
    }
}

// Two butterflies per iteration through the arch seam (ROADMAP PF5).
//
// WHY IT IS BIT-EXACT. Within one stage, butterfly j touches only
// re/im[i+j] and re/im[i+j+half], so consecutive j are disjoint whenever
// half >= 2 and can be evaluated together. Each lane then performs exactly
// the multiplies, subtracts and adds the reference above performs for that
// j, in the same order, on the same values - the seam's operations are one
// IEEE-754 operation each, and -ffp-contract=off (top-level CMakeLists.txt)
// stops the compiler fusing any of them. So the result is not "accurate to
// 1e-15", it is the same doubles.
//
// The len = 2 stage (half == 1) stays scalar. Vectorising it would mean
// pairing ADJACENT butterflies, whose operands interleave rather than run
// contiguously, and that needs shuffle operations the seam deliberately does
// not carry. It is 1/log2(P) of the butterflies - 1/7 at P = 128, 1/9 at
// P = 512 - and the radix-4 codelets ROADMAP PF4 describes remove the stage
// altogether rather than vectorising it.
template <std::size_t P>
void fft_radix2_forward_vector(const FftRadix2Tables<P>& t, std::span<double, P> re,
                               std::span<double, P> im) {
    fft_radix2_bitreverse<P>(t, re, im);

    double* const rp = re.data();
    double* const ip = im.data();

    for (std::size_t len = 2; len <= P; len <<= 1) {
        const std::size_t half = len / 2;
        if (half == 1) {
            for (std::size_t i = 0; i < P; i += 2) {
                const double wr = t.stage_re[0];
                const double wi = t.stage_im[0];
                const double xr = rp[i + 1];
                const double xi = ip[i + 1];
                const double vr = xr * wr - xi * wi;
                const double vi = xr * wi + xi * wr;
                const double ur = rp[i];
                const double ui = ip[i];
                rp[i] = ur + vr;
                ip[i] = ui + vi;
                rp[i + 1] = ur - vr;
                ip[i + 1] = ui - vi;
            }
            continue;
        }
        // half is a power of two and at least 2 here, so the j loop divides
        // exactly and needs no scalar tail.
        for (std::size_t i = 0; i < P; i += len) {
            for (std::size_t j = 0; j < half; j += 2) {
                const arch::f64x2 wr = arch::f64x2::load(&t.stage_re[half - 1 + j]);
                const arch::f64x2 wi = arch::f64x2::load(&t.stage_im[half - 1 + j]);
                const arch::f64x2 xr = arch::f64x2::load(rp + i + j + half);
                const arch::f64x2 xi = arch::f64x2::load(ip + i + j + half);
                const arch::f64x2 vr = xr * wr - xi * wi;
                const arch::f64x2 vi = xr * wi + xi * wr;
                const arch::f64x2 ur = arch::f64x2::load(rp + i + j);
                const arch::f64x2 ui = arch::f64x2::load(ip + i + j);
                (ur + vr).store(rp + i + j);
                (ui + vi).store(ip + i + j);
                (ur - vr).store(rp + i + j + half);
                (ui - vi).store(ip + i + j + half);
            }
        }
    }
}

// In place over separate re/im arrays: on return (re, im) hold
// A[k] = sum_m a[m] * exp(-2*pi*i*m*k/P) for k = 0..P-1 (unnormalized
// forward transform). Split arrays rather than std::complex so the
// butterfly's four independent multiply-add chains stay contiguous in j,
// which is what lets the vector form above run two butterflies at once.
//
// The hottest kernel in the codec (ROADMAP PF5): every fast MDCT and every
// fast IMDCT - the default on both the encode and the decode path since
// 0.9.0 - is one call to this, 36 times a frame for a 5.1 encode and once
// per channel per block on decode.
//
// A build with no vector unit runs the plain loop, and that is a measurement
// rather than a tidiness preference. A manual two-wide unroll costs
// something when there is no vector instruction at the end of it: the
// compiler's own vectoriser now has an interleaved access pattern to
// re-derive rather than a plain loop to analyse. Clang 21 LOSES 0.68-0.99x
// on a generic build for exactly that reason, while GCC 15 GAINS 1.5-1.7x
// (its vectoriser never gets the plain loop at all). Neither form is right
// for "no SIMD" in general, which is what makes the choice the architecture
// seam's rather than this kernel's - see arch::kHasSimd's own comment in the
// generic header. Where there is a vector unit the intrinsics settle it and
// both compilers gain, so both branches here are the fast one for their own
// target.
template <std::size_t P>
void fft_radix2_forward(const FftRadix2Tables<P>& t, std::span<double, P> re,
                        std::span<double, P> im) {
    if constexpr (arch::kHasSimd) {
        fft_radix2_forward_vector<P>(t, re, im);
    } else {
        fft_radix2_forward_reference<P>(t, re, im);
    }
}

}  // namespace ac3::internal
