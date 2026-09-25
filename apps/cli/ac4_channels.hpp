#pragma once

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <span>
#include <vector>

#include "ac4dec/decoder.hpp"

// AC-4's channels in WAV files, for `decode` and `ac4-encode` alike: a WAV
// file holds them in the WAVEFORMATEXTENSIBLE speaker order the E-AC-3 path
// writes (plan::wav_order: FL FR FC LFE BL BR, then SL SR and the top front
// pair), with Ls and Rs at SL and SR, Lb and Rb at BL and BR, and Lw and Rw,
// which that order has no place for, last. So 5.1's surrounds take the fifth
// and sixth channels, as E-AC-3's do.

namespace ac3cli {

[[nodiscard]] inline int ac4_wav_rank(ac4::Speaker speaker) {
    switch (speaker) {
        case ac4::Speaker::kLeft:
            return 0;
        case ac4::Speaker::kRight:
            return 1;
        case ac4::Speaker::kCentre:
            return 2;
        case ac4::Speaker::kLfe:
            return 3;
        case ac4::Speaker::kLeftBack:
            return 4;
        case ac4::Speaker::kRightBack:
            return 5;
        case ac4::Speaker::kLeftSurround:
            return 9;
        case ac4::Speaker::kRightSurround:
            return 10;
        case ac4::Speaker::kTopFrontLeft:
            return 12;
        case ac4::Speaker::kTopFrontRight:
            return 14;
        default:
            return 99;
    }
}

// The indices of `speakers` ordered by `rank`, ties kept in their order.
template <typename Rank>
[[nodiscard]] std::vector<std::size_t> ac4_order(std::span<const ac4::Speaker> speakers, Rank rank) {
    std::vector<std::size_t> order(speakers.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::ranges::stable_sort(order, {}, [&](std::size_t c) { return rank(speakers[c]); });
    return order;
}

}  // namespace ac3cli
