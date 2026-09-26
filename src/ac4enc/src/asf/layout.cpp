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

void add_window(FrameLayout& layout, int length, int group, int half) {
    layout.window_length.push_back(length);
    layout.window_group.push_back(group);
    if (static_cast<int>(layout.group_windows.size()) <= group) {
        layout.group_windows.push_back(0);
        layout.group_length.push_back(length);
        layout.group_half.push_back(half);
    }
    ++layout.group_windows[static_cast<std::size_t>(group)];
}

// Pseudocode 3 and the first half of Pseudocode 4, from the transmitted bits.
void derive(FrameLayout& layout, int frame_length) {
    layout.window_length.clear();
    layout.window_group.clear();
    layout.group_windows.clear();
    layout.group_length.clear();
    layout.group_half.clear();
    if (layout.single) {
        const int index = layout.transf_length[0];
        const int windows = 1 << (whole_frame_index(frame_length) - index);
        layout.long_frame = windows == 1;
        layout.different_framing = false;
        int group = 0;
        for (int w = 0; w < windows; ++w) {
            if (w > 0 && layout.grouping_bits[static_cast<std::size_t>(w - 1)] == 0) {
                ++group;
            }
            add_window(layout, block_length(frame_length, index), group, 0);
        }
        return;
    }
    if (layout.long_frame) {
        layout.different_framing = false;
        add_window(layout, frame_length, 0, 0);
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
        add_window(layout, length, group, layout.different_framing ? half : 0);
    }
}

}  // namespace

int whole_frame_index(int frame_length) noexcept {
    if (frame_length >= 1536) {
        return 4;
    }
    return frame_length >= 768 ? 3 : 2;
}

int block_length(int frame_length, int index) noexcept {
    if (frame_length >= 1536) {
        return frame_length >> (4 - index);
    }
    return frame_length >> (whole_frame_index(frame_length) - index);
}

int group_transf_index(const FrameLayout& layout, std::size_t g) noexcept {
    if (layout.single) {
        return layout.transf_length[0];
    }
    if (layout.long_frame) {
        return 4;
    }
    return layout.transf_length[static_cast<std::size_t>(layout.group_half[g])];
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

int lfe_max_sfb_bits(int frame_length) noexcept {
    return frame_length >= 1536 ? 3 : 2;
}

FrameLayout long_layout(int frame_length) {
    FrameLayout layout;
    layout.long_frame = true;
    if (frame_length < 1536) {
        const int whole = whole_frame_index(frame_length);
        layout.single = true;
        layout.transf_length = {whole, whole};
    }
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

FrameLayout short_layout(int frame_length, int index, int attack) {
    FrameLayout layout;
    layout.long_frame = false;
    layout.single = true;
    layout.transf_length = {index, index};
    const int windows = 1 << (whole_frame_index(frame_length) - index);
    for (const bool b : half_breaks(windows, attack)) {
        layout.grouping_bits.push_back(b ? 0 : 1);
    }
    derive(layout, frame_length);
    return layout;
}

}  // namespace ac4::detail
