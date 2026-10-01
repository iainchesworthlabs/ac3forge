# ---------------------------------------------------------------------------
# iclforge::ac3_minimal - the whole of src/ac3 under the minimum-footprint
# profile (ICLFORGE_MINIMAL_DECODER, or ICLFORGE_MINIMAL_ENCODER for the
# encode-only archive). Included and returned from by CMakeLists.txt in this
# directory, so the ordinary static+shared build below it cannot be perturbed
# by this profile at all:
# there is exactly one `if` between the two shapes, at the top of that file,
# and everything specific to this one lives here.
#
# The differences from the ordinary build, and why each is here rather than
# expressed as options on the normal targets:
#
#   - STATIC only. A shared library needs a dynamic loader; this profile's
#     targets are bare metal (see cmake/toolchains/arm-none-eabi.toolchain.cmake).
#     Declaring iclforge_ac3_shared at all fails outright for arm-none-eabi.
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
#   - cmake/MinimalDecoder.cmake's iclforge::minimal_profile compile options, and
#     PUBLIC rather than PRIVATE: -fno-exceptions is not a private
#     implementation detail of an archive, it is a property a consumer has to
#     share or the two disagree about whether a call can throw.
# ---------------------------------------------------------------------------

add_library(iclforge_ac3_minimal STATIC)
add_library(iclforge::ac3_minimal ALIAS iclforge_ac3_minimal)

# Which "iclforge/base/detail/profiling.hpp" the profile's sources see. Off, the
# markers expand to nothing (tracy_disabled/); with ICLFORGE_STAGE_TIMERS they
# become calls into whatever application links this archive
# (stage_timers/, and apps/baremetal/stage_timers.cpp for the probe). A
# directory choice rather than a define, per the platform-tree rule
# (tools/checks/check_platform_macros.ps1), and the root CMakeLists.txt has
# already refused the option outside this profile.
if(ICLFORGE_STAGE_TIMERS)
    set(_ac3_minimal_profiling_dir
        "${PROJECT_SOURCE_DIR}/src/base/variants/profiling-stage_timers")
    message(STATUS "Minimum-footprint profile: zone markers routed to the stage-timer backend")
else()
    set(_ac3_minimal_profiling_dir
        "${PROJECT_SOURCE_DIR}/src/base/variants/profiling-tracy_disabled")
endif()

target_sources(iclforge_ac3_minimal
    PRIVATE
        # --- bitstream and shared coding tools ---------------------------
        src/core/bitalloc.cpp        # §7.2 bit allocation, both generations
        src/core/coupling.cpp        # §7.4.3's coordinate dequantizer, which both decoders
                                     # call on every coupled block
        src/core/eac3_tables.cpp     # Annex E tables and the chanmap layout algebra
        src/core/eac3_tools.cpp      # spx/ecpl band geometry and the §3.5.5 enhanced-coupling
                                     # reconstruction the decoder shares
        src/core/exponents.cpp       # §7.1 exponent decoding
        "${PROJECT_SOURCE_DIR}/src/dsp/src/fft.cpp"  # the 512-point DFT §3.5.5 enhanced coupling
                                     # needs
        src/core/mantissas.cpp       # §7.3 mantissa ungrouping and dither
        src/core/mdct.cpp            # §7.9.4 inverse transform (and the unused forward)
        src/core/transform/stub/reference_transform.cpp
        # --- the syntax/trace types BOTH directions reference --------------
        # Not decode-only, though they were listed that way while this profile
        # only had a decoder. FrameEncoder::encode_frame calls
        # verify::FrameTrace::reset() and eac3::emit_frame calls
        # Eac3SubstreamTrace::reset() unconditionally, exactly as the decoders
        # do, whether or not a caller ever sets a trace - so leaving them in the
        # decode half made the encode profile fail to link.
        src/verify/mirror.cpp
        src/verify/eac3_mirror.cpp
        # Runtime AVX2 dispatch. mdct.cpp asks
        # iclforge::internal::cpu::has_avx2() before each vectorised kernel, so
        # this profile has to answer - and both answers must LINK, not just
        # compile.
        #
        # The MINIMAL variant of the probe, not the shared one: the shared
        # implementation reports a bad ICLFORGE_SIMD_TIER through fmt, which
        # this profile does not carry and whose formatted-output machinery it
        # cannot afford. See that file's own header for why this is a separate
        # translation unit rather than a branch. none/mdct_avx2.cpp supplies
        # the std::unreachable() bodies for the declarations mdct.cpp calls on
        # the branch a constant-false has_avx2() makes dead.
        "${PROJECT_SOURCE_DIR}/src/base/src/minimal/cpu_features.cpp"
        src/internal/avx2/none/mdct_avx2.cpp)

# --- and then one direction or the other ------------------------------------
#
# The list above is what BOTH need: the bitstream layer, the shared coding
# tools, the transform. What follows is the half that differs, and the two are
# never both added - ICLFORGE_MINIMAL_DECODER and ICLFORGE_MINIMAL_ENCODER are
# mutually exclusive at the root, because measured on an ESP32-S3 no two of
# these shapes fit in internal SRAM at once (233,546 / 201,770 / 243,770 peak
# against 277,400 free).
if(ICLFORGE_MINIMAL_DECODER)
target_sources(iclforge_ac3_minimal
    PRIVATE
        # --- decode ------------------------------------------------------
        src/decoder/decoder.cpp             # AC-3, plus split_frames/split_access_units
        src/decoder/diagnostics.cpp         # DecoderConfig::diagnostics' describe() (AP11)
        src/decoder/eac3_decoder.cpp        # Annex E, every tool
        src/decoder/output.cpp              # OutputStage::apply/mix_levels - both decoders'
                                            # own OutputStage member is called unconditionally
                                            # from decode_frame_core/apply_output/conceal
        src/decoder/transient_prenoise.cpp  # §3.7 post-IMDCT correction
        # --- what the decoders call into ---------------------------------
        "${PROJECT_SOURCE_DIR}/src/dsp/src/qmf.cpp"  # the polyphase QMF bank JOC runs through
        "${PROJECT_SOURCE_DIR}/src/objects/src/emdf.cpp"  # the TS 102 366 Annex H container
                                   # the objects ride in
        # --- getting a stream IN ------------------------------------------
        # The profile had no input path at all until these: the probe decodes a
        # fixture linked into its own image, which is fine for a measurement and
        # useless for a player. An embedded decoder reads from somewhere - a
        # flash partition, an SD card, a socket - and none of those can be
        # spanned before the last byte arrives, which is what split_frames and
        # split_access_units both require.
        #
        # elementary.cpp is here for read_frame_header alone. The rest of it -
        # scan(), the timing helpers, describe() - is unreachable from this
        # profile and --gc-sections drops it; the file carries no fmt, no
        # exceptions and no std::string, which is why it can be here at all.
        src/io/elementary.cpp
        src/io/stream_accumulator.cpp
        src/meta/drc.cpp           # §7.7 dynrng/compr application
        src/meta/mixing.cpp        # §7.8 downmix coefficients - OutputStage::apply's own
        src/oba/joc.cpp            # §6 object reconstruction from the bed
        "${PROJECT_SOURCE_DIR}/src/objects/src/oamd.cpp"  # §H.1 object metadata
        # --- and placing them -----------------------------------------------
        # Reconstructed objects are mono signals with a position each; a part
        # driving loudspeakers has to pan them onto its layout, and this is
        # the panner forge's `qc objects=` and the encoder's own bed render
        # use: pan_targets/pan_direction/position_direction, height-aware,
        # for any Table E2.5 layout. Pure arithmetic over <vector> and <cmath>
        # - no fmt, no exceptions - which is why it can be here. The probe's
        # eac3_atmos_render row exercises it onto 7.1.4.
        "${PROJECT_SOURCE_DIR}/src/render/src/spatial.cpp"
)
else()
target_sources(iclforge_ac3_minimal
    PRIVATE
        # --- encode ------------------------------------------------------
        src/encoder/encoder.cpp     # AC-3
        src/encoder/eac3_frame.cpp  # Annex E. The bigger half by a long way -
                                    # 5,715 lines against encoder.cpp's 2,516,
                                    # and 243,770 bytes of peak against 201,770
        src/encoder/plan.cpp        # layout/tool selection and the CLI vocabulary
        src/encoder/assignment.cpp  # channel assignment
        src/encoder/bandwidth.cpp   # §7.2.2's coded bandwidth
        src/encoder/transient.cpp   # block-switch detection
        src/encoder/silent_frame.cpp
        # --- what the encoders call into ---------------------------------
        src/meta/bsi.cpp            # valid_bsi_info/valid_alternate_bsi, which
                                    # encode_frame checks its config against.
                                    # format_timecode is deliberately NOT here:
                                    # it is the file's only fmt user and lives in
                                    # bsi_format.cpp so this one can be built
                                    # without fmt at all
        src/meta/drc.cpp            # §7.7 dynrng/compr the encoder writes
        src/meta/mixing.cpp         # §7.8 mix metadata
        src/quality/perceptual.cpp  # the masking model bit allocation consults
        src/quality/distortion.cpp) # EQ13's decision search
endif()
     # DecoderConfig::trace's own types

# The decode scalar this profile carries. float32 whatever the ordinary
# build's option says, with one exception: this profile exists for targets
# whose FPU is single-precision at best, and the ESP32-S3 port did not fit in
# internal SRAM until the decode path moved to float, so double is not a shape
# a caller can configure it into. The fixed-point tier is the profile's other
# legitimate shape - a part with no FPU at all (planning/arithmetic-tiers.md) -
# and is the one value of ICLFORGE_DECODE_SCALAR honoured here.
if(ICLFORGE_DECODE_SCALAR STREQUAL "fixed")
    set(_ac3_minimal_scalar_dir "fixed32")
else()
    set(_ac3_minimal_scalar_dir "float32")
endif()

target_include_directories(iclforge_ac3_minimal
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        # The profile is one archive of files from six libraries (base, dsp, objects and render
        # are not built on their own here: their CMakeLists.txt return early for it), so it
        # carries all six public include trees and the six export headers, generated below.
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/base/include>"
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/dsp/include>"
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/objects/include>"
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/render/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
        "$<INSTALL_INTERFACE:include>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
        "${CMAKE_CURRENT_SOURCE_DIR}/variants/profile-minimal"
        # float32 or fixed32 - see _ac3_minimal_scalar_dir above.
        "${CMAKE_CURRENT_SOURCE_DIR}/variants/decode-scalar-${_ac3_minimal_scalar_dir}"
        # And the encoders' front end in float32 too (encode_scalar_t), for
        # the same part's sake: in double, transient detection and the
        # forward transform were 64% of an AC-3 5.1 frame on an ESP32-S3.
        "${CMAKE_CURRENT_SOURCE_DIR}/variants/encode-scalar-float32"
        # Tracy is never part of this profile - the disabled variant's macros
        # expand to nothing, which is what a footprint build wants. What CAN
        # answer the same markers here is the stage-timer backend, resolved
        # above this target_sources() block: the application supplies the
        # clock and the table, the library only calls in.
        "${_ac3_minimal_profiling_dir}"
        # The SIMD arch seam is resolved by src/arithmetic/CMakeLists.txt and comes
        # with iclforge::arithmetic, linked below: mdct.cpp/bitalloc.cpp/exponents.cpp
        # include iclforge/arithmetic/detail/simd.hpp unconditionally, so this profile needs
        # a directory the same way the ordinary build does.
        #
        # Both of the profile's bare-metal targets resolve to generic/ - an
        # arm-none-eabi Cortex-M3 has no vector unit at all, and the ESP32-S3's PIE
        # is fixed-point, so its float32 path is the scalar FPU either way
        # (docs/platforms/bare-metal/esp32-s3.md) - through ICLFORGE_SIMD's own
        # "anything else lands on generic" arm rather than by being spelled out
        # here; a minimum-footprint build for aarch64 takes the NEON directory, whose
        # float32 decode path holds four lanes.
        # The runtime-AVX2 seam's three directories, resolved exactly as
        # src/ac3/CMakeLists.txt resolves them for the full library and for
        # the same reason - the include SPELLING must not depend on which
        # directory answers it. This profile always takes probe/none: an
        # arm-none-eabi Cortex-M3 has no AVX2 to detect, so has_avx2() is a
        # constant false here rather than a question. src/internal/avx2 is on
        # the path for mdct_avx2.hpp's plain-signature declarations, which
        # mdct.cpp includes unconditionally on every configuration.
        "${PROJECT_SOURCE_DIR}/src/base/variants/cpu-probe-none"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/internal/avx2")

target_compile_features(iclforge_ac3_minimal PUBLIC cxx_std_23)

# The decode-critical translation units at -O2 under an otherwise
# size-optimised build. Off by default: this profile's subject is size, and
# the arm-none-eabi leg's image figure is only comparable across builds that
# optimise for the same thing. A target that has to keep up with real time
# and has flash to spare turns it on - apps/baremetal/platform/esp32s3 does,
# and docs/platforms/bare-metal/esp32-s3.md's Timing section has the measurement behind
# the list below.
#
# Each file is here because a board run showed the optimiser paying for it,
# stage by stage, and the ones it did not pay for are deliberately absent:
# mdct.cpp and fft.cpp (the float32 IMDCT ran in 3.40 ms either way),
# exponents.cpp and mantissas.cpp (under a tenth of a millisecond between
# them). What it bought at 240 MHz, per frame of the 5.1 Atmos fixture:
# bit allocation 4.55 -> 2.09 ms, the JOC mixing 11.0 -> 8.2 ms, the
# E-AC-3 stages in eac3_decoder.cpp about 1.5 ms between them; and, per frame
# of the 7.1.4 stream folded to stereo, the output stage's seating and fold
# 1.53 -> 0.83 ms for 2,688 bytes of flash. The cost is flash, not SRAM -
# the code lives in flash on every part this profile targets - and it is
# stated on the board page beside the gain.
option(ICLFORGE_MINIMAL_HOT_O2
    "Minimum-footprint profile: compile the decode-critical sources at -O2 (costs flash, not SRAM)"
    OFF)
if(ICLFORGE_MINIMAL_HOT_O2)
    set_source_files_properties(
        src/core/bitalloc.cpp
        src/core/eac3_tools.cpp
        "${PROJECT_SOURCE_DIR}/src/dsp/src/fft.cpp"
        src/decoder/decoder.cpp
        src/decoder/eac3_decoder.cpp
        src/decoder/output.cpp
        src/oba/joc.cpp
        PROPERTIES COMPILE_OPTIONS "-O2")
    message(STATUS "Minimum-footprint profile: decode-critical sources at -O2")
endif()

target_link_libraries(iclforge_ac3_minimal
    PUBLIC iclforge::minimal_profile
    PRIVATE "$<BUILD_INTERFACE:iclforge::warnings>" "$<BUILD_INTERFACE:iclforge::arithmetic>")

# iclforge/<library>/export.hpp is generated for each of the six libraries the archive holds, and
# every annotated header includes its own. This profile is static-only, so each is asked for the
# no-op variant outright (ICLFORGE_<LIBRARY>_STATIC_DEFINE below) rather than the dllexport/dllimport
# pair the ordinary build needs - there is no DLL here to export from or import into. Only the
# codec's own header carries the class-template macros (_ac3_export_custom_content).
include(GenerateExportHeader)
generate_export_header(iclforge_ac3_minimal
    BASE_NAME ICLFORGE_AC3
    CUSTOM_CONTENT_FROM_VARIABLE _ac3_export_custom_content
    EXPORT_MACRO_NAME ICLFORGE_AC3_EXPORT
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/generated/iclforge/ac3/export.hpp"
    DEFINE_NO_DEPRECATED
    STATIC_DEFINE ICLFORGE_AC3_STATIC_DEFINE)
foreach(_lib IN ITEMS base dsp objects render)
    string(TOUPPER "${_lib}" _upper)
    generate_export_header(iclforge_ac3_minimal
        BASE_NAME ICLFORGE_${_upper}
        EXPORT_MACRO_NAME ICLFORGE_${_upper}_EXPORT
        EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/generated/iclforge/${_lib}/export.hpp"
        DEFINE_NO_DEPRECATED
        STATIC_DEFINE ICLFORGE_${_upper}_STATIC_DEFINE)
    target_compile_definitions(iclforge_ac3_minimal PUBLIC ICLFORGE_${_upper}_STATIC_DEFINE)
endforeach()
target_compile_definitions(iclforge_ac3_minimal PUBLIC ICLFORGE_AC3_STATIC_DEFINE)

set_target_properties(iclforge_ac3_minimal PROPERTIES OUTPUT_NAME "iclforge_ac3_minimal")
