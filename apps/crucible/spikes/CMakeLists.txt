# Phase 0 spikes for the Windows AC3Forge Crucible (docs/platforms/windows-demo.md).
#
# Standalone on purpose: these are throwaway experiments that answer one question
# each and are never linked into the library or the app. Configure with the repo's
# own MSVC toolchain so no Developer Prompt is needed:
#
#   cmake -S apps/crucible/spikes -B <build> -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/windows.msvc.toolchain.cmake
#   cmake --build <build>
cmake_minimum_required(VERSION 3.25)
project(ac3forge_windows_spikes LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(tone_player s1_taps/tone_player.cpp)
add_executable(s1_taps s1_taps/s1_taps.cpp)
add_executable(s5_latency s5_latency/s5_latency.cpp)
add_executable(period_probe s5_latency/period_probe.cpp)

foreach(target tone_player s1_taps s5_latency period_probe)
    target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_compile_options(${target} PRIVATE /W4 /EHsc /permissive- /utf-8)
    # ActivateAudioInterfaceAsync lives in mmdevapi.lib; PKEY_* property keys need
    # the propsys import; avrt is for the MMCSS registration on the capture threads.
    target_link_libraries(${target} PRIVATE mmdevapi ole32 oleaut32 propsys avrt)
endforeach()

# S4 needs the real encoder. Pull the repo root in the way the Android app does
# (everything it does not need pre-seeded OFF, then add_subdirectory), so the
# spike measures the same ac3::forge the demo will link.
option(AC3FORGE_SPIKES_WITH_LIBRARY "Build the spikes that link ac3::forge (S4)" ON)
if(AC3FORGE_SPIKES_WITH_LIBRARY)
    foreach(opt CLI GUI TESTS EXAMPLES FUZZERS CAPI MATROSKA MP4 MPEGTS AC4 IAB IAMF ADM PYTHON)
        set(AC3FORGE_BUILD_${opt} OFF CACHE BOOL "" FORCE)
    endforeach()
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../.." ac3forge_build)
    add_executable(s4_throughput s4_throughput/s4_throughput.cpp)
    target_compile_options(s4_throughput PRIVATE /W4 /EHsc /permissive- /utf-8)
    target_link_libraries(s4_throughput PRIVATE ac3::forge)
endif()

# S1 part two: the same tap through the library's own Capture/DeviceWatcher.
if(AC3FORGE_SPIKES_WITH_LIBRARY)
    add_executable(s1_library_tap s1_taps/s1_library_tap.cpp)
    target_compile_definitions(s1_library_tap PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_compile_options(s1_library_tap PRIVATE /W4 /EHsc /permissive- /utf-8)
    target_link_libraries(s1_library_tap PRIVATE ac3::audio)
endif()
