#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

#include "ac3/render/layout.hpp"
#include "play_meters.hpp"

// ac3::hearth::PlayMeters (apps/hearth/engine/play_meters.cpp): meters that
// measure the output as it is rendered and hand a reading out only when the
// device's clock has reached the audio it describes.

namespace {

using ac3::hearth::MeterSnapshot;
using ac3::hearth::PlayMeters;

ac3::render::OutputLayout layout(const char* name) {
    const auto parsed = ac3::render::OutputLayout::parse(name);
    REQUIRE(parsed.has_value());
    return *parsed;
}

// Feeds `frames` of a sine at `amplitude` to every slot, in 256-frame blocks,
// starting at output frame `start`. Returns the output frame after the last.
std::uint64_t feed(PlayMeters& meters, std::size_t slots, double amplitude, std::uint64_t frames,
                   std::uint64_t start) {
    constexpr std::size_t kBlock = 256;
    std::vector<float> block(kBlock);
    std::vector<std::span<const float>> views(slots, std::span<const float>(block));
    std::uint64_t at = start;
    for (std::uint64_t done = 0; done < frames; done += kBlock) {
        for (std::size_t n = 0; n < kBlock; ++n) {
            block[n] = static_cast<float>(
                amplitude * std::sin(2.0 * std::numbers::pi * 997.0 *
                                     static_cast<double>(at + n) / 48000.0));
        }
        at += kBlock;
        meters.meter(views, kBlock, at);
    }
    return at;
}

}  // namespace

TEST_CASE("play meters: a reading comes out only once the clock has reached its audio",
          "[hearth][play-meters]") {
    PlayMeters meters{layout("2.0"), 48000, 2400};
    const std::uint64_t end = feed(meters, 2, 0.5, 2560, 0);
    REQUIRE(end == 2560);
    // One interval's worth, stamped with the end of the block that finished it.
    CHECK(meters.queued() == 1);

    MeterSnapshot latest;
    CHECK_FALSE(meters.release(2559, latest));
    REQUIRE(meters.release(2560, latest));
    CHECK(latest.output_frame == 2560);
    CHECK(latest.levels.size() == 2);
    CHECK(meters.queued() == 0);
    // Released once only.
    CHECK_FALSE(meters.release(1'000'000, latest));
}

TEST_CASE("play meters: the levels and the loudness describe the audio", "[hearth][play-meters]") {
    PlayMeters meters{layout("2.0"), 48000, 2400};
    // A second of a sine at half scale: -6 dBFS peak, -9 dBFS RMS.
    const std::uint64_t end = feed(meters, 2, 0.5, 48000, 0);
    MeterSnapshot latest;
    REQUIRE(meters.release(end, latest));
    REQUIRE(latest.levels.size() == 2);
    for (const auto& level : latest.levels) {
        CHECK(level.hold_db == Catch::Approx(-6.02).margin(0.1));
        CHECK(level.rms_db == Catch::Approx(-9.03).margin(0.2));
        CHECK_FALSE(level.clipped);
    }
    REQUIRE(latest.momentary_lkfs.has_value());
    REQUIRE(latest.integrated_lkfs.has_value());
    REQUIRE(latest.true_peak_dbtp.has_value());
    CHECK(*latest.true_peak_dbtp == Catch::Approx(-6.02).margin(0.2));
    // BS.1770's calibration reads a full-scale 997 Hz sine in one channel as
    // -3.01 LKFS; two channels of one at half scale sum to 6 dB less.
    CHECK(*latest.momentary_lkfs == Catch::Approx(-6.02).margin(0.1));
    // Three seconds have not passed.
    CHECK_FALSE(latest.short_term_lkfs.has_value());
}

TEST_CASE("play meters: a new item starts its own loudness, and the rest runs on",
          "[hearth][play-meters]") {
    PlayMeters meters{layout("2.0"), 48000, 2400};
    std::uint64_t at = feed(meters, 2, 0.5, 48000, 0);
    meters.restart_programme();
    // Less than one 400 ms block of the new item.
    at = feed(meters, 2, 0.05, 4800, at);
    MeterSnapshot latest;
    REQUIRE(meters.release(at, latest));
    CHECK_FALSE(latest.integrated_lkfs.has_value());
    CHECK_FALSE(latest.loudness_range.has_value());
    REQUIRE(latest.true_peak_dbtp.has_value());
    CHECK(*latest.true_peak_dbtp == Catch::Approx(-26.02).margin(0.2));
    // Momentary loudness runs on through the join, most of its window still
    // the loud item...
    REQUIRE(latest.momentary_lkfs.has_value());
    CHECK(*latest.momentary_lkfs > -8.0);
    // ...and the peak hold remembers it too.
    CHECK(latest.levels[0].hold_db == Catch::Approx(-6.02).margin(0.1));
}

TEST_CASE("play meters: short-term loudness reaches back past a join until the new item fills it",
          "[hearth][play-meters]") {
    PlayMeters meters{layout("2.0"), 48000, 2400};
    std::uint64_t at = feed(meters, 2, 0.5, 4 * 48000, 0);
    MeterSnapshot latest;
    REQUIRE(meters.release(at, latest));
    REQUIRE(latest.short_term_lkfs.has_value());
    CHECK_FALSE(meters.bridging());

    // A second into a quiet item, two thirds of the window are the loud one,
    // while the integrated loudness is the quiet item's alone.
    meters.restart_programme();
    CHECK(meters.bridging());
    at = feed(meters, 2, 0.05, 48000, at);
    REQUIRE(meters.release(at, latest));
    REQUIRE(latest.short_term_lkfs.has_value());
    CHECK(*latest.short_term_lkfs == Catch::Approx(-6.02 + (10.0 * std::log10(2.0 / 3.0))).margin(0.3));
    REQUIRE(latest.integrated_lkfs.has_value());
    CHECK(*latest.integrated_lkfs == Catch::Approx(-26.02).margin(0.3));

    // Once the new item's own window is full, the old meter goes.
    at = feed(meters, 2, 0.05, (2 * 48000) + 4800, at);
    CHECK_FALSE(meters.bridging());
    REQUIRE(meters.release(at, latest));
    REQUIRE(latest.short_term_lkfs.has_value());
    CHECK(*latest.short_term_lkfs == Catch::Approx(-26.02).margin(0.3));
}

TEST_CASE("play meters: joins closer together than the short-term window keep it whole",
          "[hearth][play-meters]") {
    PlayMeters meters{layout("2.0"), 48000, 2400};
    std::uint64_t at = feed(meters, 2, 0.5, 4 * 48000, 0);
    meters.restart_programme();
    at = feed(meters, 2, 0.05, 48000, at);
    meters.restart_programme();
    at = feed(meters, 2, 0.05, 24000, at);
    CHECK(meters.bridging());
    MeterSnapshot latest;
    REQUIRE(meters.release(at, latest));
    // Half the window is still the first item, two items back.
    REQUIRE(latest.short_term_lkfs.has_value());
    CHECK(*latest.short_term_lkfs == Catch::Approx(-6.02 + (10.0 * std::log10(0.5))).margin(0.3));
    CHECK_FALSE(latest.integrated_lkfs.has_value());
}

TEST_CASE("play meters: integrated loudness and loudness range are read once a second",
          "[hearth][play-meters]") {
    // The library works both out over the whole programme at each read.
    PlayMeters meters{layout("2.0"), 48000, 2400};
    MeterSnapshot latest;
    std::uint64_t at = feed(meters, 2, 0.5, 48000, 0);
    REQUIRE(meters.release(at, latest));
    REQUIRE(latest.integrated_lkfs.has_value());
    const double first = *latest.integrated_lkfs;
    CHECK(first == Catch::Approx(-6.02).margin(0.1));

    // Half a second of something quieter, and the reading from a second in
    // still stands.
    at = feed(meters, 2, 0.25, 24000, at);
    REQUIRE(meters.release(at, latest));
    REQUIRE(latest.integrated_lkfs.has_value());
    CHECK(*latest.integrated_lkfs == first);

    // A second after that reading, the next one takes the quieter audio in.
    at = feed(meters, 2, 0.25, 30000, at);
    REQUIRE(meters.release(at, latest));
    REQUIRE(latest.integrated_lkfs.has_value());
    CHECK(*latest.integrated_lkfs < first - 0.5);
}

TEST_CASE("play meters: a flushed timeline drops what was waiting and starts every meter again",
          "[hearth][play-meters]") {
    PlayMeters meters{layout("5.1"), 48000, 2400};
    // With nothing fed yet, there is no item to start again.
    meters.restart_programme();
    CHECK_FALSE(meters.bridging());

    std::uint64_t at = feed(meters, 6, 0.5, 19200, 0);
    meters.restart_programme();
    feed(meters, 6, 0.5, 2560, at);
    REQUIRE(meters.queued() > 0);
    REQUIRE(meters.bridging());
    meters.restart_timeline();
    CHECK(meters.queued() == 0);
    CHECK_FALSE(meters.bridging());
    MeterSnapshot latest;
    CHECK_FALSE(meters.release(1'000'000, latest));

    // The new timeline counts from zero, with every meter started again.
    const std::uint64_t end = feed(meters, 6, 0.05, 2560, 0);
    REQUIRE(meters.release(end, latest));
    CHECK(latest.output_frame == 2560);
    CHECK(latest.levels[0].hold_db < -20.0);
    CHECK_FALSE(latest.momentary_lkfs.has_value());
    REQUIRE(latest.true_peak_dbtp.has_value());
    CHECK(*latest.true_peak_dbtp == Catch::Approx(-26.02).margin(0.2));
}

TEST_CASE("play meters: snapshots come out in order however many wait", "[hearth][play-meters]") {
    // A snapshot a block. Twenty in, ten out, so the ring's oldest entry is
    // part-way round when sixty more make it grow.
    PlayMeters meters{layout("2.0"), 48000, 256};
    std::uint64_t end = feed(meters, 2, 0.5, 256 * 20, 0);
    MeterSnapshot latest;
    REQUIRE(meters.release(256 * 10, latest));
    CHECK(latest.output_frame == 256 * 10);
    end = feed(meters, 2, 0.5, 256 * 60, end);
    REQUIRE(meters.queued() == 70);

    std::uint64_t previous = latest.output_frame;
    for (std::uint64_t heard = previous + 256; heard <= end; heard += 256) {
        REQUIRE(meters.release(heard, latest));
        CHECK(latest.output_frame == heard);
        CHECK(latest.output_frame == previous + 256);
        previous = latest.output_frame;
    }
    CHECK(meters.queued() == 0);
}

TEST_CASE("play meters: slots placed only by angle are metered but not weighted for loudness",
          "[hearth][play-meters]") {
    PlayMeters meters{layout("30/0,-30/0"), 48000, 2400};
    const std::uint64_t end = feed(meters, 2, 0.5, 9600, 0);
    MeterSnapshot latest;
    REQUIRE(meters.release(end, latest));
    CHECK(latest.levels.size() == 2);
    CHECK(latest.levels[0].hold_db == Catch::Approx(-6.02).margin(0.1));
    CHECK_FALSE(latest.momentary_lkfs.has_value());
    CHECK_FALSE(latest.integrated_lkfs.has_value());
}
