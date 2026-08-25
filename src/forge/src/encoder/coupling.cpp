#include "ac3/encoder/coupling.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ac3::coupling {

namespace {

// §7.4.3: exponent 15 switches the mantissa from the implicit-leading-one
// form to a plain fraction, which is what lets very small coordinates be
// represented at all.
constexpr int kMaxExp = 15;
constexpr int kMaxMaster = 3;

// How many sub-bands a band starting at this coefficient should span. The
// steps are where a critical band passes one and two sub-band widths: bin
// 117 is 11.0 kHz and bin 181 is 17.0 kHz at 48 kHz, against a sub-band's
// 1125 Hz. Below the first step a sub-band is already coarser than the ear,
// so nothing is gained by joining any.
constexpr int band_width(int start_bin) {
    if (start_bin < 117) {
        return 1;
    }
    return start_bin < 181 ? 2 : 3;
}

}  // namespace

double decode_coordinate(Coordinate coordinate, int master, int mantissa_bits) {
    const double one = static_cast<double>(1 << mantissa_bits);
    const double mantissa = coordinate.exp == kMaxExp
                                ? coordinate.mant / one
                                : (coordinate.mant + one) / (2.0 * one);
    return std::ldexp(mantissa, -(coordinate.exp + 3 * master));
}

Coordinate quantize_coordinate(double value, int master, int mantissa_bits) {
    const int max_mant = (1 << mantissa_bits) - 1;
    const double one = static_cast<double>(1 << mantissa_bits);
    if (!(value > 0.0)) {
        return {.exp = kMaxExp, .mant = 0};
    }

    // Find the shift that lands the value in [0.5, 1), which is the range the
    // implicit-leading-one mantissa encodes. The master already contributes
    // 3 * master of that shift.
    //
    // std::ilogb extracts the unbiased binary exponent directly from the
    // IEEE-754 representation - exact, no rounding, and identical on every
    // conformant platform by construction. floor(-std::log2(value)) computes
    // the same integer through a transcendental libm call, whose last-bit
    // behaviour is NOT required to be identical across implementations
    // (roadmap VX12): for a value within a few ULPs of a power of two, one
    // platform's log2 can round the wrong way across the boundary and shift
    // lands one off from another platform's, silently taking a different
    // exponent field for the same input. std::ilogb(value) == n for value in
    // [2^n, 2^(n+1)), so the target shift is -(ilogb(value) + 1) - matching
    // floor(-log2(value)) everywhere except exactly AT a power of two, where
    // the mant > max_mant renormalisation below already corrects the
    // one-off difference (this used to rely on that path to self-correct;
    // now it simply never needs to).
    int shift = -std::ilogb(value) - 1;
    shift = std::max(shift, 0);
    int exp = shift - 3 * master;

    if (exp < 0) {
        // Louder than this master allows: clamp to the largest coordinate.
        return {.exp = 0, .mant = static_cast<std::uint8_t>(max_mant)};
    }
    if (exp >= kMaxExp) {
        // Quieter than the implicit-one form reaches: use the exp==15 escape,
        // whose mantissa is a plain fraction and can go all the way to zero.
        const double scaled = std::ldexp(value, kMaxExp + 3 * master);
        const auto mant = static_cast<int>(std::lround(scaled * one));
        return {.exp = kMaxExp,
                .mant = static_cast<std::uint8_t>(std::clamp(mant, 0, max_mant))};
    }

    const double scaled = std::ldexp(value, exp + 3 * master);  // now in [0.5, 1)
    auto mant = static_cast<int>(std::lround(scaled * 2.0 * one)) - (1 << mantissa_bits);
    if (mant > max_mant) {
        // Rounding pushed it to the next binade; renormalise one step up.
        if (exp == 0) {
            return {.exp = 0, .mant = static_cast<std::uint8_t>(max_mant)};
        }
        --exp;
        mant = static_cast<int>(
                   std::lround(std::ldexp(value, exp + 3 * master) * 2.0 * one)) -
               (1 << mantissa_bits);
    }
    return {.exp = static_cast<std::uint8_t>(exp),
            .mant = static_cast<std::uint8_t>(std::clamp(mant, 0, max_mant))};
}

BandLayout group_bands(int cplbegf, int subbands, std::span<const bool> structure) {
    assert(subbands >= 1 && subbands <= kSubBands);
    assert(structure.size() >= static_cast<std::size_t>(subbands));

    const int first_bin = start_mant(cplbegf);
    BandLayout out;
    out.count = 1;
    out.start[0] = first_bin;
    out.size[0] = kBinsPerSubBand;
    for (int sbnd = 1; sbnd < subbands; ++sbnd) {
        const auto band = static_cast<std::size_t>(out.count);
        if (structure[static_cast<std::size_t>(sbnd)]) {
            out.size[band - 1] += kBinsPerSubBand;
        } else {
            out.start[band] = first_bin + sbnd * kBinsPerSubBand;
            out.size[band] = kBinsPerSubBand;
            ++out.count;
        }
    }
    return out;
}

std::array<bool, kSubBands> band_structure(int cplbegf, int subbands) {
    std::array<bool, kSubBands> out{};
    const int first_bin = start_mant(cplbegf);
    int band_start = first_bin;
    int width = 1;
    for (int sbnd = 1; sbnd < subbands; ++sbnd) {
        if (width < band_width(band_start)) {
            out[static_cast<std::size_t>(sbnd)] = true;
            ++width;
        } else {
            band_start = first_bin + sbnd * kBinsPerSubBand;
            width = 1;
        }
    }
    return out;
}

int choose_master(std::span<const double> values) {
    // The master must cover the LOUDEST coordinate (a too-large master would
    // push it below exponent 0 and clip it); quiet bands are handled by the
    // exp==15 escape, so they never force the master up.
    double loudest = 0.0;
    for (const double value : values) {
        loudest = std::max(loudest, value);
    }
    if (!(loudest > 0.0)) {
        return kMaxMaster;
    }
    // Same shift as quantize_coordinate above, and for the same reason
    // (roadmap VX12): std::ilogb rather than floor(-log2(...)), so the two
    // functions agree exactly on what "the loudest value's own shift" means
    // instead of merely agreeing up to a libm rounding difference at a
    // power-of-two boundary.
    const int shift = std::max(0, -std::ilogb(loudest) - 1);
    // master * 3 must not exceed the loudest value's own shift.
    return std::clamp(shift / 3, 0, kMaxMaster);
}

}  // namespace ac3::coupling
