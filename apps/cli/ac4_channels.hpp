#pragma once

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <span>
#include <vector>

#include "ac3/core/eac3_tables.hpp"
#include "ac3/core/tables.hpp"
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

// The level and loudness meters' order, A/52's: L C R Ls Rs, the LFE, then any
// other.
[[nodiscard]] inline int ac4_meter_rank(ac4::Speaker speaker) {
    switch (speaker) {
        case ac4::Speaker::kLeft:
            return 0;
        case ac4::Speaker::kCentre:
            return 1;
        case ac4::Speaker::kRight:
            return 2;
        case ac4::Speaker::kLeftSurround:
            return 3;
        case ac4::Speaker::kRightSurround:
            return 4;
        case ac4::Speaker::kLfe:
            return 5;
        default:
            return 99;
    }
}

// The coding mode that names an AC-4 layout's bed for a meter: 1/0, 2/0, 3/0
// or 3/2; a 7.X layout's last pair is metered past it.
[[nodiscard]] inline ac3::Acmod ac4_bed_acmod(std::span<const ac4::Speaker> speakers) {
    const auto has = [&](ac4::Speaker s) { return std::ranges::find(speakers, s) != speakers.end(); };
    if (has(ac4::Speaker::kLeftSurround)) {
        return ac3::Acmod::k3_2;
    }
    if (has(ac4::Speaker::kLeft)) {
        return has(ac4::Speaker::kCentre) ? ac3::Acmod::k3_0 : ac3::Acmod::k2_0;
    }
    return ac3::Acmod::k1_0;
}

// Where an AC-4 speaker is among A/52's Table E2.5 locations, for a meter that
// weights channels by where they are (BS.1770-5 Annex 3) and for placing a
// decoded presentation on an AC-3 or E-AC-3 layout: Lb and Rb are the rear
// surrounds, Lw and Rw the wides, and the top front pair the vertical heights.
[[nodiscard]] inline ac3::eac3::chanmap::Location ac4_location(ac4::Speaker speaker) {
    using L = ac3::eac3::chanmap::Location;
    switch (speaker) {
        case ac4::Speaker::kLeft:
            return L::kLeft;
        case ac4::Speaker::kRight:
            return L::kRight;
        case ac4::Speaker::kCentre:
            return L::kCentre;
        case ac4::Speaker::kLfe:
            return L::kLfe;
        case ac4::Speaker::kLeftSurround:
            return L::kLeftSurround;
        case ac4::Speaker::kRightSurround:
            return L::kRightSurround;
        case ac4::Speaker::kLeftBack:
            return L::kLrs;
        case ac4::Speaker::kRightBack:
            return L::kRrs;
        case ac4::Speaker::kLeftWide:
            return L::kLw;
        case ac4::Speaker::kRightWide:
            return L::kRw;
        case ac4::Speaker::kTopFrontLeft:
            return L::kVhl;
        case ac4::Speaker::kTopFrontRight:
            return L::kVhr;
    }
    return L::kCentre;
}

}  // namespace ac3cli
