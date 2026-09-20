#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <thread>
#include <vector>

#include "ac3/audio/monitor.hpp"

// MonitorSink's playback position, pause and flush against a real output
// device (src/audio/src/backend/*/monitor.cpp).
//
// Hidden: the tag starts with a dot, so `ac3tests` does not run this - it
// needs a sound card, plays about a second of quiet tone through the default
// output, and is the only way to exercise what each platform's own clock
// reports. The portable half of the same arithmetic is checked without
// hardware in test_playback_counter.cpp, which does run everywhere.
//
// Run it deliberately:  ac3tests "[monitor-live]"
//   Windows: WASAPI shared mode. Linux: ALSA or PipeWire, whichever the build
//   selected. macOS: Core Audio. Each reads its position from a different
//   platform call, and this case is the same check over all of them.

namespace {

constexpr std::uint32_t kRate = 48'000;
constexpr std::uint16_t kChannels = 2;
constexpr std::size_t kChunkFrames = 480;

// A quiet tone, so a test that runs on somebody's desk is not startling.
std::vector<float> tone_chunk(double& phase) {
    std::vector<float> chunk(kChunkFrames * kChannels);
    const double step = 2.0 * std::numbers::pi * 440.0 / kRate;
    for (std::size_t frame = 0; frame < kChunkFrames; ++frame) {
        const auto sample = static_cast<float>(0.03 * std::sin(phase));
        phase += step;
        for (std::uint16_t channel = 0; channel < kChannels; ++channel) {
            chunk[frame * kChannels + channel] = sample;
        }
    }
    return chunk;
}

// Keeps the sink fed for `chunks` periods, which is what a caller playing in
// real time does; submit() refusing means the queue is full, not an error.
void feed(ac3::audio::MonitorSink& sink, double& phase, int chunks) {
    for (int i = 0; i < chunks; ++i) {
        const auto chunk = tone_chunk(phase);
        for (int attempt = 0; attempt < 200 && !sink.submit(chunk); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
}

}  // namespace

TEST_CASE("monitor live: the position follows the device, and pause and flush hold it",
          "[.][monitor-live]") {
    ac3::audio::MonitorSink sink;
    const auto started = sink.start(/*device_id=*/"", kRate, kChannels);
    if (!started) {
        WARN("no default output device: " << ac3::audio::describe(started.error()));
        return;
    }
    REQUIRE(sink.running());
    CHECK_FALSE(sink.paused());

    double phase = 0.0;
    feed(sink, phase, 25);  // a quarter of a second, queued ahead

    // The device's own clock, reported once the render thread has run: every
    // backend fills this in from its own platform call, so the one thing to
    // establish is that it moves, and moves forward.
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    const auto first = sink.position();
    REQUIRE(first.has_value());
    feed(sink, phase, 25);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto second = sink.position();
    REQUIRE(second.has_value());
    CHECK(second->frames_played > first->frames_played);
    // Under a second of audio has been submitted, so the position cannot
    // sensibly be past that however the platform counts.
    CHECK(second->frames_played < kRate);

    // A pause stops the device without closing it: the position stands still
    // and the queue goes on taking frames.
    REQUIRE(sink.pause().has_value());
    CHECK(sink.paused());
    const auto paused_at = sink.position();
    REQUIRE(paused_at.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto still = sink.position();
    REQUIRE(still.has_value());
    // A period may still be in flight when pause() returns, so this allows
    // one, not the 9600 frames 200 ms of playback would have added.
    CHECK(still->frames_played - paused_at->frames_played <= kChunkFrames * 2);
    CHECK(sink.submit(tone_chunk(phase)));

    REQUIRE(sink.resume().has_value());
    CHECK_FALSE(sink.paused());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto resumed = sink.position();
    REQUIRE(resumed.has_value());
    CHECK(resumed->frames_played > still->frames_played);
    // About 200 ms of playing, and none of the 200 ms paused: a clock that
    // ran on through the pause would put this near 400 ms.
    CHECK(resumed->frames_played - still->frames_played < kRate * 3 / 10);

    // A flush drops what has not been played, here and in the device, and the
    // position counts from zero again.
    feed(sink, phase, 25);
    sink.flush();
    const auto flushed = sink.position();
    REQUIRE(flushed.has_value());
    CHECK(flushed->frames_played <= kChunkFrames * 2);
    CHECK(flushed->frames_queued <= kChunkFrames * 4);
    CHECK(sink.stats().frames_submitted <= kChunkFrames * 4);

    // And playback carries on from the next submit.
    feed(sink, phase, 25);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto after = sink.position();
    REQUIRE(after.has_value());
    CHECK(after->frames_played > 0);

    sink.stop();
    CHECK_FALSE(sink.running());
    CHECK_FALSE(sink.paused());
    CHECK_FALSE(sink.position().has_value());
}

// An output that goes away mid-stream, with a person to take it away.
//
// Hidden, under a tag of its own so that "[monitor-live]" never waits for
// one: run it deliberately and follow the prompt.
//
//   ac3tests "[monitor-unplug]"
//
// Within 30 seconds of the prompt, take the default output away: unplug a USB
// or HDMI audio device, or disable it in the system's sound settings. A
// shared-mode stream the platform moves to another output of its own accord
// (PipeWire's session manager does) carries on playing, which this case then
// reports rather than fails. Otherwise the sink has to notice on its own,
// answer every call as a stopped sink does, and start() has to open whatever
// the default output is now, with no stop() in between.
TEST_CASE("monitor live: an output that goes away stops the sink, which can start again",
          "[.][monitor-unplug]") {
    using ac3::audio::MonitorError;
    using namespace std::chrono_literals;

    ac3::audio::MonitorSink sink;
    const auto started = sink.start(/*device_id=*/"", kRate, kChannels);
    if (!started) {
        WARN("no default output device: " << ac3::audio::describe(started.error()));
        return;
    }
    double phase = 0.0;

    WARN("Take the default output away now: unplug it or disable it (30 s).");
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (sink.running() && std::chrono::steady_clock::now() < deadline) {
        if (!sink.submit(tone_chunk(phase))) {
            std::this_thread::sleep_for(2ms);
        }
    }
    if (sink.running()) {
        WARN("still playing after 30 s: nothing was taken away, or the stream moved to "
             "another output");
        sink.stop();
        return;
    }

    // A stopped sink's answers, each at once.
    CHECK_FALSE(sink.position().has_value());
    CHECK_FALSE(sink.can_submit());
    CHECK_FALSE(sink.submit(tone_chunk(phase)));
    CHECK_FALSE(sink.paused());
    const auto before = std::chrono::steady_clock::now();
    sink.flush();
    CHECK(std::chrono::steady_clock::now() - before < 50ms);
    const auto paused = sink.pause();
    REQUIRE_FALSE(paused.has_value());
    CHECK(paused.error() == MonitorError::kNotRunning);
    const auto resumed = sink.resume();
    REQUIRE_FALSE(resumed.has_value());
    CHECK(resumed.error() == MonitorError::kNotRunning);

    // start() again, with no stop() first, on whatever the default is now.
    const auto again = sink.start(/*device_id=*/"", kRate, kChannels);
    if (!again) {
        WARN("no default output to start again on: " << ac3::audio::describe(again.error()));
        return;
    }
    REQUIRE(sink.running());
    feed(sink, phase, 25);
    std::this_thread::sleep_for(200ms);
    const auto playing = sink.position();
    REQUIRE(playing.has_value());
    CHECK(playing->frames_played > 0);
    sink.stop();
    CHECK_FALSE(sink.running());
}
