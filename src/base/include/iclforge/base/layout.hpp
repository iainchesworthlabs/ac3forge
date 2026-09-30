#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string_view>

// The speaker vocabulary: where a channel sits, named the way TS 103 420 and A/52
// Annex E name them, and an ordered set of them. It lives here, not in eac3_tables.hpp,
// because the renderer, the audio backends and the spatial panner use it for streams of
// any codec.

namespace iclforge::base {

// One speaker feed. A pair location expands to two adjacent enumerators, in
// that order, which is what lets the expansion below be a single sweep.
enum class Location : std::uint8_t {
    kLeft,
    kCentre,
    kRight,
    kLeftSurround,
    kRightSurround,
    kLc,
    kRc,
    kLrs,
    kRrs,
    kCs,
    kTs,
    kLsd,
    kRsd,
    kLw,
    kRw,
    kVhl,
    kVhr,
    kVhc,
    kLts,
    kRts,
    kLfe2,
    kLfe,
};

// Sixteen locations, six of which name two channels.
inline constexpr int kMaxChannels = 22;

[[nodiscard]] constexpr std::string_view name(Location location) {
    constexpr std::array<std::string_view, kMaxChannels> names = {
        "L",   "C",   "R",  "Ls", "Rs",  "Lc",  "Rc",  "Lrs", "Rrs", "Cs",   "Ts",
        "Lsd", "Rsd", "Lw", "Rw", "Vhl", "Vhr", "Vhc", "Lts", "Rts", "LFE2", "LFE"};
    return names[static_cast<std::size_t>(location)];
}

// A map's locations in coded order, which §E2.3.1.8 defines as bit order.
struct Layout {
    std::array<Location, kMaxChannels> items{};
    int count = 0;

    [[nodiscard]] constexpr Location operator[](int index) const {
        return items[static_cast<std::size_t>(index)];
    }
    [[nodiscard]] constexpr auto begin() const { return items.begin(); }
    [[nodiscard]] constexpr auto end() const { return std::next(items.begin(), count); }
    // Where a location sits in this layout, or -1.
    [[nodiscard]] constexpr int index_of(Location location) const {
        for (int i = 0; i < count; ++i) {
            if (items[static_cast<std::size_t>(i)] == location) {
                return i;
            }
        }
        return -1;
    }
};

}  // namespace iclforge::base
