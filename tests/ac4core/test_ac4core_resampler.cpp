// The AC-4 sample rate converter (src/ac4core/include/iclforge/ac4core/dsp/resampler.hpp): the
// output sample count of every frame rate of ETSI TS 103 190-1 V1.4.1 Table 83,
// the sequence ETSI TS 103 190-2 V1.3.1 Table 47 locks to sequence_counter, and
// the filter's passband, stopband and delay, measured with tones, in the
// decoder's direction and the encoder's.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <tuple>
#include <utility>
#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "iclforge/ac4core/dsp/resampler.hpp"
#include "iclforge/ac4core/dsp/resampler_design.hpp"
#include "iclforge/ac4core/dsp/resampler_vector.hpp"
#include "iclforge/ac4core/dsp/scalar_traits.hpp"

namespace {

namespace dsp = iclforge::ac4::detail::dsp;

// The decoder's scalar: float in a float build, where the converter's table is
// kept and its dot product run at float, and double otherwise.
using Real = iclforge::ac4::detail::Real;
using RealFilter = dsp::BasicResamplerFilter<Real>;

struct Rate {
    int index;
    int frame;  // frame_len_base, the internal frame length at 48 kHz
    int up;     // the decoder's resampling ratio, up / down
    int down;
};

// Table 83 at 48 kHz: 1001/1000 x 25/24 is 1001/960.
constexpr std::array<Rate, 13> kRates{{
    {0, 1920, 1001, 960},
    {1, 1920, 25, 24},
    {2, 2048, 15, 16},
    {3, 1536, 1001, 960},
    {4, 1536, 25, 24},
    {5, 960, 1001, 960},
    {6, 960, 25, 24},
    {7, 1024, 15, 16},
    {8, 768, 1001, 960},
    {9, 768, 25, 24},
    {10, 512, 15, 16},
    {11, 384, 1001, 960},
    {12, 384, 25, 24},
}};

std::int64_t floor_div(std::int64_t a, std::int64_t b) {
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

struct Tone {
    double amplitude = 0.0;
    double phase = 0.0;         // y[m] = amplitude cos(w m - phase)
    double residual_rms = 0.0;  // what the fitted tone leaves
};

// Least squares of y[m] = a cos(w m) + b sin(w m) over y.
Tone fit_tone(std::span<const double> y, double w) {
    double cc = 0.0;
    double ss = 0.0;
    double cs = 0.0;
    double yc = 0.0;
    double ys = 0.0;
    for (std::size_t m = 0; m < y.size(); ++m) {
        const double c = std::cos(w * static_cast<double>(m));
        const double s = std::sin(w * static_cast<double>(m));
        cc += c * c;
        ss += s * s;
        cs += c * s;
        yc += y[m] * c;
        ys += y[m] * s;
    }
    const double det = cc * ss - cs * cs;
    if (y.empty() || det == 0.0) {
        return {};
    }
    const double a = (yc * ss - ys * cs) / det;
    const double b = (ys * cc - yc * cs) / det;
    double residual = 0.0;
    for (std::size_t m = 0; m < y.size(); ++m) {
        const double e = y[m] - a * std::cos(w * static_cast<double>(m)) -
                         b * std::sin(w * static_cast<double>(m));
        residual += e * e;
    }
    return {.amplitude = std::hypot(a, b),
            .phase = std::atan2(b, a),
            .residual_rms = std::sqrt(residual / static_cast<double>(y.size()))};
}

// Converts `count` samples of cos(2 pi f n), f in cycles per input sample, and
// returns the output after the filter's reach, whole.
std::vector<double> convert_tone(const std::shared_ptr<const dsp::ResamplerFilter>& filter,
                                 double f, std::size_t count) {
    std::vector<double> in(count);
    for (std::size_t n = 0; n < count; ++n) {
        in[n] = std::cos(2.0 * std::numbers::pi * f * static_cast<double>(n));
    }
    dsp::Resampler<double> resampler(filter);
    std::vector<double> out;
    resampler.process(in, out);
    // The first outputs' taps reach back before the tone started.
    const auto skip =
        static_cast<std::size_t>(2 * filter->taps() * filter->up() / filter->down() + 2);
    return {out.begin() + static_cast<std::ptrdiff_t>(skip), out.end()};
}

double to_db(double ratio) {
    return 20.0 * std::log10(ratio);
}

}  // namespace

TEST_CASE("the sample rate converter gives every frame rate its exact output count, from any frame",
          "[ac4core][dsp][src]") {
    constexpr std::int64_t kFrames = 100'000;
    for (const Rate& rate : kRates) {
        CAPTURE(rate.index);
        const auto filter = std::make_shared<const dsp::ResamplerFilter>(rate.up, rate.down);
        dsp::Resampler<double> resampler(filter);
        const std::int64_t n = rate.frame;
        std::int64_t total = 0;
        // Started at frame t, as a phase-locked converter is, the next frame
        // gives floor((t + 1) R) - floor(t R), R = N x up / down.
        for (std::int64_t t = 0; t < kFrames; ++t) {
            resampler.reset(t * n);
            const auto count =
                static_cast<std::int64_t>(resampler.outputs_for(static_cast<std::size_t>(n)));
            REQUIRE(count == floor_div((t + 1) * n * rate.up, rate.down) -
                                 floor_div(t * n * rate.up, rate.down));
            total += count;
        }
        // Whole: 48 000 samples a second at every rate.
        CHECK(total == kFrames * n * rate.up / rate.down);
        CHECK(total * rate.down == kFrames * n * rate.up);
    }
}

TEST_CASE("the converter runs through frames in Table 47's sequence of output counts",
          "[ac4core][dsp][src]") {
    struct Sequence {
        int frame;
        std::array<std::size_t, 5> counts;  // phi_t = 0 .. 4
    };
    // Part 2 Table 47: 29.97, 59.94 and 119.88 fps.
    constexpr std::array<Sequence, 3> kSequences{{
        {1536, {1601, 1602, 1601, 1602, 1602}},
        {768, {800, 801, 801, 801, 801}},
        {384, {400, 400, 401, 400, 401}},
    }};
    const auto filter = std::make_shared<const dsp::ResamplerFilter>(1001, 960);
    for (const Sequence& sequence : kSequences) {
        CAPTURE(sequence.frame);
        dsp::Resampler<double> resampler(filter);
        const std::vector<double> frame(static_cast<std::size_t>(sequence.frame), 0.25);
        std::vector<double> out;
        for (std::size_t t = 0; t < 25; ++t) {
            out.clear();
            resampler.process(frame, out);
            CHECK(out.size() == sequence.counts[t % 5]);
        }
        // A constant comes through as itself once the taps are past the start.
        for (const double v : out) {
            CHECK(std::abs(v - 0.25) < 1e-12);
        }
        // Started at phi_t = 3, as sequence_counter 3 would start it.
        resampler.reset(3 * static_cast<std::int64_t>(sequence.frame));
        for (std::size_t t = 3; t < 13; ++t) {
            out.clear();
            resampler.process(frame, out);
            CHECK(out.size() == sequence.counts[t % 5]);
        }
    }
}

TEST_CASE("a converter whose phase jumps gives the new phase's counts and keeps converting",
          "[ac4core][dsp][src]") {
    const auto filter = std::make_shared<const dsp::ResamplerFilter>(1001, 960);
    constexpr std::int64_t kFrame = 1536;
    constexpr std::array<std::size_t, 5> kCounts = {1601, 1602, 1601, 1602, 1602};
    dsp::Resampler<double> resampler(filter);
    const std::vector<double> frame(static_cast<std::size_t>(kFrame), -0.5);
    std::vector<double> out;
    for (std::size_t t = 0; t < 3; ++t) {
        out.clear();
        resampler.process(frame, out);
        CHECK(out.size() == kCounts[t]);
    }
    // phi_t goes 0, 1, 2 and then 4: the sequence goes on from there.
    resampler.rephase(4 * kFrame);
    constexpr std::array<std::size_t, 4> kPhases = {4, 0, 1, 2};
    for (const std::size_t phase : kPhases) {
        out.clear();
        resampler.process(frame, out);
        CHECK(out.size() == kCounts[phase]);
        // The constant goes on through the jump: nothing silenced, nothing lost.
        for (const double v : out) {
            CHECK(std::abs(v + 0.5) < 1e-12);
        }
    }
}

TEST_CASE("a converter started at a later frame puts its samples where one run from the start does",
          "[ac4core][dsp][src]") {
    const auto filter = std::make_shared<const dsp::ResamplerFilter>(1001, 960);
    constexpr std::size_t kFrame = 1536;
    std::vector<double> signal(12 * kFrame);
    for (std::size_t n = 0; n < signal.size(); ++n) {
        signal[n] = std::sin(0.013 * static_cast<double>(n)) +
                    0.3 * std::cos(0.41 * static_cast<double>(n));
    }
    dsp::Resampler<double> whole(filter);
    std::vector<std::vector<double>> frames(12);
    for (std::size_t t = 0; t < 12; ++t) {
        whole.process(std::span<const double>(signal).subspan(t * kFrame, kFrame), frames[t]);
    }
    dsp::Resampler<double> late(filter);
    late.reset(4 * static_cast<std::int64_t>(kFrame));
    for (std::size_t t = 4; t < 12; ++t) {
        std::vector<double> out;
        late.process(std::span<const double>(signal).subspan(t * kFrame, kFrame), out);
        REQUIRE(out.size() == frames[t].size());
        // From the second frame on the taps see only what both converters were given.
        if (t >= 5) {
            CHECK(out == frames[t]);
        }
    }
}

TEST_CASE("the converter's passband is flat to 0.001 dB and its stopband 100 dB down, both ways",
          "[ac4core][dsp][src]") {
    struct Ratio {
        int up;
        int down;
    };
    // The decoder's three ratios, and the encoder's.
    constexpr std::array<Ratio, 6> kRatios{
        {{25, 24}, {15, 16}, {1001, 960}, {24, 25}, {16, 15}, {960, 1001}}};
    for (const Ratio& ratio : kRatios) {
        CAPTURE(ratio.up, ratio.down);
        const auto filter = std::make_shared<const dsp::ResamplerFilter>(ratio.up, ratio.down);
        const double to_out = static_cast<double>(ratio.down) / static_cast<double>(ratio.up);
        // In the passband: the gain, and what else comes out - the images and
        // aliases the stopband holds down.
        double ripple_db = 0.0;
        double residual_db = -400.0;
        for (const double fraction : {0.02, 0.3, 0.6, 0.9, 1.0}) {
            const double f = fraction * filter->passband_edge();
            const std::vector<double> y = convert_tone(filter, f, 24'000);
            const Tone tone = fit_tone(y, 2.0 * std::numbers::pi * f * to_out);
            ripple_db = std::max(ripple_db, std::abs(to_db(tone.amplitude)));
            residual_db = std::max(residual_db,
                                   to_db(tone.residual_rms * std::numbers::sqrt2 / tone.amplitude));
        }
        CAPTURE(filter->taps(), ripple_db, residual_db);
        CHECK(ripple_db < 0.001);
        CHECK(residual_db < -100.0);
        // Converting down, a tone between the two Nyquist frequencies does not
        // come through. (Converting up, the input has nothing there; its
        // images are the residual above.)
        if (ratio.up > ratio.down) {
            continue;
        }
        double leak_db = -400.0;
        for (const double fraction : {1.02, 1.04}) {
            const double f = std::min(0.4999, fraction * filter->stopband_edge());
            const std::vector<double> y = convert_tone(filter, f, 24'000);
            double power = 0.0;
            for (const double v : y) {
                power += v * v;
            }
            leak_db =
                std::max(leak_db, to_db(std::sqrt(2.0 * power / static_cast<double>(y.size()))));
        }
        CAPTURE(leak_db);
        CHECK(leak_db < -100.0);
    }
}

TEST_CASE("the converter delays by the delay() it states", "[ac4core][dsp][src]") {
    for (const Rate& rate : {kRates[0], kRates[1], kRates[2]}) {
        CAPTURE(rate.index);
        const auto filter = std::make_shared<const dsp::ResamplerFilter>(rate.up, rate.down);
        // A tone slow enough that its phase names the delay unambiguously.
        const double f = 0.001;
        const double to_out = static_cast<double>(rate.down) / static_cast<double>(rate.up);
        std::vector<double> in(40'000);
        for (std::size_t n = 0; n < in.size(); ++n) {
            in[n] = std::cos(2.0 * std::numbers::pi * f * static_cast<double>(n));
        }
        dsp::Resampler<double> resampler(filter);
        std::vector<double> out;
        resampler.process(in, out);
        // Output m stands for input m x down / up - delay(): from output
        // `skip` on, y = cos(w m - phase) with phase = -2 pi f (start - delay).
        const std::size_t skip = 400;
        const Tone tone = fit_tone(std::span<const double>(out).subspan(skip),
                                   2.0 * std::numbers::pi * f * to_out);
        const double start = static_cast<double>(skip) * to_out;
        const double delay = start + tone.phase / (2.0 * std::numbers::pi * f);
        CHECK(std::abs(delay - filter->delay()) < 1e-4);
    }
}

namespace {

// The float table of a ratio as the converter keeps it, whichever scalar the build runs at: the
// design of dsp/resampler_design.hpp made now, at run time, with the portable functions, phases 0
// to up / 2 rounded to float, and the rest written out as those read backwards.
std::vector<float> portable_float_table(int up, int down, int& taps) {
    // Through volatile, so that the compiler cannot evaluate the design while it compiles this.
    volatile int up_now = up;
    volatile int down_now = down;
    const dsp::ResamplerDesign design = dsp::design_resampler<dsp::PortableMath>(up_now, down_now);
    taps = design.taps;
    const auto width = static_cast<std::size_t>(taps);
    std::vector<float> half(static_cast<std::size_t>(up / 2 + 1) * width);
    std::vector<double> row(width);
    dsp::design_half_phases<dsp::PortableMath>(design, half.data(), row.data());
    std::vector<float> full(static_cast<std::size_t>(up) * width);
    for (int p = 0; p < up; ++p) {
        const bool mirrored = p > up / 2;
        const auto source = static_cast<std::size_t>(mirrored ? up - p : p);
        for (std::size_t k = 0; k < width; ++k) {
            full[static_cast<std::size_t>(p) * width + k] =
                half[source * width + (mirrored ? width - 1 - k : k)];
        }
    }
    return full;
}

// FNV-1a over the bit pattern of every coefficient of a float table.
std::uint64_t fnv_float_image(const std::vector<float>& coefficients) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const float coefficient : coefficients) {
        const auto bits = std::bit_cast<std::uint32_t>(coefficient);
        for (unsigned shift = 0; shift < 32; shift += 8) {
            hash ^= (bits >> shift) & 0xFFU;
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

}  // namespace

TEST_CASE("the converter's table is the double design rounded once to the scalar it runs at",
          "[ac4core][dsp][src]") {
    const double epsilon = dsp::kFixed<Real> ? 0x1p-30 : static_cast<double>(std::numeric_limits<Real>::epsilon());
    for (const Rate& rate : {kRates[1], kRates[2], kRates[0]}) {
        CAPTURE(rate.index);
        const dsp::ResamplerFilter design(rate.up, rate.down);
        const RealFilter kept(rate.up, rate.down);
        REQUIRE(kept.up() == design.up());
        REQUIRE(kept.down() == design.down());
        REQUIRE(kept.taps() == design.taps());
        CHECK(kept.delay() == design.delay());
        // At float the table is the compiler's, designed without the C library and with the
        // mirrored half of it read backwards; at double it is the C library's design. Every
        // coefficient is what the double design rounds to.
        // At Fixed32 the table is Q1.30, each coefficient the double design times 2^30 rounded
        // half away from zero, which the phase's own pointer reads.
        int rounded_differently = 0;
        double worst_sum = 0.0;
        // A generic lambda, so that each scalar's branch is compiled only at that scalar.
        const auto check = [&]<typename R>(int p, int k, double& sum) {
            if constexpr (dsp::kFixed<R>) {
                const auto phase = kept.phase(p);
                const auto stored = static_cast<std::int32_t>(
                    phase.coefficients[phase.reversed ? design.taps() - 1 - k : k]);
                const double scaled = design.coefficient(p, k) * 0x1p30;
                if (stored != static_cast<std::int32_t>(scaled < 0.0 ? scaled - 0.5 : scaled + 0.5)) {
                    ++rounded_differently;
                }
                sum += std::ldexp(static_cast<double>(stored), -30);
            } else {
                const R stored = kept.coefficient(p, k);
                if (stored != static_cast<R>(design.coefficient(p, k))) {
                    ++rounded_differently;
                }
                sum += static_cast<double>(stored);
            }
        };
        for (int p = 0; p < design.up(); ++p) {
            double sum = 0.0;
            for (int k = 0; k < design.taps(); ++k) {
                check.template operator()<Real>(p, k, sum);
            }
            worst_sum = std::max(worst_sum, std::abs(sum - 1.0));
        }
        CHECK(rounded_differently == 0);
        // A phase still sums to 1 to the scalar's precision: a constant passes unchanged.
        CHECK(worst_sum < epsilon * design.taps());
    }
}

TEST_CASE("the converter's table rounded to float is the same on every platform",
          "[ac4core][dsp][src]") {
    // FNV-1a over the bit pattern of every coefficient of every phase as a float. The table is
    // pinned three ways: the double design's entries rounded once (the design calls sin and sqrt at
    // double, whose last bit the C libraries do not all agree on), the same design with the
    // portable functions made now at run time, and, in a float build, the table the compiler made.
    // A float table that differs by a bit anywhere would give the host, the Cortex-M3 leg and the
    // ESP32s each a converter of its own (planning/ac4.md, D14a4 and D14a5): what the pins hold is
    // that the libraries in CI round it the same way and that the compiler's evaluation agrees with
    // the machine's.
    struct Pin {
        const Rate& rate;
        std::uint64_t hash;
    };
    const std::array<Pin, 3> pins{{{kRates[1], 0x6b1acefe1feaea93ULL},
                                   {kRates[2], 0x59f33c109eaa0ec5ULL},
                                   {kRates[0], 0xf55c1b394d2a0147ULL}}};
    for (const Pin& pin : pins) {
        CAPTURE(pin.rate.index, pin.rate.up, pin.rate.down);
        const dsp::ResamplerFilter design(pin.rate.up, pin.rate.down);
        std::vector<float> rounded;
        for (int p = 0; p < design.up(); ++p) {
            for (int k = 0; k < design.taps(); ++k) {
                rounded.push_back(static_cast<float>(design.coefficient(p, k)));
            }
        }
        CHECK(fnv_float_image(rounded) == pin.hash);
        int taps = 0;
        const std::vector<float> portable_table =
            portable_float_table(pin.rate.up, pin.rate.down, taps);
        REQUIRE(taps == design.taps());
        CHECK(fnv_float_image(portable_table) == pin.hash);
        if constexpr (std::is_same_v<Real, float>) {
            const RealFilter kept(pin.rate.up, pin.rate.down);
            std::vector<float> image;
            for (int p = 0; p < kept.up(); ++p) {
                for (int k = 0; k < kept.taps(); ++k) {
                    image.push_back(static_cast<float>(kept.coefficient(p, k)));
                }
            }
            CHECK(fnv_float_image(image) == pin.hash);
        }
    }
}

TEST_CASE(
    "the converter's float table built by the compiler is what the design makes at run time",
    "[ac4core][dsp][src]") {
    // The compiler evaluates the design with no library, and this machine runs it: the same
    // function in IEEE double arithmetic, so the same bits. The ratios here are small, for the
    // compiler's step limit in a test; the decoder's own three are in the test above.
    constexpr dsp::HalfTable<3, 2> kThreeHalves = dsp::design_half_table<3, 2>();
    constexpr dsp::HalfTable<5, 4> kFiveQuarters = dsp::design_half_table<5, 4>();
    static_assert(kThreeHalves.kPhases == 2);
    static_assert(kFiveQuarters.kPhases == 3);
    static_assert(kThreeHalves.kTaps == 94);
    const auto check = [](int up, int down, std::span<const float> compiled) {
        CAPTURE(up, down);
        int taps = 0;
        const std::vector<float> run_time = portable_float_table(up, down, taps);
        const auto width = static_cast<std::size_t>(taps);
        REQUIRE(compiled.size() == static_cast<std::size_t>(up / 2 + 1) * width);
        int differ = 0;
        for (std::size_t i = 0; i < compiled.size(); ++i) {
            // The first phases of the run-time table, written out in full, are the compiler's.
            if (std::bit_cast<std::uint32_t>(run_time[i]) !=
                std::bit_cast<std::uint32_t>(compiled[i])) {
                ++differ;
            }
        }
        CHECK(differ == 0);
    };
    check(3, 2, kThreeHalves.coefficients);
    check(5, 4, kFiveQuarters.coefficients);
    // And the variable template is that table.
    CHECK(dsp::kHalfTable<3, 2>.coefficients == kThreeHalves.coefficients);
}

TEST_CASE("phase up - p of the design is phase p read from its last coefficient to its first",
          "[ac4core][dsp][src]") {
    // The window and the sinc are even, so tap k of one is, in the other, tap taps - 1 - k, and a
    // table can keep half its phases. The double design, whose phases are all computed, shows it to
    // the rounding of their arguments.
    for (const Rate& rate : {kRates[1], kRates[2], kRates[0]}) {
        CAPTURE(rate.index);
        const dsp::ResamplerFilter design(rate.up, rate.down);
        double worst = 0.0;
        for (int p = 1; p < design.up(); ++p) {
            for (int k = 0; k < design.taps(); ++k) {
                worst = std::max(worst, std::abs(design.coefficient(design.up() - p, k) -
                                                 design.coefficient(p, design.taps() - 1 - k)));
            }
        }
        CAPTURE(worst);
        CHECK(worst < 1e-13);
    }
}

namespace {

// At float a filter keeps half its phases and the others are read backwards, whether the compiler
// built the table or the filter did; at double it keeps all of them. Only a float build has a float
// filter to ask.
template <typename Scalar>
void check_how_the_filter_keeps_its_phases() {
    if constexpr (std::is_same_v<Scalar, float>) {
        struct Ratio {
            int up;
            int down;
            bool compiled;
        };
        // The decoder's three, whose tables the compiler built; the others the filter designs when
        // it is made, the last with an even number of phases.
        constexpr std::array<Ratio, 6> kRatios{{{25, 24, true},
                                                {15, 16, true},
                                                {1001, 960, true},
                                                {3, 2, false},
                                                {5, 4, false},
                                                {4, 3, false}}};
        for (const Ratio& ratio : kRatios) {
            CAPTURE(ratio.up, ratio.down, ratio.compiled);
            const dsp::BasicResamplerFilter<float> filter(ratio.up, ratio.down);
            REQUIRE(filter.up() == ratio.up);
            REQUIRE(filter.taps() > 1);
            // Phase 0 and the first half are kept as they are, the rest as the mirror of another.
            CHECK(!filter.phase(0).reversed);
            for (int p = 1; p < ratio.up; ++p) {
                const auto here = filter.phase(p);
                const auto there = filter.phase(ratio.up - p);
                REQUIRE(here.coefficients != nullptr);
                CHECK(here.reversed == (p > ratio.up / 2));
                if (here.reversed) {
                    // Phase p is phase up - p read backwards.
                    CHECK(here.coefficients == there.coefficients);
                    CHECK(!there.reversed);
                }
                for (int k = 0; k < filter.taps(); ++k) {
                    const float a = filter.coefficient(ratio.up - p, k);
                    const float b = filter.coefficient(p, filter.taps() - 1 - k);
                    REQUIRE(std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b));
                }
            }
            // Out of range.
            CHECK(filter.phase(-1).coefficients == nullptr);
            CHECK(filter.phase(ratio.up).coefficients == nullptr);
            CHECK(filter.coefficient(0, -1) == 0.0F);
            CHECK(filter.coefficient(0, filter.taps()) == 0.0F);
            CHECK(filter.coefficient(ratio.up, 0) == 0.0F);
            // A phase sums to 1, to the precision of float.
            for (int p = 0; p < ratio.up; p += std::max(1, ratio.up / 13)) {
                double sum = 0.0;
                for (int k = 0; k < filter.taps(); ++k) {
                    sum += static_cast<double>(filter.coefficient(p, k));
                }
                CHECK(std::abs(sum - 1.0) <
                      100.0 * static_cast<double>(std::numeric_limits<float>::epsilon()));
            }
        }
        // The filter's own design of a ratio is the compiler's of the same ratio: both are
        // dsp::design_half_phases, so a filter of 3/2 has the table the compiler makes of it.
        const dsp::BasicResamplerFilter<float> own(3, 2);
        const dsp::HalfTable<3, 2>& compiled = dsp::kHalfTable<3, 2>;
        for (int p = 0; p < 2; ++p) {
            for (int k = 0; k < own.taps(); ++k) {
                REQUIRE(std::bit_cast<std::uint32_t>(own.coefficient(p, k)) ==
                        std::bit_cast<std::uint32_t>(
                            compiled.coefficients[static_cast<std::size_t>(p * own.taps() + k)]));
            }
        }
    } else if constexpr (dsp::kFixed<Scalar>) {
        // A Fixed32 filter keeps half its phases, in Q1.30, as a float one does; a table of up to
        // kResamplerCopyLimit bytes is copied and a longer one read where the compiler put it.
        for (const auto [up, down] : {std::pair{25, 24}, std::pair{15, 16}, std::pair{1001, 960}}) {
            CAPTURE(up, down);
            const dsp::BasicResamplerFilter<Scalar> filter(up, down);
            CHECK(!filter.phase(0).reversed);
            for (int p = 1; p < up; ++p) {
                const auto here = filter.phase(p);
                CHECK(here.reversed == (p > up / 2));
                if (here.reversed) {
                    CHECK(here.coefficients == filter.phase(up - p).coefficients);
                }
            }
            // Read in place, two filters of the ratio read the same constants; copied, each its own.
            const dsp::BasicResamplerFilter<Scalar> other(up, down);
            CHECK((filter.phase(0).coefficients == other.phase(0).coefficients) == (up == 1001));
        }
        CHECK(sizeof(dsp::HalfTableQ30<25, 24>::coefficients) <= dsp::kResamplerCopyLimit);
        CHECK(sizeof(dsp::HalfTableQ30<15, 16>::coefficients) <= dsp::kResamplerCopyLimit);
        CHECK(sizeof(dsp::HalfTableQ30<1001, 960>::coefficients) > dsp::kResamplerCopyLimit);
    } else {
        // A double filter keeps every phase as it is.
        const dsp::BasicResamplerFilter<Scalar> filter(1001, 960);
        for (int p = 0; p < filter.up(); ++p) {
            CHECK(!filter.phase(p).reversed);
        }
    }
}

}  // namespace

TEST_CASE("a filter at float keeps half its phases and reads the others backwards",
          "[ac4core][dsp][src]") {
    check_how_the_filter_keeps_its_phases<Real>();
}

TEST_CASE("the converter's float dot product is the sum of four lanes added in the order it states",
          "[ac4core][dsp][src]") {
    std::uint32_t state = 7U;
    const auto next = [&state] {
        state = state * 1664525U + 1013904223U;
        return static_cast<float>(static_cast<double>(state >> 8U) / 16777216.0 - 0.5);
    };
    constexpr std::array<std::size_t, 13> kCounts{0, 1, 2, 3, 4, 5, 7, 8, 9, 94, 100, 101, 1001};
    for (const std::size_t n : kCounts) {
        CAPTURE(n);
        std::vector<float> c(n);
        std::vector<float> x(n);
        for (std::size_t i = 0; i < n; ++i) {
            c[i] = next();
            x[i] = next() * 30000.0F;
        }
        // Written out: lane j takes taps j, j + 4, j + 8 and so on, a product and an add at a time;
        // the lanes are added as (0 + 1) + (2 + 3); the taps left over are added in order.
        std::array<float, 4> lane{};
        std::size_t k = 0;
        for (; k + 4 <= n; k += 4) {
            for (std::size_t j = 0; j < 4; ++j) {
                lane[j] += c[k + j] * x[k + j];
            }
        }
        float want = (lane[0] + lane[1]) + (lane[2] + lane[3]);
        for (; k < n; ++k) {
            want += c[k] * x[k];
        }
        const float got = dsp::dot_four_lanes(c.data(), x.data(), n);
        CHECK(std::bit_cast<std::uint32_t>(got) == std::bit_cast<std::uint32_t>(want));
    }
}

TEST_CASE("the reversed dot product is the four lane sum of the phase written out backwards",
          "[ac4core][dsp][src]") {
    std::uint32_t state = 11U;
    const auto next = [&state] {
        state = state * 1664525U + 1013904223U;
        return static_cast<float>(static_cast<double>(state >> 8U) / 16777216.0 - 0.5);
    };
    constexpr std::array<std::size_t, 13> kCounts{0, 1, 2, 3, 4, 5, 7, 8, 9, 94, 100, 101, 1001};
    for (const std::size_t n : kCounts) {
        CAPTURE(n);
        std::vector<float> c(n);
        std::vector<float> x(n);
        for (std::size_t i = 0; i < n; ++i) {
            c[i] = next();
            x[i] = next() * 30000.0F;
        }
        // The phase as the filter's caller would have it in full: tap k is c[n - 1 - k].
        const std::vector<float> backwards(c.rbegin(), c.rend());
        const float want = dsp::dot_four_lanes(backwards.data(), x.data(), n);
        const float got = dsp::dot_four_lanes_reversed(c.data(), x.data(), n);
        CHECK(std::bit_cast<std::uint32_t>(got) == std::bit_cast<std::uint32_t>(want));
    }
}

TEST_CASE("the converter at the decoder's scalar follows the double converter to that scalar",
          "[ac4core][dsp][src]") {
    // At Fixed32 the converter runs in the time domain, kTimeShift below the double decoder's
    // (dsp/scalar_traits.hpp), where a raw unit is 2^-24: the signal goes in and comes out
    // through `scale`, and the bound is that unit against the peak.
    const double epsilon = dsp::kFixed<Real> ? 0x1p-24 : static_cast<double>(std::numeric_limits<Real>::epsilon());
    const double scale = std::ldexp(1.0, dsp::kTimeShift<Real>);
    for (const Rate& rate : {kRates[1], kRates[2], kRates[0]}) {
        CAPTURE(rate.index);
        const auto design = std::make_shared<const dsp::ResamplerFilter>(rate.up, rate.down);
        const auto kept = std::make_shared<const RealFilter>(rate.up, rate.down);
        dsp::Resampler<double> reference(design);
        dsp::Resampler<Real> converter(kept);
        // A broadband signal at the QMF domain's full scale, 2^15: two tones and noise,
        // handed to both as the same numbers.
        std::uint32_t state = 20260930U;
        double peak = 0.0;
        double worst = 0.0;
        std::size_t count = 0;
        std::size_t n = 0;
        for (int t = 0; t < 6; ++t) {
            std::vector<Real> in(static_cast<std::size_t>(rate.frame));
            std::vector<double> in_double(in.size());
            for (std::size_t i = 0; i < in.size(); ++i, ++n) {
                state = state * 1664525U + 1013904223U;
                const double noise = (static_cast<double>(state >> 8U) / 16777216.0 - 0.5) * 6000.0;
                const double value = 16000.0 * std::sin(0.11 * static_cast<double>(n)) +
                                     9000.0 * std::cos(1.7 * static_cast<double>(n)) + noise;
                in[i] = static_cast<Real>(value * scale);
                in_double[i] = static_cast<double>(in[i]) / scale;
            }
            std::vector<double> want;
            std::vector<Real> got;
            reference.process(in_double, want);
            converter.process(in, got);
            REQUIRE(got.size() == want.size());
            for (std::size_t i = 0; i < want.size(); ++i) {
                peak = std::max(peak, std::abs(want[i]));
                worst = std::max(worst, std::abs(want[i] - static_cast<double>(got[i]) / scale));
            }
            count += want.size();
        }
        CAPTURE(peak, worst, count);
        CHECK(count > 10'000);
        CHECK(worst <= 32.0 * epsilon * peak);
    }
}

TEST_CASE("a converter at a ratio of 1 copies its input without delay", "[ac4core][dsp][src]") {
    const auto filter = std::make_shared<const dsp::ResamplerFilter>(48000, 48000);
    CHECK(filter->up() == 1);
    CHECK(filter->down() == 1);
    CHECK(filter->delay() == 0.0);
    dsp::Resampler<double> resampler(filter);
    const std::vector<double> in = {0.5, -0.25, 1.0, 0.0, -1.0};
    std::vector<double> out;
    resampler.process(in, out);
    CHECK(out == in);
    CHECK(resampler.outputs_for(7) == 7);
}
