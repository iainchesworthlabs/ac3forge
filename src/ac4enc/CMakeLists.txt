# ---------------------------------------------------------------------------
# ac4::encoder - an AC-4 encoder, ETSI TS 103 190-1 V1.4.1 and TS 103 190-2
# V1.3.1, written from the published texts (planning/ac4.md, phase E1 on).
#
# A peer of src/ac4dec, in the same OBJECT + static + shared arrangement (see
# src/forge/CMakeLists.txt's ac3::forge for why). It links ac4::ac4, since
# its own header describes what it writes with the inspector's Toc and syntax
# trace, and the shared core ac4::core: the forward transform, the windows
# and the codebooks. Its syntax writer is a transcription of the tables of its
# own, separate from the decoder's reader. Installed and exported with the
# inspector, the decoder and the core as ac4::encoder_static and
# ac4::encoder_shared (cmake/InstallLibrary.cmake), the names these aliases
# give the targets in the tree; EXPORT_NAME below makes the installed names
# the same.
# ---------------------------------------------------------------------------
add_library(ac4enc_objects OBJECT)
add_library(ac4enc_static STATIC)
add_library(ac4enc_shared SHARED)

# As src/ac4dec links them, for the reasons it gives: each library links the
# inspector of its own kind PUBLIC and the core PRIVATE, which the static
# library passes on as a link-only dependency and the shared one carries the
# part of; the OBJECT library takes both for compiling only.
target_link_libraries(ac4enc_static PUBLIC ac4enc_objects ac4::ac4_static PRIVATE ac4::core)
target_link_libraries(ac4enc_shared PUBLIC ac4enc_objects ac4::ac4_shared PRIVATE ac4::core)

add_library(ac4::encoder_static ALIAS ac4enc_static)
add_library(ac4::encoder_shared ALIAS ac4enc_shared)
if(BUILD_SHARED_LIBS)
    add_library(ac4::encoder ALIAS ac4enc_shared)
else()
    add_library(ac4::encoder ALIAS ac4enc_static)
endif()

target_sources(ac4enc_objects
    PRIVATE
        src/acpl/acpl_encoder.cpp
        src/acpl/acpl_syntax.cpp
        src/asf/analysis.cpp
        src/asf/coder.cpp
        src/asf/layout.cpp
        src/asf/multichannel.cpp
        src/asf/psycho.cpp
        src/asf/stereo.cpp
        src/aspx/aspx_encoder.cpp
        src/aspx/aspx_syntax.cpp
        src/bit_writer.cpp
        src/encoder.cpp
        src/frame/dialogue.cpp
        src/frame/drc_gains.cpp
        src/frame/frame_writer.cpp
        src/frame/metadata.cpp
        src/frame/sync_frame.cpp
        src/frame/timing.cpp
        src/frame/toc_writer.cpp)

target_include_directories(ac4enc_objects
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
        "$<INSTALL_INTERFACE:include>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_features(ac4enc_objects PUBLIC cxx_std_23)

# The inspector's and the core's usage requirements, compiled with and not
# linked: the libraries above link them. ac4::ac4 is the one
# BUILD_SHARED_LIBS picks, and $<BUILD_INTERFACE:...> is on all four for the
# reason src/ac4dec/CMakeLists.txt gives.
target_link_libraries(ac4enc_objects
    PRIVATE
        "$<BUILD_INTERFACE:$<COMPILE_ONLY:ac4::ac4>>"
        "$<BUILD_INTERFACE:$<COMPILE_ONLY:ac4::core>>"
        "$<BUILD_INTERFACE:ac3::warnings>"
        "$<BUILD_INTERFACE:ac3::coverage>")

set_target_properties(ac4enc_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

# The same C4251 suppression, for the same reason, as src/ac4dec.
target_compile_options(ac4enc_objects PUBLIC "$<$<CXX_COMPILER_ID:MSVC>:/wd4251>")

include(GenerateExportHeader)
set_target_properties(ac4enc_shared PROPERTIES DEFINE_SYMBOL "AC4ENC_BUILDING_SHARED")
generate_export_header(ac4enc_shared
    BASE_NAME AC4ENC
    EXPORT_MACRO_NAME AC4ENC_EXPORT
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/generated/ac4enc/export.hpp"
    DEFINE_NO_DEPRECATED
    STATIC_DEFINE AC4ENC_STATIC_DEFINE)
target_compile_definitions(ac4enc_objects PRIVATE AC4ENC_BUILDING_SHARED)
target_compile_definitions(ac4enc_static PUBLIC AC4ENC_STATIC_DEFINE)

set_target_properties(ac4enc_shared PROPERTIES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION}")

set_target_properties(ac4enc_shared PROPERTIES OUTPUT_NAME "ac4enc")
set_target_properties(ac4enc_static PROPERTIES OUTPUT_NAME "ac4enc_static")

# The names install(EXPORT ... NAMESPACE ac4::) gives the targets, as the
# tree's aliases have them: ac4::encoder_static, not ac4::ac4enc_static.
set_target_properties(ac4enc_objects PROPERTIES EXPORT_NAME "encoder_objects")
set_target_properties(ac4enc_static PROPERTIES EXPORT_NAME "encoder_static")
set_target_properties(ac4enc_shared PROPERTIES EXPORT_NAME "encoder_shared")
