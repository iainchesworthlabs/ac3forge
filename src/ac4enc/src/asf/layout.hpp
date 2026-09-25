#pragma once

#include <array>
#include <cstdint>
#include <vector>

// One frame's transform layout, for frame lengths of 1 536 samples and more:
// ETSI TS 103 190-1 V1.4.1 asf_transform_info() (Table 37), the grouping bits
// of asf_psy_info() (Table 38), and what Pseudocode 3 derives from them.
//
// A long frame is one block of the frame's length. Otherwise each half of the
// frame is split by its own transf_length: index 3 is one block of half the
// frame, 2 two of a quarter, 1 four of an eighth and 0 eight of a sixteenth
// (Table 100's lengths at 2 048, 1 920 and 1 536).

namespace ac4::detail {

struct FrameLayout {
    bool long_frame = true;
    std::array<int, 2> transf_length{3, 3};
    // scale_factor_grouping_bit, n_grp_bits of them in bitstream order: 1
    // keeps the next window in the current group, 0 starts a new one.
    std::vector<std::uint8_t> grouping_bits;

    // Derived, per Pseudocode 3 and 4.
    bool different_framing = false;
    std::vector<int> window_length;   // samples, per window in order
    std::vector<int> window_group;    // the group of each window
    std::vector<int> group_windows;   // num_win_in_group
    std::vector<int> group_length;    // the transform length of a group's windows
    std::vector<int> group_half;      // which max_sfb[] a group takes
};

// Transform length index 0 to 3 in samples, for a frame of `frame_length`.
[[nodiscard]] int block_length(int frame_length, int index) noexcept;

// Table 109: n_grp_bits for a split frame.
[[nodiscard]] int grouping_bit_count(std::array<int, 2> transf_length) noexcept;

// Table 106, at 44.1 and 48 kHz: the width of max_sfb for a transform length.
[[nodiscard]] int max_sfb_bits(int transform_length) noexcept;

// Table 106's n_side_bits: the width of max_sfb_master, and of a side-limited
// max_sfb_side, for a transform length.
[[nodiscard]] int side_bits(int transform_length) noexcept;

// A long frame.
[[nodiscard]] FrameLayout long_layout(int frame_length);

// A split frame. `attack` is, per half, the window of that half where a
// transient starts, or -1: the windows before it are grouped, it takes a
// group of its own, and so do the windows after it; a half without one is a
// single group. The halves never share a group.
[[nodiscard]] FrameLayout split_layout(int frame_length, std::array<int, 2> transf_length, std::array<int, 2> attack);

}  // namespace ac4::detail
