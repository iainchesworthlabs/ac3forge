# ---------------------------------------------------------------------------
# iclforge_c - C API: a stable, minimal C-callable surface over
# iclforge::ac3's encode/decode core (AC-3, E-AC-3, Atmos/OAMD/JOC), for bindings
# and embedding by callers that cannot or do not want to link C++23. The
# public header (include/iclforge_c/iclforge.h) is plain C11 - no C++ type
# ever crosses it, only opaque handles and POD structs - so this is the one
# target in the tree meant to be consumed by a non-C++ toolchain directly.
#
# Built and installed as both a static and a shared library, same OBJECT-
# library-plus-two-thin-wrappers shape as iclforge::ac3 itself - see
# src/ac3/CMakeLists.txt's header comment for the full reasoning.
#
# Unlike iclforge_ac3_static/iclforge_ac3_shared, BOTH variants here statically embed
# iclforge::ac3_static (PRIVATE - forge's own types never appear in this
# target's public header, so nothing about that leaks to a consumer's
# include path) rather than tracking BUILD_SHARED_LIBS. A binding or embedder
# reaching for a C ABI wants exactly one library to load (dlopen/ctypes/
# ffi.dlopen all expect a single self-contained module) - making
# iclforge_c_shared's own linkage depend on a separately-shipped
# libiclforge_ac3.so would defeat that. iclforge_ac3_static's objects are already built
# PIC (src/ac3/CMakeLists.txt), so embedding them in a shared library is
# safe on every platform this project targets.
#
# The embedded codec has to stay private to this library, or a program that
# links iclforge::ac3_shared and this library together can run the C++ API on the
# copy inside this one. Two things keep it private:
#  - linking iclforge::c_shared must not put libiclforge_ac3_static.a on the
#    consumer's link line, where it beats a libiclforge_ac3.so listed after it
#    (iclforge_capi_objects, below);
#  - libiclforge_c.so exports the C API and nothing else (the end of this file).
# Without them iclforge-tests in the BUILD_SHARED_LIBS=ON pass bound none of its
# ac3:: symbols to libiclforge_ac3.so; tools/checks/check_shared_forge_binding.sh
# checks that it does.
# ---------------------------------------------------------------------------
add_library(iclforge_capi_objects OBJECT)
add_library(iclforge_capi_static STATIC)
add_library(iclforge_capi_shared SHARED)

target_link_libraries(iclforge_capi_static PUBLIC iclforge_capi_objects)
target_link_libraries(iclforge_capi_shared PUBLIC iclforge_capi_objects)

add_library(iclforge::c_static ALIAS iclforge_capi_static)
add_library(iclforge::c_shared ALIAS iclforge_capi_shared)
if(BUILD_SHARED_LIBS)
    add_library(iclforge::c ALIAS iclforge_capi_shared)
else()
    add_library(iclforge::c ALIAS iclforge_capi_static)
endif()

target_sources(iclforge_capi_objects
    PRIVATE
        src/atmos.cpp
        src/common.cpp
        src/decoder.cpp
        src/eac3.cpp
        src/eac3_encoder.cpp
        src/encoder.cpp
        src/levels.cpp
        src/loudness.cpp
        src/qc.cpp
        src/scan.cpp)

# AC-4 (decision 21, planning/ac4.md phase I4): embedded the same way
# iclforge::ac3_static is - see the target_link_libraries() calls below - so a
# caller loading this one library gets AC-4 too. iclforge.h's AC-4 section is
# declared either way (that header's own comment: this codebase selects a
# variant by CMake, never by preprocessor conditional - see
# tools/checks/check_platform_macros.ps1), so the choice below is which
# translation unit answers those 76 functions, not whether they exist:
# ac4.cpp/ac4_encoder.cpp with ICLFORGE_BUILD_AC4 on, ac4_absent.cpp (which
# names no ac4:: C++ type, so it needs neither the AC-4 headers nor the
# static libraries linked below) with it off.
if(ICLFORGE_BUILD_AC4)
    target_sources(iclforge_capi_objects
        PRIVATE
            src/ac4.cpp
            src/ac4_encoder.cpp)
else()
    target_sources(iclforge_capi_objects PRIVATE src/ac4_absent.cpp)
endif()

target_include_directories(iclforge_capi_objects
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
        "$<INSTALL_INTERFACE:include>")

target_compile_features(iclforge_capi_objects PUBLIC cxx_std_23)

# See this file's header comment on why both iclforge_capi_static and iclforge_capi_shared
# embed the static codec rather than tracking BUILD_SHARED_LIBS.
#
# iclforge_capi_objects takes only iclforge::ac3_static's usage requirements (include
# path, ICLFORGE_AC3_STATIC_DEFINE, C++23), through $<COMPILE_ONLY:...>. It must not
# link it: an OBJECT library has no link step to hide a PRIVATE dependency
# behind, so the dependency lands in its INTERFACE_LINK_LIBRARIES, and both
# libraries below link iclforge_capi_objects PUBLIC for its include path. Linked
# PRIVATE here, iclforge::ac3_static reached every consumer of
# iclforge::c_shared, and iclforge-tests' link line carried libiclforge_ac3_static.a
# ahead of libiclforge_ac3.so, so the archive supplied the symbols the tests call.
# The two libraries link it themselves instead, where PRIVATE means what it
# says: a shared library keeps it to itself, and a static one passes it on to
# its consumers as a link-only dependency, which they need.
#
# $<BUILD_INTERFACE:...> wraps iclforge::warnings and iclforge::coverage for the "dev-only,
# must not enter the export graph" reason src/ac3/CMakeLists.txt's identical
# iclforge_ac3_objects treatment documents, and the compile-only iclforge::ac3_static
# because nothing that uses the installed package compiles these objects.
target_link_libraries(iclforge_capi_objects
    PRIVATE
        "$<BUILD_INTERFACE:$<COMPILE_ONLY:iclforge::ac3_static>>"
        "$<BUILD_INTERFACE:iclforge::warnings>"
        "$<BUILD_INTERFACE:iclforge::coverage>")
target_link_libraries(iclforge_capi_static PRIVATE iclforge::ac3_static)
target_link_libraries(iclforge_capi_shared PRIVATE iclforge::ac3_static)

# Same treatment for AC-4's static libraries (decision 21): iclforge_capi_objects
# takes only their usage requirements (compile-only), and each real library
# links the static AC-4 archives itself, PRIVATE, for the same "an OBJECT
# library has no link step to hide a PRIVATE dependency behind" reason as
# iclforge::ac3_static above. iclforge::ac4dec_static PUBLIC-links iclforge::ac4_static
# and PRIVATE-links iclforge::ac4core itself (src/ac4dec/CMakeLists.txt), and CMake
# carries a static library's dependencies to its own consumers as
# link-only, so naming the decoder and encoder archives here is enough to
# pull in the inspector and the DSP core transitively - apps/cli/CMakeLists.txt
# relies on the same transitive closure for iclforge::ac4dec/iclforge::ac4enc.
if(ICLFORGE_BUILD_AC4)
    target_link_libraries(iclforge_capi_objects
        PRIVATE
            "$<BUILD_INTERFACE:$<COMPILE_ONLY:iclforge::ac4dec_static>>"
            "$<BUILD_INTERFACE:$<COMPILE_ONLY:iclforge::ac4enc_static>>")
    target_link_libraries(iclforge_capi_static PRIVATE iclforge::ac4dec_static iclforge::ac4enc_static)
    target_link_libraries(iclforge_capi_shared PRIVATE iclforge::ac4dec_static iclforge::ac4enc_static)
endif()

set_target_properties(iclforge_capi_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

include(GenerateExportHeader)
set_target_properties(iclforge_capi_shared PROPERTIES DEFINE_SYMBOL "ICLFORGE_C_BUILDING_SHARED")
generate_export_header(iclforge_capi_shared
    BASE_NAME ICLFORGE_C
    EXPORT_MACRO_NAME ICLFORGE_C_EXPORT
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/generated/iclforge_c/export.h"
    DEFINE_NO_DEPRECATED
    STATIC_DEFINE ICLFORGE_C_STATIC_DEFINE)
target_compile_definitions(iclforge_capi_objects PRIVATE ICLFORGE_C_BUILDING_SHARED)
target_compile_definitions(iclforge_capi_static PUBLIC ICLFORGE_C_STATIC_DEFINE)

# generate_export_header() emits a C++-flavoured export.h ("#ifdef __cplusplus
# extern \"C\" {") by default only for the dllexport/dllimport attribute
# syntax itself, which is fine - the macro it defines (ICLFORGE_C_EXPORT) is a
# plain object-like #define either way, usable from a .c translation unit
# exactly like from a .cpp one. iclforge.h's own extern "C" block (guarded on
# __cplusplus) is what actually matters for C linkage, not this header.

# Compile-time version macros (AP1) - a plain configure_file rather than
# GenerateVersion.cmake's restamp-every-build machinery, since PROJECT_VERSION
# only changes on a reconfigure anyway (unlike the git provenance
# iclforge/ac3/version.hpp also carries, which this header does not need).
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" ICLFORGE_C_VERSION_MATCH "${PROJECT_VERSION}")
set(ICLFORGE_C_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(ICLFORGE_C_VERSION_MINOR "${CMAKE_MATCH_2}")
set(ICLFORGE_C_VERSION_PATCH "${CMAKE_MATCH_3}")
# Feeds version.h.in's #cmakedefine ICLFORGE_HAS_AC4 - see that file's comment.
set(ICLFORGE_HAS_AC4 ${ICLFORGE_BUILD_AC4})
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/iclforge_c/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/iclforge_c/version.h"
    @ONLY)

set_target_properties(iclforge_capi_shared PROPERTIES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION}")

set_target_properties(iclforge_capi_shared PROPERTIES OUTPUT_NAME "iclforge_c")
set_target_properties(iclforge_capi_static PROPERTIES OUTPUT_NAME "iclforge_c_static")

# libiclforge_c.so exports the C API and nothing else. iclforge_ac3_static's objects are
# compiled once, for iclforge_ac3_shared as well, with ICLFORGE_AC3_EXPORT marking the C++
# API for export, so without --exclude-libs the linker exports every one of them
# that the embedded copy pulled in: 346 of the 562 lines of this library's ABI
# allowlist were codec symbols, beside its 216 C functions. A process that then
# loaded libiclforge_ac3.so had the C++ API, and libiclforge_ac3.so's own calls to
# itself, bound to the copy in this library. --exclude-libs makes the linker
# treat every symbol it takes from an archive as local; src/adm/CMakeLists.txt
# does the same for the same reason. ELF linkers only (GNU ld, gold, lld, mold):
# Mach-O's has no such option and MSVC's link.exe exports what an object marks
# dllexport, so macOS and Windows are left as they were.
if(LINUX)
    target_link_options(iclforge_capi_shared PRIVATE "LINKER:--exclude-libs,ALL")
endif()
