#include <catch2/catch_test_macros.hpp>

#include "position.h"

// The null-sink driver's clock (apps/windows/driver/Source/Main/position.h):
// the one piece of the driver that is logic rather than framework plumbing,
// kept free of kernel dependencies so it can be pinned down here, where
// coverage is measured, rather than in a guest where it cannot be. Every
// number below is the 7.1/48 kHz/16-bit format the driver offers: 768,000
// bytes per second, 16-byte blocks.

using ac3nullsink::kHnsPerSecond;
using ac3nullsink::PositionClock;
using ac3nullsink::u64;

namespace {

constexpr u64 kBytesPerSecond = 48000ULL * 8 * 2;
constexpr u64 kPacketBytes = 7680;  // 10 ms of 7.1 at 48 kHz, 16-bit
constexpr u64 kMs = kHnsPerSecond / 1000;

PositionClock configured() {
    PositionClock clock;
    clock.configure(kBytesPerSecond, kPacketBytes);
    return clock;
}

}  // namespace

TEST_CASE("nullsink clock: nothing moves until Run", "[crucible][nullsink]") {
    PositionClock clock = configured();
    CHECK_FALSE(clock.running());
    CHECK(clock.position_at(5 * kHnsPerSecond) == 0);
    CHECK(clock.packets_due(5 * kHnsPerSecond) == 0);
    CHECK(clock.current_packet() == 0);
}

TEST_CASE("nullsink clock: the position advances at exactly the nominal rate", "[crucible][nullsink]") {
    PositionClock clock = configured();
    const u64 t0 = 123456789;
    clock.run(t0);
    CHECK(clock.running());
    CHECK(clock.position_at(t0) == 0);
    CHECK(clock.position_at(t0 + kHnsPerSecond) == kBytesPerSecond);
    CHECK(clock.position_at(t0 + 10 * kMs) == kPacketBytes);
    // A quarter of a second in, to the byte.
    CHECK(clock.position_at(t0 + kHnsPerSecond / 4) == kBytesPerSecond / 4);
}

TEST_CASE("nullsink clock: the position never runs backwards", "[crucible][nullsink]") {
    PositionClock clock = configured();
    clock.run(1000);
    u64 last = 0;
    for (u64 t = 1000; t < 1000 + 2 * kHnsPerSecond; t += 137) {
        const u64 now = clock.position_at(t);
        CHECK(now >= last);
        last = now;
    }
    // A clock read from before Run (a QPC race) reads as the start, not
    // as a wrap.
    CHECK(clock.position_at(500) == 0);
}

TEST_CASE("nullsink clock: pause freezes, run resumes from where it left off", "[crucible][nullsink]") {
    PositionClock clock = configured();
    clock.run(0);
    clock.pause(100 * kMs);
    const u64 paused = clock.position_at(100 * kMs);
    CHECK(paused == 10 * kPacketBytes);
    CHECK_FALSE(clock.running());
    // Time passes while paused; the position does not.
    CHECK(clock.position_at(5 * kHnsPerSecond) == paused);
    // Resume much later: continuous, no jump.
    clock.run(5 * kHnsPerSecond);
    CHECK(clock.position_at(5 * kHnsPerSecond) == paused);
    CHECK(clock.position_at(5 * kHnsPerSecond + 10 * kMs) == paused + kPacketBytes);
}

TEST_CASE("nullsink clock: stop returns to zero", "[crucible][nullsink]") {
    PositionClock clock = configured();
    clock.run(0);
    (void)clock.mark_completed();
    clock.stop();
    CHECK(clock.position_at(kHnsPerSecond) == 0);
    CHECK(clock.current_packet() == 0);
    CHECK_FALSE(clock.running());
}

TEST_CASE("nullsink clock: packets complete as the position passes their ends", "[crucible][nullsink]") {
    PositionClock clock = configured();
    clock.run(0);
    CHECK(clock.packets_due(9 * kMs) == 0);
    CHECK(clock.packets_due(10 * kMs) == 1);
    CHECK(clock.next_completion_hns() == 10 * kMs);
    CHECK(clock.mark_completed() == 0);
    CHECK(clock.current_packet() == 1);
    CHECK(clock.packets_due(10 * kMs) == 0);
    CHECK(clock.next_completion_hns() == 20 * kMs);
}

TEST_CASE("nullsink clock: a late timer owes every packet it missed, once each", "[crucible][nullsink]") {
    // A debugger break or a suspended guest: the timer fires 55 ms late.
    PositionClock clock = configured();
    clock.run(0);
    CHECK(clock.packets_due(55 * kMs) == 5);
    for (u64 i = 0; i < 5; ++i) {
        CHECK(clock.mark_completed() == i);
    }
    CHECK(clock.packets_due(55 * kMs) == 0);
    // The schedule does not slide: the next packet is due at 60 ms, not
    // 55 + 10, so the position stays at the nominal rate.
    CHECK(clock.next_completion_hns() == 60 * kMs);
}

TEST_CASE("nullsink clock: completions after a pause line up with the position", "[crucible][nullsink]") {
    PositionClock clock = configured();
    clock.run(0);
    // Pause mid-packet: 1.5 packets in, one complete.
    clock.pause(15 * kMs);
    CHECK(clock.packets_due(15 * kMs) == 1);
    (void)clock.mark_completed();
    // Resume at t = 1 s; the second packet completes another 5 ms later.
    clock.run(kHnsPerSecond);
    CHECK(clock.next_completion_hns() == kHnsPerSecond + 5 * kMs);
    CHECK(clock.packets_due(kHnsPerSecond + 4 * kMs) == 0);
    CHECK(clock.packets_due(kHnsPerSecond + 5 * kMs) == 1);
}

TEST_CASE("nullsink clock: an unconfigured packet size owes nothing", "[crucible][nullsink]") {
    PositionClock clock;
    clock.configure(kBytesPerSecond, 0);
    clock.run(0);
    CHECK(clock.packets_due(kHnsPerSecond) == 0);
    CHECK(clock.next_completion_hns() == 0);
}
