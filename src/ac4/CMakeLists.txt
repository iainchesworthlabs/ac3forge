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
# The minimum-footprint profile (AC3FORGE_MINIMAL_AC4, planning/ac4.md, D14b)
# takes the objects into the archive as sources and passes on only the OBJECT
# library's compile-time requirements. ESP-IDF's linker-script generator asks
# every library in an executable's link graph for its file, which an OBJECT
# library has none of; a static archive that links it hands it that graph.
if(AC3FORGE_MINIMAL_DECODER)
    add_library(ac4_static STATIC $<TARGET_OBJECTS:ac4_objects>)
    target_link_libraries(ac4_static PUBLIC $<COMPILE_ONLY:ac4_objects>)
else()
    add_library(ac4_static STATIC)
    target_link_libraries(ac4_static PUBLIC ac4_objects)
endif()

add_library(ac4::ac4_static ALIAS ac4_static)

# The shared library is what a bare-metal build has no use for and cannot have:
# no dynamic loader, and CMake refuses add_library(SHARED) for a target that has
# none (policy CMP0164). The minimum-footprint profile (AC3FORGE_MINIMAL_AC4,
# planning/ac4.md, D14b) builds the static archive alone; the same test guards
# each line below that names ac4_shared.
if(NOT AC3FORGE_MINIMAL_DECODER)
    add_library(ac4_shared SHARED)
    target_link_libraries(ac4_shared PUBLIC ac4_objects)
    add_library(ac4::ac4_shared ALIAS ac4_shared)
endif()
if(BUILD_SHARED_LIBS AND NOT AC3FORGE_MINIMAL_DECODER)
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

# In the minimum-footprint profile: its compile options (no exceptions, no RTTI,
# a section for each function and datum), PUBLIC for a consumer to share, and
# code that is not position-independent - see src/ac4core/CMakeLists.txt.
if(AC3FORGE_MINIMAL_DECODER)
    target_link_libraries(ac4_objects PUBLIC ac3::minimal_profile)
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
if(NOT AC3FORGE_MINIMAL_DECODER)
    set_target_properties(ac4_shared PROPERTIES DEFINE_SYMBOL "AC4_BUILDING_SHARED")
    set(_ac4_export_target ac4_shared)
else()
    # The header is the same either way: the profile's consumers define
    # AC4_STATIC_DEFINE (below), and it asks nothing of the target it is made from.
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

if(NOT AC3FORGE_MINIMAL_DECODER)
    set_target_properties(ac4_shared PROPERTIES
        VERSION "${PROJECT_VERSION}"
        SOVERSION "${PROJECT_VERSION}")

    set_target_properties(ac4_shared PROPERTIES OUTPUT_NAME "ac4")
endif()
set_target_properties(ac4_static PROPERTIES OUTPUT_NAME "ac4_static")
