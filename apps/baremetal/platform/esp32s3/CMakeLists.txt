# ---------------------------------------------------------------------------
# The ESP32-S3 shape of the minimum-footprint decoder probe (roadmap PF7).
#
# An ESP-IDF project, driven by idf.py rather than by this repo's presets, for
# the same reason apps/android is a Gradle project: the SDK owns the top-level
# build. IDF is reached from the other direction - components/ac3forge/
# add_subdirectory()s the repo root - so there is no ac3forge preset for this
# target and no entry in cmake/toolchains/.
#
# Build it with the IDF environment exported:
#
#   idf.py set-target esp32s3
#   idf.py build
#   idf.py -p <PORT> flash monitor     # a real board
#   idf.py qemu                        # no board; see the PSRAM note below
#
# WHY A SECOND BARE-METAL TARGET AT ALL. The arm-none-eabi/mps2-an385 leg
# proves the decoder runs with no OS on a Cortex-M3, which has no FPU - every
# floating-point operation software-emulated. That is the least forgiving
# target and the right one to prove correctness on. It is not a target anybody
# ships. An ESP32-S3 is: 240 MHz dual-core Xtensa LX7 with a single-precision
# FPU and 128-bit PIE SIMD, and the first target where "does this decode in
# real time?" is a question worth asking rather than a foregone no.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.28)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(ac3probe_esp32s3 LANGUAGES C CXX)
