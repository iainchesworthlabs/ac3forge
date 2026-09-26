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
# thin wrapper targets rather than one add_library().
# ---------------------------------------------------------------------------
add_library(ac4dec_objects OBJECT)
add_library(ac4dec_static STATIC)
add_library(ac4dec_shared SHARED)

target_link_libraries(ac4dec_static PUBLIC ac4dec_objects)
target_link_libraries(ac4dec_shared PUBLIC ac4dec_objects)

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
        src/pcm/asf_reconstruct.cpp
        src/pcm/aspx.cpp
        src/pcm/companding.cpp
        src/pcm/de.cpp
        src/pcm/downmix.cpp
        src/pcm/drc.cpp
        src/pcm/mixer.cpp
        src/pcm/multichannel.cpp
        src/pcm/routing.cpp
        src/pcm/snf_random.cpp
        src/pcm/stereo.cpp
        src/pcm/substream_pcm.cpp
        src/presentations.cpp
        src/syntax/asf.cpp
        src/syntax/aspx.cpp
        src/syntax/acpl.cpp
        src/syntax/channel_elements.cpp
        src/syntax/metadata.cpp
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

# ac4::ac4 is PUBLIC: the decoder's own headers name the inspector's Toc and
# presentation types. ac4::core (src/ac4core: the tables and transforms the
# encoder shares) is PRIVATE, and reaches the static and shared wrappers'
# link lines through this OBJECT library's link interface.
# $<BUILD_INTERFACE:...> on the rest for the reason src/ac4/CMakeLists.txt
# gives.
target_link_libraries(ac4dec_objects
    PUBLIC
        ac4::ac4
    PRIVATE
        ac4::core
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
