#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// One frame's transform layout: ETSI TS 103 190-1 V1.4.1 asf_transform_info()
// (Table 37), the grouping bits of asf_psy_info() (Table 38), and what
// Pseudocode 3 derives from them.
//
// From 1 536 samples a frame, a long frame is one block of the frame's length.
// Otherwise each half of the frame is split by its own transf_length: index 3
// is one block of half the frame, 2 two of a quarter, 1 four of an eighth and 0
// eight of a sixteenth (Table 100's lengths at 2 048, 1 920 and 1 536).
//
// Below 1 536 samples (Table 103) the frame has no b_long_frame and one
// transf_length, held in transf_length[0], for blocks of one length: at 1 024,
// 960 and 768 samples index 3 is the whole frame and 0 an eighth of it, at 512
// and 384 index 2 the whole frame and 0 a quarter. A frame of one block is a
// long frame there too.

namespace ac4::detail {

struct FrameLayout {
    bool long_frame = true;
    std::array<int, 2> transf_length{3, 3};
    bool single = false;  // below 1 536 samples: one transf_length for the frame
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

// Below 1 536 samples, the transf_length of a block of the whole frame: 3 at
// 1 024, 960 and 768, 2 at 512 and 384 (Table 103).
[[nodiscard]] int whole_frame_index(int frame_length) noexcept;

// Transform length index 0 to 3 in samples, for a frame of `frame_length`:
// Table 100's partial blocks from 1 536 samples, Table 103's below.
[[nodiscard]] int block_length(int frame_length, int index) noexcept;

// The transform length index of window group g, as get_transf_length()
// (Pseudocode 2) gives it: 4 for a long frame from 1 536 samples, the frame's
// transf_length below.
[[nodiscard]] int group_transf_index(const FrameLayout& layout, std::size_t g) noexcept;

// Table 109: n_grp_bits for a split frame from 1 536 samples.
[[nodiscard]] int grouping_bit_count(std::array<int, 2> transf_length) noexcept;

// Table 106, at 44.1 and 48 kHz: the width of max_sfb for a transform length.
[[nodiscard]] int max_sfb_bits(int transform_length) noexcept;

// Table 106's n_side_bits: the width of max_sfb_master, and of a side-limited
// max_sfb_side, for a transform length.
[[nodiscard]] int side_bits(int transform_length) noexcept;

// Table 106's n_msfbl_bits: the width of sf_info_lfe()'s max_sfb, whose
// transform is the frame.
[[nodiscard]] int lfe_max_sfb_bits(int frame_length) noexcept;

// A frame of one block. Below 1 536 samples its transf_length is the whole
// frame's index, the one sf_info_lfe() takes as well (src/ac4dec/ERRATA.md,
// "sf_info_lfe() below 1536 samples").
[[nodiscard]] FrameLayout long_layout(int frame_length);

// A split frame, from 1 536 samples. `attack` is, per half, the window of that
// half where a transient starts, or -1: the windows before it are grouped, it
// takes a group of its own, and so do the windows after it; a half without
// one is a single group. The halves never share a group.
[[nodiscard]] FrameLayout split_layout(int frame_length, std::array<int, 2> transf_length, std::array<int, 2> attack);

// Below 1 536 samples, blocks of transf_length `index` grouped as a half of
// split_layout() is around `attack`.
[[nodiscard]] FrameLayout short_layout(int frame_length, int index, int attack);

}  // namespace ac4::detail
