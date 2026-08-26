# ---------------------------------------------------------------------------
# ac3::forge_minimal - the whole of src/forge under the minimum-footprint
# decoder profile (AC3FORGE_MINIMAL_DECODER, roadmap PF7). Included and
# returned from by CMakeLists.txt in this directory, so the ordinary
# static+shared build below it cannot be perturbed by this profile at all:
# there is exactly one `if` between the two shapes, at the top of that file,
# and everything specific to this one lives here.
#
# The differences from the ordinary build, and why each is here rather than
# expressed as options on the normal targets:
#
#   - STATIC only. A shared library needs a dynamic loader; this profile's
#     targets are bare metal (see cmake/toolchains/arm-none-eabi.toolchain.cmake).
#     Declaring forge_shared at all fails outright for arm-none-eabi.
#
#   - Decode-only sources. See the list below for what each one is for; the
#     encoder, the WAV/container I/O, the analysis and QC layers and the
#     object ENCODER are all absent, and the sources that remain are exactly
#     what a decode reaches. This is checked, not asserted: the archive is
#     linked into apps/baremetal's probe with --gc-sections and any missing
#     symbol is a link error.
#
#   - src/core/transform/stub/ instead of src/core/transform/reference/, and
#     src/internal/profile/minimal/ instead of .../full/ - the two
#     CMake-selected variants that carry the profile's one behavioural
#     difference. See src/core/reference_transform.hpp.
#
#   - cmake/MinimalDecoder.cmake's ac3::minimal_profile compile options, and
#     PUBLIC rather than PRIVATE: -fno-exceptions is not a private
#     implementation detail of an archive, it is a property a consumer has to
#     share or the two disagree about whether a call can throw.
# ---------------------------------------------------------------------------

add_library(forge_minimal STATIC)
add_library(ac3::forge_minimal ALIAS forge_minimal)

target_sources(forge_minimal
    PRIVATE
        # --- bitstream and shared coding tools ---------------------------
        src/core/bitalloc.cpp        # §7.2 bit allocation, both generations
        src/core/eac3_tables.cpp     # Annex E tables and the chanmap layout algebra
        src/core/exponents.cpp       # §7.1 exponent decoding
        src/core/fft.cpp             # the 512-point DFT §3.5.5 enhanced coupling needs
        src/core/mantissas.cpp       # §7.3 mantissa ungrouping and dither
        src/core/mdct.cpp            # §7.9.4 inverse transform (and the unused forward)
        src/core/transform/stub/reference_transform.cpp
        # ROADMAP PF5's runtime-dispatch follow-on. mdct.cpp asks
        # ac3::internal::cpu::has_avx2() before each vectorised kernel, so
        # this profile has to answer - and both answers must LINK, not just
        # compile.
        #
        # The MINIMAL variant of the probe, not the shared one: the shared
        # implementation reports a bad AC3FORGE_SIMD_TIER through fmt, which
        # this profile does not carry and whose formatted-output machinery it
        # cannot afford. See that file's own header for why this is a separate
        # translation unit rather than a branch. none/mdct_avx2.cpp supplies
        # the std::unreachable() bodies for the declarations mdct.cpp calls on
        # the branch a constant-false has_avx2() makes dead.
        src/internal/cpu/minimal/cpu_features.cpp
        src/internal/avx2/none/mdct_avx2.cpp
        # --- decode ------------------------------------------------------
        src/decoder/decoder.cpp             # AC-3, plus split_frames/split_access_units
        src/decoder/diagnostics.cpp         # DecoderConfig::diagnostics' describe() (AP11)
        src/decoder/eac3_decoder.cpp        # Annex E, every tool
        src/decoder/output.cpp              # OutputStage::apply/mix_levels - both decoders'
                                            # own OutputStage member is called unconditionally
                                            # from decode_frame_core/apply_output/conceal
        src/decoder/transient_prenoise.cpp  # §3.7 post-IMDCT correction
        # --- what the decoders call into ---------------------------------
        src/dsp/qmf.cpp            # the polyphase QMF bank JOC's reconstruction runs through
        src/emdf/emdf.cpp          # the TS 102 366 Annex H container the objects ride in
        src/encoder/coupling.cpp   # despite the path: §7.4.3's coordinate DEQUANTIZER, which
                                   # both decoders call on every coupled block
        src/encoder/eac3_tools.cpp # despite the path: spx/ecpl band geometry and the
                                   # §3.5.5 reconstruction the DECODER shares (AP2's
                                   # naming sweep owns moving it)
        src/meta/drc.cpp           # §7.7 dynrng/compr application
        src/meta/mixing.cpp        # §7.8 downmix coefficients - OutputStage::apply's own
        src/oba/joc.cpp            # §6 object reconstruction from the bed
        src/oba/oamd.cpp           # §H.1 object metadata
        src/verify/eac3_mirror.cpp # DecoderConfig::syntax's E-AC-3 trace types - unconditionally
                                   # referenced at the top of Eac3Decoder::decode_substream, whether
                                   # or not a caller ever sets one
        src/verify/mirror.cpp)     # DecoderConfig::trace's own types

target_include_directories(forge_minimal
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
        "$<INSTALL_INTERFACE:include>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/internal/profile/minimal"
        # Tracy is never part of this profile - the disabled variant's macros
        # expand to nothing, which is what a footprint build wants.
        "${CMAKE_CURRENT_SOURCE_DIR}/src/internal/profiling/tracy_disabled"
        # Roadmap PF5's SIMD arch seam (src/forge/CMakeLists.txt has the full
        # explanation): mdct.cpp/bitalloc.cpp/exponents.cpp unconditionally
        # include ac3/internal/arch/simd.hpp now, so this profile needs a
        # resolved directory the same way the ordinary build does - it just
        # never needs anything other than generic/. AC3FORGE_SIMD's own
        # auto-resolution explicitly names "a soft-float embedded target" as
        # a generic/ case, which is exactly what arm-none-eabi is: the
        # Cortex-M3 this profile targets has no vector unit for x86_64/
        # aarch64's intrinsics to reach, so there is no "auto" question to
        # ask here the way there is for the full library's desktop/Pi targets.
        "${CMAKE_CURRENT_SOURCE_DIR}/src/internal/arch/generic"
        # The runtime-AVX2 seam's three directories, resolved exactly as
        # src/forge/CMakeLists.txt resolves them for the full library and for
        # the same reason - the include SPELLING must not depend on which
        # directory answers it. This profile always takes probe/none: an
        # arm-none-eabi Cortex-M3 has no AVX2 to detect, so has_avx2() is a
        # constant false here rather than a question. src/internal/avx2 is on
        # the path for mdct_avx2.hpp's plain-signature declarations, which
        # mdct.cpp includes unconditionally on every configuration.
        "${CMAKE_CURRENT_SOURCE_DIR}/src/internal/cpu"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/internal/cpu/probe/none"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/internal/avx2")

target_compile_features(forge_minimal PUBLIC cxx_std_23)

target_link_libraries(forge_minimal
    PUBLIC ac3::minimal_profile
    PRIVATE "$<BUILD_INTERFACE:ac3::warnings>")

# ac3/export.hpp is generated, and every annotated header includes it. This
# profile is static-only, so the generated header is asked for the no-op
# variant outright (AC3FORGE_STATIC_DEFINE below) rather than the
# dllexport/dllimport pair the ordinary build needs - there is no DLL here to
# export from or import into.
include(GenerateExportHeader)
generate_export_header(forge_minimal
    BASE_NAME AC3FORGE
    EXPORT_MACRO_NAME AC3FORGE_EXPORT
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/generated/ac3/export.hpp"
    DEFINE_NO_DEPRECATED
    STATIC_DEFINE AC3FORGE_STATIC_DEFINE)
target_compile_definitions(forge_minimal PUBLIC AC3FORGE_STATIC_DEFINE)

set_target_properties(forge_minimal PROPERTIES OUTPUT_NAME "ac3forge_minimal")
