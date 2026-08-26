# ---------------------------------------------------------------------------
# iamf::iamf - a standalone writer for AOM's IAMF (Immersive Audio Model and
# Formats) v1.1.0, roadmap item IM3 phase 1 of 3 (see ROADMAP.md): an
# `ipcm`-substream OBU/ISOBMFF writer for a channel-based 7.1.4 element,
# fed from an E-AC-3 decode. E-AC-3 is not in IAMF's codec list (Opus,
# AAC-LC, FLAC, LPCM only) and never can be, so this is a decode-then-rewrap
# bridge, not a new encoder output - see include/iamf/iamf.hpp's own header
# comment for the full picture. Phase 2 (object elements) waits on IAMF
# v2.0; phase 3 (an OBU reader) is separate later work.
#
# Codec-blind by design, the same shape matroska::matroska, mp4::mp4,
# mpegts::mpegts and ac3iab::ac3iab already use for their own formats: it
# links nothing from ac3::forge and knows nothing about AC-3, E-AC-3 or JOC
# (see CONTRIBUTING.md's repository-layout section on what a bare
# `include/iamf/` prefix, with no `ac3/`, means).
#
# Like ac3iab (src/ac3iab), this module has no third-party dependency of its
# own - it writes IAMF's OBU/ISOBMFF syntax directly against the published
# spec, the same way ac3iab hand-rolls its own bit reader for SMPTE ST
# 2098-2 - so, like the three container writers and ac3iab, it defaults ON
# rather than OFF (unlike ac3adm, src/ac3adm, whose opt-in/excluded-from-
# install treatment exists specifically because IT vendors third-party
# Boost/libadm code - a reason that does not apply here). It has no opt-in
# vcpkg feature of its own yet, following ac3iab's own precedent: new in
# this PR, nothing downstream (CLI, GUI, examples beyond mux_iamf.cpp)
# consumes it yet.
#
# Built and installed as both a static and a shared library - see
# src/forge/CMakeLists.txt's ac3::forge for the full explanation of why an
# OBJECT library plus two thin wrapper targets, rather than one
# add_library().
# ---------------------------------------------------------------------------
add_library(iamf_objects OBJECT)
add_library(iamf_static STATIC)
add_library(iamf_shared SHARED)

target_link_libraries(iamf_static PUBLIC iamf_objects)
target_link_libraries(iamf_shared PUBLIC iamf_objects)

add_library(iamf::iamf_static ALIAS iamf_static)
add_library(iamf::iamf_shared ALIAS iamf_shared)
if(BUILD_SHARED_LIBS)
    add_library(iamf::iamf ALIAS iamf_shared)
else()
    add_library(iamf::iamf ALIAS iamf_static)
endif()

target_sources(iamf_objects
    PRIVATE
        src/iamf.cpp)

target_include_directories(iamf_objects
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
        "$<INSTALL_INTERFACE:include>")

target_compile_features(iamf_objects PUBLIC cxx_std_23)

# $<BUILD_INTERFACE:...> wrapping ac3::warnings/ac3::coverage - see
# src/forge/CMakeLists.txt's identical forge_objects treatment for why a
# plain PRIVATE link isn't enough for an OBJECT library that's part of an
# install(EXPORT) set.
target_link_libraries(iamf_objects PRIVATE
    "$<BUILD_INTERFACE:ac3::warnings>"
    "$<BUILD_INTERFACE:ac3::coverage>")

set_target_properties(iamf_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

include(GenerateExportHeader)
set_target_properties(iamf_shared PROPERTIES DEFINE_SYMBOL "IAMF_BUILDING_SHARED")
generate_export_header(iamf_shared
    BASE_NAME IAMF
    EXPORT_MACRO_NAME IAMF_EXPORT
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/generated/iamf/export.hpp"
    DEFINE_NO_DEPRECATED
    STATIC_DEFINE IAMF_STATIC_DEFINE)
target_compile_definitions(iamf_objects PRIVATE IAMF_BUILDING_SHARED)
target_compile_definitions(iamf_static PUBLIC IAMF_STATIC_DEFINE)

set_target_properties(iamf_shared PROPERTIES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION}")

set_target_properties(iamf_shared PROPERTIES OUTPUT_NAME "iamf")
set_target_properties(iamf_static PROPERTIES OUTPUT_NAME "iamf_static")
