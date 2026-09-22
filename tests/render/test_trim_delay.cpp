// Per-output trim and delay (ac3/render/trim_delay.hpp).
//
// A delay line is where an off-by-one hides: a sample one early or one late
// sounds the same and moves a speaker 7 mm. So the delays are checked sample
// for sample, across block boundaries and delays longer than a block, and a
// change of delay is checked to drop what the old one held.

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

#include "ac3/render/trim_delay.hpp"

namespace {

using ac3::render::TrimDelay;
using Catch::Approx;

// A ramp 1, 2, 3, ... per output, fed through in blocks of `block`, and what
// came out, concatenated.
std::vector<std::vector<float>> run_ramp(TrimDelay& stage, std::size_t outputs,
                                         std::size_t total, std::size_t block) {
    std::vector<std::vector<float>> result(outputs);
    std::vector<std::vector<float>> planes(outputs, std::vector<float>(block));
    std::vector<std::span<float>> spans(outputs);
    std::size_t next = 1;
    for (std::size_t done = 0; done < total; done += block) {
        const std::size_t n = std::min(block, total - done);
        for (std::size_t o = 0; o < outputs; ++o) {
            for (std::size_t k = 0; k < n; ++k) {
                planes[o][k] = static_cast<float>(next + k);
            }
            spans[o] = std::span<float>(planes[o].data(), n);
        }
        next += n;
        stage.process(spans);
        for (std::size_t o = 0; o < outputs; ++o) {
            const auto end = planes[o].begin() + static_cast<std::ptrdiff_t>(n);
            result[o].insert(result[o].end(), planes[o].begin(), end);
        }
    }
    return result;
}

}  // namespace

TEST_CASE("trim and delay are configured over the caller's storage", "[render][trim-delay]") {
    REQUIRE(TrimDelay::storage_floats(16, 960) == 15360);
    REQUIRE(TrimDelay::samples_for_ms(20.0, 48000) == 960);
    REQUIRE(TrimDelay::samples_for_ms(1.0, 44100) == 44);    // 44.1 rounds down
    REQUIRE(TrimDelay::samples_for_ms(0.99, 48000) == 48);   // 47.52 rounds up
    REQUIRE(TrimDelay::samples_for_ms(-3.0, 48000) == 0);
    REQUIRE(TrimDelay::samples_for_ms(std::numeric_limits<double>::quiet_NaN(), 48000) == 0);
    REQUIRE(TrimDelay::samples_for_ms(std::numeric_limits<double>::infinity(), 48000) == 0);

    std::vector<float> storage(TrimDelay::storage_floats(4, 100), 7.0F);
    TrimDelay stage;
    REQUIRE(stage.outputs() == 0);
    REQUIRE_FALSE(stage.configure(std::span<float>(storage.data(), 399), 4, 100));
    REQUIRE(stage.outputs() == 0);
    REQUIRE_FALSE(stage.configure(storage, TrimDelay::kMaxOutputs + 1, 0));
    REQUIRE(stage.configure(storage, 4, 100));
    REQUIRE(stage.outputs() == 4);
    REQUIRE(stage.max_delay() == 100);
    REQUIRE(storage[123] == 0.0F);  // the lines start silent
    REQUIRE_FALSE(stage.active());

    // No delay storage at all is a trim-only stage.
    TrimDelay trims_only;
    REQUIRE(trims_only.configure({}, 8, 0));
    REQUIRE_FALSE(trims_only.set_delay(0, 1));
    REQUIRE(trims_only.set_delay(0, 0));
    REQUIRE(trims_only.set_trim_db(7, -3.0));
}

TEST_CASE("a trim is a gain in dB, inside its range", "[render][trim-delay]") {
    std::vector<float> storage;
    TrimDelay stage;
    REQUIRE(stage.configure(storage, 3, 0));
    REQUIRE(stage.set_trim_db(0, 6.0206));
    REQUIRE(stage.set_trim_db(1, -12.0412));
    REQUIRE(stage.trim_db(0) == Approx(6.0206));
    REQUIRE(stage.active());

    std::array<float, 4> a = {0.25F, -0.25F, 0.5F, 0.0F};
    std::array<float, 4> b = {1.0F, 1.0F, 1.0F, 1.0F};
    std::array<float, 4> c = {0.3F, 0.3F, 0.3F, 0.3F};
    const std::array<std::span<float>, 3> spans = {a, b, c};
    stage.process(spans);
    REQUIRE(a[0] == Approx(0.5F).epsilon(1e-4));
    REQUIRE(a[1] == Approx(-0.5F).epsilon(1e-4));
    REQUIRE(b[2] == Approx(0.25F).epsilon(1e-4));
    REQUIRE(c[3] == 0.3F);  // 0 dB is exactly unity

    REQUIRE_FALSE(stage.set_trim_db(0, TrimDelay::kMaxTrimDb + 0.1));
    REQUIRE_FALSE(stage.set_trim_db(0, TrimDelay::kMinTrimDb - 0.1));
    REQUIRE_FALSE(stage.set_trim_db(0, std::numeric_limits<double>::quiet_NaN()));
    REQUIRE_FALSE(stage.set_trim_db(3, 0.0));
    REQUIRE(stage.trim_db(0) == Approx(6.0206));  // a refusal changes nothing
    REQUIRE(stage.set_trim_db(0, TrimDelay::kMaxTrimDb));
    REQUIRE(stage.set_trim_db(0, TrimDelay::kMinTrimDb));
}

TEST_CASE("a delay shifts an output by exactly its samples, across blocks",
          "[render][trim-delay]") {
    // Delays shorter than, equal to and longer than the 256-sample block, and
    // one output left undelayed beside them.
    const std::array<std::size_t, 5> delays = {0, 1, 255, 256, 700};
    std::vector<float> storage(TrimDelay::storage_floats(delays.size(), 960));
    TrimDelay stage;
    REQUIRE(stage.configure(storage, delays.size(), 960));
    for (std::size_t o = 0; o < delays.size(); ++o) {
        REQUIRE(stage.set_delay(o, delays[o]));
        REQUIRE(stage.delay(o) == delays[o]);
    }
    constexpr std::size_t kTotal = 256 * 8;
    for (const std::size_t block : {std::size_t{256}, std::size_t{100}, std::size_t{1}}) {
        CAPTURE(block);
        stage.reset();
        const auto result = run_ramp(stage, delays.size(), kTotal, block);
        for (std::size_t o = 0; o < delays.size(); ++o) {
            CAPTURE(o);
            const std::size_t d = delays[o];
            for (std::size_t k = 0; k < kTotal; ++k) {
                // Input sample k (0-based) is the value k + 1.
                const float expected = k < d ? 0.0F : static_cast<float>(k - d + 1);
                if (result[o][k] != expected) {
                    CAPTURE(k, result[o][k], expected);
                    FAIL("delayed output is off");
                }
            }
        }
    }
    REQUIRE_FALSE(stage.set_delay(0, 961));
    REQUIRE_FALSE(stage.set_delay(5, 1));
}

TEST_CASE("changing a delay drops what the old one held", "[render][trim-delay]") {
    std::vector<float> storage(TrimDelay::storage_floats(1, 64));
    TrimDelay stage;
    REQUIRE(stage.configure(storage, 1, 64));
    REQUIRE(stage.set_delay(0, 48));
    std::array<float, 32> block{};
    block.fill(1.0F);
    const std::array<std::span<float>, 1> spans = {block};
    stage.process(spans);  // 32 ones go into the line

    // Shorter: nothing of the old line comes out, just the new delay's silence.
    REQUIRE(stage.set_delay(0, 16));
    block.fill(2.0F);
    stage.process(spans);
    for (std::size_t k = 0; k < 16; ++k) {
        REQUIRE(block[k] == 0.0F);
    }
    for (std::size_t k = 16; k < 32; ++k) {
        REQUIRE(block[k] == 2.0F);
    }

    // Setting the same delay again is not a change, and keeps the line.
    REQUIRE(stage.set_delay(0, 16));
    block.fill(3.0F);
    stage.process(spans);
    for (std::size_t k = 0; k < 16; ++k) {
        REQUIRE(block[k] == 2.0F);
    }

    // reset() empties the lines and keeps the settings.
    stage.reset();
    REQUIRE(stage.delay(0) == 16);
    block.fill(4.0F);
    stage.process(spans);
    REQUIRE(block[0] == 0.0F);
    REQUIRE(block[16] == 4.0F);
}

TEST_CASE("trim applies after the delay, and outputs stay independent", "[render][trim-delay]") {
    std::vector<float> storage(TrimDelay::storage_floats(2, 8));
    TrimDelay stage;
    REQUIRE(stage.configure(storage, 2, 8));
    REQUIRE(stage.set_delay(0, 2));
    REQUIRE(stage.set_trim_db(0, -6.0206));
    std::array<float, 4> first = {1.0F, 1.0F, 1.0F, 1.0F};
    std::array<float, 4> second = {1.0F, 1.0F, 1.0F, 1.0F};
    const std::array<std::span<float>, 2> spans = {first, second};
    stage.process(spans);
    REQUIRE(first[0] == 0.0F);
    REQUIRE(first[1] == 0.0F);
    REQUIRE(first[2] == Approx(0.5F).epsilon(1e-4));
    REQUIRE(second == std::array<float, 4>{1.0F, 1.0F, 1.0F, 1.0F});

    // Fewer spans than outputs: the outputs given are processed and the
    // missing one is skipped, not read.
    std::array<float, 4> only = {2.0F, 2.0F, 2.0F, 2.0F};
    const std::array<std::span<float>, 1> one = {only};
    stage.process(one);
    REQUIRE(only[0] == Approx(0.5F).epsilon(1e-4));  // output 0's line: the last block's ones
    REQUIRE(only[2] == Approx(1.0F).epsilon(1e-4));  // then this block's twos, trimmed
}
