# ---------------------------------------------------------------------------
# The ESP32-C3 shape of the minimum-footprint probe: the fixed-point tier's
# part (planning/arithmetic-tiers.md, Phase D).
#
# An ESP-IDF project driven by idf.py, the same shape as the esp32s3 sibling
# beside it and for the same reasons - that file carries the arguments this one
# does not repeat: why the component is reached through esp-idf/ rather than a
# private path, why the two profiles are mutually exclusive, and why a build
# directory has to be cleaned between them.
#
#   idf.py set-target esp32c3
#   idf.py build
#   idf.py qemu                    # no board needed; qemu-riscv32
#   idf.py -p <PORT> flash monitor # a board
#
# WHY A THIRD TARGET. The C3 is the part the fixed-point tier was written for:
# a 160 MHz single-core RV32IMC with no floating-point unit at all, where
# `float` is a compiled subroutine and `double` is a longer one. The S3 beside
# it has an FPU and runs the float tier; the Cortex-M3 leg has none but is not
# a part anybody ships. This is both at once - no FPU and a real product part -
# and it is where the tier's two claims meet the silicon: that integer
# arithmetic decodes to the same PCM everywhere, and that it costs a third of
# what software floating point costs.
#
# The RISC-V leg is also the tier's third architecture. Its `pcm_hash` lines
# are checked against the same pinned values the x86 host and the Cortex-M3
# leg are held to (tests/golden/fixed-probe-pcm-hashes.json), which is what
# "bit-identical by construction" has to mean if it means anything.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.28)

get_filename_component(AC3FORGE_ESP_COMPONENTS
                       "${CMAKE_CURRENT_SOURCE_DIR}/../../../../esp-idf" ABSOLUTE)
set(EXTRA_COMPONENT_DIRS "${AC3FORGE_ESP_COMPONENTS}")

set(AC3FORGE_ESP_PROFILE "decoder" CACHE STRING
    "Which probe to build: decoder or encoder")

# The fixed-point tier, and not as a suggestion: this part has no FPU, so the
# profile's own float32 default would make every coefficient operation a
# compiled subroutine. Set here rather than left to the command line so that a
# bare `idf.py build` for this target builds the thing the target is for.
#
# It is still an override, not a hard-wire - `idf.py -DAC3FORGE_DECODE_SCALAR=float
# build` measures the float tier on the same part, which is the comparison
# docs/platforms/esp32.md's C3 row is made of. src/forge/minimal.cmake is what
# honours it; every other value of the option resolves to float32 there.
set(AC3FORGE_DECODE_SCALAR "fixed" CACHE STRING
    "Decode arithmetic: fixed (this part has no FPU), float, or double")

# The decode-critical sources at -O2, as on the S3 and for the same reason:
# the code is in flash, of which a C3 module has 4 MB, and the internal SRAM
# the footprint ceilings guard is untouched by it. On a part with no FPU the
# question this answers is sharper - see src/forge/minimal.cmake's list.
set(AC3FORGE_MINIMAL_HOT_O2 ON CACHE BOOL
    "Decode-critical sources at -O2; see src/forge/minimal.cmake")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(ac3probe_esp32c3 LANGUAGES C CXX)
