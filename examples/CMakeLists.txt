# ---------------------------------------------------------------------------
# The examples from docs/library/, as buildable programs.
#
# They exist to be compiled: a snippet in a document rots silently, and one
# that is a build target cannot. Each is also runnable and prints what it
# produced (via fmt::print, not std::print - see cmake/Fmt.cmake), so the
# documented API is exercised and not merely parsed. They link iclforge::warnings
# like every other first-party target, so a documented call that would make
# a caller's build noisy fails here first.
# ---------------------------------------------------------------------------

set(ICLFORGE_EXAMPLES
    encode_ac3
    encode_eac3
    decode_stream
    metadata
    stream_edit
    spatial_objects
    atmos_objects
    station_broadcast
    mux_mkv
    mux_mp4
    mux_fmp4
    mux_ts
    wav_roundtrip
    custom_layout
    multi_source_assignment
    scripted_object_motion
    osc_object_control
    object_signing
    level_metering
    decode_robustness
    atmos_fallback
    qc_report)

# read_adm demonstrates iclforge::adm (src/adm), the standalone BW64/RF64 + ADM
# parser - "standalone" meaning zero dependency on iclforge::ac3, unlike every other
# example in the list above. encode_adm (ADM BWF reader phase 3) is the end-to-end
# counterpart: iclforge::adm::parse_bw64 -> iclforge::admbridge::build -> iclforge::oba::AtmosEncoder, so unlike
# every other entry here (including read_adm) it needs BOTH iclforge::ac3 and the opt-in ADM modules
# together. Both are only added when ICLFORGE_BUILD_ADM turned those modules on (default OFF - see
# root CMakeLists.txt's own option() for why), so the foreach below cannot assume either is always
# present the way it can for e.g. mux_mkv.
if(ICLFORGE_BUILD_ADM)
    list(APPEND ICLFORGE_EXAMPLES read_adm encode_adm)
    # encode_iab (IAB reader phase 3) needs iclforge::admbridge's own IAB mapping (build_iab), gated
    # the same way encode_adm needs the ADM one - see src/admbridge/CMakeLists.txt's own header
    # comment for why that whole module rides ICLFORGE_BUILD_ADM. Unlike encode_adm it does NOT
    # need iclforge::adm directly (iclforge::iab, its own container reader, is always available -
    # ICLFORGE_BUILD_IAB defaults ON).
    list(APPEND ICLFORGE_EXAMPLES encode_iab)
endif()

# mux_iamf demonstrates IAMF writer phase 1's decode -> IAMF bridge: it encodes a synthetic
# 7.1.4 E-AC-3 stream, decodes it back with iclforge::Eac3Decoder, permutes the result into iamf::'s
# own channel order and calls iclforge::iamf::mux() - so, like encode_adm above, it needs BOTH iclforge::ac3
# and the standalone writer module together, and only exists to build when ICLFORGE_BUILD_IAMF
# (default ON, see the root CMakeLists.txt's own option()) turned iclforge::iamf on.
if(ICLFORGE_BUILD_IAMF)
    list(APPEND ICLFORGE_EXAMPLES mux_iamf)
endif()

# read_iab demonstrates iclforge::iab (src/iab), IAB reader phases 1-2 - "standalone" meaning
# zero dependency on iclforge::ac3, the same shape read_adm has for iclforge::adm above. Unlike
# ICLFORGE_BUILD_ADM (default OFF), ICLFORGE_BUILD_IAB defaults ON, so this is unconditional like
# mux_mkv rather than gated in the foreach below.
if(ICLFORGE_BUILD_IAB)
    list(APPEND ICLFORGE_EXAMPLES read_iab)
endif()

foreach(example IN LISTS ICLFORGE_EXAMPLES)
    add_executable(${example} "${example}.cpp")
    if(example STREQUAL "read_adm")
        # No iclforge::ac3 here - src/adm is deliberately independent of it (see
        # src/adm/CMakeLists.txt's header comment), so this example only needs
        # the parser itself plus the same iclforge::warnings every other example gets.
        target_link_libraries(${example} PRIVATE iclforge::adm iclforge::warnings iclforge::fmt)
    elseif(example STREQUAL "encode_adm")
        # The one example (and ADM BWF reader's own phase-3 deliverable) needing iclforge::ac3
        # (AtmosEncoder) and the ADM modules (parse/bridge) at the same time.
        target_link_libraries(${example} PRIVATE iclforge::ac3 iclforge::adm iclforge::admbridge
                                                  iclforge::warnings iclforge::fmt)
    elseif(example STREQUAL "mux_iamf")
        target_link_libraries(${example} PRIVATE iclforge::ac3 iclforge::iamf iclforge::warnings iclforge::fmt)
    elseif(example STREQUAL "read_iab")
        # No iclforge::ac3 here either - src/iab is deliberately independent of it (see
        # src/iab/CMakeLists.txt's header comment), the same reason read_adm above needs none.
        target_link_libraries(${example} PRIVATE iclforge::iab iclforge::warnings iclforge::fmt)
    elseif(example STREQUAL "encode_iab")
        # The one example (and IAB reader's own phase-3 deliverable) needing iclforge::ac3
        # (AtmosEncoder), iclforge::iab (its own container reader) and iclforge::admbridge (build_iab)
        # at the same time - the IAB mirror of encode_adm above.
        target_link_libraries(${example} PRIVATE iclforge::ac3 iclforge::iab iclforge::admbridge
                                                  iclforge::warnings iclforge::fmt)
    else()
        target_link_libraries(${example} PRIVATE iclforge::ac3 iclforge::warnings iclforge::fmt)
    endif()
    set_target_properties(${example} PROPERTIES FOLDER examples)
endforeach()

# Only these need a container writer; mux_fmp4 reuses iclforge::mp4 the same way mux_mp4 does.
target_link_libraries(mux_mkv PRIVATE iclforge::matroska)
target_link_libraries(mux_mp4 PRIVATE iclforge::mp4)
target_link_libraries(mux_fmp4 PRIVATE iclforge::mp4)
target_link_libraries(mux_ts PRIVATE iclforge::mpegts)

# Only this one needs the object signer.
target_link_libraries(object_signing PRIVATE iclforge::signing)

# capi_encode_decode (C API) is a plain C source, not C++ - the .c extension is what actually
# proves iclforge_c/iclforge.h compiles under a C toolchain rather than merely under a C++ one
# parsing valid-C syntax, so it cannot join the foreach()/ICLFORGE_EXAMPLES list above (that loop
# hardcodes add_executable(${example} "${example}.cpp")) and gets its own block instead. It links
# iclforge::c only, never iclforge::ac3 - the whole point of the C API is that a consumer needs
# nothing else.
if(ICLFORGE_BUILD_CAPI)
    # The only C (not C++) source anywhere in this tree - the project() call in the root
    # CMakeLists.txt only declares CXX, so the C compiler has to be brought in explicitly before
    # CMake will generate a compile rule for a .c file at all.
    enable_language(C)
    add_executable(capi_encode_decode capi_encode_decode.c)
    target_link_libraries(capi_encode_decode PRIVATE iclforge::c iclforge::warnings)

    add_executable(capi_encode_eac3 capi_encode_eac3.c)
    target_link_libraries(capi_encode_eac3 PRIVATE iclforge::c iclforge::warnings)

    # AP4: pin the dialect rather than riding whatever the compiler defaults to
    # (gnu17 on GCC/Clang, an unspecified MSVC default) - this is the project's
    # one standing proof that iclforge_c/iclforge.h is valid strict C11 (with
    # -Wpedantic from iclforge::warnings above) on every desktop leg, not merely
    # valid under whichever superset dialect happened to be the default.
    foreach(capi_example IN ITEMS capi_encode_decode capi_encode_eac3)
        set_target_properties(${capi_example} PROPERTIES
            FOLDER examples
            C_STANDARD 11
            C_STANDARD_REQUIRED ON
            C_EXTENSIONS OFF)
        # Both call sin() to make their tone, and a C link, unlike a clang++/g++ one, does not add
        # libm. They never had to name it: the static codec rode along on iclforge::c's link
        # interface, which made CMake link them with the C++ driver. With BUILD_SHARED_LIBS=ON it
        # no longer does (src/capi/CMakeLists.txt), so they link as the C programs they are.
        if(UNIX)
            target_link_libraries(${capi_example} PRIVATE m)
        endif()
    endforeach()

    if(ICLFORGE_BUILD_TESTS)
        add_test(NAME "example.capi_encode_decode" COMMAND capi_encode_decode)
        add_test(NAME "example.capi_encode_eac3" COMMAND capi_encode_eac3)
    endif()
endif()

# decode_ac4 demonstrates the AC-4 inspector and decoder (src/ac4, src/ac4dec), which link nothing
# of iclforge::ac3, as read_iab links nothing of it. It decodes the stream it is given, so it keeps out
# of the list above, whose programs ctest runs with no arguments, and ctest hands it a committed DEE
# stream. ICLFORGE_BUILD_AC4 defaults ON, and with it off there is no decoder to link.
if(ICLFORGE_BUILD_AC4)
    add_executable(decode_ac4 decode_ac4.cpp)
    target_link_libraries(decode_ac4 PRIVATE iclforge::ac4dec iclforge::warnings iclforge::fmt)
    set_target_properties(decode_ac4 PROPERTIES FOLDER examples)
    if(ICLFORGE_BUILD_TESTS)
        add_test(NAME "example.decode_ac4" COMMAND decode_ac4
            "${PROJECT_SOURCE_DIR}/tests/golden/external-baseline/ac4-51-film-96/dee.ac4")
    endif()
endif()

# Run them under ctest when tests are on, so "the examples still work" is
# checked by the same command as everything else.
if(ICLFORGE_BUILD_TESTS)
    foreach(example IN LISTS ICLFORGE_EXAMPLES)
        add_test(NAME "example.${example}" COMMAND ${example})
    endforeach()
endif()
