#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <numeric>
#include <vector>

#include "bit_reservoir.hpp"

// ac3::internal::BitReservoir is the accounting behind E-AC-3's average-rate
// mode: what one frame does not spend is what the next one may. The encoder
// tests (tests/encoder/test_eac3.cpp's [abr] cases) prove the rate it
// delivers on real frames; these prove the arithmetic underneath, where the
// awkward cases - an exhausted window, an overspend, a window of one - are
// reachable directly instead of having to be provoked through content.

using ac3::internal::AbrController;
using ac3::internal::BitReservoir;

TEST_CASE("bit reservoir starts at one frame's share, not the window's", "[abr][reservoir]") {
    // A window that began empty would let the very first frame spend the
    // whole window's budget - a second of bits on 32 ms of audio, which is
    // not what an average rate is understood to allow.
    const BitReservoir reservoir{100, 8};
    CHECK(reservoir.allowance() == 100);
    CHECK(reservoir.target_words() == 100);
    CHECK(reservoir.window_frames() == 8);
}

TEST_CASE("bit reservoir hands an underspend to the next frame", "[abr][reservoir]") {
    BitReservoir reservoir{100, 4};
    reservoir.commit(60);
    // 40 words unspent, so the next frame may have its own 100 plus those 40.
    CHECK(reservoir.allowance() == 140);
    reservoir.commit(140);
    CHECK(reservoir.allowance() == 100);
}

TEST_CASE("bit reservoir charges an overspend to the frames after it", "[abr][reservoir]") {
    BitReservoir reservoir{100, 4};
    reservoir.commit(250);  // 150 words past this frame's share
    CHECK(reservoir.allowance() == 0);
    reservoir.commit(0);
    CHECK(reservoir.allowance() == 50);  // 400 budget - 250 - 0 - 100 still owed
}

TEST_CASE("bit reservoir forgets an overspend once it leaves the window", "[abr][reservoir]") {
    // The window is what makes this an AVERAGE rather than a running total:
    // a frame's cost stops counting against the budget as soon as the window
    // has slid past it, so one expensive frame cannot suppress the stream
    // for ever. What the window remembers instead is the two silent frames
    // that paid for it - so the allowance comes back not to par but to the
    // whole pool, which is exactly the guarantee: any three consecutive
    // frames, 300 words.
    BitReservoir reservoir{100, 3};
    reservoir.commit(300);
    CHECK(reservoir.allowance() == 0);
    reservoir.commit(0);
    CHECK(reservoir.allowance() == 0);  // 300 still in the window
    reservoir.commit(0);
    CHECK(reservoir.allowance() == 300);  // 300 has slid out; the pool is free
}

TEST_CASE("a one-frame window pools nothing", "[abr][reservoir]") {
    // The degenerate case: every frame stands on its own, so the allowance
    // never moves whatever the frames before it did.
    BitReservoir reservoir{100, 1};
    CHECK(reservoir.window_frames() == 1);
    CHECK(reservoir.allowance() == 100);
    reservoir.commit(10);
    CHECK(reservoir.allowance() == 100);
    reservoir.commit(1000);
    CHECK(reservoir.allowance() == 100);
}

TEST_CASE("bit reservoir holds the average over any window of frames", "[abr][reservoir]") {
    // The property the whole thing exists for, stated directly: a frame that
    // never spends past its allowance leaves EVERY window of window_frames
    // consecutive frames at or under window_frames * target. Driven with an
    // adversarial pattern - alternately taking everything on offer and
    // taking nothing - which is the shape most likely to expose an
    // off-by-one in the ring.
    constexpr std::uint32_t kTarget = 100;
    constexpr std::size_t kWindow = 5;
    BitReservoir reservoir{kTarget, kWindow};
    std::vector<std::uint32_t> spent;
    for (std::size_t frame = 0; frame < 40; ++frame) {
        const std::uint32_t take = frame % 3 == 0 ? reservoir.allowance() : 0;
        reservoir.commit(take);
        spent.push_back(take);
    }
    for (std::size_t at = 0; at + kWindow <= spent.size(); ++at) {
        const auto total = std::accumulate(spent.begin() + static_cast<std::ptrdiff_t>(at),
                                           spent.begin() + static_cast<std::ptrdiff_t>(at + kWindow),
                                           std::uint64_t{0});
        CHECK(total <= static_cast<std::uint64_t>(kWindow) * kTarget);
    }
}

TEST_CASE("a saturated window still hands out one frame's share", "[abr][reservoir]") {
    // Every frame taking exactly its share is the steady state a constant
    // -complexity stream lands in; the allowance must not drift off it.
    BitReservoir reservoir{192, 16};
    for (int frame = 0; frame < 64; ++frame) {
        CHECK(reservoir.allowance() == 192);
        reservoir.commit(192);
    }
}

TEST_CASE("the ABR controller has no offset until it is seeded", "[abr][reservoir]") {
    // The first frame has nothing to steer from, so the encoder runs the same
    // budget-fitting search CBR does and reports the result here.
    AbrController controller{192, 16};
    CHECK_FALSE(controller.offset().has_value());
    CHECK(controller.allowance() == 192);
    controller.seed(400);
    REQUIRE(controller.offset().has_value());
    CHECK(*controller.offset() == 400);
    // Only the first seed counts - a later frame's ceiling says nothing about
    // where the offset belongs.
    controller.seed(50);
    CHECK(*controller.offset() == 400);
}

TEST_CASE("the ABR controller raises the offset while frames come in cheap",
          "[abr][reservoir]") {
    AbrController controller{200, 16};
    controller.seed(400);
    const int before = *controller.offset();
    for (int frame = 0; frame < 8; ++frame) {
        controller.commit(100, false);  // half a frame's share, every frame
    }
    CHECK(*controller.offset() > before);
}

TEST_CASE("the ABR controller lowers the offset while frames run expensive",
          "[abr][reservoir]") {
    AbrController controller{200, 16};
    controller.seed(400);
    const int before = *controller.offset();
    for (int frame = 0; frame < 8; ++frame) {
        controller.commit(400, false);  // double its share, every frame
    }
    CHECK(*controller.offset() < before);
}

TEST_CASE("a clipped frame cannot push the ABR offset up", "[abr][reservoir]") {
    // Conditional integration, stated directly. A frame the allowance cut
    // short is not evidence the offset is too LOW - it is evidence of the
    // opposite - so the upward correction is suppressed for it. Without that
    // guard the offset winds up against the ceiling and every frame ends up
    // pinned to its share, which is CBR wearing ABR's name.
    AbrController clipped{200, 16};
    AbrController free{200, 16};
    clipped.seed(500);
    free.seed(500);
    for (int frame = 0; frame < 4; ++frame) {
        clipped.commit(100, true);  // half its share, but only because it was cut
        free.commit(100, false);    // half its share, and that is all it wanted
    }
    CHECK(*clipped.offset() == 500);  // suppressed: no upward correction
    CHECK(*free.offset() > 500);      // genuinely cheap: spend more
}

TEST_CASE("a clipped frame that overspent still pulls the ABR offset down",
          "[abr][reservoir]") {
    // The other half of conditional integration: only the UPWARD correction
    // is suppressed. A frame that drew on banked credit and came out over its
    // share is real evidence the offset is too high, clipped or not - which
    // is what makes the wound-up equilibrium unreachable rather than merely
    // unlikely.
    AbrController controller{200, 16};
    controller.seed(500);
    for (int frame = 0; frame < 4; ++frame) {
        controller.commit(400, true);
    }
    CHECK(*controller.offset() < 500);
}

TEST_CASE("the ABR controller keeps the offset inside the search space", "[abr][reservoir]") {
    // (csnroffst << 4) | fsnroffst is 10 bits; an offset outside [0, 1023]
    // is not a value the bit allocation can be asked for at all.
    AbrController high{200, 4};
    high.seed(1000);
    for (int frame = 0; frame < 200; ++frame) {
        high.commit(1, false);  // absurdly cheap, every frame: drive it up hard
    }
    CHECK(*high.offset() == 1023);

    AbrController low{200, 4};
    low.seed(20);
    for (int frame = 0; frame < 200; ++frame) {
        low.commit(4000, false);  // absurdly expensive: drive it down hard
    }
    CHECK(*low.offset() == 0);
}
