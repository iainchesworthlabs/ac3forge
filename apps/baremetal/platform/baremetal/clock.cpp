// ac3probe::now_us() for arm-none-eabi on QEMU's mps2-an385.
//
// std::clock() is what newlib gives here, and under the rdimon specs the
// toolchain links (see cmake/toolchains/arm-none-eabi.toolchain.cmake) it is
// serviced by semihosting - QEMU answers it from the host.
//
// WHAT THIS NUMBER IS WORTH, stated plainly because it would otherwise be read
// as a Cortex-M3 measurement: QEMU is not a cycle-accurate emulator. It
// translates Thumb-2 to host instructions and runs them as fast as the host
// manages, so the elapsed time here describes an x86-64 machine executing a
// translation of this code, not a 25 MHz Cortex-M3 executing it. Comparing two
// runs of this leg tells you about the host's load, not about the decoder.
//
// It is implemented anyway rather than stubbed to zero for one reason: a
// platform seam with a hole in it invites the caller to branch on which
// platform it is, and the whole point of the seam (and of
// tools/checks/check_platform_macros.ps1) is that no caller ever does that.
// The line is printed for shape; the number that matters comes from silicon.
//
// CLOCKS_PER_SEC is 1000000 on newlib for this target, but the conversion is
// written out rather than assumed - it costs one multiply once per frame, and
// an assumption about a libc constant is exactly the kind of thing that is
// silently wrong on the next toolchain.

#include <cstdint>
#include <ctime>

#include "probe.hpp"

namespace ac3probe {

std::uint64_t now_us() {
    const std::clock_t ticks = std::clock();
    if (ticks == static_cast<std::clock_t>(-1)) {
        return 0;
    }
    return (static_cast<std::uint64_t>(ticks) * 1000000ULL) /
           static_cast<std::uint64_t>(CLOCKS_PER_SEC);
}

}  // namespace ac3probe
