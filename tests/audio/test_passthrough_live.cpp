#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "ac3/audio/passthrough.hpp"
#include "ac3/core/tables.hpp"
#include "ac3/encoder/encoder.hpp"
#include "ac3/iec61937/iec61937.hpp"

// PassthroughSink's playback position, pause and flush against a real
// receiver (src/audio/src/backend/*/passthrough.cpp).
//
// Hidden: the tag starts with a dot, so `ac3tests` does not run this. It
// needs an output that takes AC-3 over IEC 61937 - an HDMI or S/PDIF link to
// a receiver - and bitstreams about two seconds of AC-3 silence to the first
// one the enumeration says will take it. The portable arithmetic behind the
// position is checked without hardware in test_playback_counter.cpp.
//
// Run it deliberately:  ac3tests "[passthrough-live]"
//   Windows: WASAPI exclusive mode. Linux: ALSA or PipeWire, whichever the
//   build selected. macOS: Core Audio. Android: AAudio. The receiver's display
//   shows Dolby Digital while it runs; after the pause it may take a moment to
//   show it again.

namespace {

constexpr std::uint32_t kRate = 48'000;
constexpr std::uint64_t kBurstFrames = ac3::kSamplesPerFrame;

// One silent AC-3 stereo frame as a burst: silence, since a test that runs on
// somebody's receiver should not be heard.
std::vector<std::byte> silent_burst() {
    ac3::EncoderConfig config;
    config.sample_rate = ac3::SampleRate::k48000;
    config.bitrate_kbps = 192;
    config.acmod = ac3::Acmod::k2_0;
    ac3::FrameEncoder encoder{config};
    const std::vector<float> silence(ac3::kSamplesPerFrame, 0.0F);
    const std::vector<std::span<const float>> views(2, silence);
    const auto frame = encoder.encode_frame(views);
    REQUIRE(frame.has_value());
    auto burst = ac3::iec61937::wrap_frame(*frame);
    REQUIRE(burst.has_value());
    return std::move(*burst);
}

// Keeps the sink fed with `bursts` more, which is what a caller playing in
// real time does; submit() refusing means the queue is full, not an error.
void feed(ac3::audio::PassthroughSink& sink, const std::vector<std::byte>& burst, int bursts) {
    for (int i = 0; i < bursts; ++i) {
        for (int attempt = 0; attempt < 400 && !sink.submit(burst); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
}

// The first output the enumeration says takes AC-3, or nothing.
std::string ac3_output() {
    const auto devices = ac3::audio::enumerate_render_devices(kRate);
    if (devices) {
        for (const auto& device : *devices) {
            if (device.supports_ac3_passthrough) {
                WARN("bitstreaming to " << device.name);
                return device.id;
            }
        }
    }
    WARN("no output here takes AC-3 over IEC 61937");
    return {};
}

}  // namespace

TEST_CASE("passthrough live: the position follows the receiver's link, and pause and flush hold it",
          "[.][passthrough-live]") {
    const std::string id = ac3_output();
    if (id.empty()) {
        return;
    }

    ac3::audio::PassthroughSink sink;
    const auto started = sink.start(id, kRate, ac3::audio::BitstreamFormat::kAc3);
    if (!started) {
        WARN("the passthrough output would not open: " << ac3::audio::describe(started.error()));
        return;
    }
    REQUIRE(sink.running());
    CHECK_FALSE(sink.paused());
    const auto burst = silent_burst();
    feed(sink, burst, 8);  // a quarter of a second, queued ahead

    // The link's own clock, in the content's frames: it moves, and forward.
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    const auto first = sink.position();
    REQUIRE(first.has_value());
    feed(sink, burst, 8);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto second = sink.position();
    REQUIRE(second.has_value());
    CHECK(second->frames_played > first->frames_played);
    // Under a second has gone by, so under a second can have been played.
    CHECK(second->frames_played < kRate);

    // A pause stops the device without closing it: the position stands still
    // and the queue goes on taking bursts.
    REQUIRE(sink.pause().has_value());
    CHECK(sink.paused());
    const auto paused_at = sink.position();
    REQUIRE(paused_at.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto still = sink.position();
    REQUIRE(still.has_value());
    // A period may still be in flight when pause() returns.
    CHECK(still->frames_played - paused_at->frames_played <= kBurstFrames);
    CHECK(sink.submit(burst));
    REQUIRE(sink.resume().has_value());
    CHECK_FALSE(sink.paused());
    feed(sink, burst, 8);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto resumed = sink.position();
    REQUIRE(resumed.has_value());
    CHECK(resumed->frames_played > still->frames_played);
    // About 200 ms of playing, and none of the 200 ms paused: a clock that
    // ran on through the pause would put this near 400 ms.
    CHECK(resumed->frames_played - still->frames_played < kRate * 3 / 10);

    // A flush drops what has not been played, here and in the device, and the
    // position counts from zero again.
    feed(sink, burst, 8);
    sink.flush();
    const auto flushed = sink.position();
    REQUIRE(flushed.has_value());
    CHECK(flushed->frames_played <= kBurstFrames);
    CHECK(flushed->frames_queued <= kBurstFrames * 2);

    // And playback carries on from the next submit.
    feed(sink, burst, 8);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto after = sink.position();
    REQUIRE(after.has_value());
    CHECK(after->frames_played > 0);

    sink.stop();
    CHECK_FALSE(sink.running());
    CHECK_FALSE(sink.paused());
    CHECK_FALSE(sink.position().has_value());
}

// A receiver that goes away mid-stream, with a person to take it away.
//
// Hidden, and under a tag of its own so that "[passthrough-live]" never waits
// for one: run it deliberately, with a receiver bitstreaming, and follow the
// two prompts.
//
//   ac3tests "[passthrough-unplug]"
//
// First, within 30 seconds, pull the HDMI or S/PDIF cable, or switch the
// receiver off or to another input (whichever makes the machine lose the
// output - on Windows, disabling the device under Sound settings does it
// too). Then, within 30 seconds more, put it back. The sink has to notice the
// first on its own and answer every call as a stopped sink does; and start()
// has to work again afterwards with no stop() in between.
TEST_CASE("passthrough live: a receiver that goes away stops the sink, which can start again",
          "[.][passthrough-unplug]") {
    using ac3::audio::PassthroughError;
    using namespace std::chrono_literals;

    const std::string id = ac3_output();
    if (id.empty()) {
        return;
    }
    ac3::audio::PassthroughSink sink;
    const auto started = sink.start(id, kRate, ac3::audio::BitstreamFormat::kAc3);
    if (!started) {
        WARN("the passthrough output would not open: " << ac3::audio::describe(started.error()));
        return;
    }
    const auto burst = silent_burst();

    WARN("Take the output away now: unplug the cable or switch the receiver off (30 s).");
    auto deadline = std::chrono::steady_clock::now() + 30s;
    while (sink.running() && std::chrono::steady_clock::now() < deadline) {
        if (!sink.submit(burst)) {
            std::this_thread::sleep_for(2ms);
        }
    }
    REQUIRE_FALSE(sink.running());

    // A stopped sink's answers, each at once.
    CHECK_FALSE(sink.position().has_value());
    CHECK_FALSE(sink.can_submit());
    CHECK_FALSE(sink.submit(burst));
    CHECK_FALSE(sink.paused());
    const auto before = std::chrono::steady_clock::now();
    sink.flush();
    CHECK(std::chrono::steady_clock::now() - before < 50ms);
    const auto paused = sink.pause();
    REQUIRE_FALSE(paused.has_value());
    CHECK(paused.error() == PassthroughError::kNotRunning);
    const auto resumed = sink.resume();
    REQUIRE_FALSE(resumed.has_value());
    CHECK(resumed.error() == PassthroughError::kNotRunning);

    // start() again, with no stop() first: refused while the output is away,
    // and playing once it is back.
    WARN("Put the output back now (30 s).");
    deadline = std::chrono::steady_clock::now() + 30s;
    while (!sink.running() && std::chrono::steady_clock::now() < deadline) {
        if (!sink.start(id, kRate, ac3::audio::BitstreamFormat::kAc3)) {
            std::this_thread::sleep_for(500ms);
        }
    }
    REQUIRE(sink.running());
    feed(sink, burst, 8);
    std::this_thread::sleep_for(200ms);
    const auto playing = sink.position();
    REQUIRE(playing.has_value());
    CHECK(playing->frames_played > 0);
    sink.stop();
    CHECK_FALSE(sink.running());
}
