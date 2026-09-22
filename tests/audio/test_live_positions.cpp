#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "ac3/audio/live_positions.hpp"
#include "ac3/oba/scene.hpp"
#include "udp_socket.hpp"

// LivePositionSource's own thread and a per-test loopback UdpSocket sender:
// these tests exercise the real receiver thread, the real mutex-guarded
// mailbox, and the real merge/apply/push path together - the shape the
// Linux LLVM TSan leg watches (ctest -L concurrency), which is why every
// case here carries that tag even where a single case's own body is not
// obviously "concurrent": what ThreadSanitizer is watching is the
// cross-thread traffic LivePositionSource itself generates once started,
// not just the assertions a test happens to write.

namespace {

using Catch::Matchers::WithinAbs;

void append_osc_string(std::vector<std::byte>& out, std::string_view text) {
    for (const char c : text) {
        out.push_back(std::byte{static_cast<unsigned char>(c)});
    }
    out.push_back(std::byte{0});
    while (out.size() % 4 != 0) {
        out.push_back(std::byte{0});
    }
}

void append_f32(std::vector<std::byte>& out, float v) {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(v);
    out.push_back(std::byte{static_cast<unsigned char>((bits >> 24) & 0xFFU)});
    out.push_back(std::byte{static_cast<unsigned char>((bits >> 16) & 0xFFU)});
    out.push_back(std::byte{static_cast<unsigned char>((bits >> 8) & 0xFFU)});
    out.push_back(std::byte{static_cast<unsigned char>(bits & 0xFFU)});
}

std::vector<std::byte> osc_xyz(std::size_t object, float x, float y, float z) {
    std::vector<std::byte> out;
    append_osc_string(out, "/object/" + std::to_string(object) + "/xyz");
    append_osc_string(out, ",fff");
    append_f32(out, x);
    append_f32(out, y);
    append_f32(out, z);
    return out;
}

std::vector<std::byte> osc_gain(std::size_t object, float gain) {
    std::vector<std::byte> out;
    append_osc_string(out, "/object/" + std::to_string(object) + "/gain");
    append_osc_string(out, ",f");
    append_f32(out, gain);
    return out;
}

std::vector<std::byte> osc_release(std::size_t object) {
    std::vector<std::byte> out;
    append_osc_string(out, "/object/" + std::to_string(object) + "/release");
    append_osc_string(out, ",");
    return out;
}

ac3::oba::SceneCursor two_object_cursor() {
    auto scene = ac3::oba::ObjectScene::create({
        {.name = "a", .automation = {{.time_s = 0.0, .position = {.x = 0.1}, .gain = 0.5}}},
        {.name = "b", .automation = {{.time_s = 0.0, .position = {.x = 0.9}, .gain = 0.6}}},
    });
    REQUIRE(scene.has_value());
    return ac3::oba::SceneCursor{std::move(*scene)};
}

// Retries `predicate` (which itself drains the source) for up to `timeout` -
// loopback delivery is normally instant, but this avoids flakiness from
// scheduler jitter on a loaded CI runner instead of a single fixed sleep.
template <typename Predicate>
bool wait_for(Predicate&& predicate, std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    return predicate();
}

}  // namespace

TEST_CASE("start/stop lifecycle", "[audio][live_positions][concurrency]") {
    ac3::audio::LivePositionSource source{2};
    CHECK_FALSE(source.running());

    const auto started = source.start("127.0.0.1", 0);
    REQUIRE(started.has_value());
    CHECK(source.running());
    CHECK(source.local_port() != 0);  // port 0 asked for an ephemeral one

    source.stop();
    CHECK_FALSE(source.running());

    // Safe to call again - the destructor does exactly this unconditionally.
    source.stop();
}

TEST_CASE("starting twice on the same instance is refused", "[audio][live_positions][concurrency]") {
    ac3::audio::LivePositionSource source{1};
    REQUIRE(source.start("127.0.0.1", 0).has_value());
    const auto second = source.start("127.0.0.1", 0);
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error() == ac3::audio::PositionSourceError::kAlreadyRunning);
}

TEST_CASE("a bind address that is not a dotted-quad is refused", "[audio][live_positions][concurrency]") {
    ac3::audio::LivePositionSource source{1};
    const auto started = source.start("not-an-ip-address", 0);
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error() == ac3::audio::PositionSourceError::kBadAddress);
    CHECK_FALSE(source.running());
}

TEST_CASE("a real loopback datagram reaches the cursor", "[audio][live_positions][concurrency]") {
    ac3::audio::LivePositionSource source{2};
    REQUIRE(source.start("127.0.0.1", 0).has_value());
    const auto port = source.local_port();

    ac3::audio::UdpSocket sender;
    const auto message = osc_xyz(0, 0.9F, 0.1F, 0.5F);
    REQUIRE(sender.send_to("127.0.0.1", port, message));

    auto cursor = two_object_cursor();
    const bool reached = wait_for([&] {
        source.drain_into(cursor, 0.0);
        return cursor.is_live(0);
    });
    REQUIRE(reached);

    const auto placement = cursor.sample(0.0)[0];
    CHECK_THAT(placement.position.x, WithinAbs(0.9, 1e-6));
    CHECK_THAT(placement.position.y, WithinAbs(0.1, 1e-6));
    // Gain rode through from the authored scene - the whole point of
    // apply()'s merge (see scene_osc.hpp) - not reset to a default 1.0.
    CHECK(placement.gain == 0.5);
    CHECK_FALSE(cursor.is_live(1));  // object 1 was never addressed

    const auto stats = source.stats();
    CHECK(stats.datagrams >= 1);
    CHECK(stats.updates_applied >= 1);

    source.stop();
}

TEST_CASE("a gain-only update waits, across drains, for a position", "[audio][live_positions][concurrency]") {
    ac3::audio::LivePositionSource source{2};
    REQUIRE(source.start("127.0.0.1", 0).has_value());
    const auto port = source.local_port();

    ac3::audio::UdpSocket sender;
    REQUIRE(sender.send_to("127.0.0.1", port, osc_gain(1, 0.2F)));

    auto cursor = two_object_cursor();
    // The gain-only message has nothing to apply yet (ac3::oba::apply's own
    // contract) - draining repeatedly must never spuriously mark it live.
    for (int i = 0; i < 5; ++i) {
        source.drain_into(cursor, 0.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK_FALSE(cursor.is_live(1));

    REQUIRE(sender.send_to("127.0.0.1", port, osc_xyz(1, 0.3F, 0.4F, 0.0F)));
    const bool reached = wait_for([&] {
        source.drain_into(cursor, 0.0);
        return cursor.is_live(1);
    });
    REQUIRE(reached);

    const auto placement = cursor.sample(0.0)[1];
    CHECK_THAT(placement.position.x, WithinAbs(0.3, 1e-6));
    // The gain sent BEFORE the position finally reached the placement too -
    // the remembered-until-a-position-arrives behaviour this test is named
    // for.
    CHECK_THAT(placement.gain, WithinAbs(0.2, 1e-6));

    source.stop();
}

TEST_CASE("release hands a driven object back to its authored timeline",
          "[audio][live_positions][concurrency]") {
    ac3::audio::LivePositionSource source{2};
    REQUIRE(source.start("127.0.0.1", 0).has_value());
    const auto port = source.local_port();

    ac3::audio::UdpSocket sender;
    REQUIRE(sender.send_to("127.0.0.1", port, osc_xyz(0, 0.9F, 0.1F, 0.5F)));

    auto cursor = two_object_cursor();
    REQUIRE(wait_for([&] {
        source.drain_into(cursor, 0.0);
        return cursor.is_live(0);
    }));

    REQUIRE(sender.send_to("127.0.0.1", port, osc_release(0)));
    REQUIRE(wait_for([&] {
        source.drain_into(cursor, 0.0);
        return !cursor.is_live(0);
    }));
    CHECK(cursor.sample(0.0)[0].position.x == 0.1);  // back to authored

    source.stop();
}

TEST_CASE("a burst of concurrent sends never crashes or corrupts the mailbox",
          "[audio][live_positions][concurrency]") {
    // The shape ThreadSanitizer exists to watch: one thread pushing updates
    // as fast as it can while another drains, for long enough that a real
    // data race (not merely a possible one) would actually be scheduled.
    ac3::audio::LivePositionSource source{4};
    REQUIRE(source.start("127.0.0.1", 0).has_value());
    const auto port = source.local_port();

    std::atomic_bool keep_sending{true};
    std::thread sender_thread([&] {
        ac3::audio::UdpSocket sender;
        int n = 0;
        while (keep_sending.load(std::memory_order_relaxed)) {
            const auto x = static_cast<float>(n % 100) / 100.0F;
            std::ignore = sender.send_to(
                "127.0.0.1", port, osc_xyz(static_cast<std::size_t>(n % 4), x, x, 0.0F));
            ++n;
        }
    });

    auto cursor = two_object_cursor();
    // two_object_cursor only has 2 objects; drain against a 4-object scene
    // instead so every slot LivePositionSource was constructed with has
    // somewhere to land.
    auto scene = ac3::oba::ObjectScene::create({
        {.automation = {{.time_s = 0.0}}},
        {.automation = {{.time_s = 0.0}}},
        {.automation = {{.time_s = 0.0}}},
        {.automation = {{.time_s = 0.0}}},
    });
    REQUIRE(scene.has_value());
    ac3::oba::SceneCursor wide_cursor{std::move(*scene)};

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline) {
        source.drain_into(wide_cursor, 0.0);
    }
    keep_sending.store(false, std::memory_order_relaxed);
    sender_thread.join();
    source.drain_into(wide_cursor, 0.0);  // one last drain of whatever is still pending

    for (std::size_t i = 0; i < 4; ++i) {
        const auto placement = wide_cursor.sample(0.0)[i];
        CHECK(placement.position.x >= 0.0);
        CHECK(placement.position.x <= 1.0);
    }

    source.stop();
}
