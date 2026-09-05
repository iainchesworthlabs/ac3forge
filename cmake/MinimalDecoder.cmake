# ---------------------------------------------------------------------------
# MinimalDecoder.cmake
#
# Roadmap PF7: the minimum-footprint decoder profile. AC3FORGE_MINIMAL_DECODER
# turns src/forge into a single decode-only static library
# (ac3::forge_minimal) built for a target that has a few hundred kilobytes of
# RAM and no operating system - a set-top box, a receiver, a DSP port.
#
# What the profile actually changes, and why each one is a build-time decision
# rather than a runtime one:
#
#   - Decode-only sources. The encoder is roughly half the library and none of
#     it is reachable from a decode; leaving it out of the archive is the
#     largest single code-size win and needs no cleverness.
#
#   - No direct-form transform tables. src/forge/src/core/transform/stub/ is
#     compiled in place of .../reference/, removing 1,900,544 bytes of .bss -
#     see src/forge/src/core/reference_transform.hpp for the per-table
#     measurements. This is the one change with a visible behavioural
#     consequence: DecoderConfig::fast_imdct == false is refused
#     (DecodeError::kUnsupported) rather than silently served by the fast path.
#
#   - -fno-exceptions -fno-rtti. The codec's own error mechanism is
#     std::expected throughout - it has no throw, no try and no catch of its
#     own, and -fno-exceptions is what asserts it: a throw, try or catch in
#     the sources this profile compiles fails this build, which the QEMU leg
#     runs through tools/checks/run_baremetal_probe.sh. What remains are the
#     standard library's own throw sites, which with -fno-exceptions become
#     calls to std::terminate: std::vector's length_error/bad_alloc. See
#     docs/building.md's gap note - this profile removes the exception TABLES,
#     it does not remove the allocation.
#
#   - -ffunction-sections -fdata-sections, and --gc-sections when linking an
#     executable, so an integrator linking a subset pays for a subset.
#
# MSVC is not a target of this profile and is rejected rather than
# approximated: /GR- exists but /EHs- only suppresses the assumption that
# extern "C" can throw, and no MSVC target this profile is aimed at exists.
# ---------------------------------------------------------------------------

if(NOT AC3FORGE_MINIMAL_DECODER)
    return()
endif()

if(NOT (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
        CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang"))
    message(FATAL_ERROR
        "AC3FORGE_MINIMAL_DECODER needs GCC or Clang; the active compiler is "
        "${CMAKE_CXX_COMPILER_ID}. See cmake/MinimalDecoder.cmake for why MSVC is not "
        "approximated here.")
endif()

add_library(ac3_minimal_profile INTERFACE)
add_library(ac3::minimal_profile ALIAS ac3_minimal_profile)

target_compile_options(ac3_minimal_profile INTERFACE
    -fno-exceptions
    -fno-rtti
    -ffunction-sections
    -fdata-sections)

# --gc-sections is a LINK option and only meaningful for an executable, so it
# rides the interface target rather than being a global add_link_options():
# a static archive is not linked, and forcing the flag on a consumer that
# deliberately keeps every section would be this file overreaching.
target_link_options(ac3_minimal_profile INTERFACE "LINKER:--gc-sections")

message(STATUS "Minimum-footprint decoder profile: ON (decode-only, no exceptions/RTTI, "
               "no direct-form transform tables)")
