#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ac3/oba/atmos.hpp"
#include "fake_devices.hpp"
#include "output_stage.hpp"
#include "slots.hpp"

// The output stage's less travelled routes, beside test_output_stage.cpp's
// main ones: a refusal to start on each of the decoded and DD legs, the
// setters a running engine reaches it through, the PCM-surround bypass, a
// raw frame too narrow to carry a bed, headphones decoding a unit that
// carries object metadata (the LFE through its delay line), a spatial sink
// that refuses to open or to take a submit, and a spatial sink that lost
// its device being rebuilt on the next probe.

using namespace ac3::crucible;
using namespace ac3::crucible::testing;

namespace {

constexpr std::size_t kFrames = 1536;

// test_output_stage.cpp's Encoded, with the object metadata switchable:
// the headphones decode only has objects to place when the unit carries it.
struct Encoder {
    std::unique_ptr<ac3::oba::AtmosEncoder> encoder;
    std::vector<std::vector<float>> objects;
    std::vector<std::span<const float>> views;
    std::vector<ac3::oba::ObjectPlacement> placements;
    std::vector<std::span<const float>> bed_views;
    std::vector<std::byte> unit;
    double phase = 0.0;

    explicit Encoder(bool object_metadata) {
        ac3::oba::AtmosConfig atmos;
        atmos.numblkscod = 3;
        atmos.bitrate_kbps = 448;
        atmos.emit_object_metadata = object_metadata;
        encoder = std::make_unique<ac3::oba::AtmosEncoder>(atmos, kObjectSlots);
        objects.assign(kObjectSlots, std::vector<float>(kFrames, 0.0F));
        views.resize(kObjectSlots);
        placements.resize(kObjectSlots);
        placements[0].position = {0.2, 0.8, 0.0};
        placements[0].gain = 1.0;
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

OutputStageConfig config_over(const std::shared_ptr<FakeDevices>& devices, bool bypass = false) {
    return {.devices = devices,
            .bypass_codec = bypass,
            .low_latency = false,
            .null_sink_substring = "Desktop Atmos",
            .pinned = std::nullopt,
            .sample_rate = 48000,
            .ac3_bitrate_kbps = 448};
}

DeviceFacts surround_card() {
    auto pcm = realtek_default();
    pcm.shared_channels = 6;
    pcm.name = "Speakers (5.1 card)";
    return pcm;
}

DeviceFacts ac3_only_avr() {
    auto avr = hdmi_avr();
    avr.accepts_eac3 = false;
    return avr;
}

bool mentions(const std::string& text, const std::string& what) {
    return text.find(what) != std::string::npos;
}

}  // namespace

TEST_CASE("output stage edges: each leg's sink refusing to start leaves no output and the sink's reason",
          "[crucible][output_stage]") {
    auto devices = std::make_shared<FakeDevices>();
    SECTION("DD 5.1") { devices->devices = {null_sink(), ac3_only_avr()}; }
    SECTION("PCM surround") { devices->devices = {null_sink(), surround_card()}; }
    SECTION("stereo") { devices->devices = {null_sink(), realtek_default()}; }
    devices->refuse_next_start = true;
    OutputStage stage(config_over(devices));
    const auto& status = stage.reprobe(false);
    CHECK(status.mode == OutputMode::kNone);
    CHECK_FALSE(status.running);
    CHECK(mentions(status.reason, "could not start: refused by the test"));
}

TEST_CASE("output stage edges: the pinned mode and the preferred endpoint take effect at the next probe",
          "[crucible][output_stage]") {
    auto devices = std::make_shared<FakeDevices>();
    devices->devices = {realtek_default(), null_sink(), hdmi_avr()};
    OutputStage stage(config_over(devices));
    CHECK(stage.reprobe(false).mode == OutputMode::kDdPlus51);

    stage.set_pinned(OutputMode::kDd51);
    CHECK(stage.status().mode == OutputMode::kDdPlus51);  // nothing moves until a probe
    const auto& pinned = stage.reprobe(false);
    CHECK(pinned.mode == OutputMode::kDd51);
    CHECK(pinned.endpoint_id == "avr");
    CHECK(mentions(pinned.reason, "pinned"));

    stage.set_pinned(std::nullopt);
    stage.set_preferred_endpoint("realtek");
    const auto& preferred = stage.reprobe(false);
    CHECK(preferred.endpoint_id == "realtek");
    CHECK(preferred.mode == OutputMode::kStereo);
}

TEST_CASE("output stage edges: the null-sink substring decides which endpoint is marked as the silent device",
          "[crucible][output_stage]") {
    auto devices = std::make_shared<FakeDevices>();
    devices->devices = {realtek_default(), null_sink()};
    OutputStage stage(config_over(devices));
    const auto marked = [&stage](const std::string& id) {
        for (const auto& e : stage.enumerate()) {
            if (e.id == id) {
                return e.is_null_sink;
            }
        }
        return false;
    };
    CHECK(marked("null"));
    CHECK_FALSE(marked("realtek"));

    stage.set_null_sink_substring("REALTEK");  // matched without regard to case
    CHECK(marked("realtek"));
    CHECK_FALSE(marked("null"));

    stage.set_null_sink_substring("");  // an empty needle marks nothing
    CHECK_FALSE(marked("realtek"));
    CHECK_FALSE(marked("null"));
}

TEST_CASE("output stage edges: the PCM-surround bypass plays the encoder's bed in WAVE channel order",
          "[crucible][output_stage]") {
    auto devices = std::make_shared<FakeDevices>();
    devices->devices = {null_sink(), surround_card()};
    OutputStage stage(config_over(devices, /*bypass=*/true));
    CHECK(stage.reprobe(false).mode == OutputMode::kPcmSurround);
    Encoder encoder(false);
    const auto raw = encoder.next();
    stage.submit(encoder.unit, raw);
    CHECK(stage.status().bypassed);
    REQUIRE(devices->pcm_sinks[0]->submits == 1);  // no decoder, so no unit held back
    const auto& pcm = devices->pcm_sinks[0]->last_pcm;
    REQUIRE(pcm.size() == kFrames * 6);
    // Coded L C R Ls Rs LFE becomes WAVE L R C LFE Ls Rs.
    constexpr std::size_t kCodedForWave[6] = {0, 2, 1, 5, 3, 4};
    for (const std::size_t i : {std::size_t{0}, std::size_t{100}, kFrames - 1}) {
        for (std::size_t w = 0; w < 6; ++w) {
            CHECK(pcm[i * 6 + w] == raw.bed[kCodedForWave[w]][i]);
        }
    }
}

TEST_CASE("output stage edges: a raw frame without a full bed is not played by a bypassed PCM sink",
          "[crucible][output_stage]") {
    auto devices = std::make_shared<FakeDevices>();
    devices->devices = {null_sink(), realtek_default()};
    OutputStage stage(config_over(devices, /*bypass=*/true));
    CHECK(stage.reprobe(false).mode == OutputMode::kStereo);
    Encoder encoder(false);
    auto raw = encoder.next();
    raw.bed = raw.bed.subspan(0, 2);
    stage.submit(encoder.unit, raw);
    CHECK(stage.status().units_submitted == 1);
    CHECK(devices->pcm_sinks[0]->submits == 0);
}

TEST_CASE("output stage edges: headphones decode a unit's objects and delay its LFE to meet them",
          "[crucible][output_stage]") {
    auto devices = std::make_shared<FakeDevices>();
    devices->devices = {null_sink(), headphones_spatial()};
    OutputStage stage(config_over(devices));
    CHECK(stage.reprobe(true).mode == OutputMode::kHeadphones);
    REQUIRE(devices->object_sinks.size() == 1);

    Encoder encoder(true);
    for (int i = 0; i < 6; ++i) {
        const auto raw = encoder.next();
        stage.submit(encoder.unit, raw);
    }
    CHECK_FALSE(stage.status().bypassed);
    const auto& sink = *devices->object_sinks[0];
    CHECK(sink.started);
    CHECK(sink.static_channels == 0x8);  // the program's LFE, as the one static channel
    CHECK(sink.submits >= 4);            // one unit is held back (§3.7)
    CHECK(sink.last_dynamic_objects > 0);
    CHECK(sink.last_static_objects == 1);
    CHECK(sink.samples > 0);
    CHECK(stage.status().underruns == 0);
}

TEST_CASE("output stage edges: a spatial sink that refuses to open leaves no output and says so",
          "[crucible][output_stage]") {
    auto devices = std::make_shared<FakeDevices>();
    devices->devices = {null_sink(), headphones_spatial()};
    bool bypass = false;
    SECTION("through the bypass") { bypass = true; }
    SECTION("through the decoder") { bypass = false; }
    OutputStage stage(config_over(devices, bypass));
    CHECK(stage.reprobe(true).mode == OutputMode::kHeadphones);
    REQUIRE(devices->object_sinks.size() == 1);
    devices->object_sinks[0]->refuse_start = true;

    Encoder encoder(true);
    for (int i = 0; i < 3; ++i) {
        const auto raw = encoder.next();
        stage.submit(encoder.unit, raw);
    }
    CHECK(stage.status().mode == OutputMode::kNone);
    CHECK_FALSE(stage.status().running);
    CHECK(mentions(stage.status().reason, "spatial sink refused: refused by the test"));
    CHECK(devices->object_sinks[0]->stopped);
    CHECK(devices->object_sinks[0]->submits == 0);
    // Only the unit that met the refusal counted; the rest found nothing running.
    CHECK(stage.status().units_submitted <= 2);
}

TEST_CASE("output stage edges: a spatial sink that stopped itself counts an underrun at once",
          "[crucible][output_stage]") {
    auto devices = std::make_shared<FakeDevices>();
    devices->devices = {null_sink(), headphones_spatial()};
    OutputStage stage(config_over(devices, /*bypass=*/true));
    CHECK(stage.reprobe(true).mode == OutputMode::kHeadphones);
    Encoder encoder(false);
    {
        const auto raw = encoder.next();
        stage.submit(encoder.unit, raw);
    }
    auto& sink = *devices->object_sinks[0];
    REQUIRE(sink.submits == 1);
    sink.running = false;
    sink.refuse_submits = 1000000;
    const auto raw = encoder.next();
    stage.submit(encoder.unit, raw);
    CHECK(stage.status().underruns == 1);
    CHECK(sink.submits == 1);
}

TEST_CASE("output stage edges: headphones whose spatial sink lost its device are rebuilt on the next probe",
          "[crucible][output_stage]") {
    auto devices = std::make_shared<FakeDevices>();
    devices->devices = {null_sink(), headphones_spatial()};
    OutputStage stage(config_over(devices, /*bypass=*/true));
    CHECK(stage.reprobe(true).mode == OutputMode::kHeadphones);
    Encoder encoder(false);
    const auto raw = encoder.next();
    stage.submit(encoder.unit, raw);
    REQUIRE(devices->object_sinks[0]->started);

    // Started and still running: the same answer is not a switch.
    stage.reprobe(true);
    CHECK(devices->object_sinks.size() == 1);

    devices->object_sinks[0]->running = false;
    const auto& status = stage.reprobe(true);
    CHECK(status.mode == OutputMode::kHeadphones);
    CHECK(status.running);
    CHECK(devices->object_sinks[0]->stopped);
    CHECK(devices->object_sinks.size() == 2);
}
