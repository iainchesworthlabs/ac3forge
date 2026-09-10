#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "ac3/core/eac3_tools.hpp"
#include "fixed32.hpp"

// The Annex E tools in the fixed-point tier's scalar
// (planning/arithmetic-tiers.md, Phase C), beside `eac3_tools.cpp`'s double
// and float forms.
//
// Why here rather than there. `Fixed32` is internal to `src/forge/src/core/`
// and `ac3/core/eac3_tools.hpp` is library surface, so the tier's forms cannot
// be declared alongside the other two without making the type public. They are
// header-only instead, exactly as `mdct_fixed.hpp` is, and the decoder picks
// them up by overload resolution at the same call sites: a name looked up on a
// `Fixed32` argument finds this file's overload, and one looked up on a
// `double` or a `float` finds the exported one, unchanged.
//
// Tables. Every constant below is taken from the SAME source the double form
// reads - through that form's own exported entry point where there is one -
// and rounded to the format once, at start-up. Nothing here re-derives a
// value from its defining expression, so there is one statement of each table
// in the project and this is a narrowing of it.

namespace ac3::eac3 {

namespace fixed_detail {

// Table E3.14's attenuation, from `spx_attenuation` itself: 32 codes x 3
// stored taps, each below one, so the format holds them with room.
struct SpxAttenuation {
    std::array<std::array<internal::Fixed32, 3>, kSpxAttenCodes> cell{};

    SpxAttenuation() {
        for (int code = 0; code < kSpxAttenCodes; ++code) {
            for (int tap = 0; tap < 3; ++tap) {
                cell[static_cast<std::size_t>(code)][static_cast<std::size_t>(tap)] =
                    internal::Fixed32{spx_attenuation(code, tap)};
            }
        }
    }
};

inline const SpxAttenuation& spx_attenuation_fixed() {
    static const SpxAttenuation t;
    return t;
}

// §E3.4.5's synthesis basis, weights included: `aht_inverse` of the j-th unit
// vector is w_j times the kernel's j-th row, which is the whole of what the
// inverse multiplies a coefficient by. Taken that way rather than re-deriving
// cos(j(2m+1)pi/12) here, so the weights that section's radicals decide
// (eac3_tools.cpp records how they were established) are stated once.
// Every entry is at most sqrt(2).
struct AhtBasis {
    std::array<std::array<internal::Fixed32, kBlocksPerFrameSize>, kBlocksPerFrameSize> row{};

    AhtBasis() {
        for (std::size_t j = 0; j < kBlocksPerFrameSize; ++j) {
            std::array<double, kBlocksPerFrameSize> unit{};
            std::array<double, kBlocksPerFrameSize> out{};
            unit[j] = 1.0;
            aht_inverse(unit, out);
            for (std::size_t m = 0; m < kBlocksPerFrameSize; ++m) {
                row[j][m] = internal::Fixed32{out[m]};
            }
        }
    }
};

inline const AhtBasis& aht_basis() {
    static const AhtBasis t;
    return t;
}

}  // namespace fixed_detail

// §E3.6.4.2.3's band-border notch, the same taps at the same seams as the
// floating forms: one product per tap, one rounding.
inline void spx_apply_notch(std::span<internal::Fixed32> synth, int startmant,
                            const BandLayout& bands, std::span<const bool> wrapflag,
                            int spxattencod) {
    if (spxattencod < 0) {
        return;
    }
    const auto& table = fixed_detail::spx_attenuation_fixed();
    const auto notch = [&](int centre) {
        for (int tap = 0; tap < kSpxAttenTaps; ++tap) {
            const int at = centre - 2 + tap - startmant;
            if (at < 0 || at >= static_cast<int>(synth.size())) {
                continue;
            }
            // The five taps are a symmetric set stored as three; an index
            // past the middle mirrors back, as spx_attenuation does.
            const int stored = tap < 3 ? tap : kSpxAttenTaps - 1 - tap;
            synth[static_cast<std::size_t>(at)] =
                synth[static_cast<std::size_t>(at)] *
                table.cell[static_cast<std::size_t>(spxattencod)][static_cast<std::size_t>(stored)];
        }
    };
    notch(startmant);
    for (int bnd = 1; bnd < bands.count; ++bnd) {
        if (wrapflag[static_cast<std::size_t>(bnd)]) {
            notch(bands.start[static_cast<std::size_t>(bnd)]);
        }
    }
}

// §E3.4.5's six-point inverse. The same sum in the same order as the floating
// forms, over the same basis; six products of a mantissa by a value at most
// sqrt(2), so the result stays inside the format for any legal input and the
// decoder's own block exponent (block_norm.hpp) reads its peak afterwards.
inline void aht_inverse(std::span<const internal::Fixed32, kBlocksPerFrameSize> coefficients,
                        std::span<internal::Fixed32, kBlocksPerFrameSize> out) {
    const auto& basis = fixed_detail::aht_basis();
    for (std::size_t m = 0; m < kBlocksPerFrameSize; ++m) {
        internal::Fixed32 sum{};
        for (std::size_t j = 0; j < kBlocksPerFrameSize; ++j) {
            sum += coefficients[j] * basis.row[j][m];
        }
        out[m] = sum;
    }
}


// §3.5.5.4's sine and cosine of pi times an angle, in the tier. The angle is
// a fraction of pi on (-1, 1] as §3.5.5.3 transmits and wraps it, so this is
// the float `sincos_pi`'s method - reduce to the nearest quarter turn, then a
// Taylor series in the remainder - with the reduction exact (a quarter turn is
// a shift) and the series in Q7.24. |r| is at most pi/4, where the truncation
// errors of these degrees are 1.7e-9 and 2.4e-8, both below the format's own
// resolution; what the result carries is the five roundings of the series,
// some tens of raw units, against angles transmitted at pi/32.
inline void sincos_pi(internal::Fixed32 a, internal::Fixed32& sine,
                      internal::Fixed32& cosine) {
    using internal::Fixed32;
    // q = the nearest quarter turn, as a count of half-units of the angle.
    const Fixed32 twice = a.scaled_by_pow2(1);
    const Fixed32 half = Fixed32::from_raw(Fixed32::kOne / 2);
    const int q = static_cast<int>(twice.raw >= 0 ? twice + half : twice - half);
    // r = (a - q/2) * pi, at most pi/4 in magnitude. The subtraction is exact
    // and pi is inside the format with room.
    static constexpr Fixed32 kPi = Fixed32::from_raw(52707179);  // 3.14159265358979 * 2^24
    const Fixed32 r = (a - Fixed32::from_integer_scaled(q, -1)) * kPi;
    const Fixed32 r2 = r * r;
    const auto c = [](double value) { return Fixed32{value}; };
    const Fixed32 sr =
        r * (Fixed32{1} -
             r2 * (c(1.0 / 6.0) -
                   r2 * (c(1.0 / 120.0) - r2 * (c(1.0 / 5040.0) - r2 * c(1.0 / 362880.0)))));
    const Fixed32 cr =
        Fixed32{1} -
        r2 * (c(0.5) -
              r2 * (c(1.0 / 24.0) - r2 * (c(1.0 / 720.0) - r2 * c(1.0 / 40320.0))));
    switch (((q % 4) + 4) % 4) {
        case 0:
            sine = sr;
            cosine = cr;
            break;
        case 1:
            sine = cr;
            cosine = -sr;
            break;
        case 2:
            sine = -sr;
            cosine = -cr;
            break;
        default:
            sine = -cr;
            cosine = sr;
            break;
    }
}

// §E3.5's four stages in the tier, defined in eac3_tools.cpp beside the
// double and float forms because the tables they read are that file's. Each
// carries the exponents its values are stored under - see block_norm.hpp for
// the convention, and eac3_tools.cpp for what each stage does with them.
//
// The spectrum takes each neighbouring block's own exponent and reports the
// one its 256 output bins share; the reconstruction takes the difference
// between that and the receiving channel's, and applies it per bin.
void ecpl_channel_spectrum_fixed(std::span<const internal::Fixed32, 256> prev_mant, int prev_norm,
                                 std::span<const internal::Fixed32, 256> curr_mant, int curr_norm,
                                 std::span<const internal::Fixed32, 256> next_mant, int next_norm,
                                 std::span<internal::Fixed32, 256> real_out,
                                 std::span<internal::Fixed32, 256> imag_out, int& out_norm);

void ecpl_amplitudes_fixed(std::span<const int> ecplamp, std::span<const int> ecplchaos,
                           bool ecpltrans, bool is_first_channel, int begin_subbnd,
                           int end_subbnd, std::span<const bool> structure,
                           std::span<internal::Fixed32> amp_out);

void ecpl_angles_fixed(int channel, std::span<const int> ecplangle, std::span<const int> ecplchaos,
                       bool ecpltrans, bool is_first_channel, int begin_subbnd, int end_subbnd,
                       std::span<const bool> structure, EcplNoise& noise,
                       std::span<internal::Fixed32> angle_out, bool interpolate = false);

void ecpl_channel_coefficients_fixed(std::span<const internal::Fixed32, 256> real_in,
                                     std::span<const internal::Fixed32, 256> imag_in,
                                     std::span<const internal::Fixed32> amp_bin,
                                     std::span<const internal::Fixed32> angle_bin, int begin_mant,
                                     int end_mant, int out_shift,
                                     std::span<internal::Fixed32, 256> mant_out);

}  // namespace ac3::eac3
