# ---------------------------------------------------------------------------
# iclforge::ac3 - the AC-3 and E-AC-3 codec: bitstream syntax, bit allocation, the encoder and decoder
# with their output stage, the Annex E tools, Atmos in E-AC-3 (JOC and the object encoder), the EMDF
# container, the encoder/decoder mirror check and the psychoacoustic model. No GUI, no third-party
# codec code (CONTRIBUTING.md's "The clean-room rule"), and no platform-specific code at all: this
# target builds identically on any C++23 platform.
#
# What the codec uses and does not own is in the libraries it links: iclforge::base (bit I/O, the
# speaker vocabulary, the kernels' build variants), iclforge::dsp, iclforge::objects (the object
# model and the OAMD payload), iclforge::render (PcmBlock and the panner) and iclforge::iec61937.
# None of them links this one. AC-4 is a peer, in src/ac4*, and links none of this.
#
# Built and installed as a static library, a shared library and the OBJECT library both wrap;
# iclforge_add_library() (cmake/IclforgeLibrary.cmake) makes the three, their aliases, the export
# header and the file names. In-tree consumers link iclforge::ac3, which follows BUILD_SHARED_LIBS; an
# installed package has no ambient default and exports iclforge::ac3_static and iclforge::ac3_shared.
# ---------------------------------------------------------------------------

# Class templates the library exports through explicit instantiations (iclforge/ac3/encoder/transient.hpp).
# Each compiler wants the attribute in a different place, so three macros, one per place. MSVC exports
# the instantiation definition and imports through the declaration (C4910 forbids dllexport on
# `extern template`). GCC and Clang take the visibility attribute on the class template itself, and
# reject it on an instantiation that follows an extern declaration. Generated rather than written into
# the header because a conditional directive in a source file is what
# tools/checks/check_platform_macros.ps1 forbids; this one is the export header's own kind, not a
# platform branch.
set(_ac3_export_custom_content [[
/* ICLFORGE_AC3_TEMPLATE_CLASS       on the class template's definition
   ICLFORGE_AC3_TEMPLATE_IMPORT      on `extern template class` declarations
   ICLFORGE_AC3_TEMPLATE_INSTANTIATE on `template class` definitions */
#ifdef ICLFORGE_AC3_STATIC_DEFINE
#  define ICLFORGE_AC3_TEMPLATE_CLASS
#  define ICLFORGE_AC3_TEMPLATE_IMPORT
#  define ICLFORGE_AC3_TEMPLATE_INSTANTIATE
#elif defined(_MSC_VER)
#  define ICLFORGE_AC3_TEMPLATE_CLASS
#  ifdef ICLFORGE_AC3_BUILDING_SHARED
#    define ICLFORGE_AC3_TEMPLATE_IMPORT
#  else
#    define ICLFORGE_AC3_TEMPLATE_IMPORT ICLFORGE_AC3_EXPORT
#  endif
#  define ICLFORGE_AC3_TEMPLATE_INSTANTIATE ICLFORGE_AC3_EXPORT
#else
#  define ICLFORGE_AC3_TEMPLATE_CLASS ICLFORGE_AC3_EXPORT
#  define ICLFORGE_AC3_TEMPLATE_IMPORT
#  define ICLFORGE_AC3_TEMPLATE_INSTANTIATE
#endif
]])

# The minimum-footprint profile (decode-only or encode-only, static, no exceptions, no fmt) builds a
# different shape from a hand-picked list of this library's and its neighbours' sources, so it has its
# own file. Off by default; the root CMakeLists.txt refuses the combination of these options with
# anything that would need the full library.
if(ICLFORGE_MINIMAL_DECODER OR ICLFORGE_MINIMAL_ENCODER)
    include("${CMAKE_CURRENT_SOURCE_DIR}/minimal.cmake")
    return()
endif()

# --- Version + git stamp ---------------------------------------------------
find_package(Git QUIET)
set(ICLFORGE_VERSION_TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/include/iclforge/ac3/version.hpp.in")
set(ICLFORGE_VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/iclforge/ac3/version.hpp")

# OS/architecture/compiler this binary is built for, for version_details()'s --version/About-dialog
# output. Computed here (CMake already knows all three from the toolchain it configured) rather than
# read back with predefined macros at compile time, so version.cpp needs no #ifdef.
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(ICLFORGE_BUILD_OS "macOS")
else()
    set(ICLFORGE_BUILD_OS "${CMAKE_SYSTEM_NAME}")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(ICLFORGE_BUILD_COMPILER "GCC ${CMAKE_CXX_COMPILER_VERSION}")
else()
    # AppleClang, Clang and MSVC already spell their own CMAKE_CXX_COMPILER_ID the way version_details() wants it.
    set(ICLFORGE_BUILD_COMPILER "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
endif()

set(ICLFORGE_BUILD_TARGET "${ICLFORGE_BUILD_OS} ${ICLFORGE_BUILD_ARCH} (${ICLFORGE_BUILD_COMPILER})")

# Generate once at configure time so the header exists for the first build.
set(ICLFORGE_VERSION "${PROJECT_VERSION}")
set(ICLFORGE_VERSION_FULL "${PROJECT_VERSION_FULL}")
set(SRC "${ICLFORGE_VERSION_TEMPLATE}")
set(DST "${ICLFORGE_VERSION_HEADER}")
set(WORKDIR "${PROJECT_SOURCE_DIR}")
include("${PROJECT_SOURCE_DIR}/cmake/GenerateVersion.cmake")

# Re-stamp on every build so the git information tracks new commits without a reconfigure.
# configure_file only rewrites when the content changes, so this does not recompile dependants.
add_custom_target(iclforge_version ALL
    BYPRODUCTS "${ICLFORGE_VERSION_HEADER}"
    COMMAND "${CMAKE_COMMAND}"
        "-DICLFORGE_VERSION=${PROJECT_VERSION}"
        "-DICLFORGE_VERSION_FULL=${PROJECT_VERSION_FULL}"
        "-DICLFORGE_BUILD_TARGET=${ICLFORGE_BUILD_TARGET}"
        "-DSRC=${ICLFORGE_VERSION_TEMPLATE}"
        "-DDST=${ICLFORGE_VERSION_HEADER}"
        "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
        "-DWORKDIR=${PROJECT_SOURCE_DIR}"
        -P "${PROJECT_SOURCE_DIR}/cmake/GenerateVersion.cmake"
    COMMENT "Stamping version and git information"
    VERBATIM)

# --- The build variants this library owns -------------------------------------------------
# The scalar the DECODER carries its coefficients in, the build profile and the encoder's front-end
# scalar are each chosen by directory, the same mechanism iclforge::base uses for the SIMD tier: CMake
# picks the directory that holds iclforge/ac3/detail/<x>.hpp and no source picks with an #ifdef.
#
# double is the default and stays it for every ordinary build: the gold references, the quality trend
# and the cross-platform bitstream hashes are all stated in terms of that arithmetic.
# iclforge_ac3_minimal picks float32 unconditionally - see minimal.cmake.
set(ICLFORGE_DECODE_SCALAR "double" CACHE STRING
    "Scalar the decoder carries coefficients in: double (default), float, or fixed (Q7.24 in an int32, for parts with no FPU)")
set_property(CACHE ICLFORGE_DECODE_SCALAR PROPERTY STRINGS double float fixed)
if(ICLFORGE_DECODE_SCALAR STREQUAL "float")
    set(_ac3_scalar_dir "float32")
elseif(ICLFORGE_DECODE_SCALAR STREQUAL "double")
    set(_ac3_scalar_dir "float64")
elseif(ICLFORGE_DECODE_SCALAR STREQUAL "fixed")
    # iclforge::arithmetic's Fixed32 - the third tier (planning/arithmetic-tiers.md).
    set(_ac3_scalar_dir "fixed32")
else()
    message(FATAL_ERROR
        "ICLFORGE_DECODE_SCALAR is 'double', 'float' or 'fixed' (got '${ICLFORGE_DECODE_SCALAR}')")
endif()

set(ICLFORGE_ENCODE_SCALAR "double" CACHE STRING
    "Scalar the encoders' analysis front end runs in: double (default) or float")
set_property(CACHE ICLFORGE_ENCODE_SCALAR PROPERTY STRINGS double float)
if(ICLFORGE_ENCODE_SCALAR STREQUAL "float")
    set(_ac3_encode_scalar_dir "float32")
elseif(ICLFORGE_ENCODE_SCALAR STREQUAL "double")
    set(_ac3_encode_scalar_dir "float64")
else()
    message(FATAL_ERROR
        "ICLFORGE_ENCODE_SCALAR is 'double' or 'float' (got '${ICLFORGE_ENCODE_SCALAR}')")
endif()

iclforge_add_library(ac3
    SOURCES
        src/analysis/levels.cpp
        src/core/bitalloc.cpp
        src/core/coupling.cpp
        src/core/eac3_tables.cpp
        src/core/eac3_tools.cpp
        src/core/exponents.cpp
        src/core/mantissas.cpp
        src/core/mdct.cpp
        src/core/transform/reference/reference_transform.cpp
        src/decoder/decoder.cpp
        src/decoder/diagnostics.cpp
        src/decoder/eac3_decoder.cpp
        src/decoder/output.cpp
        src/decoder/transient_prenoise.cpp
        src/emdf/frame_layout.cpp
        src/encoder/assignment.cpp
        src/encoder/bandwidth.cpp
        src/encoder/eac3_frame.cpp
        src/encoder/encoder.cpp
        src/encoder/plan.cpp
        src/encoder/silent_frame.cpp
        src/encoder/transient.cpp
        src/io/dec3.cpp
        src/io/elementary.cpp
        src/io/stream_accumulator.cpp
        src/io/metadata_edit.cpp
        src/io/object_strip.cpp
        src/io/probe.cpp
        src/io/wav.cpp
        src/io/wav_format.cpp
        src/io/wav_stream_reader.cpp
        src/io/wav_stream_writer.cpp
        src/meta/bsi.cpp
        src/meta/bsi_format.cpp
        src/meta/drc.cpp
        src/meta/loudness.cpp
        src/meta/mixing.cpp
        src/meta/qc.cpp
        src/oba/atmos.cpp
        src/oba/joc.cpp
        src/quality/distortion.cpp
        src/quality/perceptual.cpp
        src/verify/bap_census.cpp
        src/verify/eac3_mirror.cpp
        src/verify/eac3_selfcheck.cpp
        src/verify/mirror.cpp
        src/verify/selfcheck.cpp
        src/verify/trace_export.cpp
        src/version.cpp
    DEPENDS base dsp objects render iec61937
    EXPORT_CUSTOM_CONTENT_VAR _ac3_export_custom_content
    PRIVATE_INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/variants/decode-scalar-${_ac3_scalar_dir}"
        # src/core on the path so the CMake-selected src/core/transform/<variant>/reference_transform.cpp
        # can include the declaration it implements the same way mdct.cpp does.
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${CMAKE_CURRENT_SOURCE_DIR}/variants/profile-full"
        "${CMAKE_CURRENT_SOURCE_DIR}/variants/encode-scalar-${_ac3_encode_scalar_dir}"
        # mdct_avx2.hpp's plain-signature declarations must resolve for mdct.cpp on every configuration.
        "${CMAKE_CURRENT_SOURCE_DIR}/src/internal/avx2"
    LINK_PRIVATE
        "$<BUILD_INTERFACE:iclforge::tracy>"
        "$<BUILD_INTERFACE:iclforge::fmt_private>"
        "$<BUILD_INTERFACE:iclforge::arithmetic>")
add_dependencies(iclforge_ac3_objects iclforge_version)
target_include_directories(iclforge_ac3_objects PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>")

# tests/CMakeLists.txt compiles src/internal/avx2/avx2_probe.cpp a SECOND time, directly into iclforge-tests
# (the same white-box arrangement src/signing/src has there).
# The one target in the whole build that ever sees an AVX2 compiler flag: an AVX2 flag applied to the
# codec's other files would compile them for a feature the deployment machine might not have, which
# iclforge/base/detail/cpu_features.hpp's has_avx2() exists to let this project avoid.
if(ICLFORGE_SIMD_RESOLVED STREQUAL "x86_64" AND ICLFORGE_AVX2)
    add_library(iclforge_ac3_simd_avx2 OBJECT
        src/internal/avx2/avx2_probe.cpp
        src/internal/avx2/mdct_avx2.cpp)

    target_include_directories(iclforge_ac3_simd_avx2 PRIVATE
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/internal/avx2")
    target_compile_features(iclforge_ac3_simd_avx2 PRIVATE cxx_std_23)
    # Compile-only, as iclforge_add_library() does for the objects it makes: this object library is
    # PUBLIC-linked into the static wrapper too, and an ordinary link would carry the shared variants
    # of these libraries (what the bare aliases are under BUILD_SHARED_LIBS) into it.
    target_link_libraries(iclforge_ac3_simd_avx2 PRIVATE
        "$<COMPILE_ONLY:iclforge::base>" "$<COMPILE_ONLY:iclforge::render>"
        "$<COMPILE_ONLY:iclforge::objects>" "$<COMPILE_ONLY:iclforge::dsp>"
        "$<BUILD_INTERFACE:iclforge::warnings>")

    # /arch:AVX2 for MSVC and clang-cl, -mavx2 for GCC/Clang/AppleClang. Deliberately never -mfma
    # alongside it: the project's own code must never call an FMA intrinsic, and not requesting it keeps
    # has_avx2()'s CPUID gate to the single AVX2 bit.
    if(ICLFORGE_IS_MSVC_LIKE)
        target_compile_options(iclforge_ac3_simd_avx2 PRIVATE /arch:AVX2)
    else()
        target_compile_options(iclforge_ac3_simd_avx2 PRIVATE -mavx2)
    endif()

    set_target_properties(iclforge_ac3_simd_avx2 PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        # LTO/LTCG is the one mechanism that can hoist AVX2-flagged codegen across a translation-unit
        # boundary into a caller never compiled with the flag: a correctness requirement, not tidiness.
        INTERPROCEDURAL_OPTIMIZATION OFF
        EXPORT_NAME ac3_simd_avx2)

    target_link_libraries(iclforge_ac3_static PUBLIC iclforge_ac3_simd_avx2)
    target_link_libraries(iclforge_ac3_shared PUBLIC iclforge_ac3_simd_avx2)
    set(ICLFORGE_AVX2_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/internal/avx2" PARENT_SCOPE)
else()
    # mdct.cpp's call sites still need something to link against: the unreachable-body stub, compiled
    # straight into the objects with no AVX2 flag.
    target_sources(iclforge_ac3_objects PRIVATE src/internal/avx2/none/mdct_avx2.cpp)
endif()
