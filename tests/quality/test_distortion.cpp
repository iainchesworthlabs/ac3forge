#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <utility>
#include <vector>

#include "ac3/core/bitalloc.hpp"
#include "ac3/core/bitalloc_tables.hpp"
#include "ac3/core/exponents.hpp"
#include "ac3/core/mantissas.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/quality/distortion.hpp"

namespace {

// What the DECODER does, spelled out through the two public functions
// decoder.cpp itself calls (core/mantissas.cpp), with no shortcut: the
// coefficient it will reconstruct for one bin.
//
// This is the reference the closed form in quality/distortion.cpp is
// measured against. It is deliberately the slow, obvious composition -
// if the two ever disagree, this one is right.
double decoder_reconstruction(std::int32_t fixed, int exponent, int bap) {
    if (bap == 0) {
        return 0.0;  // §7.3.1, dithflag clear
    }
    const auto mantissa =
        static_cast<std::int32_t>(static_cast<std::int64_t>(fixed) << exponent);
    const std::uint32_t code = ac3::quantize_mantissa(mantissa, bap);
    return ac3::dequantize_mantissa(code, bap) / static_cast<double>(1U << exponent);
}

double coefficient_of(std::int32_t fixed) { return static_cast<double>(fixed) / 16777216.0; }

// Equal up to summation order. Both sides add the same terms, so the only
// admissible difference is the last few bits of a double; an absolute floor
// covers the bands where both are exactly zero.
bool close_enough(double actual, double expected) {
    const double tolerance = std::max(std::abs(expected) * 1e-12, 1e-300);
    return std::abs(actual - expected) <= tolerance;
}

// Every mantissa worth trying for one bap: the quantizer decision
// boundaries and their immediate neighbours (where an off-by-one in the
// rounding would show), the clamp edges, and a uniform sweep over the rest.
std::vector<std::int32_t> probe_mantissas(int bap) {
    std::vector<std::int32_t> probes;
    constexpr std::int32_t kMax = 16777215;   // 2^24 - 1
    constexpr std::int32_t kMin = -16777216;  // -2^24

    const auto push = [&](std::int64_t value) {
        probes.push_back(static_cast<std::int32_t>(std::clamp<std::int64_t>(value, kMin, kMax)));
    };

    // Uniform sweep across the whole representable range.
    for (std::int64_t value = kMin; value <= kMax; value += 1021) {
        push(value);
    }
    // Decision boundaries. For the symmetric quantizers the reconstruction
    // points sit at (2k - (L-1))/L, so the boundaries are halfway between;
    // for the asymmetric ones they are odd multiples of half a step.
    if (bap >= 1 && bap <= 5) {
        const int levels = ac3::kSymmetricLevels[static_cast<std::size_t>(bap)];
        for (int k = 0; k <= levels; ++k) {
            const double boundary = (2.0 * k - levels) / levels * 16777216.0;
            for (int nudge = -2; nudge <= 2; ++nudge) {
                push(static_cast<std::int64_t>(std::llround(boundary)) + nudge);
            }
        }
    } else if (bap >= 6) {
        const int shift = 25 - ac3::kBapBits[static_cast<std::size_t>(bap)];
        const std::int64_t step = std::int64_t{1} << shift;
        for (std::int64_t value = kMin; value <= kMax; value += step) {
            for (int nudge = -2; nudge <= 2; ++nudge) {
                push(value + step / 2 + nudge);
            }
        }
    }
    push(kMin);
    push(kMax);
    push(0);
    return probes;
}

}  // namespace

// The whole premise of quality/distortion.cpp is that it may evaluate
// dequantize(quantize(m)) in closed form instead of calling the pair. That
// is a second copy of the §7.3 quantizer arithmetic, which is the exact
// shape of bug ac3/verify/mirror.hpp exists to catch elsewhere - so it is
// pinned here over the whole input space rather than trusted.
TEST_CASE("reconstruction_error matches the real quantizer at every bap", "[quality][distortion]") {
    for (int bap = 0; bap <= 15; ++bap) {
        const std::vector<std::int32_t> probes = probe_mantissas(bap);
        std::size_t checked = 0;
        for (const std::int32_t fixed : probes) {
            const double expected = coefficient_of(fixed) - decoder_reconstruction(fixed, 0, bap);
            const double actual = ac3::quality::reconstruction_error(fixed, 0, bap);
            if (actual != expected) {
                CAPTURE(bap, fixed, expected, actual);
                FAIL("closed-form reconstruction error diverged from quantize/dequantize");
            }
            ++checked;
        }
        CAPTURE(bap);
        CHECK(checked > 30000);
    }
}

// The exponent only scales the result, but it scales BOTH the mantissa
// going in and the reconstruction coming out, so getting it wrong is a
// silent factor-of-two per step rather than an obvious error.
TEST_CASE("reconstruction_error scales with the decoded exponent", "[quality][distortion]") {
    std::mt19937 rng(0x51C3);
    std::uniform_int_distribution<std::int32_t> dist(-16777216, 16777215);
    for (int trial = 0; trial < 4000; ++trial) {
        const std::int32_t raw = dist(rng);
        // Only exponents the encoder could actually pair with this
        // coefficient: §8.2.7 takes the leading-zero count, and §8.2.10
        // only ever lowers it, so the shift never overflows 25 bits.
        const int maximum = ac3::exponent_from_fixed(raw);
        for (int exponent = 0; exponent <= maximum; ++exponent) {
            const int bap = 1 + (trial % 15);
            const double expected =
                coefficient_of(raw) - decoder_reconstruction(raw, exponent, bap);
            const double actual = ac3::quality::reconstruction_error(raw, exponent, bap);
            CAPTURE(raw, exponent, bap, expected, actual);
            REQUIRE(actual == expected);
        }
    }
}

TEST_CASE("a zero-bit mantissa loses the whole coefficient", "[quality][distortion]") {
    for (const std::int32_t fixed : {0, 1, -1, 1234567, -16777216, 16777215}) {
        CHECK(ac3::quality::reconstruction_error(fixed, 0, 0) == coefficient_of(fixed));
    }
}

// accumulate_block() is the same arithmetic again, this time fused into a
// banding loop that walks Table 7.13 forward instead of looking each bin up.
// Both halves of that - the residue and the band it lands in - are checked
// against the obvious form.
TEST_CASE("accumulate_block bands the same error the decoder reconstructs",
          "[quality][distortion]") {
    std::mt19937 rng(0x0BA5);
    std::uniform_real_distribution<double> dist(-0.8, 0.8);

    constexpr int kEnd = 253;
    std::vector<std::int32_t> fixed(kEnd);
    std::vector<std::uint8_t> exps(kEnd);
    for (int bin = 0; bin < kEnd; ++bin) {
        // A spectral tilt, so exponents actually vary across the band edges
        // rather than every bin sharing one.
        const double tilt = std::pow(0.985, bin);
        fixed[static_cast<std::size_t>(bin)] = ac3::to_fixed25(dist(rng) * tilt);
        exps[static_cast<std::size_t>(bin)] = static_cast<std::uint8_t>(
            ac3::exponent_from_fixed(fixed[static_cast<std::size_t>(bin)]));
    }

    // A real allocation rather than an invented bap array: the mix of baps
    // this produces (including the zero-bit bins at the top) is the mix the
    // measurement will actually meet.
    std::vector<std::uint8_t> bap(kEnd);
    ac3::compute_bit_allocation(exps, ac3::SampleRate::k48000, ac3::BitAllocCodes{}, 15, 0, bap);

    for (const int start : {0, 37, 85, 121}) {
        ac3::quality::BandNoise measured;
        ac3::quality::accumulate_block(
            std::span<const std::int32_t>(fixed).subspan(static_cast<std::size_t>(start)), exps,
            bap, start, kEnd, measured);

        std::array<double, ac3::quality::kBands> reference_noise{};
        std::array<double, ac3::quality::kBands> reference_signal{};
        for (int bin = start; bin < kEnd; ++bin) {
            const auto band = static_cast<std::size_t>(ac3::bin_to_band(bin));
            const std::int32_t value = fixed[static_cast<std::size_t>(bin)];
            const double error =
                coefficient_of(value) -
                decoder_reconstruction(value, exps[static_cast<std::size_t>(bin)],
                                       bap[static_cast<std::size_t>(bin)]);
            reference_noise[band] += error * error;
            reference_signal[band] += coefficient_of(value) * coefficient_of(value);
        }

        CAPTURE(start);
        for (std::size_t band = 0; band < ac3::quality::kBands; ++band) {
            CAPTURE(band);
            // Summation order differs (the fast path accumulates a band in
            // one register, the reference adds into an array), so this is a
            // floating-point equality only up to that reordering.
            REQUIRE(close_enough(measured.noise[band], reference_noise[band]));
            REQUIRE(close_enough(measured.signal[band], reference_signal[band]));
        }
        // Nothing below the stream's own start bin may be counted.
        for (int bin = 0; bin < start; ++bin) {
            const auto band = static_cast<std::size_t>(ac3::bin_to_band(bin));
            if (band < static_cast<std::size_t>(ac3::bin_to_band(start))) {
                CHECK(measured.signal[band] == 0.0);
                CHECK(measured.noise[band] == 0.0);
            }
        }
    }
}

// The property a search minimising measured noise depends on - and it is
// weaker than "more bits is always better", which is simply false here.
//
// A/52's symmetric quantizers (bap 1-5, Tables 7.19-7.23) are not nested
// grids: 3 levels reconstruct at +-2/3, 5 levels at +-0.8 and +-0.4, and
// nothing in the second set lies near 2/3. So a mantissa at 0.62 is served
// BETTER by bap 1 than by bap 2, pointwise, and any search that assumed
// otherwise would be assuming its way past a real property of the format.
// What holds is the aggregate: expected squared error falls with every step
// of bap, across the symmetric family and across the seam into the
// asymmetric one. That is what makes the measurement useful, and it is what
// is checked here - over normalized mantissas, since §8.2.7's exponent puts
// every mantissa the encoder actually quantizes in [0.5, 1).
TEST_CASE("expected measured noise falls with every step of bap", "[quality][distortion]") {
    std::mt19937 rng(0x4B17);
    std::uniform_real_distribution<double> magnitude(0.5, 1.0);
    std::bernoulli_distribution negative(0.5);

    constexpr int kTrials = 20000;
    std::vector<std::int32_t> normalized(kTrials);
    for (int trial = 0; trial < kTrials; ++trial) {
        const double value = magnitude(rng) * (negative(rng) ? -1.0 : 1.0);
        normalized[static_cast<std::size_t>(trial)] = ac3::to_fixed25(value);
    }

    double previous = 1.0;
    for (int bap = 1; bap <= 15; ++bap) {
        double sum = 0.0;
        for (const std::int32_t fixed : normalized) {
            const double error = ac3::quality::reconstruction_error(fixed, 0, bap);
            sum += error * error;
        }
        const double mean_square = sum / kTrials;
        CAPTURE(bap, mean_square, previous);
        CHECK(mean_square < previous);
        previous = mean_square;
    }
}

// Pointwise, what does hold: the error never exceeds half a quantizer step
// unless the value is outside the quantizer's own span, and the span is a
// property of the table rather than of the input.
TEST_CASE("measured error stays within half a step inside the quantizer span",
          "[quality][distortion]") {
    std::mt19937 rng(0x2D9E);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);
    for (int bap = 1; bap <= 15; ++bap) {
        const bool symmetric = bap <= 5;
        const int levels = ac3::kSymmetricLevels[static_cast<std::size_t>(std::min(bap, 5))];
        const double step = symmetric
                                ? 2.0 / levels
                                : 1.0 / (1 << (ac3::kBapBits[static_cast<std::size_t>(bap)] - 1));
        const double span = symmetric ? static_cast<double>(levels - 1) / levels : 1.0 - step;
        for (int trial = 0; trial < 3000; ++trial) {
            const double value = dist(rng);
            const auto fixed = ac3::to_fixed25(value);
            const double error = std::abs(ac3::quality::reconstruction_error(fixed, 0, bap));
            CAPTURE(bap, value, step, span, error);
            if (std::abs(value) <= span) {
                CHECK(error <= step / 2 + 1e-9);
            } else {
                // Outside the span the error is exactly the distance to the
                // edge, which is the whole point of measuring rather than
                // assuming a uniform noise model.
                CHECK(error <= std::abs(value) - span + step / 2 + 1e-9);
            }
        }
    }
}

TEST_CASE("snr_db reports silence and saturation without infinities",
          "[quality][distortion]") {
    ac3::quality::BandNoise empty;
    CHECK(ac3::quality::snr_db(empty) == ac3::quality::kMaxSnrDb);

    ac3::quality::BandNoise perfect;
    perfect.signal[3] = 1.0;
    perfect.noise[3] = 0.0;
    CHECK(ac3::quality::snr_db(perfect) == ac3::quality::kMaxSnrDb);

    ac3::quality::BandNoise half;
    half.signal[7] = 100.0;
    half.noise[7] = 1.0;
    CHECK_THAT(ac3::quality::snr_db(half), Catch::Matchers::WithinAbs(20.0, 1e-9));

    std::array<double, ac3::quality::kBands> per_band{};
    ac3::quality::band_snr_db(half, per_band);
    CHECK_THAT(per_band[7], Catch::Matchers::WithinAbs(20.0, 1e-9));
    CHECK(per_band[0] == ac3::quality::kMaxSnrDb);
}

TEST_CASE("reset clears an accumulator for reuse", "[quality][distortion]") {
    ac3::quality::BandNoise measured;
    measured.signal[1] = 5.0;
    measured.noise[1] = 0.5;
    CHECK(measured.total_signal() == 5.0);
    CHECK(measured.total_noise() == 0.5);
    measured.reset();
    CHECK(measured.total_signal() == 0.0);
    CHECK(measured.total_noise() == 0.0);
}
