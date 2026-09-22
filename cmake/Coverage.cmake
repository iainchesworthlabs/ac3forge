# ---------------------------------------------------------------------------
# Coverage.cmake
#
# Defines an INTERFACE target `ac3::coverage` that, when AC3FORGE_ENABLE_COVERAGE
# is on, turns on gcov-style source-based coverage instrumentation (GCC/Clang)
# via --coverage. Off by default: only the dedicated linux-gcc-coverage preset
# turns it on, so normal dev/CI builds pay no instrumentation cost.
#
# Link it PRIVATE into every first-party target whose coverage should be
# measured - today that is every library component (forge, audio, signing,
# matroska, mp4, mpegts, the C API, ac3adm, admbridge) plus ac3cli and
# ac3tests.
#
# The distinction that matters, and that cost a measurement run to notice:
# LINKING an instrumented library gets a target the gcov RUNTIME, not
# instrumentation of its own sources. A PRIVATE link of this target lands in
# the library's INTERFACE_LINK_LIBRARIES as $<LINK_ONLY:ac3::coverage>, so
# --coverage reaches every downstream LINK line automatically (ac3perf/ac3bench
# link the instrumented ac3::forge with no ac3::coverage of their own and link
# fine) - but --coverage is target-scoped at COMPILE time, so a consumer's own
# .cpp files still compile without -fprofile-arcs and emit no .gcno. That is
# why ac3cli has to link this explicitly (apps/cli/CMakeLists.txt) now that
# tools/checks/coverage_report.sh gates it: without it a gcovr filter for
# apps/cli returns zero files, not a low percentage.
#
# The coverage preset still turns AC3FORGE_BUILD_EXAMPLES off, as a pure
# build-time saving: examples/ is documentation that happens to compile, over
# an API surface tests/ already covers, and each one is its own ctest process -
# see CMakePresets.json. Vendored third-party code
# (src/ac3adm's FetchContent'd libbw64/libadm) is deliberately NOT
# instrumented: these flags are target-scoped and nothing links ac3::coverage
# into those targets, and tools/checks/coverage_report.sh's filters are
# first-party-only anyway.
# ---------------------------------------------------------------------------

option(AC3FORGE_ENABLE_COVERAGE "Enable gcov/llvm-cov source coverage instrumentation" OFF)

add_library(ac3_coverage INTERFACE)
add_library(ac3::coverage ALIAS ac3_coverage)

if(AC3FORGE_ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # -fno-inline keeps line/branch attribution accurate for a Debug
        # build's already-unoptimized code; --coverage covers both -fprofile-
        # arcs and -ftest-coverage plus linking the gcov runtime.
        target_compile_options(ac3_coverage INTERFACE --coverage -fno-inline)
        target_link_options(ac3_coverage INTERFACE --coverage)
    else()
        message(WARNING
            "AC3FORGE_ENABLE_COVERAGE is on but ${CMAKE_CXX_COMPILER_ID} is not "
            "GCC/Clang; coverage instrumentation is not supported on this "
            "compiler and will be skipped.")
    endif()
endif()
