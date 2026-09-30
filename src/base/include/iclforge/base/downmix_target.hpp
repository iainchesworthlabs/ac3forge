#pragma once

#include <cstdint>

namespace iclforge::base {

// Which fold to produce. kAsCoded is the default and does nothing at all -
// the coded channels come out exactly as they went in.
enum class DownmixTarget : std::uint8_t {
    kAsCoded,
    kLoRo,  // §7.8.1's plain stereo fold
    kLtRt,  // §7.8.2's Dolby Surround compatible fold
    kMono,  // §7.8's "output_mode == 1/0" branch
};

}  // namespace iclforge::base
