// The AC-4 sample rate converter (src/ac4core/src/dsp/resampler.hpp): the
// output sample count of every frame rate of ETSI TS 103 190-1 V1.4.1 Table 83,
// the sequence ETSI TS 103 190-2 V1.3.1 Table 47 locks to sequence_counter, and
// the filter's passband, stopband and delay, measured with tones, in the
// decoder's direction and the encoder's.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <numbers>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "dsp/resampler.hpp"

namespace {

namespace dsp = ac4::detail::dsp;

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
