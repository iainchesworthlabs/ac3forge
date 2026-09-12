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
// onto both); anything wider is TDM, and an ESP32-S3 TDM line carries at
// most 128 bits a frame (I2S_LL_SLOT_FRAME_BIT_MAX, ESP-IDF v6.1's
// i2s_tdm.c) - 4 slots at 32 bits. `ac3forge::interleave_24in32` is the
// only padding-capable (TDM) conversion this codebase has, so a 16-bit
// slot width - `AC3FORGE_EXAMPLE_I2S_SLOT_BITS`'s other value, which
// standard mode has always supported for stereo - stays standard-only
// here, capped at 2 slots rather than the 8 the frame-bit ceiling would
// otherwise allow: TDM at 16 bits needs its own interleave_16in16-shaped
// function, not built yet.
//
// One line: sized exactly to `channels`, standard mode for 1-2 or TDM for
// 3-4, no padding beyond what the layout itself needs. Two lines only come
// into it once channels exceeds one line's ceiling: they share one bit
// clock and word select (see the streaming example's sink/i2s/audio_sink.cpp
// for why - line 1 is a slave taking its clock from line 0's output pins),
// so both must present the SAME frame shape for a shared word-select
// transition to mean the same thing to each. Both lines therefore run at
// the full per-line ceiling width in TDM mode, real channels filling from
// line 0 first; whatever is left over rides in line 1 with its remaining
// slots zeroed - the same "fixed width, pad with zeros" rule
// ac3forge::interleave_24in32 already applies within one line.

namespace ac3forge {

struct SinkLinePlan {
    // The frame width this line is opened for - the TDM slot mask's size,
    // or 1/2 in standard mode. Zero means this line is not used at all.
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
struct SinkLineCeiling {
    std::size_t slots = 0;
    // Whether a second line can actually carry the overflow at this width: it
    // always runs TDM once it is needed at all (see the header comment on why
    // both lines must share one frame shape), and TDM only exists at 32 bits.
    bool second_line_usable = false;
};

[[nodiscard]] inline SinkLineCeiling line_ceiling(int slot_bits) {
    if (slot_bits == 32) {
        return {4, true};  // 128-bit frame / 32
    }
    if (slot_bits == 16) {
        return {2, false};  // standard mode only - see the header comment
    }
    return {0, false};
}

// The most slots this line configuration could ever be asked to carry - what
// the streaming example's sink_slots() reports once that is a runtime
// ceiling (AC3FORGE_EXAMPLE_I2S_SLOT_BITS, AC3FORGE_EXAMPLE_I2S_SECOND_LINE)
// rather than a build-time slot count, and what accept_layout() validates a
// requested layout against before plan_sink ever runs. 0 for a slot width
// plan_sink also refuses.
[[nodiscard]] inline std::size_t sink_ceiling(int slot_bits, bool second_line) {
    const SinkLineCeiling ceiling = line_ceiling(slot_bits);
    if (ceiling.slots == 0) {
        return 0;
    }
    return (second_line && ceiling.second_line_usable) ? ceiling.slots * 2 : ceiling.slots;
}

// `slot_bits` is 16 or 32 - anything else is refused. `second_line` says
// whether one is wired and enabled at all; without one, or at 16 bits where
// spilling to it would need the same not-yet-built TDM interleave a single
// 16-bit line already lacks, anything past a single line's own ceiling is
// refused exactly like `channels == 0`.
[[nodiscard]] inline std::optional<SinkPlan> plan_sink(std::size_t channels, int slot_bits,
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
    if (channels <= ceiling.slots) {
        plan.line0 = {channels, channels, channels > 2};
    } else {
        plan.line0 = {ceiling.slots, ceiling.slots, true};
        plan.line1 = {ceiling.slots, channels - ceiling.slots, true};
    }
    return plan;
}

}  // namespace ac3forge
