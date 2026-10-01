# ---------------------------------------------------------------------------
# iclforge-perf - real-time throughput regression guards (hard pass/fail). See
# test_performance.cpp for why this is a separate target from iclforge-tests, and
# why the whole thing exists at all (a ~28x, zero-test-coverage regression in
# the forward MDCT, found profiling the Shield Android app with Tracy).
#
# Catch2 is already resolved by the parent tests/CMakeLists.txt before this
# subdirectory is added, so this file only defines the target.
# ---------------------------------------------------------------------------
add_executable(iclforge-perf test_performance.cpp)

# The AC-4 cases need no guard: the root CMakeLists.txt refuses
# ICLFORGE_BUILD_TESTS without ICLFORGE_BUILD_AC4, so the AC-4 libraries exist
# wherever this directory is built. The same holds for the three targets below.
target_link_libraries(iclforge-perf
    PRIVATE
        iclforge::ac3
        iclforge::ac4enc
        iclforge::ac4dec
        iclforge::warnings
        iclforge::fmt
        Catch2::Catch2WithMain)

target_include_directories(iclforge-perf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# real_audio.hpp resolves tests/golden/audio's fixtures through this, so the
# gate reads real programme material whatever directory ctest launches it
# from - see that header for why a synthetic tone is not an acceptable
# substitute for it.
target_compile_definitions(iclforge-perf PRIVATE
    "ICLFORGE_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")

# TEST_PREFIX so `ctest -R '^Performance::'`/`-E` can target this target's
# cases specifically without a fragile name-based guess, and
# `-L Performance` for the same reason via PROPERTIES below - see
# CMakePresets.json's test-linux-llvm-asan-ubsan preset, which excludes this
# exact label (an instrumented build's timing is not a signal worth having
# an opinion about, at any slack factor).
catch_discover_tests(iclforge-perf
    TEST_PREFIX "Performance::"
    PROPERTIES LABELS "Performance")

# ---------------------------------------------------------------------------
# iclforge-bench - the trend-tracking half (numbers, not a threshold). Not a
# ctest-registered test at all: see bench_encoder.cpp's own header comment
# and docs/performance-trend.md for how its --json-out output feeds
# tools/ci/append_performance_history.py, the same relationship
# tools/checks/verify_gold_reference.sh's --json-out has to
# append_quality_history.py.
# ---------------------------------------------------------------------------
add_executable(iclforge-bench bench_encoder.cpp)

target_link_libraries(iclforge-bench PRIVATE iclforge::ac3 iclforge::ac4enc iclforge::ac4dec iclforge::warnings iclforge::fmt)

target_include_directories(iclforge-bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Same real-audio fixture the kernel bench and the gate use, for the same
# reason: a 440 Hz tone leaves the SNR search, coupling, rematrixing and the
# block-switched transform out of the measurement entirely.
target_compile_definitions(iclforge-bench PRIVATE
    "ICLFORGE_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")

# ---------------------------------------------------------------------------
# iclforge-membench - heap-churn and footprint numbers for the same workloads as
# iclforge-bench, plus the decode paths iclforge-bench never covered. Links
# iclforge::ac3_static explicitly so the global operator new/delete
# replacements in bench_memory.cpp observe the codec's allocations too: on
# Windows a DLL's allocations bind to the DLL's own operator new, so
# linking the shared library would count nothing. The allocator
# introspection and peak-RSS probe live in platform/{windows,linux}/
# (the platform-tree rule, as everywhere else). Not ctest-registered, same
# reasoning as iclforge-bench above: nothing here asserts anything, and its JSON
# feeds tools/ci/append_memory_history.py (docs/performance-trend.md's
# memory tables) from the persist-performance-trend CI job. macOS would
# need a mem_probe of its own (malloc_size; ru_maxrss in bytes, not
# Linux's KiB) - the trend job only measures linux-gcc, so the target
# simply does not exist there yet rather than carrying an untested port.
# ---------------------------------------------------------------------------
#
# Not built on the ThreadSanitizer leg. The global operator
# new/delete replacements above are the whole mechanism of this target, and
# TSan's own runtime defines those same operators in
# libclang_rt.tsan_cxx-x86_64.a - so linking it under -fsanitize=thread is a
# hard "ld.lld: error: duplicate symbol: operator new(unsigned long)", not a
# warning. Measured, not assumed: the ASan+UBSan leg links this target today
# and always has, so the collision is specific to TSan's runtime rather than
# to sanitizers in general.
#
# Nothing is lost by skipping it there. This target exists to count the
# codec's own allocations for the memory trend
# (tools/ci/append_memory_history.py), and a count taken through a
# sanitizer's intercepting allocator would not be the codec's number anyway -
# the same reason the sanitizer legs are exempt from performance gating.
if((WIN32 OR LINUX) AND NOT ICLFORGE_SANITIZERS MATCHES "thread")
    add_executable(iclforge-membench bench_memory.cpp)

    if(WIN32)
        target_sources(iclforge-membench PRIVATE platform/windows/mem_probe.cpp)
    else()
        target_sources(iclforge-membench PRIVATE platform/linux/mem_probe.cpp)
    endif()

    target_include_directories(iclforge-membench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    # The AC-4 libraries' static builds, for the same reason as
    # ac3::forge_static: a DLL's allocations would not reach the counters.
    target_link_libraries(iclforge-membench PRIVATE
        iclforge::ac3_static iclforge::ac4enc_static iclforge::ac4dec_static iclforge::warnings iclforge::fmt)

    # The AC-4 workloads read the real-audio fixture (real_audio.hpp).
    target_compile_definitions(iclforge-membench PRIVATE
        "ICLFORGE_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")
endif()

# ---------------------------------------------------------------------------
# iclforge-kernelbench - per-kernel micro-benchmarks, one level below iclforge-bench's
# whole-frame numbers. Where iclforge-bench answers "how fast is a frame", this
# answers "which stage of that frame" - the observability slice of the
# performance programme these three both belong to (Tracy zones give the
# real-time breakdown, this gives an offline one that needs no profiler
# attached). Not ctest-registered, same reasoning as iclforge-bench above: nothing
# here asserts anything, and its JSON feeds tools/ci/append_kernel_history.py
# (docs/performance-trend.md's per-kernel tables) as a trend signal, never a
# CI gate - see that script's docstring for why it is deliberately
# non-gating.
# ---------------------------------------------------------------------------
add_executable(iclforge-kernelbench kernel_bench.cpp)

# iclforge::ac4core (static in every build) brings src/ac4core/include/iclforge/ac4core's transform headers,
# as it does for tests/ac4core.
target_link_libraries(iclforge-kernelbench PRIVATE iclforge::ac3 iclforge::ac4core iclforge::warnings iclforge::fmt)

target_include_directories(iclforge-kernelbench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# So the binary can find tests/golden/audio's real-audio fixtures regardless
# of the working directory it is launched from - kernel inputs synthesized
# from silence or a single tone give misleadingly tidy bit-allocation/
# mantissa-cost numbers real program material never produces.
target_compile_definitions(iclforge-kernelbench PRIVATE
    "ICLFORGE_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")