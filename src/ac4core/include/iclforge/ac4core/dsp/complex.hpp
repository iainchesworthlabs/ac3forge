#pragma once

#include <cmath>
#include <cstdlib>
#include <type_traits>

// The project's own complex type, in place of std::complex<Real>
// (planning/ac4.md, "D14a": "a complex type of the project's own in place of
// std::complex<Real> (a fixed-point type cannot instantiate std::complex)").
// AC-4's QMF-domain code (the analysis/synthesis pair, the transforms, A-SPX's
// high-frequency generator, A-CPL's and A-JOC's decorrelation) is templated on
// Real and names its sample through this type or the QmfValue alias built on
// it, so a later phase's fixed-point tier (D14d) can instantiate the same
// code: `std::complex<iclforge::internal::Fixed32>` is not a valid type (the
// standard requires a floating-point scalar), where this one only asks of
// Real what the operators below use.
//
// An aggregate of two named scalars, not a class with invariants to keep -
// the same shape src/arithmetic/arch/*/ac3/internal/arch/simd.hpp's
// f64x2/f32x4 use, and for the same reason: the operations below should read
// as plainly the arithmetic a caller would otherwise have written by hand.
// C++20's parenthesised aggregate initialisation makes `Complex(a, b)`
// construct one exactly as a user-declared two-argument constructor would,
// so every existing call site written against std::complex's constructor
// syntax (`Complex(re, im)`) needs no change beyond the type it now names.
//
// operator/ divides by Smith's algorithm (scale by the larger component's
// ratio before dividing, C99 Annex G.5.1's _Cdivd), the same numerically
// stable form every major standard library's std::complex<T>::operator/=
// already uses - not for speed, but so that swapping this type in for
// std::complex<Real> at Real = double does not move a decoded stream's last
// bit by way of a different division algorithm. operator+, - and * are each
// one floating operation per component (or, for *, the plain four-multiply
// cross form), which no implementation has room to compute any way other
// than the one this file writes, so those three are bit-identical to
// std::complex<double>'s by construction rather than by matching effort.
//
// AC-4's inputs are always finite, ordinary-magnitude audio-derived values;
// this type makes no attempt at std::complex's NaN/infinity edge-case
// handling (C99 Annex G's proviso for multiplication), which never matters
// for one.

namespace iclforge::ac4::detail::dsp {

template <typename Real>
struct Complex {
    using value_type = Real;  // std::complex's own name for it; a generic caller reads it back.

    Real re{};
    Real im{};

    [[nodiscard]] constexpr Real real() const noexcept { return re; }
    [[nodiscard]] constexpr Real imag() const noexcept { return im; }

    friend constexpr Complex operator+(Complex a) noexcept { return a; }
    friend constexpr Complex operator-(Complex a) noexcept { return {-a.re, -a.im}; }

    friend constexpr Complex operator+(Complex a, Complex b) noexcept {
        return {a.re + b.re, a.im + b.im};
    }
    friend constexpr Complex operator+(Complex a, Real b) noexcept { return {a.re + b, a.im}; }
    friend constexpr Complex operator+(Real a, Complex b) noexcept { return {a + b.re, b.im}; }

    friend constexpr Complex operator-(Complex a, Complex b) noexcept {
        return {a.re - b.re, a.im - b.im};
    }
    friend constexpr Complex operator-(Complex a, Real b) noexcept { return {a.re - b, a.im}; }
    friend constexpr Complex operator-(Real a, Complex b) noexcept { return {a - b.re, -b.im}; }

    // The plain cross form: for the finite, ordinary-magnitude values AC-4
    // ever multiplies, this is the value std::complex<Real>::operator* gives
    // too (see this file's header comment).
    friend constexpr Complex operator*(Complex a, Complex b) noexcept {
        return {a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re};
    }
    friend constexpr Complex operator*(Complex a, Real b) noexcept { return {a.re * b, a.im * b}; }
    friend constexpr Complex operator*(Real a, Complex b) noexcept { return {a * b.re, a * b.im}; }

    // Smith's algorithm (see this file's header comment): scale by the
    // smaller-magnitude component's ratio to the larger, so the divisor never
    // squares a value the format could overflow, and every major standard
    // library's std::complex<T>::operator/= computes the same two branches.
    // Not constexpr: std::abs(Real) is not, and nothing needs this one to be -
    // a decoder never divides two complex values in a constant expression.
    friend Complex operator/(Complex a, Complex b) noexcept {
        // std::abs at double and float; iclforge::internal's, by argument-dependent lookup, at
        // Fixed32 and MantExp.
        using std::abs;
        if (abs(b.re) >= abs(b.im)) {
            const Real r = b.im / b.re;
            const Real d = b.re + b.im * r;
            return {(a.re + a.im * r) / d, (a.im - a.re * r) / d};
        }
        const Real r = b.re / b.im;
        const Real d = b.re * r + b.im;
        return {(a.re * r + a.im) / d, (a.im * r - a.re) / d};
    }
    friend constexpr Complex operator/(Complex a, Real b) noexcept { return {a.re / b, a.im / b}; }
    friend Complex operator/(Real a, Complex b) noexcept { return Complex{a, Real{}} / b; }

    friend constexpr bool operator==(Complex a, Complex b) noexcept {
        return a.re == b.re && a.im == b.im;
    }
    friend constexpr bool operator==(Complex a, Real b) noexcept { return a.re == b && a.im == Real{}; }

    // In terms of the binary operators above, exactly as Fixed32's own
    // compound assignments are (fixed32.hpp) - one rule, stated once.
    constexpr Complex& operator+=(Complex o) noexcept { return *this = *this + o; }
    constexpr Complex& operator+=(Real o) noexcept { return *this = *this + o; }
    constexpr Complex& operator-=(Complex o) noexcept { return *this = *this - o; }
    constexpr Complex& operator-=(Real o) noexcept { return *this = *this - o; }
    constexpr Complex& operator*=(Complex o) noexcept { return *this = *this * o; }
    constexpr Complex& operator*=(Real o) noexcept { return *this = *this * o; }
    Complex& operator/=(Complex o) noexcept { return *this = *this / o; }
    constexpr Complex& operator/=(Real o) noexcept { return *this = *this / o; }
};

template <typename Real>
[[nodiscard]] constexpr Complex<Real> conj(Complex<Real> z) noexcept {
    return {z.re, -z.im};
}

// |z|^2: a sum of two squares, one floating multiply and one add per
// component, the same value std::norm(std::complex<Real>) computes.
template <typename Real>
[[nodiscard]] constexpr Real norm(Complex<Real> z) noexcept {
    return z.re * z.re + z.im * z.im;
}

// std::abs(std::complex<Real>) is defined as std::hypot(re, im) since C++11;
// calling the same library function here gives the same value. A scalar that is
// not floating takes the root of the norm through its own scalar_sqrt.
template <typename Real>
[[nodiscard]] inline Real abs(Complex<Real> z) noexcept {
    if constexpr (std::is_floating_point_v<Real>) {
        return std::hypot(z.re, z.im);
    } else {
        return scalar_sqrt(norm(z));
    }
}

}  // namespace iclforge::ac4::detail::dsp
