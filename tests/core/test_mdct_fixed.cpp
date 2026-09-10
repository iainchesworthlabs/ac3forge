// The fixed-point tier's inverse transform pair (src/forge/src/core/
// mdct_fixed.hpp) against the double one, on the inputs the header's own
// precondition admits: random dense blocks, sparse tonal ones, and the
// coherent worst case that takes the FFT to the top of the format.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <random>
#include <span>

#include "ac3/core/mdct.hpp"
#include "fixed32.hpp"
#include "mdct_fixed.hpp"

using ac3::internal::Fixed32;

namespace {

constexpr double kUlp = 1.0 / 16777216.0;

struct Comparison {
    double snr_db = 0.0;
    double max_abs_ulp = 0.0;
    double peak = 0.0;
};

// The fixed inverse of a block against the double inverse of the SAME
// values (the block rounded to the format first, so the only difference is
// the arithmetic), as a signal-to-error ratio over the 512 outputs and the
// largest single-sample error in raw units.
Comparison compare(const std::array<double, 256>& wide_coeffs, bool short_block) {
    std::array<Fixed32, 256> fixed_coeffs{};
    std::array<double, 256> rounded{};
    for (std::size_t i = 0; i < 256; ++i) {
        fixed_coeffs[i] = Fixed32{wide_coeffs[i]};
        rounded[i] = static_cast<double>(fixed_coeffs[i]);
        REQUIRE(std::abs(fixed_coeffs[i].raw) < ac3::internal::kFixedImdctInputLimit);
    }
    std::array<double, 512> wide_out{};
    std::array<Fixed32, 512> fixed_out{};
    if (short_block) {
        ac3::imdct256_pair_windowed(rounded, wide_out, /*fast=*/true);
        ac3::internal::imdct256_pair_windowed_fixed(fixed_coeffs, fixed_out);
    } else {
        ac3::imdct512_windowed(rounded, wide_out, /*fast=*/true);
        ac3::internal::imdct512_windowed_fixed(fixed_coeffs, fixed_out);
    }
    Comparison c;
    double signal = 0.0;
    double error = 0.0;
    for (std::size_t i = 0; i < 512; ++i) {
        const double d = static_cast<double>(fixed_out[i]) - wide_out[i];
        signal += wide_out[i] * wide_out[i];
        error += d * d;
        c.max_abs_ulp = std::max(c.max_abs_ulp, std::abs(d) / kUlp);
        c.peak = std::max(c.peak, std::abs(wide_out[i]));
    }
    c.snr_db = 10.0 * std::log10(signal / std::max(error, 1e-300));
    return c;
}

}  // namespace

TEST_CASE("the fixed inverse pair tracks the double one on dense random blocks", "[fixed32]") {
    std::mt19937 rng(0x1d3c);
    std::uniform_real_distribution<double> dist(-0.499, 0.499);
    for (const bool short_block : {false, true}) {
        double worst_snr = 1e9;
        double worst_ulp = 0.0;
        for (int trial = 0; trial < 40; ++trial) {
            std::array<double, 256> coeffs{};
            for (auto& v : coeffs) {
                v = dist(rng);
            }
            const auto c = compare(coeffs, short_block);
            worst_snr = std::min(worst_snr, c.snr_db);
            worst_ulp = std::max(worst_ulp, c.max_abs_ulp);
        }
        INFO((short_block ? "short" : "long") << " blocks: worst SNR " << worst_snr
             << " dB, worst sample error " << worst_ulp << " raw units");
        // A dense block's output is a few units in magnitude and its error a
        // few raw units: the header's estimate is 130 dB and the test asks
        // for less so a compiler's reordering of a sum cannot fail it.
        CHECK(worst_snr > 120.0);
        CHECK(worst_ulp < 64.0);
    }
}

TEST_CASE("the fixed inverse pair tracks the double one on sparse tonal blocks", "[fixed32]") {
    std::mt19937 rng(0x70a1);
    std::uniform_int_distribution<int> bin(0, 255);
    std::uniform_real_distribution<double> amp(-0.499, 0.499);
    for (const bool short_block : {false, true}) {
        double worst_snr = 1e9;
        for (int trial = 0; trial < 40; ++trial) {
            std::array<double, 256> coeffs{};
            for (int k = 0; k < 3; ++k) {
                coeffs[static_cast<std::size_t>(bin(rng))] = amp(rng);
            }
            const auto c = compare(coeffs, short_block);
            worst_snr = std::min(worst_snr, c.snr_db);
        }
        INFO((short_block ? "short" : "long") << " blocks: worst SNR " << worst_snr << " dB");
        // A sparse block's output is smaller, so the same raw-unit floor is
        // a larger fraction of it - the tier's precision is relative to the
        // block's peak coefficient, and a three-tone block's output peaks
        // well below the dense one's.
        CHECK(worst_snr > 110.0);
    }
}

TEST_CASE("the fixed inverse pair does not wrap on the coherent worst case", "[fixed32]") {
    // Every coefficient at the precondition's edge with one sign: the input
    // the header's seven-bit growth bound is stated for. The double output
    // reaches tens, the format holds 128, and the fixed output must be the
    // same values to within the arithmetic's own rounding rather than a
    // wrapped or saturated one.
    for (const bool short_block : {false, true}) {
        std::array<double, 256> coeffs{};
        coeffs.fill(0.499);
        const auto c = compare(coeffs, short_block);
        INFO((short_block ? "short" : "long") << " blocks: peak " << c.peak << ", SNR "
             << c.snr_db << " dB, worst sample error " << c.max_abs_ulp << " raw units");
        CHECK(c.peak < 128.0);
        CHECK(c.snr_db > 120.0);
        CHECK(c.max_abs_ulp < 256.0);
        // And the alternating-sign form, which drives the other half of the
        // spectrum.
        for (std::size_t i = 1; i < 256; i += 2) {
            coeffs[i] = -coeffs[i];
        }
        const auto d = compare(coeffs, short_block);
        CHECK(d.peak < 128.0);
        CHECK(d.snr_db > 120.0);
    }
}

TEST_CASE("the fixed inverse is linear in the block exponent", "[fixed32]") {
    // The decoders store a quiet block scaled up by its exponent and scale
    // the transform's output back down; that is only sound if the transform
    // of the scaled block is the scaled transform. Check the fixed inverse
    // of a block against the fixed inverse of the same block one bit down,
    // doubled: equal to within the two roundings.
    std::mt19937 rng(0x5ca1);
    std::uniform_real_distribution<double> dist(-0.499, 0.499);
    std::array<Fixed32, 256> full{};
    std::array<Fixed32, 256> half{};
    for (std::size_t i = 0; i < 256; ++i) {
        full[i] = Fixed32{dist(rng)};
        half[i] = full[i].scaled_by_pow2(-1);
    }
    std::array<Fixed32, 512> out_full{};
    std::array<Fixed32, 512> out_half{};
    ac3::internal::imdct512_windowed_fixed(full, out_full);
    ac3::internal::imdct512_windowed_fixed(half, out_half);
    double worst = 0.0;
    double signal = 0.0;
    double error = 0.0;
    for (std::size_t i = 0; i < 512; ++i) {
        const double doubled = static_cast<double>(out_half[i].scaled_by_pow2(1));
        const double d = doubled - static_cast<double>(out_full[i]);
        worst = std::max(worst, std::abs(d) / kUlp);
        signal += static_cast<double>(out_full[i]) * static_cast<double>(out_full[i]);
        error += d * d;
    }
    const double snr_db = 10.0 * std::log10(signal / std::max(error, 1e-300));
    INFO("worst difference " << worst << " raw units, " << snr_db << " dB");
    // Both transforms round, and the half block's roundings come back
    // doubled: a few tens of raw units on outputs of a few units, and no
    // saturation anywhere in either.
    CHECK(worst < 128.0);
    CHECK(snr_db > 115.0);
}
