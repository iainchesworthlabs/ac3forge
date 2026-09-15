#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>

#include "ac3/core/window.hpp"
#include "fft_kernel.hpp"
#include "fixed32.hpp"

// The fixed-point tier's §7.9.4 inverse pair (planning/arithmetic-tiers.md,
// Phase B): the same pre-twiddle, N/4-point FFT, post-twiddle and window as
// mdct.cpp's fast branch, transcribed step for step, in Fixed32.
//
// No scaling inside the transform. The spec's inverse is an unscaled sum
// (§7.9.4.1 step 3 has no 1/N; the encoder's forward carried it), and the
// N/4 = 128-point FFT of that sum can grow by seven bits across its stages -
// the four butterflies of a radix-4 stage sum four rotated inputs, so 4 x 4 x
// 4 x 2 over the stages len = 4, 16, 64 and the trailing radix-2. Q7.24 has
// seven bits of headroom. So the transform's precondition is that every
// coefficient it is given is below one half in magnitude: the pre-twiddle
// pairs two of them into a complex value of magnitude at most sqrt(2)/2, the
// stages can take that to at most 90.5, and the post-twiddle and the window
// (a rotation and a factor of at most 1) leave it there. Below 128, so
// nothing wraps, for any input at all - including the coherent one no real
// stream produces. The decoders hold the precondition through the block
// exponent they store each channel's coefficients under (block_norm.hpp:
// the store is normalised so its largest coefficient sits just below one
// half), which is also what gives the tier its precision: every rounding
// step here is a raw unit, 2^-24, and the output it is a raw unit OF is the
// block's own signal scaled up to the format, not the absolute level of a
// quiet passage.
//
// Where the rounding goes. A product rounds once (fixed32.hpp's rule) and
// the FFT's twiddle products sit in the intermediate domain, whose values
// are up to two orders of magnitude larger than the output, so their
// roundings arrive at the output already small; the two products of the
// post-twiddle and the one of the window are what set the floor, at about
// one raw unit per output sample. The test beside this measures it:
// tests/core/test_mdct_fixed.cpp holds the fixed inverse to the double one
// on random, tonal and worst-case blocks.
//
// No saturation either, for the same reason. Every product in the pair is a
// value below 90.5 times a twiddle or a window entry of magnitude at most
// one, so its result is below 90.5 plus a raw unit and Fixed32's saturation
// can never act on it. The pair therefore computes in ImdctValue below,
// which has Fixed32's bits, sums and rounding and product_unsaturated's
// product: the same output, bit for bit, for every input the precondition
// admits. On RV32IMAC at -O2 that product is seven instructions and no
// branch, against ten and a branch for the saturating one, and an AC-3 5.1
// frame is some 88,000 of them (docs/platforms/bare-metal/esp32-c6.md has
// what that bought on a board).
//
// Header-only and inline, like fft_kernel.hpp: the decoders instantiate it
// through scalar_inverse.hpp and the test instantiates it directly, so no
// symbol needs exporting from the library. The tables are built once, from
// the same double expressions mdct.cpp's own tables come from, and rounded
// to the format once - a twiddle at 2^-24 is closer to the true value than
// anything the arithmetic around it keeps.

namespace ac3::internal {

// The pair's working value (see "No saturation" above). The FFT kernel takes
// it as its VecType, the lane type fft_kernel.hpp's batched callers use, with
// Fixed32 twiddles, so the kernel's text and its floating instantiations are
// untouched.
struct ImdctValue {
    std::int32_t raw = 0;

    friend constexpr ImdctValue operator+(ImdctValue a, ImdctValue b) {
        return {static_cast<std::int32_t>(static_cast<std::uint32_t>(a.raw) +
                                          static_cast<std::uint32_t>(b.raw))};
    }
    friend constexpr ImdctValue operator-(ImdctValue a, ImdctValue b) {
        return {static_cast<std::int32_t>(static_cast<std::uint32_t>(a.raw) -
                                          static_cast<std::uint32_t>(b.raw))};
    }
    friend constexpr ImdctValue operator-(ImdctValue a) {
        return {static_cast<std::int32_t>(0U - static_cast<std::uint32_t>(a.raw))};
    }
    // Times a twiddle or a window entry: Fixed32's product rounded half up,
    // without the saturation test (fixed32.hpp's product_unsaturated).
    friend constexpr ImdctValue operator*(ImdctValue a, Fixed32 w) {
        return {Fixed32::product_unsaturated(Fixed32::from_raw(a.raw), w).raw};
    }
};

[[nodiscard]] constexpr ImdctValue imdct_value(Fixed32 v) {
    return {v.raw};
}

struct FixedImdctTables {
    static constexpr std::size_t kN = 512;
    // §7.9.4.1 step 2: xcos1[k] = -cos(2pi(8k+1)/8N), xsin1[k] = -sin(...).
    std::array<Fixed32, kN / 4> cos1{};
    std::array<Fixed32, kN / 4> sin1{};
    // §7.9.4.2 step 2: xcos2[k] = -cos(2pi(8k+1)/4N), xsin2[k] = -sin(...).
    std::array<Fixed32, kN / 8> cos2{};
    std::array<Fixed32, kN / 8> sin2{};
    // The shared kernel's own tables at the two sizes the pair needs.
    FftTables<kN / 4, Fixed32> fft128{};
    FftTables<kN / 8, Fixed32> fft64{};
    // §7.9.4.1 step 5's window, the double table rounded once.
    std::array<Fixed32, kN> window{};

    FixedImdctTables() {
        constexpr double kPi = std::numbers::pi;
        for (std::size_t k = 0; k < kN / 4; ++k) {
            const double angle = 2.0 * kPi * (8.0 * static_cast<double>(k) + 1.0) /
                                 (8.0 * static_cast<double>(kN));
            cos1[k] = Fixed32{-std::cos(angle)};
            sin1[k] = Fixed32{-std::sin(angle)};
        }
        for (std::size_t k = 0; k < kN / 8; ++k) {
            const double angle = 2.0 * kPi * (8.0 * static_cast<double>(k) + 1.0) /
                                 (4.0 * static_cast<double>(kN));
            cos2[k] = Fixed32{-std::cos(angle)};
            sin2[k] = Fixed32{-std::sin(angle)};
        }
        for (std::size_t i = 0; i < kN; ++i) {
            window[i] = Fixed32{kAnalysisWindow[i]};
        }
    }
};

inline const FixedImdctTables& fixed_imdct_tables() {
    static const FixedImdctTables t;
    return t;
}

// The largest coefficient magnitude the pair accepts, in raw units: one half.
// See the header comment for where the bound comes from.
inline constexpr std::int32_t kFixedImdctInputLimit = Fixed32::kOne / 2;

// §7.9.4.1: the 512-sample transform, windowed. Every |coeffs[k]| must be
// below one half (kFixedImdctInputLimit); see above.
inline void imdct512_windowed_fixed(std::span<const Fixed32, 256> coeffs,
                                    std::span<Fixed32, 512> x) {
    const auto& t = fixed_imdct_tables();
    constexpr std::size_t kQuarter = FixedImdctTables::kN / 4;  // 128
    constexpr std::size_t kEighth = FixedImdctTables::kN / 8;   // 64
    constexpr std::size_t kHalfN = FixedImdctTables::kN / 2;    // 256

    // Steps 2 and 3: Z[k] = (X[N/2-2k-1] + j X[2k]) (xcos1[k] + j xsin1[k]),
    // written conjugated and digit-reversed for the kernel, then the
    // inverse DFT as conj(FFT(conj(Z))) - the identity mdct.cpp's fast
    // branch uses.
    std::array<ImdctValue, kQuarter> z_re{};
    std::array<ImdctValue, kQuarter> z_im{};
    for (std::size_t k = 0; k < kQuarter; ++k) {
        const ImdctValue a = imdct_value(coeffs[kHalfN - (2 * k) - 1]);
        const ImdctValue b = imdct_value(coeffs[2 * k]);
        const Fixed32 c = t.cos1[k];
        const Fixed32 s = t.sin1[k];
        const std::size_t d = t.fft128.bitrev[k];
        z_re[d] = (a * c) - (b * s);
        z_im[d] = -((b * c) + (a * s));
    }
    fft_forward_bitrev<kQuarter, ImdctValue, Fixed32>(t.fft128, z_re, z_im);

    // Step 4: the conjugation back and the post-twiddle in one pass:
    // y[n] = conj(Z[n]) (xcos1[n] + j xsin1[n]).
    std::array<ImdctValue, kQuarter> y_re{};
    std::array<ImdctValue, kQuarter> y_im{};
    for (std::size_t n = 0; n < kQuarter; ++n) {
        const ImdctValue tr = z_re[n];
        const ImdctValue ti = -z_im[n];
        const Fixed32 c = t.cos1[n];
        const Fixed32 s = t.sin1[n];
        y_re[n] = (tr * c) - (ti * s);
        y_im[n] = (ti * c) + (tr * s);
    }

    // Step 5: windowing and de-interleaving, the same field-for-field
    // transcription as the double form's.
    const auto& w = t.window;
    const auto out = [&x](std::size_t i, ImdctValue v) { x[i] = Fixed32::from_raw(v.raw); };
    for (std::size_t n = 0; n < kEighth; ++n) {
        out(2 * n, -y_im[kEighth + n] * w[2 * n]);
        out((2 * n) + 1, y_re[kEighth - n - 1] * w[(2 * n) + 1]);
        out(kQuarter + (2 * n), -y_re[n] * w[kQuarter + (2 * n)]);
        out(kQuarter + (2 * n) + 1, y_im[kQuarter - n - 1] * w[kQuarter + (2 * n) + 1]);
        out(kHalfN + (2 * n), -y_re[kEighth + n] * w[kHalfN - (2 * n) - 1]);
        out(kHalfN + (2 * n) + 1, y_im[kEighth - n - 1] * w[kHalfN - (2 * n) - 2]);
        out((3 * kQuarter) + (2 * n), y_im[n] * w[kQuarter - (2 * n) - 1]);
        out((3 * kQuarter) + (2 * n) + 1, -y_re[kQuarter - n - 1] * w[kQuarter - (2 * n) - 2]);
    }
}

// §7.9.4.2: the two 256-sample transforms of a block-switched channel,
// windowed into the same 512 samples. The same precondition; the 64-point
// FFTs grow by six bits, so the margin is wider.
inline void imdct256_pair_windowed_fixed(std::span<const Fixed32, 256> coeffs,
                                         std::span<Fixed32, 512> x) {
    const auto& t = fixed_imdct_tables();
    constexpr std::size_t kQuarter = FixedImdctTables::kN / 4;  // 128
    constexpr std::size_t kEighth = FixedImdctTables::kN / 8;   // 64
    constexpr std::size_t kHalfN = FixedImdctTables::kN / 2;    // 256

    // Step 1: the two half-block sets are the even and odd coefficients.
    // Steps 2 and 3, as the long form's, once per set.
    std::array<ImdctValue, kEighth> z1_re{};
    std::array<ImdctValue, kEighth> z1_im{};
    std::array<ImdctValue, kEighth> z2_re{};
    std::array<ImdctValue, kEighth> z2_im{};
    for (std::size_t k = 0; k < kEighth; ++k) {
        const Fixed32 c = t.cos2[k];
        const Fixed32 s = t.sin2[k];
        // x1[i] = coeffs[2i], x2[i] = coeffs[2i+1]; the gathers below read
        // x1[N/4-2k-1], x1[2k] and the same of x2 straight out of coeffs.
        const ImdctValue a1 = imdct_value(coeffs[2 * (kQuarter - (2 * k) - 1)]);
        const ImdctValue b1 = imdct_value(coeffs[2 * (2 * k)]);
        const ImdctValue a2 = imdct_value(coeffs[(2 * (kQuarter - (2 * k) - 1)) + 1]);
        const ImdctValue b2 = imdct_value(coeffs[(2 * (2 * k)) + 1]);
        const std::size_t d = t.fft64.bitrev[k];
        z1_re[d] = (a1 * c) - (b1 * s);
        z1_im[d] = -((b1 * c) + (a1 * s));
        z2_re[d] = (a2 * c) - (b2 * s);
        z2_im[d] = -((b2 * c) + (a2 * s));
    }
    fft_forward_bitrev<kEighth, ImdctValue, Fixed32>(t.fft64, z1_re, z1_im);
    fft_forward_bitrev<kEighth, ImdctValue, Fixed32>(t.fft64, z2_re, z2_im);

    // Step 4, both sets.
    std::array<ImdctValue, kEighth> y1_re{};
    std::array<ImdctValue, kEighth> y1_im{};
    std::array<ImdctValue, kEighth> y2_re{};
    std::array<ImdctValue, kEighth> y2_im{};
    for (std::size_t n = 0; n < kEighth; ++n) {
        const Fixed32 c = t.cos2[n];
        const Fixed32 s = t.sin2[n];
        const ImdctValue t1r = z1_re[n];
        const ImdctValue t1i = -z1_im[n];
        const ImdctValue t2r = z2_re[n];
        const ImdctValue t2i = -z2_im[n];
        y1_re[n] = (t1r * c) - (t1i * s);
        y1_im[n] = (t1i * c) + (t1r * s);
        y2_re[n] = (t2r * c) - (t2i * s);
        y2_im[n] = (t2i * c) + (t2r * s);
    }

    // Step 5, N = 512 throughout as the spec's own note has it.
    const auto& w = t.window;
    const auto out = [&x](std::size_t i, ImdctValue v) { x[i] = Fixed32::from_raw(v.raw); };
    for (std::size_t n = 0; n < kEighth; ++n) {
        out(2 * n, -y1_im[n] * w[2 * n]);
        out((2 * n) + 1, y1_re[kEighth - n - 1] * w[(2 * n) + 1]);
        out(kQuarter + (2 * n), -y1_re[n] * w[kQuarter + (2 * n)]);
        out(kQuarter + (2 * n) + 1, y1_im[kEighth - n - 1] * w[kQuarter + (2 * n) + 1]);
        out(kHalfN + (2 * n), -y2_re[n] * w[kHalfN - (2 * n) - 1]);
        out(kHalfN + (2 * n) + 1, y2_im[kEighth - n - 1] * w[kHalfN - (2 * n) - 2]);
        out((3 * kQuarter) + (2 * n), y2_im[n] * w[kQuarter - (2 * n) - 1]);
        out((3 * kQuarter) + (2 * n) + 1, -y2_re[kEighth - n - 1] * w[kQuarter - (2 * n) - 2]);
    }
}

}  // namespace ac3::internal
