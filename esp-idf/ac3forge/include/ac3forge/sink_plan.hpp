#pragma once

#include <cstddef>
#include <optional>

// Which mode (standard I2S or TDM) and how many slots each of up to two I2S
// lines needs to carry a given channel count - the arithmetic behind the
// ESP32-S3 streaming example's sink reconfiguring itself when PUT /layout
// asks for a different width, instead of a Kconfig-fixed slot count picked
// at build time.
//
// Free of ESP-IDF, like layout.hpp and render.hpp beside it and for the same
// reason: this is the part with real logic (and a hardware ceiling to get
// right), the peripheral setup around it is not, and tests/io/test_sink_plan.cpp
// builds this on the host.
//
// Standard I2S is always exactly two slots (or one, a mono layout doubled
// onto both); anything wider is TDM, and a TDM line carries at most
// I2S_LL_SLOT_FRAME_BIT_MAX bits a frame - 128 on the ESP32-S3 and the
// ESP32-C6, 512 on the ESP32-P4, which ESP-IDF v6.1's i2s_tdm.c holds a slot
// configuration to. That constant is the `frame_bit_max` every function below
// takes: this header stays free of any target's own value, the same way it
// stays free of ESP-IDF itself, and a caller that already includes
// hal/i2s_ll.h for its own reasons (the streaming example's sink does, for
// I2S_LL_INST_NUM) passes I2S_LL_SLOT_FRAME_BIT_MAX straight through. At 128
// bits that is 4 slots at 32 bits (ac3forge::interleave_24in32) or 8 at 16
// (ac3forge::interleave_16in16); at 512 it is 16 or 32.
//
// One line: standard mode for 1-2 channels, sized to them. TDM from 3, and a
// TDM line always runs at its full ceiling width - with the slots past the
// layout's channels zeroed by the interleave. Two reasons. A TDM DAC or DSP
// is set up for a fixed frame (TDM4, TDM8), whatever the programme carries.
// And the driver's clock does not reach every shape: on an ESP32-C6 on
// 2026-09-15, ESP-IDF v6.1 accepted three- and five-slot frames at 16 and 24
// bits and clocked them 6.7% fast (a second of frames drained in 937 ms),
// where the other accepted 16-bit and 24-bit shapes and two to four 32-bit
// slots drained in 999. The driver's i2s_tdm_calculate_clock divides MCLK by
// BCLK in integers and only warns when that does not divide: three 16-bit
// slots under the default 256x MCLK need 5.33 and get 5, a 51,200 Hz frame.
//
// A fixed frame (SinkFrame::fixed): TDM at the full width for every channel
// count from one, for a TDM DAC set up for one frame shape. An ESS ES9080 is
// the case in hand: its slot count, slot width and channel map are written
// over I2C once, and its PLL can lock to the bit clock, so a 2.0 play opened
// as standard I2S would change the bit clock under it and put the samples in
// slots it does not read. A mono layout in a fixed frame rides slot 0 alone
// with the rest zeroed, as in any TDM frame; a DAC that should play it on two
// outputs maps both to that slot. SinkFrame::follow_layout, standard I2S for
// one or two channels, is what a stereo I2S DAC such as a PCM5102 needs.
//
// Two lines only come into it once channels exceeds one line's ceiling: they
// share one bit clock and word select (see the streaming example's
// sink/i2s/audio_sink.cpp for why - line 1 is a slave taking its clock from
// line 0's output pins), so both must present the SAME frame shape for a
// shared word-select transition to mean the same thing to each - the full
// width again, real channels filling from line 0 first and whatever is left
// over riding in line 1 with its remaining slots zeroed. In a fixed frame a
// usable second line runs for every layout, all of its slots zeroed while
// line 0 holds the whole layout: a second DAC on line 1's data pin keeps its
// PLL locked to the shared bit clock whatever plays, and a data pin nobody
// drives gives it no defined samples.

namespace ac3forge {

struct SinkLinePlan {
    // The frame width this line is opened for - the TDM slot mask's size,
    // always the line's ceiling in TDM, or 1/2 in standard mode. Zero means
    // this line is not used at all.
    std::size_t slots = 0;
    // How many of those slots carry real audio, always <= slots; the rest
    // are zeroed by the interleave, not left with a previous frame's data.
    // Zero on a second line a fixed frame holds open with nothing to carry.
    std::size_t channels = 0;
    // false: standard (Philips) I2S. true: TDM.
    bool tdm = false;
};

struct SinkPlan {
    SinkLinePlan line0;
    SinkLinePlan line1;  // slots == 0 if the layout does not need a second line
};

// One line's own ceiling at a given slot width and frame width, and whether a
// second line can be brought in at all at that width - the arithmetic
// plan_sink and sink_ceiling both stand on, so it exists once rather than
// twice. `slots` is 0 for a slot width this codebase does not support
// (anything but 16 or 32).
//
// constexpr, so a sink can size its per-line buffers from it at compile time
// rather than restating the numbers beside an array bound.
struct SinkLineCeiling {
    std::size_t slots = 0;
    // Whether a second line can carry the overflow at this width: it always
    // runs TDM once it is needed at all (see the header comment on why both
    // lines must share one frame shape). True at both slot widths the
    // component supports, which is what puts twice one line's ceiling within
    // reach of a part with two I2S peripherals - eight 16-bit slots a line on
    // the ESP32-S3/C6, sixteen 32-bit ones on a part with a 512-bit frame.
    bool second_line_usable = true;
};

// `frame_bit_max` is the target's own I2S_LL_SLOT_FRAME_BIT_MAX (hal/i2s_ll.h)
// - required, not defaulted, deliberately: a default would still be a
// target-specific assumption hidden in this platform-neutral header, and a
// silent-wrong-ceiling trap for a call site that forgot to pass its own. See
// the header comment for what the value is on each target this component
// builds for.
[[nodiscard]] constexpr SinkLineCeiling line_ceiling(int slot_bits, int frame_bit_max) {
    if (slot_bits == 32 || slot_bits == 16) {
        return {static_cast<std::size_t>(frame_bit_max) / static_cast<std::size_t>(slot_bits), true};
    }
    return {0, false};
}

// The most slots this line configuration could ever be asked to carry - what
// the streaming example's sink_slots() reports once that is a runtime
// ceiling (AC3FORGE_EXAMPLE_I2S_SLOT_BITS, AC3FORGE_EXAMPLE_I2S_SECOND_LINE)
// rather than a build-time slot count, and what accept_layout() validates a
// requested layout against before plan_sink ever runs. 0 for a slot width
// plan_sink also refuses.
[[nodiscard]] constexpr std::size_t sink_ceiling(int slot_bits, bool second_line,
                                                  int frame_bit_max) {
    const SinkLineCeiling ceiling = line_ceiling(slot_bits, frame_bit_max);
    if (ceiling.slots == 0) {
        return 0;
    }
    return (second_line && ceiling.second_line_usable) ? ceiling.slots * 2 : ceiling.slots;
}

// Whether a line's frame follows the layout or holds one shape for every
// layout - see the header comment. An enum rather than a second bool beside
// plan_sink's `second_line`, so a call site cannot swap the two.
enum class SinkFrame {
    // Standard I2S sized to one or two channels, TDM at the full width from three.
    follow_layout,
    // TDM at the full width for every channel count, on every usable line.
    fixed,
};

// `slot_bits` is 16 or 32 - anything else is refused. `second_line` says
// whether one is wired and enabled at all; without one, or at a width whose
// second line is not usable yet (SinkLineCeiling::second_line_usable),
// anything past a single line's own ceiling is refused exactly like
// `channels == 0`. `frame` says whether one or two channels open standard
// I2S (SinkFrame::follow_layout) or the full TDM frame (SinkFrame::fixed).
// `frame_bit_max` is line_ceiling's own required parameter, passed through.
[[nodiscard]] constexpr std::optional<SinkPlan> plan_sink(std::size_t channels, int slot_bits,
                                                           bool second_line, SinkFrame frame,
                                                           int frame_bit_max) {
    if (channels == 0) {
        return std::nullopt;
    }
    const SinkLineCeiling ceiling = line_ceiling(slot_bits, frame_bit_max);
    if (ceiling.slots == 0) {
        return std::nullopt;
    }
    const bool use_second_line = second_line && ceiling.second_line_usable;
    const std::size_t total_ceiling = use_second_line ? ceiling.slots * 2 : ceiling.slots;
    if (channels > total_ceiling) {
        return std::nullopt;
    }

    const bool fixed = frame == SinkFrame::fixed;
    SinkPlan plan;
    if (channels <= 2 && !fixed) {
        plan.line0 = {channels, channels, false};
    } else if (channels <= ceiling.slots) {
        plan.line0 = {ceiling.slots, channels, true};
        if (fixed && use_second_line) {
            plan.line1 = {ceiling.slots, 0, true};
        }
    } else {
        plan.line0 = {ceiling.slots, ceiling.slots, true};
        plan.line1 = {ceiling.slots, channels - ceiling.slots, true};
    }
    return plan;
}

}  // namespace ac3forge
