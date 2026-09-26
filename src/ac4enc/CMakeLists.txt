# ---------------------------------------------------------------------------
# ac4::encoder - an AC-4 encoder, ETSI TS 103 190-1 V1.4.1 and TS 103 190-2
# V1.3.1, written from the published texts (planning/ac4.md, phase E1 on).
#
# A peer of src/ac4dec, in the same OBJECT + static + shared arrangement (see
# src/forge/CMakeLists.txt's ac3::forge for why). It links ac4::ac4 PUBLIC,
# since its own header describes what it writes with the inspector's Toc, and
# the shared core ac4::core PRIVATE: the forward transform, the windows and
# the codebooks. Its syntax writer is a transcription of the tables of its
# own, separate from the decoder's reader.
# ---------------------------------------------------------------------------
add_library(ac4enc_objects OBJECT)
add_library(ac4enc_static STATIC)
add_library(ac4enc_shared SHARED)

target_link_libraries(ac4enc_static PUBLIC ac4enc_objects)
target_link_libraries(ac4enc_shared PUBLIC ac4enc_objects)

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
        src/frame/timing.cpp)

target_include_directories(ac4enc_objects
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
        "$<INSTALL_INTERFACE:include>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_features(ac4enc_objects PUBLIC cxx_std_23)

# ac4::ac4 is PUBLIC: the encoder's header names the inspector's Toc and the
# shared syntax trace. ac4::core is PRIVATE, as it is to the decoder.
target_link_libraries(ac4enc_objects
    PUBLIC
        ac4::ac4
    PRIVATE
        ac4::core
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
