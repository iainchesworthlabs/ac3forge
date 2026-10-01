# ---------------------------------------------------------------------------
# iclforge::matroska - a minimal MKV container writer and reader.
#
# Deliberately standalone: it links nothing from iclforge::ac3 and knows nothing
# about AC-3. It nests EBML elements and lays out clusters, taking every frame
# as opaque bytes, so it can be reasoned about, tested and reused on its own.
# The dependency runs one way only - front ends may use both, but the codec
# library never reaches for the container and vice versa.
#
# Built and installed as both a static and a shared library - see
# src/ac3/CMakeLists.txt's iclforge::ac3 for the full explanation of why an
# OBJECT library plus two thin wrapper targets, rather than one add_library().
# ---------------------------------------------------------------------------
add_library(iclforge_matroska_objects OBJECT)
add_library(iclforge_matroska_static STATIC)
add_library(iclforge_matroska_shared SHARED)

target_link_libraries(iclforge_matroska_static PUBLIC iclforge_matroska_objects)
target_link_libraries(iclforge_matroska_shared PUBLIC iclforge_matroska_objects)

add_library(iclforge::matroska_static ALIAS iclforge_matroska_static)
add_library(iclforge::matroska_shared ALIAS iclforge_matroska_shared)
if(BUILD_SHARED_LIBS)
    add_library(iclforge::matroska ALIAS iclforge_matroska_shared)
else()
    add_library(iclforge::matroska ALIAS iclforge_matroska_static)
endif()

target_sources(iclforge_matroska_objects
    PRIVATE
        src/matroska.cpp
        src/reader.cpp)

target_include_directories(iclforge_matroska_objects
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
        "$<INSTALL_INTERFACE:include>")

target_compile_features(iclforge_matroska_objects PUBLIC cxx_std_23)

# $<BUILD_INTERFACE:...> wrapping - see src/ac3/CMakeLists.txt's identical
# iclforge_ac3_objects treatment for why a plain PRIVATE link isn't enough for
# an OBJECT library that's part of an install(EXPORT) set. iclforge::coverage is a
# no-op unless ICLFORGE_ENABLE_COVERAGE is on (see cmake/Coverage.cmake).
target_link_libraries(iclforge_matroska_objects PRIVATE
    "$<BUILD_INTERFACE:iclforge::warnings>"
    "$<BUILD_INTERFACE:iclforge::coverage>")

set_target_properties(iclforge_matroska_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

# MSVC's C4251 ("class X needs to have dll-interface...") fires for every
# PRIVATE STL-typed member of a ICLFORGE_MATROSKA_EXPORT class (iclforge::matroska::Writer's own
# std::vector<std::byte> buffers) - pure implementation detail, never touched
# by a caller except through the class's own (exported) methods. Suppressed
# here rather than by scattering non-portable #pragma warning(disable:)
# through headers GCC/Clang also compile - see src/ac3/CMakeLists.txt's
# identical iclforge_ac3_objects treatment, including why this is PUBLIC rather than
# PRIVATE (the warning fires wherever a translation unit parses the
# dllexport/dllimport-decorated class definition, not only inside this
# target's own compilation).
target_compile_options(iclforge_matroska_objects PUBLIC "$<$<CXX_COMPILER_ID:MSVC>:/wd4251>")

include(GenerateExportHeader)
set_target_properties(iclforge_matroska_shared PROPERTIES DEFINE_SYMBOL "ICLFORGE_MATROSKA_BUILDING_SHARED")
generate_export_header(iclforge_matroska_shared
    BASE_NAME ICLFORGE_MATROSKA
    EXPORT_MACRO_NAME ICLFORGE_MATROSKA_EXPORT
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/generated/iclforge/matroska/export.hpp"
    DEFINE_NO_DEPRECATED
    STATIC_DEFINE ICLFORGE_MATROSKA_STATIC_DEFINE)
target_compile_definitions(iclforge_matroska_objects PRIVATE ICLFORGE_MATROSKA_BUILDING_SHARED)
target_compile_definitions(iclforge_matroska_static PUBLIC ICLFORGE_MATROSKA_STATIC_DEFINE)

set_target_properties(iclforge_matroska_shared PROPERTIES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION}")

set_target_properties(iclforge_matroska_shared PROPERTIES OUTPUT_NAME "iclforge_matroska")
set_target_properties(iclforge_matroska_static PROPERTIES OUTPUT_NAME "iclforge_matroska_static")
