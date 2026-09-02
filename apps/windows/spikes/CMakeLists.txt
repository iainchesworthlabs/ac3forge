# Phase 0 spikes for the Windows Desktop Atmos Demo (docs/platforms/windows-demo.md).
#
# Standalone on purpose: these are throwaway experiments that answer one question
# each and are never linked into the library or the app. Configure with the repo's
# own MSVC toolchain so no Developer Prompt is needed:
#
#   cmake -S apps/windows/spikes -B <build> -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/windows.msvc.toolchain.cmake
#   cmake --build <build>
cmake_minimum_required(VERSION 3.25)
project(ac3forge_windows_spikes LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(tone_player s1_taps/tone_player.cpp)
add_executable(s1_taps s1_taps/s1_taps.cpp)

foreach(target tone_player s1_taps)
    target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_compile_options(${target} PRIVATE /W4 /EHsc /permissive- /utf-8)
    # ActivateAudioInterfaceAsync lives in mmdevapi.lib; PKEY_* property keys need
    # the propsys import; avrt is for the MMCSS registration on the capture threads.
    target_link_libraries(${target} PRIVATE mmdevapi ole32 oleaut32 propsys avrt)
endforeach()
