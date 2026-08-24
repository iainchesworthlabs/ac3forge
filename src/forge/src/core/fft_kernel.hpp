#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>

// The fixed-size FFT kernels shared by every transform in this library that
// needs one: the §7.9.4 fast MDCT fold (mdct.cpp, P = 128 for the long
// transform and P = 64 for the two short ones), §7.9.4.1/§7.9.4.2 step 3's
// inverse (mdct.cpp again, P = 128 and P = 64), and dft512 (fft.cpp,
// P = 512, the enhanced-coupling spectrum both encoder and decoder consume).
// Three sizes, all known at compile time, all instantiated from this header.
//
// This replaced a generic iterative radix-2 decimation-in-time core that ran
// an explicit bit-reversal pass and then log2(P) stages of two-point
// butterflies, every one of them loading a twiddle from a table and doing a
// full complex multiply against it. Three things changed, in decreasing
// order of what they were worth (measured standalone at P = 64/128/512:
// 1.70x / 1.59x / 1.75x against the previous core, same inputs):
//
//   1. Radix-4 stages. One radix-4 butterfly does the work of two radix-2
//      stages over the same four points with three complex multiplies
//      instead of four, and reads/writes the array once instead of twice.
//      log2(P) is odd for P = 128 and P = 512, so those two end on a single
//      radix-2 stage (see kHasTrailingRadix2); P = 64 is all radix-4.
//   2. Trivial-twiddle elimination. Every stage's j = 0 group has all-unit
//      twiddles, and the FIRST radix-4 stage (len = 4) is nothing BUT that
//      group - a quarter of the transform's butterflies at P = 512, every
//      one of them previously doing 4 real multiplies against a tabulated
//      1.0 and 0.0. That group is now multiply-free at every stage.
//   3. The digit-reversal permutation is gone as a pass of its own. The
//      kernel takes its input ALREADY digit-reversed, and each caller folds
//      the permutation into the loop that produces that input - the DCT-IV
//      quarter-split in dct4_scaled, the §7.9.4.1 step-2 pre-twiddle in the
//      inverses, the input copy in dft512. Those loops were writing z[m]
//      anyway; writing z[bitrev[m]] costs one indexed store instead of a
//      whole extra P-length pass with a branch in it.
//
// Both the caller-visible interfaces this feeds - the fast MDCT fold and the
// fast IMDCT - remain the OPTIONAL half of a pair. Nothing here touches the
// direct-form evaluations they are validated against (mdct.cpp's
// ForwardCosTable/InnerSumTable paths and their §7.9.4.1 step-3 sum), which
// stay the spec's own statement of each transform and the oracle every
// fast-path test measures against.
//
// Internal to src/forge/src/core/ on purpose - transform plumbing between
// translation units, not library surface.

namespace ac3::internal {

// One-time tables for a P-point transform: everything angle-dependent,
// computed once - the same treatment Twiddles/InnerSumTable/ForwardCosTable
// give every other transform in this library. Each stage stores std::cos/
// std::sin of its exact angle rather than generating twiddles by iterated
// complex multiply (which carries j-1 accumulated rounding steps by its j-th
// butterfly).
template <std::size_t P>
struct FftTables {
    static_assert((P & (P - 1)) == 0 && P >= 4, "these kernels need a power of two >= 4");

    static constexpr int kLog2 = std::countr_zero(P);
    // Radix-4 halves the stage count, so an odd log2(P) leaves one radix-2
    // stage over. It runs LAST, at len = P: pairing the stages the other way
    // (radix-2 first, at len = 2) costs exactly the same number of complex
    // multiplies, but this way the first radix-4 stage sits at len = 4,
    // where every twiddle is 1 and the whole stage is multiply-free.
    static constexpr bool kHasTrailingRadix2 = (kLog2 % 2) != 0;
    static constexpr std::size_t kLastRadix4Len = kHasTrailingRadix2 ? P / 2 : P;

    // Radix-2 bit-reversal permutation of 0..P-1. It is still the right
    // permutation for a radix-4 decimation-in-time pass: a radix-4 stage of
    // length len is exactly the radix-2 stages len/2 and len merged, so the
    // four length-(len/4) sub-blocks it combines are the same contiguous
    // blocks the radix-2 recursion already put there. Public because the
    // kernel below does NOT apply it - callers do, on the way in.
    std::array<std::uint16_t, P> bitrev{};

    // Stage twiddles, flattened into P-1 slots and packed so a stage's three
    // (or one) runs are contiguous:
    //
    //   radix-4 stage of length len, quarter q = len/4, at offset q-1:
    //     [q-1,     q-1 + q)   W1[j] = exp(-2*pi*i*j/len)
    //     [q-1 + q, q-1 + 2q)  W2[j] = exp(-2*pi*i*2j/len)
    //     [q-1 + 2q, q-1 + 3q) W3[j] = exp(-2*pi*i*3j/len)
    //   trailing radix-2 stage (odd log2(P) only), half = P/2, at offset
    //   half-1: [half-1, half-1 + half) W[j] = exp(-2*pi*i*j/P)
    //
    // The q = 1, 4, 16, ... progression makes those runs tile exactly: a
    // stage occupying [q-1, 4q-1) is followed by one starting at 4q-1, and
    // the last run ends at P-1 either way.
    std::array<double, P - 1> stage_re{};
    std::array<double, P - 1> stage_im{};

    FftTables() {
        for (std::size_t i = 1; i < P; ++i) {
            bitrev[i] = static_cast<std::uint16_t>(
                (bitrev[i >> 1] >> 1) | ((i & 1) != 0 ? P / 2 : 0));
        }
        for (std::size_t len = 4; len <= kLastRadix4Len; len <<= 2) {
            const std::size_t q = len / 4;
            const std::size_t base = q - 1;
            for (std::size_t j = 0; j < q; ++j) {
                for (std::size_t r = 1; r <= 3; ++r) {
                    // r*j < 3q < len for every (r, j) here, so this angle is
                    // already inside one period and needs no reduction. That
                    // matters: std::cos of a large un-reduced angle is not
                    // bit-identical to std::cos of the small angle it is
                    // congruent to (mdct.cpp's InnerSumTable documents the
                    // ~1.3e-13 gap it measured at ~792 radians), so a
                    // tabulated twiddle is only trustworthy when its angle
                    // was reduced before the library call.
                    const double angle = -2.0 * std::numbers::pi *
                                         static_cast<double>(r * j) / static_cast<double>(len);
                    stage_re[base + ((r - 1) * q) + j] = std::cos(angle);
                    stage_im[base + ((r - 1) * q) + j] = std::sin(angle);
                }
            }
        }
        if constexpr (kHasTrailingRadix2) {
            const std::size_t half = P / 2;
            const std::size_t base = half - 1;
            for (std::size_t j = 0; j < half; ++j) {
                const double angle =
                    -2.0 * std::numbers::pi * static_cast<double>(j) / static_cast<double>(P);
                stage_re[base + j] = std::cos(angle);
                stage_im[base + j] = std::sin(angle);
            }
        }
    }
};

// One radix-4 decimation-in-time stage of length Len over the whole array.
//
// Derivation, since the twiddle assignment below looks crossed and is not:
// merging the radix-2 stages Len/2 and Len over the four contiguous
// sub-blocks A = [i, i+q), B = [i+q, i+2q), C = [i+2q, i+3q), D = [i+3q,
// i+4q) gives, with u = exp(-2*pi*i*j/Len),
//
//   a = A[j], b = u^2 * B[j], c = u * C[j], d = u^3 * D[j]
//   X[j]      = (a + b) + (c + d)          X[j+2q] = (a + b) - (c + d)
//   X[j+q]    = (a - b) - i*(c - d)        X[j+3q] = (a - b) + i*(c - d)
//
// B carries u^2 (it is the odd half of the FIRST length-Len/2 transform,
// twiddled by exp(-2*pi*i*j/(Len/2))) while C carries u (it is the even half
// of the second, twiddled by exp(-2*pi*i*j/Len)). Multiplying by -i is a
// swap and a sign, not a multiply, which is where radix-4's fourth complex
// multiply went.
template <std::size_t P, std::size_t Len>
void fft_radix4_stage(const FftTables<P>& t, std::span<double, P> re, std::span<double, P> im) {
    constexpr std::size_t kQ = Len / 4;
    constexpr std::size_t kBase = kQ - 1;
    for (std::size_t i = 0; i < P; i += Len) {
        // j == 0: W1 = W2 = W3 = 1, so this group is multiply-free. At
        // Len == 4 it is the only group there is, which is what makes the
        // whole first stage free.
        {
            const double ar = re[i];
            const double ai = im[i];
            const double br = re[i + kQ];
            const double bi = im[i + kQ];
            const double cr = re[i + (2 * kQ)];
            const double ci = im[i + (2 * kQ)];
            const double dr = re[i + (3 * kQ)];
            const double di = im[i + (3 * kQ)];
            const double t0r = ar + br;
            const double t0i = ai + bi;
            const double t1r = ar - br;
            const double t1i = ai - bi;
            const double t2r = cr + dr;
            const double t2i = ci + di;
            const double t3r = cr - dr;
            const double t3i = ci - di;
            re[i] = t0r + t2r;
            im[i] = t0i + t2i;
            re[i + kQ] = t1r + t3i;
            im[i + kQ] = t1i - t3r;
            re[i + (2 * kQ)] = t0r - t2r;
            im[i + (2 * kQ)] = t0i - t2i;
            re[i + (3 * kQ)] = t1r - t3i;
            im[i + (3 * kQ)] = t1i + t3r;
        }
        for (std::size_t j = 1; j < kQ; ++j) {
            const double w1r = t.stage_re[kBase + j];
            const double w1i = t.stage_im[kBase + j];
            const double w2r = t.stage_re[kBase + kQ + j];
            const double w2i = t.stage_im[kBase + kQ + j];
            const double w3r = t.stage_re[kBase + (2 * kQ) + j];
            const double w3i = t.stage_im[kBase + (2 * kQ) + j];
            const std::size_t i0 = i + j;
            const std::size_t i1 = i0 + kQ;
            const std::size_t i2 = i0 + (2 * kQ);
            const std::size_t i3 = i0 + (3 * kQ);
            const double ar = re[i0];
            const double ai = im[i0];
            const double br = (re[i1] * w2r) - (im[i1] * w2i);
            const double bi = (re[i1] * w2i) + (im[i1] * w2r);
            const double cr = (re[i2] * w1r) - (im[i2] * w1i);
            const double ci = (re[i2] * w1i) + (im[i2] * w1r);
            const double dr = (re[i3] * w3r) - (im[i3] * w3i);
            const double di = (re[i3] * w3i) + (im[i3] * w3r);
            const double t0r = ar + br;
            const double t0i = ai + bi;
            const double t1r = ar - br;
            const double t1i = ai - bi;
            const double t2r = cr + dr;
            const double t2i = ci + di;
            const double t3r = cr - dr;
            const double t3i = ci - di;
            re[i0] = t0r + t2r;
            im[i0] = t0i + t2i;
            re[i1] = t1r + t3i;
            im[i1] = t1i - t3r;
            re[i2] = t0r - t2r;
            im[i2] = t0i - t2i;
            re[i3] = t1r - t3i;
            im[i3] = t1i + t3r;
        }
    }
}

// The single length-P radix-2 stage an odd log2(P) leaves over, run last.
template <std::size_t P>
void fft_radix2_final_stage(const FftTables<P>& t, std::span<double, P> re,
                            std::span<double, P> im) {
    constexpr std::size_t kHalf = P / 2;
    constexpr std::size_t kBase = kHalf - 1;
    {
        // j == 0 again: w = 1.
        const double ur = re[0];
        const double ui = im[0];
        const double vr = re[kHalf];
        const double vi = im[kHalf];
        re[0] = ur + vr;
        im[0] = ui + vi;
        re[kHalf] = ur - vr;
        im[kHalf] = ui - vi;
    }
    for (std::size_t j = 1; j < kHalf; ++j) {
        const double wr = t.stage_re[kBase + j];
        const double wi = t.stage_im[kBase + j];
        const double xr = re[j + kHalf];
        const double xi = im[j + kHalf];
        const double vr = (xr * wr) - (xi * wi);
        const double vi = (xr * wi) + (xi * wr);
        const double ur = re[j];
        const double ui = im[j];
        re[j] = ur + vr;
        im[j] = ui + vi;
        re[j + kHalf] = ur - vr;
        im[j + kHalf] = ui - vi;
    }
}

// The radix-4 stage sequence len = 4, 16, 64, ..., unrolled at compile time
// so every stage's length, quarter and table offset are constants.
template <std::size_t P, std::size_t Len>
void fft_radix4_chain(const FftTables<P>& t, std::span<double, P> re, std::span<double, P> im) {
    fft_radix4_stage<P, Len>(t, re, im);
    if constexpr (Len * 4 <= FftTables<P>::kLastRadix4Len) {
        fft_radix4_chain<P, Len * 4>(t, re, im);
    }
}

// In place over separate re/im arrays. INPUT MUST ALREADY BE DIGIT-REVERSED:
// on entry (re, im) hold a[t.bitrev[m]] = input[m], and on return they hold
// A[k] = sum_m input[m] * exp(-2*pi*i*m*k/P) for k = 0..P-1 in natural order
// (unnormalized forward transform). Split arrays rather than std::complex so
// the butterfly's independent multiply-add chains stay visible to the
// auto-vectorizer.
template <std::size_t P>
void fft_forward_bitrev(const FftTables<P>& t, std::span<double, P> re, std::span<double, P> im) {
    fft_radix4_chain<P, 4>(t, re, im);
    if constexpr (FftTables<P>::kHasTrailingRadix2) {
        fft_radix2_final_stage<P>(t, re, im);
    }
}

}  // namespace ac3::internal
