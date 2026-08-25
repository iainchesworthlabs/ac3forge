# ---------------------------------------------------------------------------
# ac3::forge - the whole codec: bitstream, DSP, encoder, decoder, the spatial
# object layer and the IEC 61937 sink. No GUI, no third-party codec code (see
# CONTRIBUTING.md's "The clean-room rule") - and, deliberately, no
# platform-specific code at all: this target builds identically on any
# C++23 platform, with no WASAPI/ALSA dependency, so a third party linking it
# purely to encode/decode AC-3/E-AC-3/Atmos streams needs nothing else. Live
# capture, monitor playback and IEC 61937 delivery are ac3::audio
# (src/audio/), a separate target ac3cli/ac3gui link alongside this one -
# see that target's own header comment.
#
# Built and installed as BOTH a static and a shared library - a distributed
# package can't know in advance which a downstream consumer wants. One
# OBJECT library compiles the sources exactly once; forge_static and
# forge_shared are thin archive/DLL wrappers around the same object code
# (a single add_library() can't be both STATIC and SHARED at once). In-tree
# consumers (ac3cli/ac3gui/tests) link the plain ac3::forge alias, which
# resolves to whichever variant BUILD_SHARED_LIBS asks for; an installed
# package has no such ambient default, so it exports both variants under
# their own names, ac3::forge_static / ac3::forge_shared.
#
# The raw target names are forge_objects/forge_static/forge_shared, not
# ac3forge_*: install(EXPORT ... NAMESPACE ac3::) prepends the namespace to
# whatever the target is actually named, so an ac3forge_static target would
# export as the stuttering ac3::ac3forge_static, not ac3::forge_static - the
# leading "ac3" only needs to appear once, either in the target's own name or
# the namespace, never both. (matroska::matroska's own targets don't have
# this problem: matroska_static under the matroska:: namespace already lands
# on the intended matroska::matroska_static with no stutter.)
# ---------------------------------------------------------------------------
# Roadmap PF7's minimum-footprint decoder profile builds something different
# enough - static only, decode only, no exceptions, no direct-form transform
# tables - that it gets its own file rather than a dozen conditionals through
# this one. Off by default; the root CMakeLists.txt refuses the combination of
# this option with anything that would need the full library.
if(AC3FORGE_MINIMAL_DECODER)
    include("${CMAKE_CURRENT_SOURCE_DIR}/minimal.cmake")
    return()
endif()

add_library(forge_objects OBJECT)
add_library(forge_static STATIC)
add_library(forge_shared SHARED)

target_link_libraries(forge_static PUBLIC forge_objects)
target_link_libraries(forge_shared PUBLIC forge_objects)

add_library(ac3::forge_static ALIAS forge_static)
add_library(ac3::forge_shared ALIAS forge_shared)
if(BUILD_SHARED_LIBS)
    add_library(ac3::forge ALIAS forge_shared)
else()
    add_library(ac3::forge ALIAS forge_static)
endif()

# --- Version + git stamp ---------------------------------------------------
find_package(Git QUIET)
set(AC3FORGE_VERSION_TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/include/ac3/version.hpp.in")
set(AC3FORGE_VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/ac3/version.hpp")

# OS/architecture/compiler this binary is built for, for version_details()'s
# --version/About-dialog output. Computed here (CMake already knows all three
# from the toolchain it configured) rather than read back with predefined
# macros at compile time, so version.cpp needs no #ifdef - see ac3::audio's
# platform tree (src/audio/CMakeLists.txt) and tools/checks/check_platform_macros.ps1.
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(AC3FORGE_BUILD_OS "macOS")
else()
    set(AC3FORGE_BUILD_OS "${CMAKE_SYSTEM_NAME}")
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    set(AC3FORGE_BUILD_ARCH "x86_64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
    set(AC3FORGE_BUILD_ARCH "arm64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86|i686|X86)$")
    set(AC3FORGE_BUILD_ARCH "x86")
else()
    set(AC3FORGE_BUILD_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(AC3FORGE_BUILD_COMPILER "GCC ${CMAKE_CXX_COMPILER_VERSION}")
else()
    # AppleClang, Clang and MSVC already spell their own CMAKE_CXX_COMPILER_ID
    # the way version_details() wants it.
    set(AC3FORGE_BUILD_COMPILER "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
endif()

set(AC3FORGE_BUILD_TARGET "${AC3FORGE_BUILD_OS} ${AC3FORGE_BUILD_ARCH} (${AC3FORGE_BUILD_COMPILER})")

# Generate once at configure time so the header exists for the first build.
set(AC3FORGE_VERSION "${PROJECT_VERSION}")
set(AC3FORGE_VERSION_FULL "${PROJECT_VERSION_FULL}")
set(SRC "${AC3FORGE_VERSION_TEMPLATE}")
set(DST "${AC3FORGE_VERSION_HEADER}")
set(WORKDIR "${PROJECT_SOURCE_DIR}")
include("${PROJECT_SOURCE_DIR}/cmake/GenerateVersion.cmake")

# Re-stamp on every build so the git information tracks new commits without a
# reconfigure. configure_file only rewrites when the content changes, so this
# does not needlessly recompile dependants.
add_custom_target(ac3forge_version ALL
    BYPRODUCTS "${AC3FORGE_VERSION_HEADER}"
    COMMAND "${CMAKE_COMMAND}"
        "-DAC3FORGE_VERSION=${PROJECT_VERSION}"
        "-DAC3FORGE_VERSION_FULL=${PROJECT_VERSION_FULL}"
        "-DAC3FORGE_BUILD_TARGET=${AC3FORGE_BUILD_TARGET}"
        "-DSRC=${AC3FORGE_VERSION_TEMPLATE}"
        "-DDST=${AC3FORGE_VERSION_HEADER}"
        "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
        "-DWORKDIR=${PROJECT_SOURCE_DIR}"
        -P "${PROJECT_SOURCE_DIR}/cmake/GenerateVersion.cmake"
    COMMENT "Stamping version and git information"
    VERBATIM)

# Everything here is portable: bitstream, DSP, containers, measurement. The
# OS-specific code is selected further down, out of this list.
target_sources(forge_objects
    PRIVATE
        src/analysis/levels.cpp
        src/core/bitalloc.cpp
        src/core/eac3_tables.cpp
        src/core/exponents.cpp
        src/core/fft.cpp
        src/core/mantissas.cpp
        src/core/mdct.cpp
        src/core/transform/reference/reference_transform.cpp
        src/decoder/decoder.cpp
        src/decoder/eac3_decoder.cpp
        src/decoder/output.cpp
        src/decoder/transient_prenoise.cpp
        src/dsp/biquad.cpp
        src/dsp/qmf.cpp
        src/dsp/resampler.cpp
        src/emdf/emdf.cpp
        src/emdf/frame_layout.cpp
        src/encoder/assignment.cpp
        src/encoder/bandwidth.cpp
        src/encoder/coupling.cpp
        src/encoder/eac3_frame.cpp
        src/encoder/eac3_tools.cpp
        src/encoder/encoder.cpp
        src/encoder/plan.cpp
        src/encoder/transient.cpp
        src/internal/cpu/cpu_features.cpp
        src/io/dec3.cpp
        src/io/elementary.cpp
        src/io/metadata_edit.cpp
        src/io/object_strip.cpp
        src/io/probe.cpp
        src/io/wav.cpp
        src/io/wav_format.cpp
        src/io/wav_stream_reader.cpp
        src/io/wav_stream_writer.cpp
        src/meta/bsi.cpp
        src/meta/drc.cpp
        src/meta/loudness.cpp
        src/meta/mixing.cpp
        src/meta/qc.cpp
        src/oba/atmos.cpp
        src/oba/joc.cpp
        src/oba/motion.cpp
        src/oba/oamd.cpp
        src/oba/scene.cpp
        src/oba/scene_json.cpp
        src/oba/scene_text.cpp
        src/quality/distortion.cpp
        src/quality/perceptual.cpp
        src/sinks/iec61937.cpp
        src/spatial/spatial.cpp
        src/verify/eac3_mirror.cpp
        src/verify/eac3_selfcheck.cpp
        src/verify/mirror.cpp
        src/verify/selfcheck.cpp
        src/version.cpp)

add_dependencies(forge_objects ac3forge_version)

target_include_directories(forge_objects
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
        "$<INSTALL_INTERFACE:include>")

# Selects which of the two identically-pathed ac3/internal/profiling.hpp
# variants (src/internal/profiling/tracy_{enabled,disabled}/) every
# `#include "ac3/internal/profiling.hpp"` call site resolves to - CMake
# choosing the directory, not an #ifdef in the header, per
# tools/checks/check_platform_macros.ps1's platform/feature-isolation rule (the
# same reason src/audio's platform backends and the quarantine hook seam are
# selected this way rather than with a preprocessor conditional). PRIVATE:
# this header is internal build-diagnostics tooling, never installed, so it
# has no business on forge_objects's PUBLIC include path at all.
if(AC3FORGE_ENABLE_TRACY)
    target_include_directories(forge_objects PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/internal/profiling/tracy_enabled")
else()
    target_include_directories(forge_objects PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/internal/profiling/tracy_disabled")
endif()

# The same mechanism again, for the build-profile facts every source asks
# through ac3/internal/profile.hpp (roadmap PF7): full/ says the direct-form
# transform tables are present, minimal/ says they are not, and CMake picks
# the directory rather than any source picking with an #ifdef.
#
# ac3::forge itself is always the full profile - the minimum-footprint
# decoder is a SEPARATE target (forge_minimal, below), not a reconfiguration
# of this one, so turning the option on never changes what an ordinary build
# produces.
target_include_directories(forge_objects PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/internal/profile/full")

# src/core/ on the include path so the CMake-selected
# src/core/transform/<variant>/reference_transform.cpp can include the
# declaration it implements the same way mdct.cpp includes it - quote-relative
# lookup only reaches a file's own directory, and the two variants live one
# level down from the header they share.
target_include_directories(forge_objects PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/core")

# ---------------------------------------------------------------------------
# The SIMD arch tree (ROADMAP PF5).
#
# Exactly the mechanism directly above, on the architecture axis instead of
# the profiling one: src/internal/arch/{generic,x86_64,aarch64}/ each hold an
# identically-pathed ac3/internal/arch/simd.hpp, one of them goes on the
# include path, and every `#include "ac3/internal/arch/simd.hpp"` in the
# codec core resolves to it. No translation unit ever asks what it is being
# compiled for - the rule tools/checks/check_platform_macros.ps1 enforces -
# and adding an architecture means adding a directory, not editing
# conditionals in the kernels.
#
# The kernels themselves are NOT duplicated per directory. They live once,
# in shared code (src/core/fft_radix2.hpp, src/core/mdct.cpp,
# src/core/bitalloc.cpp, src/core/exponents.cpp), written against the two
# 128-bit vector types the header defines; the directories carry the types
# and their operations. See the generic header for why that is what makes
# the bit-exactness claim testable.
#
# AC3FORGE_SIMD forces a directory. `generic` in particular is what a
# reproducibility comparison wants (is a difference between two machines the
# vector kernels or something else?), and is the reference
# tests/core/test_simd_kernels.cpp's scalar side is written to match.
# ---------------------------------------------------------------------------
set(AC3FORGE_SIMD "auto" CACHE STRING
    "SIMD kernel directory: auto (from CMAKE_SYSTEM_PROCESSOR), generic, x86_64, or aarch64")
set_property(CACHE AC3FORGE_SIMD PROPERTY STRINGS auto generic x86_64 aarch64)

if(AC3FORGE_SIMD STREQUAL "auto")
    # AC3FORGE_BUILD_ARCH is resolved further up this file for version
    # strings, from the same CMAKE_SYSTEM_PROCESSOR; reuse it rather than
    # spelling the platform regexes a second time and letting the two drift.
    # Anything that is not one of the two architectures with a directory -
    # 32-bit x86, WebAssembly (Emscripten reports "x86"), a soft-float
    # embedded target - lands on generic, which is a complete implementation
    # and not a stub.
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
    "${CMAKE_CURRENT_SOURCE_DIR}/src/internal/arch/${AC3FORGE_SIMD_RESOLVED}")
if(NOT IS_DIRECTORY "${AC3FORGE_ARCH_INCLUDE_DIR}")
    message(FATAL_ERROR
        "AC3FORGE_SIMD=${AC3FORGE_SIMD} resolved to '${AC3FORGE_SIMD_RESOLVED}', which has no "
        "directory under src/forge/src/internal/arch/. Add "
        "src/forge/src/internal/arch/<name>/ac3/internal/arch/simd.hpp - the generic/ copy is "
        "the portable implementation and is a working starting point.")
endif()
target_include_directories(forge_objects PRIVATE "${AC3FORGE_ARCH_INCLUDE_DIR}")

# tests/CMakeLists.txt reads this so test_simd_kernels.cpp sees the same
# simd.hpp the library was built with - the same white-box arrangement
# src/signing/src and AC3FORGE_AUDIO_BACKEND_DIR already have there.
set(AC3FORGE_ARCH_INCLUDE_DIR "${AC3FORGE_ARCH_INCLUDE_DIR}" PARENT_SCOPE)
# For the configure summary at the end of the top-level CMakeLists.txt.
set(AC3FORGE_SIMD_RESOLVED "${AC3FORGE_SIMD_RESOLVED}" PARENT_SCOPE)

# ---------------------------------------------------------------------------
# Runtime CPU-feature detection (ROADMAP PF5's dynamic-dispatch follow-on).
#
# AC3FORGE_SIMD above is a compile-time choice: it bakes in exactly one of
# SSE2/NEON/scalar, because those are architectural (guaranteed present, no
# feature to ask about). AVX2 is not architectural - it is a real CPU
# feature a build machine cannot know the DEPLOYMENT machine has - so it
# needs an actual runtime answer instead. `AC3FORGE_AVX2` (default ON,
# meaningful only on an x86_64 AC3FORGE_SIMD_RESOLVED) controls whether
# forge_simd_avx2 (below) exists at all; the three-way switch here controls
# how ac3::internal::cpu::has_avx2() answers "is it safe to run AVX2 on
# THIS process's actual CPU" - one of src/internal/cpu/probe/{none,msvc,
# builtin}/ goes on the include path, exactly the same directory-selection
# mechanism as the SIMD arch tree above, so no translation unit ever asks
# what it is being compiled for here either. See
# src/internal/cpu/cpu_features.hpp for the full contract.
#
# msvc/ is shared by real MSVC AND clang-cl: clang-cl reports
# CMAKE_CXX_COMPILER_ID "Clang" with CMAKE_CXX_COMPILER_FRONTEND_VARIANT
# "MSVC" (the same distinction CompilerWarnings.cmake and the top-level
# CMakeLists.txt's -ffp-contract policy already key on) - and although
# clang-cl is Clang underneath, __builtin_cpu_supports needs compiler-rt's
# cpu-model support linked in, which this project's clang-cl configuration
# (MSVC-CRT-linked) does not guarantee. <intrin.h>'s CPUID/XGETBV
# intrinsics have no such dependency on either compiler, so MSVC and
# clang-cl share that path instead of the GCC/Clang/AppleClang builtin one.
option(AC3FORGE_AVX2
    "Compile an AVX2 SIMD tier (x86_64 only) alongside the baseline SSE2 one, selected at runtime"
    ON)

# Shared between the probe-directory choice just below and the AVX2 compile
# flag further down - both need the same real-MSVC-or-clang-cl distinction,
# and computing it once keeps the two decisions from ever being able to
# silently disagree with each other.
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR
   (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND
    CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    set(AC3FORGE_IS_MSVC_LIKE TRUE)
else()
    set(AC3FORGE_IS_MSVC_LIKE FALSE)
endif()

if(NOT (AC3FORGE_SIMD_RESOLVED STREQUAL "x86_64" AND AC3FORGE_AVX2))
    set(AC3FORGE_CPU_PROBE_DIR "none")
elseif(AC3FORGE_IS_MSVC_LIKE)
    set(AC3FORGE_CPU_PROBE_DIR "msvc")
else()
    set(AC3FORGE_CPU_PROBE_DIR "builtin")
endif()

# Two directories, not one: src/internal/cpu itself holds cpu_features.hpp
# (bare-includable, unconditional - the same "just a directory on the path"
# convention src/core/fft_kernel.hpp uses, since there is only ever one
# copy of this file), and the resolved probe/<name>/ subdirectory holds the
# one genuinely multi-variant piece, hardware_avx2.hpp, reached through the
# "ac3/internal/cpu/hardware_avx2.hpp" spelling every probe/ variant shares
# (the same convention the SIMD arch tree above uses, for the same reason:
# the include spelling must not change depending on which directory
# answered it).
target_include_directories(forge_objects PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/internal/cpu"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/internal/cpu/probe/${AC3FORGE_CPU_PROBE_DIR}"
    # mdct_avx2.hpp's plain-signature declarations (below) must resolve for
    # mdct.cpp on every config, same reasoning as the cpu/ directory just
    # above - its own .cpp backing is directory/target-selected further
    # down, but the include path itself is unconditional.
    "${CMAKE_CURRENT_SOURCE_DIR}/src/internal/avx2")

# tests/CMakeLists.txt compiles src/internal/cpu/cpu_features.cpp a SECOND
# time, directly into ac3tests, against these same two directories - the
# same white-box arrangement (and the same DLL-export reasoning) as
# forge_simd_avx2's own PARENT_SCOPE variables further down. Exposed
# unconditionally, not only when the AVX2 tier is active: has_avx2()
# correctly answering "false" on a generic/AVX2-disabled build is exactly as
# worth testing as it answering "true" where AVX2 is actually compiled in.
set(AC3FORGE_CPU_FEATURES_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/src/internal/cpu" PARENT_SCOPE)
set(AC3FORGE_CPU_PROBE_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/src/internal/cpu/probe/${AC3FORGE_CPU_PROBE_DIR}" PARENT_SCOPE)
set(AC3FORGE_CPU_FEATURES_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/internal/cpu/cpu_features.cpp" PARENT_SCOPE)
set(AC3FORGE_IS_MSVC_LIKE "${AC3FORGE_IS_MSVC_LIKE}" PARENT_SCOPE)

# getenv (AC3FORGE_SIMD_TIER's override, cpu_features.cpp) is flagged as
# "unsafe" by MSVC's CRT (C4996, since it has no bounds to check the caller
# did anything wrong with) - a real question for a caller-supplied buffer,
# not for reading one environment variable into a function-local static at
# process start. Scoped to this one file rather than /wd4996 on the whole
# target, the same reasoning as forge_shared's /wd4251 further down: a
# narrow, understood exception, not a blanket policy. Same-directory
# CMakeLists.txt as the target_sources() call above that adds this file, so
# the "silently no-ops from another directory" trap set_source_files_properties()
# usually carries does not apply here.
#
# AC3FORGE_IS_MSVC_LIKE, not a $<CXX_COMPILER_ID:MSVC> generator expression:
# clang-cl reports its own CXX_COMPILER_ID as "Clang" even in MSVC mode (the
# same distinction this file's own AC3FORGE_IS_MSVC_LIKE computation exists
# for), so a bare :MSVC genex here would miss it and clang-cl would hit the
# same C4996-equivalent hard error real MSVC needs this suppression for -
# confirmed by hand: it does.
if(AC3FORGE_IS_MSVC_LIKE)
    set_source_files_properties(src/internal/cpu/cpu_features.cpp PROPERTIES
        COMPILE_OPTIONS "/wd4996")
endif()

# The one target in the whole build that ever sees an AVX2 compiler flag.
# Object library, not folded into forge_objects: an AVX2 flag applied to
# forge_objects would apply to every OTHER file in the codec core too,
# which is exactly the "compiled with -march= for a feature the deployment
# machine might not have" mistake ac3::internal::cpu::has_avx2() exists to
# let this project avoid - see cpu_features.hpp. avx2_probe.cpp proved the
# whole build/link/dispatch/test pipeline (ROADMAP PF5's dynamic-dispatch
# follow-on, phase one) before any real kernel joined it; mdct_avx2.cpp is
# the first real one - apply_analysis_window and the DCT-IV/IMDCT twiddle
# stages, the two PF5 kernels phase one's own measurement (docs/building.md)
# showed a real, reproducible cycle-count win for. dft512's normalisation
# and bitalloc.cpp/exponents.cpp's kernels are deliberately NOT here: that
# same measurement found no signal for them above the ~2-4% cross-build
# noise floor at this granularity.
if(AC3FORGE_SIMD_RESOLVED STREQUAL "x86_64" AND AC3FORGE_AVX2)
    add_library(forge_simd_avx2 OBJECT
        src/internal/avx2/avx2_probe.cpp
        src/internal/avx2/mdct_avx2.cpp)

    target_include_directories(forge_simd_avx2 PRIVATE
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/internal/avx2")
    target_compile_features(forge_simd_avx2 PRIVATE cxx_std_23)
    target_link_libraries(forge_simd_avx2 PRIVATE "$<BUILD_INTERFACE:ac3::warnings>")

    # /arch:AVX2 for MSVC and clang-cl, -mavx2 for GCC/Clang/AppleClang.
    # Deliberately never -mfma alongside it: this project's own code must
    # never call an FMA intrinsic regardless of what the flag would
    # otherwise permit (see the top-level CMakeLists.txt's
    # -ffp-contract=off comment - contraction is exactly what an AVX2
    # kernel's bit-exactness argument cannot survive), and not requesting
    # it keeps has_avx2()'s CPUID gate to the single AVX2 bit rather than
    # needing a second, independent FMA capability check nothing here uses.
    if(AC3FORGE_IS_MSVC_LIKE)
        target_compile_options(forge_simd_avx2 PRIVATE /arch:AVX2)
    else()
        target_compile_options(forge_simd_avx2 PRIVATE -mavx2)
    endif()

    set_target_properties(forge_simd_avx2 PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        # LTO/LTCG is the one mechanism that can hoist AVX2-flagged codegen
        # across a translation-unit boundary into a caller that was never
        # compiled with the flag - silently producing an illegal
        # instruction on hardware has_avx2() would otherwise have refused
        # to run it on. This line is a correctness requirement, not a
        # tidiness one: it must stay off on this target regardless of
        # whatever LTO policy the rest of the build ever adopts.
        INTERPROCEDURAL_OPTIMIZATION OFF)

    # forge_static/forge_shared already pull in forge_objects's object code
    # this same way (top of this file) - an OBJECT library "links" into a
    # STATIC/SHARED target by contributing its object files to the final
    # archive/binary, not by needing anything from forge_objects itself, so
    # this is independent of that link and just as safe to add here.
    target_link_libraries(forge_static PUBLIC forge_simd_avx2)
    target_link_libraries(forge_shared PUBLIC forge_simd_avx2)

    # tests/CMakeLists.txt compiles avx2_probe.cpp a SECOND time, directly
    # into ac3tests (the same white-box arrangement src/signing/src already
    # has there) rather than calling into this object library's copy - that
    # sidesteps needing AC3FORGE_EXPORT/dllimport treatment on internal
    # build plumbing that would otherwise only matter for the
    # config-linux-llvm-shared (BUILD_SHARED_LIBS=ON) leg. AC3FORGE_IS_MSVC_LIKE
    # is already exposed PARENT_SCOPE further up this file (unconditionally,
    # since cpu_features.cpp needs it too regardless of whether this AVX2
    # tier exists at all).
    set(AC3FORGE_AVX2_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/internal/avx2" PARENT_SCOPE)
else()
    # mdct.cpp's call sites still need something to link against - the
    # unreachable-body stub, compiled straight into forge_objects with no
    # AVX2 flag at all (it contains no AVX2 code), exactly the same
    # "always linkable, directory-selected implementation" shape
    # AC3FORGE_CPU_PROBE_DIR's "none" variant already uses for
    # cpuid_reports_avx2() above.
    target_sources(forge_objects PRIVATE src/internal/avx2/none/mdct_avx2.cpp)
endif()

target_compile_features(forge_objects PUBLIC cxx_std_23)

# $<BUILD_INTERFACE:...> wrapping, not a plain PRIVATE link: ac3::warnings/
# ac3::coverage/ac3::tracy are dev-only interface targets that must never
# propagate to an installed consumer regardless - but OBJECT libraries
# specifically require install(EXPORT) to be able to resolve every dependency
# they have, PRIVATE included, since a .o file's compile-time requirements
# aren't fully self-contained the way a finished .lib/.dll's are. Wrapping
# makes the dependency invisible to the export graph outright, which is what
# "dev-only, never for a consumer" actually means here, not "somehow also
# exported." ac3::tracy is a no-op INTERFACE target unless AC3FORGE_ENABLE_TRACY
# is on (see cmake/Tracy.cmake), so linking it unconditionally costs nothing
# by default.
target_link_libraries(forge_objects PRIVATE
    "$<BUILD_INTERFACE:ac3::warnings>"
    "$<BUILD_INTERFACE:ac3::coverage>"
    "$<BUILD_INTERFACE:ac3::tracy>"
    "$<BUILD_INTERFACE:ac3::fmt>")

# --- Static+shared plumbing -------------------------------------------------
# PIC is required for any object code that might end up in a shared library
# (mandatory on Linux/macOS, ignored by MSVC) - the same objects feed
# forge_static too, but a static archive being built PIC costs nothing.
# Hidden visibility by default + explicit AC3FORGE_EXPORT annotation (see
# ac3/export.hpp's callers) is the standard pattern: only the intentionally
# public API surfaces across the .so/.dll boundary, everything else - helper
# functions, implementation detail classes - stays invisible outside it.
set_target_properties(forge_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

# MSVC's C4251 ("class X needs to have dll-interface...") fires for every
# PRIVATE STL-typed member of an AC3FORGE_EXPORT class - std::vector/std::map/
# std::optional fields that are pure implementation detail, never touched by
# a caller except through the class's own (exported) methods. That's exactly
# the case this warning can't tell apart from a genuinely unsafe one, so it's
# suppressed here rather than by scattering non-portable
# #pragma warning(disable:) through headers GCC/Clang also compile (see
# cmake/CompilerWarnings.cmake's own MSVC-vs-portable split for the same
# reasoning). Scoped to this target, not ac3::warnings globally: it's a
# consequence of exporting a library, not a project-wide warning policy call.
#
# PUBLIC, not PRIVATE: the warning fires wherever a translation unit parses
# the dllexport/dllimport-decorated class definition, not only inside
# forge_objects's own compilation - a consumer built against
# ac3::forge_shared (ac3cli/ac3gui/tests/examples, once BUILD_SHARED_LIBS=ON)
# includes these same headers under the dllimport branch and hits the
# identical warning in its own translation units. ac3::forge_static's
# consumers never see it (AC3FORGE_STATIC_DEFINE strips the decoration
# entirely), so this is a no-op for them, not an unwanted suppression.
target_compile_options(forge_objects PUBLIC "$<$<CXX_COMPILER_ID:MSVC>:/wd4251>")

# generate_export_header() normally adds the "building the DLL" compile
# definition to the target it's called on (here, forge_shared) - but
# forge_shared has no sources of its own, everything actually compiles
# via forge_objects, so that auto-added definition would never reach the
# code it's meant to affect. Setting DEFINE_SYMBOL explicitly and adding it
# to forge_objects by hand closes that gap. The object code is therefore
# always compiled as if building the DLL (dllexport, not dllimport) - this
# is harmless for the objects that end up in forge_static too: a plain
# (non-decorated) consumer of the static archive still resolves the same
# underlying symbol names, dllexport just also lets those symbols export
# through a DLL if one were ever built from them, which forge_static
# itself isn't. AC3FORGE_STATIC_DEFINE is what a static consumer needs
# instead, so the generated header emits no import/export decoration at
# all - see the target_compile_definitions on forge_static below.
#
# The generated header keeps the .hpp extension and generated/ac3/ layout
# every other generated header here already uses (ac3/version.hpp), rather
# than generate_export_header's own .h default, so nothing that already
# includes "ac3/export.hpp" (every annotated header) needs to change.
include(GenerateExportHeader)
# DEFINE_SYMBOL is a target property generate_export_header() reads (not one
# of its own function keywords) to decide which #ifdef the generated header
# guards the dllexport branch with - set explicitly since it would otherwise
# default to forge_shared_EXPORTS, and it's forge_objects that must
# actually define it (see above).
set_target_properties(forge_shared PROPERTIES DEFINE_SYMBOL "AC3FORGE_BUILDING_SHARED")
generate_export_header(forge_shared
    BASE_NAME AC3FORGE
    EXPORT_MACRO_NAME AC3FORGE_EXPORT
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/generated/ac3/export.hpp"
    DEFINE_NO_DEPRECATED
    STATIC_DEFINE AC3FORGE_STATIC_DEFINE)
target_compile_definitions(forge_objects PRIVATE AC3FORGE_BUILDING_SHARED)
target_compile_definitions(forge_static PUBLIC AC3FORGE_STATIC_DEFINE)

# SOVERSION is the full version, not just the major component: pre-1.0, no
# ABI-compatibility promise holds across any two releases, so there is no
# meaningful "compatible" version range narrower than an exact match.
set_target_properties(forge_shared PROPERTIES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION}")

# On Windows, forge_shared's import library and forge_static's archive
# would otherwise both be named ac3forge.lib in the same output directory -
# name the static one distinctly so installing both side by side doesn't
# clobber either. forge_shared keeps the plain "ac3forge" name (-> the
# ac3forge.dll a consumer actually expects). These are the installed
# filenames a consumer sees, deliberately unrelated to the CMake target
# names above.
set_target_properties(forge_shared PROPERTIES OUTPUT_NAME "ac3forge")
set_target_properties(forge_static PROPERTIES OUTPUT_NAME "ac3forge_static")
