#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "iclforge/ac3/encoder/assignment.hpp"
#include "iclforge/objects/scene.hpp"
#include "iclforge/ac4/ac4.hpp"
#include "ac4_encode_core.hpp"
#include "ac4_objects_core.hpp"
#include "iclforge/ac4dec/decoder.hpp"
#include "iclforge/ac4enc/encoder.hpp"

// The steps ac3cli's `atmos-encode codec=ac4` and ac3gui's AC-4 objects share
// (apps/common/ac4_objects_core.hpp): which channels are which objects, where a
// pinned channel sits, the audio each object carries, and the writer call.
// tests/cli/test_cli_atmos_encode_ac4.cpp holds the command to these, and the
// Qt Quick suite (tst_e2e_ac4_objects.qml) the page.

using iclforge::apps::Ac4ObjectSlot;
using iclforge::plan::Assignment;
using iclforge::plan::Destination;
using iclforge::plan::DestinationKind;
using iclforge::plan::SourceShape;
using Location = iclforge::eac3::chanmap::Location;

namespace {

Destination obj(double trim_db = 0.0) {
    return {.kind = DestinationKind::kObject, .trim_db = trim_db};
}
Destination objm() {
    return {.kind = DestinationKind::kObjectMono};
}
Destination at(Location location, double trim_db = 0.0) {
    return {.kind = DestinationKind::kLocation, .location = location, .trim_db = trim_db};
}

constexpr std::uint32_t kRate = 48000;

std::vector<float> tone(double hz, std::size_t count, double amplitude) {
    std::vector<float> x(count);
    for (std::size_t n = 0; n < count; ++n) {
        x[n] = static_cast<float>(
            amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(n) / kRate));
    }
    return x;
}

}  // namespace

TEST_CASE("AC-4 object slots list the dynamic objects, then the speakers, then the LFE",
          "[gui][ac4]") {
    // Source 0 has three channels, source 1 two.
    const std::vector<SourceShape> shapes{{.channels = 3, .label = "a"},
                                          {.channels = 2, .label = "b"}};
    Assignment assignment;
    assignment.set(0, 0, objm());
    assignment.set(0, 1, objm());
    assignment.set(0, 2, obj(-6.0));
    assignment.set(1, 0, at(Location::kLeft));
    assignment.set(1, 1, at(Location::kLfe, 3.0));
    const auto slots = iclforge::apps::ac4_object_slots(assignment, shapes);
    REQUIRE(slots.size() == 4);

    // atmos-encode's own order: every obj row, then each objm run folded to one.
    CHECK(slots[0].kind == Ac4ObjectSlot::Kind::kDynamic);
    REQUIRE(slots[0].taps.size() == 1);
    CHECK(slots[0].taps[0].first == 2);
    CHECK(slots[0].taps[0].second == Catch::Approx(std::pow(10.0, -6.0 / 20.0)));
    CHECK(slots[1].kind == Ac4ObjectSlot::Kind::kDynamic);
    REQUIRE(slots[1].taps.size() == 2);
    CHECK(slots[1].taps[0] == std::pair<std::size_t, double>{0, 0.5});
    CHECK(slots[1].taps[1] == std::pair<std::size_t, double>{1, 0.5});
    // A channel sent to a speaker is held at its azimuth; an LFE has none, and comes last.
    CHECK(slots[2].kind == Ac4ObjectSlot::Kind::kPinned);
    CHECK(slots[2].azimuth_deg == 30.0);
    REQUIRE(slots[2].taps.size() == 1);
    CHECK(slots[2].taps[0].first == 3);
    CHECK(slots[3].kind == Ac4ObjectSlot::Kind::kLfe);
    REQUIRE(slots[3].taps.size() == 1);
    CHECK(slots[3].taps[0].first == 4);
    CHECK(slots[3].taps[0].second == Catch::Approx(std::pow(10.0, 3.0 / 20.0)));
}

TEST_CASE("a channel pinned to a speaker sits where ADM puts the speaker", "[gui][ac4]") {
    // tests/ac3/oba/test_atmos_motion.cpp's ring constants, which iclforge::admbridge's polar
    // conversion is checked against: L at +30 degrees, SR at -110.
    const auto left = iclforge::apps::ac4_pin_position(30.0);
    CHECK(left.x == Catch::Approx(0.25).margin(1e-6));
    CHECK(left.y == Catch::Approx(0.066987).margin(1e-6));
    CHECK(left.z == 0.0);
    const auto rear = iclforge::apps::ac4_pin_position(-110.0);
    CHECK(rear.x == Catch::Approx(0.969846).margin(1e-6));
    CHECK(rear.y == Catch::Approx(0.671010).margin(1e-6));
    const auto centre = iclforge::apps::ac4_pin_position(0.0);
    CHECK(centre.x == Catch::Approx(0.5).margin(1e-9));
    CHECK(centre.y == Catch::Approx(0.0).margin(1e-9));
}

TEST_CASE("AC-4 flat planes offset each source and pad every channel to the longest",
          "[gui][ac4]") {
    const std::vector<std::vector<float>> first{{1.0F, 2.0F, 3.0F}, {4.0F, 5.0F, 6.0F}};
    const std::vector<std::vector<float>> second{{7.0F, 8.0F}};
    const std::vector<iclforge::apps::Ac4SourceView> sources{{.channels = first, .offset_samples = 0},
                                                        {.channels = second, .offset_samples = 3}};
    const auto planes = iclforge::apps::ac4_flat_planes(sources);
    REQUIRE(planes.size() == 3);
    // The second source ends at 3 + 2, past the first's three samples: zeros, not a held value.
    CHECK(planes[0] == std::vector<float>{1.0F, 2.0F, 3.0F, 0.0F, 0.0F});
    CHECK(planes[1] == std::vector<float>{4.0F, 5.0F, 6.0F, 0.0F, 0.0F});
    CHECK(planes[2] == std::vector<float>{0.0F, 0.0F, 0.0F, 7.0F, 8.0F});
    CHECK(iclforge::apps::ac4_flat_planes({}).empty());
}

TEST_CASE("AC-4 object planes sum each slot's taps at their gains", "[gui][ac4]") {
    const std::vector<std::vector<float>> flat{{1.0F, 2.0F}, {3.0F, 4.0F}, {5.0F, 6.0F}};
    std::vector<Ac4ObjectSlot> slots(2);
    slots[0].taps = {{0, 0.5}, {1, 0.5}};
    slots[1].taps = {{2, 2.0}, {7, 1.0}};  // a tap past the channels contributes nothing
    const auto planes = iclforge::apps::ac4_object_planes(slots, flat);
    REQUIRE(planes.size() == 2);
    CHECK(planes[0] == std::vector<float>{2.0F, 3.0F});
    CHECK(planes[1] == std::vector<float>{10.0F, 12.0F});
}

TEST_CASE("an AC-4 object keeps its position and turns its linear gain into dB", "[gui][ac4]") {
    iclforge::oba::ObjectPlacement placement;
    placement.position = {.x = 0.2, .y = 0.7, .z = -0.5};
    placement.gain = 0.5;
    placement.lfe_send = 1.0;
    const auto properties = iclforge::apps::ac4_object_properties(placement);
    CHECK(properties.position[0] == 0.2);
    CHECK(properties.position[1] == 0.7);
    CHECK(properties.position[2] == -0.5);
    CHECK(properties.gain_db == Catch::Approx(20.0 * std::log10(0.5)));
    placement.gain = 0.0;
    const auto silent = iclforge::apps::ac4_object_properties(placement);
    CHECK(std::isinf(silent.gain_db));
    CHECK(silent.gain_db < 0.0);
}

TEST_CASE("an AC-4 object encode is refused by count, LFE and rate before it reads audio",
          "[gui][ac4]") {
    const auto dynamic = [](std::size_t n) {
        return std::vector<Ac4ObjectSlot>(n, Ac4ObjectSlot{});
    };
    iclforge::apps::Ac4ObjectsParams params;
    CHECK_FALSE(iclforge::apps::ac4_objects_refusal(dynamic(1), params).has_value());
    CHECK_FALSE(iclforge::apps::ac4_objects_refusal(dynamic(64), params).has_value());
    const auto many = iclforge::apps::ac4_objects_refusal(dynamic(65), params);
    REQUIRE(many.has_value());
    CHECK(many->find("64 at most") != std::string::npos);

    const auto lfe_slot = [] {
        Ac4ObjectSlot slot;
        slot.kind = Ac4ObjectSlot::Kind::kLfe;
        return slot;
    };
    auto with_lfes = dynamic(1);
    with_lfes.push_back(lfe_slot());
    CHECK_FALSE(iclforge::apps::ac4_objects_refusal(with_lfes, params).has_value());
    with_lfes.push_back(lfe_slot());
    const auto lfes = iclforge::apps::ac4_objects_refusal(with_lfes, params);
    REQUIRE(lfes.has_value());
    CHECK(lfes->find("one LFE object") != std::string::npos);

    const std::vector<Ac4ObjectSlot> lfe_alone{lfe_slot()};
    CHECK(iclforge::apps::ac4_objects_refusal(lfe_alone, params).has_value());
    CHECK(iclforge::apps::ac4_objects_refusal({}, params).has_value());

    params.sample_rate_hz = 32000;
    const auto rate = iclforge::apps::ac4_objects_refusal(dynamic(2), params);
    REQUIRE(rate.has_value());
    CHECK(rate->find("48 or 44.1 kHz") != std::string::npos);
}

TEST_CASE("the encoder takes the 64 objects the page and the command allow, and no more",
          "[gui][ac4]") {
    // The limit iclforge::apps::kAc4MaxObjects states is the writer's own: if the writer moves it,
    // this fails and the constant, the page's text and the command's help move with it.
    const auto config_of = [](std::size_t n) {
        iclforge::apps::Ac4ObjectsParams params;
        params.bitrate_kbps = 512;
        const std::vector<iclforge::oba::ObjectPlacement> placements(n);
        return iclforge::apps::ac4_objects_config(params, std::vector<bool>{}, placements);
    };
    CHECK(iclforge::apps::kAc4MaxObjects == 64);
    CHECK(iclforge::ac4::Encoder::refusal_reason(config_of(64)).empty());
    CHECK_FALSE(iclforge::ac4::Encoder::refusal_reason(config_of(65)).empty());
    // The one frame rate an object stream is written at.
    auto config = config_of(2);
    CHECK(config.frame_rate_index == iclforge::apps::kAc4ObjectFrameRateIndex);
    config.frame_rate_index = 2;
    CHECK_FALSE(iclforge::ac4::Encoder::refusal_reason(config).empty());
}

TEST_CASE("a scene of slots encodes to a stream that decodes to its objects, one update a frame",
          "[gui][ac4]") {
    // The ADM fixture's shape (tests/cli/test_cli_atmos_adm.cpp): two channels held at the
    // speakers' places and one dynamic object held at the rear right for 0.096 s, then at the
    // front. Three tones, 0.192 s.
    constexpr std::size_t kSamples = 6 * 1536;
    const std::vector<std::vector<float>> flat{
        tone(300.0, kSamples, 0.3), tone(500.0, kSamples, 0.3), tone(800.0, kSamples, 0.3)};
    const std::vector<SourceShape> shapes{{.channels = 3, .label = "fixture"}};
    Assignment assignment;
    assignment.set(0, 0, at(Location::kLeft));
    assignment.set(0, 1, at(Location::kRight));
    assignment.set(0, 2, obj());
    const auto slots = iclforge::apps::ac4_object_slots(assignment, shapes);
    REQUIRE(slots.size() == 3);  // the dynamic object first, then the two pins

    const auto rear = iclforge::apps::ac4_pin_position(-110.0);
    const auto front = iclforge::apps::ac4_pin_position(0.0);
    const auto scene = iclforge::oba::ObjectScene::create(
        {iclforge::oba::SceneObject{.name = "moving",
                               .automation = {{.time_s = 0.0,
                                               .position = rear,
                                               .gain = 1.0,
                                               .interp = iclforge::oba::Interpolation::kHold},
                                              {.time_s = 0.096,
                                               .position = front,
                                               .gain = 1.0,
                                               .interp = iclforge::oba::Interpolation::kHold}}}});
    REQUIRE(scene.has_value());

    iclforge::apps::Ac4ObjectsParams params;
    params.bitrate_kbps = 256;
    const auto encoded = iclforge::apps::encode_ac4_scene(params, slots, flat, *scene);
    REQUIRE(encoded.has_value());
    CHECK(encoded->lag_samples == 3072 + 1313);
    REQUIRE_FALSE(encoded->frames.empty());

    iclforge::ac4::Decoder decoder;
    std::vector<std::array<double, 3>> moving;  // the dynamic object's position, frame by frame
    std::size_t object_count = 0;
    for (const iclforge::ac4::EncodedFrame& frame : encoded->frames) {
        const auto decoded = decoder.decode(frame.raw_ac4_frame);
        REQUIRE(decoded.has_value());
        if (!decoded->has_value()) {
            continue;
        }
        const iclforge::ac4::DecodedFrame& pcm = **decoded;
        object_count = std::max(object_count, pcm.objects.size());
        if (!pcm.objects.empty()) {
            moving.push_back(pcm.objects.front().properties.position);
        }
    }
    CHECK(object_count == 3);
    REQUIRE(moving.size() >= 5);
    // Early frames hold the rear right, the last the front: the jump is in the stream.
    CHECK(moving[1][0] == Catch::Approx(rear.x).margin(0.06));
    CHECK(moving[1][1] == Catch::Approx(rear.y).margin(0.06));
    CHECK(moving.back()[0] == Catch::Approx(front.x).margin(0.06));
    CHECK(moving.back()[1] == Catch::Approx(front.y).margin(0.06));
}
