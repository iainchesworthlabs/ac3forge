#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>

#include "iclforge/arithmetic/fixed32.hpp"
#include "iclforge/arithmetic/mant_exp.hpp"
#include "iclforge/ac4core/dsp/complex.hpp"

// What the decoder's tools ask of its scalar beyond arithmetic, so that one source serves the
// double, float and fixed-point tiers (planning/ac4.md, D14d).
//
// At double and float each name below is the scalar itself or the expression the tools wrote
// before the fixed tier existed: an energy is a Real, energy_of() is norm(), apply_gain() is a
// product, and every scale is zero. Their output is the bytes it was.
//
// At Fixed32 (Q7.24) the time domain and the QMF domain sit at fixed powers of two below the
// double decoder's, whose full scale is 2^15 (src/ac4dec/src/pcm/substream_pcm.cpp): the time
// domain at 2^-12, where full scale is 8 and the committed streams' largest sample (0.55 of full
// scale) is 4.4 of the format's 128, and the QMF domain at 2^-16, where their largest value (22
// times full scale in the double decoder) is 11. A raw unit of the time domain is then 2^-27 of
// full scale; at 2^-15, with full scale 1.0, the decode's agreement with double fell 20 dB with
// every 20 dB of level below full scale, from 123 dB on noise at a third of full scale, which
// these three bits move. An energy, a gain or a scale factor is
// a MantExp (iclforge/arithmetic/mant_exp.hpp), a mantissa and a power of two, since they span
// a range no absolute format holds; a value the double decoder states in its own units enters
// this tier's through qmf_amplitude() or qmf_energy(), which move its exponent.

namespace iclforge::ac4::detail::dsp {

using iclforge::internal::Fixed32;
using iclforge::internal::MantExp;

template <typename Real>
inline constexpr bool kFixed = std::is_same_v<Real, Fixed32>;

template <typename Real>
struct EnergyOf {
    using type = Real;
};
template <>
struct EnergyOf<Fixed32> {
    using type = MantExp;
};

// An energy, a gain or a scale factor of the QMF domain at the scalar Real.
template <typename Real>
using Energy = typename EnergyOf<Real>::type;

// The power of two the fixed tier's time-domain and QMF-domain values are at against the
// double decoder's; zero at double and float.
template <typename Real>
inline constexpr int kTimeShift = kFixed<Real> ? -12 : 0;
template <typename Real>
inline constexpr int kQmfShift = kFixed<Real> ? -16 : 0;

// An amplitude or an energy given in the double decoder's QMF units, in this tier's.
template <typename Real>
[[nodiscard]] constexpr Energy<Real> qmf_amplitude(Energy<Real> value) noexcept {
    if constexpr (kFixed<Real>) {
        return value.scaled_by_pow2(kQmfShift<Real>);
    } else {
        return value;
    }
}
template <typename Real>
[[nodiscard]] constexpr Energy<Real> qmf_energy(Energy<Real> value) noexcept {
    if constexpr (kFixed<Real>) {
        return value.scaled_by_pow2(2 * kQmfShift<Real>);
    } else {
        return value;
    }
}

// |z|^2 as an energy: norm() at double and float; at Fixed32 the squares summed exactly in 64
// bits, each below 2^62, and their sum unsigned.
template <typename Real>
[[nodiscard]] constexpr Energy<Real> energy_of(Complex<Real> z) noexcept {
    if constexpr (kFixed<Real>) {
        const auto re = static_cast<std::int64_t>(z.re.raw);
        const auto im = static_cast<std::int64_t>(z.im.raw);
        const std::uint64_t sum = static_cast<std::uint64_t>(re * re) + static_cast<std::uint64_t>(im * im);
        // Halved into the signed range, which the power of two restores.
        return MantExp::make(static_cast<std::int64_t>(sum >> 1U), 1 - 2 * Fixed32::kFractionBits);
    } else {
        return norm(z);
    }
}

template <typename Real>
[[nodiscard]] constexpr Energy<Real> energy_of(Real x) noexcept {
    if constexpr (kFixed<Real>) {
        const auto v = static_cast<std::int64_t>(x.raw);
        return MantExp::make(v * v, -2 * Fixed32::kFractionBits);
    } else {
        return x * x;
    }
}

// The scalar's value as an energy, and an energy as the scalar: identities but at Fixed32.
template <typename Real>
[[nodiscard]] constexpr Energy<Real> to_energy(Real x) noexcept {
    if constexpr (kFixed<Real>) {
        return MantExp{x};
    } else {
        return x;
    }
}
template <typename Real>
[[nodiscard]] constexpr Real from_energy(Energy<Real> x) noexcept {
    if constexpr (kFixed<Real>) {
        return x.to_fixed();
    } else {
        return x;
    }
}

// g x for a gain or level `g`: the product at double and float. At Fixed32 the 64-bit product
// of x's raw value and g's mantissa, shifted once to g's power of two and rounded half up,
// saturated at the format's edges: a gain of 10^5 on a subband of 10^-5 keeps x's bits.
[[nodiscard]] constexpr Fixed32 apply_gain_fixed(const MantExp& g, Fixed32 x) noexcept {
    if (g.m == 0 || x.raw == 0) {
        return Fixed32{};
    }
    const std::int64_t product = static_cast<std::int64_t>(x.raw) * g.m;
    const std::int64_t shift = MantExp::kMantissaBits - static_cast<std::int64_t>(g.e);
    std::int64_t raw = 0;
    if (shift <= 0) {
        if (shift <= -32) {
            raw = product < 0 ? std::numeric_limits<std::int64_t>::min() : std::numeric_limits<std::int64_t>::max();
        } else {
            const std::int64_t limit = std::numeric_limits<std::int64_t>::max() >> static_cast<unsigned>(-shift);
            raw = product > limit ? std::numeric_limits<std::int64_t>::max()
                  : product < -limit ? std::numeric_limits<std::int64_t>::min()
                                     : product * (std::int64_t{1} << static_cast<unsigned>(-shift));
        }
    } else if (shift < 63) {
        raw = (product + (std::int64_t{1} << static_cast<unsigned>(shift - 1))) >> static_cast<unsigned>(shift);
    }
    if (raw > std::numeric_limits<std::int32_t>::max()) {
        return Fixed32::from_raw(std::numeric_limits<std::int32_t>::max());
    }
    if (raw < std::numeric_limits<std::int32_t>::min()) {
        return Fixed32::from_raw(std::numeric_limits<std::int32_t>::min());
    }
    return Fixed32::from_raw(static_cast<std::int32_t>(raw));
}

template <typename Real>
[[nodiscard]] constexpr Real apply_gain(const Energy<Real>& g, Real x) noexcept {
    if constexpr (kFixed<Real>) {
        return apply_gain_fixed(g, x);
    } else {
        return g * x;
    }
}
template <typename Real>
[[nodiscard]] constexpr Complex<Real> apply_gain(const Energy<Real>& g, Complex<Real> z) noexcept {
    if constexpr (kFixed<Real>) {
        return {apply_gain_fixed(g, z.re), apply_gain_fixed(g, z.im)};
    } else {
        return g * z;
    }
}

}  // namespace iclforge::ac4::detail::dsp
