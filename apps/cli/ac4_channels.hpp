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
// pair), with Ls and Rs at SL and SR, Lb and Rb at BL and BR, the top back
// pair at TBL and TBR, and Lw and Rw, which that order has no place for, last.
// So 5.1's surrounds take the fifth and sixth channels, as E-AC-3's do, and
// 5.1.4 and 7.1.4 come out as DEE takes them in. The top side pair of an X.2
// layout, which the order has no place for either, takes the top front pair's
// places, which an X.2 layout leaves empty.

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
        case ac4::Speaker::kTopSideLeft:
            return 12;
        case ac4::Speaker::kTopFrontRight:
        case ac4::Speaker::kTopSideRight:
            return 14;
        case ac4::Speaker::kTopBackLeft:
            return 15;
        case ac4::Speaker::kTopBackRight:
            return 17;
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
