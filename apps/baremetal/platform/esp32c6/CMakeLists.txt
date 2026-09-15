# ---------------------------------------------------------------------------
# The ESP32-C6 shape of the minimum-footprint probe: the fixed-point tier on the
# part Hearth's smaller sink is built for (planning/hearth-reference-player.md,
# chip C, phase C1).
#
# The same shape as the esp32c3 sibling beside it, which points at the esp32s3
# project for the arguments all three share; this file says only what differs.
#
#   idf.py set-target esp32c6
#   idf.py build
#   idf.py -p <PORT> flash monitor
#
# NO `idf.py qemu`. ESP-IDF v6.1's qemu-system-riscv32 (esp_develop_9.2.2_20260417)
# emulates one Espressif machine, esp32c3, and idf.py's own table of QEMU targets
# (tools/idf_py_actions/qemu_ext.py) has no esp32c6 row, so `idf.py qemu` refuses
# the target. Every number this project prints comes from a board.
#
# WHAT DIFFERS FROM THE C3. The same kind of core - one 160 MHz RISC-V with no
# floating-point unit, RV32IMAC here where the C3 is RV32IMC - and so the same
# tier. 512 KB of SRAM where the C3 has 400 KB, shared with WiFi 6, Bluetooth LE
# and 802.15.4, and no external PSRAM: ESP-IDF has no external-RAM support for
# this part. The tier's PCM is the same on every architecture it has run on, so
# the questions here are what the part has left for a decoder once a network is
# up, and whether a frame still decodes inside 32 ms while WiFi takes its share
# of the one core.
#
# THE NETWORK LOAD is a Kconfig choice (main/Kconfig.projbuild), for the reason
# main/CMakeLists.txt gives: `none` builds the probe with no network code, and
# `wifi` joins an access point, accepts one TCP connection and reads what arrives
# on it from a task of its own while the probe decodes on the main task. The
# credentials belong in an overlay kept outside the repository:
#
#   idf.py -DSDKCONFIG=<build>/sdkconfig \
#          "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.wifi;<path>/credentials" build
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.28)

get_filename_component(AC3FORGE_ESP_COMPONENTS
                       "${CMAKE_CURRENT_SOURCE_DIR}/../../../../esp-idf" ABSOLUTE)
set(EXTRA_COMPONENT_DIRS "${AC3FORGE_ESP_COMPONENTS}")

set(AC3FORGE_ESP_PROFILE "decoder" CACHE STRING
    "Which probe to build: decoder or encoder")

# The fixed-point tier by default, as on the C3 and for the same reason: no FPU.
# `idf.py -DAC3FORGE_DECODE_SCALAR=float build` measures the float tier on the
# same part, which is the comparison docs/platforms/bare-metal/esp32-c6.md's
# tables are made of.
set(AC3FORGE_DECODE_SCALAR "fixed" CACHE STRING
    "Decode arithmetic: fixed (this part has no FPU), float, or double")

set(AC3FORGE_MINIMAL_HOT_O2 ON CACHE BOOL
    "Decode-critical sources at -O2; see src/forge/minimal.cmake")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(ac3probe_esp32c6 LANGUAGES C CXX)
