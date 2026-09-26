#pragma once

#include <array>
#include <cstddef>
#include <optional>

#include "ac4dec/decoder.hpp"
#include "syntax/oamd.hpp"

// What object audio metadata means (ETSI TS 103 190-2 V1.3.1 clause 6.3.9 and
// Annex F): each object_info_block() an object's properties, against the
// block before it for what it reuses and what it codes as a difference; and
// oamd_timing_data() the sample at which each block takes effect and the ramp
// that leads to it (clause 5.9.2). The syntax is syntax/oamd.hpp's.

namespace ac4::detail {

// One object's metadata as its blocks have set it: the properties of the last
// block, and the standard precision position a difference refers to - pos3D_X
// and pos3D_Y, 0 to 62, and pos3D_Z with its sign, -15 to 15 (6.3.9.8.4).
struct ObjectMetadataState {
    ObjectProperties properties{};
    std::array<int, 3> standard{31, 31, 0};
    bool have = false;
};

// The properties block `block` sets for an object with (`dynamic`) or without
// render information, against `state`, which it moves on. `previous_gain` is
// the gain the object before it takes in the same block, which
// object_gain_code 0b11 copies (0 dB for the first; src/ac4dec/ERRATA.md,
// "Object audio metadata").
[[nodiscard]] ObjectProperties apply_block(const ObjectInfoBlock& block, bool dynamic,
                                           std::optional<double> previous_gain,
                                           ObjectMetadataState& state);

// Clause 5.9.2: update_sample = sample_offset + 32 x block_offset_factor, from
// the codec frame's first output sample, and the block's ramp_duration.
struct BlockTiming {
    int sample = 0;
    int ramp = 0;
};
[[nodiscard]] BlockTiming block_timing(const OamdTimingData& timing, int block) noexcept;

// Part 2 Table A.27's speaker index as a Speaker, for a bed object's
// loudspeaker (Annex F.3); nothing for one the decoder does not name (the
// screen edge pair, and the 22.2 layout's).
[[nodiscard]] std::optional<Speaker> speaker_of_index(int table_a27_index) noexcept;

}  // namespace ac4::detail
