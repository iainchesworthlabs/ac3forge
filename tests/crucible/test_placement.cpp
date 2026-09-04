#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "placement.hpp"

using ac3::crucible::kObjectSlots;
using ac3::crucible::kPositionedSlots;
using ac3::crucible::PlacementSmoother;
using ac3::crucible::PlacementTarget;
using Catch::Approx;

TEST_CASE("a positioned slot glides to its target and settles exactly", "[crucible]") {
    PlacementSmoother smoother(3.0);
    std::vector<ac3::oba::ObjectPlacement> out(kObjectSlots);
    smoother.set_target(0, {.position = {1.0, 0.0, 0.5}, .gain = 1.0});

    smoother.step(out);
    // One frame in: moved, but nowhere near there (a 32 ms jump is a click).
    CHECK(out[0].position.x > 0.5);
    CHECK(out[0].position.x < 0.9);
    CHECK(out[0].gain > 0.0);
    CHECK(out[0].gain < 0.9);

    for (int i = 0; i < 100; ++i) smoother.step(out);
    CHECK(out[0].position.x == 1.0);
    CHECK(out[0].position.y == 0.0);
    CHECK(out[0].position.z == 0.5);
    CHECK(out[0].gain == 1.0);
    CHECK_FALSE(out[0].snap);
}

TEST_CASE("snap jumps a slot to its target with no lag", "[crucible]") {
    PlacementSmoother smoother(3.0);
    std::vector<ac3::oba::ObjectPlacement> out(kObjectSlots);
    smoother.set_target(4, {.position = {0.2, 0.8, -1.0}, .gain = 0.7});
    smoother.snap(4);
    smoother.step(out);
    CHECK(out[4].position.x == Approx(0.2));
    CHECK(out[4].position.z == Approx(-1.0));
    CHECK(out[4].gain == Approx(0.7));
}

TEST_CASE("freeing a slot fades the gain out in place", "[crucible]") {
    PlacementSmoother smoother(3.0);
    std::vector<ac3::oba::ObjectPlacement> out(kObjectSlots);
    smoother.set_target(2, {.position = {0.9, 0.9, 0.0}, .gain = 1.0});
    smoother.snap(2);
    smoother.set_gain(2, 0.0);
    smoother.step(out);
    CHECK(out[2].gain < 1.0);
    CHECK(out[2].gain > 0.0);
    CHECK(out[2].position.x == Approx(0.9));  // did not move
    for (int i = 0; i < 100; ++i) smoother.step(out);
    CHECK(out[2].gain == 0.0);
}

TEST_CASE("the bed slots are always the pinned speaker placements", "[crucible]") {
    PlacementSmoother smoother(3.0);
    std::vector<ac3::oba::ObjectPlacement> out(kObjectSlots);
    smoother.step(out);
    for (int slot = kPositionedSlots; slot < kObjectSlots; ++slot) {
        CHECK(out[static_cast<std::size_t>(slot)].snap);
        CHECK(out[static_cast<std::size_t>(slot)].gain == 1.0);
    }
    CHECK(out[kPositionedSlots].position.x == 0.0);      // L
    CHECK(out[kPositionedSlots + 1].position.x == 1.0);  // R
}

TEST_CASE("an idle positioned slot is silent at the centre", "[crucible]") {
    PlacementSmoother smoother(3.0);
    std::vector<ac3::oba::ObjectPlacement> out(kObjectSlots);
    smoother.step(out);
    CHECK(out[7].gain == 0.0);
    CHECK(out[7].position.x == Approx(0.5));
    CHECK(out[7].position.y == Approx(0.5));
}

TEST_CASE("a zero time constant is immediate", "[crucible]") {
    PlacementSmoother smoother(0.0);
    std::vector<ac3::oba::ObjectPlacement> out(kObjectSlots);
    smoother.set_target(0, {.position = {1.0, 1.0, 1.0}, .gain = 1.0});
    smoother.step(out);
    CHECK(out[0].position.x == 1.0);
    CHECK(out[0].gain == 1.0);
}

TEST_CASE("an object's size glides like its position and reaches the placement", "[crucible]") {
    PlacementSmoother smoother(3.0);
    std::vector<ac3::oba::ObjectPlacement> out(kObjectSlots);
    smoother.set_target(2, {.position = {0.5, 0.5, 0.0}, .gain = 1.0, .size = 0.6});
    smoother.snap(2);
    smoother.step(out);
    CHECK(out[2].size.width == Approx(0.6));
    CHECK(out[2].size.depth == Approx(0.6));
    CHECK(out[2].size.height == Approx(0.6));
    CHECK(out[3].size.is_point());
    // A new size approaches over a few frames rather than jumping.
    smoother.set_target(2, {.position = {0.5, 0.5, 0.0}, .gain = 1.0, .size = 0.0});
    smoother.step(out);
    CHECK(out[2].size.width < 0.6);
    CHECK(out[2].size.width > 0.0);
    for (int i = 0; i < 40; ++i) {
        smoother.step(out);
    }
    CHECK(out[2].size.width == Approx(0.0).margin(1e-6));
}
