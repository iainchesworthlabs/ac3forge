# ---------------------------------------------------------------------------
# iclforge::iamf - a standalone writer for AOM's IAMF (Immersive Audio Model and
# Formats) v1.1.0, phase 1 of 3: an
# `ipcm`-substream OBU/ISOBMFF writer for a channel-based 7.1.4 element,
# fed from an E-AC-3 decode. E-AC-3 is not in IAMF's codec list (Opus,
# AAC-LC, FLAC, LPCM only) and never can be, so this is a decode-then-rewrap
# bridge, not a new encoder output - see include/iamf/iamf.hpp's own header
# comment for the full picture. Phase 2 (object elements) waits on IAMF
# v2.0; phase 3 (an OBU reader) is separate later work.
#
# Codec-blind by design, the same shape iclforge::matroska, iclforge::mp4,
# iclforge::mpegts and iclforge::iab already use for their own formats: it
# links nothing from iclforge::ac3 and knows nothing about AC-3, E-AC-3 or JOC
# (see CONTRIBUTING.md's repository-layout section on what a bare
# `include/iamf/` prefix, with no `ac3/`, means).
#
# Like ac3iab (src/iab), this module has no third-party dependency of its
# own - it writes IAMF's OBU/ISOBMFF syntax directly against the published
# spec, the same way ac3iab hand-rolls its own bit reader for SMPTE ST
# 2098-2 - so, like the three container writers and ac3iab, it defaults ON
# rather than OFF (unlike ac3adm, src/adm, whose opt-in/excluded-from-
# install treatment exists specifically because IT vendors third-party
# Boost/libadm code - a reason that does not apply here). It has no opt-in
# vcpkg feature of its own yet, following ac3iab's own precedent: new in
# this PR, nothing downstream (CLI, GUI, examples beyond mux_iamf.cpp)
# consumes it yet.
#
# Built and installed as both a static and a shared library - see
# src/ac3/CMakeLists.txt's iclforge::ac3 for the full explanation of why an
# OBJECT library plus two thin wrapper targets, rather than one
# add_library().
# ---------------------------------------------------------------------------
add_library(iclforge_iamf_objects OBJECT)
add_library(iclforge_iamf_static STATIC)
add_library(iclforge_iamf_shared SHARED)

target_link_libraries(iclforge_iamf_static PUBLIC iclforge_iamf_objects)
target_link_libraries(iclforge_iamf_shared PUBLIC iclforge_iamf_objects)

add_library(iclforge::iamf_static ALIAS iclforge_iamf_static)
add_library(iclforge::iamf_shared ALIAS iclforge_iamf_shared)
if(BUILD_SHARED_LIBS)
    add_library(iclforge::iamf ALIAS iclforge_iamf_shared)
else()
    add_library(iclforge::iamf ALIAS iclforge_iamf_static)
endif()

target_sources(iclforge_iamf_objects
    PRIVATE
        src/iamf.cpp)

target_include_directories(iclforge_iamf_objects
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
        "$<INSTALL_INTERFACE:include>")

target_compile_features(iclforge_iamf_objects PUBLIC cxx_std_23)

# $<BUILD_INTERFACE:...> wrapping iclforge::warnings/iclforge::coverage - see
# src/ac3/CMakeLists.txt's identical iclforge_ac3_objects treatment for why a
# plain PRIVATE link isn't enough for an OBJECT library that's part of an
# install(EXPORT) set.
target_link_libraries(iclforge_iamf_objects PRIVATE
    "$<BUILD_INTERFACE:iclforge::warnings>"
    "$<BUILD_INTERFACE:iclforge::coverage>")

set_target_properties(iclforge_iamf_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

include(GenerateExportHeader)
set_target_properties(iclforge_iamf_shared PROPERTIES DEFINE_SYMBOL "ICLFORGE_IAMF_BUILDING_SHARED")
generate_export_header(iclforge_iamf_shared
    BASE_NAME ICLFORGE_IAMF
    EXPORT_MACRO_NAME ICLFORGE_IAMF_EXPORT
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/generated/iclforge/iamf/export.hpp"
    DEFINE_NO_DEPRECATED
    STATIC_DEFINE ICLFORGE_IAMF_STATIC_DEFINE)
target_compile_definitions(iclforge_iamf_objects PRIVATE ICLFORGE_IAMF_BUILDING_SHARED)
target_compile_definitions(iclforge_iamf_static PUBLIC ICLFORGE_IAMF_STATIC_DEFINE)

set_target_properties(iclforge_iamf_shared PROPERTIES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION}")

set_target_properties(iclforge_iamf_shared PROPERTIES OUTPUT_NAME "iclforge_iamf")
set_target_properties(iclforge_iamf_static PROPERTIES OUTPUT_NAME "iclforge_iamf_static")
