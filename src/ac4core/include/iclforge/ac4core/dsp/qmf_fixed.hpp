#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "iclforge/arithmetic/fixed32.hpp"
#include "iclforge/ac4core/dsp/complex.hpp"
#include "iclforge/ac4core/dsp/qmf.hpp"
#include "iclforge/ac4core/dsp/qmf_constants.hpp"
#include "iclforge/ac4core/dsp/qmf_kernels.hpp"
#include "iclforge/ac4core/dsp/scalar_traits.hpp"
#include "iclforge/ac4core/tables/qmf_tables_fixed.hpp"

// One slot of the QMF analysis and synthesis (dsp/qmf.hpp) at Fixed32, with a block exponent
// per slot (planning/ac4.md, D14d). The steps and their order are qmf_kernels.hpp's; what
// differs is where the values sit in the format.
//
// Analysis. The window's five products per output are summed in 64 bits, QWIN in Q1.30
// (tables/qmf_tables_fixed.hpp) against samples in Q7.24, below 2^62 on any input. The slot's
// 128 sums are then shifted, together and once, so that the largest lies in [1/4, 1/2): a
// quiet slot keeps its bits through the transform, and from below 1/2 neither the rotation
// (|z| <= 0.71), the 64-point transform (64 times), nor the unpacking's sums (twice) can reach
// the format's 128, so every product skips the saturation test. The subband values are then
// shifted back by the slot's exponent, to the QMF domain's scale (dsp/scalar_traits.hpp,
// kTimeShift and kQmfShift).
//
// Synthesis. The slot's 64 subband values are shifted together so that the sum of the
// magnitudes of their real and imaginary parts lies in [8, 16). The packing takes its factors
// without the 1/128 the floating tiers fold into them (1/64 of Pseudocode 66 and 1/2 of the
// unpacking). Each of its differences is a sum of two parts, below 16, so each turned value is
// below 16 sqrt(2) and each packed part below 46; and the packed values' magnitudes sum to at
// most four times the parts' sum, under 64, which bounds every value of the transform and of the
// rotation after it. A bound by the largest part alone would have to allow the transform's
// 64-fold growth, which a sparse slot, a tone's one or two subbands, does not take: the sum
// keeps such a slot's bits.
// The values go into the delay line shifted by the slot's exponent, less the 1/128, at the time
// domain's scale; the window's ten products per output are summed in 64 bits and rounded once.

namespace iclforge::ac4::detail::dsp::qmf::fixed {

using iclforge::internal::Fixed32;

[[nodiscard]] constexpr Fixed32 q(double value) noexcept {
    return Fixed32{value};
}

// The factors of qmf_constants.hpp at Fixed32, the packing's without its 1/128.
struct FixedConstants {
    std::array<Fixed32, 48> fft1_re{};
    std::array<Fixed32, 48> fft1_im{};
    std::array<Fixed32, 12> fft2_re{};
    std::array<Fixed32, 12> fft2_im{};
    std::array<Fixed32, 64> rot_re{};
    std::array<Fixed32, 64> rot_im{};
    std::array<Fixed32, 32> post_cos{};
    std::array<Fixed32, 32> post_sin{};
    std::array<Fixed32, 32> pre_re{};
    std::array<Fixed32, 32> pre_im{};
    std::array<Fixed32, 32> prho_re{};
    std::array<Fixed32, 32> prho_im{};
};

[[nodiscard]] consteval FixedConstants make_fixed_constants() {
    FixedConstants c{};
    for (std::size_t p = 0; p < 16; ++p) {
        for (std::size_t k = 1; k <= 3; ++k) {
            const auto units = static_cast<long long>(8 * p * k);
            c.fft1_re[(k - 1) * 16 + p] = q(cos_units(units));
            c.fft1_im[(k - 1) * 16 + p] = q(sin_units(units));
        }
    }
    for (std::size_t p = 0; p < 4; ++p) {
        for (std::size_t k = 1; k <= 3; ++k) {
            const auto units = static_cast<long long>(32 * p * k);
            c.fft2_re[(k - 1) * 4 + p] = q(cos_units(units));
            c.fft2_im[(k - 1) * 4 + p] = q(sin_units(units));
        }
    }
    for (std::size_t m = 0; m < 64; ++m) {
        const auto units = static_cast<long long>(4 * m);
        c.rot_re[m] = q(cos_units(units));
        c.rot_im[m] = q(sin_units(units));
    }
    for (std::size_t k = 0; k < 32; ++k) {
        const auto odd = static_cast<long long>(2 * k + 1);
        c.post_cos[k] = q(cos_units(odd) / 2.0);
        c.post_sin[k] = q(sin_units(odd) / 2.0);
        c.pre_re[k] = q(cos_units(-255 * odd));
        c.pre_im[k] = q(sin_units(-255 * odd));
        c.prho_re[k] = q(cos_units(-253 * odd));
        c.prho_im[k] = q(sin_units(-253 * odd));
    }
    return c;
}

inline constexpr FixedConstants kFixedConstants = make_fixed_constants();

// A product the bounds above keep below the format's edge.
[[nodiscard]] inline Fixed32 mul(Fixed32 a, Fixed32 b) noexcept {
    return Fixed32::product_unsaturated(a, b);
}

// fft64() of qmf_kernels.hpp, every twiddle product unsaturated.
inline void fft64(Fixed32* xr, Fixed32* xi, Fixed32* yr, Fixed32* yi) noexcept {
    const FixedConstants& c = kFixedConstants;
    Complex<Fixed32> a[4];
    Complex<Fixed32> b[4];
    const auto turn = [](Complex<Fixed32> v, Fixed32 wr, Fixed32 wi) {
        return Complex<Fixed32>{mul(v.re, wr) - mul(v.im, wi), mul(v.re, wi) + mul(v.im, wr)};
    };
    for (std::size_t p = 0; p < 16; ++p) {
        for (std::size_t i = 0; i < 4; ++i) {
            a[i] = Complex<Fixed32>{xr[p + 16 * i], xi[p + 16 * i]};
        }
        butterfly4(a, b);
        yr[4 * p] = b[0].re;
        yi[4 * p] = b[0].im;
        for (std::size_t k = 1; k < 4; ++k) {
            const std::size_t t = (k - 1) * 16 + p;
            const Complex<Fixed32> v = turn(b[k], c.fft1_re[t], c.fft1_im[t]);
            yr[4 * p + k] = v.re;
            yi[4 * p + k] = v.im;
        }
    }
    for (std::size_t p = 0; p < 4; ++p) {
        for (std::size_t r = 0; r < 4; ++r) {
            for (std::size_t i = 0; i < 4; ++i) {
                a[i] = Complex<Fixed32>{yr[r + 4 * p + 16 * i], yi[r + 4 * p + 16 * i]};
            }
            butterfly4(a, b);
            for (std::size_t k = 0; k < 4; ++k) {
                Complex<Fixed32> v = b[k];
                if (p != 0 && k != 0) {
                    const std::size_t t = (k - 1) * 4 + p;
                    v = turn(v, c.fft2_re[t], c.fft2_im[t]);
                }
                xr[r + 16 * p + 4 * k] = v.re;
                xi[r + 16 * p + 4 * k] = v.im;
            }
        }
    }
    for (std::size_t r = 0; r < 16; ++r) {
        for (std::size_t i = 0; i < 4; ++i) {
            a[i] = Complex<Fixed32>{xr[r + 16 * i], xi[r + 16 * i]};
        }
        butterfly4(a, b);
        for (std::size_t k = 0; k < 4; ++k) {
            yr[r + 16 * k] = b[k].re;
            yi[r + 16 * k] = b[k].im;
        }
    }
}

// v rounded half up by 2^shift, a right shift for shift > 0 and an exact left one otherwise,
// for a value the caller has bounded to fit.
[[nodiscard]] inline std::int32_t shift_rounded(std::int64_t v, int shift) noexcept {
    if (shift <= 0) {
        return static_cast<std::int32_t>(v * (std::int64_t{1} << static_cast<unsigned>(-shift)));
    }
    return static_cast<std::int32_t>((v + (std::int64_t{1} << static_cast<unsigned>(shift - 1))) >>
                                     static_cast<unsigned>(shift));
}

// The bit length of the largest magnitude among `values`, 0 when all are zero.
template <typename T, std::size_t N>
[[nodiscard]] inline int bit_length(const std::array<T, N>& values) noexcept {
    using U = std::make_unsigned_t<T>;
    U largest = 0;
    for (const T v : values) {
        const U magnitude = v < 0 ? U{0} - static_cast<U>(v) : static_cast<U>(v);
        largest = magnitude > largest ? magnitude : largest;
    }
    return static_cast<int>(sizeof(U) * 8) - std::countl_zero(largest);
}

// One slot of the analysis from the delay line's ten blocks of 64 (head the newest) into 64
// subband values.
inline void analysis_slot(const Fixed32* filt, std::size_t head, Complex<Fixed32>* out,
                          QmfScratch<Fixed32>& scratch) noexcept {
    const auto& qwin = tables::kQwinQ30;
    std::array<const Fixed32*, 10> block{};
    for (std::size_t b = 0; b < 10; ++b) {
        block[b] = filt + physical(head, b) * 64;
    }
    std::array<std::int64_t, 128> sums{};
    for (std::size_t h = 0; h < 2; ++h) {
        for (std::size_t s = 0; s < 64; ++s) {
            const std::size_t n = 64 * h + s;
            std::int64_t sum = 0;
            for (std::size_t k = 0; k < 5; ++k) {
                sum += static_cast<std::int64_t>(block[2 * k + h][s].raw) * qwin[128 * k + n];
            }
            sums[n] = sum;
        }
    }
    const int bits = bit_length(sums);
    if (bits == 0) {
        for (std::size_t k = 0; k < 64; ++k) {
            out[k] = Complex<Fixed32>{};
        }
        return;
    }
    // The sums are in Q.54; shifted down by `shift` the largest is in [2^22, 2^23).
    const int shift = bits - 23;
    Fixed32* u = scratch.u.data();
    for (std::size_t n = 0; n < 128; ++n) {
        u[n] = Fixed32::from_raw(shift_rounded(sums[n], shift));
    }
    const FixedConstants& c = kFixedConstants;
    for (std::size_t m = 0; m < 64; ++m) {
        const Fixed32 even = u[2 * m];
        const Fixed32 odd = u[2 * m + 1];
        scratch.a_re[m] = mul(even, c.rot_re[m]) - mul(odd, c.rot_im[m]);
        scratch.a_im[m] = mul(even, c.rot_im[m]) + mul(odd, c.rot_re[m]);
    }
    fft64(scratch.a_re.data(), scratch.a_im.data(), scratch.b_re.data(), scratch.b_im.data());
    // The normalised slot is the true one times 2^(30 - shift), and the QMF domain is
    // kTimeShift - kQmfShift bits below the time domain.
    const int back = shift - 30 + kQmfShift<Fixed32> - kTimeShift<Fixed32>;
    for (std::size_t k = 0; k < 32; ++k) {
        const Fixed32 z0r = scratch.b_re[k];
        const Fixed32 z0i = scratch.b_im[k];
        const Fixed32 z1r = scratch.b_re[63 - k];
        const Fixed32 z1i = scratch.b_im[63 - k];
        const Fixed32 er = z0r + z1r;
        const Fixed32 ei = z0i - z1i;
        const Fixed32 dr = z0r - z1r;
        const Fixed32 di = z0i + z1i;
        const Fixed32 h = c.post_cos[k];
        const Fixed32 g = c.post_sin[k];
        const Fixed32 ar = mul(er, h) + mul(ei, g);
        const Fixed32 ai = mul(ei, h) - mul(er, g);
        const Fixed32 br = mul(di, h) + mul(dr, g);
        const Fixed32 bi = mul(di, g) - mul(dr, h);
        out[k] = Complex<Fixed32>{(ar + br).scaled_by_pow2(back), (ai + bi).scaled_by_pow2(back)};
        out[63 - k] = Complex<Fixed32>{(bi - ai).scaled_by_pow2(back), (br - ar).scaled_by_pow2(back)};
    }
}

// One slot of the synthesis: 64 subband values into the delay line's newest block of 128 (at
// `block`), then 64 samples out of the ten blocks (head the newest).
inline void synthesis_slot(const Complex<Fixed32>* in, Fixed32* filt, std::size_t head,
                           Fixed32* pcm, QmfScratch<Fixed32>& scratch) noexcept {
    Fixed32* block_out = filt + head * 128;
    std::uint64_t magnitudes = 0;
    for (std::size_t k = 0; k < 64; ++k) {
        for (const std::int32_t part : {in[k].re.raw, in[k].im.raw}) {
            magnitudes += part < 0 ? 0U - static_cast<std::uint64_t>(static_cast<std::int64_t>(part))
                            : static_cast<std::uint64_t>(part);
        }
    }
    const int bits = 64 - std::countl_zero(magnitudes);
    if (bits == 0) {
        for (std::size_t n = 0; n < 128; ++n) {
            block_out[n] = Fixed32{};
        }
    } else {
        // The sum of the parts' magnitudes shifted into [2^27, 2^28), [8, 16) in Q7.24.
        const int shift = bits - 28;
        std::array<Complex<Fixed32>, 64> norm{};
        for (std::size_t k = 0; k < 64; ++k) {
            norm[k] = Complex<Fixed32>{Fixed32::from_raw(shift_rounded(in[k].re.raw, shift)),
                                       Fixed32::from_raw(shift_rounded(in[k].im.raw, shift))};
        }
        const FixedConstants& c = kFixedConstants;
        for (std::size_t k = 0; k < 32; ++k) {
            const Fixed32 q0r = norm[k].re;
            const Fixed32 q0i = norm[k].im;
            const Fixed32 q1r = norm[63 - k].re;
            const Fixed32 q1i = norm[63 - k].im;
            const Fixed32 d1r = q0r - q1i;
            const Fixed32 d1i = q0i - q1r;
            const Fixed32 d2r = q0r + q1i;
            const Fixed32 d2i = q0i + q1r;
            const Fixed32 sr = mul(d1r, c.pre_re[k]) - mul(d1i, c.pre_im[k]);
            const Fixed32 si = mul(d1r, c.pre_im[k]) + mul(d1i, c.pre_re[k]);
            const Fixed32 dr = mul(d2r, c.prho_re[k]) - mul(d2i, c.prho_im[k]);
            const Fixed32 di = mul(d2r, c.prho_im[k]) + mul(d2i, c.prho_re[k]);
            scratch.a_re[k] = sr - di;
            scratch.a_im[k] = si + dr;
            scratch.a_re[63 - k] = sr + di;
            scratch.a_im[63 - k] = dr - si;
        }
        fft64(scratch.a_re.data(), scratch.a_im.data(), scratch.b_re.data(), scratch.b_im.data());
        // The normalised slot is the true one times 2^-shift, the factors 128 times the
        // floating tiers', and the time domain kTimeShift - kQmfShift bits above the QMF domain.
        const int back = shift - 7 + kTimeShift<Fixed32> - kQmfShift<Fixed32>;
        for (std::size_t m = 0; m < 64; ++m) {
            const Fixed32 fr = scratch.b_re[m];
            const Fixed32 fi = scratch.b_im[m];
            block_out[2 * m] = (mul(fr, c.rot_re[m]) - mul(fi, c.rot_im[m])).scaled_by_pow2(back);
            block_out[2 * m + 1] = (mul(fr, c.rot_im[m]) + mul(fi, c.rot_re[m])).scaled_by_pow2(back);
        }
    }
    const auto& qwin = tables::kQwinQ30;
    std::array<const Fixed32*, 10> block{};
    for (std::size_t b = 0; b < 10; ++b) {
        block[b] = filt + physical(head, b) * 128;
    }
    for (std::size_t sb = 0; sb < 64; ++sb) {
        std::int64_t sum = 0;
        for (std::size_t k = 0; k < 5; ++k) {
            sum += static_cast<std::int64_t>(block[2 * k][sb].raw) * qwin[128 * k + sb];
            sum += static_cast<std::int64_t>(block[2 * k + 1][64 + sb].raw) * qwin[128 * k + 64 + sb];
        }
        const std::int64_t rounded = (sum + (std::int64_t{1} << 29U)) >> 30U;
        constexpr std::int64_t kMax = std::numeric_limits<std::int32_t>::max();
        constexpr std::int64_t kMin = std::numeric_limits<std::int32_t>::min();
        pcm[sb] = Fixed32::from_raw(static_cast<std::int32_t>(rounded > kMax   ? kMax
                                                              : rounded < kMin ? kMin
                                                                               : rounded));
    }
}

}  // namespace iclforge::ac4::detail::dsp::qmf::fixed
