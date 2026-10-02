#pragma once

#include <cmath>
#include <limits>

#include "iclforge/ac4core/detail/real.hpp"
#include "iclforge/ac4core/dsp/complex.hpp"
#include "iclforge/ac4core/dsp/scalar_traits.hpp"

// The decoder's QMF-domain and time-domain values in the units of the build's scalar, for
// tests that state theirs in the double decoder's: full scale 2^15 (src/ac4dec/src/pcm/
// substream_pcm.cpp). At double and float the two are the same and these are identities; at
// Fixed32 the QMF domain is 2^kQmfShift and the time domain 2^kTimeShift below them
// (iclforge/ac4core/dsp/scalar_traits.hpp, planning/ac4.md, D14d).

namespace ac4dec_units {

using Real = iclforge::ac4::detail::Real;
using QmfValue = iclforge::ac4::detail::dsp::Complex<Real>;

inline constexpr int kQmfShift = iclforge::ac4::detail::dsp::kQmfShift<Real>;
inline constexpr int kTimeShift = iclforge::ac4::detail::dsp::kTimeShift<Real>;

// A QMF-domain value given in the double decoder's units.
[[nodiscard]] inline Real qmf_real(double value) {
    return static_cast<Real>(std::ldexp(value, kQmfShift));
}
[[nodiscard]] inline QmfValue qmf(double re, double im = 0.0) {
    return QmfValue{qmf_real(re), qmf_real(im)};
}

// A QMF-domain value of the build, in the double decoder's units.
[[nodiscard]] inline double qmf_units(Real value) {
    return std::ldexp(static_cast<double>(value), -kQmfShift);
}

// The scalar's relative rounding: its epsilon at double and float, and at Fixed32 one raw unit
// against a value of 1/16, the size of the tests' QMF values in the fixed tier's units.
[[nodiscard]] inline double relative_epsilon() {
    if constexpr (iclforge::ac4::detail::dsp::kFixed<Real>) {
        return 0x1p-20;
    } else {
        return static_cast<double>(std::numeric_limits<Real>::epsilon());
    }
}

// A spectral line given in the double decoder's units, for a test that moves lines through the
// stereo and multichannel tools by themselves: at Fixed32 the decoder holds a track's lines at
// an exponent of the track's own (src/ac4dec/src/pcm/asf_reconstruct.hpp), and these hold a
// test's at 2^12, which keeps lines of a few thousand inside the format.
inline constexpr int kLineExponent = iclforge::ac4::detail::dsp::kFixed<Real> ? 12 : 0;
[[nodiscard]] inline Real line_real(double value) {
    return static_cast<Real>(std::ldexp(value, -kLineExponent));
}
[[nodiscard]] inline double line_units(Real value) {
    return std::ldexp(static_cast<double>(value), kLineExponent);
}

// The same for the time domain.
[[nodiscard]] inline Real time_real(double value) {
    return static_cast<Real>(std::ldexp(value, kTimeShift));
}
[[nodiscard]] inline double time_units(Real value) {
    return std::ldexp(static_cast<double>(value), -kTimeShift);
}

}  // namespace ac4dec_units
