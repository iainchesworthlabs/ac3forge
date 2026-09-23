# ---------------------------------------------------------------------------
# The ESP32-P4 shape of the minimum-footprint probe (minimum-footprint decoder profile).
#
# An ESP-IDF project, driven by idf.py rather than by this repo's presets, for
# the same reason apps/android is a Gradle project: the SDK owns the top-level
# build. IDF is reached from the other direction - esp-idf/ac3forge/
# add_subdirectory()s the repo root - so there is no ac3forge preset for this
# target and no entry in cmake/toolchains/.
#
# Build it with the IDF environment exported:
#
#   idf.py set-target esp32p4
#   idf.py build                                  # decode
#   idf.py -DAC3FORGE_ESP_PROFILE=encoder build   # encode
#   idf.py -p <PORT> flash monitor                # a real board
#
# There is no QEMU leg: ESP-IDF v6.1's qemu-system-riscv32 emulates the
# esp32c3 machine and no other RISC-V part (see the ESP32-C6 page for the same
# gap), so `idf.py qemu` refuses this target. Every figure this probe produces
# comes from a board.
#
# The two profiles are mutually exclusive and produce different applications
# from the same project - see the profile block below. Switching between them
# needs `idf.py fullclean` first: the choice reaches the library as cache
# variables a warm build directory has already resolved.
#
# WHY A FOURTH BARE-METAL TARGET. ESP32-P4 was assessed and declined
# 2026-09-08 as a replacement for the ESP32-S3 Wi-Fi Sendspin sink - it has no
# radio of its own, and the S3 probe had by then cleared every fixture in real
# time, so there was nothing left to rescue. It was reopened 2026-09-21 as the
# complementary "best" tier of a shared C6/S3/P4 sink family on one PCB: C6 =
# good (5.1, one DAC), S3 = better (7.1.4, both DACs at 16-bit), P4 = best
# (9.1.6 with full Annex E tools desired, both DACs at 32-bit on one wide TDM
# controller) - see planning/esp32-sink-tiers.md. This project is that plan's
# Phase P1: the probe and a board timing table, no network yet.
#
# Dual-core RISC-V (RV32IMAFC) with a single-precision FPU - the `F` in
# IMAFC - and 768 KB of L2MEM, no external PSRAM on the module this was built
# against. Its PIE vector extension is integer-only, the same limit the S3's
# PIE carries (see docs/platforms/bare-metal/esp32-c3.md, "Why this part, and
# not another ESP32 variant"), so this profile decodes in scalar float on the
# FPU, not through any vector unit. Wi-Fi and Bluetooth are not on this die at
# all: the board this was brought up on carries a companion ESP32-C6 over
# SDIO for that, which this probe does not touch - see the sink-tiers plan for
# what a networked shape needs.
#
# 400 MHz is the part's datasheet maximum, but not this board's: the board
# this was brought up on carries pre-production silicon, chip revision v1.3,
# a hardware generation IDF's own Kconfig separates from the v3.x it defaults
# to (components/esp_hw_support/port/esp32p4/Kconfig.hw_support: "huge
# hardware difference... not compatible"). 360 MHz is this revision's ceiling
# - see sdkconfig.defaults for what 400 does on it.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.28)

# The ac3forge component lives at the repo root, beside the other bindings, not
# under this project. Four directories up: esp32p4 -> platform -> baremetal ->
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
set(AC3FORGE_ESP_PROFILE "decoder" CACHE STRING
    "Which probe to build: decoder or encoder")

# The decode-critical sources at -O2 (src/forge/minimal.cmake has the list
# and why each file is on it), the same default the S3 and C3 projects set.
set(AC3FORGE_MINIMAL_HOT_O2 ON CACHE BOOL
    "Decode-critical sources at -O2; see src/forge/minimal.cmake")

# No AC3FORGE_DECODE_SCALAR override here, deliberately - unlike the C3 and C6
# projects beside this one. The component
# (esp-idf/ac3forge/CMakeLists.txt) picks float where
# ESP-IDF's SOC_CPU_HAS_FPU capability says the part has a floating-point
# unit, which this one does, the same reasoning the S3 project leaves
# unstated too - see docs/platforms/bare-metal/esp32-s3.md, "Building -> The
# ESP-IDF component".

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(ac3probe_esp32p4 LANGUAGES C CXX)
