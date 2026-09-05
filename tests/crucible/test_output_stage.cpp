#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <memory>
#include <numbers>
#include <span>
#include <vector>

#include "ac3/oba/atmos.hpp"
#include "fake_devices.hpp"
#include "output_stage.hpp"
#include "slots.hpp"

// The output stage over fake devices: the policy's choice becomes a live
// sink of the right kind on the right endpoint, each of the five routes
// carries a real access unit from the demo's own encoder to that sink, a
// probe that changes the answer switches sinks mid-stream, a sink that
// refuses to start leaves the stage in "none" with a reason, a full sink
// counts an underrun, and the bypass takes the raw frame instead of a
// decode.

using namespace ac3::crucible;
using namespace ac3::crucible::testing;

namespace {

constexpr std::size_t kFrames = 1536;  // six blocks

// The engine's own front half in miniature: fifteen objects, a tone on the
// first positioned slot and on each bed slot, encoded into one E-AC-3
// access unit per frame with the encoder's 5.1 bed alongside.
struct Encoded {
    std::unique_ptr<ac3::oba::AtmosEncoder> encoder;
    std::vector<std::vector<float>> objects;
    std::vector<std::span<const float>> views;
    std::vector<ac3::oba::ObjectPlacement> placements;
    std::vector<std::span<const float>> bed_views;
    std::vector<std::byte> unit;
    double phase = 0.0;

    Encoded() {
        ac3::oba::AtmosConfig atmos;
        atmos.numblkscod = 3;
        atmos.bitrate_kbps = 448;
        atmos.emit_object_metadata = false;  // no key: 5.1 bed only, as the app does
        encoder = std::make_unique<ac3::oba::AtmosEncoder>(atmos, kObjectSlots);
        objects.assign(kObjectSlots, std::vector<float>(kFrames, 0.0F));
        views.resize(kObjectSlots);
        placements.resize(kObjectSlots);
        placements[0].position = {0.2, 0.8, 0.0};
        for (int bed = 0; bed < kBedSlots; ++bed) {
            placements[static_cast<std::size_t>(kPositionedSlots + bed)] =
                bed_placement(static_cast<BedChannel>(bed));
        }
        bed_views.resize(6);
    }

    RawFrame next() {
        for (std::size_t i = 0; i < kFrames; ++i) {
            const auto v = static_cast<float>(0.5 * std::sin(phase));
            objects[0][i] = v;
            for (int bed = 0; bed < kBedSlots; ++bed) {
                objects[static_cast<std::size_t>(kPositionedSlots + bed)][i] = v;
            }
            phase += 2.0 * std::numbers::pi * 440.0 / 48000.0;
        }
        for (std::size_t s = 0; s < objects.size(); ++s) {
            views[s] = objects[s];
        }
        auto encoded = encoder->encode_frame(views, placements);
        REQUIRE(encoded);
        unit = std::move(encoded->bytes);
        const auto bed = encoder->bed();
        for (std::size_t ch = 0; ch < 6 && ch < bed.size(); ++ch) {
            bed_views[ch] = bed[ch];
        }
        return RawFrame{.objects = views, .placements = placements, .bed = bed_views};
    }
};

OutputStageConfig config_over(const std::shared_ptr<FakeDevices>& devices,
                              std::optional<OutputMode> pinned = std::nullopt, bool bypass = false) {
    return {.devices = devices,
            .bypass_codec = bypass,
            .low_latency = false,
            .null_sink_substring = "Desktop Atmos",
            .pinned = pinned,
            .sample_rate = 48000,
            .ac3_bitrate_kbps = 448};
}

float rms(std::span<const float> samples) {
    double sum = 0.0;
    for (const float v : samples) {
        sum += static_cast<double>(v) * static_cast<double>(v);
    }
    return samples.empty() ? 0.0F : static_cast<float>(std::sqrt(sum / static_cast<double>(samples.size())));
}

}  // namespace

TEST_CASE("output stage: an E-AC-3 capable endpoint gets a burst sink and E-AC-3 bursts", "[crucible][output_stage]") {
    auto devices = std::make_shared<FakeDevices>();
    devices->devices = {realtek_default(), null_sink(), hdmi_avr()};
    OutputStage stage(config_over(devices));
    const auto& status = stage.reprobe(false);
    CHECK(status.mode == OutputMode::kDdPlus51);  // no key: DD+ 5.1, not Atmos
    CHECK(status.endpoint_id == "avr");
    CHECK(status.running);
    REQUIRE(devices->burst_sinks.size() == 1);
    CHECK(devices->burst_sinks[0]->started);
    CHECK(devices->burst_sinks[0]->eac3);
    CHECK(devices->burst_sinks[0]->device_id == "avr");

    Encoded encoded;
    for (int i = 0; i < 4; ++i) {
        const auto raw = encoded.next();
        stage.submit(encoded.unit, raw);
    }
    CHECK(stage.status().units_submitted == 4);
    // Every unit is packed into exactly one IEC 61937 burst of the E-AC-3 size.
    CHECK(devices->burst_sinks[0]->submits == 4);
    CHECK(devices->burst_sinks[0]->bytes == 4 * 24576);
    CHECK(stage.status().underruns == 0);
    CHECK_FALSE(stage.status().bypassed);
}

TEST_CASE("output stage: with a key the same endpoint carries Atmos", "[crucible][output_stage]") {
    auto devices = std::make_shared<FakeDevices>();
    devices->devices = {realtek_default(), null_sink(), hdmi_avr()};
    OutputStage stage(config_over(devices));
    CHECK(stage.reprobe(true).mode == OutputMode::kAtmos);
    CHECK(devices->burst_sinks.size() == 1);
}

TEST_CASE("output stage: an AC-3-only endpoint gets the DD 5.1 leg", "[crucible][output_stage]") {
    auto devices = std::make_shared<FakeDevices>();
    auto avr = hdmi_avr();
    avr.accepts_eac3 = false;
    devices->devices = {realtek_default(), null_sink(), avr};
    OutputStage stage(config_over(devices));
    CHECK(stage.reprobe(false).mode == OutputMode::kDd51);
    REQUIRE(devices->burst_sinks.size() == 1);
    CHECK_FALSE(devices->burst_sinks[0]->eac3);
    Encoded encoded;
    for (int i = 0; i < 3; ++i) {
        const auto raw = encoded.next();
        stage.submit(encoded.unit, raw);
    }
    // The bed is re-encoded as AC-3 and wrapped: one 6144-byte burst per frame.
    CHECK(devices->burst_sinks[0]->submits == 3);
    CHECK(devices->burst_sinks[0]->bytes == 3 * 6144);
}

TEST_CASE("output stage: a multichannel PCM endpoint gets decoded 5.1", "[crucible][output_stage]") {
    auto devices = std::make_shared<FakeDevices>();
    auto pcm = realtek_default();
    pcm.shared_channels = 6;
    pcm.name = "Speakers (5.1 card)";
    devices->devices = {null_sink(), pcm};
    OutputStage stage(config_over(devices));
    CHECK(stage.reprobe(false).mode == OutputMode::kPcmSurround);
    REQUIRE(devices->pcm_sinks.size() == 1);
    CHECK(devices->pcm_sinks[0]->channels == 6);
    Encoded encoded;
    for (int i = 0; i < 4; ++i) {
        const auto raw = encoded.next();
        stage.submit(encoded.unit, raw);
    }
    // The decoder holds one unit back (§3.7), so the first submit yields nothing.
    CHECK(devices->pcm_sinks[0]->submits >= 2);
    CHECK(devices->pcm_sinks[0]->last_pcm.size() == kFrames * 6);
    CHECK(rms(devices->pcm_sinks[0]->last_pcm) > 0.05F);
}

TEST_CASE("output stage: a stereo endpoint gets a Lo/Ro decode, and the bypass a bed fold", "[crucible][output_stage]") {
    auto devices = std::make_shared<FakeDevices>();
    devices->devices = {null_sink(), realtek_default()};
    OutputStage stage(config_over(devices));
    CHECK(stage.reprobe(false).mode == OutputMode::kStereo);
    REQUIRE(devices->pcm_sinks.size() == 1);
    CHECK(devices->pcm_sinks[0]->channels == 2);

    Encoded encoded;
    for (int i = 0; i < 4; ++i) {
        const auto raw = encoded.next();
        stage.submit(encoded.unit, raw);
    }
    const auto decoded_submits = devices->pcm_sinks[0]->submits;
    CHECK(decoded_submits >= 2);
    CHECK(devices->pcm_sinks[0]->last_pcm.size() == kFrames * 2);
    CHECK_FALSE(stage.status().bypassed);
    // The sink's queue depth is reported after each submit, for the
    // engine's catch-up rule.
    devices->pcm_sinks[0]->queued_frames = 4321;
    {
        const auto raw = encoded.next();
        stage.submit(encoded.unit, raw);
    }
    CHECK(stage.status().sink_queue_frames == 4321);

    stage.set_bypass(true);
    const auto raw = encoded.next();
    stage.submit(encoded.unit, raw);
    CHECK(stage.status().bypassed);
    CHECK(devices->pcm_sinks[0]->submits == decoded_submits + 2);  // the queue-depth submit above, then this
    // The raw fold of the encoder's bed: signal present, and the
    // normalisation keeps a bed carrying the tone on every channel under
    // full scale (a full-scale sine has an RMS of 0.707).
    const float folded = rms(devices->pcm_sinks[0]->last_pcm);
    CHECK(folded > 0.2F);
    CHECK(folded < 0.7F);
}

TEST_CASE("output stage: headphones render the engine's objects through the bypass", "[crucible][output_stage]") {
    auto devices = std::make_shared<FakeDevices>();
    devices->devices = {null_sink(), headphones_spatial()};
    OutputStage stage(config_over(devices, std::nullopt, true));
    // Headphones need a key in the policy's eyes (objects to render).
    CHECK(stage.reprobe(true).mode == OutputMode::kHeadphones);
    REQUIRE(devices->object_sinks.size() == 1);
    CHECK_FALSE(devices->object_sinks[0]->started);  // started on the first unit

    Encoded encoded;
    const auto raw = encoded.next();
    stage.submit(encoded.unit, raw);
    CHECK(devices->object_sinks[0]->started);
    CHECK(devices->object_sinks[0]->max_dynamic_objects == kObjectSlots);
    CHECK(devices->object_sinks[0]->static_channels == 0x8);  // the bed's LFE
    CHECK(devices->object_sinks[0]->submits == 1);
    CHECK(devices->object_sinks[0]->last_dynamic_objects == kObjectSlots);
    CHECK(devices->object_sinks[0]->last_static_objects == 1);
    // Object 0 was placed at x = 0.2: left of centre in listener metres.
    REQUIRE(devices->object_sinks[0]->last_object_x.size() == kObjectSlots);
    CHECK(devices->object_sinks[0]->last_object_x[0] < 0.0F);
}

TEST_CASE("output stage: a probe that changes the answer switches sinks mid-stream", "[crucible][output_stage]") {
    auto devices = std::make_shared<FakeDevices>();
    devices->devices = {null_sink(), realtek_default()};
    OutputStage stage(config_over(devices));
    CHECK(stage.reprobe(false).mode == OutputMode::kStereo);
    Encoded encoded;
    {
        const auto first = encoded.next();
        stage.submit(encoded.unit, first);
    }

    // The AVR arrives.
    devices->devices.push_back(hdmi_avr());
    const auto& status = stage.reprobe(false);
    CHECK(status.mode == OutputMode::kDdPlus51);
    CHECK(status.endpoint_id == "avr");
    REQUIRE(devices->pcm_sinks.size() == 1);
    CHECK(devices->pcm_sinks[0]->stopped);
    REQUIRE(devices->burst_sinks.size() == 1);
    CHECK(devices->burst_sinks[0]->started);
    const auto raw = encoded.next();
    stage.submit(encoded.unit, raw);
    CHECK(devices->burst_sinks[0]->submits == 1);

    // The same answer again is not a switch.
    stage.reprobe(false);
    CHECK(devices->burst_sinks.size() == 1);
    CHECK_FALSE(devices->burst_sinks[0]->stopped);

    // The AVR leaves: back to stereo on a fresh sink.
    devices->devices.pop_back();
    CHECK(stage.reprobe(false).mode == OutputMode::kStereo);
    CHECK(devices->burst_sinks[0]->stopped);
    CHECK(devices->pcm_sinks.size() == 2);
}

TEST_CASE("output stage: a sink that refuses to start leaves no output and a reason", "[crucible][output_stage]") {
    auto devices = std::make_shared<FakeDevices>();
    devices->devices = {null_sink(), hdmi_avr()};
    devices->refuse_next_start = true;
    OutputStage stage(config_over(devices));
    const auto& status = stage.reprobe(false);
    CHECK(status.mode == OutputMode::kNone);
    CHECK_FALSE(status.running);
    CHECK(status.reason.find("could not start") != std::string::npos);
    CHECK(status.reason.find("refused by the test") != std::string::npos);
    Encoded encoded;
    const auto raw = encoded.next();
    stage.submit(encoded.unit, raw);  // ignored, not fatal
    CHECK(stage.status().units_submitted == 0);
}

TEST_CASE("output stage: a full sink is waited on briefly, then counted as an underrun", "[crucible][output_stage]") {
    auto devices = std::make_shared<FakeDevices>();
    devices->devices = {null_sink(), hdmi_avr()};
    devices->refuse_next_submits = 1000000;  // never accepts
    OutputStage stage(config_over(devices));
    CHECK(stage.reprobe(false).mode == OutputMode::kDdPlus51);
    Encoded encoded;
    const auto raw = encoded.next();
    stage.submit(encoded.unit, raw);
    CHECK(stage.status().underruns == 1);
    CHECK(devices->burst_sinks[0]->submits == 0);
}

TEST_CASE("output stage: stop tears the sink down and submit is then a no-op", "[crucible][output_stage]") {
    auto devices = std::make_shared<FakeDevices>();
    devices->devices = {null_sink(), realtek_default()};
    OutputStage stage(config_over(devices));
    stage.reprobe(false);
    stage.stop();
    CHECK_FALSE(stage.status().running);
    CHECK(devices->pcm_sinks[0]->stopped);
    Encoded encoded;
    const auto raw = encoded.next();
    stage.submit(encoded.unit, raw);
    CHECK(stage.status().units_submitted == 0);
}

TEST_CASE("output stage: low-latency mode asks the PCM sink for its smallest period", "[crucible][output_stage]") {
    auto devices = std::make_shared<FakeDevices>();
    devices->devices = {null_sink(), realtek_default()};
    auto config = config_over(devices);
    config.low_latency = true;
    OutputStage stage(config);
    CHECK(stage.reprobe(false).mode == OutputMode::kStereo);
    REQUIRE(devices->pcm_sinks.size() == 1);
    CHECK(devices->pcm_sinks[0]->low_latency);
    // Bitstream sinks have no such knob; a normal-mode stage does not ask.
    OutputStage normal(config_over(devices));
    normal.reprobe(false);
    REQUIRE(devices->pcm_sinks.size() == 2);
    CHECK_FALSE(devices->pcm_sinks[1]->low_latency);
}
