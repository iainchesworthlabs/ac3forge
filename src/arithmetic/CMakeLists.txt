# ---------------------------------------------------------------------------
# ac3::arithmetic - the scalar building blocks the decoder's arithmetic tiers
# share: Fixed32 (ac3/internal/fixed32.hpp, the Q7.24 fixed-point tier's
# type) and the project's own cross-platform-identical float transcendentals
# (ac3/internal/scalar_math.hpp). Header-only, so there is nothing to
# compile here - just a target that carries the include path.
#
# Why this is its own target rather than living inside ac3::forge, where both
# files used to be (src/forge/src/core/{fixed32,scalar_math}.hpp): src/ac4core
# needs Fixed32 too, for the same reason ac3::forge does (a fixed-point tier
# with no floating-point unit, planning/ac4.md's D14), and a decoder that
# linked ac3::forge to get it would bring the whole AC-3/E-AC-3 codec into
# every AC-4 build (src/ac4dec/CMakeLists.txt's own header explains why AC-4
# links none of it). Copying the two files into src/ac4core instead was the
# other option and was rejected: planning/ac4.md's decision 31 is explicit
# that a copied Fixed32 would drift from the original. So both consumers
# depend on this one small target instead - "no copies" made structural
# rather than a rule to remember.
#
# Not installed and not exported: like ac3::warnings and the other interface
# targets forge_objects links $<BUILD_INTERFACE:...>-wrapped, this is
# in-tree build plumbing, not part of what a downstream consumer of the
# installed package needs to reach.
# ---------------------------------------------------------------------------
add_library(ac3_arithmetic INTERFACE)
add_library(ac3::arithmetic ALIAS ac3_arithmetic)

target_include_directories(ac3_arithmetic INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")

target_compile_features(ac3_arithmetic INTERFACE cxx_std_23)


# ---------------------------------------------------------------------------
# The target architecture, resolved once for everything that keys on it.
# ---------------------------------------------------------------------------
# CMAKE_SYSTEM_PROCESSOR names the machine CMake was configured for, which on
# Apple platforms is NOT the same thing as the machine the compiler is asked to
# emit code for: CMAKE_OSX_ARCHITECTURES overrides it per compile line. An
# Intel Mac cross-building an arm64 wheel (cibuildwheel does exactly this - see
# python/pyproject.toml and .github/workflows/wheels.yml) still reports
# CMAKE_SYSTEM_PROCESSOR=x86_64 while every compile line carries `-arch arm64`,
# so anything keyed on the raw variable - the SIMD directory and the AVX2 tier
# below both are - silently picks the HOST's answer and hands the compiler an
# x86 flag for an ARM target. That is a hard error, not a slow path -
# `clang++: error: unsupported option '-mavx2' for target
# 'x86_64-apple-darwin24.6.0'`, a diagnostic that unhelpfully names the host
# triple rather than the -arch the flag is actually invalid for.
#
# `-arch` therefore wins wherever one is set. More than one of them is a
# universal binary, where the same source is compiled once per slice from a
# single configure: no compile-time SIMD choice can be right for both slices,
# so that case gets its own name and lands on generic below rather than
# guessing at whichever slice happens to be listed first.
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    list(LENGTH CMAKE_OSX_ARCHITECTURES AC3FORGE_OSX_ARCH_COUNT)
    if(AC3FORGE_OSX_ARCH_COUNT GREATER 1)
        set(AC3FORGE_TARGET_PROCESSOR "universal")
    else()
        set(AC3FORGE_TARGET_PROCESSOR "${CMAKE_OSX_ARCHITECTURES}")
    endif()
    unset(AC3FORGE_OSX_ARCH_COUNT)
else()
    set(AC3FORGE_TARGET_PROCESSOR "${CMAKE_SYSTEM_PROCESSOR}")
endif()

if(AC3FORGE_TARGET_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    set(AC3FORGE_BUILD_ARCH "x86_64")
elseif(AC3FORGE_TARGET_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
    set(AC3FORGE_BUILD_ARCH "arm64")
elseif(AC3FORGE_TARGET_PROCESSOR MATCHES "^(x86|i686|X86)$")
    set(AC3FORGE_BUILD_ARCH "x86")
else()
    set(AC3FORGE_BUILD_ARCH "${AC3FORGE_TARGET_PROCESSOR}")
endif()

# ---------------------------------------------------------------------------
# The SIMD arch tree.
#
# The mechanism src/internal/profiling/tracy_{enabled,disabled}/ uses for the
# profiling seam, on the architecture axis: arch/{generic,x86_64,aarch64}/ each
# hold an identically-pathed ac3/internal/arch/simd.hpp, one of them goes on the
# include path, and every `#include "ac3/internal/arch/simd.hpp"` in the
# codec resolves to it. No translation unit ever asks what it is being
# compiled for - the rule tools/checks/check_platform_macros.ps1 enforces -
# and adding an architecture means adding a directory, not editing
# conditionals in the kernels.
#
# The kernels themselves are NOT duplicated per directory. They live once,
# in shared code (ac3::forge's src/core/mdct.cpp, bitalloc.cpp and exponents.cpp,
# src/ac4core's src/dsp/qmf_vector.hpp), written against the 128-bit vector types
# the header defines; the directories carry the types and their operations. See
# the generic header for why that is what makes the bit-exactness claim testable.
#
# The directories were src/forge/src/internal/arch/ until src/ac4core's QMF
# banks took vector kernels of their own (planning/ac4.md, D14a, and decision 31's
# "the vector types" in a header-only target both consumers link): this target
# carries the include path, so linking ac3::arithmetic is all a consumer does.
#
# AC3FORGE_SIMD forces a directory. `generic` in particular is what a
# reproducibility comparison wants (is a difference between two machines the
# vector kernels or something else?), and is the reference
# tests/core/test_simd_kernels.cpp's scalar side is written to match.
# ---------------------------------------------------------------------------
set(AC3FORGE_SIMD "auto" CACHE STRING
    "SIMD kernel directory: auto (from the effective target architecture), generic, x86_64, or aarch64")
set_property(CACHE AC3FORGE_SIMD PROPERTY STRINGS auto generic x86_64 aarch64)

if(AC3FORGE_SIMD STREQUAL "auto")
    # AC3FORGE_BUILD_ARCH is resolved above, from the effective target
    # architecture (CMAKE_SYSTEM_PROCESSOR, or CMAKE_OSX_ARCHITECTURES where a
    # cross-build sets one); reuse it rather than spelling the platform regexes a
    # second time and letting the two drift - and rather than reading
    # CMAKE_SYSTEM_PROCESSOR directly,
    # which is the host's answer on an Apple cross-build and was how an
    # arm64 wheel built on an Intel runner ended up being handed -mavx2.
    # Anything that is not one of the two architectures with a directory -
    # 32-bit x86, WebAssembly (Emscripten reports "x86"), a macOS universal
    # binary, a soft-float embedded target - lands on generic, which is a
    # complete implementation and not a stub.
    if(AC3FORGE_BUILD_ARCH STREQUAL "x86_64")
        set(AC3FORGE_SIMD_RESOLVED "x86_64")
    elseif(AC3FORGE_BUILD_ARCH STREQUAL "arm64")
        set(AC3FORGE_SIMD_RESOLVED "aarch64")
    else()
        set(AC3FORGE_SIMD_RESOLVED "generic")
    endif()
else()
    set(AC3FORGE_SIMD_RESOLVED "${AC3FORGE_SIMD}")
endif()

set(AC3FORGE_ARCH_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/arch/${AC3FORGE_SIMD_RESOLVED}")
if(NOT IS_DIRECTORY "${AC3FORGE_ARCH_INCLUDE_DIR}")
    message(FATAL_ERROR
        "AC3FORGE_SIMD=${AC3FORGE_SIMD} resolved to '${AC3FORGE_SIMD_RESOLVED}', which has no "
        "directory under src/arithmetic/arch/. Add "
        "src/arithmetic/arch/<name>/ac3/internal/arch/simd.hpp - the generic/ copy is "
        "the portable implementation and is a working starting point.")
endif()

# Every consumer of ac3::arithmetic (ac3::forge, forge_minimal, src/ac4core) gets the
# one directory through the target.
target_include_directories(ac3_arithmetic INTERFACE "$<BUILD_INTERFACE:${AC3FORGE_ARCH_INCLUDE_DIR}>")

# tests/CMakeLists.txt reads this so test_simd_kernels.cpp sees the same
# simd.hpp the library was built with - the same white-box arrangement
# src/signing/src and AC3FORGE_AUDIO_BACKEND_DIR already have there.
set(AC3FORGE_ARCH_INCLUDE_DIR "${AC3FORGE_ARCH_INCLUDE_DIR}" PARENT_SCOPE)
# For the configure summary at the end of the top-level CMakeLists.txt - which
# a minimum-footprint configure reaches too, so that summary names the
# directory that build actually compiled rather than assuming generic.
set(AC3FORGE_SIMD_RESOLVED "${AC3FORGE_SIMD_RESOLVED}" PARENT_SCOPE)
# For ac3::forge's version strings and its AVX2 tier, which keyed on the same answer
# before this file resolved it.
set(AC3FORGE_BUILD_ARCH "${AC3FORGE_BUILD_ARCH}" PARENT_SCOPE)
