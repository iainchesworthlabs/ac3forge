#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

#include "fixed32.hpp"

// The fixed-point tier's block exponent (planning/arithmetic-tiers.md,
// Phase B), shared by both decoders.
//
// Q7.24 is an absolute format: a raw unit is 2^-24 of full scale wherever the
// value sits. Phase A stored coefficients in it directly and measured what
// that costs - a coefficient's error is half a raw unit, the transform sums
// two hundred and fifty-six of them, and a dense quiet channel (the surrounds
// of the gold streams, at -33 dBFS with most of their bins coded) came out
// 99 dB from the double decode, with coupling's factor of eight taking the
// coupled channels to 88. The information was there on the wire and lost at
// dequantisation: a mantissa of sixteen bits under an exponent of twelve
// keeps twelve of them in an absolute store.
//
// So the store is normalised per stream per block. Each stream's coefficients
// are kept scaled up by 2^norm, where norm is the smallest exponent among the
// block's coded bins less kNormGuardBits, so that the largest coefficient sits
// just below one half - the fixed inverse transform's precondition
// (mdct_fixed.hpp) - and every mantissa keeps all of its bits. The exponent
// travels with the block: the coupling reconstruction scales the shared
// channel's coefficients into the receiving channel's exponent, a rematrixed
// pair is stored under one exponent, the §7.7 gain is split into a mantissa
// applied to the coefficients and a power of two added to the exponent, and
// the overlap-add aligns the two halves it sums before the float conversion
// applies the exponent, exactly, at the end. Nothing here applies to the
// floating tiers: kNormalisedStore is false for them, every norm is zero, and
// exponent_scale(exp - 0) is the call they always made.
//
// Where the exponent comes from. A stream's own coded bins set it before any
// mantissa is read: the smallest exponent among them bounds every coefficient
// (AC-3's coupling and rematrixing bounds are known then too, so decoder.cpp
// folds them in at the same point). The E-AC-3 tools that can raise a
// channel's level above its own coded bins run after that - standard
// coupling's decoupling, enhanced coupling's reconstruction, spectral
// extension's synthesis, rematrixing - and each lowers the channel's exponent
// where it runs, shifting what the channel already holds down to match
// (renormalise below). That costs a pass over the channel only when a tool
// needs room, and never reserves bits it might not. An AHT stream is the
// other way round: its six blocks are reconstructed at once, so its exponent
// is exact, from the reconstructed peaks (aht_effective_exponent), and no
// bound is needed at all. Every bound is on the side of never wrapping: an
// exponent one too small costs a bit of precision, one too large costs the
// block.

namespace ac3::internal {

template <typename Scalar>
inline constexpr bool kNormalisedStore = std::is_same_v<Scalar, Fixed32>;

// One bit below one half: the transform's precondition (mdct_fixed.hpp).
inline constexpr int kNormGuardBits = 1;
// A sum and a difference of two channels (§7.5.4) can each double.
inline constexpr int kRematrixGuardBits = 1;
// The spectral extension blend adds a noise term of at most the translated
// band's own level (§E3.6.4.2.4) before the coordinate scales the sum.
inline constexpr int kSpxBlendGuardBits = 1;
// Enhanced coupling's reconstruction runs through float in this phase
// (Phase C brings it into the tier); the DFT-domain amplitude it applies can
// exceed the MDCT-domain coefficient it stands in for, so two bits of room
// above the coupling channel's own level.
inline constexpr int kEcplGuardBits = 2;
// Larger than any exponent: what min_exponent returns for an empty range.
inline constexpr int kNoExponent = 64;
// The exponent's range. The floor keeps exp - norm within the 32-entry
// power-of-two table for every legal exponent (exponents run to 24); the
// ceiling is a stream whose loudest bin is at the format's floor.
inline constexpr int kNormFloor = -7;
inline constexpr int kNormCeiling = 23;

template <typename Range>
[[nodiscard]] inline int min_exponent(const Range& exps, int begin, int end) {
    int m = kNoExponent;
    for (int bin = begin; bin < end; ++bin) {
        m = std::min(m, static_cast<int>(exps[static_cast<std::size_t>(bin)]));
    }
    return m;
}

// The exponent a stream is stored under when its coded coefficients are all
// below 2^-min_exp in magnitude (kNoExponent when it has none).
[[nodiscard]] inline int store_norm(int min_exp) {
    if (min_exp >= kNoExponent) {
        return 0;
    }
    return std::clamp(min_exp - kNormGuardBits, kNormFloor, kNormCeiling);
}

// The effective exponent of a coupled channel's coefficients in one band:
// the coupling channel's smallest exponent there, the coordinate's exponent
// (cplcoexp + 3 mstrcplco, coupling.hpp's coordinate_exponent), and the
// factor of eight the decoder applies (§7.4.3).
[[nodiscard]] inline int coupled_exponent(int cpl_min_exp, int coordinate_exp) {
    return cpl_min_exp >= kNoExponent ? kNoExponent : cpl_min_exp + coordinate_exp - 3;
}

// Lowers a stream's exponent to `wanted` when a tool needs more room than the
// exponent it was stored under, shifting what it holds down to match; a no-op
// when the stored exponent is already low enough, and in the floating tiers.
template <typename Scalar>
inline void renormalise(std::array<Scalar, 256>& coeffs, int& norm, int wanted) {
    if constexpr (kNormalisedStore<Scalar>) {
        if (wanted < norm) {
            const int shift = norm - wanted;
            for (auto& value : coeffs) {
                value = value.scaled_by_pow2(-shift);
            }
            norm = wanted;
        }
    } else {
        (void)coeffs;
        (void)norm;
        (void)wanted;
    }
}

// The number of bits a raw magnitude occupies: 24 for a value in [0.5, 1).
[[nodiscard]] inline int raw_width(std::int64_t magnitude) {
    return magnitude <= 0 ? 0 : 64 - std::countl_zero(static_cast<std::uint64_t>(magnitude));
}

// The exponent a bin's six reconstructed AHT blocks (§E3.4.5) are actually
// below: the coded one, less the bits the largest of them has above one half
// - or plus the bits it has below. kNoExponent for a bin that is all zero.
template <typename Scalar>
[[nodiscard]] inline int aht_effective_exponent(const std::array<Scalar, 6>& blocks, int exp) {
    if constexpr (kNormalisedStore<Scalar>) {
        std::int64_t peak = 0;
        for (const auto& value : blocks) {
            peak = std::max(peak, std::abs(static_cast<std::int64_t>(value.raw)));
        }
        if (peak == 0) {
            return kNoExponent;
        }
        return exp - (raw_width(peak) - Fixed32::kFractionBits);
    } else {
        (void)blocks;
        return exp;
    }
}

// The bits a spectral extension channel's exponent has to come down by for
// its synthesis to fit (§E3.6.4): the translated bands copy from
// [copystart, startmant) of the channel as it stands, the blend adds at most
// the band's own level to that (kSpxBlendGuardBits), and the coordinate is a
// mantissa below one times thirty-two over its power of two - so the
// synthesis is below 2^(width + 1 + 5 - smallest coordinate exponent) raw
// units, and has to stay below one half. Zero when it already does.
template <typename Scalar>
[[nodiscard]] inline int spx_room(const std::array<Scalar, 256>& coeffs, int copystart,
                                  int startmant, int min_coordinate_exp) {
    if constexpr (kNormalisedStore<Scalar>) {
        std::int64_t peak = 0;
        for (int bin = copystart; bin < startmant; ++bin) {
            peak = std::max(peak, std::abs(static_cast<std::int64_t>(
                                      coeffs[static_cast<std::size_t>(bin)].raw)));
        }
        if (peak == 0 || min_coordinate_exp >= kNoExponent) {
            return 0;
        }
        const int synthesis_width = raw_width(peak) + kSpxBlendGuardBits + 5 - min_coordinate_exp;
        return std::max(0, synthesis_width - (Fixed32::kFractionBits - kNormGuardBits));
    } else {
        (void)coeffs;
        (void)copystart;
        (void)startmant;
        (void)min_coordinate_exp;
        return 0;
    }
}

// A delay half that holds nothing yet is stored under the ceiling, so the
// first block's overlap-add aligns to the block rather than to the empty
// history - kNormCeiling is what a mute writes too. The floating tiers never
// read these.
template <std::size_t N>
[[nodiscard]] constexpr std::array<int, N> fresh_delay_norms() {
    std::array<int, N> norms{};
    norms.fill(kNormCeiling);
    return norms;
}

template <std::size_t Slots, std::size_t N>
[[nodiscard]] constexpr std::array<std::array<int, N>, Slots> fresh_delay_norm_slots() {
    std::array<std::array<int, N>, Slots> slots{};
    slots.fill(fresh_delay_norms<N>());
    return slots;
}

// A sixteen-bit vector-quantiser entry over 2^15 (the adaptive hybrid
// transform's, per E3.4.4) in the caller's scalar: the division the floating
// tiers always did, and for the fixed one the scaled integer it is - the
// entry being larger than the format holds before the scale and exact after
// it. A template because a scalar's own member cannot be NAMED in a branch
// of a non-template, discarded or not (src/internal/cpu/minimal/
// cpu_features.cpp's header records the same trap).
template <typename Scalar>
[[nodiscard]] inline Scalar vq_entry(int entry) {
    if constexpr (kNormalisedStore<Scalar>) {
        return Scalar::from_integer_scaled(entry, -15);
    } else {
        return static_cast<Scalar>(entry) / Scalar{32768};
    }
}

// A stored value widened to double at its true scale: the retained block
// §7.10's concealment repeats is kept in double whatever the store is.
template <typename Scalar>
[[nodiscard]] inline double widen_stored(Scalar value, int norm) {
    if constexpr (kNormalisedStore<Scalar>) {
        return std::ldexp(static_cast<double>(value), -norm);
    } else {
        (void)norm;
        return static_cast<double>(value);
    }
}

// raw / 2^shift, rounded half up, in 64 bits.
[[nodiscard]] inline std::int64_t shift_right_rounded(std::int32_t raw, int shift) {
    if (shift <= 0) {
        return raw;
    }
    if (shift >= 63) {
        return 0;
    }
    return (static_cast<std::int64_t>(raw) + (std::int64_t{1} << (shift - 1))) >> shift;
}

// §7.9.5's overlap-add, pcm = 2 (x + delay), for a channel whose transform
// output `x` is stored under `x_norm` and whose delay half under
// `delay_norm`. The two are aligned to the smaller exponent (the louder
// block's scale, so the quieter one gives up bits it has to spare), summed
// in 64 bits, saturated to the format - a legal stream never reaches that,
// a hostile one clips rather than wraps - converted once, and scaled by the
// exact power of two the exponent and the factor of two make. Then the
// delay takes x's second half under x's exponent. The floating tiers'
// instantiation is the loop the decoders always ran.
template <typename Scalar>
inline void overlap_add_normalised(const std::array<Scalar, 512>& x,
                                   std::array<Scalar, 256>& delay, int& delay_norm, int x_norm,
                                   std::span<float> pcm) {
    if constexpr (kNormalisedStore<Scalar>) {
        const int aligned = std::min(x_norm, delay_norm);
        const int x_shift = x_norm - aligned;
        const int delay_shift = delay_norm - aligned;
        const float scale = std::ldexp(1.0F, -(Fixed32::kFractionBits - 1) - aligned);
        for (std::size_t n = 0; n < 256; ++n) {
            const std::int64_t sum = shift_right_rounded(x[n].raw, x_shift) +
                                     shift_right_rounded(delay[n].raw, delay_shift);
            const auto clipped = static_cast<std::int32_t>(
                std::clamp<std::int64_t>(sum, std::numeric_limits<std::int32_t>::min(),
                                         std::numeric_limits<std::int32_t>::max()));
            pcm[n] = static_cast<float>(clipped) * scale;
            delay[n] = x[n + 256];
        }
        delay_norm = x_norm;
    } else {
        (void)delay_norm;
        (void)x_norm;
        for (std::size_t n = 0; n < 256; ++n) {
            pcm[n] = static_cast<float>(Scalar{2} * (x[n] + delay[n]));
            delay[n] = x[n + 256];
        }
    }
}

}  // namespace ac3::internal
