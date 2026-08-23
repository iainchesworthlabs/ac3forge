#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3/oba/motion.hpp"
#include "ac3/oba/oamd.hpp"
#include "ac3/oba/scene.hpp"

namespace {

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

// The keyframe file ac3cli has always read, with everything the grammar
// allows in it: comments to end of line, a comment-only line, a blank line,
// leading whitespace, and an object index the file never mentions (1) sitting
// between two it does.
constexpr std::string_view kLegacyFile =
    "# a scene\n"
    "0 0.0 0.10 0.20 0.00 0.50 0.0\n"
    "0 1.5 0.90 0.80 0.40 0.90 0.1   # arrives back right, high\n"
    "\n"
    "   0 3.0 0.50 0.50 -0.25 0.20 0.0\n"
    "2 0.25 0.30 0.70 0.10 0.75 0.05\n"
    "2 2.75 0.70 0.30 -0.10 0.25 0.00\n";

// A dense sweep across and beyond the authored range, so the ends-hold rule
// and every segment are exercised rather than just the midpoints.
std::vector<double> sample_times() {
    std::vector<double> times;
    for (int i = -20; i <= 400; ++i) {
        times.push_back(static_cast<double>(i) * 0.01);
    }
    return times;
}

bool exactly_equal(const ac3::oba::ObjectPlacement& a, const ac3::oba::ObjectPlacement& b) {
    return a.position.x == b.position.x && a.position.y == b.position.y &&
           a.position.z == b.position.z && a.gain == b.gain && a.lfe_send == b.lfe_send;
}

std::vector<float> tone(double hz, double amplitude, std::uint64_t start) {
    std::vector<float> out(ac3::kSamplesPerFrame);
    for (int n = 0; n < ac3::kSamplesPerFrame; ++n) {
        const double t = static_cast<double>(start + static_cast<std::uint64_t>(n)) / 48000.0;
        out[static_cast<std::size_t>(n)] =
            static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * hz * t));
    }
    return out;
}

ac3::oba::ObjectScene must_create(std::vector<ac3::oba::SceneObject> objects,
                                  const ac3::oba::Orientation& orientation = {}) {
    auto scene = ac3::oba::ObjectScene::create(std::move(objects), orientation);
    REQUIRE(scene.has_value());
    return std::move(*scene);
}

}  // namespace

// ---------------------------------------------------------------------------
// The migration guarantee. Everything else here is new behaviour; this is the
// part that says the existing one did not change.
// ---------------------------------------------------------------------------

TEST_CASE("a scene from a legacy keyframe file evaluates exactly as KeyframePath did",
          "[oba][scene]") {
    const auto parsed = ac3::oba::scene_objects_from_keyframe_text(kLegacyFile);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->size() == 3);
    // Object 1 is the gap: mentioned by nobody, present, empty.
    CHECK(parsed->at(0).automation.size() == 3);
    CHECK(parsed->at(1).automation.empty());
    CHECK(parsed->at(2).automation.size() == 2);

    // The same authored points through the layer that predates this type.
    std::vector<ac3::oba::ObjectPath> paths;
    for (const auto& object : *parsed) {
        if (object.automation.empty()) {
            continue;
        }
        std::vector<ac3::oba::Keyframe> keyframes;
        for (const auto& point : object.automation) {
            keyframes.push_back({.time_s = point.time_s,
                                 .position = point.position,
                                 .gain = point.gain,
                                 .lfe_send = point.lfe_send});
        }
        auto path = ac3::oba::KeyframePath::create(std::move(keyframes));
        REQUIRE(path.has_value());
        paths.emplace_back(std::move(*path));
    }
    REQUIRE(paths.size() == 2);

    std::vector<ac3::oba::SceneObject> objects{parsed->at(0), parsed->at(2)};
    const auto scene = must_create(std::move(objects));

    // Bit-for-bit, not approximately: a placement that differs in the last
    // ulp encodes to a different bed gain and eventually to different bytes.
    for (const double t : sample_times()) {
        for (std::size_t i = 0; i < paths.size(); ++i) {
            const auto before = paths[i].evaluate(t);
            const auto after = scene.evaluate(i, t);
            INFO("object " << i << " at t=" << t);
            CHECK(exactly_equal(before, after));
        }
    }
}

TEST_CASE("a scene drives an encode to byte-identical output", "[oba][scene]") {
    // Real audio over more than three frames: silence and a single frame both
    // pass whatever they are given (see docs/verification.md), so neither
    // would prove the placements reached the bitstream at all.
    constexpr int kObjects = 2;
    constexpr int kFrames = 6;

    const auto parsed = ac3::oba::scene_objects_from_keyframe_text(kLegacyFile);
    REQUIRE(parsed.has_value());

    std::vector<ac3::oba::ObjectPath> paths;
    for (const std::size_t index : {std::size_t{0}, std::size_t{2}}) {
        std::vector<ac3::oba::Keyframe> keyframes;
        for (const auto& point : parsed->at(index).automation) {
            keyframes.push_back({.time_s = point.time_s,
                                 .position = point.position,
                                 .gain = point.gain,
                                 .lfe_send = point.lfe_send});
        }
        auto path = ac3::oba::KeyframePath::create(std::move(keyframes));
        REQUIRE(path.has_value());
        paths.emplace_back(std::move(*path));
    }

    // The same file through the new reader, including a JSON save/load in the
    // middle - so this covers the whole migration a user would actually do,
    // not just the parser swap.
    const auto direct = must_create({parsed->at(0), parsed->at(2)});
    const auto reloaded = ac3::oba::scene_from_json(ac3::oba::to_json(direct));
    REQUIRE(reloaded.has_value());

    const auto encode = [&](auto&& placement_at) {
        ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 448}, kObjects};
        std::vector<std::byte> stream;
        std::uint64_t n0 = 0;
        for (int frame = 0; frame < kFrames; ++frame) {
            const std::vector<std::vector<float>> sources{tone(440.0, 0.30, n0),
                                                          tone(997.0, 0.22, n0)};
            const std::vector<std::span<const float>> views{sources[0], sources[1]};
            const double t =
                static_cast<double>(n0 + ac3::kSamplesPerFrame) / 48000.0;
            const auto placement = placement_at(t);
            const auto unit = encoder.encode_frame(views, placement);
            REQUIRE(unit.has_value());
            stream.insert(stream.end(), unit->bytes.begin(), unit->bytes.end());
            n0 += ac3::kSamplesPerFrame;
        }
        return stream;
    };

    const auto before = encode([&](double t) { return ac3::oba::evaluate_placements(paths, t); });
    const auto after = encode([&](double t) { return direct.evaluate(t); });
    const auto after_json = encode([&](double t) { return reloaded->evaluate(t); });

    REQUIRE(before.size() > 0);
    CHECK(before == after);
    CHECK(before == after_json);
}

// ---------------------------------------------------------------------------
// The keyframe grammar
// ---------------------------------------------------------------------------

TEST_CASE("the keyframe grammar keeps its shape", "[oba][scene]") {
    SECTION("a file with no newline at the end still yields its last line") {
        const auto parsed = ac3::oba::scene_objects_from_keyframe_text("0 1 2 3 4 5 6");
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->size() == 1);
        REQUIRE(parsed->at(0).automation.size() == 1);
        CHECK(parsed->at(0).automation[0].time_s == 1.0);
        CHECK(parsed->at(0).automation[0].lfe_send == 6.0);
    }

    SECTION("an empty file is an empty scene, not an error") {
        const auto parsed = ac3::oba::scene_objects_from_keyframe_text("");
        REQUIRE(parsed.has_value());
        CHECK(parsed->empty());
    }

    SECTION("comments and blank lines are skipped without consuming a line number") {
        const auto parsed = ac3::oba::scene_objects_from_keyframe_text("# one\n\n0 nope\n");
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().kind == ac3::oba::SceneErrorKind::kSyntax);
        CHECK(parsed.error().line == 3);
    }

    SECTION("a short line names the columns it wanted") {
        const auto parsed = ac3::oba::scene_objects_from_keyframe_text("0 1 2 3 4 5\n");
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().line == 1);
        CHECK(parsed.error().message == "expected 'object time_s x y z gain lfe_send'");
    }

    SECTION("a negative object index is refused rather than wrapping") {
        const auto parsed = ac3::oba::scene_objects_from_keyframe_text("-1 0 0 0 0 1 0\n");
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().kind == ac3::oba::SceneErrorKind::kSyntax);
    }

    SECTION("an absurd object index is refused rather than sizing a vector by it") {
        const auto parsed =
            ac3::oba::scene_objects_from_keyframe_text("999999999999 0 0 0 0 1 0\n");
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().kind == ac3::oba::SceneErrorKind::kBadValue);
        CHECK(parsed.error().line == 1);
    }

    SECTION("two points at one instant are refused, not silently ordered") {
        const auto parsed =
            ac3::oba::scene_objects_from_keyframe_text("0 1 0 0 0 1 0\n0 1 1 1 0 1 0\n");
        REQUIRE(parsed.has_value());
        const auto scene = ac3::oba::ObjectScene::create(*parsed);
        REQUIRE_FALSE(scene.has_value());
        CHECK(scene.error().kind == ac3::oba::SceneErrorKind::kDuplicateTime);
    }

    SECTION("keyframe text round-trips through a scene") {
        const auto parsed = ac3::oba::scene_objects_from_keyframe_text(kLegacyFile);
        REQUIRE(parsed.has_value());
        const auto scene = must_create({parsed->at(0), parsed->at(2)});
        const auto reparsed =
            ac3::oba::scene_objects_from_keyframe_text(ac3::oba::to_keyframe_text(scene));
        REQUIRE(reparsed.has_value());
        REQUIRE(reparsed->size() == 2);
        const auto again = must_create(*reparsed);
        for (const double t : sample_times()) {
            for (std::size_t i = 0; i < 2; ++i) {
                CHECK(exactly_equal(scene.evaluate(i, t), again.evaluate(i, t)));
            }
        }
    }
}

TEST_CASE("scene_from_text sniffs the two forms apart", "[oba][scene]") {
    const auto keyframes = ac3::oba::scene_from_text(kLegacyFile,
                                                     {.position = {.x = 0.25, .y = 0.75, .z = 0.5},
                                                      .gain = 0.125,
                                                      .lfe_send = 0.0625});
    REQUIRE(keyframes.has_value());
    REQUIRE(keyframes->object_count() == 3);
    // The gap took the caller's fallback, not a library-invented default.
    const auto gap = keyframes->evaluate(1, 7.0);
    CHECK(gap.position.x == 0.25);
    CHECK(gap.gain == 0.125);
    CHECK(gap.lfe_send == 0.0625);

    const auto json = ac3::oba::scene_from_text(ac3::oba::to_json(*keyframes));
    REQUIRE(json.has_value());
    CHECK(json->object_count() == 3);

    // Leading whitespace before the brace still reads as JSON.
    const auto padded = ac3::oba::scene_from_text("\n\n  " + ac3::oba::to_json(*keyframes));
    REQUIRE(padded.has_value());
}

// ---------------------------------------------------------------------------
// Interpolation and ramp semantics
// ---------------------------------------------------------------------------

TEST_CASE("each interpolation means what it says", "[oba][scene]") {
    using ac3::oba::Interpolation;
    const auto scene = must_create({
        {.name = "held",
         .automation = {{.time_s = 0.0,
                         .position = {.x = 0.0, .y = 0.0, .z = 0.0},
                         .gain = 0.25,
                         .interp = Interpolation::kHold},
                        {.time_s = 2.0,
                         .position = {.x = 1.0, .y = 1.0, .z = 1.0},
                         .gain = 0.75}}},
        {.name = "linear",
         .automation = {{.time_s = 0.0,
                         .position = {.x = 0.0, .y = 0.0, .z = 0.0},
                         .gain = 0.25,
                         .interp = Interpolation::kLinear},
                        {.time_s = 2.0,
                         .position = {.x = 1.0, .y = 1.0, .z = 1.0},
                         .gain = 0.75}}},
        {.name = "smooth",
         .automation = {{.time_s = 0.0,
                         .position = {.x = 0.0, .y = 0.0, .z = 0.0},
                         .gain = 0.25,
                         .interp = Interpolation::kSmooth},
                        {.time_s = 2.0,
                         .position = {.x = 1.0, .y = 1.0, .z = 1.0},
                         .gain = 0.75}}},
    });

    SECTION("hold steps at the next point rather than travelling to it") {
        CHECK(scene.evaluate(0, 1.0).position.x == 0.0);
        CHECK(scene.evaluate(0, 1.999).gain == 0.25);
        CHECK(scene.evaluate(0, 2.0).gain == 0.75);
    }

    SECTION("linear is the straight line, exactly") {
        CHECK(scene.evaluate(1, 1.0).position.x == 0.5);
        CHECK(scene.evaluate(1, 1.0).gain == 0.5);
        CHECK(scene.evaluate(1, 0.5).position.z == 0.25);
    }

    SECTION("smooth passes through the midpoint with zero slope at the ends") {
        // f(0.5) = 0.5 for smoothstep too, so the midpoint is the one place
        // the two agree - what differs is everywhere else.
        CHECK(scene.evaluate(2, 1.0).position.x == 0.5);
        CHECK(scene.evaluate(2, 0.5).position.x < scene.evaluate(1, 0.5).position.x);
        CHECK(scene.evaluate(2, 1.5).position.x > scene.evaluate(1, 1.5).position.x);
        // Near the start the shaped curve has barely moved: 0.05 of the way
        // along is 0.00725 of the distance, not 0.05 of it.
        CHECK_THAT(scene.evaluate(2, 0.1).position.x, WithinAbs(0.00725, 1e-12));
    }

    SECTION("both ends hold rather than extrapolating or muting") {
        for (std::size_t i = 0; i < 3; ++i) {
            CHECK(scene.evaluate(i, -100.0).gain == 0.25);
            CHECK(scene.evaluate(i, -100.0).position.x == 0.0);
            CHECK(scene.evaluate(i, 100.0).gain == 0.75);
            CHECK(scene.evaluate(i, 100.0).position.x == 1.0);
        }
    }

    SECTION("a one-point object never moves") {
        const auto still = must_create(
            {{.name = "still",
              .automation = {{.time_s = 4.0, .position = {.x = 0.3, .y = 0.6, .z = -0.2}}}}});
        for (const double t : {-5.0, 0.0, 4.0, 400.0}) {
            CHECK(still.evaluate(0, t).position.x == 0.3);
            CHECK(still.evaluate(0, t).position.z == -0.2);
        }
        CHECK(still.duration_s() == 4.0);
    }
}

TEST_CASE("a scene reports its own extent and refuses to be unusable", "[oba][scene]") {
    SECTION("duration is the last authored instant across every object") {
        const auto scene = must_create({
            {.automation = {{.time_s = 0.0}, {.time_s = 3.5}}},
            {.automation = {{.time_s = 9.25}}},
        });
        CHECK(scene.duration_s() == 9.25);
        CHECK(scene.object_count() == 2);
    }

    SECTION("an object with no automation is refused") {
        const auto scene = ac3::oba::ObjectScene::create({{.name = "silent one"}});
        REQUIRE_FALSE(scene.has_value());
        CHECK(scene.error().kind == ac3::oba::SceneErrorKind::kEmptyObject);
        // The diagnostic names the object, not just its index.
        CHECK(scene.error().message.find("silent one") != std::string::npos);
    }

    SECTION("a non-finite value is refused") {
        const auto scene = ac3::oba::ObjectScene::create(
            {{.automation = {{.time_s = std::nan(""), .position = {}}}}});
        REQUIRE_FALSE(scene.has_value());
        CHECK(scene.error().kind == ac3::oba::SceneErrorKind::kBadValue);
    }

    SECTION("an empty object list is a legal, empty scene") {
        const auto scene = must_create({});
        CHECK(scene.object_count() == 0);
        CHECK(scene.duration_s() == 0.0);
        CHECK(scene.evaluate(1.0).empty());
    }

    SECTION("an out-of-range index degrades rather than reading off the end") {
        const auto scene = must_create({{.automation = {{.time_s = 0.0}}}});
        const auto nothing = scene.evaluate(99, 1.0);
        CHECK(nothing.gain == 1.0);
        CHECK(nothing.position.x == 0.5);
    }

    SECTION("automation authored out of order is sorted, not rejected") {
        const auto scene = must_create({{.automation = {{.time_s = 2.0, .gain = 0.2},
                                                        {.time_s = 0.0, .gain = 0.8}}}});
        CHECK(scene.evaluate(0, 0.0).gain == 0.8);
        CHECK(scene.evaluate(0, 2.0).gain == 0.2);
    }
}

// ---------------------------------------------------------------------------
// Orientation
// ---------------------------------------------------------------------------

TEST_CASE("orientation turns the scene without rendering it", "[oba][scene]") {
    const ac3::oba::Position front{.x = 0.5, .y = 0.0, .z = 0.0};

    SECTION("an all-zero orientation is an exact no-op") {
        const ac3::oba::Position odd{.x = 0.1234567890123, .y = 0.98765432109, .z = -0.33333};
        const auto turned = ac3::oba::rotate(odd, {});
        CHECK(turned.x == odd.x);
        CHECK(turned.y == odd.y);
        CHECK(turned.z == odd.z);
    }

    SECTION("a quarter turn sends the front wall to the right wall") {
        const auto turned = ac3::oba::rotate(front, ac3::oba::orientation_from_degrees(90, 0, 0));
        CHECK_THAT(turned.x, WithinAbs(1.0, 1e-12));
        CHECK_THAT(turned.y, WithinAbs(0.5, 1e-12));
        CHECK_THAT(turned.z, WithinAbs(0.0, 1e-12));
    }

    SECTION("a half turn sends the front wall to the back") {
        const auto turned = ac3::oba::rotate(front, ac3::oba::orientation_from_degrees(180, 0, 0));
        CHECK_THAT(turned.x, WithinAbs(0.5, 1e-12));
        CHECK_THAT(turned.y, WithinAbs(1.0, 1e-12));
    }

    SECTION("a full turn comes back to where it started") {
        const ac3::oba::Position p{.x = 0.2, .y = 0.9, .z = 0.4};
        const auto turned = ac3::oba::rotate(p, ac3::oba::orientation_from_degrees(360, 0, 0));
        CHECK_THAT(turned.x, WithinAbs(p.x, 1e-12));
        CHECK_THAT(turned.y, WithinAbs(p.y, 1e-12));
        CHECK_THAT(turned.z, WithinAbs(p.z, 1e-12));
    }

    SECTION("yaw leaves height alone") {
        const ac3::oba::Position high{.x = 0.5, .y = 0.0, .z = 0.8};
        CHECK(ac3::oba::rotate(high, ac3::oba::orientation_from_degrees(37, 0, 0)).z == 0.8);
    }

    SECTION("pitch raises the front, roll raises the right") {
        const auto pitched =
            ac3::oba::rotate(front, ac3::oba::orientation_from_degrees(0, 90, 0));
        CHECK_THAT(pitched.z, WithinAbs(1.0, 1e-12));
        CHECK_THAT(pitched.y, WithinAbs(0.5, 1e-12));

        const ac3::oba::Position right{.x = 1.0, .y = 0.5, .z = 0.0};
        const auto rolled = ac3::oba::rotate(right, ac3::oba::orientation_from_degrees(0, 0, 90));
        CHECK_THAT(rolled.z, WithinAbs(1.0, 1e-12));
        CHECK_THAT(rolled.x, WithinAbs(0.5, 1e-12));
    }

    SECTION("a rotation out of the room clamps to the room, it does not leave it") {
        // A front-wall corner at full height, tipped nose-up: the rotated
        // point wants to be above the ceiling.
        const ac3::oba::Position corner{.x = 0.0, .y = 0.0, .z = 0.9};
        const auto turned =
            ac3::oba::rotate(corner, ac3::oba::orientation_from_degrees(0, 80, 0));
        CHECK(turned.z <= 1.0);
        CHECK(turned.z >= -1.0);
        CHECK(turned.x >= 0.0);
        CHECK(turned.y <= 1.0);
    }

    SECTION("a scene's orientation reaches its evaluated placements") {
        auto scene = must_create({{.name = "front",
                                   .automation = {{.time_s = 0.0,
                                                   .position = {.x = 0.5, .y = 0.0, .z = 0.0}}}}});
        CHECK(scene.evaluate(0, 0.0).position.y == 0.0);
        scene.set_orientation(ac3::oba::orientation_from_degrees(90, 0, 0));
        const auto turned = scene.evaluate(0, 0.0);
        CHECK_THAT(turned.position.x, WithinAbs(1.0, 1e-12));
        // Gain is a level, not a coordinate: turning the room does not touch it.
        CHECK(turned.gain == 1.0);
    }
}

// ---------------------------------------------------------------------------
// The live seam
// ---------------------------------------------------------------------------

TEST_CASE("a cursor overlays live updates on the authored timeline", "[oba][scene]") {
    ac3::oba::SceneCursor cursor{must_create({
        {.name = "a", .automation = {{.time_s = 0.0, .position = {.x = 0.0}, .gain = 0.4}}},
        {.name = "b", .automation = {{.time_s = 0.0, .position = {.x = 1.0}, .gain = 0.6}}},
    })};

    CHECK_FALSE(cursor.is_live(0));
    CHECK(cursor.sample(0.0)[0].position.x == 0.0);

    REQUIRE(cursor.push({.object = 0,
                         .placement = {.position = {.x = 0.75, .y = 0.25, .z = 0.5},
                                       .gain = 0.9,
                                       .lfe_send = 0.1}}));
    CHECK(cursor.is_live(0));
    const auto live = cursor.sample(12.0);
    CHECK(live[0].position.x == 0.75);
    CHECK(live[0].gain == 0.9);
    // The object nobody is driving still follows the timeline.
    CHECK(live[1].position.x == 1.0);
    CHECK(live[1].gain == 0.6);

    SECTION("latest value wins, with nothing invented in between") {
        REQUIRE(cursor.push({.object = 0, .placement = {.position = {.x = 0.1}, .gain = 0.2}}));
        CHECK(cursor.sample(0.0)[0].position.x == 0.1);
        CHECK(cursor.sample(0.0)[0].gain == 0.2);
    }

    SECTION("release hands the object back to its automation") {
        cursor.release(0);
        CHECK_FALSE(cursor.is_live(0));
        CHECK(cursor.sample(5.0)[0].position.x == 0.0);
        CHECK(cursor.sample(5.0)[0].gain == 0.4);
    }

    SECTION("release_all hands everything back") {
        REQUIRE(cursor.push({.object = 1, .placement = {.gain = 0.05}}));
        cursor.release_all();
        CHECK_FALSE(cursor.is_live(0));
        CHECK_FALSE(cursor.is_live(1));
    }

    SECTION("an object the scene does not have is refused, not fatal") {
        CHECK_FALSE(cursor.push({.object = 99, .placement = {}}));
        CHECK_FALSE(cursor.is_live(99));
        CHECK(cursor.sample(0.0).size() == 2);
    }

    SECTION("the scene's orientation turns live objects too") {
        cursor.scene().set_orientation(ac3::oba::orientation_from_degrees(90, 0, 0));
        REQUIRE(cursor.push({.object = 0, .placement = {.position = {.x = 0.5, .y = 0.0}}}));
        const auto turned = cursor.sample(0.0);
        CHECK_THAT(turned[0].position.x, WithinAbs(1.0, 1e-12));
        CHECK_THAT(turned[0].position.y, WithinAbs(0.5, 1e-12));
    }
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

TEST_CASE("a scene round-trips through JSON bit-exactly", "[oba][scene]") {
    using ac3::oba::Interpolation;
    const auto scene = must_create(
        {
            {.name = "broadcast",
             .bed = ac3::oba::bed::kC,
             .automation = {{.time_s = 0.0,
                             .position = {.x = 0.5, .y = 0.04, .z = 0.05},
                             .gain = 0.0,
                             .lfe_send = 0.0,
                             .interp = Interpolation::kSmooth},
                            {.time_s = 2.0,
                             .position = {.x = 0.123456789012345, .y = 1.0 / 3.0, .z = -0.75},
                             .gain = 0.30000000000000004,
                             .lfe_send = 0.0625,
                             .interp = Interpolation::kHold}}},
            {.name = "comet \"tail\"\n",
             .bed = ac3::oba::bed::kLR | ac3::oba::bed::kTflTfr,
             .automation = {{.time_s = 1e-9, .position = {.x = 1.0, .y = 0.0, .z = 1.0}}}},
        },
        ac3::oba::orientation_from_degrees(37.5, -12.25, 4.0));

    const auto text = ac3::oba::to_json(scene);
    const auto back = ac3::oba::scene_from_json(text);
    REQUIRE(back.has_value());

    REQUIRE(back->object_count() == scene.object_count());
    for (std::size_t i = 0; i < scene.object_count(); ++i) {
        const auto& a = scene.objects()[i];
        const auto& b = back->objects()[i];
        INFO("object " << i);
        CHECK(a.name == b.name);
        CHECK(a.bed == b.bed);
        REQUIRE(a.automation.size() == b.automation.size());
        for (std::size_t k = 0; k < a.automation.size(); ++k) {
            CHECK(a.automation[k].time_s == b.automation[k].time_s);
            CHECK(a.automation[k].position.x == b.automation[k].position.x);
            CHECK(a.automation[k].position.y == b.automation[k].position.y);
            CHECK(a.automation[k].position.z == b.automation[k].position.z);
            CHECK(a.automation[k].gain == b.automation[k].gain);
            CHECK(a.automation[k].lfe_send == b.automation[k].lfe_send);
            CHECK(a.automation[k].interp == b.automation[k].interp);
        }
    }
    CHECK(back->orientation().yaw_rad == scene.orientation().yaw_rad);
    CHECK(back->orientation().pitch_rad == scene.orientation().pitch_rad);
    CHECK(back->orientation().roll_rad == scene.orientation().roll_rad);

    // Writing what was read gives the same text: the format has one spelling
    // per scene, so a file under version control shows edits, not churn.
    CHECK(ac3::oba::to_json(*back) == text);
}

TEST_CASE("the JSON reader accepts what it should", "[oba][scene]") {
    SECTION("a minimal scene needs only the version, one object and its points") {
        const auto scene = ac3::oba::scene_from_json(
            R"({"ac3forge_scene":1,"objects":[{"automation":[{"t":0,"x":0.5,"y":0.5,"z":0}]}]})");
        REQUIRE(scene.has_value());
        CHECK(scene->object_count() == 1);
        CHECK(scene->objects()[0].name.empty());
        CHECK(scene->objects()[0].bed == 0);
        CHECK(scene->objects()[0].automation[0].gain == 1.0);
        CHECK(scene->objects()[0].automation[0].interp == ac3::oba::Interpolation::kLinear);
    }

    SECTION("an orientation may be given in degrees instead of radians") {
        const auto scene = ac3::oba::scene_from_json(
            R"({"ac3forge_scene":1,"orientation":{"yaw_deg":90},)"
            R"("objects":[{"automation":[{"t":0,"x":0.5,"y":0,"z":0}]}]})");
        REQUIRE(scene.has_value());
        CHECK_THAT(scene->orientation().yaw_rad, WithinRel(std::numbers::pi / 2.0, 1e-15));
        CHECK_THAT(scene->evaluate(0, 0.0).position.x, WithinAbs(1.0, 1e-12));
    }

    SECTION("bed labels name TS 103 420 Table 12's channels") {
        const auto scene = ac3::oba::scene_from_json(
            R"({"ac3forge_scene":1,"objects":[{"bed":["lr","c","lfe","ls_rs"],)"
            R"("automation":[{"t":0,"x":0.5,"y":0.5,"z":0}]}]})");
        REQUIRE(scene.has_value());
        CHECK(scene->objects()[0].bed == ac3::oba::bed::k51);
    }

    SECTION("string escapes and numeric forms are read as JSON defines them") {
        const auto scene = ac3::oba::scene_from_json(
            R"({"ac3forge_scene":1,"objects":[{"name":"aé\t\"b\"",)"
            R"("automation":[{"t":1e-3,"x":5E-1,"y":-0.0,"z":0,"gain":2}]}]})");
        REQUIRE(scene.has_value());
        CHECK(scene->objects()[0].name == "aé\t\"b\"");
        CHECK(scene->objects()[0].automation[0].time_s == 0.001);
        CHECK(scene->objects()[0].automation[0].position.x == 0.5);
        CHECK(scene->objects()[0].automation[0].gain == 2.0);
    }

    SECTION("an empty objects array is a legal empty scene") {
        const auto scene = ac3::oba::scene_from_json(R"({"ac3forge_scene":1,"objects":[]})");
        REQUIRE(scene.has_value());
        CHECK(scene->object_count() == 0);
    }
}

TEST_CASE("the JSON reader refuses what it should", "[oba][scene]") {
    const auto refuses = [](std::string_view text, ac3::oba::SceneErrorKind kind) {
        const auto scene = ac3::oba::scene_from_json(text);
        REQUIRE_FALSE(scene.has_value());
        INFO(scene.error().message);
        CHECK(scene.error().kind == kind);
    };
    using ac3::oba::SceneErrorKind;

    refuses("", SceneErrorKind::kSyntax);
    refuses("[]", SceneErrorKind::kSyntax);
    refuses(R"({"objects":[]})", SceneErrorKind::kBadField);
    refuses(R"({"ac3forge_scene":1})", SceneErrorKind::kBadField);
    refuses(R"({"ac3forge_scene":2,"objects":[]})", SceneErrorKind::kBadValue);
    refuses(R"({"ac3forge_scene":1,"objects":[]} trailing)", SceneErrorKind::kSyntax);
    refuses(R"({"ac3forge_scene":1,"objects":[],"nonsense":1})", SceneErrorKind::kBadField);
    // A misspelled member is an error, not a silent default - the whole reason
    // the reader is strict.
    refuses(R"({"ac3forge_scene":1,"objects":[{"automation":[{"t":0,"x":0,"y":0,"z":0,"gian":2}]}]})",
            SceneErrorKind::kBadField);
    refuses(R"({"ac3forge_scene":1,"objects":[{"automation":[{"t":0,"x":0,"y":0}]}]})",
            SceneErrorKind::kBadField);
    refuses(R"({"ac3forge_scene":1,"objects":[{"name":"x"}]})", SceneErrorKind::kBadField);
    refuses(R"({"ac3forge_scene":1,"objects":[{"automation":[]}]})",
            SceneErrorKind::kEmptyObject);
    refuses(
        R"({"ac3forge_scene":1,"objects":[{"automation":[{"t":0,"x":0,"y":0,"z":0,"interp":"ease"}]}]})",
        SceneErrorKind::kBadValue);
    refuses(R"({"ac3forge_scene":1,"objects":[{"bed":["middle"],)"
            R"("automation":[{"t":0,"x":0,"y":0,"z":0}]}]})",
            SceneErrorKind::kBadValue);
    refuses(R"({"ac3forge_scene":1,"orientation":{"yaw_rad":0,"yaw_deg":90},"objects":[]})",
            SceneErrorKind::kBadField);
    refuses(R"({"ac3forge_scene":1,"orientation":{"tilt":1},"objects":[]})",
            SceneErrorKind::kBadField);
    refuses(R"({"ac3forge_scene":1,"objects":[{"automation":[{"t":0,"x":0,"y":0,"z":nan}]}]})",
            SceneErrorKind::kSyntax);
    refuses(R"({"ac3forge_scene":1,"objects":[{"name":"unterminated})", SceneErrorKind::kSyntax);
    refuses(R"({"ac3forge_scene":1,"objects":[{"automation":[{"t":0,"x":0,"y":0,"z":0}]}])",
            SceneErrorKind::kSyntax);

    SECTION("an error points at the line it is on") {
        const auto scene = ac3::oba::scene_from_json(
            "{\n  \"ac3forge_scene\": 1,\n  \"objects\": [\n    { \"wrong\": 1 }\n  ]\n}\n");
        REQUIRE_FALSE(scene.has_value());
        CHECK(scene.error().line == 4);
    }
}
