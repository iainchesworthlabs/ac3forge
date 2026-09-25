// The fixed-point tier's enhanced-coupling functions (src/forge/src/core/
// eac3_tools_fixed.hpp): ecpl_amplitudes_fixed, ecpl_angles_fixed,
// ecpl_channel_coefficients_fixed and ecpl_channel_spectrum_fixed.
//
// Split out of test_fixed32.cpp because they are the one part of it that calls
// into the library rather than into inline code. The functions are internal -
// declared in a header under src/, not in the installed include tree - so they
// are not exported from libac3forge.so, and tests/CMakeLists.txt builds
// this file only when ac3::forge is the static library. The rest of
// test_fixed32.cpp is inline arithmetic and runs in either build.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

#include "ac3/core/eac3_tools.hpp"
#include "eac3_tools_fixed.hpp"
#include "fixed32.hpp"

using ac3::internal::Fixed32;

namespace {

constexpr double kUlp = 1.0 / 16777216.0;  // one raw unit

}  // namespace

TEST_CASE("ecpl_channel_spectrum_fixed of all-zero neighbors is all zero", "[fixed32]") {
    // The tier's own block-floating-point form of ecpl_channel_spectrum
    // (eac3_tools_fixed.hpp/.cpp), exercised for the first time here: every
    // existing ecpl_channel_spectrum test (test_enhanced_coupling.cpp) calls
    // the double or float overload, never this one, and it is the only one
    // of the three with its own DFT (dft512_fixed) and its own
    // block-exponent bookkeeping (prev_norm/curr_norm/next_norm in,
    // out_norm out) rather than a plain uniform scale.
    std::array<Fixed32, 256> zero{};
    std::array<Fixed32, 256> real_out{};
    std::array<Fixed32, 256> imag_out{};
    real_out.fill(Fixed32{1});  // poison, so the function must actually write zero
    imag_out.fill(Fixed32{1});
    int out_norm = -1;
    ac3::eac3::ecpl_channel_spectrum_fixed(zero, 0, zero, 0, zero, 0, real_out, imag_out,
                                           out_norm);
    for (int k = 0; k < 256; ++k) {
        CAPTURE(k);
        CHECK(real_out[static_cast<std::size_t>(k)].raw == 0);
        CHECK(imag_out[static_cast<std::size_t>(k)].raw == 0);
    }
}

TEST_CASE("ecpl_channel_coefficients_fixed: zero amplitude silences a channel; "
          "unity amplitude and zero angle is a plain fold",
          "[fixed32]") {
    // The tier's form of ecpl_channel_coefficients (eac3_tools_fixed.hpp/
    // .cpp) - untested before this. Mirrors the two cases
    // test_enhanced_coupling.cpp pins for the double form of the same
    // function, at values Fixed32 holds exactly (5 and -3 are well inside
    // the format's +-127 range, and out_shift = 0 keeps the raw value as the
    // exact same fixed-point number the double form computes).
    using ac3::eac3::ecpl_channel_coefficients_fixed;
    std::array<Fixed32, 256> real_in{};
    std::array<Fixed32, 256> imag_in{};
    real_in[20] = Fixed32{5.0};
    imag_in[20] = Fixed32{-3.0};

    {
        const std::array<Fixed32, 1> amp = {Fixed32{0.0}};
        const std::array<Fixed32, 1> angle = {Fixed32{0.0}};
        std::array<Fixed32, 256> mant_out{};
        mant_out.fill(Fixed32{1});  // poison
        ecpl_channel_coefficients_fixed(real_in, imag_in, amp, angle, 20, 21, /*out_shift=*/0,
                                        mant_out);
        CHECK(mant_out[20].raw == 0);
    }
    {
        // angle == 0 -> cos(0) == 1, sin(0) == 0, so Zr[ch] == Zr, Zi[ch] ==
        // Zi exactly - amp == 1 leaves the complex value untouched.
        const std::array<Fixed32, 1> amp = {Fixed32{1.0}};
        const std::array<Fixed32, 1> angle = {Fixed32{0.0}};
        std::array<Fixed32, 256> mant_out{};
        ecpl_channel_coefficients_fixed(real_in, imag_in, amp, angle, 20, 21, /*out_shift=*/0,
                                        mant_out);
        CHECK(mant_out[20].raw != 0);
        // Every other bin must stay untouched (the function only writes
        // [begin_mant, end_mant)).
        for (int bin = 0; bin < 256; ++bin) {
            if (bin == 20) {
                continue;
            }
            CAPTURE(bin);
            CHECK(mant_out[static_cast<std::size_t>(bin)].raw == 0);
        }
    }
}

TEST_CASE("the tier's ecpl amplitudes and angles are the double ones", "[fixed32]") {
    // ecpl_amplitudes_fixed/ecpl_angles_fixed are what a Fixed32 decode runs
    // for every enhanced-coupling channel, and a float or double build never
    // calls them - so this comparison is the only thing that exercises them
    // on such a build. Every band-to-bin path is covered: first channel and
    // later, transient and not, and both the direct and the interpolated
    // conversion.
    const int begin = 0;
    const int end = ac3::eac3::kEcplSubBands;
    const auto layout =
        ac3::eac3::ecpl_group_bands(begin, end, ac3::eac3::kDefaultEcplBandStructure);
    REQUIRE(layout.count > 1);
    const auto bands = static_cast<std::size_t>(layout.count);
    std::vector<int> amp_codes(bands);
    std::vector<int> angle_codes(bands);
    std::vector<int> chaos_codes(bands);
    std::mt19937 rng(0xec91);
    for (std::size_t b = 0; b < bands; ++b) {
        amp_codes[b] = static_cast<int>(rng() % 32);
        angle_codes[b] = static_cast<int>(rng() % 64);
        chaos_codes[b] = static_cast<int>(rng() % 8);
    }
    const auto bins = static_cast<std::size_t>(
        ac3::eac3::kEcplSubBandTab[static_cast<std::size_t>(end)] -
        ac3::eac3::kEcplSubBandTab[static_cast<std::size_t>(begin)]);

    for (const bool first : {true, false}) {
        for (const bool transient : {false, true}) {
            CAPTURE(first, transient);
            std::vector<double> amp(bins);
            std::vector<Fixed32> amp_fixed(bins);
            ac3::eac3::ecpl_amplitudes(amp_codes, chaos_codes, transient, first, begin, end,
                                       ac3::eac3::kDefaultEcplBandStructure, amp);
            ac3::eac3::ecpl_amplitudes_fixed(amp_codes, chaos_codes, transient, first, begin,
                                             end, ac3::eac3::kDefaultEcplBandStructure,
                                             amp_fixed);
            for (std::size_t i = 0; i < bins; ++i) {
                CAPTURE(i);
                // One table read and at most one product and sum.
                CHECK(std::abs(static_cast<double>(amp_fixed[i]) - amp[i]) <= 4.0 * kUlp);
            }

            for (const bool interpolate : {false, true}) {
                CAPTURE(interpolate);
                ac3::eac3::EcplNoise noise;
                ac3::eac3::EcplNoise noise_fixed;
                std::vector<double> angle(bins);
                std::vector<Fixed32> angle_fixed(bins);
                ac3::eac3::ecpl_angles(/*channel=*/1, angle_codes, chaos_codes, transient, first,
                                       begin, end, ac3::eac3::kDefaultEcplBandStructure, noise,
                                       angle, interpolate);
                ac3::eac3::ecpl_angles_fixed(/*channel=*/1, angle_codes, chaos_codes, transient,
                                             first, begin, end,
                                             ac3::eac3::kDefaultEcplBandStructure, noise_fixed,
                                             angle_fixed, interpolate);
                // Through the rotation each names, not the number: a value at
                // the wrap may land a whole turn apart between the two.
                double worst = 0.0;
                for (std::size_t i = 0; i < bins; ++i) {
                    const double a = std::numbers::pi * angle[i];
                    const double b = std::numbers::pi * static_cast<double>(angle_fixed[i]);
                    worst = std::max({worst, std::abs(std::cos(a) - std::cos(b)),
                                      std::abs(std::sin(a) - std::sin(b))});
                }
                INFO("worst angle error " << worst);
                CHECK(worst <= 1e-5);
                if (first) {
                    // The first coupled channel's angles are zero by
                    // definition in both tiers, whatever was sent.
                    CHECK(std::ranges::all_of(angle_fixed, [](Fixed32 v) { return v.raw == 0; }));
                }
            }
        }
    }
}

TEST_CASE("ecpl_channel_spectrum_fixed matches the double spectrum on real content",
          "[fixed32]") {
    // The all-zero case above only proves the tier writes zeros. Here three
    // blocks of full-scale content go through its own DFT, whose per-stage
    // shedding is what keeps a 512-point transform inside 32 bits; widened by
    // the exponent it reports, the result has to be the double spectrum.
    std::mt19937 rng(0x5bec);
    std::uniform_real_distribution<double> dist(-0.99, 0.99);
    std::array<double, 256> prev{};
    std::array<double, 256> curr{};
    std::array<double, 256> next{};
    std::array<Fixed32, 256> prev_fixed{};
    std::array<Fixed32, 256> curr_fixed{};
    std::array<Fixed32, 256> next_fixed{};
    for (std::size_t k = 0; k < 256; ++k) {
        prev_fixed[k] = Fixed32{dist(rng)};
        curr_fixed[k] = Fixed32{dist(rng)};
        next_fixed[k] = Fixed32{dist(rng)};
        prev[k] = static_cast<double>(prev_fixed[k]);
        curr[k] = static_cast<double>(curr_fixed[k]);
        next[k] = static_cast<double>(next_fixed[k]);
    }
    std::array<double, 256> real_out{};
    std::array<double, 256> imag_out{};
    ac3::eac3::ecpl_channel_spectrum(prev, curr, next, real_out, imag_out, /*fast=*/false);
    std::array<Fixed32, 256> real_fixed{};
    std::array<Fixed32, 256> imag_fixed{};
    int out_norm = 0;
    ac3::eac3::ecpl_channel_spectrum_fixed(prev_fixed, 0, curr_fixed, 0, next_fixed, 0,
                                           real_fixed, imag_fixed, out_norm);

    double peak = 0.0;
    double worst = 0.0;
    for (std::size_t k = 0; k < 256; ++k) {
        peak = std::max({peak, std::abs(real_out[k]), std::abs(imag_out[k])});
        worst = std::max(
            {worst, std::abs(std::ldexp(static_cast<double>(real_fixed[k]), -out_norm) - real_out[k]),
             std::abs(std::ldexp(static_cast<double>(imag_fixed[k]), -out_norm) - imag_out[k])});
    }
    INFO("peak " << peak << ", worst error " << worst << ", out_norm " << out_norm);
    REQUIRE(peak > 0.1);  // real content, not a near-silent spectrum
    CHECK(worst <= 1e-4 * peak);
}
