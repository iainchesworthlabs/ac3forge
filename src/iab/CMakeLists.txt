# ---------------------------------------------------------------------------
# iclforge::iab - a standalone reader for SMPTE ST 2098-2:2022's Immersive
# Audio Bitstream (IAB), phase 1 of 3 - a
# codec-blind bitstream parser, the same shape iclforge::matroska, iclforge::mp4
# and iclforge::mpegts already use for their own containers: it links nothing
# from iclforge::ac3 and knows nothing about AC-3, E-AC-3 or JOC, so it can be
# reasoned about, tested and reused on its own (see src/mpegts/CMakeLists.txt's
# own header, which this file mirrors structurally).
#
# Unlike iclforge::adm (src/adm), this module has no third-party dependency at
# all - IAB's own element/Plex-coded bitstream needs nothing beyond this
# module's own bit reader (src/bitreader.hpp) - so, like the three container
# writers, it defaults ON rather than OFF, and (also unlike iclforge::adm) has no
# opt-in vcpkg feature of its own yet: it is new in this PR and nothing
# downstream (CLI, GUI, examples) consumes it yet - IAB reader's phase 3
# (`atmos-iab`, mapping onto iclforge::admbridge) is what will eventually give it
# a consumer. Following iclforge::c's own precedent (see
# packaging/vcpkg-port/iclforge/portfile.cmake's header comment), it is left
# out of the vcpkg port until it has one.
#
# Built and installed as both a static and a shared library - see
# src/ac3/CMakeLists.txt's iclforge::ac3 for the full explanation of why an
# OBJECT library plus two thin wrapper targets, rather than one
# add_library().
# ---------------------------------------------------------------------------
add_library(iclforge_iab_objects OBJECT)
add_library(iclforge_iab_static STATIC)
add_library(iclforge_iab_shared SHARED)

target_link_libraries(iclforge_iab_static PUBLIC iclforge_iab_objects)
target_link_libraries(iclforge_iab_shared PUBLIC iclforge_iab_objects)

add_library(iclforge::iab_static ALIAS iclforge_iab_static)
add_library(iclforge::iab_shared ALIAS iclforge_iab_shared)
if(BUILD_SHARED_LIBS)
    add_library(iclforge::iab ALIAS iclforge_iab_shared)
else()
    add_library(iclforge::iab ALIAS iclforge_iab_static)
endif()

target_sources(iclforge_iab_objects
    PRIVATE
        src/bitreader.cpp
        src/iab_reader.cpp
        src/iab.cpp
        src/mxf_reader.cpp)

target_include_directories(iclforge_iab_objects
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
        "$<INSTALL_INTERFACE:include>")

target_compile_features(iclforge_iab_objects PUBLIC cxx_std_23)

# $<BUILD_INTERFACE:...> wrapping iclforge::warnings/iclforge::coverage - see
# src/ac3/CMakeLists.txt's identical iclforge_ac3_objects treatment for why a
# plain PRIVATE link isn't enough for an OBJECT library that's part of an
# install(EXPORT) set.
target_link_libraries(iclforge_iab_objects PRIVATE
    "$<BUILD_INTERFACE:iclforge::warnings>"
    "$<BUILD_INTERFACE:iclforge::coverage>")

set_target_properties(iclforge_iab_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

include(GenerateExportHeader)
set_target_properties(iclforge_iab_shared PROPERTIES DEFINE_SYMBOL "ICLFORGE_IAB_BUILDING_SHARED")
generate_export_header(iclforge_iab_shared
    BASE_NAME ICLFORGE_IAB
    EXPORT_MACRO_NAME ICLFORGE_IAB_EXPORT
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/generated/iclforge/iab/export.hpp"
    DEFINE_NO_DEPRECATED
    STATIC_DEFINE ICLFORGE_IAB_STATIC_DEFINE)
target_compile_definitions(iclforge_iab_objects PRIVATE ICLFORGE_IAB_BUILDING_SHARED)
target_compile_definitions(iclforge_iab_static PUBLIC ICLFORGE_IAB_STATIC_DEFINE)

set_target_properties(iclforge_iab_shared PROPERTIES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION}")

set_target_properties(iclforge_iab_shared PROPERTIES OUTPUT_NAME "iclforge_iab")
set_target_properties(iclforge_iab_static PROPERTIES OUTPUT_NAME "iclforge_iab_static")
