#include <catch2/catch_test_macros.hpp>

#include "slots.hpp"

// The slot plan, tested on every platform: nothing here is Windows, and the
// rules (five-slot bed, ten positioned, full-screen forces the bed, waiters
// get freed slots in order) are what the UI's behaviour hangs on.

using ac3::windemo::AppId;
using ac3::windemo::BedChannel;
using ac3::windemo::kBedSlots;
using ac3::windemo::kObjectSlots;
using ac3::windemo::kPositionedSlots;
using ac3::windemo::SlotAllocator;

TEST_CASE("the plan spends fifteen objects as ten positioned plus five bed", "[windemo]") {
    STATIC_CHECK(kObjectSlots == 15);
    STATIC_CHECK(kPositionedSlots == 10);
    STATIC_CHECK(kBedSlots == 5);
    CHECK(ac3::windemo::bed_slot(BedChannel::kL) == 10);
    CHECK(ac3::windemo::bed_slot(BedChannel::kRs) == 14);
}

TEST_CASE("bed placements are pinned to the speakers and snapped", "[windemo]") {
    const auto l = ac3::windemo::bed_placement(BedChannel::kL);
    const auto rs = ac3::windemo::bed_placement(BedChannel::kRs);
    CHECK(l.snap);
    CHECK(rs.snap);
    CHECK(l.position.x == 0.0);
    CHECK(l.position.y == 0.0);
    CHECK(rs.position.x == 1.0);
    CHECK(rs.position.y == 1.0);
    CHECK(ac3::windemo::bed_placement(BedChannel::kC).position.x == 0.5);
}

TEST_CASE("a new application starts in the bed and positioning takes the lowest slot", "[windemo]") {
    SlotAllocator slots;
    slots.add(100);
    CHECK(slots.in_bed(100));
    CHECK(slots.free_positioned_slots() == kPositionedSlots);

    CHECK(slots.position(100) == 0);
    CHECK_FALSE(slots.in_bed(100));
    CHECK(slots.slot_of(100) == 0);
    CHECK(slots.free_positioned_slots() == kPositionedSlots - 1);

    slots.add(200);
    CHECK(slots.position(200) == 1);

    slots.unposition(100);
    CHECK(slots.in_bed(100));
    CHECK(slots.free_positioned_slots() == kPositionedSlots - 1);
    // The freed slot is reused before a higher one.
    slots.add(300);
    CHECK(slots.position(300) == 0);
}

TEST_CASE("positioning an unknown application registers it", "[windemo]") {
    SlotAllocator slots;
    CHECK(slots.position(7) == 0);
    CHECK(slots.known(7));
}

TEST_CASE("the budget is ten and the eleventh waits for a slot", "[windemo]") {
    SlotAllocator slots;
    for (AppId app = 1; app <= static_cast<AppId>(kPositionedSlots); ++app) {
        REQUIRE(slots.position(app).has_value());
    }
    CHECK(slots.free_positioned_slots() == 0);
    CHECK_FALSE(slots.position(11).has_value());
    CHECK(slots.in_bed(11));

    // The request was remembered: freeing any slot hands it over.
    slots.unposition(3);
    CHECK(slots.slot_of(11) == 2);
    CHECK(slots.in_bed(3));
}

TEST_CASE("a full-screen application is the bed whatever the user asked", "[windemo]") {
    SlotAllocator slots;
    REQUIRE(slots.position(1) == 0);
    REQUIRE(slots.position(2) == 1);

    slots.set_fullscreen(1);
    CHECK(slots.in_bed(1));
    CHECK(slots.slot_of(2) == 1);  // untouched
    CHECK(slots.free_positioned_slots() == kPositionedSlots - 1);

    // Asking again while full-screen does not get a slot either.
    CHECK_FALSE(slots.position(1).has_value());

    // Leaving full-screen restores what the user wanted.
    slots.set_fullscreen(std::nullopt);
    CHECK(slots.slot_of(1) == 0);
}

TEST_CASE("a full-screen application's slot goes to whoever was waiting", "[windemo]") {
    SlotAllocator slots;
    for (AppId app = 1; app <= static_cast<AppId>(kPositionedSlots); ++app) {
        REQUIRE(slots.position(app).has_value());
    }
    CHECK_FALSE(slots.position(11).has_value());
    slots.set_fullscreen(5);
    CHECK(slots.slot_of(11) == 4);
    CHECK(slots.in_bed(5));
    // ...and when the game ends, the waiting list is the game, not the
    // application that took its slot.
    slots.set_fullscreen(std::nullopt);
    CHECK(slots.in_bed(5));
    CHECK(slots.slot_of(11) == 4);
}

TEST_CASE("removing an application frees its slot and forgets it", "[windemo]") {
    SlotAllocator slots;
    REQUIRE(slots.position(1) == 0);
    slots.remove(1);
    CHECK_FALSE(slots.known(1));
    CHECK(slots.free_positioned_slots() == kPositionedSlots);
    slots.remove(1);  // harmless twice
    CHECK(slots.free_positioned_slots() == kPositionedSlots);
}

TEST_CASE("adding a known application twice changes nothing", "[windemo]") {
    SlotAllocator slots;
    REQUIRE(slots.position(1) == 0);
    slots.add(1);
    CHECK(slots.slot_of(1) == 0);
    CHECK(slots.apps().size() == 1);
}
