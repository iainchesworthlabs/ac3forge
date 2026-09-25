# ---------------------------------------------------------------------------
# ac3forge_c - C API: a stable, minimal C-callable surface over
# ac3::forge's encode/decode core (AC-3, E-AC-3, Atmos/OAMD/JOC), for bindings
# and embedding by callers that cannot or do not want to link C++23. The
# public header (include/ac3forge_c/ac3forge.h) is plain C11 - no C++ type
# ever crosses it, only opaque handles and POD structs - so this is the one
# target in the tree meant to be consumed by a non-C++ toolchain directly.
#
# Built and installed as both a static and a shared library, same OBJECT-
# library-plus-two-thin-wrappers shape as ac3::forge itself - see
# src/forge/CMakeLists.txt's header comment for the full reasoning.
#
# Unlike forge_static/forge_shared, BOTH variants here statically embed
# ac3::forge_static (PRIVATE - forge's own types never appear in this
# target's public header, so nothing about that leaks to a consumer's
# include path) rather than tracking BUILD_SHARED_LIBS. A binding or embedder
# reaching for a C ABI wants exactly one library to load (dlopen/ctypes/
# ffi.dlopen all expect a single self-contained module) - making
# ac3forge_c_shared's own linkage depend on a separately-shipped
# libac3forge.so would defeat that. forge_static's objects are already built
# PIC (src/forge/CMakeLists.txt), so embedding them in a shared library is
# safe on every platform this project targets.
#
# The embedded codec has to stay private to this library, or a program that
# links ac3::forge_shared and this library together can run the C++ API on the
# copy inside this one. Two things keep it private:
#  - linking ac3::forge_c_shared must not put libac3forge_static.a on the
#    consumer's link line, where it beats a libac3forge.so listed after it
#    (forge_c_objects, below);
#  - libac3forge_c.so exports the C API and nothing else (the end of this file).
# Without them ac3tests in the BUILD_SHARED_LIBS=ON pass bound none of its
# ac3:: symbols to libac3forge.so; tools/checks/check_shared_forge_binding.sh
# checks that it does.
# ---------------------------------------------------------------------------
add_library(forge_c_objects OBJECT)
add_library(forge_c_static STATIC)
add_library(forge_c_shared SHARED)

target_link_libraries(forge_c_static PUBLIC forge_c_objects)
target_link_libraries(forge_c_shared PUBLIC forge_c_objects)

add_library(ac3::forge_c_static ALIAS forge_c_static)
add_library(ac3::forge_c_shared ALIAS forge_c_shared)
if(BUILD_SHARED_LIBS)
    add_library(ac3::forge_c ALIAS forge_c_shared)
else()
    add_library(ac3::forge_c ALIAS forge_c_static)
endif()

target_sources(forge_c_objects
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

target_include_directories(forge_c_objects
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
        "$<INSTALL_INTERFACE:include>")

target_compile_features(forge_c_objects PUBLIC cxx_std_23)

# See this file's header comment on why both forge_c_static and forge_c_shared
# embed the static codec rather than tracking BUILD_SHARED_LIBS.
#
# forge_c_objects takes only ac3::forge_static's usage requirements (include
# path, AC3FORGE_STATIC_DEFINE, C++23), through $<COMPILE_ONLY:...>. It must not
# link it: an OBJECT library has no link step to hide a PRIVATE dependency
# behind, so the dependency lands in its INTERFACE_LINK_LIBRARIES, and both
# libraries below link forge_c_objects PUBLIC for its include path. Linked
# PRIVATE here, ac3::forge_static reached every consumer of
# ac3::forge_c_shared, and ac3tests' link line carried libac3forge_static.a
# ahead of libac3forge.so, so the archive supplied the symbols the tests call.
# The two libraries link it themselves instead, where PRIVATE means what it
# says: a shared library keeps it to itself, and a static one passes it on to
# its consumers as a link-only dependency, which they need.
#
# $<BUILD_INTERFACE:...> wraps ac3::warnings and ac3::coverage for the "dev-only,
# must not enter the export graph" reason src/forge/CMakeLists.txt's identical
# forge_objects treatment documents, and the compile-only ac3::forge_static
# because nothing that uses the installed package compiles these objects.
target_link_libraries(forge_c_objects
    PRIVATE
        "$<BUILD_INTERFACE:$<COMPILE_ONLY:ac3::forge_static>>"
        "$<BUILD_INTERFACE:ac3::warnings>"
        "$<BUILD_INTERFACE:ac3::coverage>")
target_link_libraries(forge_c_static PRIVATE ac3::forge_static)
target_link_libraries(forge_c_shared PRIVATE ac3::forge_static)

set_target_properties(forge_c_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

include(GenerateExportHeader)
set_target_properties(forge_c_shared PROPERTIES DEFINE_SYMBOL "AC3FORGEC_BUILDING_SHARED")
generate_export_header(forge_c_shared
    BASE_NAME AC3FORGEC
    EXPORT_MACRO_NAME AC3FORGEC_EXPORT
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/generated/ac3forge_c/export.h"
    DEFINE_NO_DEPRECATED
    STATIC_DEFINE AC3FORGEC_STATIC_DEFINE)
target_compile_definitions(forge_c_objects PRIVATE AC3FORGEC_BUILDING_SHARED)
target_compile_definitions(forge_c_static PUBLIC AC3FORGEC_STATIC_DEFINE)

# generate_export_header() emits a C++-flavoured export.h ("#ifdef __cplusplus
# extern \"C\" {") by default only for the dllexport/dllimport attribute
# syntax itself, which is fine - the macro it defines (AC3FORGEC_EXPORT) is a
# plain object-like #define either way, usable from a .c translation unit
# exactly like from a .cpp one. ac3forge.h's own extern "C" block (guarded on
# __cplusplus) is what actually matters for C linkage, not this header.

# Compile-time version macros (AP1) - a plain configure_file rather than
# GenerateVersion.cmake's restamp-every-build machinery, since PROJECT_VERSION
# only changes on a reconfigure anyway (unlike the git provenance
# ac3/version.hpp also carries, which this header does not need).
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" AC3FORGEC_VERSION_MATCH "${PROJECT_VERSION}")
set(AC3FORGEC_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(AC3FORGEC_VERSION_MINOR "${CMAKE_MATCH_2}")
set(AC3FORGEC_VERSION_PATCH "${CMAKE_MATCH_3}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/ac3forge_c/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/ac3forge_c/version.h"
    @ONLY)

set_target_properties(forge_c_shared PROPERTIES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION}")

set_target_properties(forge_c_shared PROPERTIES OUTPUT_NAME "ac3forge_c")
set_target_properties(forge_c_static PROPERTIES OUTPUT_NAME "ac3forge_c_static")

# libac3forge_c.so exports the C API and nothing else. forge_static's objects are
# compiled once, for forge_shared as well, with AC3FORGE_EXPORT marking the C++
# API for export, so without --exclude-libs the linker exports every one of them
# that the embedded copy pulled in: 346 of the 562 lines of this library's ABI
# allowlist were codec symbols, beside its 216 C functions. A process that then
# loaded libac3forge.so had the C++ API, and libac3forge.so's own calls to
# itself, bound to the copy in this library. --exclude-libs makes the linker
# treat every symbol it takes from an archive as local; src/ac3adm/CMakeLists.txt
# does the same for the same reason. ELF linkers only (GNU ld, gold, lld, mold):
# Mach-O's has no such option and MSVC's link.exe exports what an object marks
# dllexport, so macOS and Windows are left as they were.
if(LINUX)
    target_link_options(forge_c_shared PRIVATE "LINKER:--exclude-libs,ALL")
endif()
