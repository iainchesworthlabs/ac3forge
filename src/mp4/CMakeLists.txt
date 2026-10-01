# ---------------------------------------------------------------------------
# iclforge::mp4 - a minimal MP4 (ISOBMFF) container writer, plus fragmented
# MP4/CMAF segmenting and HLS/DASH signaling helpers.
#
# Deliberately standalone: it links nothing from iclforge::ac3 and knows nothing
# about AC-3. It nests ISOBMFF boxes and lays out one audio track's samples,
# taking every frame - and the codec's sample-entry configuration box - as
# opaque bytes, so it can be reasoned about, tested and reused on its own.
# The dependency runs one way only - front ends may use both this and
# iclforge::ac3, but the codec library never reaches for the container and vice
# versa. Mirrors src/matroska/CMakeLists.txt in every particular; see that
# file's own header for the OBJECT/STATIC/SHARED shape's full rationale.
#
# mp4.cpp (mux()) and fragment.cpp (fragment()) share their ISOBMFF
# box-writing plumbing through src/isobmff_detail.hpp; hls.cpp and dash.cpp
# share their segment-duration/bandwidth arithmetic through
# src/manifest_detail.hpp - neither is a public header, both are `inline`
# functions included by more than one .cpp in this same target (see either
# file's own comment).
# ---------------------------------------------------------------------------
add_library(iclforge_mp4_objects OBJECT)
add_library(iclforge_mp4_static STATIC)
add_library(iclforge_mp4_shared SHARED)

target_link_libraries(iclforge_mp4_static PUBLIC iclforge_mp4_objects)
target_link_libraries(iclforge_mp4_shared PUBLIC iclforge_mp4_objects)

add_library(iclforge::mp4_static ALIAS iclforge_mp4_static)
add_library(iclforge::mp4_shared ALIAS iclforge_mp4_shared)
if(BUILD_SHARED_LIBS)
    add_library(iclforge::mp4 ALIAS iclforge_mp4_shared)
else()
    add_library(iclforge::mp4 ALIAS iclforge_mp4_static)
endif()

target_sources(iclforge_mp4_objects
    PRIVATE
        src/mp4.cpp
        src/reader.cpp
        src/fragment.cpp
        src/hls.cpp
        src/dash.cpp)

target_include_directories(iclforge_mp4_objects
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
        "$<INSTALL_INTERFACE:include>")

target_compile_features(iclforge_mp4_objects PUBLIC cxx_std_23)

# $<BUILD_INTERFACE:...> wrapping - see src/ac3/CMakeLists.txt's identical
# iclforge_ac3_objects treatment for why a plain PRIVATE link isn't enough for
# an OBJECT library that's part of an install(EXPORT) set. iclforge::coverage is a
# no-op unless ICLFORGE_ENABLE_COVERAGE is on (see cmake/Coverage.cmake).
# iclforge::fmt_private compiles {fmt} into hls.cpp and dash.cpp instead of linking it, so
# iclforge_mp4_static carries no undefined {fmt} reference into a consumer's link - see cmake/Fmt.cmake.
target_link_libraries(iclforge_mp4_objects PRIVATE
    "$<BUILD_INTERFACE:iclforge::warnings>"
    "$<BUILD_INTERFACE:iclforge::coverage>"
    "$<BUILD_INTERFACE:iclforge::fmt_private>")

set_target_properties(iclforge_mp4_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

# MSVC's C4251 ("class X needs to have dll-interface...") fires for every
# PRIVATE STL-typed member of an ICLFORGE_MP4_EXPORT class - iclforge::mp4::Reader's
# std::unique_ptr to its parse state, and iclforge::mp4::FragmentWriter's own
# std::vector buffers and segment window - pure implementation detail, never
# touched by a caller except through the class's own (exported) methods.
# Suppressed here rather than by scattering non-portable #pragma
# warning(disable:) through headers GCC/Clang also compile - see
# src/matroska/CMakeLists.txt's identical iclforge_matroska_objects treatment,
# including why this is PUBLIC rather than PRIVATE (the warning fires wherever
# a translation unit parses the dllexport/dllimport-decorated class
# definition, not only inside this target's own compilation). mp4:: needed
# none of this before these two: every other export here is a free function
# or a plain aggregate.
target_compile_options(iclforge_mp4_objects PUBLIC "$<$<CXX_COMPILER_ID:MSVC>:/wd4251>")

include(GenerateExportHeader)
set_target_properties(iclforge_mp4_shared PROPERTIES DEFINE_SYMBOL "ICLFORGE_MP4_BUILDING_SHARED")
generate_export_header(iclforge_mp4_shared
    BASE_NAME ICLFORGE_MP4
    EXPORT_MACRO_NAME ICLFORGE_MP4_EXPORT
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/generated/iclforge/mp4/export.hpp"
    DEFINE_NO_DEPRECATED
    STATIC_DEFINE ICLFORGE_MP4_STATIC_DEFINE)
target_compile_definitions(iclforge_mp4_objects PRIVATE ICLFORGE_MP4_BUILDING_SHARED)
target_compile_definitions(iclforge_mp4_static PUBLIC ICLFORGE_MP4_STATIC_DEFINE)

set_target_properties(iclforge_mp4_shared PROPERTIES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION}")

set_target_properties(iclforge_mp4_shared PROPERTIES OUTPUT_NAME "iclforge_mp4")
set_target_properties(iclforge_mp4_static PROPERTIES OUTPUT_NAME "iclforge_mp4_static")
