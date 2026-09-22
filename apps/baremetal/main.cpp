// The probe's entry point for every target whose C runtime calls main() - the
// host build and the arm-none-eabi/mps2-an385 one.
//
// One file for both rather than a directory each, because they do not differ:
// the split the platform tree is expressing here is "a C runtime calls main"
// against "an RTOS calls app_main", which is two cases. ESP-IDF is the other
// one, at platform/esp32s3/main/main.cpp.
//
// The startup files that DO differ between host and bare metal - the reset
// vector, the TLS shim, the linker script - are already selected per platform
// by apps/baremetal/CMakeLists.txt, and none of them is an entry point.

#include "probe.hpp"

int main() { return ac3probe::run(); }
