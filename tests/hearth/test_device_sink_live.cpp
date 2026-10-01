#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "iclforge/audio/monitor.hpp"
#include "iclforge/render/layout.hpp"
#include "pcm_sink.hpp"

// Hearth's real local output (apps/hearth/engine/device_sink.cpp, the sink the
// window builds) against a real device. Every other test of the player runs it
// against a fake device that opens at any rate, which is how a machine whose
// output would not take an item's own rate went unnoticed.
//
// Hidden: the tag starts with a dot, so `iclforge-tests` does not run this - it needs
// a sound card. It plays silence, so a run on somebody's desk makes no sound.
//
// Run it deliberately:  iclforge-tests "[hearth-device]"

TEST_CASE("device sink live: the default output opens at an item's own rate and plays",
          "[.][hearth-device]") {
    using iclforge::hearth::OutputMode;
    using iclforge::hearth::PcmSink;

    const auto layout = iclforge::render::OutputLayout::parse("5.1");
    REQUIRE(layout.has_value());

    // 44.1 kHz is what most songs are and 48 kHz what most AC-3 is; the third
    // is here because at most one of the three is the rate the output's engine
    // runs at, so at least two of them have to be converted.
    for (const std::uint32_t rate : {44'100U, 48'000U, 96'000U}) {
        CAPTURE(rate);
        const auto sink = iclforge::hearth::make_device_sink(std::string{});
        const auto opened = sink->open(PcmSink::Format{.sample_rate = rate, .layout = *layout});
        if (!opened) {
            INFO("open() said: " << opened.error());
            // A machine with no output says something else; this refusal is the bug.
            const std::string_view refusal =
                iclforge::audio::describe(iclforge::audio::MonitorError::kFormatRejected);
            REQUIRE(std::string_view{opened.error()}.find(refusal) == std::string_view::npos);
            WARN("no default output device: " << opened.error());
            return;
        }
        REQUIRE(sink->is_open());
        CHECK(opened->sample_rate == rate);
        CHECK(opened->channels > 0);
        CHECK(opened->mode == OutputMode::kLocalPcm);

        constexpr std::size_t kFrames = 480;
        const std::vector<float> silence(kFrames, 0.0F);
        const std::vector<std::span<const float>> slots(layout->slots(), std::span{silence});
        for (int block = 0; block < 25; ++block) {
            for (int attempt = 0; attempt < 200 && !sink->submit(slots, kFrames); ++attempt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));

        // The device's own clock moved, counted in the item's own frames.
        const auto position = sink->position();
        REQUIRE(position.has_value());
        CHECK(position->frames_played > 0);
        CHECK(position->frames_played < rate);

        sink->close();
        CHECK_FALSE(sink->is_open());
    }
}
