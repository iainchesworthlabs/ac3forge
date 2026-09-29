# ---------------------------------------------------------------------------
# ac4::ac4 - an AC-4 sync-frame / TOC / presentation / substream-group
# bitstream inspector.
#
# Deliberately standalone: it links nothing from ac3::forge and knows
# nothing about AC-3, E-AC-3 or Atmos - AC-4 is a peer codec, not an
# extension of this project's own one, so it gets the same "codec-blind"
# treatment mpegts::/mp4::/matroska:: do rather than the ac3:: prefix. It
# reads TOC/presentation/substream-group framing and reports audio_data and
# metadata() payloads as byte ranges, never decoding them - see
# include/ac4/ac4.hpp for the full scope statement. Built as both a static
# and a shared library, and installed and exported with the decoder and the
# core (cmake/InstallLibrary.cmake). See src/forge/CMakeLists.txt's
# ac3::forge for the full explanation of why an OBJECT library plus two thin
# wrapper targets, rather than one add_library().
# ---------------------------------------------------------------------------
add_library(ac4_objects OBJECT)
add_library(ac4_static STATIC)
# Static only under the minimum-footprint profile (AC3FORGE_MINIMAL_DECODER): the parts
# it builds for have no dynamic loader, and CMake refuses add_library(SHARED) for a
# platform that has none (src/forge/minimal.cmake says the same of ac3::forge).
if(NOT AC3FORGE_MINIMAL_DECODER)
    add_library(ac4_shared SHARED)
endif()

target_link_libraries(ac4_static PUBLIC ac4_objects)
if(TARGET ac4_shared)
    target_link_libraries(ac4_shared PUBLIC ac4_objects)
endif()

add_library(ac4::ac4_static ALIAS ac4_static)
if(TARGET ac4_shared)
    add_library(ac4::ac4_shared ALIAS ac4_shared)
endif()
if(BUILD_SHARED_LIBS AND TARGET ac4_shared)
    add_library(ac4::ac4 ALIAS ac4_shared)
else()
    add_library(ac4::ac4 ALIAS ac4_static)
endif()

target_sources(ac4_objects
    PRIVATE
        src/ac4.cpp)

target_include_directories(ac4_objects
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
        "$<INSTALL_INTERFACE:include>")

target_compile_features(ac4_objects PUBLIC cxx_std_23)

# $<BUILD_INTERFACE:...> wrapping - see src/forge/CMakeLists.txt's identical
# forge_objects treatment for why a plain PRIVATE link isn't enough for an
# OBJECT library that's part of an install(EXPORT) set. ac3::coverage is a
# no-op unless AC3FORGE_ENABLE_COVERAGE is on (see cmake/Coverage.cmake).
target_link_libraries(ac4_objects PRIVATE
    "$<BUILD_INTERFACE:ac3::warnings>"
    "$<BUILD_INTERFACE:ac3::coverage>")

# Position-independent for the shared library's sake, which the minimum-footprint
# profile has none of: there the objects go into a static image at a fixed address,
# and -fPIC would only add a table of addresses to every access to a global.
if(AC3FORGE_MINIMAL_DECODER)
    set(_ac4_pic OFF)
else()
    set(_ac4_pic ON)
endif()
set_target_properties(ac4_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ${_ac4_pic}
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

# MSVC's C4251 for the PRIVATE STL-typed members of AC4_EXPORT classes - the
# identical, identically-reasoned suppression src/mpegts/CMakeLists.txt
# applies for mpegts::Writer; see its comment for why PUBLIC and why here
# rather than header pragmas.
target_compile_options(ac4_objects PUBLIC "$<$<CXX_COMPILER_ID:MSVC>:/wd4251>")

include(GenerateExportHeader)
# The header is the same whichever target it is generated from; the static library
# generates it where there is no shared one.
if(TARGET ac4_shared)
    set_target_properties(ac4_shared PROPERTIES DEFINE_SYMBOL "AC4_BUILDING_SHARED")
    set(_ac4_export_target ac4_shared)
else()
    set(_ac4_export_target ac4_static)
endif()
generate_export_header(${_ac4_export_target}
    BASE_NAME AC4
    EXPORT_MACRO_NAME AC4_EXPORT
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/generated/ac4/export.hpp"
    DEFINE_NO_DEPRECATED
    STATIC_DEFINE AC4_STATIC_DEFINE)
target_compile_definitions(ac4_objects PRIVATE AC4_BUILDING_SHARED)
target_compile_definitions(ac4_static PUBLIC AC4_STATIC_DEFINE)

if(TARGET ac4_shared)
    set_target_properties(ac4_shared PROPERTIES
        VERSION "${PROJECT_VERSION}"
        SOVERSION "${PROJECT_VERSION}")
    set_target_properties(ac4_shared PROPERTIES OUTPUT_NAME "ac4")
endif()
set_target_properties(ac4_static PROPERTIES OUTPUT_NAME "ac4_static")
