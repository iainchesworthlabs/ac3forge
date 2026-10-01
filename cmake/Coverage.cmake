# ---------------------------------------------------------------------------
# Coverage.cmake
#
# Defines an INTERFACE target `iclforge::coverage` that, when ICLFORGE_ENABLE_COVERAGE
# is on, turns on gcov-style source-based coverage instrumentation (GCC/Clang)
# via --coverage. Off by default: only the dedicated linux-gcc-coverage preset
# turns it on, so normal dev/CI builds pay no instrumentation cost.
#
# Link it PRIVATE into every first-party target whose coverage should be
# measured - today that is every library component (forge, audio, signing,
# matroska, mp4, mpegts, the C API, ac3adm, admbridge) plus forge and
# iclforge-tests.
#
# The distinction that matters, and that cost a measurement run to notice:
# LINKING an instrumented library gets a target the gcov RUNTIME, not
# instrumentation of its own sources. A PRIVATE link of this target lands in
# the library's INTERFACE_LINK_LIBRARIES as $<LINK_ONLY:iclforge::coverage>, so
# --coverage reaches every downstream LINK line automatically (iclforge-perf/iclforge-bench
# link the instrumented iclforge::ac3 with no iclforge::coverage of their own and link
# fine) - but --coverage is target-scoped at COMPILE time, so a consumer's own
# .cpp files still compile without -fprofile-arcs and emit no .gcno. That is
# why forge has to link this explicitly (apps/cli/CMakeLists.txt) now that
# tools/checks/coverage_report.sh gates it: without it a gcovr filter for
# apps/cli returns zero files, not a low percentage.
#
# The coverage preset still turns ICLFORGE_BUILD_EXAMPLES off, as a pure
# build-time saving: examples/ is documentation that happens to compile, over
# an API surface tests/ already covers, and each one is its own ctest process -
# see CMakePresets.json. Vendored third-party code
# (src/adm's FetchContent'd libbw64/libadm) is deliberately NOT
# instrumented: these flags are target-scoped and nothing links iclforge::coverage
# into those targets, and tools/checks/coverage_report.sh's filters are
# first-party-only anyway.
# ---------------------------------------------------------------------------

option(ICLFORGE_ENABLE_COVERAGE "Enable gcov/llvm-cov source coverage instrumentation" OFF)

add_library(iclforge_coverage INTERFACE)
add_library(iclforge::coverage ALIAS iclforge_coverage)

if(ICLFORGE_ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        # clang-cl (the config-windows-llvm-coverage preset): LLVM's own
        # source-based coverage rather than gcov, because that is what
        # clang-cl supports and because llvm-cov reports BRANCH coverage,
        # which the Windows-only code (apps/windows) has no other way of
        # getting. Two things differ from the GCC arm: the link step is
        # MSVC-style and never goes through the compiler driver, so the
        # profile runtime has to be named explicitly (it lives in clang's
        # resource directory); and there is no -fno-inline spelling here, a
        # Debug build's /Od already keeps functions intact. Read the result
        # with tools/checks/coverage_crucible.ps1 (LLVM_PROFILE_FILE,
        # llvm-profdata merge, llvm-cov report).
        # /Od /Ob0 after the release base's /O2 (the last flag wins): the
        # preset is release-based because clang_rt.profile is built against
        # the release CRT and a /MDd binary dies in the profile writer at
        # exit, but attribution wants unoptimised code, as the GCC arm's
        # Debug build gives it for free.
        target_compile_options(iclforge_coverage INTERFACE
            -fprofile-instr-generate -fcoverage-mapping /Od /Ob0)
        execute_process(
            COMMAND "${CMAKE_CXX_COMPILER}" -print-resource-dir
            OUTPUT_VARIABLE AC3_CLANG_RESOURCE_DIR
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        file(TO_CMAKE_PATH "${AC3_CLANG_RESOURCE_DIR}" AC3_CLANG_RESOURCE_DIR)
        find_library(AC3_CLANG_PROFILE_RUNTIME
            NAMES clang_rt.profile-x86_64 clang_rt.profile-aarch64
            PATHS "${AC3_CLANG_RESOURCE_DIR}/lib/windows"
            NO_DEFAULT_PATH)
        if(NOT AC3_CLANG_PROFILE_RUNTIME)
            message(FATAL_ERROR
                "ICLFORGE_ENABLE_COVERAGE with clang-cl needs the profile runtime "
                "(clang_rt.profile-*.lib) under ${AC3_CLANG_RESOURCE_DIR}/lib/windows; "
                "it was not found.")
        endif()
        target_link_libraries(iclforge_coverage INTERFACE "${AC3_CLANG_PROFILE_RUNTIME}")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # -fno-inline keeps line/branch attribution accurate for a Debug
        # build's already-unoptimized code; --coverage covers both -fprofile-
        # arcs and -ftest-coverage plus linking the gcov runtime.
        target_compile_options(iclforge_coverage INTERFACE --coverage -fno-inline)
        target_link_options(iclforge_coverage INTERFACE --coverage)
    else()
        message(WARNING
            "ICLFORGE_ENABLE_COVERAGE is on but ${CMAKE_CXX_COMPILER_ID} is not "
            "GCC/Clang; coverage instrumentation is not supported on this "
            "compiler and will be skipped.")
    endif()
endif()
