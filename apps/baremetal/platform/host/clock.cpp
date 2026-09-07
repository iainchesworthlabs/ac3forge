// ac3probe::now_us() for the host build.
//
// std::chrono::steady_clock is the right answer wherever there is one: it is
// monotonic by definition, so a clock adjustment mid-run cannot make a decode
// appear to take negative time.
//
// The host's timing number is not what this profile is about - a desktop
// decodes a frame in microseconds and nobody doubted it would. It exists so the
// host shape reports the same key=value lines as the others, which is what lets
// tools/checks/run_baremetal_probe.sh gate both shapes with one parser.

#include <chrono>
#include <cstdint>

#include "probe.hpp"

namespace ac3probe {

std::uint64_t now_us() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

}  // namespace ac3probe
