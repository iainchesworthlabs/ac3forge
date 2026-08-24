#include "ac3/quality/distortion.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

#include "ac3/core/bitalloc_tables.hpp"
#include "ac3/core/exponents.hpp"
#include "ac3/core/mantissas.hpp"
#include "ac3/internal/profiling.hpp"

namespace ac3::quality {

namespace {

// 2^-24: the scaling between a 25-bit fixed-point mantissa and the [-1, 1)
// value it represents. Exact in binary floating point, which is what lets
// the residue below be computed at mantissa scale and rescaled afterwards
// without changing a single bit of the result.
constexpr double kInverseFixedScale = 1.0 / 16777216.0;

// 4^-exponent, for turning a squared residue at mantissa scale into squared
// coefficient power. Built once rather than per bin because std::ldexp in
// the inner loop is most of the cost of the loop. Every entry is a power of
// two, so multiplying by it is exact.
constexpr std::array<double, kMaxExponent + 1> make_inverse_scales() {
    std::array<double, kMaxExponent + 1> table{};
    for (std::size_t e = 0; e < table.size(); ++e) {
        double value = 1.0;
        for (std::size_t i = 0; i < 2 * e; ++i) {
            value *= 0.5;
        }
        table[e] = value;
    }
    return table;
}

constexpr auto kInverseScale = make_inverse_scales();

// The symmetric quantizers' reconstruction points, (2k - (L-1))/L, indexed
// [bap][code]. Table-driven rather than divided per bin, and written with
// the same expression dequantize_mantissa() uses so the two produce the
// identical double rather than merely the same real number - which is what
// keeps the equivalence test below able to demand exact equality instead of
// a tolerance nobody could justify a size for.
constexpr std::array<std::array<double, 15>, 6> make_symmetric_reconstructions() {
    std::array<std::array<double, 15>, 6> table{};
    for (std::size_t bap = 1; bap <= 5; ++bap) {
        const int levels = kSymmetricLevels[bap];
        for (int code = 0; code < levels; ++code) {
            table[bap][static_cast<std::size_t>(code)] =
                (2.0 * code - (levels - 1)) / levels;
        }
    }
    return table;
}

constexpr auto kSymmetricReconstruction = make_symmetric_reconstructions();

// `mantissa - reconstruction`, at mantissa scale: the value the decoder will
// fail to reproduce, before the 2^-exponent that puts it back in the
// coefficient domain.
//
// This is the closed form of dequantize_mantissa(quantize_mantissa(m, bap),
// bap) subtracted from m, and it is only allowed to exist because
// tests/quality/test_distortion.cpp proves the two agree bit-exactly over
// the whole input space at every bap. Each branch mirrors the corresponding
// branch of quantize_mantissa() in core/mantissas.cpp line for line; a
// change there without a change here is what that test is watching for.
double residue_of(std::int32_t mantissa, int bap) {
    assert(bap >= 0 && bap <= 15);
    const double value = static_cast<double>(mantissa) * kInverseFixedScale;
    if (bap == 0) {
        // A zero-bit mantissa reconstructs as zero (§7.3.1), so the whole
        // normalized value is error.
        return value;
    }
    if (bap <= 5) {
        const auto levels =
            static_cast<std::int64_t>(kSymmetricLevels[static_cast<std::size_t>(bap)]);
        const std::int64_t numerator = static_cast<std::int64_t>(mantissa) * levels +
                                       ((levels - 1) << 24) + (std::int64_t{1} << 24);
        const std::int64_t code = std::clamp<std::int64_t>(numerator >> 25, 0, levels - 1);
        return value - kSymmetricReconstruction[static_cast<std::size_t>(bap)]
                                               [static_cast<std::size_t>(code)];
    }
    const int bits = kBapBits[static_cast<std::size_t>(bap)];
    const int shift = 25 - bits;
    std::int64_t code =
        (static_cast<std::int64_t>(mantissa) + (std::int64_t{1} << (shift - 1))) >> shift;
    code = std::clamp<std::int64_t>(code, -(std::int64_t{1} << (bits - 1)),
                                    (std::int64_t{1} << (bits - 1)) - 1);
    // The reconstruction is the clamped code scaled back up by the same
    // shift, so the residue is what the rounding (or the clamp) discarded.
    // Both operands are exact integers in a double and the scaling is a
    // power of two, so this rounds exactly once - in the same place the
    // dequantize-then-subtract form does.
    return static_cast<double>(static_cast<std::int64_t>(mantissa) - (code << shift)) *
           kInverseFixedScale;
}

}  // namespace

void BandNoise::reset() {
    signal.fill(0.0);
    noise.fill(0.0);
}

double BandNoise::total_signal() const {
    double sum = 0.0;
    for (const double value : signal) {
        sum += value;
    }
    return sum;
}

double BandNoise::total_noise() const {
    double sum = 0.0;
    for (const double value : noise) {
        sum += value;
    }
    return sum;
}

double reconstruction_error(std::int32_t fixed, int exponent, int bap) {
    assert(exponent >= 0 && exponent <= kMaxExponent);
    const auto mantissa =
        static_cast<std::int32_t>(static_cast<std::int64_t>(fixed) << exponent);
    // Dividing by an exact power of two, so the residue's own rounding is
    // the only one in the result.
    return residue_of(mantissa, bap) / static_cast<double>(1U << exponent);
}

void accumulate_block(std::span<const std::int32_t> fixed, std::span<const std::uint8_t> exps,
                      std::span<const std::uint8_t> bap, int start, int end, BandNoise& out) {
    AC3_ZONE_SCOPED_N("quality_accumulate_block");
    assert(start >= 0 && start <= end);
    assert(fixed.size() >= static_cast<std::size_t>(end - start));
    assert(exps.size() >= static_cast<std::size_t>(end));
    assert(bap.size() >= static_cast<std::size_t>(end));
    if (start >= end) {
        return;
    }

    // Walking bands outside the bin loop rather than calling bin_to_band()
    // per bin: the band a bin belongs to only ever moves forward, so this is
    // one comparison per bin instead of a table lookup, and it keeps the two
    // running accumulators in registers across a whole band.
    int band = tables::kMaskTab[static_cast<std::size_t>(start)];
    int band_end = std::min(tables::kBandStart[static_cast<std::size_t>(band)] +
                                tables::kBandSize[static_cast<std::size_t>(band)],
                            end);
    double signal = 0.0;
    double noise = 0.0;
    for (int bin = start; bin < end; ++bin) {
        if (bin >= band_end) {
            out.signal[static_cast<std::size_t>(band)] += signal;
            out.noise[static_cast<std::size_t>(band)] += noise;
            signal = 0.0;
            noise = 0.0;
            band = tables::kMaskTab[static_cast<std::size_t>(bin)];
            band_end = std::min(tables::kBandStart[static_cast<std::size_t>(band)] +
                                    tables::kBandSize[static_cast<std::size_t>(band)],
                                end);
        }
        const std::int32_t value = fixed[static_cast<std::size_t>(bin - start)];
        const int exponent = exps[static_cast<std::size_t>(bin)];
        const auto mantissa =
            static_cast<std::int32_t>(static_cast<std::int64_t>(value) << exponent);
        const double residue = residue_of(mantissa, bap[static_cast<std::size_t>(bin)]);
        // (residue / 2^exponent)^2, with the scaling folded into one exact
        // power-of-two multiply after the square rather than a division
        // before it - identical bit pattern, one operation fewer.
        noise += residue * residue * kInverseScale[static_cast<std::size_t>(exponent)];
        const double coefficient = static_cast<double>(value) * kInverseFixedScale;
        signal += coefficient * coefficient;
    }
    out.signal[static_cast<std::size_t>(band)] += signal;
    out.noise[static_cast<std::size_t>(band)] += noise;
}

double snr_db(const BandNoise& measured) {
    const double signal = measured.total_signal();
    const double noise = measured.total_noise();
    if (signal <= 0.0 || noise <= 0.0) {
        return kMaxSnrDb;
    }
    return std::min(kMaxSnrDb, 10.0 * std::log10(signal / noise));
}

void band_snr_db(const BandNoise& measured, std::span<double> out) {
    assert(out.size() >= static_cast<std::size_t>(kBands));
    for (std::size_t b = 0; b < static_cast<std::size_t>(kBands); ++b) {
        const double signal = measured.signal[b];
        const double noise = measured.noise[b];
        out[b] = (signal <= 0.0 || noise <= 0.0)
                     ? kMaxSnrDb
                     : std::min(kMaxSnrDb, 10.0 * std::log10(signal / noise));
    }
}

}  // namespace ac3::quality
