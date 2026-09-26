// A-JOC in the AC-4 shared core (src/ac4core/src/ajoc): ETSI TS 103 190-2
// V1.3.1 clause 5.7's band mapping (Table 28), dequantisation (Tables 29 to
// 32) and differential decoding (Pseudocode 16), and the reconstruction
// (Pseudocodes 17 and 18) against its formulas on inputs whose outputs can be
// worked by hand: z = C_dry x + C_wet y with the coefficients ramped slot by
// slot, y the decorrelated and ducked D x, D = |C_wet^T| C_dry; dialogue
// enhancement in full decoding (Pseudocode 22) and in core decoding (clause
// 5.8.2.4).

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "acpl/acpl.hpp"
#include "ajoc/ajoc.hpp"

namespace {

namespace ajoc = ac4::detail::ajoc;
namespace acpl = ac4::detail::acpl;
using Complex = std::complex<double>;
using Catch::Approx;

constexpr int kSlots = 32;
constexpr int kSubbands = 64;
constexpr std::size_t kValues = static_cast<std::size_t>(kSlots) * kSubbands;

[[nodiscard]] std::size_t at(int index) {
    return static_cast<std::size_t>(index);
}

// A matrix whose every value is `v`.
std::vector<Complex> constant(Complex v) {
    return std::vector<Complex>(kValues, v);
}

// A frame of parameters: one data point at slot `start` ramping over `ramp`
// slots, `bands` bands for every object, each object's dry coefficients
// `dry[o][ch]` in every band.
ajoc::FrameParameters parameters(const std::vector<std::vector<double>>& dry, int bands, int start,
                                 int ramp) {
    ajoc::FrameParameters p;
    p.resize(static_cast<int>(dry[0].size()), static_cast<int>(dry.size()));
    p.num_dpoints = 1;
    p.start_pos[0] = start;
    p.ramp_len[0] = ramp;
    for (int o = 0; o < p.num_umx; ++o) {
        p.num_bands[at(o)] = bands;
        for (int ch = 0; ch < p.num_dmx; ++ch) {
            for (int pb = 0; pb < bands; ++pb) {
                p.dry_at(o, 0, ch, pb) = dry[at(o)][at(ch)];
            }
        }
    }
    return p;
}

struct Run {
    std::vector<std::vector<Complex>> x;
    std::vector<std::vector<Complex>> z;
    std::vector<const std::vector<Complex>*> in;
    std::vector<std::vector<Complex>*> out;

    Run(std::vector<std::vector<Complex>> inputs, std::size_t outputs)
        : x(std::move(inputs)), z(outputs) {
        for (const auto& m : x) {
            in.push_back(&m);
        }
        for (auto& m : z) {
            out.push_back(&m);
        }
    }
};

}  // namespace

TEST_CASE("A-JOC Table 28 maps the 64 QMF subbands to each band count", "[ac4core][ajoc]") {
    constexpr std::array<int, 8> kCounts = {23, 15, 12, 9, 7, 5, 3, 1};
    for (int code = 0; code < 8; ++code) {
        CHECK(ajoc::num_bands(code) == kCounts[at(code)]);
    }
    for (const int bands : kCounts) {
        CAPTURE(bands);
        int previous = 0;
        for (int sb = 0; sb < kSubbands; ++sb) {
            const int pb = ajoc::sb_to_pb(bands, sb);
            CHECK(pb >= previous);
            CHECK(pb <= previous + 1);
            previous = pb;
        }
        CHECK(ajoc::sb_to_pb(bands, 0) == 0);
        CHECK(ajoc::sb_to_pb(bands, 63) == bands - 1);
    }
    // Rows of the rendered page 82: 12 to 13, 23 to 25 and 48 to 63.
    const std::array<int, 8> row12 = {12, 10, 7, 6, 4, 3, 1, 0};
    const std::array<int, 8> row25 = {17, 13, 10, 8, 6, 4, 2, 0};
    const std::array<int, 8> row48 = {22, 14, 11, 8, 6, 4, 2, 0};
    for (std::size_t c = 0; c < kCounts.size(); ++c) {
        CHECK(ajoc::sb_to_pb(kCounts[c], 13) == row12[c]);
        CHECK(ajoc::sb_to_pb(kCounts[c], 25) == row25[c]);
        CHECK(ajoc::sb_to_pb(kCounts[c], 48) == row48[c]);
    }
}

TEST_CASE("A-JOC Tables 29 to 32 dequantise about their centres", "[ac4core][ajoc]") {
    CHECK(ajoc::dequantise(false, 1, 0) == -5.0048828125);
    CHECK(ajoc::dequantise(false, 1, 25) == 0.0);
    CHECK(ajoc::dequantise(false, 1, 9) == -3.203125);
    CHECK(ajoc::dequantise(false, 0, 100) == 5.0048828125);
    CHECK(ajoc::dequantise(false, 0, 94) == 4.404296875);
    CHECK(ajoc::dequantise(true, 1, 0) == -2.001953125);
    CHECK(ajoc::dequantise(true, 1, 4) == -1.201171875);
    CHECK(ajoc::dequantise(true, 0, 40) == 2.001953125);
    CHECK(ajoc::dequantise(true, 0, 20) == 0.0);
}

TEST_CASE("A-JOC Pseudocode 16 wraps along frequency and adds along time", "[ac4core][ajoc]") {
    std::array<int, ajoc::kMaxBands> previous{};
    std::array<int, ajoc::kMaxBands> out{};
    // DIFF_FREQ: the first value as it is, then each next one added modulo
    // nquant (51 coarse dry).
    const std::array<int, 3> freq = {40, 20, 50};
    REQUIRE(ajoc::differential_decode(false, 51, 3, freq, previous, out));
    CHECK(out[0] == 40);
    CHECK(out[1] == 9);  // (40 + 20) mod 51
    CHECK(out[2] == 8);  // (9 + 50) mod 51
    // DIFF_TIME: each added to the data point before, within 0 to nquant - 1.
    previous = out;
    const std::array<int, 3> time = {-40, 2, 42};
    REQUIRE(ajoc::differential_decode(true, 51, 3, time, previous, out));
    CHECK(out[0] == 0);
    CHECK(out[1] == 11);
    CHECK(out[2] == 50);
    const std::array<int, 1> outside = {11};
    previous[0] = 45;
    CHECK_FALSE(ajoc::differential_decode(true, 51, 1, outside, previous, out));
    // A first DIFF_FREQ value past the range.
    const std::array<int, 1> first = {51};
    CHECK_FALSE(ajoc::differential_decode(false, 51, 1, first, previous, out));
}

TEST_CASE("A-JOC decorrelators are D0 D2 D1 in turn", "[ac4core][ajoc]") {
    const std::array<int, 7> expected = {0, 2, 1, 0, 2, 1, 0};
    for (int de = 0; de < 7; ++de) {
        CHECK(ajoc::decorrelator_of(de) == expected[at(de)]);
    }
}

TEST_CASE("A-JOC's dry matrix gives C_dry x once its ramp has run", "[ac4core][ajoc]") {
    const std::vector<std::vector<double>> dry = {{0.5, 0.0}, {0.0, 1.0}, {0.25, -0.5}};
    const ajoc::FrameParameters p = parameters(dry, 1, 0, 1);
    auto r = std::make_unique<ajoc::Reconstruction<double>>();
    Run run({constant({1.0, 0.0}), constant({0.0, 2.0})}, 3);
    for (int frame = 0; frame < 2; ++frame) {
        r->reconstruct(p, kSlots, run.in, run.out, 1.0, {});
        for (int o = 0; o < 3; ++o) {
            const Complex expected =
                dry[at(o)][0] * Complex{1.0, 0.0} + dry[at(o)][1] * Complex{0.0, 2.0};
            for (int ts = 0; ts < kSlots; ++ts) {
                CAPTURE(frame, o, ts);
                // The ramp starts after slot 0 of the first frame, from 0.
                const Complex want = frame == 0 && ts == 0 ? Complex{} : expected;
                const Complex got = run.z[at(o)][at(ts) * kSubbands + 17];
                CHECK(got.real() == Approx(want.real()).margin(1e-12));
                CHECK(got.imag() == Approx(want.imag()).margin(1e-12));
            }
        }
    }
}

TEST_CASE("A-JOC's ramp reaches its target in ajoc_ramp_len slots and holds", "[ac4core][ajoc]") {
    // Pseudocodes 17 and 18: the value moves by (target - prev) / ramp_len a
    // slot from the slot after ajoc_start_pos, so it arrives after ramp_len
    // slots and stays (src/ac4dec/ERRATA.md, "A-JOC's ramp").
    ajoc::FrameParameters p = parameters({{1.0}}, 1, 4, 8);
    auto r = std::make_unique<ajoc::Reconstruction<double>>();
    Run run({constant({1.0, 0.0})}, 1);
    r->reconstruct(p, kSlots, run.in, run.out, 1.0, {});
    for (int ts = 0; ts < kSlots; ++ts) {
        CAPTURE(ts);
        const double want = ts <= 4 ? 0.0 : std::min(1.0, (ts - 4) / 8.0);
        CHECK(run.z[0][at(ts) * kSubbands].real() == Approx(want).margin(1e-12));
    }
    // A second data point at slot 20 ramping to -0.5 over 4 slots; a ramp
    // runs across the frame boundary where it is longer than what is left.
    p.num_dpoints = 2;
    p.start_pos = {4, 20};
    p.ramp_len = {8, 4};
    p.dry_at(0, 1, 0, 0) = -0.5;
    r->reconstruct(p, kSlots, run.in, run.out, 1.0, {});
    for (int ts = 0; ts < kSlots; ++ts) {
        CAPTURE(ts);
        double want = 1.0;  // held from the first frame, and slot 4's point is 1.0 again
        if (ts > 20) {
            want = std::max(-0.5, 1.0 - 1.5 * (ts - 20) / 4.0);
        }
        CHECK(run.z[0][at(ts) * kSubbands].real() == Approx(want).margin(1e-12));
    }
}

TEST_CASE("A-JOC's coefficients follow Table 28 subband by subband", "[ac4core][ajoc]") {
    // 23 bands, each band's coefficient its index / 10.
    ajoc::FrameParameters p = parameters({{0.0}}, 23, 0, 1);
    for (int pb = 0; pb < 23; ++pb) {
        p.dry_at(0, 0, 0, pb) = pb / 10.0;
    }
    auto r = std::make_unique<ajoc::Reconstruction<double>>();
    Run run({constant({1.0, 0.0})}, 1);
    r->reconstruct(p, kSlots, run.in, run.out, 1.0, {});
    for (int sb = 0; sb < kSubbands; ++sb) {
        CAPTURE(sb);
        CHECK(run.z[0][5 * kSubbands + at(sb)].real() ==
              Approx(ajoc::sb_to_pb(23, sb) / 10.0).margin(1e-12));
    }
}

TEST_CASE("A-JOC's wet path is the ducked decorrelator of D x, D = |C_wet| C_dry",
          "[ac4core][ajoc]") {
    // One input, one object of dry 0.6 and wet 0.4 on decorrelator 0: the
    // decorrelator's input is 0.24 x, ramped as the coefficients are.
    ajoc::FrameParameters p = parameters({{0.6}}, 1, 0, 1);
    p.num_decorr = 1;
    p.decorr_enable[0] = true;
    p.wet_at(0, 0, 0, 0) = 0.4;
    auto r = std::make_unique<ajoc::Reconstruction<double>>();
    // A signal that changes slot by slot, so the decorrelator and the ducker
    // have something to do.
    std::vector<Complex> x(kValues);
    for (std::size_t k = 0; k < kValues; ++k) {
        x[k] = std::polar(1.0 + 0.5 * std::sin(0.1 * static_cast<double>(k)),
                          0.3 * static_cast<double>(k));
    }
    Run run({x}, 1);
    // The same decorrelator and ducker, on D x by hand.
    auto decorrelator = std::make_unique<acpl::Decorrelator<double>>(0);
    auto ducker = std::make_unique<acpl::TransientDucker<double>>();
    for (int frame = 0; frame < 2; ++frame) {
        r->reconstruct(p, kSlots, run.in, run.out, 1.0, {});
        std::vector<Complex> u(kValues);
        for (int ts = 0; ts < kSlots; ++ts) {
            const double d = frame == 0 && ts == 0 ? 0.0 : 0.24;
            for (int sb = 0; sb < kSubbands; ++sb) {
                u[at(ts) * kSubbands + at(sb)] = d * x[at(ts) * kSubbands + at(sb)];
            }
        }
        std::vector<Complex> y(kValues);
        decorrelator->process(u, y, kSlots);
        ducker->process(y, kSlots);
        for (int ts = 0; ts < kSlots; ++ts) {
            const double dry = frame == 0 && ts == 0 ? 0.0 : 0.6;
            const double wet = frame == 0 && ts == 0 ? 0.0 : 0.4;
            for (int sb = 0; sb < kSubbands; sb += 7) {
                const std::size_t k = at(ts) * kSubbands + at(sb);
                const Complex want = dry * x[k] + wet * y[k];
                CAPTURE(frame, ts, sb);
                CHECK(std::abs(run.z[0][k] - want) < 1e-12);
            }
        }
    }
}

TEST_CASE("A-JOC's decorrelation input matrix takes each object at its own bands",
          "[ac4core][ajoc]") {
    // Two objects of 23 bands and of 1 on one decorrelator (src/ac4dec/
    // ERRATA.md, "The decorrelation input matrix"): in subband sb, D is
    // |wet_0| dry_0 at object 0's band of sb plus |wet_1| dry_1, object 1's
    // one band covering every subband.
    ajoc::FrameParameters p = parameters({{0.0}, {0.5}}, 1, 0, 1);
    p.num_bands[0] = 23;
    for (int pb = 0; pb < 23; ++pb) {
        p.dry_at(0, 0, 0, pb) = pb / 20.0;
        p.wet_at(0, 0, 0, pb) = -0.2;  // its magnitude counts
    }
    p.num_decorr = 1;
    p.decorr_enable[0] = true;
    p.wet_at(1, 0, 0, 0) = 0.3;
    auto r = std::make_unique<ajoc::Reconstruction<double>>();
    std::vector<Complex> x(kValues);
    for (std::size_t k = 0; k < kValues; ++k) {
        x[k] = std::polar(1.0 + 0.5 * std::cos(0.07 * static_cast<double>(k)),
                          0.2 * static_cast<double>(k));
    }
    Run run({x}, 2);
    auto decorrelator = std::make_unique<acpl::Decorrelator<double>>(0);
    auto ducker = std::make_unique<acpl::TransientDucker<double>>();
    for (int frame = 0; frame < 2; ++frame) {
        r->reconstruct(p, kSlots, run.in, run.out, 1.0, {});
        std::vector<Complex> u(kValues);
        for (int ts = 0; ts < kSlots; ++ts) {
            for (int sb = 0; sb < kSubbands; ++sb) {
                const double d = 0.2 * (ajoc::sb_to_pb(23, sb) / 20.0) + 0.3 * 0.5;
                u[at(ts) * kSubbands + at(sb)] =
                    (frame == 0 && ts == 0 ? 0.0 : d) * x[at(ts) * kSubbands + at(sb)];
            }
        }
        std::vector<Complex> y(kValues);
        decorrelator->process(u, y, kSlots);
        ducker->process(y, kSlots);
        for (int ts = 0; ts < kSlots; ++ts) {
            for (int sb = 0; sb < kSubbands; sb += 3) {
                const std::size_t k = at(ts) * kSubbands + at(sb);
                const double scale = frame == 0 && ts == 0 ? 0.0 : 1.0;
                const Complex want = scale * (0.5 * x[k] + 0.3 * y[k]);
                CAPTURE(frame, ts, sb);
                CHECK(std::abs(run.z[1][k] - want) < 1e-12);
            }
        }
    }
}

TEST_CASE("A-JOC dialogue enhancement scales the dialogue objects after D is taken",
          "[ac4core][ajoc]") {
    // Pseudocode 22 in full decoding: the dialogue object's dry and wet times
    // de_gain; the decorrelation input matrix is Pseudocode 18's, taken before.
    ajoc::FrameParameters p = parameters({{0.6}}, 1, 0, 1);
    p.num_decorr = 1;
    p.decorr_enable[0] = true;
    p.wet_at(0, 0, 0, 0) = 0.4;
    auto plain = std::make_unique<ajoc::Reconstruction<double>>();
    auto enhanced = std::make_unique<ajoc::Reconstruction<double>>();
    std::vector<Complex> x(kValues);
    for (std::size_t k = 0; k < kValues; ++k) {
        x[k] = std::polar(1.0, 0.7 * static_cast<double>(k));
    }
    Run a({x}, 1);
    Run b({x}, 1);
    const std::array<std::uint8_t, 1> dialogue = {1};
    for (int frame = 0; frame < 2; ++frame) {
        plain->reconstruct(p, kSlots, a.in, a.out, 1.0, dialogue);
        enhanced->reconstruct(p, kSlots, b.in, b.out, 2.0, dialogue);
    }
    // Every value of the enhanced object twice the plain one's: the same
    // decorrelator input, both paths doubled.
    for (std::size_t k = kSubbands; k < kValues; k += 13) {
        CHECK(std::abs(b.z[0][k] - 2.0 * a.z[0][k]) < 1e-12);
    }
    // A gain of 1 or less leaves the coefficients alone.
    auto unity = std::make_unique<ajoc::Reconstruction<double>>();
    Run c({x}, 1);
    for (int frame = 0; frame < 2; ++frame) {
        unity->reconstruct(p, kSlots, c.in, c.out, 0.5, dialogue);
    }
    for (std::size_t k = 0; k < kValues; k += 13) {
        CHECK(std::abs(c.z[0][k] - a.z[0][k]) < 1e-12);
    }
}

TEST_CASE("A-JOC core decoding's dialogue enhancement adds H_M H_A x", "[ac4core][ajoc]") {
    // Clause 5.8.2.4: two downmix signals, object 0 the dialogue at dry (0.5,
    // 0.25), object 1 not; its downmix coefficients (1, 0.5); de_gain 1
    // (10^(G/20) - 1 at G of 6.02 dB). H_M ramps from 0 over the first frame.
    const ajoc::FrameParameters p = parameters({{0.5, 0.25}, {1.0, 0.0}}, 1, 0, 1);
    auto r = std::make_unique<ajoc::Reconstruction<double>>();
    const Complex x0{1.0, 0.0};
    const Complex x1{0.0, 1.0};
    std::vector<std::vector<Complex>> x = {constant(x0), constant(x1)};
    std::vector<std::vector<Complex>*> ptrs = {&x[0], &x[1]};
    const std::array<std::uint8_t, 2> dialogue = {1, 0};
    const std::array<double, 2> coeff = {1.0, 0.5};
    r->enhance_core(p, kSlots, ptrs, 1.0, dialogue, coeff);
    for (int ts = 0; ts < kSlots; ++ts) {
        const double h_a = ts == 0 ? 0.0 : 1.0;  // the dry ramp, times de_gain
        const Complex dlg = h_a * (0.5 * x0 + 0.25 * x1);
        const double alpha = (ts + 1) / static_cast<double>(kSlots);
        CAPTURE(ts);
        CHECK(std::abs(x[0][at(ts) * kSubbands + 9] - (x0 + alpha * 1.0 * dlg)) < 1e-12);
        CHECK(std::abs(x[1][at(ts) * kSubbands + 9] - (x1 + alpha * 0.5 * dlg)) < 1e-12);
    }
    // The next frame's H_M is the coefficients throughout.
    x = {constant(x0), constant(x1)};
    r->enhance_core(p, kSlots, ptrs, 1.0, dialogue, coeff);
    const Complex dlg = 0.5 * x0 + 0.25 * x1;
    CHECK(std::abs(x[0][9] - (x0 + dlg)) < 1e-12);
    CHECK(std::abs(x[1][9] - (x1 + 0.5 * dlg)) < 1e-12);
}
