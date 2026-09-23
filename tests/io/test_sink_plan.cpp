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
using ac3forge::SinkFrame;
using ac3forge::SinkLinePlan;
using ac3forge::SinkPlan;

namespace {

// The two frame choices, short enough that every call below names the one it
// asks for.
constexpr SinkFrame kFollow = SinkFrame::follow_layout;
constexpr SinkFrame kFixed = SinkFrame::fixed;

// I2S_LL_SLOT_FRAME_BIT_MAX on the two targets the real sink builds for
// today (ESP32-S3, ESP32-C6), and on the ESP32-P4's wide TDM controller -
// see sink_plan.hpp's header comment. frame_bit_max is a required parameter
// precisely so a test (or a real call site) cannot forget which target it
// means.
constexpr int kS3C6FrameBits = 128;
constexpr int kP4FrameBits = 512;

bool same_line(const SinkLinePlan& a, const SinkLinePlan& b) {
    return a.slots == b.slots && a.channels == b.channels && a.tdm == b.tdm;
}

}  // namespace

TEST_CASE("sink_ceiling matches the per-line ceiling plan_sink refuses past",
          "[io][sink_plan]") {
    CHECK(sink_ceiling(32, false, kS3C6FrameBits) == 4);
    CHECK(sink_ceiling(32, true, kS3C6FrameBits) == 8);
    // 128 bits a frame: eight 16-bit slots on one line, sixteen across two.
    CHECK(sink_ceiling(16, false, kS3C6FrameBits) == 8);
    CHECK(sink_ceiling(16, true, kS3C6FrameBits) == 16);
    CHECK(sink_ceiling(24, false, kS3C6FrameBits) == 0);
    CHECK(sink_ceiling(0, true, kS3C6FrameBits) == 0);

    CHECK(line_ceiling(32, kS3C6FrameBits).slots == 4);
    CHECK(line_ceiling(32, kS3C6FrameBits).second_line_usable);
    CHECK(line_ceiling(16, kS3C6FrameBits).slots == 8);
    CHECK(line_ceiling(16, kS3C6FrameBits).second_line_usable);
    CHECK(line_ceiling(24, kS3C6FrameBits).slots == 0);

    // Usable in a constant expression, which is what lets a sink size a
    // per-line buffer from it.
    STATIC_REQUIRE(line_ceiling(16, kS3C6FrameBits).slots * 16 == 128);
    STATIC_REQUIRE(line_ceiling(32, kS3C6FrameBits).slots * 32 == 128);
    STATIC_REQUIRE(plan_sink(8, 16, false, kFollow, kS3C6FrameBits).has_value());
    STATIC_REQUIRE(plan_sink(2, 16, false, kFixed, kS3C6FrameBits)->line0.slots == 8);
}

TEST_CASE("one line, 32-bit, frame following the layout: standard mode for 1-2, a full four-slot TDM frame for 3-4, refused past 4",
          "[io][sink_plan]") {
    for (const std::size_t channels : {std::size_t{1}, std::size_t{2}}) {
        CAPTURE(channels);
        const auto plan = plan_sink(channels, 32, false, kFollow, kS3C6FrameBits);
        REQUIRE(plan.has_value());
        REQUIRE(plan->line0.slots == channels);
        REQUIRE(plan->line0.channels == channels);
        REQUIRE_FALSE(plan->line0.tdm);
        REQUIRE(plan->line1.slots == 0);
    }
    // A TDM line runs its full width, the channels past the layout zeroed: a
    // DAC is set up for a fixed frame, and odd slot counts are shapes the
    // driver clocks wrongly at some widths (see sink_plan.hpp).
    for (const std::size_t channels : {std::size_t{3}, std::size_t{4}}) {
        CAPTURE(channels);
        const auto plan = plan_sink(channels, 32, false, kFollow, kS3C6FrameBits);
        REQUIRE(plan.has_value());
        REQUIRE(plan->line0.slots == 4);
        REQUIRE(plan->line0.channels == channels);
        REQUIRE(plan->line0.tdm);
        REQUIRE(plan->line1.slots == 0);
    }
    REQUIRE_FALSE(plan_sink(5, 32, false, kFollow, kS3C6FrameBits).has_value());
    REQUIRE_FALSE(plan_sink(0, 32, false, kFollow, kS3C6FrameBits).has_value());
}

TEST_CASE("one line, 16-bit, frame following the layout: standard mode for 1-2, a full eight-slot TDM frame for 3-8, refused past 8",
          "[io][sink_plan]") {
    const auto mono = plan_sink(1, 16, false, kFollow, kS3C6FrameBits);
    REQUIRE(mono.has_value());
    REQUIRE(mono->line0.slots == 1);
    REQUIRE_FALSE(mono->line0.tdm);

    const auto stereo = plan_sink(2, 16, false, kFollow, kS3C6FrameBits);
    REQUIRE(stereo.has_value());
    REQUIRE(stereo->line0.slots == 2);
    REQUIRE_FALSE(stereo->line0.tdm);

    // 5.1 and 7.1 among them: six and eight channels fit one line at this
    // width, where 32-bit slots stop at four. Three and five are the slot
    // counts the ESP32-C6 clocked 6.7% fast when opened as frames of their
    // own, so every count opens the full eight.
    for (std::size_t channels = 3; channels <= 8; ++channels) {
        CAPTURE(channels);
        const auto plan = plan_sink(channels, 16, false, kFollow, kS3C6FrameBits);
        REQUIRE(plan.has_value());
        REQUIRE(plan->line0.slots == 8);
        REQUIRE(plan->line0.channels == channels);
        REQUIRE(plan->line0.tdm);
        REQUIRE(plan->line1.slots == 0);
    }

    // Nine would need a 136-bit frame - refused, not silently rounded down.
    REQUIRE_FALSE(plan_sink(9, 16, false, kFollow, kS3C6FrameBits).has_value());
    REQUIRE_FALSE(plan_sink(0, 16, false, kFollow, kS3C6FrameBits).has_value());
}

TEST_CASE("an unrecognised slot width is refused, not silently rounded", "[io][sink_plan]") {
    for (const SinkFrame frame : {kFollow, kFixed}) {
        REQUIRE_FALSE(plan_sink(2, 24, false, frame, kS3C6FrameBits).has_value());
        REQUIRE_FALSE(plan_sink(2, 0, false, frame, kS3C6FrameBits).has_value());
        REQUIRE_FALSE(plan_sink(2, -32, false, frame, kS3C6FrameBits).has_value());
    }
}

TEST_CASE("two lines, 32-bit, frame following the layout: line 0 fills to its ceiling before line 1 is used at all",
          "[io][sink_plan]") {
    // Within one line's own reach: line 1 stays unused even though it is
    // enabled - no reason to bring up a second peripheral nothing needs.
    for (const std::size_t channels : {std::size_t{1}, std::size_t{2}, std::size_t{3},
                                       std::size_t{4}}) {
        CAPTURE(channels);
        const auto plan = plan_sink(channels, 32, true, kFollow, kS3C6FrameBits);
        REQUIRE(plan.has_value());
        REQUIRE(plan->line0.slots == (channels <= 2 ? channels : std::size_t{4}));
        REQUIRE(plan->line0.channels == channels);
        REQUIRE(plan->line1.slots == 0);
    }

    // Past one line's ceiling: both lines run the SAME frame shape (both
    // TDM, both the full 4-slot width), since they share a bit clock and
    // word select and a mismatched frame would desync that. Line 0 takes
    // its full 4 real channels; line 1 carries the remainder, its unused
    // tail slots present (fixed width) but not carrying real audio.
    const auto five = plan_sink(5, 32, true, kFollow, kS3C6FrameBits);
    REQUIRE(five.has_value());
    REQUIRE(five->line0.slots == 4);
    REQUIRE(five->line0.channels == 4);
    REQUIRE(five->line0.tdm);
    REQUIRE(five->line1.slots == 4);
    REQUIRE(five->line1.channels == 1);
    REQUIRE(five->line1.tdm);

    const auto eight = plan_sink(8, 32, true, kFollow, kS3C6FrameBits);
    REQUIRE(eight.has_value());
    REQUIRE(eight->line0.slots == 4);
    REQUIRE(eight->line0.channels == 4);
    REQUIRE(eight->line1.slots == 4);
    REQUIRE(eight->line1.channels == 4);

    // Past both lines' combined ceiling (8): refused, not clamped.
    REQUIRE_FALSE(plan_sink(9, 32, true, kFollow, kS3C6FrameBits).has_value());

    // The same 5-8 range with the second line NOT enabled: refused, since
    // there is nowhere for the overflow to go.
    REQUIRE_FALSE(plan_sink(5, 32, false, kFollow, kS3C6FrameBits).has_value());
    REQUIRE_FALSE(plan_sink(8, 32, false, kFollow, kS3C6FrameBits).has_value());
}

TEST_CASE("two lines, 16-bit: sixteen channels as two eight-slot frames",
          "[io][sink_plan]") {
    // The widest shape the component plans: two 128-bit frames of eight
    // 16-bit slots, which is what a pair of eight-channel TDM DACs takes.
    const auto sixteen = plan_sink(16, 16, true, kFollow, kS3C6FrameBits);
    REQUIRE(sixteen.has_value());
    REQUIRE(sixteen->line0.slots == 8);
    REQUIRE(sixteen->line0.channels == 8);
    REQUIRE(sixteen->line0.tdm);
    REQUIRE(sixteen->line1.slots == 8);
    REQUIRE(sixteen->line1.channels == 8);
    REQUIRE(sixteen->line1.tdm);
    REQUIRE_FALSE(plan_sink(17, 16, true, kFollow, kS3C6FrameBits).has_value());

    // Line 0 fills first; the overflow rides line 1 with its remaining slots
    // zeroed, the same rule the 32-bit pair follows.
    const auto twelve = plan_sink(12, 16, true, kFollow, kS3C6FrameBits);
    REQUIRE(twelve.has_value());
    REQUIRE(twelve->line0.channels == 8);
    REQUIRE(twelve->line1.slots == 8);
    REQUIRE(twelve->line1.channels == 4);

    // A layout that fits one line leaves the second down, and a stereo one
    // is still standard mode.
    const auto eight = plan_sink(8, 16, true, kFollow, kS3C6FrameBits);
    REQUIRE(eight.has_value());
    REQUIRE(eight->line0.slots == 8);
    REQUIRE(eight->line1.slots == 0);

    const auto two = plan_sink(2, 16, true, kFollow, kS3C6FrameBits);
    REQUIRE(two.has_value());
    REQUIRE(two->line0.slots == 2);
    REQUIRE_FALSE(two->line0.tdm);
    REQUIRE(two->line1.slots == 0);

    // In a fixed frame the second line runs for every layout, carrying
    // nothing while line 0 holds all of it, so a second DAC's data pin is
    // never left undriven.
    const auto fixed_two = plan_sink(2, 16, true, kFixed, kS3C6FrameBits);
    REQUIRE(fixed_two.has_value());
    REQUIRE(fixed_two->line0.slots == 8);
    REQUIRE(fixed_two->line0.tdm);
    REQUIRE(fixed_two->line1.slots == 8);
    REQUIRE(fixed_two->line1.channels == 0);
    REQUIRE(fixed_two->line1.tdm);
    REQUIRE_FALSE(plan_sink(17, 16, true, kFixed, kS3C6FrameBits).has_value());
}

TEST_CASE("fixed frame, one line: every channel count from 1 opens the full TDM frame",
          "[io][sink_plan]") {
    // A TDM DAC set up over I2C for one frame shape (an ES9080, say) reads a
    // mono or 2.0 play from the same frame as a 7.1 one, the slots past the
    // layout zeroed: no standard mode at 1 or 2.
    for (std::size_t channels = 1; channels <= 8; ++channels) {
        CAPTURE(channels);
        const auto plan = plan_sink(channels, 16, false, kFixed, kS3C6FrameBits);
        REQUIRE(plan.has_value());
        REQUIRE(plan->line0.slots == 8);
        REQUIRE(plan->line0.channels == channels);
        REQUIRE(plan->line0.tdm);
        REQUIRE(plan->line1.slots == 0);
    }
    for (std::size_t channels = 1; channels <= 4; ++channels) {
        CAPTURE(channels);
        const auto plan = plan_sink(channels, 32, false, kFixed, kS3C6FrameBits);
        REQUIRE(plan.has_value());
        REQUIRE(plan->line0.slots == 4);
        REQUIRE(plan->line0.channels == channels);
        REQUIRE(plan->line0.tdm);
        REQUIRE(plan->line1.slots == 0);
    }

    // The same ceilings as a frame that follows the layout.
    REQUIRE_FALSE(plan_sink(9, 16, false, kFixed, kS3C6FrameBits).has_value());
    REQUIRE_FALSE(plan_sink(5, 32, false, kFixed, kS3C6FrameBits).has_value());
    REQUIRE_FALSE(plan_sink(0, 16, false, kFixed, kS3C6FrameBits).has_value());
    REQUIRE_FALSE(plan_sink(0, 32, false, kFixed, kS3C6FrameBits).has_value());
}

TEST_CASE("fixed frame, two lines at 32 bits: line 1 runs its full frame for every layout",
          "[io][sink_plan]") {
    // Within line 0's reach, line 1 is still open at the same shape with
    // nothing to carry, every slot zeroed: a second DAC on its data pin reads
    // defined samples while its PLL follows the shared bit clock.
    for (std::size_t channels = 1; channels <= 4; ++channels) {
        CAPTURE(channels);
        const auto plan = plan_sink(channels, 32, true, kFixed, kS3C6FrameBits);
        REQUIRE(plan.has_value());
        REQUIRE(plan->line0.slots == 4);
        REQUIRE(plan->line0.channels == channels);
        REQUIRE(plan->line0.tdm);
        REQUIRE(plan->line1.slots == 4);
        REQUIRE(plan->line1.channels == 0);
        REQUIRE(plan->line1.tdm);
    }
    // Past it, the same split a frame that follows the layout makes.
    for (std::size_t channels = 5; channels <= 8; ++channels) {
        CAPTURE(channels);
        const auto fixed = plan_sink(channels, 32, true, kFixed, kS3C6FrameBits);
        const auto follow = plan_sink(channels, 32, true, kFollow, kS3C6FrameBits);
        REQUIRE(fixed.has_value());
        REQUIRE(follow.has_value());
        REQUIRE(same_line(fixed->line0, follow->line0));
        REQUIRE(same_line(fixed->line1, follow->line1));
        REQUIRE(fixed->line1.channels == channels - 4);
    }
    REQUIRE_FALSE(plan_sink(9, 32, true, kFixed, kS3C6FrameBits).has_value());
}

TEST_CASE("fixed frame and a frame following the layout plan three channels and up on one line the same way",
          "[io][sink_plan]") {
    for (std::size_t channels = 3; channels <= 8; ++channels) {
        CAPTURE(channels);
        const auto fixed = plan_sink(channels, 16, false, kFixed, kS3C6FrameBits);
        const auto follow = plan_sink(channels, 16, false, kFollow, kS3C6FrameBits);
        REQUIRE(fixed.has_value());
        REQUIRE(follow.has_value());
        REQUIRE(same_line(fixed->line0, follow->line0));
        REQUIRE(same_line(fixed->line1, follow->line1));
    }
    for (std::size_t channels = 3; channels <= 4; ++channels) {
        CAPTURE(channels);
        const auto fixed = plan_sink(channels, 32, false, kFixed, kS3C6FrameBits);
        const auto follow = plan_sink(channels, 32, false, kFollow, kS3C6FrameBits);
        REQUIRE(fixed.has_value());
        REQUIRE(follow.has_value());
        REQUIRE(same_line(fixed->line0, follow->line0));
        REQUIRE(same_line(fixed->line1, follow->line1));
    }
}

// --- the ESP32-P4's wide TDM controller (512-bit frame) --------------------
// Same arithmetic, a different frame_bit_max - these cases exist to prove
// the header generalises rather than having a P4 case hardcoded into it, and
// to pin the one thing the real sink relies on: a single wide line reaches
// this product's full sixteen-channel target with a second line never
// planned, even though the machinery to plan one still exists underneath.

TEST_CASE("wide frame (512 bits): one line reaches sixteen 32-bit slots or thirty-two 16-bit ones",
          "[io][sink_plan]") {
    CHECK(sink_ceiling(32, false, kP4FrameBits) == 16);
    CHECK(sink_ceiling(16, false, kP4FrameBits) == 32);
    CHECK(line_ceiling(32, kP4FrameBits).slots == 16);
    CHECK(line_ceiling(16, kP4FrameBits).slots == 32);
    STATIC_REQUIRE(line_ceiling(32, kP4FrameBits).slots * 32 == 512);
    STATIC_REQUIRE(line_ceiling(16, kP4FrameBits).slots * 16 == 512);
}

TEST_CASE("wide frame, one line, frame following the layout: standard mode for 1-2, TDM up to sixteen, refused past",
          "[io][sink_plan]") {
    for (const std::size_t channels : {std::size_t{1}, std::size_t{2}}) {
        CAPTURE(channels);
        const auto plan = plan_sink(channels, 32, false, kFollow, kP4FrameBits);
        REQUIRE(plan.has_value());
        REQUIRE(plan->line0.slots == channels);
        REQUIRE_FALSE(plan->line0.tdm);
        REQUIRE(plan->line1.slots == 0);
    }
    // Three channels up to the product's full sixteen: one line, no second
    // line ever planned - the whole point of a wide controller.
    for (std::size_t channels = 3; channels <= 16; ++channels) {
        CAPTURE(channels);
        const auto plan = plan_sink(channels, 32, false, kFollow, kP4FrameBits);
        REQUIRE(plan.has_value());
        REQUIRE(plan->line0.slots == 16);
        REQUIRE(plan->line0.channels == channels);
        REQUIRE(plan->line0.tdm);
        REQUIRE(plan->line1.slots == 0);
    }
    REQUIRE_FALSE(plan_sink(17, 32, false, kFollow, kP4FrameBits).has_value());
    REQUIRE_FALSE(plan_sink(0, 32, false, kFollow, kP4FrameBits).has_value());
}

TEST_CASE("wide frame, fixed frame, one line: every channel count from 1 to sixteen opens the full TDM frame",
          "[io][sink_plan]") {
    for (std::size_t channels = 1; channels <= 16; ++channels) {
        CAPTURE(channels);
        const auto plan = plan_sink(channels, 32, false, kFixed, kP4FrameBits);
        REQUIRE(plan.has_value());
        REQUIRE(plan->line0.slots == 16);
        REQUIRE(plan->line0.channels == channels);
        REQUIRE(plan->line0.tdm);
        REQUIRE(plan->line1.slots == 0);
    }
    REQUIRE_FALSE(plan_sink(17, 32, false, kFixed, kP4FrameBits).has_value());
}

TEST_CASE("wide frame, second line enabled: nothing today asks for it, but the header does not refuse it",
          "[io][sink_plan]") {
    // Confirms "the P4 sink never needs a second line" is that sink's own
    // choice of second_line=false, not a limit sink_plan.hpp bakes in for a
    // 512-bit frame: two wide lines combine to thirty-two channels exactly
    // as two narrow ones combine to sixteen.
    const auto twenty = plan_sink(20, 32, true, kFollow, kP4FrameBits);
    REQUIRE(twenty.has_value());
    REQUIRE(twenty->line0.slots == 16);
    REQUIRE(twenty->line0.channels == 16);
    REQUIRE(twenty->line0.tdm);
    REQUIRE(twenty->line1.slots == 16);
    REQUIRE(twenty->line1.channels == 4);
    REQUIRE(twenty->line1.tdm);
    REQUIRE_FALSE(plan_sink(33, 32, true, kFollow, kP4FrameBits).has_value());
}
