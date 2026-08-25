# ---------------------------------------------------------------------------
# ac3probe - the minimum-footprint decoder probe (roadmap PF7). See probe.cpp's
# own header for what it checks and why it is not a unit test.
#
# Built only under AC3FORGE_MINIMAL_DECODER, and in two shapes from one source:
#
#   - Cross-compiled for arm-none-eabi (CMAKE_SYSTEM_NAME "Generic", set by
#     cmake/toolchains/arm-none-eabi.toolchain.cmake). Adds the reset vector and
#     the linker script from platform/baremetal/, and runs under QEMU - the
#     no-OS leg PF7 names.
#
#   - Natively on the host. Same probe, same fixture, same checks, no startup
#     files needed because the host has a C runtime already. It exists so the
#     profile can be built and RUN without an emulator - a leg that only works
#     under QEMU is a leg nobody exercises locally.
#
# Nothing in probe.cpp knows which one it is: the difference is a directory
# CMake adds, per the platform-tree rule (tools/checks/check_platform_macros.ps1).
# ---------------------------------------------------------------------------

add_executable(ac3probe probe.cpp)

target_link_libraries(ac3probe PRIVATE ac3::forge_minimal ac3::warnings)

# fixture.hpp sits beside probe.cpp; the generated ac3/export.hpp comes with
# the library target.
target_include_directories(ac3probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    target_sources(ac3probe PRIVATE
        platform/baremetal/vectors.cpp
        platform/baremetal/tls.cpp)

    set(_ac3_probe_ld "${CMAKE_CURRENT_SOURCE_DIR}/platform/baremetal/mps2-an385.ld")
    target_link_options(ac3probe PRIVATE "LINKER:-T,${_ac3_probe_ld}")
    # The script is an input to the LINK step, not to any compile, so CMake
    # does not track it on its own - say so, or editing the memory map does
    # not relink.
    set_target_properties(ac3probe PROPERTIES LINK_DEPENDS "${_ac3_probe_ld}")

    # The map is what tools/checks/footprint_report.py reads to attribute code
    # size to objects, and the single most useful artefact when a footprint
    # number moves for a reason nobody can see in the diff.
    target_link_options(ac3probe PRIVATE
        "LINKER:-Map,${CMAKE_CURRENT_BINARY_DIR}/ac3probe.map")

    message(STATUS "  Bare-metal probe: ${AC3FORGE_BAREMETAL_TARGET} "
                   "(run it with tools/checks/run_baremetal_probe.sh)")
else()
    message(STATUS "  Footprint probe: host build (run bin/ac3probe directly)")
endif()

# Not ctest-registered in either shape, and deliberately: the profile requires
# AC3FORGE_BUILD_TESTS=OFF (nothing under tests/ builds against a decode-only
# archive), so there is no enable_testing() in this configure for an add_test
# to attach to. tools/checks/run_baremetal_probe.sh is the runner for both
# shapes - it configures, builds, runs (under QEMU when cross-built) and gates
# on the probe's own key=value output.
