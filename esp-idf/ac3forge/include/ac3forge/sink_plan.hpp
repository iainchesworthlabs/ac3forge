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
// onto both); anything wider is TDM, and a TDM line carries at most 128 bits
// a frame on both parts the component targets with I2S - the ESP32-S3 and
// the ESP32-C6 have the same I2S_LL_SLOT_FRAME_BIT_MAX, which ESP-IDF v6.1's
// i2s_tdm.c holds a slot configuration to. That is 4 slots at 32 bits
// (ac3forge::interleave_24in32) or 8 at 16 (ac3forge::interleave_16in16).
//
// One line: standard mode for 1-2 channels, sized to them. TDM from 3, and a
// TDM line always runs at its full ceiling width - four 32-bit slots or eight
// 16-bit ones, a 128-bit frame either way - with the slots past the layout's
// channels zeroed by the interleave. Two reasons. A TDM DAC or DSP is set up
// for a fixed frame (TDM4, TDM8), whatever the programme carries. And the
// driver's clock does not reach every shape: on an ESP32-C6 on 2026-09-15,
// ESP-IDF v6.1 accepted three- and five-slot frames at 16 and 24 bits and
// clocked them 6.7% fast (a second of frames drained in 937 ms), where the
// other accepted 16-bit and 24-bit shapes and two to four 32-bit slots
// drained in 999. The driver's i2s_tdm_calculate_clock divides MCLK by BCLK
// in integers and only warns when that does not divide: three 16-bit slots
// under the default 256x MCLK need 5.33 and get 5, a 51,200 Hz frame.
//
// Two lines only come into it once channels exceeds one line's ceiling: they
// share one bit clock and word select (see the streaming example's
// sink/i2s/audio_sink.cpp for why - line 1 is a slave taking its clock from
// line 0's output pins), so both must present the SAME frame shape for a
// shared word-select transition to mean the same thing to each - the full
// width again, real channels filling from line 0 first and whatever is left
// over riding in line 1 with its remaining slots zeroed.

namespace ac3forge {

struct SinkLinePlan {
    // The frame width this line is opened for - the TDM slot mask's size,
    // always the line's ceiling in TDM, or 1/2 in standard mode. Zero means
    // this line is not used at all.
    std::size_t slots = 0;
    // How many of those slots carry real audio, always <= slots; the rest
    // are zeroed by the interleave, not left with a previous frame's data.
    std::size_t channels = 0;
    // false: standard (Philips) I2S. true: TDM.
    bool tdm = false;
};

struct SinkPlan {
    SinkLinePlan line0;
    SinkLinePlan line1;  // slots == 0 if the layout does not need a second line
};

// One line's own ceiling at a given slot width, and whether a second line can
// be brought in at all at that width - the arithmetic plan_sink and
// sink_ceiling both stand on, so it exists once rather than twice. `slots`
// is 0 for a width this codebase does not support (anything but 16 or 32).
//
// constexpr, so a sink can size its per-line buffers from it at compile time
// rather than restating the numbers beside an array bound.
struct SinkLineCeiling {
    std::size_t slots = 0;
    // Whether a second line can carry the overflow at this width: it always
    // runs TDM once it is needed at all (see the header comment on why both
    // lines must share one frame shape). Only at 32 bits so far - the
    // streaming example's sink writes a second 16-bit line nowhere yet, so
    // sixteen 16-bit slots on two lines stay refused until it does.
    bool second_line_usable = false;
};

[[nodiscard]] constexpr SinkLineCeiling line_ceiling(int slot_bits) {
    if (slot_bits == 32) {
        return {4, true};  // 128-bit frame / 32
    }
    if (slot_bits == 16) {
        return {8, false};  // 128-bit frame / 16
    }
    return {0, false};
}

// The most slots this line configuration could ever be asked to carry - what
// the streaming example's sink_slots() reports once that is a runtime
// ceiling (AC3FORGE_EXAMPLE_I2S_SLOT_BITS, AC3FORGE_EXAMPLE_I2S_SECOND_LINE)
// rather than a build-time slot count, and what accept_layout() validates a
// requested layout against before plan_sink ever runs. 0 for a slot width
// plan_sink also refuses.
[[nodiscard]] constexpr std::size_t sink_ceiling(int slot_bits, bool second_line) {
    const SinkLineCeiling ceiling = line_ceiling(slot_bits);
    if (ceiling.slots == 0) {
        return 0;
    }
    return (second_line && ceiling.second_line_usable) ? ceiling.slots * 2 : ceiling.slots;
}

// `slot_bits` is 16 or 32 - anything else is refused. `second_line` says
// whether one is wired and enabled at all; without one, or at a width whose
// second line is not usable yet (SinkLineCeiling::second_line_usable),
// anything past a single line's own ceiling is refused exactly like
// `channels == 0`.
[[nodiscard]] constexpr std::optional<SinkPlan> plan_sink(std::size_t channels, int slot_bits,
                                                           bool second_line) {
    if (channels == 0) {
        return std::nullopt;
    }
    const SinkLineCeiling ceiling = line_ceiling(slot_bits);
    if (ceiling.slots == 0) {
        return std::nullopt;
    }
    const bool use_second_line = second_line && ceiling.second_line_usable;
    const std::size_t total_ceiling = use_second_line ? ceiling.slots * 2 : ceiling.slots;
    if (channels > total_ceiling) {
        return std::nullopt;
    }

    SinkPlan plan;
    if (channels <= 2) {
        plan.line0 = {channels, channels, false};
    } else if (channels <= ceiling.slots) {
        plan.line0 = {ceiling.slots, channels, true};
    } else {
        plan.line0 = {ceiling.slots, ceiling.slots, true};
        plan.line1 = {ceiling.slots, channels - ceiling.slots, true};
    }
    return plan;
}

}  // namespace ac3forge
