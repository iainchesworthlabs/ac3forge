# ---------------------------------------------------------------------------
# ac4::decoder - an AC-4 decoder, ETSI TS 103 190-1 V1.4.1 and TS 103 190-2
# V1.3.1, written from the published texts.
#
# A peer of src/ac4 (ac4::ac4, the bitstream inspector) and, like it,
# independent of ac3::forge: AC-4 shares no bitstream syntax with AC-3 or
# E-AC-3, and a decoder that linked ac3::forge would bring the whole codec
# into every AC-4 build. It links ac4::ac4 for the table of contents and the
# presentation and substream group information, which the inspector already
# reads, and adds everything below the substream index table.
#
# Same OBJECT + static + shared arrangement as src/ac4 - see
# src/forge/CMakeLists.txt's ac3::forge for why an OBJECT library plus two
# thin wrapper targets rather than one add_library(). Installed and exported
# with the inspector and the core as ac4::decoder_static and
# ac4::decoder_shared (cmake/InstallLibrary.cmake), the names these aliases
# give the targets in the tree; EXPORT_NAME below makes the installed names
# the same.
# ---------------------------------------------------------------------------
add_library(ac4dec_objects OBJECT)
add_library(ac4dec_static STATIC)
add_library(ac4dec_shared SHARED)

# Each library links the inspector of its own kind, PUBLIC, since the
# decoder's header names the inspector's Toc and presentation types, and the
# core PRIVATE. The static library passes the core on to whatever links it,
# as a link-only dependency, because nothing in an archive links it; the
# shared library carries the part of it that it uses and passes on nothing.
# The OBJECT library below takes both for compiling only: it has no link step
# to keep a PRIVATE dependency to itself, and both libraries link it PUBLIC for
# its include path, so a dependency linked there reached every consumer of
# either. Linked there, the core's archive was on the link line of everything
# that linked libac4dec.so, and in a static build the inspector's archive was
# too (src/capi/CMakeLists.txt found the same with ac3::forge_c).
target_link_libraries(ac4dec_static PUBLIC ac4dec_objects ac4::ac4_static PRIVATE ac4::core)
target_link_libraries(ac4dec_shared PUBLIC ac4dec_objects ac4::ac4_shared PRIVATE ac4::core)

add_library(ac4::decoder_static ALIAS ac4dec_static)
add_library(ac4::decoder_shared ALIAS ac4dec_shared)
if(BUILD_SHARED_LIBS)
    add_library(ac4::decoder ALIAS ac4dec_shared)
else()
    add_library(ac4::decoder ALIAS ac4dec_static)
endif()

target_sources(ac4dec_objects
    PRIVATE
        src/decoder.cpp
        src/huffman.cpp
        src/pcm/acpl.cpp
        src/pcm/ajcc.cpp
        src/pcm/ajoc.cpp
        src/pcm/isf.cpp
        src/pcm/asf_reconstruct.cpp
        src/pcm/aspx.cpp
        src/pcm/companding.cpp
        src/pcm/de.cpp
        src/pcm/downmix.cpp
        src/pcm/drc.cpp
        src/pcm/immersive.cpp
        src/pcm/mixer.cpp
        src/pcm/multichannel.cpp
        src/pcm/objects.cpp
        src/pcm/renderer.cpp
        src/pcm/routing.cpp
        src/pcm/snf_random.cpp
        src/pcm/stereo.cpp
        src/pcm/substream_pcm.cpp
        src/presentations.cpp
        src/syntax/asf.cpp
        src/syntax/aspx.cpp
        src/syntax/acpl.cpp
        src/syntax/ajcc.cpp
        src/syntax/ajoc.cpp
        src/syntax/channel_elements.cpp
        src/syntax/metadata.cpp
        src/syntax/oamd.cpp
        src/syntax/presentation.cpp
        src/syntax/substream.cpp)

target_include_directories(ac4dec_objects
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
        "$<INSTALL_INTERFACE:include>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_features(ac4dec_objects PUBLIC cxx_std_23)

# The inspector's and the core's usage requirements, compiled with and not
# linked: the libraries above link them (see there). ac4::ac4 is the one
# BUILD_SHARED_LIBS picks, as ac3::signing's objects take ac3::forge.
# $<BUILD_INTERFACE:...> on all four for the reason src/ac4/CMakeLists.txt
# gives, and because nothing that uses the installed package compiles these
# objects.
target_link_libraries(ac4dec_objects
    PRIVATE
        "$<BUILD_INTERFACE:$<COMPILE_ONLY:ac4::ac4>>"
        "$<BUILD_INTERFACE:$<COMPILE_ONLY:ac4::core>>"
        "$<BUILD_INTERFACE:ac3::warnings>"
        "$<BUILD_INTERFACE:ac3::coverage>")

set_target_properties(ac4dec_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

# MSVC's C4251 for the PRIVATE STL-typed members of exported classes - the
# same suppression, for the same reason, as src/ac4/CMakeLists.txt.
target_compile_options(ac4dec_objects PUBLIC "$<$<CXX_COMPILER_ID:MSVC>:/wd4251>")

include(GenerateExportHeader)
set_target_properties(ac4dec_shared PROPERTIES DEFINE_SYMBOL "AC4DEC_BUILDING_SHARED")
generate_export_header(ac4dec_shared
    BASE_NAME AC4DEC
    EXPORT_MACRO_NAME AC4DEC_EXPORT
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/generated/ac4dec/export.hpp"
    DEFINE_NO_DEPRECATED
    STATIC_DEFINE AC4DEC_STATIC_DEFINE)
target_compile_definitions(ac4dec_objects PRIVATE AC4DEC_BUILDING_SHARED)
target_compile_definitions(ac4dec_static PUBLIC AC4DEC_STATIC_DEFINE)

set_target_properties(ac4dec_shared PROPERTIES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION}")

set_target_properties(ac4dec_shared PROPERTIES OUTPUT_NAME "ac4dec")
set_target_properties(ac4dec_static PROPERTIES OUTPUT_NAME "ac4dec_static")

# install(EXPORT ... NAMESPACE ac4::) prefixes the name a target is exported
# under, which is its own name unless this says otherwise: ac4::ac4dec_static
# where the tree says ac4::decoder_static.
set_target_properties(ac4dec_objects PROPERTIES EXPORT_NAME "decoder_objects")
set_target_properties(ac4dec_static PROPERTIES EXPORT_NAME "decoder_static")
set_target_properties(ac4dec_shared PROPERTIES EXPORT_NAME "decoder_shared")
