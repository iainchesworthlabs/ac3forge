#pragma once

#include <cstdint>

// The seam between the probe and whatever starts it (roadmap PF7).
//
// probe.cpp used to define main() directly, which worked while the only two
// shapes were "the host's C runtime calls main" and "newlib's crt0 calls main".
// ESP-IDF calls app_main() instead, so the entry point became a third thing the
// platform decides - and the platform-tree rule
// (tools/checks/check_platform_macros.ps1) says a thing the platform decides is
// a directory CMake picks, never a preprocessor conditional.
//
// So probe.cpp now defines run(), and something small calls it:
//
//   apps/baremetal/main.cpp                    int main()      - host and arm-none-eabi
//   apps/baremetal/platform/esp32s3/main/      void app_main() - ESP-IDF
//
// The two hosted targets share one main.cpp rather than getting a directory
// each, because they do not actually differ: the split here is "a C runtime
// calls main" against "an RTOS calls app_main", which is two cases, not three.

namespace ac3probe {

// Decodes both fixtures, checks every channel's level, reports footprint and
// timing as key=value lines, and returns 0 on pass / 1 on fail - the exit code
// tools/checks/run_baremetal_probe.sh gates on.
int run();

// Monotonic microseconds from an arbitrary origin. Only differences are
// meaningful, and the origin need not survive a reset.
//
// Supplied per platform, because there is no portable answer that is any good
// on all three: the host has std::chrono, newlib on the mps2-an385 has
// semihosting's clock, and ESP-IDF has esp_timer. See the clock.cpp beside each
// platform's other startup files.
//
// A word on what the ARM number is worth: QEMU is not a cycle-accurate
// emulator, so the microseconds it reports describe the host's execution of an
// interpreter, not a Cortex-M3. The line is printed there for shape, not for
// truth. The number this profile actually cares about comes from real silicon.
std::uint64_t now_us();

}  // namespace ac3probe
