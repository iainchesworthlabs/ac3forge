#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>

#include "iclforge/ac4core/dsp/complex.hpp"

// The passes of dsp/fft.hpp's Stockham transform, one function for each radix and
// direction, with nothing a pass does decided at run time.
//
// A pass takes a[i] = x[q + s (p + i m)] for i < r, forms the radix-r butterfly b[k] =
// sum_i a[i] e^(-/+2 pi i ik/r) and writes b[k] w^(pk) to y[q + s (r p + k)], for p < m
// and q < s, where w^(pk) is the factor the plan holds for the pass (twiddles[p r + k]),
// conjugated in the inverse direction. This is what the plan's loop did with a radix
// switch and a pair of five-element arrays inside the innermost loop; here the radix and
// the direction are template arguments, so the butterfly is straight-line code on the
// registers, which on a core with a single-precision FPU and no out-of-order issue is a
// factor of three or more (planning/ac4.md, D14e).
//
// Every statement performs the operations of the loop it replaces, in the same order, so
// the outputs are the same bits at every scalar (tests/ac4core/test_ac4core_dsp_exact.cpp
// holds each to a verbatim copy of that loop). Two rewrites are not a copy of the old
// text, and each is an identity of IEEE 754 arithmetic rather than a reassociation:
//
//   x - (-y) is x + y, and (-x) + y is y - x, since a subtraction is the addition of
//   the negated operand and an addition commutes. The old code multiplied by the
//   conjugate of a factor, c = (w.re, -w.im), where the products b.re c.im and b.im c.im
//   are exactly the negations of b.re w.im and b.im w.im, so b c is
//   (b.re w.re + b.im w.im, b.im w.re - b.re w.im) with the same two products rounded
//   the same way and the same sum of them. A factor of 1 is not skipped: a product with
//   it can change the sign of a zero, and so a bit of the output.

namespace iclforge::ac4::detail::dsp::fft_kernels {

// One pass's description, as the plan lays it out (dsp/fft.hpp).
struct Stage {
    int radix = 0;
    std::size_t n = 0;        // the sub-transform length this pass splits
    std::size_t stride = 0;   // how many sub-transforms run side by side
    std::size_t twiddle = 0;  // offset of this pass's factors in the plan's table
};

// b * w, or b * conj(w) in the inverse direction.
template <bool Inverse, typename Real>
[[nodiscard]] inline Complex<Real> turn(Complex<Real> b, Complex<Real> w) noexcept {
    if constexpr (Inverse) {
        return {b.re * w.re + b.im * w.im, b.im * w.re - b.re * w.im};
    } else {
        return {b.re * w.re - b.im * w.im, b.re * w.im + b.im * w.re};
    }
}

template <typename Real, bool Inverse>
inline void butterfly2(const Complex<Real>* a, Complex<Real>* b) noexcept {
    b[0] = a[0] + a[1];
    b[1] = a[0] - a[1];
}

// Forward: d13 = (a1 - a3) times -i; inverse: times +i.
template <typename Real, bool Inverse>
inline void butterfly4(const Complex<Real>* a, Complex<Real>* b) noexcept {
    const Complex<Real> s02 = a[0] + a[2];
    const Complex<Real> d02 = a[0] - a[2];
    const Complex<Real> s13 = a[1] + a[3];
    const Complex<Real> t = a[1] - a[3];
    b[0] = s02 + s13;
    b[2] = s02 - s13;
    if constexpr (Inverse) {
        b[1] = Complex<Real>{d02.re - t.im, d02.im + t.re};
        b[3] = Complex<Real>{d02.re + t.im, d02.im - t.re};
    } else {
        b[1] = Complex<Real>{d02.re + t.im, d02.im - t.re};
        b[3] = Complex<Real>{d02.re - t.im, d02.im + t.re};
    }
}

// f(std::integral_constant<int, 0>), ... f(std::integral_constant<int, Count - 1>).
template <int Count, typename F>
inline void static_for(F&& f) {
    [&]<int... I>(std::integer_sequence<int, I...>) {
        (f(std::integral_constant<int, I>{}), ...);
    }(std::make_integer_sequence<int, Count>{});
}

// The radix-3 and radix-5 butterflies: b[k] = a[0] + sum over i >= 1 of a[i] roots[ik mod r],
// each product turned by the root, the terms added in the order of i.
template <typename Real, int Radix, bool Inverse>
inline void butterfly_odd(const Complex<Real>* a, Complex<Real>* b,
                          const Complex<Real>* roots) noexcept {
    static_for<Radix>([&](auto kc) {
        constexpr int k = decltype(kc)::value;
        Complex<Real> sum = a[0];
        static_for<Radix - 1>([&](auto ic) {
            constexpr int i = decltype(ic)::value + 1;
            sum += turn<Inverse>(a[i], roots[(i * k) % Radix]);
        });
        b[k] = sum;
    });
}

template <typename Real, int Radix, bool Inverse>
inline void butterfly(const Complex<Real>* a, Complex<Real>* b, const Complex<Real>* roots3,
                      const Complex<Real>* roots5) noexcept {
    if constexpr (Radix == 2) {
        butterfly2<Real, Inverse>(a, b);
    } else if constexpr (Radix == 4) {
        butterfly4<Real, Inverse>(a, b);
    } else if constexpr (Radix == 3) {
        butterfly_odd<Real, 3, Inverse>(a, b, roots3);
    } else {
        butterfly_odd<Real, 5, Inverse>(a, b, roots5);
    }
}

// One pass over inputs `in(index)` (a callable returning the Complex at that index of the
// pass's input) into y. Stride, when known at compile time (the first pass has 1), takes
// the loop over q out.
template <typename Real, int Radix, bool Inverse, typename In>
inline void pass(const In& in, Complex<Real>* y, std::size_t m, std::size_t s,
                 const Complex<Real>* tw, const Complex<Real>* roots3,
                 const Complex<Real>* roots5) noexcept {
    constexpr auto r = static_cast<std::size_t>(Radix);
    const std::size_t step = s * m;
    for (std::size_t p = 0; p < m; ++p) {
        const Complex<Real>* w = tw + p * r;
        Complex<Real>* out = y + s * r * p;
        for (std::size_t q = 0; q < s; ++q) {
            Complex<Real> a[r];
            Complex<Real> b[r];
            const std::size_t first = q + s * p;
            static_for<Radix>([&](auto ic) {
                constexpr int i = decltype(ic)::value;
                a[i] = in(first + static_cast<std::size_t>(i) * step);
            });
            butterfly<Real, Radix, Inverse>(a, b, roots3, roots5);
            static_for<Radix>([&](auto kc) {
                constexpr int k = decltype(kc)::value;
                out[q + static_cast<std::size_t>(k) * s] = turn<Inverse>(b[k], w[k]);
            });
        }
    }
}

// The pass `stage` over the plain array x into y.
template <typename Real, bool Inverse>
inline void pass_from_array(const Stage& stage, const Complex<Real>* x, Complex<Real>* y,
                            const Complex<Real>* twiddles, const Complex<Real>* roots3,
                            const Complex<Real>* roots5) noexcept {
    const std::size_t m = stage.n / static_cast<std::size_t>(stage.radix);
    const Complex<Real>* tw = twiddles + stage.twiddle;
    const auto in = [x](std::size_t index) noexcept { return x[index]; };
    switch (stage.radix) {
        case 2:
            pass<Real, 2, Inverse>(in, y, m, stage.stride, tw, roots3, roots5);
            break;
        case 3:
            pass<Real, 3, Inverse>(in, y, m, stage.stride, tw, roots3, roots5);
            break;
        case 4:
            pass<Real, 4, Inverse>(in, y, m, stage.stride, tw, roots3, roots5);
            break;
        default:
            pass<Real, 5, Inverse>(in, y, m, stage.stride, tw, roots3, roots5);
            break;
    }
}

// All passes of a plan, the first reading `first(index)` and the others the two buffers
// in turn: the first writes b, the second a, and so on. Returns the buffer the last pass
// wrote. The first pass is radix 4 in every plan this library builds for a transform
// length that is a multiple of 4, and then it takes the fused input; a plan whose first
// pass is another radix gets the same input read from `first` by that radix's pass.
template <typename Real, bool Inverse, typename First>
[[nodiscard]] inline Complex<Real>* run_stages(const Stage* stages, std::size_t count,
                                               const Complex<Real>* twiddles,
                                               const Complex<Real>* roots3,
                                               const Complex<Real>* roots5, const First& first,
                                               Complex<Real>* a, Complex<Real>* b) noexcept {
    Complex<Real>* in = nullptr;
    Complex<Real>* out = b;
    for (std::size_t i = 0; i < count; ++i) {
        const Stage& stage = stages[i];
        if (i == 0) {
            const std::size_t m = stage.n / static_cast<std::size_t>(stage.radix);
            const Complex<Real>* tw = twiddles + stage.twiddle;
            switch (stage.radix) {
                case 2:
                    pass<Real, 2, Inverse>(first, out, m, stage.stride, tw, roots3, roots5);
                    break;
                case 3:
                    pass<Real, 3, Inverse>(first, out, m, stage.stride, tw, roots3, roots5);
                    break;
                case 4:
                    pass<Real, 4, Inverse>(first, out, m, stage.stride, tw, roots3, roots5);
                    break;
                default:
                    pass<Real, 5, Inverse>(first, out, m, stage.stride, tw, roots3, roots5);
                    break;
            }
        } else {
            pass_from_array<Real, Inverse>(stage, in, out, twiddles, roots3, roots5);
        }
        in = out;
        out = (out == b) ? a : b;
    }
    return in;
}

}  // namespace iclforge::ac4::detail::dsp::fft_kernels
