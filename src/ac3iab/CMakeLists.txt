# ---------------------------------------------------------------------------
# ac3iab::ac3iab - a standalone reader for SMPTE ST 2098-2:2022's Immersive
# Audio Bitstream (IAB). Roadmap item IM1 phase 1 of 3 (see ROADMAP.md) - a
# codec-blind bitstream parser, the same shape matroska::matroska, mp4::mp4
# and mpegts::mpegts already use for their own containers: it links nothing
# from ac3::forge and knows nothing about AC-3, E-AC-3 or JOC, so it can be
# reasoned about, tested and reused on its own (see src/mpegts/CMakeLists.txt's
# own header, which this file mirrors structurally).
#
# Unlike ac3adm (src/ac3adm), this module has no third-party dependency at
# all - IAB's own element/Plex-coded bitstream needs nothing beyond this
# module's own bit reader (src/bitreader.hpp) - so, like the three container
# writers, it defaults ON rather than OFF, and (also unlike ac3adm) has no
# opt-in vcpkg feature of its own yet: it is new in this PR and nothing
# downstream (CLI, GUI, examples) consumes it yet - roadmap IM1's phase 3
# (`atmos-iab`, mapping onto ac3::admbridge) is what will eventually give it
# a consumer. Following ac3::forge_c's own precedent (see
# packaging/vcpkg-port/ac3forge/portfile.cmake's header comment), it is left
# out of the vcpkg port until it has one.
#
# Built and installed as both a static and a shared library - see
# src/forge/CMakeLists.txt's ac3::forge for the full explanation of why an
# OBJECT library plus two thin wrapper targets, rather than one
# add_library().
# ---------------------------------------------------------------------------
add_library(ac3iab_objects OBJECT)
add_library(ac3iab_static STATIC)
add_library(ac3iab_shared SHARED)

target_link_libraries(ac3iab_static PUBLIC ac3iab_objects)
target_link_libraries(ac3iab_shared PUBLIC ac3iab_objects)

add_library(ac3iab::ac3iab_static ALIAS ac3iab_static)
add_library(ac3iab::ac3iab_shared ALIAS ac3iab_shared)
if(BUILD_SHARED_LIBS)
    add_library(ac3iab::ac3iab ALIAS ac3iab_shared)
else()
    add_library(ac3iab::ac3iab ALIAS ac3iab_static)
endif()

target_sources(ac3iab_objects
    PRIVATE
        src/bitreader.cpp
        src/iab_reader.cpp
        src/iab.cpp)

target_include_directories(ac3iab_objects
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
        "$<INSTALL_INTERFACE:include>")

target_compile_features(ac3iab_objects PUBLIC cxx_std_23)

# $<BUILD_INTERFACE:...> wrapping ac3::warnings/ac3::coverage - see
# src/forge/CMakeLists.txt's identical forge_objects treatment for why a
# plain PRIVATE link isn't enough for an OBJECT library that's part of an
# install(EXPORT) set.
target_link_libraries(ac3iab_objects PRIVATE
    "$<BUILD_INTERFACE:ac3::warnings>"
    "$<BUILD_INTERFACE:ac3::coverage>")

set_target_properties(ac3iab_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

include(GenerateExportHeader)
set_target_properties(ac3iab_shared PROPERTIES DEFINE_SYMBOL "AC3IAB_BUILDING_SHARED")
generate_export_header(ac3iab_shared
    BASE_NAME AC3IAB
    EXPORT_MACRO_NAME AC3IAB_EXPORT
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/generated/ac3iab/export.hpp"
    DEFINE_NO_DEPRECATED
    STATIC_DEFINE AC3IAB_STATIC_DEFINE)
target_compile_definitions(ac3iab_objects PRIVATE AC3IAB_BUILDING_SHARED)
target_compile_definitions(ac3iab_static PUBLIC AC3IAB_STATIC_DEFINE)

set_target_properties(ac3iab_shared PROPERTIES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION}")

set_target_properties(ac3iab_shared PROPERTIES OUTPUT_NAME "ac3iab")
set_target_properties(ac3iab_static PROPERTIES OUTPUT_NAME "ac3iab_static")
