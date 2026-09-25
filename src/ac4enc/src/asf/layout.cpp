#include "asf/layout.hpp"

#include <cstddef>

namespace ac4::detail {
namespace {

// Windows in one half of a split frame at transform length index `index`.
[[nodiscard]] int windows_in_half(int index) noexcept {
    return 1 << (3 - index);
}

// The breaks within one half: before each window of the half but the first,
// whether a new group starts there.
[[nodiscard]] std::vector<bool> half_breaks(int windows, int attack) {
    std::vector<bool> breaks(static_cast<std::size_t>(windows > 0 ? windows - 1 : 0), false);
    if (attack > 0 && attack < windows) {
        breaks[static_cast<std::size_t>(attack - 1)] = true;
    }
    if (attack >= 0 && attack + 1 < windows) {
        breaks[static_cast<std::size_t>(attack)] = true;
    }
    return breaks;
}

// Pseudocode 3 and the first half of Pseudocode 4, from the transmitted bits.
void derive(FrameLayout& layout, int frame_length) {
    layout.window_length.clear();
    layout.window_group.clear();
    layout.group_windows.clear();
    layout.group_length.clear();
    layout.group_half.clear();
    if (layout.long_frame) {
        layout.different_framing = false;
        layout.window_length.push_back(frame_length);
        layout.window_group.push_back(0);
        layout.group_windows.push_back(1);
        layout.group_length.push_back(frame_length);
        layout.group_half.push_back(0);
        return;
    }
    layout.different_framing = layout.transf_length[0] != layout.transf_length[1];
    std::vector<std::uint8_t> grouping = layout.grouping_bits;
    const int windows_0 = windows_in_half(layout.transf_length[0]);
    int num_windows = static_cast<int>(grouping.size()) + 1;
    if (layout.different_framing) {
        // A break is implied between the halves; the second half's bits move
        // up one place to make room for it.
        grouping.insert(grouping.begin() + (windows_0 - 1), std::uint8_t{0});
        ++num_windows;
    }
    int group = 0;
    for (int w = 0; w < num_windows; ++w) {
        if (w > 0 && grouping[static_cast<std::size_t>(w - 1)] == 0) {
            ++group;
        }
        const int half = w < windows_0 ? 0 : 1;
        const int length = block_length(frame_length, layout.transf_length[static_cast<std::size_t>(half)]);
        layout.window_length.push_back(length);
        layout.window_group.push_back(group);
        if (static_cast<int>(layout.group_windows.size()) <= group) {
            layout.group_windows.push_back(0);
            layout.group_length.push_back(length);
            layout.group_half.push_back(layout.different_framing ? half : 0);
        }
        ++layout.group_windows[static_cast<std::size_t>(group)];
    }
}

}  // namespace

int block_length(int frame_length, int index) noexcept {
    return frame_length >> (4 - index);
}

int grouping_bit_count(std::array<int, 2> transf_length) noexcept {
    static constexpr std::array<std::array<int, 4>, 4> kTable109 = {{
        {15, 10, 8, 7},
        {10, 7, 4, 3},
        {8, 4, 3, 1},
        {7, 3, 1, 1},
    }};
    return kTable109[static_cast<std::size_t>(transf_length[0])][static_cast<std::size_t>(transf_length[1])];
}

int side_bits(int transform_length) noexcept {
    if (transform_length >= 480) {
        return 5;
    }
    return transform_length >= 240 ? 4 : 3;
}

int max_sfb_bits(int transform_length) noexcept {
    switch (transform_length) {
        case 2048:
        case 1920:
        case 1536:
        case 1024:
        case 960:
        case 768:
        case 512:
        case 480:
        case 384:
            return 6;
        case 256:
        case 240:
        case 192:
            return 5;
        default:
            return 4;  // 128, 120 and 96
    }
}

FrameLayout long_layout(int frame_length) {
    FrameLayout layout;
    layout.long_frame = true;
    derive(layout, frame_length);
    return layout;
}

FrameLayout split_layout(int frame_length, std::array<int, 2> transf_length, std::array<int, 2> attack) {
    FrameLayout layout;
    layout.long_frame = false;
    layout.transf_length = transf_length;
    const bool same = transf_length[0] == transf_length[1];
    for (std::size_t half = 0; half < 2; ++half) {
        const std::vector<bool> breaks = half_breaks(windows_in_half(transf_length[half]), attack[half]);
        if (half == 1 && same) {
            layout.grouping_bits.push_back(0);  // the halves never share a group
        }
        for (const bool b : breaks) {
            layout.grouping_bits.push_back(b ? 0 : 1);
        }
    }
    derive(layout, frame_length);
    return layout;
}

}  // namespace ac4::detail
