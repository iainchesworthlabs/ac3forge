# ---------------------------------------------------------------------------
# iclforge-probe - the minimum-footprint probe. See probe.cpp's and
# encode_probe.cpp's own headers for what each checks and why neither is a unit
# test.
#
# Built under ICLFORGE_MINIMAL_DECODER or ICLFORGE_MINIMAL_ENCODER, and in two
# shapes from one source:
#
#   - Cross-compiled for arm-none-eabi (see cmake/toolchains/arm-none-eabi.toolchain.cmake).
#     Adds the reset vector and the linker script from platform/baremetal/, and
#     runs under QEMU - the no-OS leg of the minimum-footprint decoder profile.
#
#   - Natively on the host. Same probe, same fixture, same checks, no startup
#     files needed because the host has a C runtime already. It exists so the
#     profile can be built and RUN without an emulator - a leg that only works
#     under QEMU is a leg nobody exercises locally.
#
# Nothing in probe.cpp knows which one it is: the difference is a directory
# CMake adds, per the platform-tree rule (tools/checks/check_platform_macros.ps1).
#
# A third target, ESP32-S3, is driven from outside: platform/esp32s3/ is an
# ESP-IDF project that points EXTRA_COMPONENT_DIRS at esp-idf/, whose iclforge
# component add_subdirectory()s the repo root - the way apps/android's Gradle
# build does. It owns its own application, so this file must NOT also build
# one - see the platform resolution below.
# ---------------------------------------------------------------------------

# Which of the three shapes this configure is. Resolved once, here, rather than
# re-tested at each use, and named rather than inferred - because the obvious
# inference is wrong.
#
# CMAKE_SYSTEM_NAME "Generic" was the discriminator until ESP-IDF arrived, and
# it no longer discriminates: IDF's own toolchain.cmake sets "Generic" too,
# exactly as cmake/toolchains/arm-none-eabi.toolchain.cmake does. An IDF build
# reaching this file with the old test would have quietly linked the Cortex-M3
# reset vector, the __aeabi_read_tp TLS shim and the mps2-an385 linker script
# into a second executable - none of which mean anything on an Xtensa part, and
# the failure would have surfaced as an undefined-symbol error a long way from
# its cause.
#
# The esp32s3 value is set by esp-idf/iclforge/CMakeLists.txt before it
# add_subdirectory()s the root, so it is already in the cache when this runs.
if(NOT DEFINED ICLFORGE_BAREMETAL_PLATFORM)
    if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
        set(ICLFORGE_BAREMETAL_PLATFORM "baremetal")
    else()
        set(ICLFORGE_BAREMETAL_PLATFORM "host")
    endif()
endif()

if(ICLFORGE_BAREMETAL_PLATFORM STREQUAL "esp32s3")
    # ESP-IDF builds the application from platform/esp32s3/main/, linking
    # iclforge::ac3_minimal through its own component. There is nothing for this
    # file to add, and an add_executable() here would produce a second binary
    # IDF has no way to flash.
    message(STATUS "  Footprint probe: ESP-IDF owns the application "
                   "(apps/baremetal/platform/esp32s3)")
    return()
endif()

# main.cpp is the entry point both hosted shapes share; probe.cpp is the probe
# itself, which defines iclforge_probe::run() rather than main() so that ESP-IDF (which
# calls app_main) can run the same code. See apps/baremetal/probe.hpp.
# One probe of three, never more than one: the profiles are mutually exclusive
# at the root (no two of decode / AC-3 encode / E-AC-3 encode fit in an
# ESP32-S3's internal SRAM at once), and the AC-4 decoder is a third shape of the
# decode profile, with its own probe, since AC-4 shares nothing with iclforge::ac3
# (planning/ac4.md, D14a). All three define iclforge_probe::run() rather than main(),
# so main.cpp and the ESP-IDF application run whichever was built without
# knowing which it is.
if(ICLFORGE_MINIMAL_ENCODER)
    add_executable(iclforge-probe encode_probe.cpp main.cpp)
elseif(ICLFORGE_MINIMAL_AC4)
    add_executable(iclforge-probe ac4_probe.cpp main.cpp platform/flat_heap/heap_regions.cpp)
else()
    add_executable(iclforge-probe probe.cpp main.cpp)
endif()

# The stage-timer backend's application half, in EVERY build of the probe and
# not only the ones configured with ICLFORGE_STAGE_TIMERS: a library built
# without the option never calls it, --gc-sections drops it, and the probe
# then prints stage_timers=off instead of a breakdown. Keeping it
# unconditional means the two builds differ in nothing but the library's
# include path, which is what makes their timings comparable.
target_sources(iclforge-probe PRIVATE stage_timers.cpp)

# iclforge_probe::now_us(). One implementation per platform, chosen here rather than
# by a conditional in probe.cpp - the host has std::chrono, newlib on the
# mps2-an385 has semihosting's std::clock, and ESP-IDF has esp_timer (that third
# one lives with its own app, under platform/esp32s3/).
#
# The mps2-an385 has two: semihosting (the default, host time - shape only,
# see platform/baremetal/clock.cpp) and the board's 25 MHz CMSDK timer, which
# under QEMU's -icount counts instructions (platform/baremetal/clock_timer.cpp,
# read back by run_baremetal_probe.sh --icount). A cache variable rather than
# a conditional in either file, per the platform-tree rule.
set(ICLFORGE_BAREMETAL_CLOCK "semihosting" CACHE STRING
    "Clock behind iclforge_probe::now_us() on the mps2-an385: semihosting (host time) or timer (CMSDK TIMER0; instructions under QEMU -icount)")
set_property(CACHE ICLFORGE_BAREMETAL_CLOCK PROPERTY STRINGS semihosting timer)
if(ICLFORGE_BAREMETAL_PLATFORM STREQUAL "baremetal")
    if(ICLFORGE_BAREMETAL_CLOCK STREQUAL "semihosting")
        target_sources(iclforge-probe PRIVATE platform/baremetal/clock.cpp)
    elseif(ICLFORGE_BAREMETAL_CLOCK STREQUAL "timer")
        target_sources(iclforge-probe PRIVATE platform/baremetal/clock_timer.cpp)
    else()
        message(FATAL_ERROR "ICLFORGE_BAREMETAL_CLOCK must be semihosting or timer, not "
                            "'${ICLFORGE_BAREMETAL_CLOCK}'")
    endif()
else()
    target_sources(iclforge-probe PRIVATE platform/host/clock.cpp)
endif()

# What the part can give the decode, in bytes, or 0 for no budget. A fixture
# whose measured peak is above it is skipped and named rather than aborting
# the run on an allocation nobody can satisfy - see probe.cpp's over_budget
# and the ESP32-C3 app, which is the shape that needs one. Both hosted
# builds and the mps2-an385 run every fixture, so both default to 0; a cache
# variable rather than a conditional in probe.cpp, per the platform-tree
# rule. Raising a target's budget is a measurement, not an edit.
set(ICLFORGE_PROBE_HEAP_BUDGET_BYTES "0" CACHE STRING
    "Heap a fixture may peak at before iclforge-probe skips it, in bytes; 0 means no budget")
target_compile_definitions(iclforge-probe PRIVATE
    ICLFORGE_PROBE_HEAP_BUDGET_BYTES=${ICLFORGE_PROBE_HEAP_BUDGET_BYTES})

# What the AC-4 probe decodes with is iclforge::ac4dec, whose archives bring the inspector
# and the core; it links nothing of iclforge::ac3, and has no use for its decoders.
if(ICLFORGE_MINIMAL_AC4)
    target_link_libraries(iclforge-probe PRIVATE iclforge::ac4dec_static iclforge::minimal_profile
                                           iclforge::warnings)
else()
    target_link_libraries(iclforge-probe PRIVATE iclforge::ac3_minimal iclforge::warnings)
endif()

# How much stack the AC-4 probe paints before a decode, to read the stack the decode
# used from what it left (ac4_probe.cpp), in bytes. The window has to fit the stack of
# the task the probe runs in: 256 KiB is a fraction of the mps2-an385's 4 MB and of a
# desktop's 8 MB; an ESP-IDF application sets its own. A decode that uses all of it
# fails the run, since its figure would then be a floor.
set(ICLFORGE_PROBE_STACK_WINDOW_BYTES "262144" CACHE STRING
    "Stack the AC-4 probe paints and reads back, in bytes")
target_compile_definitions(iclforge-probe PRIVATE
    ICLFORGE_PROBE_STACK_WINDOW_BYTES=${ICLFORGE_PROBE_STACK_WINDOW_BYTES})

# A measurement switch for the ENCODE probe, off by default: adds a 7.1
# access-unit fixture (an independent 5.1 substream plus a dependent, two
# FrameEncoders alive at once). Measured 2026-09-10: the host run's peak heap
# goes from 220,608 bytes to 435,263, and an ESP32-S3 dies on a 73,728-byte
# request - a 7.1 E-AC-3 encode does not fit that part's internal SRAM. So it
# is not a default fixture, whose ceilings say what fits, but it stays
# buildable so the number can be re-taken when something changes (PSRAM, a
# smaller encoder). See encode_probe.cpp and encode_fixture.hpp. The ESP-IDF
# application under platform/esp32s3/ takes the same option.
option(ICLFORGE_PROBE_SEVEN_ONE
       "Encode probe: add the 7.1 access-unit fixture (a footprint measurement; does not fit an ESP32-S3)"
       OFF)
target_compile_definitions(iclforge-probe PRIVATE
    "ICLFORGE_PROBE_SEVEN_ONE=$<BOOL:${ICLFORGE_PROBE_SEVEN_ONE}>")

# fixture.hpp sits beside probe.cpp; the generated ac3/export.hpp comes with
# the library target.
target_include_directories(iclforge-probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

if(ICLFORGE_BAREMETAL_PLATFORM STREQUAL "baremetal")
    target_sources(iclforge-probe PRIVATE
        platform/baremetal/vectors.cpp
        platform/baremetal/tls.cpp)

    set(_ac3_probe_ld "${CMAKE_CURRENT_SOURCE_DIR}/platform/baremetal/mps2-an385.ld")
    target_link_options(iclforge-probe PRIVATE "LINKER:-T,${_ac3_probe_ld}")
    # The script is an input to the LINK step, not to any compile, so CMake
    # does not track it on its own - say so, or editing the memory map does
    # not relink.
    set_target_properties(iclforge-probe PROPERTIES LINK_DEPENDS "${_ac3_probe_ld}")

    # The map is what tools/checks/footprint_report.py reads to attribute code
    # size to objects, and the single most useful artefact when a footprint
    # number moves for a reason nobody can see in the diff.
    target_link_options(iclforge-probe PRIVATE
        "LINKER:-Map,${CMAKE_CURRENT_BINARY_DIR}/iclforge-probe.map")

    message(STATUS "  Bare-metal probe: ${ICLFORGE_BAREMETAL_TARGET} "
                   "(run it with tools/checks/run_baremetal_probe.sh)")
else()
    message(STATUS "  Footprint probe: host build (run bin/iclforge-probe directly)")
endif()

# Not ctest-registered in either shape, and deliberately: the profile requires
# ICLFORGE_BUILD_TESTS=OFF (nothing under tests/ builds against a decode-only
# archive), so there is no enable_testing() in this configure for an add_test
# to attach to. tools/checks/run_baremetal_probe.sh is the runner for both
# shapes - it configures, builds, runs (under QEMU when cross-built) and gates
# on the probe's own key=value output.
