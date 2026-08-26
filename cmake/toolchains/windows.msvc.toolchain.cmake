#------------------------------------------------------------------------------
# Windows MSVC Toolchain Configuration
#
# Chainloaded by the config-windows-msvc* presets via
# VCPKG_CHAINLOAD_TOOLCHAIN_FILE. The compiler is pinned here rather than left
# to PATH discovery: cl.exe is never on PATH outside a Developer PowerShell,
# so "whatever CMake finds" resolves to a different compiler entirely.
#------------------------------------------------------------------------------

message(STATUS "Configuring Windows Toolchain (MSVC Variant)")

include("${CMAKE_CURRENT_LIST_DIR}/windows.msvc.environment.cmake")

# AC3_MSVC_TARGET_ARCH ("x64" or "arm64") is resolved by
# windows.msvc.environment.cmake, shared with windows.llvm.toolchain.cmake, so
# the vcvarsall import above and the compiler/linker directories picked below
# always agree on which architecture they mean.
#
# x64 is a single candidate, unchanged from before this file supported more
# than one target: Hostx64/x64 must resolve exactly as it did previously, so
# the other three Windows CI legs (windows-msvc[-debug], windows-llvm) don't
# regress. arm64 is new and, at authoring time, genuinely unconfirmed against
# real hardware: it is unknown whether GitHub's hosted windows-11-arm runner's
# VS Build Tools install ships a native Hostarm64/arm64 toolset, only the
# cross Hostx64/arm64 one, or both - see docs/platforms/windows.md's ARM64
# section for what a real CI run found. find_program tries each PATHS entry
# in the order given and returns the first match, so listing the native
# directory first gets it for free when it exists, without a separate EXISTS
# probe - try native first (no emulation), fall back to cross.
if(AC3_MSVC_TARGET_ARCH STREQUAL "arm64")
    set(CMAKE_SYSTEM_PROCESSOR "ARM64")
    set(_MSVC_BIN_CANDIDATES
        "${AC3_MSVC_TOOLS_DIR}/bin/Hostarm64/arm64"
        "${AC3_MSVC_TOOLS_DIR}/bin/Hostx64/arm64")
else()
    set(CMAKE_SYSTEM_PROCESSOR "AMD64")
    set(_MSVC_BIN_CANDIDATES "${AC3_MSVC_TOOLS_DIR}/bin/Hostx64/x64")
endif()

find_program(CMAKE_C_COMPILER
    NAMES cl.exe
    PATHS ${_MSVC_BIN_CANDIDATES}
    NO_DEFAULT_PATH
    REQUIRED)

find_program(CMAKE_CXX_COMPILER
    NAMES cl.exe
    PATHS ${_MSVC_BIN_CANDIDATES}
    NO_DEFAULT_PATH
    REQUIRED)

# CMake drives the MSVC link step through CMAKE_LINKER directly. Pin it to the
# toolset's own link.exe: Git for Windows ships an unrelated /usr/bin/link.exe
# that shadows it whenever Git's tools are earlier on PATH.
find_program(CMAKE_LINKER
    NAMES link.exe
    PATHS ${_MSVC_BIN_CANDIDATES}
    NO_DEFAULT_PATH
    REQUIRED)

message(STATUS "Target architecture: ${CMAKE_SYSTEM_PROCESSOR} (resolved MSVC tools directory: ${CMAKE_C_COMPILER})")

unset(_MSVC_BIN_CANDIDATES)

# This toolchain is only selected when MSVC is the active compiler for both
# languages, so the flags apply unconditionally rather than behind a redundant
# per-language $<CXX_COMPILER_ID:MSVC> generator expression.
#
# /utf-8       sources carry spec citations with non-ASCII glyphs (section
#              marks, degrees, arrows); tell cl both the source and execution
#              charsets are UTF-8 so it neither mis-decodes them nor warns.
# /bigobj      the codec's constant tables (bit-allocation, E-AC-3, AHT, JOC)
#              are large enough that a translation unit including several of
#              them can exceed the default section limit (C1128).
add_compile_options("/utf-8")
add_compile_options("/bigobj")

message(STATUS "Using MSVC at: ${CMAKE_CXX_COMPILER}")
