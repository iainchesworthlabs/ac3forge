# ---------------------------------------------------------------------------
# iclforge::base - what every codec and every service builds on and none of them owns: the MSB-first
# bit reader and writer, the speaker vocabulary (Location, Layout), the downmix target names, the CPU
# feature probe, and the build-variant directories the probe and the profiling markers are selected
# through.
#
# It knows no codec. A codec, a container or a renderer links it; it links none of them.
#
# The variant directories are chosen here, by CMake, and put on the include path of everything that
# links this library: a translation unit includes iclforge/base/detail/profiling.hpp and never asks
# what it is being compiled for, which is the rule tools/checks/check_platform_macros.ps1 enforces.
# The SIMD architecture is chosen the same way in src/arithmetic, which this directory reads the answer
# of (ICLFORGE_SIMD_RESOLVED) from the root scope.
# ---------------------------------------------------------------------------

# AVX2 is not architectural: a build machine cannot know the deployment machine has it, so it needs a
# runtime answer. The kernels live in src/ac3 (iclforge_ac3_simd_avx2); the probe that answers
# has_avx2() lives here, and one of variants/cpu-probe-{none,msvc,builtin}/ goes on the include path.
# msvc/ is shared by real MSVC and clang-cl: clang-cl reports CMAKE_CXX_COMPILER_ID "Clang" with
# CMAKE_CXX_COMPILER_FRONTEND_VARIANT "MSVC", and __builtin_cpu_supports needs compiler-rt's cpu-model
# support, which the MSVC-CRT-linked clang-cl configuration does not guarantee.
option(ICLFORGE_AVX2
    "Compile an AVX2 SIMD tier (x86_64 only) alongside the baseline SSE2 one, selected at runtime"
    ON)

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR
   (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND
    CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    set(ICLFORGE_IS_MSVC_LIKE TRUE)
else()
    set(ICLFORGE_IS_MSVC_LIKE FALSE)
endif()

if(NOT (ICLFORGE_SIMD_RESOLVED STREQUAL "x86_64" AND ICLFORGE_AVX2))
    set(ICLFORGE_CPU_PROBE_DIR "none")
elseif(ICLFORGE_IS_MSVC_LIKE)
    set(ICLFORGE_CPU_PROBE_DIR "msvc")
else()
    set(ICLFORGE_CPU_PROBE_DIR "builtin")
endif()

# tests/CMakeLists.txt compiles cpu_features.cpp a SECOND time, directly into iclforge-tests, against these
# directories - exposed unconditionally, since has_avx2() correctly answering "false" on a generic or
# AVX2-disabled build is as worth testing as it answering "true".
set(ICLFORGE_CPU_FEATURES_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include" PARENT_SCOPE)
set(ICLFORGE_CPU_PROBE_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/variants/cpu-probe-${ICLFORGE_CPU_PROBE_DIR}" PARENT_SCOPE)
set(ICLFORGE_CPU_FEATURES_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/cpu_features.cpp" PARENT_SCOPE)
set(ICLFORGE_IS_MSVC_LIKE "${ICLFORGE_IS_MSVC_LIKE}" PARENT_SCOPE)

# Which iclforge/base/detail/profiling.hpp the zone markers resolve to: CMake chooses the directory, not
# an #ifdef in the header. The stage-timer variant belongs to the minimum-footprint profile only.
if(ICLFORGE_ENABLE_TRACY)
    set(_iclforge_profiling_dir "${CMAKE_CURRENT_SOURCE_DIR}/variants/profiling-tracy_enabled")
else()
    set(_iclforge_profiling_dir "${CMAKE_CURRENT_SOURCE_DIR}/variants/profiling-tracy_disabled")
endif()
set(ICLFORGE_PROFILING_STAGE_TIMERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/variants/profiling-stage_timers" PARENT_SCOPE)

# The minimum-footprint profile compiles the sources it needs itself (src/ac3/minimal.cmake), so no
# library is built here for it.
if(ICLFORGE_MINIMAL_DECODER OR ICLFORGE_MINIMAL_ENCODER)
    return()
endif()

iclforge_add_library(base
    SOURCES
        src/cpu_features.cpp
    PUBLIC_INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/variants/cpu-probe-${ICLFORGE_CPU_PROBE_DIR}"
        "${_iclforge_profiling_dir}"
    LINK_PRIVATE
        "$<BUILD_INTERFACE:iclforge::tracy>"
        "$<BUILD_INTERFACE:iclforge::fmt_private>")

# getenv (ICLFORGE_SIMD_TIER's override) is flagged by MSVC's CRT (C4996). Scoped to this one file
# rather than /wd4996 on the whole target.
if(ICLFORGE_IS_MSVC_LIKE)
    set_source_files_properties(src/cpu_features.cpp PROPERTIES COMPILE_OPTIONS "/wd4996")
endif()
