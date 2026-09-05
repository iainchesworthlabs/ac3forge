#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <memory>
#include <vector>

#include "fake_devices.hpp"
#include "tap_pool.hpp"

// The tap pool over fake taps: it opens one tap per wanted application at
// the pool's width, closes the ones that left, reports the ones that could
// not open, reads the same number of frames from each, and reads a stalled
// tap as silence rather than holding the frame.

using ac3::crucible::AppId;
using ac3::crucible::TapPool;
using ac3::crucible::testing::FakeDevices;

namespace {

float rms(std::span<const float> samples) {
    double sum = 0.0;
    for (const float v : samples) {
        sum += static_cast<double>(v) * static_cast<double>(v);
    }
    return samples.empty() ? 0.0F : static_cast<float>(std::sqrt(sum / static_cast<double>(samples.size())));
}

}  // namespace

TEST_CASE("tap pool opens one tap per application at the pool's width", "[crucible][tap_pool]") {
    auto devices = std::make_shared<FakeDevices>();
    TapPool pool(devices, 8, 48000);
    const std::vector<AppId> wanted{101, 202};
    const auto failed = pool.sync(wanted);
    CHECK(failed.empty());
    CHECK(pool.size() == 2);
    CHECK(pool.has(101));
    CHECK(pool.has(202));
    CHECK_FALSE(pool.has(303));
    REQUIRE(devices->taps.size() == 2);
    for (const auto& tap : devices->taps) {
        CHECK(tap->started);
        CHECK(tap->channels == 8);
        CHECK(tap->sample_rate == 48000);
    }
    CHECK(pool.channels() == 8);
}

TEST_CASE("tap pool closes taps whose application left and keeps the rest", "[crucible][tap_pool]") {
    auto devices = std::make_shared<FakeDevices>();
    TapPool pool(devices, 2);
    std::ignore = pool.sync(std::vector<AppId>{1, 2, 3});
    std::ignore = pool.sync(std::vector<AppId>{2});
    CHECK(pool.size() == 1);
    CHECK(pool.has(2));
    REQUIRE(devices->taps.size() == 3);
    CHECK(devices->taps[0]->stopped);
    CHECK_FALSE(devices->taps[1]->stopped);
    CHECK(devices->taps[2]->stopped);
    // A second sync with the same list opens nothing new.
    std::ignore = pool.sync(std::vector<AppId>{2});
    CHECK(devices->taps.size() == 3);
}

TEST_CASE("tap pool reports the applications whose tap refused to open", "[crucible][tap_pool]") {
    auto devices = std::make_shared<FakeDevices>();
    TapPool pool(devices, 2);
    devices->refuse_next_start = true;
    const auto failed = pool.sync(std::vector<AppId>{7});
    REQUIRE(failed.size() == 1);
    CHECK(failed[0] == 7);
    CHECK(pool.size() == 0);
    // The next attempt is a fresh tap, and succeeds.
    CHECK(pool.sync(std::vector<AppId>{7}).empty());
    CHECK(pool.has(7));
}

TEST_CASE("tap pool reads the same frame count from every tap, as signal", "[crucible][tap_pool]") {
    auto devices = std::make_shared<FakeDevices>();
    TapPool pool(devices, 2);
    std::ignore = pool.sync(std::vector<AppId>{11, 22});
    const auto& reads = pool.read(1536, 5);
    REQUIRE(reads.size() == 2);
    for (const auto& read : reads) {
        CHECK(read.interleaved.size() == 1536 * 2);
        CHECK_FALSE(read.starved);
        // A full-scale sine has an RMS of 1/sqrt(2).
        CHECK_THAT(rms(read.interleaved), Catch::Matchers::WithinAbs(0.7071, 0.02));
    }
    // A second read continues where the first stopped.
    const auto& again = pool.read(1536, 5);
    REQUIRE(again.size() == 2);
    CHECK(devices->taps[0]->samples_read == 2 * 1536 * 2);
}

TEST_CASE("tap pool reports the deepest backlog and flush discards it", "[crucible][tap_pool]") {
    auto devices = std::make_shared<FakeDevices>();
    TapPool pool(devices, 2);
    std::ignore = pool.sync(std::vector<AppId>{1, 2});
    devices->taps[0]->backlog = 2 * 4800;   // 100 ms of stereo
    devices->taps[1]->backlog = 2 * 480;    // 10 ms
    CHECK(pool.backlog_frames() == 4800);
    pool.flush();
    CHECK(pool.backlog_frames() == 0);
    CHECK(devices->taps[0]->samples_read >= 2 * 4800);
    // Reading afterwards still delivers signal.
    const auto& reads = pool.read(256, 3);
    REQUIRE(reads.size() == 2);
    CHECK_FALSE(reads[0].starved);
}

TEST_CASE("tap pool reads a stalled tap as silence and says so", "[crucible][tap_pool]") {
    auto devices = std::make_shared<FakeDevices>();
    TapPool pool(devices, 2);
    std::ignore = pool.sync(std::vector<AppId>{5});
    devices->taps[0]->starve = true;
    const auto& reads = pool.read(256, 3);
    REQUIRE(reads.size() == 1);
    CHECK(reads[0].starved);
    CHECK(reads[0].interleaved.size() == 512);
    CHECK(rms(reads[0].interleaved) == 0.0F);
    // Back to life: the next read is signal again.
    devices->taps[0]->starve = false;
    const auto& after = pool.read(256, 3);
    CHECK_FALSE(after[0].starved);
    CHECK(rms(after[0].interleaved) > 0.5F);
}
