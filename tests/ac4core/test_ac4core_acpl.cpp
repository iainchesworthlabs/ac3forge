// A-CPL in the AC-4 shared core (src/ac4core/src/acpl): ETSI TS 103 190-1
// V1.4.1 clause 5.7.7's parameter bands (Table 197), dequantisation tables
// (Tables 203 to 208) and differential decoding (Pseudocode 121),
// interpolation (Pseudocodes 109 and 110), the decorrelators (Pseudocode 111,
// Tables 198 to 201) against their difference equations and as all-pass
// filters, and the transient ducker (Pseudocodes 112 to 114) on inputs whose
// gains can be worked by hand.

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "acpl/acpl.hpp"

namespace {

namespace acpl = ac4::detail::acpl;
using Complex = std::complex<double>;
using Catch::Approx;

constexpr int kSubbands = acpl::kSubbands;

[[nodiscard]] std::size_t at(int index) {
    return static_cast<std::size_t>(index);
}

// Pseudocode 111 written out as the difference equation of each subband,
// y[n] = sum_{i=0}^{L} a[L-i] x[n-d-i] - sum_{i=1}^{L} a[i] y[n-i], for one
// subband's samples from silence.
std::vector<Complex> difference_equation(int decorrelator, int subband, const std::vector<Complex>& x) {
    const int region = acpl::region_of(subband);
    const std::span<const double> a = acpl::coefficients(decorrelator, region);
    const int d = acpl::kRegions[at(region)].delay;
    const int length = acpl::kRegions[at(region)].length;
    std::vector<Complex> y(x.size());
    for (int n = 0; n < static_cast<int>(x.size()); ++n) {
        Complex acc{};
        for (int i = 0; i <= length; ++i) {
            if (n - d - i >= 0) {
                acc += a[at(length - i)] * x[at(n - d - i)];
            }
        }
        for (int i = 1; i <= length; ++i) {
            if (n - i >= 0) {
                acc -= a[at(i)] * y[at(n - i)];
            }
        }
        y[at(n)] = acc / a[0];
    }
    return y;
}

// One subband's response of `decorrelator` to `x`, run through the class
// `frame` slots at a time; the other subbands carry silence.
std::vector<Complex> run_decorrelator(int decorrelator, int subband, const std::vector<Complex>& x, int frame) {
    acpl::Decorrelator<double> d(decorrelator);
    std::vector<Complex> out(x.size());
    std::vector<Complex> in_matrix(at(frame) * kSubbands);
    std::vector<Complex> out_matrix(at(frame) * kSubbands);
    for (std::size_t start = 0; start < x.size(); start += at(frame)) {
        std::ranges::fill(in_matrix, Complex{});
        for (std::size_t ts = 0; ts < at(frame); ++ts) {
            in_matrix[ts * kSubbands + at(subband)] = x[start + ts];
        }
        d.process(in_matrix, out_matrix, frame);
        for (std::size_t ts = 0; ts < at(frame); ++ts) {
            out[start + ts] = out_matrix[ts * kSubbands + at(subband)];
        }
    }
    return out;
}

}  // namespace

TEST_CASE("sb_to_pb maps QMF subbands to parameter bands by Table 197", "[ac4core][acpl]") {
    CHECK(acpl::sb_to_pb(15, 0) == 0);
    CHECK(acpl::sb_to_pb(15, 10) == 9);
    CHECK(acpl::sb_to_pb(15, 63) == 14);
    CHECK(acpl::sb_to_pb(12, 20) == 9);
    CHECK(acpl::sb_to_pb(9, 30) == 8);
    CHECK(acpl::sb_to_pb(7, 3) == 2);
    CHECK(acpl::sb_to_pb(7, 40) == 6);
    CHECK(acpl::sb_to_pb(10, 5) == -1);
    CHECK(acpl::sb_to_pb(15, 64) == -1);
    CHECK(acpl::sb_to_pb(15, -1) == -1);
    // Each band count's last band ends at subband 63, and bands never fall.
    for (const int bands : {15, 12, 9, 7}) {
        CHECK(acpl::sb_to_pb(bands, 63) == bands - 1);
        for (int sb = 1; sb < kSubbands; ++sb) {
            const int step = acpl::sb_to_pb(bands, sb) - acpl::sb_to_pb(bands, sb - 1);
            CHECK((step == 0 || step == 1));
        }
    }
}

TEST_CASE("Tables 203 to 206 dequantise alpha and beta", "[ac4core][acpl]") {
    using acpl::Quant;
    // Table 203's ends, centre and a row in each half, with their ibeta.
    CHECK(acpl::dequantise_alpha(0, Quant::kFine).alpha == -2.0);
    CHECK(acpl::dequantise_alpha(16, Quant::kFine).alpha == 0.0);
    CHECK(acpl::dequantise_alpha(32, Quant::kFine).alpha == 2.0);
    CHECK(acpl::dequantise_alpha(5, Quant::kFine).alpha == -1.234375);
    CHECK(acpl::dequantise_alpha(5, Quant::kFine).ibeta == 5);
    CHECK(acpl::dequantise_alpha(23, Quant::kFine).alpha == 0.940625);
    CHECK(acpl::dequantise_alpha(23, Quant::kFine).ibeta == 7);
    CHECK(acpl::dequantise_alpha(13, Quant::kCoarse).alpha == 1.1375);
    CHECK(acpl::dequantise_alpha(13, Quant::kCoarse).ibeta == 3);
    // Table 204 and 206 entries.
    CHECK(acpl::dequantise_beta(5, 3, Quant::kFine) == 1.1882319);
    CHECK(acpl::dequantise_beta(8, 0, Quant::kFine) == 4.0);
    CHECK(acpl::dequantise_beta(0, 4, Quant::kFine) == 0.0);
    CHECK(acpl::dequantise_beta(3, 2, Quant::kCoarse) == 1.306875);
    CHECK(acpl::dequantise_beta(4, 4, Quant::kCoarse) == 1.0);

    // The fine tables share one sequence of steps r[k] = beta_dq[k][8]: alpha
    // runs -(1 + r[8 - q]) up to q = 8 and -(1 - r[q - 8]) up to q = 16, odd
    // about q = 16; ibeta rises and falls with it; and each beta row is r[q]
    // times the row for beta_q 8, to the table's seven decimals.
    std::array<double, 9> r{};
    for (int k = 0; k <= 8; ++k) {
        r[at(k)] = acpl::dequantise_beta(k, 8, Quant::kFine);
    }
    for (int q = 0; q <= 32; ++q) {
        const acpl::AlphaValue v = acpl::dequantise_alpha(q, Quant::kFine);
        const int from_centre = std::abs(q - 16);
        const double magnitude = from_centre >= 8 ? 1.0 + r[at(from_centre - 8)] : 1.0 - r[at(8 - from_centre)];
        CHECK(v.alpha == Approx(q < 16 ? -magnitude : magnitude).margin(1e-12));
        CHECK(v.ibeta == (from_centre <= 8 ? from_centre : 16 - from_centre));
    }
    for (int q = 0; q <= 8; ++q) {
        for (int ibeta = 0; ibeta <= 8; ++ibeta) {
            CHECK(acpl::dequantise_beta(q, ibeta, Quant::kFine) ==
                  Approx(r[at(q)] * acpl::dequantise_beta(8, ibeta, Quant::kFine)).margin(1e-6));
        }
    }
    // The coarse tables are the fine ones at even indices.
    for (int q = 0; q <= 16; ++q) {
        const acpl::AlphaValue coarse = acpl::dequantise_alpha(q, Quant::kCoarse);
        const acpl::AlphaValue fine = acpl::dequantise_alpha(2 * q, Quant::kFine);
        CHECK(coarse.alpha == fine.alpha);
        CHECK(2 * coarse.ibeta == fine.ibeta);
    }
    for (int q = 0; q <= 4; ++q) {
        for (int ibeta = 0; ibeta <= 4; ++ibeta) {
            CHECK(acpl::dequantise_beta(q, ibeta, Quant::kCoarse) ==
                  acpl::dequantise_beta(2 * q, 2 * ibeta, Quant::kFine));
        }
    }
}

TEST_CASE("Tables 207 and 208 give beta3's and gamma's steps", "[ac4core][acpl]") {
    CHECK(acpl::beta3_step(acpl::Quant::kFine) == 0.125);
    CHECK(acpl::beta3_step(acpl::Quant::kCoarse) == 0.25);
    CHECK(acpl::gamma_step(acpl::Quant::kFine) == 1638.0 / 16384.0);
    CHECK(acpl::gamma_step(acpl::Quant::kCoarse) == 3276.0 / 16384.0);
    // Each kind's range reaches the same largest value at both quantisations.
    for (const acpl::Kind kind : {acpl::Kind::kBeta3, acpl::Kind::kGamma}) {
        const auto fine = acpl::quantised_range(kind, acpl::Quant::kFine);
        const auto coarse = acpl::quantised_range(kind, acpl::Quant::kCoarse);
        CHECK(fine.max == 2 * coarse.max);
        CHECK(fine.min == 2 * coarse.min);
    }
}

TEST_CASE("Pseudocode 121 decodes along frequency and along time", "[ac4core][acpl]") {
    using acpl::Kind;
    using acpl::Quant;
    std::array<int, 15> coded{};
    std::array<int, 15> previous{};
    std::array<int, 15> out{};
    SECTION("DIFF_FREQ: the first band as sent, then running sums") {
        coded = {16, 1, -2, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        REQUIRE(acpl::differential_decode(Kind::kAlpha, Quant::kFine, false, 0, 15, coded, previous, out));
        CHECK(out[0] == 16);
        CHECK(out[1] == 17);
        CHECK(out[2] == 15);
        CHECK(out[4] == 18);
        CHECK(out[14] == 18);
    }
    SECTION("DIFF_TIME: each band against the same band before") {
        previous.fill(4);
        coded = {1, -1, 0, 2, -4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        REQUIRE(acpl::differential_decode(Kind::kBeta, Quant::kFine, true, 0, 7, coded, previous, out));
        CHECK(out[0] == 5);
        CHECK(out[1] == 3);
        CHECK(out[3] == 6);
        CHECK(out[4] == 0);
        CHECK(out[6] == 4);
        CHECK(out[7] == 0);  // past num_bands
    }
    SECTION("from acpl_param_band, with the bands below it 0") {
        coded = {99, 99, 99, 8, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        REQUIRE(acpl::differential_decode(Kind::kAlpha, Quant::kCoarse, false, 3, 9, coded, previous, out));
        CHECK(out[0] == 0);
        CHECK(out[2] == 0);
        CHECK(out[3] == 8);
        CHECK(out[4] == 9);
        CHECK(out[8] == 9);
    }
    SECTION("a value outside its table fails") {
        coded = {8, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        CHECK_FALSE(acpl::differential_decode(Kind::kBeta, Quant::kFine, false, 0, 15, coded, previous, out));
        coded = {-11, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        CHECK_FALSE(acpl::differential_decode(Kind::kGamma, Quant::kCoarse, false, 0, 7, coded, previous, out));
        coded = {-10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        CHECK(acpl::differential_decode(Kind::kGamma, Quant::kCoarse, false, 0, 7, coded, previous, out));
    }
}

TEST_CASE("Pseudocodes 109 and 110 interpolate a frame's parameters", "[ac4core][acpl]") {
    constexpr int kSlots = 32;
    acpl::ParamSets values{};
    acpl::ParamPrev prev{};
    for (int pb = 0; pb < acpl::kMaxParamBands; ++pb) {
        values[0][at(pb)] = 1.0 + pb;
        values[1][at(pb)] = -1.0 - pb;
    }
    prev.fill(0.5);
    std::vector<double> out(at(kSlots) * kSubbands);
    const auto value = [&](int ts, int sb) { return out[at(ts) * kSubbands + at(sb)]; };
    // Subband 40 is in band 14 of 15 and band 6 of 7.
    SECTION("smooth, one set: a line from acpl_param_prev to the set, reached at the last slot") {
        acpl::interpolate({.steep = false, .num_param_sets = 1, .param_timeslot = {}}, 15, values, prev, kSlots, out);
        CHECK(value(0, 40) == Approx(0.5 + (15.0 - 0.5) / 32.0));
        CHECK(value(15, 40) == Approx(0.5 + 16.0 * (15.0 - 0.5) / 32.0));
        CHECK(value(31, 40) == Approx(15.0));
        CHECK(value(31, 0) == Approx(1.0));
    }
    SECTION("smooth, two sets: to the first by mid-frame, then to the second") {
        acpl::interpolate({.steep = false, .num_param_sets = 2, .param_timeslot = {}}, 7, values, prev, kSlots, out);
        CHECK(value(15, 40) == Approx(7.0));
        CHECK(value(16, 40) == Approx(7.0 + (-7.0 - 7.0) / 16.0));
        CHECK(value(31, 40) == Approx(-7.0));
    }
    SECTION("steep: acpl_param_prev until each set's time slot") {
        acpl::interpolate({.steep = true, .num_param_sets = 2, .param_timeslot = {5, 20}}, 15, values, prev, kSlots,
                          out);
        CHECK(value(4, 40) == 0.5);
        CHECK(value(5, 40) == 15.0);
        CHECK(value(19, 40) == 15.0);
        CHECK(value(20, 40) == -15.0);
        CHECK(value(31, 40) == -15.0);
    }
    SECTION("acpl_param_prev becomes the last set, per subband") {
        acpl::end_frame({.steep = false, .num_param_sets = 2, .param_timeslot = {}}, 9, values, prev);
        CHECK(prev[0] == -1.0);
        CHECK(prev[63] == -9.0);
        acpl::end_frame({.steep = false, .num_param_sets = 1, .param_timeslot = {}}, 9, values, prev);
        CHECK(prev[63] == 9.0);
    }
}

TEST_CASE("Each decorrelator's impulse response is its difference equation", "[ac4core][acpl]") {
    // Two thousand slots take every filter's tail below 1e-30 (its largest
    // pole is 0.959), run through the class 32 and 6 slots at a time.
    constexpr std::size_t kLength = 2048;
    std::vector<Complex> impulse(kLength);
    impulse[0] = Complex{1.0, 0.0};
    for (int decorrelator = 0; decorrelator < acpl::kDecorrelators; ++decorrelator) {
        for (const int subband : {0, 6, 7, 22, 23, 63}) {
            const std::vector<Complex> expected = difference_equation(decorrelator, subband, impulse);
            for (const int frame : {32, 16}) {
                const std::vector<Complex> got = run_decorrelator(decorrelator, subband, impulse, frame);
                double error = 0.0;
                for (std::size_t n = 0; n < kLength; ++n) {
                    error = std::max(error, std::abs(got[n] - expected[n]));
                }
                INFO("D" << decorrelator << " subband " << subband << " frame " << frame);
                CHECK(error < 1e-12);
            }
            // Nothing before the region's delay.
            const int delay = acpl::kRegions[at(acpl::region_of(subband))].delay;
            for (int n = 0; n < delay; ++n) {
                CHECK(expected[at(n)] == Complex{});
            }
            CHECK(std::abs(expected[at(delay)]) > 0.0);
        }
    }
}

TEST_CASE("Each decorrelator is all-pass: its magnitude response is flat to 1e-9", "[ac4core][acpl]") {
    constexpr std::size_t kLength = 2048;
    std::vector<Complex> impulse(kLength);
    impulse[0] = Complex{1.0, 0.0};
    for (int decorrelator = 0; decorrelator < acpl::kDecorrelators; ++decorrelator) {
        for (int region = 0; region < 3; ++region) {
            const int subband = acpl::kRegions[at(region)].first_subband;
            const std::vector<Complex> h = run_decorrelator(decorrelator, subband, impulse, 32);
            double worst = 0.0;
            for (int k = 0; k < 97; ++k) {
                const double w = std::numbers::pi * k / 96.0;
                Complex response{};
                for (std::size_t n = 0; n < kLength; ++n) {
                    response += h[n] * std::polar(1.0, -w * static_cast<double>(n));
                }
                worst = std::max(worst, std::abs(std::abs(response) - 1.0));
            }
            INFO("D" << decorrelator << " region k" << region);
            CHECK(worst < 1e-9);
        }
    }
}

TEST_CASE("The transient ducker leaves steady signals and ducks a decaying one", "[ac4core][acpl]") {
    constexpr int kSlots = 32;
    std::vector<Complex> signal(at(kSlots) * kSubbands);
    SECTION("steady energy: every gain 1") {
        for (std::size_t i = 0; i < signal.size(); ++i) {
            signal[i] = std::polar(0.3, 0.1 * static_cast<double>(i % kSubbands));
        }
        const std::vector<Complex> input = signal;
        acpl::TransientDucker<double> ducker;
        ducker.process(signal, kSlots);
        for (std::size_t i = 0; i < signal.size(); ++i) {
            CHECK(std::abs(signal[i] - input[i]) < 1e-12);
        }
    }
    SECTION("energy E at slot 0 and e after: Pseudocode 112 worked by hand") {
        // Subband 40, alone in its band of the 15 (subbands 35 to 63); a
        // second subband, 3, carries e throughout.
        constexpr double kE = 4.0;
        constexpr double ke = 0.01;
        signal[40] = Complex{2.0, 0.0};
        for (std::size_t ts = 0; ts < at(kSlots); ++ts) {
            if (ts > 0) {
                signal[ts * kSubbands + 40] = Complex{0.0, 0.1};
            }
            signal[ts * kSubbands + 3] = Complex{0.1, 0.0};
        }
        acpl::TransientDucker<double> ducker;
        ducker.process(signal, kSlots);
        const auto gain = [&](std::size_t ts, std::size_t sb, double amplitude) {
            return std::abs(signal[ts * kSubbands + sb]) / amplitude;
        };
        const double alpha = 0.76592833836465;
        // Slot 0: the peak is the energy, so no difference and no ducking.
        CHECK(gain(0, 40, 2.0) == Approx(1.0));
        // Slot 1: peak alpha E, smooth 0.75 (0.25 E) + 0.25 e, difference
        // 0.25 (alpha E - e); 1.5 times the difference exceeds the smoothed
        // energy, so the gain is their ratio.
        const double smooth = 0.75 * 0.25 * kE + 0.25 * ke;
        const double diff = 0.25 * (alpha * kE - ke);
        REQUIRE(1.5 * diff > smooth);
        CHECK(gain(1, 40, 0.1) == Approx(smooth / (1.5 * (diff + 1e-9))));
        // Subband 3's band has steady energy: no ducking.
        CHECK(gain(1, 3, 0.1) == Approx(1.0));
        // Once the peak has decayed below e the gain returns to 1.
        CHECK(gain(at(kSlots - 1), 40, 0.1) == Approx(1.0).margin(1e-6));
    }
}
