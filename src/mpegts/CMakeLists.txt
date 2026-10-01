# ---------------------------------------------------------------------------
# iclforge::mpegts - a minimal MPEG-2 Transport Stream muxer.
#
# Deliberately standalone: it links nothing from iclforge::ac3 and knows nothing
# about AC-3 beyond the two stream_type/descriptor choices iclforge::io::StreamKind
# maps onto. It packetizes PAT + PMT + one PES-wrapped elementary stream into
# 188-byte TS packets, taking every access unit as opaque bytes, so it can be
# reasoned about, tested and reused on its own - the same shape as
# iclforge::matroska (src/matroska/), which this file mirrors structurally.
# The dependency runs one way only - front ends may use both, but the codec
# library never reaches for the container and vice versa.
#
# Built and installed as both a static and a shared library - see
# src/ac3/CMakeLists.txt's iclforge::ac3 for the full explanation of why an
# OBJECT library plus two thin wrapper targets, rather than one add_library().
# ---------------------------------------------------------------------------
add_library(iclforge_mpegts_objects OBJECT)
add_library(iclforge_mpegts_static STATIC)
add_library(iclforge_mpegts_shared SHARED)

target_link_libraries(iclforge_mpegts_static PUBLIC iclforge_mpegts_objects)
target_link_libraries(iclforge_mpegts_shared PUBLIC iclforge_mpegts_objects)

add_library(iclforge::mpegts_static ALIAS iclforge_mpegts_static)
add_library(iclforge::mpegts_shared ALIAS iclforge_mpegts_shared)
if(BUILD_SHARED_LIBS)
    add_library(iclforge::mpegts ALIAS iclforge_mpegts_shared)
else()
    add_library(iclforge::mpegts ALIAS iclforge_mpegts_static)
endif()

target_sources(iclforge_mpegts_objects
    PRIVATE
        src/mpegts.cpp
        src/reader.cpp)

target_include_directories(iclforge_mpegts_objects
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
        "$<INSTALL_INTERFACE:include>")

target_compile_features(iclforge_mpegts_objects PUBLIC cxx_std_23)

# $<BUILD_INTERFACE:...> wrapping - see src/ac3/CMakeLists.txt's identical
# iclforge_ac3_objects treatment for why a plain PRIVATE link isn't enough for
# an OBJECT library that's part of an install(EXPORT) set. iclforge::coverage is a
# no-op unless ICLFORGE_ENABLE_COVERAGE is on (see cmake/Coverage.cmake).
target_link_libraries(iclforge_mpegts_objects PRIVATE
    "$<BUILD_INTERFACE:iclforge::warnings>"
    "$<BUILD_INTERFACE:iclforge::coverage>")

set_target_properties(iclforge_mpegts_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

# MSVC's C4251 for the PRIVATE STL-typed members of ICLFORGE_MPEGTS_EXPORT classes
# (iclforge::mpegts::Writer's PSI section buffers) - the identical, identically-reasoned
# suppression src/matroska/CMakeLists.txt applies for iclforge::matroska::Writer; see
# its comment for why PUBLIC and why here rather than header pragmas.
target_compile_options(iclforge_mpegts_objects PUBLIC "$<$<CXX_COMPILER_ID:MSVC>:/wd4251>")

include(GenerateExportHeader)
set_target_properties(iclforge_mpegts_shared PROPERTIES DEFINE_SYMBOL "ICLFORGE_MPEGTS_BUILDING_SHARED")
generate_export_header(iclforge_mpegts_shared
    BASE_NAME ICLFORGE_MPEGTS
    EXPORT_MACRO_NAME ICLFORGE_MPEGTS_EXPORT
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/generated/iclforge/mpegts/export.hpp"
    DEFINE_NO_DEPRECATED
    STATIC_DEFINE ICLFORGE_MPEGTS_STATIC_DEFINE)
target_compile_definitions(iclforge_mpegts_objects PRIVATE ICLFORGE_MPEGTS_BUILDING_SHARED)
target_compile_definitions(iclforge_mpegts_static PUBLIC ICLFORGE_MPEGTS_STATIC_DEFINE)

set_target_properties(iclforge_mpegts_shared PROPERTIES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION}")

set_target_properties(iclforge_mpegts_shared PROPERTIES OUTPUT_NAME "iclforge_mpegts")
set_target_properties(iclforge_mpegts_static PROPERTIES OUTPUT_NAME "iclforge_mpegts_static")
