#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/export.hpp"

// AC-3 exponent pipeline (A/52 §7.1, §8.2.7-8.2.11).
//
// Coefficients are represented as mantissa * 2^-exponent with exponents in
// [0, 24]. The encoder extracts raw exponents from 25-bit fixed-point
// coefficients (§8.2.7: leading zeros, max 24), preprocesses them for the
// chosen strategy (§8.2.10: pairs/quads share the minimum exponent so every
// member stays representable; the absolute field is capped at 15 per §7.1.2;
// slew is limited to +-2 by only ever DECREASING exponents, which merely
// gives mantissas leading zeros and is always safe), then differentially
// encodes them three-to-a-7-bit-group (§7.1.2: 25*M1 + 5*M2 + M3).
//
// THE DECODER-MIRROR RULE (§8.2.10-8.2.11): after encoding, the encoder must
// run the normative decode (§7.1.3) and use THOSE exponents — not its raw
// ones — for mantissa normalization and bit allocation, or the decoder's
// independently computed allocation silently diverges. decode_exponents here
// is that normative §7.1.3 algorithm, shared with the in-repo decoder.

namespace ac3 {

inline constexpr int kMaxExponent = 24;          // §8.2.7
inline constexpr int kMaxAbsoluteExponent = 15;  // 4-bit exps[ch][0] field, §7.1.2

// §7.1.3: mantissas covered by each differential exponent.
[[nodiscard]] constexpr int exponent_group_size(ExpStrategy strategy) {
    switch (strategy) {
        case ExpStrategy::kD15:
            return 1;
        case ExpStrategy::kD25:
            return 2;
        case ExpStrategy::kD45:
            return 4;
        case ExpStrategy::kReuse:
            return 0;
    }
    return 0;
}

// §7.1.3 group-count formulas (fbw channels, endmant mantissas).
[[nodiscard]] constexpr int exponent_group_count(ExpStrategy strategy, int endmant) {
    switch (strategy) {
        case ExpStrategy::kD15:
            return (endmant - 1) / 3;
        case ExpStrategy::kD25:
            return (endmant - 1 + 3) / 6;
        case ExpStrategy::kD45:
            return (endmant - 1 + 9) / 12;
        case ExpStrategy::kReuse:
            return 0;
    }
    return 0;
}

// Signed 25-bit fixed-point conversion (the float/integer seam of the
// pipeline): round(c * 2^24), clamped to the representable range.
//
// Header-inline rather than an exported out-of-line call because of where it
// is called from: the two encoders convert every coefficient of every block
// of every stream, about 9,100 times per frame (6 channels x 6 blocks x up
// to 253 bins). As an exported function wrapping a libm std::round it was
// ~33-38 us a frame, about 7% of a fast-path 5.1 encode, and it kept the
// per-bin loop from vectorising at all - neither the call nor the rounding
// could be hoisted or widened across bins. It is a public-header symbol the
// library no longer exports: source callers are unaffected, a binary that
// linked the old ac3::to_fixed25 out of the shared library must recompile.
//
// The rounding is std::round's, exactly: half away from zero, bit-identical
// on every input, which is what makes this substitution safe (the encoders'
// output must stay byte-identical). std::floor(c + 0.5) is NOT that rounding
// - 0.49999999999999994 + 0.5 rounds up to 1.0 in double and would give 1
// where std::round gives 0 - so this truncates toward zero and then inspects
// the fractional remainder, which is what half-away-from-zero actually says.
// scaled - trunc(scaled) is exact in binary floating point (the remainder is
// a multiple of scaled's own ulp with magnitude below 1), so the two
// comparisons against +-0.5 below decide the tie exactly.
[[nodiscard]] constexpr std::int32_t to_fixed25(double c) {
    constexpr double kScale = 16777216.0;      // 2^24
    constexpr std::int32_t kMax = 16777215;    // 2^24 - 1
    constexpr std::int32_t kMin = -16777216;   // -2^24
    const double scaled = c * kScale;
    // Tested on the UNROUNDED product, which decides the same cases the
    // rounded test did: rounding is monotonic and both bounds are integers,
    // so scaled >= kMax implies round(scaled) >= kMax. A value just under a
    // bound that rounding pushes onto it is handled by the +-1 branches
    // below, which by construction land exactly on the bound and never past
    // it. Doing it here also bounds |scaled| below 2^24, which is what keeps
    // the int32 truncation in range (and catches the infinities, which
    // reached the same clamps before).
    if (scaled >= static_cast<double>(kMax)) {
        return kMax;
    }
    if (scaled <= static_cast<double>(kMin)) {
        return kMin;
    }
    const auto truncated = static_cast<std::int32_t>(scaled);     // toward zero
    const double frac = scaled - static_cast<double>(truncated);  // exact
    if (frac >= 0.5) {
        return truncated + 1;
    }
    if (frac <= -0.5) {
        return truncated - 1;
    }
    return truncated;
}

// The same conversion over a contiguous run of coefficients, which is how
// every caller on the encode path actually uses it - about 9,100 bins a
// frame. Value-for-value identical to calling to_fixed25 on each element
// (it is the same rounding and the same clamp); the batch form exists
// because it can do the rounding two lanes at a time through the
// architecture seam, and because on x86-64 that replaces an out-of-line call
// to libm's round() per element with in-line SSE2 arithmetic - see
// src/forge/src/internal/arch/x86_64/ac3/internal/arch/simd.hpp. The spans
// must be the same length.
AC3FORGE_EXPORT void to_fixed25_block(std::span<const double> coefficients,
                                      std::span<std::int32_t> fixed);

// §8.2.7: leading zeros of the 24-bit magnitude, capped at 24 (zero input).
//
// Inline for the same reason to_fixed25 is: it is the other half of the
// per-bin loop, and only with both bodies visible at the call site can the
// compiler keep a bin's fixed-point value in a register between them.
[[nodiscard]] constexpr int exponent_from_fixed(std::int32_t fixed) {
    // Widened before negating so INT32_MIN has somewhere to go.
    const auto widened = static_cast<std::int64_t>(fixed);
    const auto magnitude = static_cast<std::uint32_t>(widened < 0 ? -widened : widened);
    if (magnitude == 0) {
        return kMaxExponent;
    }
    // Leading zeros of the 24-bit magnitude field: countl_zero on 32 bits
    // minus the 8 bits above it. |c| >= 0.5 (bit 23 set) gives exponent 0.
    const int exponent = std::countl_zero(magnitude) - 8;
    return std::clamp(exponent, 0, kMaxExponent);
}

// Raw exponent extraction for a whole coefficient block.
AC3FORGE_EXPORT void extract_exponents(std::span<const std::int32_t> fixed,
                                       std::span<std::uint8_t> exponents);

// The two above fused into a single pass over one block's coefficients - the
// form both encoders' hot loop actually wants. Each of them used to convert
// a block bin by bin and then walk the same block again to derive exponents
// from it (encoder.cpp's step 4, eac3_frame.cpp's step5_fixed_extract), which
// is two traversals of the same data and two chances to spill it. Written out
// here as one loop over inline bodies so the compiler sees the whole per-bin
// dependency chain and can widen it.
inline void to_fixed25_block(std::span<const double> coeffs, std::span<std::int32_t> fixed,
                             std::span<std::uint8_t> exponents) {
    assert(coeffs.size() == fixed.size() && coeffs.size() == exponents.size());
    for (std::size_t i = 0; i < coeffs.size(); ++i) {
        const std::int32_t value = to_fixed25(coeffs[i]);
        fixed[i] = value;
        exponents[i] = static_cast<std::uint8_t>(exponent_from_fixed(value));
    }
}

struct EncodedExponents {
    std::uint8_t absolute = 0;         // the 4-bit exps[ch][0] field
    std::vector<std::uint8_t> groups;  // 7-bit grouped mapped values
};

// §8.2.10 encoder-side preprocessing + differential encoding. raw.size() is
// endmant; every raw exponent must be in [0, 24].
[[nodiscard]] AC3FORGE_EXPORT EncodedExponents encode_exponents(std::span<const std::uint8_t> raw,
                                                                ExpStrategy strategy);

// §7.1.3 normative decode: absolute + grouped values -> per-bin exponents.
// out.size() is endmant (group padding beyond endmant is discarded).
AC3FORGE_EXPORT void decode_exponents(std::uint8_t absolute, std::span<const std::uint8_t> groups,
                                      ExpStrategy strategy, std::span<std::uint8_t> out);

// The coupling channel's exponent set has a different shape (§7.1.3,
// §5.4.3.25): its absolute exponent is a reference that does NOT correspond
// to a coefficient - the first coded exponent is the one after it - and it is
// restricted to even values, transmitted as cplabsexp = absexp / 2. `raw`
// holds one exponent per coupling bin, and its length must be a multiple of
// 3 * the strategy's group size.
struct EncodedCouplingExponents {
    std::uint8_t cplabsexp = 0;        // the 4-bit transmitted field (absexp >> 1)
    std::vector<std::uint8_t> groups;  // ncplgrps 7-bit grouped mapped values
};

[[nodiscard]] AC3FORGE_EXPORT EncodedCouplingExponents
encode_coupling_exponents(std::span<const std::uint8_t> raw, ExpStrategy strategy);

// The matching normative decode: fills one exponent per coupling bin.
AC3FORGE_EXPORT void decode_coupling_exponents(std::uint8_t cplabsexp,
                                               std::span<const std::uint8_t> groups,
                                               ExpStrategy strategy, std::span<std::uint8_t> out);

}  // namespace ac3
