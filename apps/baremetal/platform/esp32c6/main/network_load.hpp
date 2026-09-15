#pragma once

// What else the part does while the probe decodes. main/CMakeLists.txt compiles
// one of the directories beside this header, as main/Kconfig.projbuild chooses:
//
//   net/none/   nothing; both calls return at once
//   net/wifi/   a WiFi station reading a TCP stream on a task of its own
//
// and main.cpp calls these without knowing which, the seam probe.hpp describes
// for the probe's own entry point.

namespace ac3probe {

// Brings the load up and returns once it is running. False when it could not,
// after printing a `result=fail reason=...` line; the probe does not run then.
bool network_start();

// What the load did while the probe ran, as key=value lines.
void network_report();

}  // namespace ac3probe
