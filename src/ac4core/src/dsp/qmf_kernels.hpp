#pragma once

#include <array>
#include <cstddef>

#include "dsp/complex.hpp"
#include "dsp/qmf_constants.hpp"
#include "tables/qmf_tables.hpp"

// The steps of one slot of the QMF analysis and synthesis (dsp/qmf.hpp), each
// a loop over planes of Real, with the arithmetic in the order the derivation
// there gives it: windowing, the rotation that packs or unpacks pairs of
// samples, the 64-point transform, and the butterfly that pairs subband k with
// 63 - k. Real and imaginary parts are held in separate arrays throughout, so a
// loop's iterations are independent of each other.
//
// These are the reference. A vector kernel replaces one of them only where it
// performs, in every lane, the operations this one performs, in the same order
// and without fused multiply-add, so that the two give the same bits.

namespace ac4::detail::dsp::qmf {

// Logical block `b` of a delay line of ten blocks, 0 the newest, sits at
// physical block (head + b) mod 10.
[[nodiscard]] constexpr std::size_t physical(std::size_t head, std::size_t b) noexcept {
    const std::size_t p = head + b;
    return p >= 10 ? p - 10 : p;
}

// u[n] = sum over k < 5 of qmf_filt[n + 128 k] QWIN[n + 128 k], n < 128
// (Pseudocode 65), from the analysis's ten blocks of 64 (qmf_filt[64 b + s] is
// block b at s).
template <typename Real>
inline void analysis_window(const Real* filt, std::size_t head, Real* u) noexcept {
    const auto& qwin = tables::kQwin;
    std::array<const Real*, 10> block{};
    for (std::size_t b = 0; b < 10; ++b) {
        block[b] = filt + physical(head, b) * 64;
    }
    for (std::size_t h = 0; h < 2; ++h) {
        for (std::size_t s = 0; s < 64; ++s) {
            const std::size_t n = 64 * h + s;
            Real sum{};
            for (std::size_t k = 0; k < 5; ++k) {
                sum += block[2 * k + h][s] * static_cast<Real>(qwin[128 * k + n]);
            }
            u[n] = sum;
        }
    }
}

// z[m] = (u[2m] + i u[2m + 1]) e^(i pi m / 64), m < 64.
template <typename Real>
inline void analysis_rotate(const Real* u, Real* zr, Real* zi) noexcept {
    const auto& c = kConstants<Real>;
    for (std::size_t m = 0; m < 64; ++m) {
        const Real even = u[2 * m];
        const Real odd = u[2 * m + 1];
        zr[m] = even * c.rot_re[m] - odd * c.rot_im[m];
        zi[m] = even * c.rot_im[m] + odd * c.rot_re[m];
    }
}

// One radix-4 butterfly of the inverse transform: b[k] = sum_i a[i] e^(+2 pi i i k / 4).
template <typename Real>
inline void butterfly4(const Complex<Real>* a, Complex<Real>* b) noexcept {
    const Complex<Real> s02 = a[0] + a[2];
    const Complex<Real> d02 = a[0] - a[2];
    const Complex<Real> s13 = a[1] + a[3];
    const Complex<Real> d13 = a[1] - a[3];
    b[0] = s02 + s13;
    b[2] = s02 - s13;
    // + i d13 for b[1], - i d13 for b[3]; i (x + i y) = -y + i x.
    b[1] = Complex<Real>{d02.re - d13.im, d02.im + d13.re};
    b[3] = Complex<Real>{d02.re + d13.im, d02.im - d13.re};
}

// The 64-point inverse transform, sum_m x[m] e^(+2 pi i k m / 64), unscaled, as
// three radix-4 passes of a Stockham autosort (decimation in frequency): the
// input is in (xr, xi) and clobbered, the result in (yr, yi), in natural order.
// A pass with n the length it splits, s the number of transforms side by side and
// m = n / 4, takes a[i] = x[q + s (p + i m)] and writes b[k] w^(p k) to
// y[q + s (4 p + k)], with w = e^(+2 pi i / n).
template <typename Real>
inline void fft64(Real* xr, Real* xi, Real* yr, Real* yi) noexcept {
    const auto& c = kConstants<Real>;
    Complex<Real> a[4];
    Complex<Real> b[4];
    // n = 64, s = 1: x to y.
    for (std::size_t p = 0; p < 16; ++p) {
        for (std::size_t i = 0; i < 4; ++i) {
            a[i] = Complex<Real>{xr[p + 16 * i], xi[p + 16 * i]};
        }
        butterfly4(a, b);
        yr[4 * p] = b[0].re;
        yi[4 * p] = b[0].im;
        for (std::size_t k = 1; k < 4; ++k) {
            const std::size_t t = (k - 1) * 16 + p;
            const Complex<Real> v = b[k] * Complex<Real>{c.fft1_re[t], c.fft1_im[t]};
            yr[4 * p + k] = v.re;
            yi[4 * p + k] = v.im;
        }
    }
    // n = 16, s = 4: y back to x. The factors of p = 0 are 1.
    for (std::size_t p = 0; p < 4; ++p) {
        for (std::size_t q = 0; q < 4; ++q) {
            for (std::size_t i = 0; i < 4; ++i) {
                a[i] = Complex<Real>{yr[q + 4 * p + 16 * i], yi[q + 4 * p + 16 * i]};
            }
            butterfly4(a, b);
            for (std::size_t k = 0; k < 4; ++k) {
                Complex<Real> v = b[k];
                if (p != 0 && k != 0) {
                    const std::size_t t = (k - 1) * 4 + p;
                    v = v * Complex<Real>{c.fft2_re[t], c.fft2_im[t]};
                }
                xr[q + 16 * p + 4 * k] = v.re;
                xi[q + 16 * p + 4 * k] = v.im;
            }
        }
    }
    // n = 4, s = 16: x to y, with no factors.
    for (std::size_t q = 0; q < 16; ++q) {
        for (std::size_t i = 0; i < 4; ++i) {
            a[i] = Complex<Real>{xr[q + 16 * i], xi[q + 16 * i]};
        }
        butterfly4(a, b);
        for (std::size_t k = 0; k < 4; ++k) {
            yr[q + 16 * k] = b[k].re;
            yi[q + 16 * k] = b[k].im;
        }
    }
}

// Q[k] and Q[63 - k] from Z[k] and Z[63 - k], k < 32 (see dsp/qmf.hpp).
template <typename Real>
inline void analysis_unpack(const Real* zr, const Real* zi, Complex<Real>* out) noexcept {
    const auto& c = kConstants<Real>;
    for (std::size_t k = 0; k < 32; ++k) {
        const Real z0r = zr[k];
        const Real z0i = zi[k];
        const Real z1r = zr[63 - k];
        const Real z1i = zi[63 - k];
        // E = Z[k] + conj Z[63 - k], D = Z[k] - conj Z[63 - k], and -i D for the odd part.
        const Real er = z0r + z1r;
        const Real ei = z0i - z1i;
        const Real dr = z0r - z1r;
        const Real di = z0i + z1i;
        const Real h = c.post_cos[k];
        const Real g = c.post_sin[k];
        // a = e^(-i theta) E / 2 and b = e^(+i theta) (-i D) / 2, theta = pi (2k + 1) / 256.
        const Real ar = er * h + ei * g;
        const Real ai = ei * h - er * g;
        const Real br = di * h + dr * g;
        const Real bi = di * g - dr * h;
        out[k] = Complex<Real>{ar + br, ai + bi};
        out[63 - k] = Complex<Real>{bi - ai, br - ar};
    }
}

// T[k] and T[63 - k] from Q[k] and Q[63 - k], k < 32 (see dsp/qmf.hpp).
template <typename Real>
inline void synthesis_pack(const Complex<Real>* in, Real* tr, Real* ti) noexcept {
    const auto& c = kConstants<Real>;
    for (std::size_t k = 0; k < 32; ++k) {
        const Real q0r = in[k].re;
        const Real q0i = in[k].im;
        const Real q1r = in[63 - k].re;
        const Real q1i = in[63 - k].im;
        // Q[k] -+ i conj Q[63 - k].
        const Real d1r = q0r - q1i;
        const Real d1i = q0i - q1r;
        const Real d2r = q0r + q1i;
        const Real d2i = q0i + q1r;
        // S and D, each turned by its own factor, then T[k] = S + i D and
        // T[63 - k] = conj S + i conj D.
        const Real sr = d1r * c.pre_re[k] - d1i * c.pre_im[k];
        const Real si = d1r * c.pre_im[k] + d1i * c.pre_re[k];
        const Real dr = d2r * c.prho_re[k] - d2i * c.prho_im[k];
        const Real di = d2r * c.prho_im[k] + d2i * c.prho_re[k];
        tr[k] = sr - di;
        ti[k] = si + dr;
        tr[63 - k] = sr + di;
        ti[63 - k] = dr - si;
    }
}

// The 128 values of one slot of qsyn: qsyn[2m] + i qsyn[2m + 1] = F[m] e^(i pi m / 64).
template <typename Real>
inline void synthesis_rotate(const Real* fr, const Real* fi, Real* block) noexcept {
    const auto& c = kConstants<Real>;
    for (std::size_t m = 0; m < 64; ++m) {
        block[2 * m] = fr[m] * c.rot_re[m] - fi[m] * c.rot_im[m];
        block[2 * m + 1] = fr[m] * c.rot_im[m] + fi[m] * c.rot_re[m];
    }
}

// One slot of output (Pseudocode 66): g[128 k + sb] = qsyn_filt[256 k + sb] and
// g[128 k + 64 + sb] = qsyn_filt[256 k + 192 + sb], windowed and summed over the
// ten groups of 64. The delay line's ten blocks are of 128 values each.
template <typename Real>
inline void synthesis_window(const Real* filt, std::size_t head, Real* out) noexcept {
    const auto& qwin = tables::kQwin;
    std::array<const Real*, 10> block{};
    for (std::size_t b = 0; b < 10; ++b) {
        block[b] = filt + physical(head, b) * 128;
    }
    for (std::size_t sb = 0; sb < 64; ++sb) {
        Real sum{};
        for (std::size_t k = 0; k < 5; ++k) {
            sum += block[2 * k][sb] * static_cast<Real>(qwin[128 * k + sb]);
            sum += block[2 * k + 1][64 + sb] * static_cast<Real>(qwin[128 * k + 64 + sb]);
        }
        out[sb] = sum;
    }
}

}  // namespace ac4::detail::dsp::qmf
