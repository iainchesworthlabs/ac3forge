#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "ac3/audio/spatial.hpp"

// SpatialObjectSink against a real spatial-capable output
// (src/audio/src/backend/*/spatial.cpp).
//
// Hidden: the tag starts with a dot, so `ac3tests` does not run this. Real
// device loss can't be simulated - it needs an endpoint with a spatial sound
// format already enabled (Windows Sonic for Headphones or Dolby Atmos for
// Home Theater/Headphones) and a person to take it away mid-session, the
// same [.][passthrough-unplug]/[.][monitor-unplug] shape test_passthrough_live.cpp
// and test_monitor_live.cpp already use for their own sinks. Every other
// backend is the kNoBackend stub (test_audio_backend.cpp's own
// capability-agreement checks cover that half), so start() there always
// refuses with kNoBackend and this case WARNs and returns.
//
// Run it deliberately:  ac3tests "[spatial-unplug]"

namespace {

constexpr std::uint32_t kRate = 48'000;
constexpr std::size_t kChunkFrames = 480;

}  // namespace

// An endpoint that goes away mid-stream, with a person to take it away.
//
// Hidden, and under a tag of its own so that no other run waits for one: run
// it deliberately, on an endpoint with a spatial sound format already
// enabled, and follow the prompt.
//
//   ac3tests "[spatial-unplug]"
//
// Within 30 seconds of the prompt, take the default output away: unplug a USB
// or HDMI spatial-capable device, or disable it in Settings > System > Sound.
// The sink has to notice on its own and answer submit()/can_submit() as a
// stopped sink does - whether the loss shows up as BeginUpdatingAudioObjects
// failing outright or as the endpoint simply going quiet - and start() has to
// work again afterwards with no stop() in between.
TEST_CASE("spatial live: an endpoint that goes away stops the sink, which can start again",
          "[.][spatial-unplug]") {
    using namespace std::chrono_literals;

    ac3::audio::SpatialObjectSink sink;
    const auto started = sink.start(/*device_id=*/"", kRate, /*static_channels=*/0,
                                     /*max_dynamic_objects=*/1);
    if (!started) {
        WARN("no spatial-capable default output: " << ac3::audio::describe(started.error()));
        return;
    }
    REQUIRE(sink.running());

    const std::vector<float> block(kChunkFrames, 0.0F);
    const std::array<ac3::audio::DynamicObjectUpdate, 1> updates{
        ac3::audio::DynamicObjectUpdate{.pcm = block}};

    WARN("Take the default output away now: unplug it or disable it (30 s).");
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (sink.running() && std::chrono::steady_clock::now() < deadline) {
        if (!sink.submit(updates, {})) {
            std::this_thread::sleep_for(2ms);
        }
    }
    if (sink.running()) {
        WARN("still rendering after 30 s: nothing was taken away");
        sink.stop();
        return;
    }

    // A stopped sink's answers, each at once - the same contract
    // PassthroughSink/MonitorSink's own unplug cases check.
    CHECK_FALSE(sink.can_submit());
    CHECK_FALSE(sink.submit(updates, {}));

    // start() again, with no stop() first, on whatever the default is now.
    const auto again = sink.start(/*device_id=*/"", kRate, /*static_channels=*/0,
                                   /*max_dynamic_objects=*/1);
    if (!again) {
        WARN("no spatial-capable default output to start again on: "
             << ac3::audio::describe(again.error()));
        return;
    }
    REQUIRE(sink.running());
    for (int i = 0; i < 25; ++i) {
        for (int attempt = 0; attempt < 200 && !sink.submit(updates, {}); ++attempt) {
            std::this_thread::sleep_for(2ms);
        }
    }
    std::this_thread::sleep_for(200ms);
    CHECK(sink.stats().updates_rendered > 0);
    sink.stop();
    CHECK_FALSE(sink.running());
}
