# ---------------------------------------------------------------------------
# InstallLibrary.cmake
#
# install() rules + package config for distributing ac3::forge, matroska::matroska and mp4::mp4
# independently, consumable via find_package(ac3forge). ac3::audio (src/audio/) is deliberately
# NOT installed/exported here - it is a CLI/GUI implementation detail, not part of the
# distributed package; see docs/library/index.md.
#
# include()'d from the root CMakeLists.txt after add_subdirectory(src/forge) and, for each
# optional component, its own guarded add_subdirectory(src/matroska|mp4|mpegts), before
# include(Packaging) - CPack's own library component (cmake/Packaging.cmake) packages exactly
# what gets install()'d here.
#
# matroska::matroska, mp4::mp4 and mpegts::mpegts are all optional components, off-able via
# their own AC3FORGE_BUILD_MATROSKA/AC3FORGE_BUILD_MP4/AC3FORGE_BUILD_MPEGTS option (root
# CMakeLists.txt) - each its own AC3FORGE_BUILD_<NAME> option, its own guarded
# add_subdirectory(), and its own guarded block below. Each maps 1:1 onto its own vcpkg
# feature (packaging/vcpkg-port/ac3forge/vcpkg.json's "matroska"/"mp4"/"mpegts", wired
# through portfile.cmake's vcpkg_check_features()), so a vcpkg install only gets the ones its
# feature selection actually asked for.
#
# Every install() rule below carries COMPONENT library: without one, CPack
# files it under its own "Unspecified" component, inconsistent once
# component-based packaging is on (see cmake/Packaging.cmake) - same reason
# apps/cli/CMakeLists.txt's ac3cli install() carries COMPONENT runtime.
#
# The LIBRARY DESTINATION rules below additionally carry NAMELINK_COMPONENT
# library, splitting them from COMPONENT libruntime. On Unix, a versioned
# shared library install produces two files - the real
# libac3forge.so.<version> and an unversioned libac3forge.so symlink (the
# "namelink") a linker resolves -l against - and NAMELINK_COMPONENT is CMake's
# own mechanism for filing those two files under different CPack components:
# COMPONENT names the real .so, NAMELINK_COMPONENT names the symlink. Confirmed
# empirically (see cmake/Packaging.cmake's DEB/RPM comment) that today's
# monolithic .deb bundles ac3cli together with the full SDK - headers, static
# archives, CMake package config, .so and symlink alike - because CPack's DEB/
# RPM generators ignore CPACK_COMPONENTS_ALL entirely unless *_COMPONENT_INSTALL
# is explicitly turned on for them. This split is what makes a real
# runtime/-dev separation possible there: libruntime becomes a small
# "just the .so a linked binary needs at runtime" package, while library
# keeps everything only a builder needs (headers, static archives, CMake
# config, and the symlink you link against, -l style). RUNTIME/ARCHIVE (the
# Windows .dll/.lib pair, and the static archives on every platform) stay
# under COMPONENT library throughout: NAMELINK_COMPONENT only ever affects the
# LIBRARY DESTINATION install, i.e. Unix .so installs - Windows has no
# namelink concept at all, so this is a no-op there and the Windows dev ZIP is
# unaffected.
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(PkgConfig)

# OFF is what a vcpkg port needs: vcpkg's per-triplet linkage policy (and its post-build lint)
# expects a port to ship only the variant matching that triplet's VCPKG_LIBRARY_LINKAGE, not
# both. ON (the default) keeps today's direct-build/CPack SDK behaviour unchanged - both
# variants installed and exported, same as before this option existed. forge_static/forge_shared
# and their matroska/mp4/mpegts equivalents still get *built* either way - only what gets
# install()'d/exported is filtered by this option, so nothing above this point in the tree
# needs touching for it to take effect.
option(AC3FORGE_INSTALL_BOTH_LINKAGES "Install/export both static and shared library variants (OFF installs only the BUILD_SHARED_LIBS-selected one)" ON)

if(AC3FORGE_INSTALL_BOTH_LINKAGES)
    set(_ac3forge_forge_install_targets forge_objects forge_static forge_shared)
    set(_ac3forge_signing_install_targets signing_objects signing_static signing_shared)
    set(_ac3forge_matroska_install_targets matroska_objects matroska_static matroska_shared)
    set(_ac3forge_mp4_install_targets mp4_objects mp4_static mp4_shared)
    set(_ac3forge_mpegts_install_targets mpegts_objects mpegts_static mpegts_shared)
    set(_ac3forge_iab_install_targets ac3iab_objects ac3iab_static ac3iab_shared)
    set(_ac3forge_iamf_install_targets iamf_objects iamf_static iamf_shared)
    set(_ac3forge_capi_install_targets forge_c_objects forge_c_static forge_c_shared)
elseif(BUILD_SHARED_LIBS)
    # ac3::forge_c (src/capi/CMakeLists.txt) statically embeds ac3::forge_static PRIVATE
    # unconditionally, regardless of BUILD_SHARED_LIBS - see that file's header comment for why
    # (a self-contained C ABI, not one that depends on a separately-shipped forge shared
    # library). forge_c_objects is an OBJECT library, so that PRIVATE dependency still ends up in
    # forge_c_objects's own INTERFACE_LINK_LIBRARIES (OBJECT libraries have no link step of their
    # own to hide it behind) - and since forge_c_objects is itself part of capiTargets whenever
    # AC3FORGE_BUILD_CAPI is ON, forge_static must be in an export set too, or install(EXPORT
    # capiTargets) fails with "requires target forge_static that is not in any export set."
    # forge_shared has no such requirement, so it doesn't need the same treatment here.
    if(AC3FORGE_BUILD_CAPI)
        set(_ac3forge_forge_install_targets forge_objects forge_static forge_shared)
    else()
        set(_ac3forge_forge_install_targets forge_objects forge_shared)
    endif()
    set(_ac3forge_signing_install_targets signing_objects signing_shared)
    set(_ac3forge_matroska_install_targets matroska_objects matroska_shared)
    set(_ac3forge_mp4_install_targets mp4_objects mp4_shared)
    set(_ac3forge_mpegts_install_targets mpegts_objects mpegts_shared)
    set(_ac3forge_iab_install_targets ac3iab_objects ac3iab_shared)
    set(_ac3forge_iamf_install_targets iamf_objects iamf_shared)
    set(_ac3forge_capi_install_targets forge_c_objects forge_c_shared)
else()
    set(_ac3forge_forge_install_targets forge_objects forge_static)
    set(_ac3forge_signing_install_targets signing_objects signing_static)
    set(_ac3forge_matroska_install_targets matroska_objects matroska_static)
    set(_ac3forge_mp4_install_targets mp4_objects mp4_static)
    set(_ac3forge_mpegts_install_targets mpegts_objects mpegts_static)
    set(_ac3forge_iab_install_targets ac3iab_objects ac3iab_static)
    set(_ac3forge_iamf_install_targets iamf_objects iamf_static)
    set(_ac3forge_capi_install_targets forge_c_objects forge_c_static)
endif()

# ac3adm/admbridge deliberately do NOT follow the AC3FORGE_INSTALL_BOTH_LINKAGES/BUILD_SHARED_LIBS
# selection above - they are always shared-only, unconditionally, regardless of how the rest of
# this project is configured. See src/ac3adm/CMakeLists.txt's and src/admbridge/CMakeLists.txt's
# own header comments for why: ac3adm PRIVATE-embeds the third-party libbw64/libadm (never
# installed/exported by this project in their own right), which only a self-contained SHARED
# library can absorb without either re-exporting them or leaving a STATIC archive with genuinely
# unresolved symbols. admbridge follows suit because it PUBLIC-links ac3adm::ac3adm_shared.
set(_ac3forge_adm_install_targets ac3adm_objects ac3adm_shared)
set(_ac3forge_admbridge_install_targets admbridge_objects admbridge_shared)

# Two separate EXPORT sets, not the one combined set an earlier draft of this
# plan sketched: install(EXPORT ... NAMESPACE X) applies X uniformly to
# every target in that export set, and ac3::forge_static/ac3::forge_shared
# need a different namespace from matroska::matroska_static/
# matroska::matroska_shared. Both still land in the one ac3forgeConfig.cmake
# a consumer's find_package(ac3forge) resolves - see ac3forgeConfig.cmake.in,
# which include()s both generated *Targets.cmake files.
#
# forgeTargets (not ac3forgeTargets): every other export set here is named
# after its own AC3FORGE_BUILD_<NAME> component switch (matroskaTargets,
# mp4Targets, mpegtsTargets, capiTargets) - forge has no such switch, since
# it's the one mandatory, always-built component, but it still gets named
# after its own component identity ("forge", matching its raw target names
# forge_static/forge_shared) rather than after the overall package, for the
# same consistency reason.
# The _objects OBJECT library has to be in the same export set as the
# _static/_shared targets that PUBLIC-link it, even though nothing about it
# needs installing on its own (its compiled code is already embedded in the
# installed .lib/.dll) - install(EXPORT) otherwise refuses to generate,
# since it can't resolve a usage-requirement dependency that isn't itself
# part of any export set.
install(TARGETS ${_ac3forge_forge_install_targets}
    EXPORT forgeTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT library
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT libruntime NAMELINK_COMPONENT library
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT library)

# Source headers, from ac3::forge's include/ tree.
install(DIRECTORY "${PROJECT_SOURCE_DIR}/src/forge/include/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    COMPONENT library)

# Generated headers - ac3/version.hpp (from ac3/version.hpp.in) and the
# generate_export_header() output - live in the library's own binary dir, not
# its source tree (see src/forge/CMakeLists.txt), so the install(DIRECTORY
# .../include/) call above never sees them. A consumer's
# #include <ac3/version.hpp>/<ac3/export.hpp> needs both installed at the
# same relative paths the in-tree BUILD_INTERFACE include dirs already use.
install(FILES
        "${CMAKE_BINARY_DIR}/src/forge/generated/ac3/version.hpp"
        "${CMAKE_BINARY_DIR}/src/forge/generated/ac3/export.hpp"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/ac3"
    COMPONENT library)

ac3forge_pkgconfig_libname(_ac3forge_forge_pc_libname forge_shared ac3forge ac3forge_static
    "${_ac3forge_forge_install_targets}")
ac3forge_install_pkgconfig(
    NAME ac3forge
    DESCRIPTION "${PROJECT_DESCRIPTION}"
    LIBNAME "${_ac3forge_forge_pc_libname}")

# ac3::signing is mandatory, not an AC3FORGE_BUILD_<NAME>-gated optional component (same as
# ac3::forge itself, unconditionally add_subdirectory()'d in the root CMakeLists.txt) - so unlike
# matroska::matroska/mp4::mp4/mpegts::mpegts/ac3::forge_c below, its install/export block carries
# no if(AC3FORGE_BUILD_...) guard.
install(TARGETS ${_ac3forge_signing_install_targets}
    EXPORT signingTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT library
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT libruntime NAMELINK_COMPONENT library
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT library)

install(DIRECTORY "${PROJECT_SOURCE_DIR}/src/signing/include/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    COMPONENT library)

install(FILES "${CMAKE_BINARY_DIR}/src/signing/generated/ac3/signing/export.hpp"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/ac3/signing"
    COMPONENT library)

ac3forge_pkgconfig_libname(_ac3forge_signing_pc_libname signing_shared ac3signing ac3signing_static
    "${_ac3forge_signing_install_targets}")
ac3forge_install_pkgconfig(
    NAME ac3signing
    DESCRIPTION "EMDF Atmos object-signing tag for ac3forge"
    LIBNAME "${_ac3forge_signing_pc_libname}"
    REQUIRES ac3forge)

# matroska::matroska is an optional component (AC3FORGE_BUILD_MATROSKA, see the root
# CMakeLists.txt) - a vcpkg port maps this straight to its own "matroska" feature. Its
# targets/headers/export set only exist to install when the component was actually built;
# ac3forgeConfig.cmake.in's include() of matroskaTargets.cmake is itself conditional
# (if(EXISTS)) to match. mp4::mp4 (below) follows this same shape: its own
# AC3FORGE_BUILD_<NAME> option, its own guarded block, its own EXPORT set name.
if(AC3FORGE_BUILD_MATROSKA)
    install(TARGETS ${_ac3forge_matroska_install_targets}
        EXPORT matroskaTargets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT library
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT libruntime NAMELINK_COMPONENT library
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT library)

    install(DIRECTORY "${PROJECT_SOURCE_DIR}/src/matroska/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT library)

    install(FILES "${CMAKE_BINARY_DIR}/src/matroska/generated/matroska/export.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/matroska"
        COMPONENT library)

    ac3forge_pkgconfig_libname(_ac3forge_matroska_pc_libname matroska_shared matroska matroska_static
        "${_ac3forge_matroska_install_targets}")
    ac3forge_install_pkgconfig(
        NAME matroska
        DESCRIPTION "Standalone Matroska (.mkv) container writer"
        LIBNAME "${_ac3forge_matroska_pc_libname}")
endif()

# mp4::mp4 is an optional component (AC3FORGE_BUILD_MP4, see the root CMakeLists.txt), same
# shape as matroska::matroska above including the AC3FORGE_INSTALL_BOTH_LINKAGES-selected
# target list - its targets, headers and export set only exist to install when the component
# was actually built. ac3forgeConfig.cmake.in's include() of mp4Targets.cmake is itself
# conditional (if(EXISTS)) to match. Maps onto its own "mp4" vcpkg feature the same way
# matroska does (packaging/vcpkg-port/ac3forge/vcpkg.json).
if(AC3FORGE_BUILD_MP4)
    install(TARGETS ${_ac3forge_mp4_install_targets}
        EXPORT mp4Targets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT library
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT libruntime NAMELINK_COMPONENT library
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT library)

    install(DIRECTORY "${PROJECT_SOURCE_DIR}/src/mp4/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT library)

    install(FILES "${CMAKE_BINARY_DIR}/src/mp4/generated/mp4/export.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/mp4"
        COMPONENT library)

    ac3forge_pkgconfig_libname(_ac3forge_mp4_pc_libname mp4_shared mp4 mp4_static
        "${_ac3forge_mp4_install_targets}")
    ac3forge_install_pkgconfig(
        NAME mp4
        DESCRIPTION "Standalone MP4/ISOBMFF container writer, plus fMP4/CMAF and HLS/DASH signaling"
        LIBNAME "${_ac3forge_mp4_pc_libname}")
endif()

# mpegts::mpegts is an optional component (AC3FORGE_BUILD_MPEGTS, see the root
# CMakeLists.txt) - same shape as matroska::matroska immediately above, including its own
# "mpegts" vcpkg feature (packaging/vcpkg-port/ac3forge/vcpkg.json).
if(AC3FORGE_BUILD_MPEGTS)
    install(TARGETS ${_ac3forge_mpegts_install_targets}
        EXPORT mpegtsTargets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT library
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT libruntime NAMELINK_COMPONENT library
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT library)

    install(DIRECTORY "${PROJECT_SOURCE_DIR}/src/mpegts/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT library)

    install(FILES "${CMAKE_BINARY_DIR}/src/mpegts/generated/mpegts/export.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/mpegts"
        COMPONENT library)

    ac3forge_pkgconfig_libname(_ac3forge_mpegts_pc_libname mpegts_shared mpegts mpegts_static
        "${_ac3forge_mpegts_install_targets}")
    ac3forge_install_pkgconfig(
        NAME mpegts
        DESCRIPTION "Standalone MPEG-2 Transport Stream container writer"
        LIBNAME "${_ac3forge_mpegts_pc_libname}")
endif()

# ac3iab::ac3iab is an optional component (AC3FORGE_BUILD_IAB, see the root CMakeLists.txt) -
# same shape as matroska::matroska/mp4::mp4/mpegts::mpegts above, a reader rather than a
# writer. No vcpkg feature of its own yet - new in this PR, following ac3::forge_c's own
# precedent of installing/exporting from day one but waiting to add a vcpkg feature until it
# is formally documented (see packaging/vcpkg-port/ac3forge/portfile.cmake's header comment).
if(AC3FORGE_BUILD_IAB)
    install(TARGETS ${_ac3forge_iab_install_targets}
        EXPORT iabTargets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT library
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT libruntime NAMELINK_COMPONENT library
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT library)

    install(DIRECTORY "${PROJECT_SOURCE_DIR}/src/ac3iab/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT library)

    install(FILES "${CMAKE_BINARY_DIR}/src/ac3iab/generated/ac3iab/export.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/ac3iab"
        COMPONENT library)

    ac3forge_pkgconfig_libname(_ac3forge_iab_pc_libname ac3iab_shared ac3iab ac3iab_static
        "${_ac3forge_iab_install_targets}")
    ac3forge_install_pkgconfig(
        NAME ac3iab
        DESCRIPTION "Standalone SMPTE ST 2098-2 Immersive Audio Bitstream reader"
        LIBNAME "${_ac3forge_iab_pc_libname}")
endif()

# ac3adm::ac3adm and ac3::admbridge are optional components (AC3FORGE_BUILD_ADM, see the root
# CMakeLists.txt - admbridge shares ac3adm's own flag, see src/admbridge/CMakeLists.txt's header
# comment) - but unlike every other component in this file, each installs/exports SHARED ONLY
# (${_ac3forge_adm_install_targets}/${_ac3forge_admbridge_install_targets}, set above,
# unconditionally shared regardless of AC3FORGE_INSTALL_BOTH_LINKAGES/BUILD_SHARED_LIBS) - see
# src/ac3adm/CMakeLists.txt's header comment for why. No vcpkg/Conan feature: ac3adm already has
# none (needs Boost, see docs/library/index.md), and admbridge transitively depends on it.
if(AC3FORGE_BUILD_ADM)
    install(TARGETS ${_ac3forge_adm_install_targets}
        EXPORT admTargets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT library
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT libruntime NAMELINK_COMPONENT library
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT library)

    install(DIRECTORY "${PROJECT_SOURCE_DIR}/src/ac3adm/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT library)

    install(FILES "${CMAKE_BINARY_DIR}/src/ac3adm/generated/ac3adm/export.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/ac3adm"
        COMPONENT library)

    # LIBNAME hardcoded, not ac3forge_pkgconfig_libname() - ac3adm/admbridge are shared-only, so
    # there's no static/shared choice to derive here, unlike every other component above.
    ac3forge_install_pkgconfig(
        NAME ac3adm
        DESCRIPTION "Standalone BW64/RF64 + Audio Definition Model (ADM) parser"
        LIBNAME ac3adm)

    install(TARGETS ${_ac3forge_admbridge_install_targets}
        EXPORT admbridgeTargets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT library
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT libruntime NAMELINK_COMPONENT library
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT library)

    install(DIRECTORY "${PROJECT_SOURCE_DIR}/src/admbridge/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT library)

    install(FILES "${CMAKE_BINARY_DIR}/src/admbridge/generated/ac3/admbridge/export.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/ac3/admbridge"
        COMPONENT library)

    ac3forge_install_pkgconfig(
        NAME admbridge
        DESCRIPTION "Maps a parsed ADM object graph onto/from ac3::oba::AtmosEncoder"
        LIBNAME admbridge
        REQUIRES ac3forge ac3adm)
endif()

# iamf::iamf is an optional component (AC3FORGE_BUILD_IAMF, see the root CMakeLists.txt) - same
# shape as ac3iab immediately above, a writer rather than a reader. No vcpkg feature of its own
# yet, same reasoning as ac3iab's own comment: new in this PR, no downstream consumer to wait on
# yet.
if(AC3FORGE_BUILD_IAMF)
    install(TARGETS ${_ac3forge_iamf_install_targets}
        EXPORT iamfTargets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT library
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT libruntime NAMELINK_COMPONENT library
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT library)

    install(DIRECTORY "${PROJECT_SOURCE_DIR}/src/iamf/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT library)

    install(FILES "${CMAKE_BINARY_DIR}/src/iamf/generated/iamf/export.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/iamf"
        COMPONENT library)

    ac3forge_pkgconfig_libname(_ac3forge_iamf_pc_libname iamf_shared iamf iamf_static
        "${_ac3forge_iamf_install_targets}")
    ac3forge_install_pkgconfig(
        NAME iamf
        DESCRIPTION "IAMF v1.1.0 OBU / ISO-BMFF writer"
        LIBNAME "${_ac3forge_iamf_pc_libname}")
endif()

# ac3::forge_c is an optional component (AC3FORGE_BUILD_CAPI, see the root CMakeLists.txt) - same
# shape as matroska::matroska/mp4::mp4/mpegts::mpegts above. Roadmap item F1's whole point is a
# stable C-callable surface for OTHER toolchains, so its header (ac3forge_c/ac3forge.h) installs
# to its own include/ac3forge_c/ subdirectory rather than under include/ac3/ - a C or non-C++
# consumer has no reason to see (or accidentally #include) any C++ header this package ships.
if(AC3FORGE_BUILD_CAPI)
    install(TARGETS ${_ac3forge_capi_install_targets}
        EXPORT capiTargets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT library
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT libruntime NAMELINK_COMPONENT library
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT library)

    install(DIRECTORY "${PROJECT_SOURCE_DIR}/src/capi/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT library)

    install(FILES "${CMAKE_BINARY_DIR}/src/capi/generated/ac3forge_c/export.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/ac3forge_c"
        COMPONENT library)

    ac3forge_pkgconfig_libname(_ac3forge_capi_pc_libname forge_c_shared ac3forge_c ac3forge_c_static
        "${_ac3forge_capi_install_targets}")
    ac3forge_install_pkgconfig(
        NAME ac3forge_c
        DESCRIPTION "Stable C11 API over ac3::forge's encode/decode core"
        LIBNAME "${_ac3forge_capi_pc_libname}")
endif()

# The config file find_package(ac3forge) actually loads. No find_dependency()
# calls needed in ac3forgeConfig.cmake.in: with the platform-audio code
# physically in a separate, non-exported target (ac3::audio), the installed
# package has no third-party or system dependency whatsoever - matches
# vcpkg.json's own note that the codec itself has none.
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ac3forgeConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/ac3forgeConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ac3forge")

# SameMajorVersion, not exact: pre-1.0, there is no ABI-compatibility promise
# across any two releases (see src/forge/CMakeLists.txt's SOVERSION comment for
# the full reasoning), but SameMajorVersion is the conventional default and
# is what actually governs here - find_package()'s own version matching
# against a requested `find_package(ac3forge X.Y.Z)`, not the .so's SONAME
# (which is set separately, to the full version, precisely because 0.x has
# no narrower compatible range to express).
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/ac3forgeConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion)

install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/ac3forgeConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/ac3forgeConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ac3forge"
    COMPONENT library)

install(EXPORT forgeTargets
    FILE forgeTargets.cmake
    NAMESPACE ac3::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ac3forge"
    COMPONENT library)

install(EXPORT signingTargets
    FILE signingTargets.cmake
    NAMESPACE ac3::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ac3forge"
    COMPONENT library)

if(AC3FORGE_BUILD_MATROSKA)
    install(EXPORT matroskaTargets
        FILE matroskaTargets.cmake
        NAMESPACE matroska::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ac3forge"
        COMPONENT library)
endif()

if(AC3FORGE_BUILD_MP4)
    install(EXPORT mp4Targets
        FILE mp4Targets.cmake
        NAMESPACE mp4::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ac3forge"
        COMPONENT library)
endif()

if(AC3FORGE_BUILD_MPEGTS)
    install(EXPORT mpegtsTargets
        FILE mpegtsTargets.cmake
        NAMESPACE mpegts::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ac3forge"
        COMPONENT library)
endif()

if(AC3FORGE_BUILD_IAB)
    install(EXPORT iabTargets
        FILE iabTargets.cmake
        NAMESPACE ac3iab::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ac3forge"
        COMPONENT library)
endif()

if(AC3FORGE_BUILD_ADM)
    install(EXPORT admTargets
        FILE admTargets.cmake
        NAMESPACE ac3adm::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ac3forge"
        COMPONENT library)

    # ac3:: namespace, not ac3admbridge:: - matches its in-tree alias (ac3::admbridge_shared,
    # see src/admbridge/CMakeLists.txt), the same way capiTargets uses ac3:: below for forge_c.
    install(EXPORT admbridgeTargets
        FILE admbridgeTargets.cmake
        NAMESPACE ac3::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ac3forge"
        COMPONENT library)
endif()

if(AC3FORGE_BUILD_IAMF)
    install(EXPORT iamfTargets
        FILE iamfTargets.cmake
        NAMESPACE iamf::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ac3forge"
        COMPONENT library)
endif()

if(AC3FORGE_BUILD_CAPI)
    install(EXPORT capiTargets
        FILE capiTargets.cmake
        NAMESPACE ac3::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ac3forge"
        COMPONENT library)
endif()
