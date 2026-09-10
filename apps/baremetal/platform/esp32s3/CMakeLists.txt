# ---------------------------------------------------------------------------
# The ESP32-S3 shape of the minimum-footprint probe (roadmap PF7).
#
# An ESP-IDF project, driven by idf.py rather than by this repo's presets, for
# the same reason apps/android is a Gradle project: the SDK owns the top-level
# build. IDF is reached from the other direction - esp-idf/ac3forge/
# add_subdirectory()s the repo root - so there is no ac3forge preset for this
# target and no entry in cmake/toolchains/.
#
# Build it with the IDF environment exported:
#
#   idf.py set-target esp32s3
#   idf.py build                                  # decode
#   idf.py -DAC3FORGE_ESP_PROFILE=encoder build   # encode
#   idf.py -p <PORT> flash monitor                # a real board
#   idf.py qemu                                   # no board; see the PSRAM note below
#
# The two profiles are mutually exclusive and produce different applications
# from the same project - see the profile block below. Switching between them
# needs `idf.py fullclean` first: the choice reaches the library as cache
# variables a warm build directory has already resolved.
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

# The ac3forge component lives at the repo root, beside the other bindings, not
# under this project. Four directories up: esp32s3 -> platform -> baremetal ->
# apps -> root, then esp-idf. IDF wants the directory CONTAINING components.
#
# This is the same line an integrator writes in their own project, which is the
# reason the component moved out: if the published shape breaks, this build -
# the one CI runs - breaks with it, rather than continuing to work through a
# private path no integrator has.
get_filename_component(AC3FORGE_ESP_COMPONENTS
                       "${CMAKE_CURRENT_SOURCE_DIR}/../../../../esp-idf" ABSOLUTE)
set(EXTRA_COMPONENT_DIRS "${AC3FORGE_ESP_COMPONENTS}")

# Decode or encode. Declared here, before project(), because the component
# reads it during IDF's component scan - which project() performs.
#
# The default differs from the arm-none-eabi leg's, which has none:
# tools/checks/run_baremetal_probe.sh passes AC3FORGE_MINIMAL_DECODER or
# AC3FORGE_MINIMAL_ENCODER explicitly on every invocation there. Here a bare
# `idf.py build` has to mean something, and decode is the useful something.
set(AC3FORGE_ESP_PROFILE "decoder" CACHE STRING
    "Which probe to build: decoder or encoder")

# The decode-critical sources at -O2 (src/forge/minimal.cmake has the list
# and why each file is on it). On here, off in the profile's own default:
# this is the part real-time decode is measured on, the code lives in flash
# of which the module has 16 MB, and the internal SRAM the footprint ceilings
# guard is untouched by it. What it costs and buys is measured in
# docs/platforms/esp32.md's Timing section, and the Atmos objects fixture
# reaches real time only with it.
set(AC3FORGE_MINIMAL_HOT_O2 ON CACHE BOOL
    "Decode-critical sources at -O2; see src/forge/minimal.cmake")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(ac3probe_esp32s3 LANGUAGES C CXX)
