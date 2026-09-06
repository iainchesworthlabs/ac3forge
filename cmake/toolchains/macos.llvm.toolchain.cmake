#------------------------------------------------------------------------------
# macOS LLVM/Clang Toolchain Configuration
#
# Chainloaded by the config-macos-llvm* presets via
# VCPKG_CHAINLOAD_TOOLCHAIN_FILE. LLVM 22 is the pinned version.
#
# Deliberately prefers Homebrew/MacPorts LLVM over Apple's bundled clang: the
# codec leans on C++23 library features (std::to_chars, <format>, ranges) whose
# availability in Apple's libc++ is gated on the deployment target. Where an
# LLVM libc++ is found it is used in place of the SDK's for exactly that reason.
#
# NOTE: written from the aqualink-automate reference; this project still has
# no local macOS host, but CI (.github/workflows/_build.yml's macos-llvm leg,
# on macos-latest/Apple Silicon) has run this for real and found the Homebrew
# LLVM it's written to find - see docs/building.md's Verified configuration
# section for the actual numbers.
#------------------------------------------------------------------------------

message(STATUS "Configuring macOS Toolchain (LLVM/Clang Variant)")

set(CMAKE_SYSTEM_NAME Darwin)

# Resolve the target architecture. vcpkg sets VCPKG_TARGET_ARCHITECTURE only
# while building a port; in the project's own configure scope fall back to the
# host, so an Apple Silicon machine does not quietly build x86_64.
if(VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64")
    set(_MACOS_ARCH "arm64")
elseif(VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
    set(_MACOS_ARCH "x86_64")
else()
    execute_process(
        COMMAND uname -m
        OUTPUT_VARIABLE _HOST_ARCH
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_HOST_ARCH STREQUAL "arm64")
        set(_MACOS_ARCH "arm64")
    else()
        set(_MACOS_ARCH "x86_64")
    endif()
endif()

set(CMAKE_SYSTEM_PROCESSOR ${_MACOS_ARCH})

# Drive the architecture through CMAKE_OSX_ARCHITECTURES as well as the raw
# -arch flag. With only the compile flag set, CMake runs its own checks against
# the SDK/host architecture and an arch/SDK mismatch goes unnoticed until link.
set(CMAKE_OSX_ARCHITECTURES "${_MACOS_ARCH}" CACHE STRING "Target macOS architecture")

message(STATUS "Target architecture: ${CMAKE_SYSTEM_PROCESSOR}")

set(_LLVM_BIN_HINTS
    "/opt/homebrew/opt/llvm/bin"
    "/usr/local/opt/llvm/bin"
    "/opt/local/libexec/llvm-22/bin"
    "/opt/local/bin"
    "/opt/homebrew/bin"
    "/usr/local/bin"
    "/usr/bin")

find_program(CMAKE_C_COMPILER
    NAMES clang-22 clang
    HINTS ${_LLVM_BIN_HINTS}
    REQUIRED)

find_program(CMAKE_CXX_COMPILER
    NAMES clang++-22 clang++
    HINTS ${_LLVM_BIN_HINTS}
    REQUIRED)

unset(_LLVM_BIN_HINTS)

# No LLD on macOS. LLVM ships ld64.lld, but autotools-based vcpkg ports drive
# libtool, which emits the ELF -Wl,-soname instead of the Mach-O
# -Wl,-install_name once it detects lld. Apple's system linker works.

# 13.3 is the floor for unrestricted std::to_chars in Apple's libc++.
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING "Minimum macOS version")

execute_process(
    COMMAND xcrun --show-sdk-path
    OUTPUT_VARIABLE CMAKE_OSX_SYSROOT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)

if(CMAKE_OSX_SYSROOT)
    message(STATUS "Using macOS SDK: ${CMAKE_OSX_SYSROOT}")
endif()

set(CMAKE_C_FLAGS_INIT "-arch ${_MACOS_ARCH}")
set(CMAKE_CXX_FLAGS_INIT "-arch ${_MACOS_ARCH}")
# Objective-C++ is a C++ dialect and has to be given the same C++ standard
# library, or one target ends up with two. Added 2026-09-06, when
# apps/crucible/CMakeLists.txt's APPLE arm became the first enable_language(OBJCXX)
# in the tree: OBJCXX picks up CMAKE_OSX_ARCHITECTURES on its own, but nothing
# copies CMAKE_CXX_FLAGS_INIT across, so without this the .mm halves of
# ac3crucible_engine and ac3crucible would compile against whichever libc++ the
# compiler defaults to while every .cpp beside them uses the one selected
# below. Inert on a configure that never enables the language. Untested: no
# macOS build of Crucible has been run from this tree.
set(CMAKE_OBJCXX_FLAGS_INIT "-arch ${_MACOS_ARCH}")

# Prefer LLVM's own libc++ over the SDK's when the chosen clang ships one.
# _LIBCPP_DISABLE_AVAILABILITY drops the vendor availability annotations, which
# otherwise gate library features on the deployment target even though we link
# against a runtime that has every symbol.
get_filename_component(_LLVM_COMPILER_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
get_filename_component(_LLVM_PREFIX "${_LLVM_COMPILER_DIR}" DIRECTORY)

set(_LLVM_LIBCXX_INCLUDE "${_LLVM_PREFIX}/include/c++/v1")
set(_LLVM_LIBCXX_LIB "${_LLVM_PREFIX}/lib")

if(EXISTS "${_LLVM_LIBCXX_INCLUDE}")
    message(STATUS "Using LLVM libc++ headers: ${_LLVM_LIBCXX_INCLUDE}")

    string(APPEND CMAKE_CXX_FLAGS_INIT
        " -nostdinc++ -isystem ${_LLVM_LIBCXX_INCLUDE} -D_LIBCPP_DISABLE_AVAILABILITY")
    # The same three for Objective-C++ - see the CMAKE_OBJCXX_FLAGS_INIT note
    # above. Mismatched availability annotations between the two languages are
    # the half of this that would fail quietly: the headers would still be
    # found, and only some inline definition would differ.
    string(APPEND CMAKE_OBJCXX_FLAGS_INIT
        " -nostdinc++ -isystem ${_LLVM_LIBCXX_INCLUDE} -D_LIBCPP_DISABLE_AVAILABILITY")

    set(CMAKE_EXE_LINKER_FLAGS_INIT
        "-L${_LLVM_LIBCXX_LIB}/c++ -L${_LLVM_LIBCXX_LIB} -Wl,-rpath,${_LLVM_LIBCXX_LIB}/c++ -Wl,-rpath,${_LLVM_LIBCXX_LIB}")
    set(CMAKE_SHARED_LINKER_FLAGS_INIT "${CMAKE_EXE_LINKER_FLAGS_INIT}")
else()
    # Apple's clang - fall back to the SDK libc++. C++23 library coverage then
    # depends on the Xcode version rather than on LLVM 22.
    message(STATUS "No LLVM libc++ at ${_LLVM_LIBCXX_INCLUDE}; using the SDK's")
    add_compile_options(-stdlib=libc++)
    add_link_options(-stdlib=libc++)
endif()

add_compile_options(-fcolor-diagnostics)

message(STATUS "Using LLVM/Clang at: ${CMAKE_CXX_COMPILER}")
message(STATUS "macOS deployment target: ${CMAKE_OSX_DEPLOYMENT_TARGET}")
