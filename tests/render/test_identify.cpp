// The identify tone (ac3/render/identify.hpp): pink noise at a stated RMS
// level on one output at a time, with a low band for LFE and subwoofer feeds.
//
// The generator is deterministic from reset(), so the levels below are the
// generator's own and cannot drift from run to run.

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "ac3/render/float_biquad.hpp"
#include "ac3/render/identify.hpp"

namespace {

using ac3::render::FloatBiquad;
using ac3::render::IdentifyTone;
using Band = IdentifyTone::Band;
using Catch::Approx;

std::vector<float> tone(IdentifyTone& generator, std::size_t samples, Band band) {
    std::vector<float> out(samples);
    // In blocks, as a player asks for it.
    for (std::size_t done = 0; done < samples; done += 256) {
        const std::size_t n = std::min<std::size_t>(256, samples - done);
        generator.generate(std::span<float>(out.data() + done, n), band);
    }
    return out;
}

double rms_db(std::span<const float> samples) {
    double sum = 0.0;
    for (const float s : samples) {
        sum += static_cast<double>(s) * static_cast<double>(s);
    }
    return 10.0 * std::log10(sum / static_cast<double>(samples.size()));
}

// What is left above `corner_hz` after two cascaded high-passes, in dB
// relative to the whole signal.
double energy_above_db(std::span<const float> samples, double corner_hz, double rate_hz) {
    FloatBiquad first = FloatBiquad::highpass(corner_hz, rate_hz);
    FloatBiquad second = FloatBiquad::highpass(corner_hz, rate_hz);
    std::vector<float> filtered(samples.size());
    for (std::size_t k = 0; k < samples.size(); ++k) {
        filtered[k] = second.process(first.process(samples[k]));
    }
    return rms_db(filtered) - rms_db(samples);
}

}  // namespace

TEST_CASE("the identify tone is the same sequence from every reset", "[render][identify]") {
    IdentifyTone a;
    IdentifyTone b;
    const auto first = tone(a, 4096, Band::kFull);
    REQUIRE(tone(b, 4096, Band::kFull) == first);
    a.reset();
    REQUIRE(tone(a, 4096, Band::kFull) == first);
    // And it is noise, not silence or a constant.
    REQUIRE(first[0] != first[1]);
    REQUIRE(rms_db(first) > -40.0);
}

TEST_CASE("the identify tone plays at its stated level in both bands", "[render][identify]") {
    for (const std::uint32_t rate : {44100U, 48000U, 96000U}) {
        CAPTURE(rate);
        const std::size_t ten_seconds = std::size_t{rate} * 10;

        IdentifyTone full(rate);
        REQUIRE(full.level_db() == IdentifyTone::kDefaultLevelDb);
        REQUIRE(rms_db(tone(full, ten_seconds, Band::kFull)) == Approx(-20.0).margin(0.5));

        IdentifyTone low(rate);
        REQUIRE(rms_db(tone(low, ten_seconds, Band::kLow)) == Approx(-20.0).margin(0.5));

        REQUIRE(full.set_level_db(-35.0));
        full.reset();
        REQUIRE(rms_db(tone(full, ten_seconds, Band::kFull)) == Approx(-35.0).margin(0.5));
    }
}

TEST_CASE("the low band keeps a subwoofer's range and the full band does not",
          "[render][identify]") {
    constexpr std::uint32_t kRate = 48000;
    IdentifyTone generator(kRate);
    const auto low = tone(generator, kRate * 10, Band::kLow);
    const auto full = tone(generator, kRate * 10, Band::kFull);
    // Above 400 Hz the low band is more than 25 dB down; the full band has
    // over a third of its energy there.
    REQUIRE(energy_above_db(low, 400.0, kRate) < -25.0);
    REQUIRE(energy_above_db(full, 400.0, kRate) > -10.0);
    // And below 20 Hz the low band's high-pass has taken the rumble out.
    FloatBiquad lowpass_a = FloatBiquad::lowpass(15.0, kRate);
    FloatBiquad lowpass_b = FloatBiquad::lowpass(15.0, kRate);
    std::vector<float> infrasonic(low.size());
    for (std::size_t k = 0; k < low.size(); ++k) {
        infrasonic[k] = lowpass_b.process(lowpass_a.process(low[k]));
    }
    REQUIRE(rms_db(infrasonic) - rms_db(low) < -12.0);
}

TEST_CASE("fill puts the tone on one output and silence on the rest", "[render][identify]") {
    IdentifyTone generator;
    std::vector<std::vector<float>> planes(4, std::vector<float>(256, 99.0F));
    std::vector<std::span<float>> spans;
    for (auto& plane : planes) {
        spans.emplace_back(plane);
    }
    generator.fill(spans, 2);
    REQUIRE(planes[0] == std::vector<float>(256, 0.0F));
    REQUIRE(planes[1] == std::vector<float>(256, 0.0F));
    REQUIRE(planes[3] == std::vector<float>(256, 0.0F));
    REQUIRE(rms_db(planes[2]) > -40.0);

    // Past the last output: every output silent.
    generator.fill(spans, 4, Band::kLow);
    for (const auto& plane : planes) {
        REQUIRE(plane == std::vector<float>(256, 0.0F));
    }
}

TEST_CASE("the identify tone's level is refused outside its range", "[render][identify]") {
    IdentifyTone generator;
    REQUIRE(generator.set_level_db(IdentifyTone::kMinLevelDb));
    REQUIRE(generator.set_level_db(IdentifyTone::kMaxLevelDb));
    REQUIRE_FALSE(generator.set_level_db(IdentifyTone::kMaxLevelDb + 1.0));
    REQUIRE_FALSE(generator.set_level_db(IdentifyTone::kMinLevelDb - 1.0));
    REQUIRE_FALSE(generator.set_level_db(std::numeric_limits<double>::quiet_NaN()));
    REQUIRE(generator.level_db() == IdentifyTone::kMaxLevelDb);
}
