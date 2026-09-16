#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <thread>

#include "ac3/audio/passthrough.hpp"
#include "ac3/audio/playback_counter.hpp"

// ac3::audio::PlaybackCounter against a fake device's clock
// (src/audio/include/ac3/audio/playback_counter.hpp).
//
// The counter is the platform-independent half of MonitorPosition: every
// backend hands it the frames it has given the device and the frames the
// device says it still holds, and what a caller reads back follows from those
// two. So a fake device with a clock of its own - it takes frames, plays them
// at whatever rate the test chooses, and reports what it has left - drives the
// same code the real backends do, which is what A2's exit asks for. What no
// test on a build machine can check is the platform call each backend reads
// its two numbers FROM; that is what the receiver and the Pi are for.

namespace {

// A device that takes frames and plays them on its own clock. `unplayed()` is
// what WASAPI's GetCurrentPadding, snd_pcm_delay, PipeWire's delay and the
// Core Audio timestamp lead each report in their own way.
struct FakeDevice {
    std::uint64_t handed_over = 0;
    std::uint64_t played = 0;

    void hand_over(std::uint64_t frames) { handed_over += frames; }

    // Time passes: the device plays up to `frames` more of what it holds.
    void play(std::uint64_t frames) { played = std::min(handed_over, played + frames); }

    [[nodiscard]] std::uint64_t unplayed() const { return handed_over - played; }
};

}  // namespace

TEST_CASE("playback counter: the reported position follows the device's own clock",
          "[audio-backend][playback-counter]") {
    ac3::audio::PlaybackCounter counter;
    FakeDevice device;

    // A caller that has submitted 4800 frames and a sink passing them on a
    // period of 480 at a time, to a device that plays a period behind - the
    // period it is handed now is the one it plays next.
    std::uint64_t queued = 4800;
    std::uint64_t in_flight = 0;
    for (int period = 0; period < 4; ++period) {
        device.play(in_flight);
        queued -= 480;
        device.hand_over(480);
        in_flight = 480;
        counter.report(device.handed_over, device.unplayed());

        const auto position = counter.position(queued, /*latency=*/0);
        CHECK(position.frames_played == device.played);
        CHECK(position.frames_queued == queued + device.unplayed());
        // Nothing is lost and nothing is counted twice: everything handed to
        // the device is either played or still waiting somewhere.
        CHECK(position.frames_played + position.frames_queued == 4800);
    }
    CHECK(counter.position(queued, 0).frames_played == 1440);
    CHECK(counter.played() == 1440);

    // A device slower than the frames arriving: the queue grows, the played
    // count still tracks the device rather than the submissions.
    for (int period = 0; period < 4; ++period) {
        device.hand_over(480);
        device.play(120);
        counter.report(device.handed_over, device.unplayed());
    }
    CHECK(counter.position(0, 0).frames_played == 1440 + 480);
    CHECK(counter.position(0, 0).frames_queued == device.unplayed());
}

TEST_CASE("playback counter: a restart counts from zero again",
          "[audio-backend][playback-counter]") {
    ac3::audio::PlaybackCounter counter;

    counter.report(48'000, 1'200);
    CHECK(counter.position(0, 0).frames_played == 46'800);

    // What flush() promises: a frame submitted and dropped is in none of the
    // figures, and the next report starts the count again.
    counter.restart();
    CHECK(counter.position(0, 0).frames_played == 0);
    CHECK(counter.position(0, 0).frames_queued == 0);
    CHECK(counter.played() == 0);

    counter.report(960, 480);
    CHECK(counter.position(0, 0).frames_played == 480);
}

TEST_CASE("playback counter: a device is never further ahead than what it was given",
          "[audio-backend][playback-counter]") {
    ac3::audio::PlaybackCounter counter;

    // Every backend's unplayed figure comes from the platform, and a device
    // that has just been prepared, paused or restarted can report a delay
    // larger than anything this stream put in it. Played frames stay at zero
    // rather than wrapping through the bottom of a 64-bit count.
    counter.report(480, 96'000);
    CHECK(counter.position(0, 0).frames_played == 0);
    CHECK(counter.position(0, 0).frames_queued == 480);

    counter.report(0, 0);
    CHECK(counter.position(0, 0).frames_played == 0);
    CHECK(counter.position(0, 0).frames_queued == 0);
}

TEST_CASE("playback counter: the queue and the device's buffer are both reported",
          "[audio-backend][playback-counter]") {
    ac3::audio::PlaybackCounter counter;

    counter.report(/*handed_over=*/2'400, /*unplayed=*/900);
    const auto position = counter.position(/*queued=*/7'200, /*latency=*/256);
    CHECK(position.frames_played == 1'500);
    CHECK(position.frames_queued == 8'100);
    // Passed through as the platform gave it, in frames, and not folded into
    // the queue: it is the delay past the device's buffer, not part of it.
    CHECK(position.latency_frames == 256);
}

TEST_CASE("playback counter: a reader sees a consistent position while the device reports",
          "[audio-backend][playback-counter][concurrency]") {
    ac3::audio::PlaybackCounter counter;
    std::atomic_bool stop{false};
    std::atomic<std::uint64_t> reads{0};
    std::atomic_bool wrong{false};

    // The arrangement every backend has: the device thread reports, the
    // caller's thread asks. The reader may catch a report half-made, but must
    // never see a played count beyond what the device has been given or one
    // that has run backwards - which is why report() stores the difference
    // rather than leaving the reader to subtract.
    std::thread reader([&] {
        std::uint64_t last = 0;
        while (!stop.load(std::memory_order_acquire)) {
            const auto position = counter.position(0, 0);
            if (position.frames_played < last || position.frames_played > 480'000) {
                wrong.store(true, std::memory_order_relaxed);
            }
            last = position.frames_played;
            reads.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // The loop below takes microseconds, which is less than it can take for a
    // new thread to be scheduled at all - so wait for the reader's first read
    // rather than race it and check nothing.
    while (reads.load(std::memory_order_relaxed) == 0) {
        std::this_thread::yield();
    }

    FakeDevice device;
    for (int period = 0; period < 1'000; ++period) {
        device.hand_over(480);
        device.play(480);
        counter.report(device.handed_over, device.unplayed());
    }
    stop.store(true, std::memory_order_release);
    reader.join();

    CHECK(reads.load() > 0);
    CHECK_FALSE(wrong.load());
    CHECK(counter.position(0, 0).frames_played == 480'000);
}

TEST_CASE("play head: a 32-bit count is widened past its wrap", "[audio-backend][playback-counter]") {
    ac3::audio::PlayHead head;
    CHECK(head.read(0) == 0);
    CHECK(head.read(1000) == 1000);
    CHECK(head.read(0xFFFFFF00U) == 0xFFFFFF00U);
    // Round, and on.
    CHECK(head.read(0x100U) == 0x100000100ULL);
    CHECK(head.read(0x20000000U) == 0x120000000ULL);
    CHECK(head.read(0xF0000000U) == 0x1F0000000ULL);
    CHECK(head.read(0x10U) == 0x200000010ULL);
}

TEST_CASE("play head: a zero reading holds, and a count that starts again carries on",
          "[audio-backend][playback-counter]") {
    ac3::audio::PlayHead head;
    CHECK(head.read(48'000) == 48'000);
    // The HAL did not answer: nothing is taken from that.
    CHECK(head.read(0) == 48'000);
    // And answered again, from where it was.
    CHECK(head.read(48'480) == 48'480);

    // The output went to standby and its count started again: what has been
    // played stays played, and the new count adds to it.
    CHECK(head.read(0) == 48'480);
    CHECK(head.read(0) == 48'480);
    CHECK(head.read(960) == 48'480);
    CHECK(head.read(1'920) == 49'440);
    // A step back that is not near the top and the bottom of the range is a
    // restart, not a wrap.
    CHECK(head.read(0x90000000U) > 49'440);
    const auto before = head.read(0x90000100U);
    CHECK(head.read(0x30000000U) == before);
}

TEST_CASE("play head: after a flush the old count reads as nothing played",
          "[audio-backend][playback-counter]") {
    ac3::audio::PlayHead head;
    CHECK(head.read(96'000) == 96'000);
    head.restart();
    // The flush has not reached the hardware: the old count, still rising.
    CHECK(head.read(96'000) == 0);
    CHECK(head.read(96'480) == 0);
    // It has: the count starts from zero, and so does what is played.
    CHECK(head.read(0) == 0);
    CHECK(head.read(480) == 480);
    CHECK(head.read(960) == 960);

    // Read first below the old count, already moving.
    head.restart();
    CHECK(head.read(240) == 240);
    CHECK(head.read(720) == 720);

    // A flush before anything was read holds nothing back.
    ac3::audio::PlayHead fresh;
    fresh.restart();
    CHECK(fresh.read(5'000) == 5'000);

    // A count that had gone round starts from zero too.
    ac3::audio::PlayHead wrapped;
    CHECK(wrapped.read(0xFFFFFF00U) == 0xFFFFFF00U);
    CHECK(wrapped.read(0x100U) == 0x100000100ULL);
    wrapped.restart();
    CHECK(wrapped.read(0) == 0);
    CHECK(wrapped.read(480) == 480);

    // An old count that never comes down is taken as it is, in the end.
    head.restart();
    for (int i = 0; i < ac3::audio::PlayHead::kStaleReadings; ++i) {
        CHECK(head.read(1'000'000) == 0);
    }
    CHECK(head.read(1'000'480) == 1'000'480);
}

TEST_CASE("playback counter: a passthrough link counts in the content's frames",
          "[audio-backend][playback-counter]") {
    // An E-AC-3 link runs four frames to each content frame, so a burst's
    // 6144 link frames are the 1536 content frames the player counts.
    ac3::audio::PlaybackCounter counter;
    FakeDevice link;
    link.hand_over(6144 * 3);
    link.play(6144 + 5);
    counter.report(link.handed_over, link.unplayed());
    const auto eac3 = ac3::audio::per_content_frame(counter.position(6144, 960),
                                                    ac3::audio::carrier_ratio(
                                                        ac3::audio::BitstreamFormat::kEac3));
    CHECK(eac3.frames_played == 1536 + 1);
    CHECK(eac3.frames_queued == (6144 * 3 - 6144 - 5 + 6144) / 4);
    CHECK(eac3.latency_frames == 240);

    // AC-3's link is the content's own rate.
    const auto ac3_link = ac3::audio::per_content_frame(
        counter.position(0, 0), ac3::audio::carrier_ratio(ac3::audio::BitstreamFormat::kAc3));
    CHECK(ac3_link.frames_played == 6144 + 5);
    // A ratio of zero is taken as one rather than divided by.
    CHECK(ac3::audio::per_content_frame(counter.position(0, 0), 0).frames_played == 6144 + 5);
}
