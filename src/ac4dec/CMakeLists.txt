# ---------------------------------------------------------------------------
# iclforge::ac4dec - an AC-4 decoder, ETSI TS 103 190-1 V1.4.1 and TS 103 190-2
# V1.3.1, written from the published texts.
#
# A peer of src/ac4 (iclforge::ac4, the bitstream inspector) and, like it,
# independent of ac3::forge: AC-4 shares no bitstream syntax with AC-3 or
# E-AC-3, and a decoder that linked iclforge::ac3 would bring the whole codec
# into every AC-4 build. It links iclforge::ac4 for the table of contents and the
# presentation and substream group information, which the inspector already
# reads, and adds everything below the substream index table.
#
# Same OBJECT + static + shared arrangement as src/ac4 - see
# src/ac3/CMakeLists.txt's iclforge::ac3 for why an OBJECT library plus two
# thin wrapper targets rather than one add_library(). Installed and exported
# with the inspector and the core as iclforge::ac4dec_static and
# iclforge::ac4dec_shared (cmake/InstallLibrary.cmake), the names these aliases
# give the targets in the tree; EXPORT_NAME below makes the installed names
# the same.
# ---------------------------------------------------------------------------
add_library(iclforge_ac4dec_objects OBJECT)
# The profile's archive takes the objects as sources and passes on only the
# OBJECT library's compile-time requirements, as src/ac4/CMakeLists.txt's does
# and for its reason.
if(AC3FORGE_MINIMAL_DECODER)
    add_library(iclforge_ac4dec_static STATIC $<TARGET_OBJECTS:iclforge_ac4dec_objects>)
else()
    add_library(iclforge_ac4dec_static STATIC)
endif()
# No shared library in the minimum-footprint profile (AC3FORGE_MINIMAL_AC4,
# planning/ac4.md, D14b): a bare-metal build has no dynamic loader, and CMake
# refuses add_library(SHARED) for a target that has none. The same test guards
# each line below that names iclforge_ac4dec_shared, as src/ac4/CMakeLists.txt's does.
if(NOT AC3FORGE_MINIMAL_DECODER)
    add_library(iclforge_ac4dec_shared SHARED)
endif()

# Each library links the inspector of its own kind, PUBLIC, since the
# decoder's header names the inspector's Toc and presentation types, and the
# core PRIVATE. The static library passes the core on to whatever links it,
# as a link-only dependency, because nothing in an archive links it; the
# shared library carries the part of it that it uses and passes on nothing.
# The OBJECT library below takes both for compiling only: it has no link step
# to keep a PRIVATE dependency to itself, and both libraries link it PUBLIC for
# its include path, so a dependency linked there reached every consumer of
# either. Linked there, the core's archive was on the link line of everything
# that linked libac4dec.so, and in a static build the inspector's archive was
# too (src/capi/CMakeLists.txt found the same with iclforge::c).
if(AC3FORGE_MINIMAL_DECODER)
    target_link_libraries(iclforge_ac4dec_static PUBLIC $<COMPILE_ONLY:iclforge_ac4dec_objects> iclforge::ac4_static PRIVATE iclforge::ac4core)
else()
    target_link_libraries(iclforge_ac4dec_static PUBLIC iclforge_ac4dec_objects iclforge::ac4_static PRIVATE iclforge::ac4core)
endif()

add_library(iclforge::ac4dec_static ALIAS iclforge_ac4dec_static)
if(NOT AC3FORGE_MINIMAL_DECODER)
    target_link_libraries(iclforge_ac4dec_shared PUBLIC iclforge_ac4dec_objects iclforge::ac4_shared PRIVATE iclforge::ac4core)
    add_library(iclforge::ac4dec_shared ALIAS iclforge_ac4dec_shared)
endif()
if(BUILD_SHARED_LIBS AND NOT AC3FORGE_MINIMAL_DECODER)
    add_library(iclforge::ac4dec ALIAS iclforge_ac4dec_shared)
else()
    add_library(iclforge::ac4dec ALIAS iclforge_ac4dec_static)
endif()

target_sources(iclforge_ac4dec_objects
    PRIVATE
        src/decoder.cpp
        src/huffman.cpp
        src/pcm/acpl.cpp
        src/pcm/ajcc.cpp
        src/pcm/ajoc.cpp
        src/pcm/isf.cpp
        src/pcm/asf_reconstruct.cpp
        src/pcm/aspx.cpp
        src/pcm/companding.cpp
        src/pcm/de.cpp
        src/pcm/downmix.cpp
        src/pcm/drc.cpp
        src/pcm/immersive.cpp
        src/pcm/mixer.cpp
        src/pcm/multichannel.cpp
        src/pcm/objects.cpp
        src/pcm/renderer.cpp
        src/pcm/routing.cpp
        src/pcm/snf_random.cpp
        src/pcm/stereo.cpp
        src/pcm/substream_pcm.cpp
        src/presentations.cpp
        src/syntax/asf.cpp
        src/syntax/aspx.cpp
        src/syntax/acpl.cpp
        src/syntax/ajcc.cpp
        src/syntax/ajoc.cpp
        src/syntax/channel_elements.cpp
        src/syntax/metadata.cpp
        src/syntax/oamd.cpp
        src/syntax/presentation.cpp
        src/syntax/substream.cpp)

target_include_directories(iclforge_ac4dec_objects
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
        "$<INSTALL_INTERFACE:include>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_features(iclforge_ac4dec_objects PUBLIC cxx_std_23)

# The inspector's and the core's usage requirements, compiled with and not
# linked: the libraries above link them (see there). iclforge::ac4 is the one
# BUILD_SHARED_LIBS picks, as iclforge::signing's objects take iclforge::ac3.
# $<BUILD_INTERFACE:...> on all five for the reason src/ac4/CMakeLists.txt
# gives, and because nothing that uses the installed package compiles these
# objects. iclforge::arithmetic is the header-only target that carries
# ac3/internal/scalar_math.hpp, which src/ac4core's dsp/real_functions.hpp
# includes: the float transcendentals that give the same float on every
# platform, called by pcm/aspx.cpp and pcm/companding.cpp through that header
# (src/ac4dec includes from src/ac4core and not from src/arithmetic,
# tools/checks/layering.json; planning/ac4.md, D14a4). Compile-only, so the
# include directory reaches these objects and nothing is linked.
target_link_libraries(iclforge_ac4dec_objects
    PRIVATE
        "$<BUILD_INTERFACE:$<COMPILE_ONLY:iclforge::ac4>>"
        "$<BUILD_INTERFACE:$<COMPILE_ONLY:iclforge::ac4core>>"
        "$<BUILD_INTERFACE:iclforge::arithmetic>"
        "$<BUILD_INTERFACE:ac3::warnings>"
        "$<BUILD_INTERFACE:ac3::coverage>")

# In the minimum-footprint profile: its compile options (no exceptions, no RTTI,
# a section for each function and datum), PUBLIC for a consumer to share, and
# code that is not position-independent - see src/ac4core/CMakeLists.txt.
if(AC3FORGE_MINIMAL_DECODER)
    target_link_libraries(iclforge_ac4dec_objects PUBLIC ac3::minimal_profile)
    set(_ac4dec_pic OFF)
else()
    set(_ac4dec_pic ON)
endif()

set_target_properties(iclforge_ac4dec_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ${_ac4dec_pic}
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

# MSVC's C4251 for the PRIVATE STL-typed members of exported classes - the
# same suppression, for the same reason, as src/ac4/CMakeLists.txt.
target_compile_options(iclforge_ac4dec_objects PUBLIC "$<$<CXX_COMPILER_ID:MSVC>:/wd4251>")

include(GenerateExportHeader)
if(NOT AC3FORGE_MINIMAL_DECODER)
    set_target_properties(iclforge_ac4dec_shared PROPERTIES DEFINE_SYMBOL "AC4DEC_BUILDING_SHARED")
    set(_ac4dec_export_target iclforge_ac4dec_shared)
else()
    # The header is the same either way, as src/ac4/CMakeLists.txt's is.
    set(_ac4dec_export_target iclforge_ac4dec_static)
endif()
generate_export_header(${_ac4dec_export_target}
    BASE_NAME AC4DEC
    EXPORT_MACRO_NAME AC4DEC_EXPORT
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/generated/iclforge/ac4dec/export.hpp"
    DEFINE_NO_DEPRECATED
    STATIC_DEFINE AC4DEC_STATIC_DEFINE)
target_compile_definitions(iclforge_ac4dec_objects PRIVATE AC4DEC_BUILDING_SHARED)
target_compile_definitions(iclforge_ac4dec_static PUBLIC AC4DEC_STATIC_DEFINE)

if(NOT AC3FORGE_MINIMAL_DECODER)
    set_target_properties(iclforge_ac4dec_shared PROPERTIES
        VERSION "${PROJECT_VERSION}"
        SOVERSION "${PROJECT_VERSION}")

    set_target_properties(iclforge_ac4dec_shared PROPERTIES OUTPUT_NAME "iclforge_ac4dec")
endif()
set_target_properties(iclforge_ac4dec_static PROPERTIES OUTPUT_NAME "iclforge_ac4dec_static")

# install(EXPORT ... NAMESPACE ac4::) prefixes the name a target is exported
# under, which is its own name unless this says otherwise: ac4::ac4dec_static
# where the tree says iclforge::ac4dec_static.
set_target_properties(iclforge_ac4dec_objects PROPERTIES EXPORT_NAME "decoder_objects")
set_target_properties(iclforge_ac4dec_static PROPERTIES EXPORT_NAME "decoder_static")
if(NOT AC3FORGE_MINIMAL_DECODER)
    set_target_properties(iclforge_ac4dec_shared PROPERTIES EXPORT_NAME "decoder_shared")
endif()
