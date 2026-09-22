# ---------------------------------------------------------------------------
# ac3adm::ac3adm - a standalone BW64/RF64 + Audio Definition Model (ADM)
# parser, built on top of two vendored third-party libraries: libbw64 (the
# BW64/RF64 container layer) and libadm (the ADM XML object model), both
# from github.com/ebu, Apache-2.0, maintained by the same BBC/IRT team that
# wrote the underlying ITU-R Recommendations (BS.2088-1, BS.2076-2)
# themselves.
#
# This is the one library in ac3forge that is NOT zero-third-party-dependency
# (docs/library/index.md's stated principle for everywhere else): libadm
# needs several Boost header-only libraries (Optional, Variant, Range,
# Iterator, Functional, Format, Integer, Rational - see vcpkg.json's "adm"
# feature comment for how this exact list was confirmed, rather than guessed,
# against libadm's own source). That dependency is DELIBERATELY opt-in, not
# default-on the way matroska/mp4/mpegts are - AC3FORGE_BUILD_ADM defaults
# OFF (see root CMakeLists.txt's own option()), and even turning it ON
# additionally needs -DVCPKG_MANIFEST_FEATURES=adm (see vcpkg.json's "adm"
# feature) to resolve the Boost packages - the guard immediately below fails
# clearly if that feature was not selected, rather than surfacing a bare
# "Boost not found" from deep inside libadm's own CMakeLists.txt.
#
# libbw64 is header-only and has no dependency of its own - it is vendored
# purely so this module does not have to hand-roll BW64/RF64 chunk-walking
# (an earlier attempt at exactly that inside this same module is what
# prompted using the EBU's own reference implementations instead).
#
# Neither library's own types appear in this module's public headers
# (ac3adm/ac3adm.hpp, ac3adm/model.hpp) - see model.hpp's own header comment
# for why (libadm's own C++ namespace is `adm`, which is also why this
# module's namespace is `ac3adm` and not `adm`).
#
# NOT wired into the Android/Shield CMake tree - matroska/mp4/mpegts stay out
# of that tree too (see apps/android's own notes - not an NDK build-gap issue
# any more, now that the project uses {fmt} rather than std::format; it's
# purely because the app has no use for them), and this module adds an even
# less NDK-friendly dependency (Boost) on top of a capability (ADM ingest)
# the Android app has no use for at all either.
#
# Installed/exported via find_package(ac3forge) (cmake/InstallLibrary.cmake's AC3FORGE_BUILD_ADM
# block) as ac3adm::ac3adm_shared ONLY - never ac3adm::ac3adm_static, regardless of
# AC3FORGE_INSTALL_BOTH_LINKAGES. bw64/adm are third-party FetchContent targets this project
# doesn't install or export in their own right (re-exporting libbw64/libadm as first-class
# packages of their own - including libadm's Boost header dependency - was judged out of scope
# for this module). A STATIC ac3adm still has genuinely unresolved symbol references into adm's
# object code that only a downstream consumer's own final link step would resolve, which would
# require shipping libadm/libbw64 too; a SHARED ac3adm.so is self-contained instead, since adm is
# forced STATIC and embedded into it at ac3adm_shared's own build step (see the
# FetchContent_MakeAvailable(libadm) comment above). See target_link_libraries(ac3adm_objects...)
# below for how bw64/adm are kept out of the exported package's Targets.cmake entirely.
# ---------------------------------------------------------------------------

# Only checked when this module is actually being configured - AC3FORGE_BUILD_ADM=OFF
# (the default) never touches find_package(Boost) at all.
find_package(Boost 1.57 QUIET)
if(NOT Boost_FOUND)
    message(FATAL_ERROR
        "AC3FORGE_BUILD_ADM=ON needs libadm's Boost header dependencies (Optional, "
        "Variant, Range, Iterator, Functional, Format, Integer, Rational), which were not "
        "found. If using vcpkg, reconfigure with -DVCPKG_MANIFEST_FEATURES=adm (see "
        "vcpkg.json's \"adm\" feature) so those packages resolve, or build with "
        "-DAC3FORGE_BUILD_ADM=OFF.")
endif()

include(FetchContent)

# libbw64 is header-only, and is deliberately NOT add_subdirectory()'d via
# FetchContent_MakeAvailable the way libadm is just below - its own src/CMakeLists.txt (as of
# 0.10.0) has a real bug that breaks that: its configure_package_config_file() call resolves
# "config/libbw64Config.cmake.in" against CMAKE_SOURCE_DIR (this whole build's top-level source
# dir - i.e. ac3forge's own repo root, once libbw64 is pulled in as a subdirectory of it) instead
# of PROJECT_SOURCE_DIR/CMAKE_CURRENT_SOURCE_DIR (libbw64's own tree), so it fails outright with
# a missing-file configure error ("File .../ac3forge/config/libbw64Config.cmake.in does not
# exist") the moment it is add_subdirectory()'d into a larger project rather than configured
# stand-alone - confirmed by actually trying it, not assumed. (Its EXAMPLES/UNIT_TESTS options
# also turned out to have no "BW64_" prefix, unlike what their own generic naming style might
# suggest - also confirmed by reading its CMakeLists.txt directly rather than guessing.)
#
# The workaround: populate the source only (FetchContent_Populate, not MakeAvailable - this
# fetches the repository without running its CMakeLists.txt at all) and define this module's own
# INTERFACE target over its include/ directory directly, which is all an INTERFACE library
# amounts to anyway (see its own src/CMakeLists.txt's target_include_directories call). The
# generated bw64/version.hpp its top-level CMakeLists.txt would otherwise produce is skipped
# entirely - grepping every public header under include/bw64/ confirms none of them include it;
# it exists only for libbw64's own examples/tests, neither of which this module builds.
#
# The fetch details are passed straight to FetchContent_Populate() below rather than through a
# separate FetchContent_Declare() + bare FetchContent_Populate(libbw64) pair: that two-call form
# (population "from previously declared details") is exactly what CMake 3.30+'s CMP0169 policy
# deprecates - and this CMake (4.4) defaults that policy to NEW, making it a hard configure error
# rather than a warning, confirmed by actually hitting it. Passing the GIT_REPOSITORY/GIT_TAG
# arguments directly here, the same way FetchContent_MakeAvailable does internally, is the
# CMP0169-unaffected form (see FetchContent.cmake's own FetchContent_Populate: it only warns/
# errors when called with nothing but a name).
FetchContent_Populate(libbw64
    QUIET
    GIT_REPOSITORY https://github.com/ebu/libbw64.git
    GIT_TAG "0.10.0"
    GIT_SHALLOW TRUE)

add_library(bw64 INTERFACE)
# SYSTEM (via the include-directories SYSTEM keyword, FetchContent's own SYSTEM option is not
# available on the Populate-only path used above) keeps libbw64's headers out of reach of
# ac3::warnings' -Werror, matching every other vendored dependency in this build.
target_include_directories(bw64 SYSTEM INTERFACE "${libbw64_SOURCE_DIR}/include")

# libadm has no such bug - its own CMakeLists.txt genuinely honors ADM_UNIT_TESTS/ADM_EXAMPLES/
# ADM_PACKAGE_AND_INSTALL (confirmed the same way, by reading it rather than guessing), so it is
# add_subdirectory()'d normally via FetchContent_MakeAvailable below. Also no vcpkg port of its
# own (confirmed via `vcpkg search`) - its Boost dependency is resolved separately, above, via
# vcpkg's "adm" manifest feature.
set(ADM_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(ADM_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ADM_PACKAGE_AND_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libadm
    GIT_REPOSITORY https://github.com/ebu/libadm.git
    GIT_TAG "0.14.0"
    GIT_SHALLOW TRUE
    SYSTEM
    EXCLUDE_FROM_ALL)

# Populates the target `adm` (STATIC or SHARED, following BUILD_SHARED_LIBS like every other
# target in this build) - see its own src/CMakeLists.txt for why that is the exact name, not
# `libadm`. Forced STATIC here regardless of this project's own ambient BUILD_SHARED_LIBS
# (save/restore around just this one call): ac3adm_shared (below) is the only installed/exported
# variant of this module (see cmake/InstallLibrary.cmake's AC3FORGE_BUILD_ADM block), and it has
# to be self-contained - a downstream consumer of the installed package never gets a libadm.so of
# its own to link against (this project never install()s adm/bw64, see the target_link_libraries
# comment below). If adm followed BUILD_SHARED_LIBS=ON here, ac3adm_shared would gain a genuine
# runtime dependency on a libadm.so this package doesn't ship - broken for any consumer, not just
# an inconvenience. Forcing it static means ac3adm_shared always embeds it, regardless of how the
# rest of this project was configured.
#
# A CACHE FORCE, not a plain set(BUILD_SHARED_LIBS OFF) normal-variable override - confirmed by
# actually building this with -DBUILD_SHARED_LIBS=ON that the plain-variable form does NOT work:
# libadm's own CMakeLists.txt declares option(BUILD_SHARED_LIBS ...) itself under CMP0077 OLD
# (it predates that policy and never opts into NEW), and OLD-policy option() creates/uses its own
# CACHE entry regardless of any pre-existing normal variable of the same name - it doesn't
# "clear" the normal variable so much as ignore it entirely, silently building adm SHARED again.
# Pre-seeding the CACHE entry (same FORCE pattern already used for ADM_UNIT_TESTS/ADM_EXAMPLES/
# ADM_PACKAGE_AND_INSTALL just above) works because option() only initializes a CACHE entry that
# doesn't already exist - one that's already present, FORCEd or not, is left alone.
set(_ac3adm_saved_build_shared_libs "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(libadm)
set(BUILD_SHARED_LIBS "${_ac3adm_saved_build_shared_libs}" CACHE BOOL "" FORCE)
unset(_ac3adm_saved_build_shared_libs)

# ---------------------------------------------------------------------------
# ac3adm's own targets - built and installed as both a static and a shared
# library, same OBJECT-library-plus-two-thin-wrappers shape as every other
# component here; see src/forge/CMakeLists.txt's ac3::forge for the full
# explanation of why.
# ---------------------------------------------------------------------------
add_library(ac3adm_objects OBJECT)
add_library(ac3adm_static STATIC)
add_library(ac3adm_shared SHARED)

target_link_libraries(ac3adm_static PUBLIC ac3adm_objects)
target_link_libraries(ac3adm_shared PUBLIC ac3adm_objects)

add_library(ac3adm::ac3adm_static ALIAS ac3adm_static)
add_library(ac3adm::ac3adm_shared ALIAS ac3adm_shared)
if(BUILD_SHARED_LIBS)
    add_library(ac3adm::ac3adm ALIAS ac3adm_shared)
else()
    add_library(ac3adm::ac3adm ALIAS ac3adm_static)
endif()

target_sources(ac3adm_objects
    PRIVATE
        src/adm_model.cpp
        src/adm.cpp
        src/float_pcm_bw64.cpp)

target_include_directories(ac3adm_objects
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
        "$<INSTALL_INTERFACE:include>")

target_compile_features(ac3adm_objects PUBLIC cxx_std_23)

# $<BUILD_INTERFACE:...> wrapping ac3::warnings - see src/forge/CMakeLists.txt's identical
# forge_objects treatment for why a plain PRIVATE link isn't enough for an OBJECT
# library that's part of an install(EXPORT) set. bw64/adm are PRIVATE too: neither
# library's types cross ac3adm's own public API (see this file's header comment), so
# nothing downstream needs to link them transitively.
# -Wnull-dereference false positive, GNU only: adm.cpp's read_pcm() calls into libbw64's inlined
# chunk accessors (bw64::DataSize64Chunk::size(), etc. - plain member-variable getters, nothing
# genuinely null-unsafe), which GCC 15.2.0 at -O3 (Release) traces through several inlining levels
# into libstdc++'s std::map/set internals (bits/stl_tree.h) and misattributes an "impossible" path
# its own optimizer introduced as a real null dereference - confirmed by actually building this
# target at -O3 (config-linux-gcc, not just the -debug preset development normally used, which
# does not optimize enough to trigger it). cmake/CompilerWarnings.cmake already documents this
# exact false-positive category for GCC < 15 against libstdc++ internals reached from this
# project's own code (see its own comment, and gcc.gnu.org/bugzilla PR 108757 and neighbours) but
# stops the guard there because the CI-pinned GCC 15 did not exhibit it - true until this module's
# own interaction with libbw64 started reaching the same internals on that exact compiler. Scoped
# to this target rather than widening that project-wide guard: nothing else in this project has
# hit it on GCC 15, so silencing it everywhere would risk hiding a genuine one elsewhere.
#
# A separate INTERFACE library linked AFTER ac3::warnings, not a plain target_compile_options()
# call on ac3adm_objects itself - confirmed by inspecting the generated compile_commands.json that
# a target's own COMPILE_OPTIONS always land BEFORE the INTERFACE_COMPILE_OPTIONS propagated from
# its linked libraries on the actual command line, regardless of which order the CMakeLists.txt
# calls target_compile_options()/target_link_libraries() in - so a plain target_compile_options()
# call here would be overridden right back by ac3::warnings' own -Wnull-dereference. Putting the
# override in its own link dependency, listed after ac3::warnings, makes it win the last-flag-wins
# rule instead.
add_library(ac3adm_suppress_gcc15_null_deref INTERFACE)
target_compile_options(ac3adm_suppress_gcc15_null_deref INTERFACE
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-null-dereference>")

# bw64/adm/ac3adm_suppress_gcc15_null_deref are all wrapped in $<BUILD_INTERFACE:...>, not just
# ac3::warnings/ac3::coverage: ac3adm_objects is an OBJECT library, which has no link step of its
# own to hide a PRIVATE dependency behind - a PRIVATE link on an OBJECT library still leaks into
# that library's own INTERFACE_LINK_LIBRARIES (see src/capi/CMakeLists.txt's forge_c_objects/
# ac3::forge_static for the same leak forcing a real export-set fix there). Since ac3adm_objects
# is itself part of the admTargets export set (cmake/InstallLibrary.cmake), an unwrapped bw64/adm
# would make install(EXPORT admTargets) fail outright ("requires target ... that is not in any
# export set") - neither is installed/exported by this project (see this file's header comment).
# BUILD_INTERFACE keeps them resolved for the in-tree build (this generator expression still
# evaluates true there) while stripping them from what gets written into the exported
# admTargets.cmake - which is safe here specifically because the only installed variant,
# ac3adm_shared, already has adm's (forced-STATIC, see above) object code embedded by the time
# it's built; a downstream consumer never needs bw64/adm on their own link line.
target_link_libraries(ac3adm_objects
    PRIVATE
        "$<BUILD_INTERFACE:ac3::warnings>"
        "$<BUILD_INTERFACE:ac3::coverage>"
        "$<BUILD_INTERFACE:bw64>"
        "$<BUILD_INTERFACE:adm>"
        "$<BUILD_INTERFACE:ac3adm_suppress_gcc15_null_deref>")

set_target_properties(ac3adm_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

include(GenerateExportHeader)
set_target_properties(ac3adm_shared PROPERTIES DEFINE_SYMBOL "AC3ADM_BUILDING_SHARED")
generate_export_header(ac3adm_shared
    BASE_NAME AC3ADM
    EXPORT_MACRO_NAME AC3ADM_EXPORT
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/generated/ac3adm/export.hpp"
    DEFINE_NO_DEPRECATED
    STATIC_DEFINE AC3ADM_STATIC_DEFINE)
target_compile_definitions(ac3adm_objects PRIVATE AC3ADM_BUILDING_SHARED)
target_compile_definitions(ac3adm_static PUBLIC AC3ADM_STATIC_DEFINE)

set_target_properties(ac3adm_shared PROPERTIES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION}")

set_target_properties(ac3adm_shared PROPERTIES OUTPUT_NAME "ac3adm")
set_target_properties(ac3adm_static PROPERTIES OUTPUT_NAME "ac3adm_static")

# --exclude-libs,ALL: confirmed empirically necessary, not a defensive guess - without this,
# ac3adm_shared's dynamic symbol table carries ~16,800 exported symbols (a real linux-llvm-shared
# build measured it), nearly all adm::/boost:: template instantiations (VTTs, boost::optional<
# adm::X>::operator=, adm::xml::detail::toString<...>, ...) reachable from ac3adm's own .cpp
# files including adm.hpp/model.hpp. CXX_VISIBILITY_PRESET hidden + VISIBILITY_INLINES_HIDDEN
# (set on ac3adm_objects above) do NOT suppress these: libadm's own class declarations carry an
# explicit default-visibility attribute of their own (for libadm's own shared-library builds),
# which - being on the class declaration itself - wins over this project's -fvisibility=hidden
# default for every instantiation of that class anywhere, including ones compiled into OUR
# translation units. --exclude-libs is the standard fix for exactly this "statically embed a
# third-party archive into a shared library without re-exporting its (or its headers') symbols"
# shape: it forces the linker to treat every symbol pulled in from a STATIC archive (adm.a,
# already forced STATIC above) as local, regardless of what visibility that symbol's own
# declaration requested - independent of, and layered on top of, the ordinary
# CXX_VISIBILITY_PRESET default. GNU ld/gold/lld all support it; MSVC's link.exe (and clang-cl's
# lld-link, which parses MSVC-style arguments) do not - guarded the same way this project's other
# frontend-variant-sensitive flags are (see the root CMakeLists.txt's AC3_FP_CONTRACT_FLAG). Not
# needed on admbridge_shared: it never statically embeds adm/bw64 itself (see this file's header
# comment - it links ac3adm_shared, a real shared-library dependency, not an archive), and its own
# leaked symbol count is in the same small ballpark (~130) every other library here already
# carries as baseline libstdc++ template noise (tools/ci/abi-allowlist/libac3forge.so.txt has
# ~220 of the same shape).
if(NOT MSVC AND NOT (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND
                      CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    target_link_options(ac3adm_shared PRIVATE "LINKER:--exclude-libs,ALL")
endif()
