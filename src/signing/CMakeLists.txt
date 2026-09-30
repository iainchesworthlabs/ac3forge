# ---------------------------------------------------------------------------
# iclforge::signing - the EMDF Atmos object signer.
#
# Clean-room and dependency-free (SHA-256/HMAC are implemented from FIPS 180-4/
# RFC 2104, not pulled from OpenSSL - the same "no third-party codec deps" rule
# vcpkg.json states). Kept a SEPARATE target from iclforge::ac3, not folded into
# it, so the codec still links nothing to encode/decode: signing is a strictly
# optional step a front end applies to finished frames, and iclforge::ac3 has no
# dependency on it. It reuses the codec's PARSING primitives (BitReader,
# decode_exponents, compute_bit_allocation, the spx helpers) via iclforge::ac3 but
# adds nothing to them.
#
# No embedded key: the signer takes a SigningKey the operator supplies at
# runtime (see signing_key.hpp). This is why the target is committed and built
# by default, unlike the old gitignored quarantine overlay it replaces - there
# is no secret in it to keep out of the tree.
#
# Built and installed as BOTH a static and a shared library, the same OBJECT-
# library-plus-two-thin-wrappers shape iclforge::ac3 itself uses (see
# src/ac3/CMakeLists.txt's header comment for the full reasoning) - unlike
# iclforge::c (src/capi/CMakeLists.txt), this is an ordinary C++ dependent
# rather than a self-contained C ABI, so iclforge_signing_static links iclforge::ac3_static
# and iclforge_signing_shared links iclforge::ac3_shared, exactly mirroring how
# iclforge_ac3_static/forge_shared each wrap iclforge_ac3_objects, rather than always
# embedding one specific variant regardless of BUILD_SHARED_LIBS the way capi
# does. Raw target names iclforge_signing_objects/signing_static/signing_shared, not
# ac3_signing_*: same stutter-avoidance reasoning as iclforge_ac3_objects/forge_static/
# iclforge_ac3_shared's own header comment (install(EXPORT ... NAMESPACE ac3::)
# prepends the namespace to whatever the target is actually named).
# ---------------------------------------------------------------------------
add_library(iclforge_signing_objects OBJECT)
add_library(iclforge_signing_static STATIC)
add_library(iclforge_signing_shared SHARED)

target_link_libraries(iclforge_signing_static PUBLIC iclforge_signing_objects iclforge::ac3_static)
target_link_libraries(iclforge_signing_shared PUBLIC iclforge_signing_objects iclforge::ac3_shared)

add_library(iclforge::signing_static ALIAS iclforge_signing_static)
add_library(iclforge::signing_shared ALIAS iclforge_signing_shared)
if(BUILD_SHARED_LIBS)
    add_library(iclforge::signing ALIAS iclforge_signing_shared)
else()
    add_library(iclforge::signing ALIAS iclforge_signing_static)
endif()

target_sources(iclforge_signing_objects
    PRIVATE
        src/sha256.cpp
        src/hmac_sha256.cpp
        src/signing_key.cpp
        src/emdf_atmos_signer.cpp)

target_include_directories(iclforge_signing_objects
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
        "$<INSTALL_INTERFACE:include>"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_features(iclforge_signing_objects PUBLIC cxx_std_23)

# iclforge::ac3 (bare, BUILD_SHARED_LIBS-selected) for the parsing primitives, PRIVATE and compile-time
# only - iclforge_signing_static/signing_shared above each PUBLICLY link their own matching iclforge_ac3_static/
# iclforge_ac3_shared variant instead, the same "propagation lives on the wrapper, not the shared object
# library" split iclforge_ac3_objects/capi_objects's own PRIVATE deps already use. iclforge::warnings/
# iclforge::coverage wrapped in $<BUILD_INTERFACE:...> for the same dev-only, never-exported reason
# iclforge_ac3_objects's identical treatment documents.
target_link_libraries(iclforge_signing_objects
    PRIVATE
        iclforge::ac3
        "$<BUILD_INTERFACE:iclforge::warnings>"
        "$<BUILD_INTERFACE:iclforge::coverage>")

# --- Static+shared plumbing -------------------------------------------------
# Same PIC/hidden-visibility/ICLFORGE_SIGNING_EXPORT shape as iclforge_ac3_objects's own
# identical block - see its header comment for the full reasoning.
set_target_properties(iclforge_signing_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

target_compile_options(iclforge_signing_objects PUBLIC "$<$<CXX_COMPILER_ID:MSVC>:/wd4251>")

include(GenerateExportHeader)
set_target_properties(iclforge_signing_shared PROPERTIES DEFINE_SYMBOL "ICLFORGE_SIGNING_BUILDING_SHARED")
generate_export_header(iclforge_signing_shared
    BASE_NAME ICLFORGE_SIGNING
    EXPORT_MACRO_NAME ICLFORGE_SIGNING_EXPORT
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/generated/iclforge/signing/export.hpp"
    DEFINE_NO_DEPRECATED
    STATIC_DEFINE ICLFORGE_SIGNING_STATIC_DEFINE)
target_compile_definitions(iclforge_signing_objects PRIVATE ICLFORGE_SIGNING_BUILDING_SHARED)
target_compile_definitions(iclforge_signing_static PUBLIC ICLFORGE_SIGNING_STATIC_DEFINE)

set_target_properties(iclforge_signing_shared PROPERTIES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION}")

# Same Windows same-directory-clash avoidance as iclforge_ac3_shared/forge_static's own OUTPUT_NAME split.
set_target_properties(iclforge_signing_shared PROPERTIES OUTPUT_NAME "iclforge_signing")
set_target_properties(iclforge_signing_static PROPERTIES OUTPUT_NAME "iclforge_signing_static")

# MSVC flags plain getenv() (signing_key.cpp's env-var fallbacks) as C4996
# "unsafe". The call is POSIX and portable - GCC/Clang raise nothing for it
# under -Wall -Wextra - and the suggested _dupenv_s is a Microsoft CRT
# extension with no NDK/POSIX equivalent, so "fix the code" is not an option
# that stays cross-platform. Scoped to this one compiler's opinion of a
# standard C function, the same way src/ac3 handles its own MSVC-only
# suppressions (see cmake/CompilerWarnings.cmake).
if(MSVC)
    target_compile_options(iclforge_signing_objects PRIVATE /wd4996)
endif()
