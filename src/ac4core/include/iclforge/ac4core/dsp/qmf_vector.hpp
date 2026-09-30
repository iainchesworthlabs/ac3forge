#pragma once

#include <array>
#include <cstddef>

#include "iclforge/arithmetic/detail/simd.hpp"
#include "iclforge/ac4core/dsp/complex.hpp"
#include "iclforge/ac4core/dsp/qmf_constants.hpp"
#include "iclforge/ac4core/dsp/qmf_kernels.hpp"
#include "iclforge/ac4core/tables/qmf_tables.hpp"

// The QMF steps of dsp/qmf_kernels.hpp on the seam's 128-bit vector types
// (src/arithmetic/arch: f64x2 at double, f32x4 at float), one step for one step.
// Each is written to perform, in every lane, the operations its scalar
// counterpart performs, in the same order: one IEEE add, subtract or multiply at a
// time, no fused multiply-add (the build pins -ffp-contract=off, and the seam's
// operators are single instructions), so a result equals the scalar loop's bit for
// bit. tests/ac4core/test_ac4core_dsp.cpp holds each to that, on random data, at
// both scalars.
//
// The planes make the lanes independent: a window's outputs, a transform pass's
// butterflies, and the pairs k and 63 - k of the unpacking each run side by side.
// Where a step's memory order does not match its lanes - the analysis's
// even and odd samples, the synthesis's interleaved output, the partner of a pair
// counted down from 63 - the values are gathered and scattered through the seam's
// set() and lane accessors, which are a load or a store each and no arithmetic.

namespace iclforge::ac4::detail::dsp::qmf::vec {

namespace arch = iclforge::internal::arch;

template <typename Real>
struct Lanes;

template <>
struct Lanes<float> {
    using V = arch::f32x4;
    static constexpr std::size_t kCount = 4;
    // The window is a table of float in every build (tables/qmf_tables.hpp).
    [[nodiscard]] static V load_window(const float* p) noexcept { return V::load(p); }
};

template <>
struct Lanes<double> {
    using V = arch::f64x2;
    static constexpr std::size_t kCount = 2;
    [[nodiscard]] static V load_window(const float* p) noexcept {
        return V::set(static_cast<double>(p[0]), static_cast<double>(p[1]));
    }
};

// A vector holding f(0) to f(kCount - 1) in its lanes.
template <typename Real, typename F>
[[nodiscard]] inline typename Lanes<Real>::V gather(F&& f) noexcept {
    using V = typename Lanes<Real>::V;
    if constexpr (Lanes<Real>::kCount == 2) {
        return V::set(f(std::size_t{0}), f(std::size_t{1}));
    } else {
        return V::set(f(std::size_t{0}), f(std::size_t{1}), f(std::size_t{2}), f(std::size_t{3}));
    }
}

// f(lane, value) for each lane of v.
template <typename Real, typename F>
inline void scatter(typename Lanes<Real>::V v, F&& f) noexcept {
    f(std::size_t{0}, v.lane0());
    f(std::size_t{1}, v.lane1());
    if constexpr (Lanes<Real>::kCount == 4) {
        f(std::size_t{2}, v.lane2());
        f(std::size_t{3}, v.lane3());
    }
}

// One complex value per lane, real and imaginary parts apart.
template <typename Real>
struct CV {
    typename Lanes<Real>::V re;
    typename Lanes<Real>::V im;
};

template <typename Real>
[[nodiscard]] inline CV<Real> operator+(CV<Real> a, CV<Real> b) noexcept {
    return {a.re + b.re, a.im + b.im};
}
template <typename Real>
[[nodiscard]] inline CV<Real> operator-(CV<Real> a, CV<Real> b) noexcept {
    return {a.re - b.re, a.im - b.im};
}
// The plain cross form, as dsp::Complex's operator*.
template <typename Real>
[[nodiscard]] inline CV<Real> operator*(CV<Real> a, CV<Real> b) noexcept {
    return {a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re};
}

template <typename Real>
[[nodiscard]] inline CV<Real> load_cv(const Real* re, const Real* im) noexcept {
    using V = typename Lanes<Real>::V;
    return {V::load(re), V::load(im)};
}
template <typename Real>
[[nodiscard]] inline CV<Real> broadcast_cv(Real re, Real im) noexcept {
    using V = typename Lanes<Real>::V;
    return {V::broadcast(re), V::broadcast(im)};
}

// butterfly4() of qmf_kernels.hpp, a butterfly to a lane.
template <typename Real>
inline void butterfly4(const CV<Real>* a, CV<Real>* b) noexcept {
    const CV<Real> s02 = a[0] + a[2];
    const CV<Real> d02 = a[0] - a[2];
    const CV<Real> s13 = a[1] + a[3];
    const CV<Real> d13 = a[1] - a[3];
    b[0] = s02 + s13;
    b[2] = s02 - s13;
    b[1] = CV<Real>{d02.re - d13.im, d02.im + d13.re};
    b[3] = CV<Real>{d02.re + d13.im, d02.im - d13.re};
}

// analysis_window(): the outputs n = 64 h + s side by side over s.
template <typename Real>
inline void analysis_window(const Real* filt, std::size_t head, Real* u) noexcept {
    using L = Lanes<Real>;
    using V = typename L::V;
    const auto& qwin = tables::kQwin;
    std::array<const Real*, 10> block{};
    for (std::size_t b = 0; b < 10; ++b) {
        block[b] = filt + physical(head, b) * 64;
    }
    for (std::size_t h = 0; h < 2; ++h) {
        for (std::size_t s = 0; s < 64; s += L::kCount) {
            V sum = V::broadcast(Real{});
            for (std::size_t k = 0; k < 5; ++k) {
                sum = sum +
                      V::load(block[2 * k + h] + s) * L::load_window(&qwin[128 * k + 64 * h + s]);
            }
            sum.store(u + 64 * h + s);
        }
    }
}

// analysis_rotate(): z[m] for kCount values of m at once, from u[2m] and u[2m + 1].
template <typename Real>
inline void analysis_rotate(const Real* u, Real* zr, Real* zi) noexcept {
    using L = Lanes<Real>;
    using V = typename L::V;
    const auto& c = kConstants<Real>;
    for (std::size_t m = 0; m < 64; m += L::kCount) {
        const V even = gather<Real>([&](std::size_t l) { return u[2 * (m + l)]; });
        const V odd = gather<Real>([&](std::size_t l) { return u[2 * (m + l) + 1]; });
        const V rr = V::load(c.rot_re.data() + m);
        const V ri = V::load(c.rot_im.data() + m);
        (even * rr - odd * ri).store(zr + m);
        (even * ri + odd * rr).store(zi + m);
    }
}

// fft64(): the three passes, the first with the butterflies of kCount values of p
// side by side and the others with those of kCount values of q.
template <typename Real>
inline void fft64(Real* xr, Real* xi, Real* yr, Real* yi) noexcept {
    using L = Lanes<Real>;
    constexpr std::size_t kN = L::kCount;
    const auto& c = kConstants<Real>;
    std::array<CV<Real>, 4> a{};
    std::array<CV<Real>, 4> b{};
    // n = 64, s = 1: x to y. Its outputs y[4p + k] are stride 4 apart.
    for (std::size_t p = 0; p < 16; p += kN) {
        for (std::size_t i = 0; i < 4; ++i) {
            a[i] = load_cv(xr + p + 16 * i, xi + p + 16 * i);
        }
        butterfly4(a.data(), b.data());
        for (std::size_t k = 1; k < 4; ++k) {
            const std::size_t t = (k - 1) * 16 + p;
            b[k] = b[k] * load_cv(c.fft1_re.data() + t, c.fft1_im.data() + t);
        }
        for (std::size_t k = 0; k < 4; ++k) {
            scatter<Real>(b[k].re, [&](std::size_t l, Real v) { yr[4 * (p + l) + k] = v; });
            scatter<Real>(b[k].im, [&](std::size_t l, Real v) { yi[4 * (p + l) + k] = v; });
        }
    }
    // n = 16, s = 4: y back to x. The factors of p = 0 are 1.
    for (std::size_t p = 0; p < 4; ++p) {
        for (std::size_t q = 0; q < 4; q += kN) {
            for (std::size_t i = 0; i < 4; ++i) {
                a[i] = load_cv(yr + q + 4 * p + 16 * i, yi + q + 4 * p + 16 * i);
            }
            butterfly4(a.data(), b.data());
            if (p != 0) {
                for (std::size_t k = 1; k < 4; ++k) {
                    const std::size_t t = (k - 1) * 4 + p;
                    b[k] = b[k] * broadcast_cv(c.fft2_re[t], c.fft2_im[t]);
                }
            }
            for (std::size_t k = 0; k < 4; ++k) {
                b[k].re.store(xr + q + 16 * p + 4 * k);
                b[k].im.store(xi + q + 16 * p + 4 * k);
            }
        }
    }
    // n = 4, s = 16: x to y, with no factors.
    for (std::size_t q = 0; q < 16; q += kN) {
        for (std::size_t i = 0; i < 4; ++i) {
            a[i] = load_cv(xr + q + 16 * i, xi + q + 16 * i);
        }
        butterfly4(a.data(), b.data());
        for (std::size_t k = 0; k < 4; ++k) {
            b[k].re.store(yr + q + 16 * k);
            b[k].im.store(yi + q + 16 * k);
        }
    }
}

// analysis_unpack(): kCount pairs at once, Z[k] beside Z[63 - k] counted down.
template <typename Real>
inline void analysis_unpack(const Real* zr, const Real* zi, Complex<Real>* out) noexcept {
    using L = Lanes<Real>;
    using V = typename L::V;
    const auto& c = kConstants<Real>;
    for (std::size_t k = 0; k < 32; k += L::kCount) {
        const V z0r = V::load(zr + k);
        const V z0i = V::load(zi + k);
        const V z1r = gather<Real>([&](std::size_t l) { return zr[63 - (k + l)]; });
        const V z1i = gather<Real>([&](std::size_t l) { return zi[63 - (k + l)]; });
        const V er = z0r + z1r;
        const V ei = z0i - z1i;
        const V dr = z0r - z1r;
        const V di = z0i + z1i;
        const V h = V::load(c.post_cos.data() + k);
        const V g = V::load(c.post_sin.data() + k);
        const V ar = er * h + ei * g;
        const V ai = ei * h - er * g;
        const V br = di * h + dr * g;
        const V bi = di * g - dr * h;
        const V low_re = ar + br;
        const V low_im = ai + bi;
        const V high_re = bi - ai;
        const V high_im = br - ar;
        scatter<Real>(low_re, [&](std::size_t l, Real v) { out[k + l].re = v; });
        scatter<Real>(low_im, [&](std::size_t l, Real v) { out[k + l].im = v; });
        scatter<Real>(high_re, [&](std::size_t l, Real v) { out[63 - (k + l)].re = v; });
        scatter<Real>(high_im, [&](std::size_t l, Real v) { out[63 - (k + l)].im = v; });
    }
}

// synthesis_pack(): kCount pairs at once from the interleaved subband samples.
template <typename Real>
inline void synthesis_pack(const Complex<Real>* in, Real* tr, Real* ti) noexcept {
    using L = Lanes<Real>;
    using V = typename L::V;
    const auto& c = kConstants<Real>;
    for (std::size_t k = 0; k < 32; k += L::kCount) {
        const V q0r = gather<Real>([&](std::size_t l) { return in[k + l].re; });
        const V q0i = gather<Real>([&](std::size_t l) { return in[k + l].im; });
        const V q1r = gather<Real>([&](std::size_t l) { return in[63 - (k + l)].re; });
        const V q1i = gather<Real>([&](std::size_t l) { return in[63 - (k + l)].im; });
        const V d1r = q0r - q1i;
        const V d1i = q0i - q1r;
        const V d2r = q0r + q1i;
        const V d2i = q0i + q1r;
        const V pre_re = V::load(c.pre_re.data() + k);
        const V pre_im = V::load(c.pre_im.data() + k);
        const V prho_re = V::load(c.prho_re.data() + k);
        const V prho_im = V::load(c.prho_im.data() + k);
        const V sr = d1r * pre_re - d1i * pre_im;
        const V si = d1r * pre_im + d1i * pre_re;
        const V dr = d2r * prho_re - d2i * prho_im;
        const V di = d2r * prho_im + d2i * prho_re;
        (sr - di).store(tr + k);
        (si + dr).store(ti + k);
        scatter<Real>(sr + di, [&](std::size_t l, Real v) { tr[63 - (k + l)] = v; });
        scatter<Real>(dr - si, [&](std::size_t l, Real v) { ti[63 - (k + l)] = v; });
    }
}

// synthesis_rotate(): kCount values of m at once, written to qsyn[2m] and qsyn[2m + 1].
template <typename Real>
inline void synthesis_rotate(const Real* fr, const Real* fi, Real* block) noexcept {
    using L = Lanes<Real>;
    using V = typename L::V;
    const auto& c = kConstants<Real>;
    for (std::size_t m = 0; m < 64; m += L::kCount) {
        const V vr = V::load(fr + m);
        const V vi = V::load(fi + m);
        const V rr = V::load(c.rot_re.data() + m);
        const V ri = V::load(c.rot_im.data() + m);
        scatter<Real>(vr * rr - vi * ri, [&](std::size_t l, Real v) { block[2 * (m + l)] = v; });
        scatter<Real>(vr * ri + vi * rr,
                      [&](std::size_t l, Real v) { block[2 * (m + l) + 1] = v; });
    }
}

// synthesis_window(): the outputs sb side by side, each summed over k in the scalar order.
template <typename Real>
inline void synthesis_window(const Real* filt, std::size_t head, Real* out) noexcept {
    using L = Lanes<Real>;
    using V = typename L::V;
    const auto& qwin = tables::kQwin;
    std::array<const Real*, 10> block{};
    for (std::size_t b = 0; b < 10; ++b) {
        block[b] = filt + physical(head, b) * 128;
    }
    for (std::size_t sb = 0; sb < 64; sb += L::kCount) {
        V sum = V::broadcast(Real{});
        for (std::size_t k = 0; k < 5; ++k) {
            sum = sum + V::load(block[2 * k] + sb) * L::load_window(&qwin[128 * k + sb]);
            sum = sum +
                  V::load(block[2 * k + 1] + 64 + sb) * L::load_window(&qwin[128 * k + 64 + sb]);
        }
        sum.store(out + sb);
    }
}

}  // namespace iclforge::ac4::detail::dsp::qmf::vec
