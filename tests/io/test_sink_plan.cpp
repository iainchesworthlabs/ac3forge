// The I2S/TDM line-planning arithmetic, tested on the host.
//
// This is the part of the streaming example's dynamic sink that can be
// checked without a board: which mode (standard I2S or TDM) and how many
// slots each of up to two I2S lines needs for a channel count. Peripheral
// setup either works on hardware or does not; the hardware ceiling and the
// two-line split are arithmetic, and arithmetic can be wrong quietly.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>

#include "ac3forge/sink_plan.hpp"

using ac3forge::line_ceiling;
using ac3forge::plan_sink;
using ac3forge::sink_ceiling;
using ac3forge::SinkLinePlan;
using ac3forge::SinkPlan;

TEST_CASE("sink_ceiling matches the per-line ceiling plan_sink refuses past",
          "[io][sink_plan]") {
    CHECK(sink_ceiling(32, false) == 4);
    CHECK(sink_ceiling(32, true) == 8);
    CHECK(sink_ceiling(16, false) == 2);
    // 16-bit stays single-line even with a second line enabled - no TDM at
    // this width for the overflow to spill into.
    CHECK(sink_ceiling(16, true) == 2);
    CHECK(sink_ceiling(24, false) == 0);
    CHECK(sink_ceiling(0, true) == 0);

    CHECK(line_ceiling(32).slots == 4);
    CHECK(line_ceiling(32).second_line_usable);
    CHECK(line_ceiling(16).slots == 2);
    CHECK_FALSE(line_ceiling(16).second_line_usable);
    CHECK(line_ceiling(24).slots == 0);
}

TEST_CASE("one line, 32-bit: standard mode for 1-2, TDM for 3-4, refused past 4",
          "[io][sink_plan]") {
    for (const std::size_t channels : {std::size_t{1}, std::size_t{2}}) {
        CAPTURE(channels);
        const auto plan = plan_sink(channels, 32, false);
        REQUIRE(plan.has_value());
        REQUIRE(plan->line0.slots == channels);
        REQUIRE(plan->line0.channels == channels);
        REQUIRE_FALSE(plan->line0.tdm);
        REQUIRE(plan->line1.slots == 0);
    }
    for (const std::size_t channels : {std::size_t{3}, std::size_t{4}}) {
        CAPTURE(channels);
        const auto plan = plan_sink(channels, 32, false);
        REQUIRE(plan.has_value());
        REQUIRE(plan->line0.slots == channels);
        REQUIRE(plan->line0.channels == channels);
        REQUIRE(plan->line0.tdm);
        REQUIRE(plan->line1.slots == 0);
    }
    REQUIRE_FALSE(plan_sink(5, 32, false).has_value());
    REQUIRE_FALSE(plan_sink(0, 32, false).has_value());
}

TEST_CASE("one line, 16-bit: standard mode only, capped at 2 - no TDM interleave at this width",
          "[io][sink_plan]") {
    const auto mono = plan_sink(1, 16, false);
    REQUIRE(mono.has_value());
    REQUIRE(mono->line0.slots == 1);
    REQUIRE_FALSE(mono->line0.tdm);

    const auto stereo = plan_sink(2, 16, false);
    REQUIRE(stereo.has_value());
    REQUIRE(stereo->line0.slots == 2);
    REQUIRE_FALSE(stereo->line0.tdm);

    // Three would need a 16-bit TDM interleave that does not exist yet -
    // refused, not silently rounded up or down.
    REQUIRE_FALSE(plan_sink(3, 16, false).has_value());
}

TEST_CASE("an unrecognised slot width is refused, not silently rounded", "[io][sink_plan]") {
    REQUIRE_FALSE(plan_sink(2, 24, false).has_value());
    REQUIRE_FALSE(plan_sink(2, 0, false).has_value());
    REQUIRE_FALSE(plan_sink(2, -32, false).has_value());
}

TEST_CASE("two lines, 32-bit: line 0 fills to its ceiling before line 1 is used at all",
          "[io][sink_plan]") {
    // Within one line's own reach: line 1 stays unused even though it is
    // enabled - no reason to bring up a second peripheral nothing needs.
    for (const std::size_t channels : {std::size_t{1}, std::size_t{2}, std::size_t{3},
                                       std::size_t{4}}) {
        CAPTURE(channels);
        const auto plan = plan_sink(channels, 32, true);
        REQUIRE(plan.has_value());
        REQUIRE(plan->line0.slots == channels);
        REQUIRE(plan->line1.slots == 0);
    }

    // Past one line's ceiling: both lines run the SAME frame shape (both
    // TDM, both the full 4-slot width), since they share a bit clock and
    // word select and a mismatched frame would desync that. Line 0 takes
    // its full 4 real channels; line 1 carries the remainder, its unused
    // tail slots present (fixed width) but not carrying real audio.
    const auto five = plan_sink(5, 32, true);
    REQUIRE(five.has_value());
    REQUIRE(five->line0.slots == 4);
    REQUIRE(five->line0.channels == 4);
    REQUIRE(five->line0.tdm);
    REQUIRE(five->line1.slots == 4);
    REQUIRE(five->line1.channels == 1);
    REQUIRE(five->line1.tdm);

    const auto eight = plan_sink(8, 32, true);
    REQUIRE(eight.has_value());
    REQUIRE(eight->line0.slots == 4);
    REQUIRE(eight->line0.channels == 4);
    REQUIRE(eight->line1.slots == 4);
    REQUIRE(eight->line1.channels == 4);

    // Past both lines' combined ceiling (8): refused, not clamped.
    REQUIRE_FALSE(plan_sink(9, 32, true).has_value());

    // The same 5-8 range with the second line NOT enabled: refused, since
    // there is nowhere for the overflow to go.
    REQUIRE_FALSE(plan_sink(5, 32, false).has_value());
    REQUIRE_FALSE(plan_sink(8, 32, false).has_value());
}

TEST_CASE("two lines, 16-bit: the combined ceiling is still only 2 - no TDM at this width",
          "[io][sink_plan]") {
    // Even with a second line enabled, 16-bit stays standard-only per line,
    // so three channels still has nowhere to go: the second line would need
    // to run TDM to carry the overflow, and 16-bit TDM does not exist.
    REQUIRE_FALSE(plan_sink(3, 16, true).has_value());
    const auto two = plan_sink(2, 16, true);
    REQUIRE(two.has_value());
    REQUIRE(two->line0.slots == 2);
    REQUIRE(two->line1.slots == 0);
}
