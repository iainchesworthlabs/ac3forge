# ---------------------------------------------------------------------------
# InstallLibrary.cmake
#
# install() rules + package config for distributing iclforge::ac3, iclforge::matroska, iclforge::mp4 and
# the other libraries below independently, consumable via find_package(iclforge). iclforge::audio
# (src/audio/) is deliberately NOT installed/exported here - it is a CLI/GUI implementation
# detail, not part of the distributed package; see docs/library/index.md.
#
# include()'d from the root CMakeLists.txt after add_subdirectory(src/ac3) and, for each
# optional component, its own guarded add_subdirectory(src/matroska|mp4|mpegts), before
# include(Packaging) - CPack's own library component (cmake/Packaging.cmake) packages exactly
# what gets install()'d here.
#
# iclforge::matroska, iclforge::mp4 and iclforge::mpegts are all optional components, off-able via
# their own ICLFORGE_BUILD_MATROSKA/ICLFORGE_BUILD_MP4/ICLFORGE_BUILD_MPEGTS option (root
# CMakeLists.txt) - each its own ICLFORGE_BUILD_<NAME> option, its own guarded
# add_subdirectory(), and its own guarded block below, as are iclforge::c, the AC-4 libraries,
# iclforge::iab and iclforge::iamf. Each maps 1:1 onto its own vcpkg feature
# (packaging/vcpkg-port/iclforge/vcpkg.json's "matroska"/"mp4"/"mpegts"/"capi"/"ac4"/"iab"/
# "iamf", wired through portfile.cmake's vcpkg_check_features()) and its own Conan option
# (packaging/conan/conanfile.py), so a vcpkg or Conan install only gets the ones its feature
# selection actually asked for.
#
# Every install() rule below carries COMPONENT library: without one, CPack
# files it under its own "Unspecified" component, inconsistent once
# component-based packaging is on (see cmake/Packaging.cmake) - same reason
# apps/cli/CMakeLists.txt's forge install() carries COMPONENT runtime.
#
# The LIBRARY DESTINATION rules below additionally carry NAMELINK_COMPONENT
# library, splitting them from COMPONENT libruntime. On Unix, a versioned
# shared library install produces two files - the real
# libiclforge_ac3.so.<version> and an unversioned libiclforge_ac3.so symlink (the
# "namelink") a linker resolves -l against - and NAMELINK_COMPONENT is CMake's
# own mechanism for filing those two files under different CPack components:
# COMPONENT names the real .so, NAMELINK_COMPONENT names the symlink. Confirmed
# empirically (see cmake/Packaging.cmake's DEB/RPM comment) that today's
# monolithic .deb bundles forge together with the full SDK - headers, static
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
# variants installed and exported, same as before this option existed. iclforge_ac3_static/forge_shared
# and their matroska/mp4/mpegts equivalents still get *built* either way - only what gets
# install()'d/exported is filtered by this option, so nothing above this point in the tree
# needs touching for it to take effect.
option(ICLFORGE_INSTALL_BOTH_LINKAGES "Install/export both static and shared library variants (OFF installs only the BUILD_SHARED_LIBS-selected one)" ON)

if(ICLFORGE_INSTALL_BOTH_LINKAGES)
    set(_iclforge_forge_install_targets iclforge_ac3_objects iclforge_ac3_static iclforge_ac3_shared)
    set(_iclforge_signing_install_targets iclforge_signing_objects iclforge_signing_static iclforge_signing_shared)
    set(_iclforge_matroska_install_targets iclforge_matroska_objects iclforge_matroska_static iclforge_matroska_shared)
    set(_iclforge_mp4_install_targets iclforge_mp4_objects iclforge_mp4_static iclforge_mp4_shared)
    set(_iclforge_mpegts_install_targets iclforge_mpegts_objects iclforge_mpegts_static iclforge_mpegts_shared)
    set(_iclforge_iab_install_targets iclforge_iab_objects iclforge_iab_static iclforge_iab_shared)
    set(_iclforge_iamf_install_targets iclforge_iamf_objects iclforge_iamf_static iclforge_iamf_shared)
    set(_iclforge_ac4_install_targets iclforge_ac4_objects iclforge_ac4_static iclforge_ac4_shared)
    set(_iclforge_ac4dec_install_targets iclforge_ac4dec_objects iclforge_ac4dec_static iclforge_ac4dec_shared)
    set(_iclforge_ac4enc_install_targets iclforge_ac4enc_objects iclforge_ac4enc_static iclforge_ac4enc_shared)
    set(_iclforge_capi_install_targets iclforge_capi_objects iclforge_capi_static iclforge_capi_shared)
elseif(BUILD_SHARED_LIBS)
    # iclforge::c (src/capi/CMakeLists.txt) statically embeds iclforge::ac3_static PRIVATE
    # unconditionally, regardless of BUILD_SHARED_LIBS - see that file's header comment for why
    # (a self-contained C ABI, not one that depends on a separately-shipped forge shared
    # library). iclforge_ac3_static used to have to be in an export set here: iclforge_capi_objects is an
    # OBJECT library, so the PRIVATE dependency ended up in its own INTERFACE_LINK_LIBRARIES
    # (OBJECT libraries have no link step of their own to hide it behind), and since
    # iclforge_capi_objects is itself part of capiTargets whenever ICLFORGE_BUILD_CAPI is ON,
    # install(EXPORT capiTargets) failed with "requires target iclforge_ac3_static that is not in any
    # export set." That dependency now sits on iclforge_capi_static and iclforge_capi_shared instead, and a
    # shared library's PRIVATE dependencies are not exported, so nothing in capiTargets names
    # iclforge_ac3_static in this branch any more. It is still installed, so that a package built this
    # way keeps shipping the archive it always has. iclforge_ac3_shared never needed the same treatment.
    if(ICLFORGE_BUILD_CAPI)
        set(_iclforge_forge_install_targets iclforge_ac3_objects iclforge_ac3_static iclforge_ac3_shared)
    else()
        set(_iclforge_forge_install_targets iclforge_ac3_objects iclforge_ac3_shared)
    endif()
    set(_iclforge_signing_install_targets iclforge_signing_objects iclforge_signing_shared)
    set(_iclforge_matroska_install_targets iclforge_matroska_objects iclforge_matroska_shared)
    set(_iclforge_mp4_install_targets iclforge_mp4_objects iclforge_mp4_shared)
    set(_iclforge_mpegts_install_targets iclforge_mpegts_objects iclforge_mpegts_shared)
    set(_iclforge_iab_install_targets iclforge_iab_objects iclforge_iab_shared)
    set(_iclforge_iamf_install_targets iclforge_iamf_objects iclforge_iamf_shared)
    set(_iclforge_ac4_install_targets iclforge_ac4_objects iclforge_ac4_shared)
    set(_iclforge_ac4dec_install_targets iclforge_ac4dec_objects iclforge_ac4dec_shared)
    set(_iclforge_ac4enc_install_targets iclforge_ac4enc_objects iclforge_ac4enc_shared)
    set(_iclforge_capi_install_targets iclforge_capi_objects iclforge_capi_shared)
else()
    set(_iclforge_forge_install_targets iclforge_ac3_objects iclforge_ac3_static)
    set(_iclforge_signing_install_targets iclforge_signing_objects iclforge_signing_static)
    set(_iclforge_matroska_install_targets iclforge_matroska_objects iclforge_matroska_static)
    set(_iclforge_mp4_install_targets iclforge_mp4_objects iclforge_mp4_static)
    set(_iclforge_mpegts_install_targets iclforge_mpegts_objects iclforge_mpegts_static)
    set(_iclforge_iab_install_targets iclforge_iab_objects iclforge_iab_static)
    set(_iclforge_iamf_install_targets iclforge_iamf_objects iclforge_iamf_static)
    set(_iclforge_ac4_install_targets iclforge_ac4_objects iclforge_ac4_static)
    set(_iclforge_ac4dec_install_targets iclforge_ac4dec_objects iclforge_ac4dec_static)
    set(_iclforge_ac4enc_install_targets iclforge_ac4enc_objects iclforge_ac4enc_static)
    set(_iclforge_capi_install_targets iclforge_capi_objects iclforge_capi_static)
endif()

# iclforge_ac3_simd_avx2 (runtime SIMD dispatch, x86_64 only,
# ICLFORGE_AVX2) is PUBLIC-linked into both iclforge_ac3_static and iclforge_ac3_shared
# unconditionally (src/ac3/CMakeLists.txt), so it needs the same
# export-set membership iclforge_ac3_objects gets just above and for the same
# reason: install(EXPORT) cannot resolve a usage-requirement dependency
# that is not itself part of an export set, regardless of which branch
# above put iclforge_ac3_static/forge_shared in the target list. Does not exist
# at all when ICLFORGE_AVX2=OFF or the target is not x86_64.
if(TARGET iclforge_ac3_simd_avx2)
    list(APPEND _iclforge_forge_install_targets iclforge_ac3_simd_avx2)
endif()

# ac3adm/admbridge deliberately do NOT follow the ICLFORGE_INSTALL_BOTH_LINKAGES/BUILD_SHARED_LIBS
# selection above - they are always shared-only, unconditionally, regardless of how the rest of
# this project is configured. See src/adm/CMakeLists.txt's and src/admbridge/CMakeLists.txt's
# own header comments for why: ac3adm PRIVATE-embeds the third-party libbw64/libadm (never
# installed/exported by this project in their own right), which only a self-contained SHARED
# library can absorb without either re-exporting them or leaving a STATIC archive with genuinely
# unresolved symbols. admbridge follows suit because it PUBLIC-links iclforge::adm_shared.
set(_iclforge_adm_install_targets iclforge_adm_objects iclforge_adm_shared)
set(_iclforge_admbridge_install_targets iclforge_admbridge_objects iclforge_admbridge_shared)

# Raw target names are iclforge_<lib>_<kind>; the export sets prefix the iclforge:: namespace, so the
# exported names are <lib>_<kind> (iclforge::mp4_static), as iclforge_add_library() sets for its libraries.
foreach(_pair IN ITEMS signing:signing matroska:matroska mp4:mp4 mpegts:mpegts iamf:iamf ac4:ac4
                       ac4dec:ac4dec ac4enc:ac4enc admbridge:admbridge iab:iab adm:adm capi:c)
    string(REPLACE ":" ";" _pair_list "${_pair}")
    list(GET _pair_list 0 _raw)
    list(GET _pair_list 1 _exp)
    foreach(_kind IN ITEMS objects static shared)
        if(TARGET iclforge_${_raw}_${_kind})
            set_target_properties(iclforge_${_raw}_${_kind} PROPERTIES EXPORT_NAME ${_exp}_${_kind})
        endif()
    endforeach()
endforeach()

# Two separate EXPORT sets, not the one combined set an earlier draft of this
# plan sketched: install(EXPORT ... NAMESPACE X) applies X uniformly to
# every target in that export set, and iclforge::ac3_static/iclforge::ac3_shared
# need a different namespace from iclforge::matroska_static/
# iclforge::matroska_shared. Both still land in the one iclforgeConfig.cmake
# a consumer's find_package(iclforge) resolves - see iclforgeConfig.cmake.in,
# which include()s both generated *Targets.cmake files.
#
# forgeTargets (not iclforgeTargets): every other export set here is named
# after its own ICLFORGE_BUILD_<NAME> component switch (matroskaTargets,
# mp4Targets, mpegtsTargets, capiTargets) - forge has no such switch, since
# it's the one mandatory, always-built component, but it still gets named
# after its own component identity ("forge", matching its raw target names
# iclforge_ac3_static/forge_shared) rather than after the overall package, for the
# same consistency reason.
# The _objects OBJECT library has to be in the same export set as the
# _static/_shared targets that PUBLIC-link it, even though nothing about it
# needs installing on its own (its compiled code is already embedded in the
# installed .lib/.dll) - install(EXPORT) otherwise refuses to generate,
# since it can't resolve a usage-requirement dependency that isn't itself
# part of any export set.
install(TARGETS ${_iclforge_forge_install_targets}
    EXPORT ac3Targets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT library
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT libruntime NAMELINK_COMPONENT library
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT library)

# Source headers, from iclforge::ac3's include/ tree.
install(DIRECTORY "${PROJECT_SOURCE_DIR}/src/ac3/include/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    COMPONENT library)

# Generated headers - ac3/version.hpp (from ac3/version.hpp.in) and the
# generate_export_header() output - live in the library's own binary dir, not
# its source tree (see src/ac3/CMakeLists.txt), so the install(DIRECTORY
# .../include/) call above never sees them. A consumer's
# #include <iclforge/ac3/version.hpp>/<ac3/export.hpp> needs both installed at the
# same relative paths the in-tree BUILD_INTERFACE include dirs already use.
install(FILES
        "${CMAKE_BINARY_DIR}/src/ac3/generated/iclforge/ac3/version.hpp"
        "${CMAKE_BINARY_DIR}/src/ac3/generated/iclforge/ac3/export.hpp"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/iclforge/ac3"
    COMPONENT library)

iclforge_pkgconfig_libname(_iclforge_forge_pc_libname iclforge_ac3_shared iclforge_ac3 iclforge_ac3_static
    "${_iclforge_forge_install_targets}")
iclforge_install_pkgconfig(
    NAME iclforge-ac3
    DESCRIPTION "Clean-room AC-3 (ATSC A/52) and E-AC-3 encoder and decoder with a spatial object layer"
    LIBNAME "${_iclforge_forge_pc_libname}"
    REQUIRES iclforge-base iclforge-dsp iclforge-objects iclforge-render iclforge-iec61937)

# The codec-blind libraries iclforge::ac3 links (src/base, dsp, objects, render, iec61937): each is a
# mandatory component, installed and exported like the codec.
iclforge_install_library(base
    DESCRIPTION "The bit reader and writer, the speaker vocabulary and the CPU feature probe the iclforge libraries build on")
iclforge_install_library(dsp
    DESCRIPTION "The FFT, the QMF bank, the sample-rate converter and the biquad sections the iclforge codecs share")
iclforge_install_library(objects
    DESCRIPTION "The object-audio model, its scene readers and the Object Audio Metadata payload of ETSI TS 103 420")
iclforge_install_library(render
    DESCRIPTION "Speaker layouts, routing, the bed and object renderer and the panner")
iclforge_install_library(iec61937
    DESCRIPTION "IEC 61937 burst packing and unpacking for AC-3, E-AC-3 and AC-4")

# iclforge::signing is mandatory, not an ICLFORGE_BUILD_<NAME>-gated optional component (same as
# iclforge::ac3 itself, unconditionally add_subdirectory()'d in the root CMakeLists.txt) - so unlike
# iclforge::matroska/iclforge::mp4/iclforge::mpegts/iclforge::c below, its install/export block carries
# no if(ICLFORGE_BUILD_...) guard.
install(TARGETS ${_iclforge_signing_install_targets}
    EXPORT signingTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT library
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT libruntime NAMELINK_COMPONENT library
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT library)

install(DIRECTORY "${PROJECT_SOURCE_DIR}/src/signing/include/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    COMPONENT library)

install(FILES "${CMAKE_BINARY_DIR}/src/signing/generated/iclforge/signing/export.hpp"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/iclforge/signing"
    COMPONENT library)

iclforge_pkgconfig_libname(_iclforge_signing_pc_libname iclforge_signing_shared iclforge_signing iclforge_signing_static
    "${_iclforge_signing_install_targets}")
iclforge_install_pkgconfig(
    NAME iclforge-signing
    DESCRIPTION "EMDF Atmos object-signing tag for iclforge"
    LIBNAME "${_iclforge_signing_pc_libname}"
    REQUIRES iclforge-ac3)

# iclforge::matroska is an optional component (ICLFORGE_BUILD_MATROSKA, see the root
# CMakeLists.txt) - a vcpkg port maps this straight to its own "matroska" feature. Its
# targets/headers/export set only exist to install when the component was actually built;
# iclforgeConfig.cmake.in's include() of matroskaTargets.cmake is itself conditional
# (if(EXISTS)) to match. iclforge::mp4 (below) follows this same shape: its own
# ICLFORGE_BUILD_<NAME> option, its own guarded block, its own EXPORT set name.
if(ICLFORGE_BUILD_MATROSKA)
    install(TARGETS ${_iclforge_matroska_install_targets}
        EXPORT matroskaTargets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT library
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT libruntime NAMELINK_COMPONENT library
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT library)

    install(DIRECTORY "${PROJECT_SOURCE_DIR}/src/matroska/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT library)

    install(FILES "${CMAKE_BINARY_DIR}/src/matroska/generated/iclforge/matroska/export.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/iclforge/matroska"
        COMPONENT library)

    iclforge_pkgconfig_libname(_iclforge_matroska_pc_libname iclforge_matroska_shared iclforge_matroska iclforge_matroska_static
        "${_iclforge_matroska_install_targets}")
    iclforge_install_pkgconfig(
        NAME iclforge-matroska
        DESCRIPTION "Standalone Matroska (.mkv) container writer"
        LIBNAME "${_iclforge_matroska_pc_libname}")
endif()

# iclforge::mp4 is an optional component (ICLFORGE_BUILD_MP4, see the root CMakeLists.txt), same
# shape as iclforge::matroska above including the ICLFORGE_INSTALL_BOTH_LINKAGES-selected
# target list - its targets, headers and export set only exist to install when the component
# was actually built. iclforgeConfig.cmake.in's include() of mp4Targets.cmake is itself
# conditional (if(EXISTS)) to match. Maps onto its own "mp4" vcpkg feature the same way
# matroska does (packaging/vcpkg-port/iclforge/vcpkg.json).
if(ICLFORGE_BUILD_MP4)
    install(TARGETS ${_iclforge_mp4_install_targets}
        EXPORT mp4Targets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT library
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT libruntime NAMELINK_COMPONENT library
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT library)

    install(DIRECTORY "${PROJECT_SOURCE_DIR}/src/mp4/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT library)

    install(FILES "${CMAKE_BINARY_DIR}/src/mp4/generated/iclforge/mp4/export.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/iclforge/mp4"
        COMPONENT library)

    iclforge_pkgconfig_libname(_iclforge_mp4_pc_libname iclforge_mp4_shared iclforge_mp4 iclforge_mp4_static
        "${_iclforge_mp4_install_targets}")
    iclforge_install_pkgconfig(
        NAME iclforge-mp4
        DESCRIPTION "Standalone MP4/ISOBMFF container writer, plus fMP4/CMAF and HLS/DASH signaling"
        LIBNAME "${_iclforge_mp4_pc_libname}")
endif()

# iclforge::mpegts is an optional component (ICLFORGE_BUILD_MPEGTS, see the root
# CMakeLists.txt) - same shape as iclforge::matroska immediately above, including its own
# "mpegts" vcpkg feature (packaging/vcpkg-port/iclforge/vcpkg.json).
if(ICLFORGE_BUILD_MPEGTS)
    install(TARGETS ${_iclforge_mpegts_install_targets}
        EXPORT mpegtsTargets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT library
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT libruntime NAMELINK_COMPONENT library
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT library)

    install(DIRECTORY "${PROJECT_SOURCE_DIR}/src/mpegts/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT library)

    install(FILES "${CMAKE_BINARY_DIR}/src/mpegts/generated/iclforge/mpegts/export.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/iclforge/mpegts"
        COMPONENT library)

    iclforge_pkgconfig_libname(_iclforge_mpegts_pc_libname iclforge_mpegts_shared iclforge_mpegts iclforge_mpegts_static
        "${_iclforge_mpegts_install_targets}")
    iclforge_install_pkgconfig(
        NAME iclforge-mpegts
        DESCRIPTION "Standalone MPEG-2 Transport Stream container writer"
        LIBNAME "${_iclforge_mpegts_pc_libname}")
endif()

# iclforge::iab is an optional component (ICLFORGE_BUILD_IAB, see the root CMakeLists.txt) -
# same shape as iclforge::matroska/iclforge::mp4/iclforge::mpegts above, a reader rather than a
# writer. The vcpkg port's "iab" feature and the Conan recipe's "iab" option switch it, off unless
# asked for (packaging/).
if(ICLFORGE_BUILD_IAB)
    install(TARGETS ${_iclforge_iab_install_targets}
        EXPORT iabTargets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT library
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT libruntime NAMELINK_COMPONENT library
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT library)

    install(DIRECTORY "${PROJECT_SOURCE_DIR}/src/iab/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT library)

    install(FILES "${CMAKE_BINARY_DIR}/src/iab/generated/iclforge/iab/export.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/iclforge/iab"
        COMPONENT library)

    iclforge_pkgconfig_libname(_iclforge_iab_pc_libname iclforge_iab_shared iclforge_iab iclforge_iab_static
        "${_iclforge_iab_install_targets}")
    iclforge_install_pkgconfig(
        NAME iclforge-iab
        DESCRIPTION "Standalone SMPTE ST 2098-2 Immersive Audio Bitstream reader"
        LIBNAME "${_iclforge_iab_pc_libname}")
endif()

# iclforge::adm and iclforge::admbridge are optional components (ICLFORGE_BUILD_ADM, see the root
# CMakeLists.txt - admbridge shares ac3adm's own flag, see src/admbridge/CMakeLists.txt's header
# comment) - but unlike every other component in this file, each installs/exports SHARED ONLY
# (${_iclforge_adm_install_targets}/${_iclforge_admbridge_install_targets}, set above,
# unconditionally shared regardless of ICLFORGE_INSTALL_BOTH_LINKAGES/BUILD_SHARED_LIBS) - see
# src/adm/CMakeLists.txt's header comment for why. No vcpkg/Conan feature: ac3adm already has
# none (needs Boost, see docs/library/index.md), and admbridge transitively depends on it.
if(ICLFORGE_BUILD_ADM)
    install(TARGETS ${_iclforge_adm_install_targets}
        EXPORT admTargets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT library
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT libruntime NAMELINK_COMPONENT library
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT library)

    install(DIRECTORY "${PROJECT_SOURCE_DIR}/src/adm/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT library)

    install(FILES "${CMAKE_BINARY_DIR}/src/adm/generated/iclforge/adm/export.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/iclforge/adm"
        COMPONENT library)

    # LIBNAME hardcoded, not iclforge_pkgconfig_libname() - ac3adm/admbridge are shared-only, so
    # there's no static/shared choice to derive here, unlike every other component above.
    iclforge_install_pkgconfig(
        NAME iclforge-adm
        DESCRIPTION "Standalone BW64/RF64 + Audio Definition Model (ADM) parser"
        LIBNAME iclforge_adm)

    install(TARGETS ${_iclforge_admbridge_install_targets}
        EXPORT admbridgeTargets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT library
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT libruntime NAMELINK_COMPONENT library
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT library)

    install(DIRECTORY "${PROJECT_SOURCE_DIR}/src/admbridge/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT library)

    install(FILES "${CMAKE_BINARY_DIR}/src/admbridge/generated/iclforge/admbridge/export.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/iclforge/admbridge"
        COMPONENT library)

    iclforge_install_pkgconfig(
        NAME iclforge-admbridge
        DESCRIPTION "Maps a parsed ADM object graph onto/from iclforge::oba::AtmosEncoder"
        LIBNAME iclforge_admbridge
        REQUIRES iclforge-ac3 iclforge-adm)
endif()

# iclforge::iamf is an optional component (ICLFORGE_BUILD_IAMF, see the root CMakeLists.txt) - same
# shape as ac3iab immediately above, a writer rather than a reader. The vcpkg port's "iamf"
# feature and the Conan recipe's "iamf" option switch it, off unless asked for (packaging/).
if(ICLFORGE_BUILD_IAMF)
    install(TARGETS ${_iclforge_iamf_install_targets}
        EXPORT iamfTargets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT library
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT libruntime NAMELINK_COMPONENT library
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT library)

    install(DIRECTORY "${PROJECT_SOURCE_DIR}/src/iamf/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT library)

    install(FILES "${CMAKE_BINARY_DIR}/src/iamf/generated/iclforge/iamf/export.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/iclforge/iamf"
        COMPONENT library)

    iclforge_pkgconfig_libname(_iclforge_iamf_pc_libname iclforge_iamf_shared iclforge_iamf iclforge_iamf_static
        "${_iclforge_iamf_install_targets}")
    iclforge_install_pkgconfig(
        NAME iclforge-iamf
        DESCRIPTION "IAMF v1.1.0 OBU / ISO-BMFF writer"
        LIBNAME "${_iclforge_iamf_pc_libname}")
endif()

# The AC-4 inspector (src/ac4, iclforge::ac4), the AC-4 decoder (src/ac4dec, iclforge::ac4dec) and the
# AC-4 encoder (src/ac4enc, iclforge::ac4enc) are optional components under one switch,
# ICLFORGE_BUILD_AC4 (see the root CMakeLists.txt), with the core the decoder and the encoder link
# (src/ac4core, iclforge::ac4core). The four share one export set, ac4Targets, installed under the ac4::
# namespace the tree's own aliases use, each with its own install() and its own .pc file - the
# pairing tools/checks/check_packaging_versions.sh counts. The decoder's targets are exported as
# decoder_static and decoder_shared, the encoder's as encoder_static and encoder_shared, and the
# core as core (src/ac4dec/CMakeLists.txt, src/ac4enc/CMakeLists.txt, src/ac4core/CMakeLists.txt).
#
# The core is a static archive of hidden symbols with no headers and no ABI of its own. The
# static decoder's and encoder's archives call into it without containing it, so it is installed
# wherever either archive is and named by their exported targets as a link-only dependency; the
# shared libraries carry the part of it that each uses, and a shared-only install has no core. The
# vcpkg port's "ac4" feature and the Conan recipe's "ac4" option switch all four, off unless asked
# for (packaging/): each adds public targets, which a curated vcpkg port's default features may
# not.
if(ICLFORGE_BUILD_AC4)
    install(TARGETS ${_iclforge_ac4_install_targets}
        EXPORT ac4Targets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT library
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT libruntime NAMELINK_COMPONENT library
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT library)

    install(DIRECTORY "${PROJECT_SOURCE_DIR}/src/ac4/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT library)

    install(FILES "${CMAKE_BINARY_DIR}/src/ac4/generated/iclforge/ac4/export.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/iclforge/ac4"
        COMPONENT library)

    iclforge_pkgconfig_libname(_iclforge_ac4_pc_libname iclforge_ac4_shared iclforge_ac4 iclforge_ac4_static
        "${_iclforge_ac4_install_targets}")
    iclforge_install_pkgconfig(
        NAME iclforge-ac4
        DESCRIPTION "AC-4 (ETSI TS 103 190) sync frame, table of contents and presentation reader"
        LIBNAME "${_iclforge_ac4_pc_libname}")

    install(TARGETS ${_iclforge_ac4dec_install_targets}
        EXPORT ac4Targets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT library
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT libruntime NAMELINK_COMPONENT library
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT library)

    install(DIRECTORY "${PROJECT_SOURCE_DIR}/src/ac4dec/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT library)

    install(FILES "${CMAKE_BINARY_DIR}/src/ac4dec/generated/iclforge/ac4dec/export.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/iclforge/ac4dec"
        COMPONENT library)

    # REQUIRES ac4: the decoder's header includes the inspector's, and each decoder library links
    # the inspector of its own kind. STATIC_REQUIRES ac4core: libac4dec_static.a calls into the
    # core's archive, which only a static-only install names here (iclforge_install_pkgconfig()).
    iclforge_pkgconfig_libname(_iclforge_ac4dec_pc_libname iclforge_ac4dec_shared iclforge_ac4dec iclforge_ac4dec_static
        "${_iclforge_ac4dec_install_targets}")
    iclforge_install_pkgconfig(
        NAME iclforge-ac4dec
        DESCRIPTION "AC-4 decoder (ETSI TS 103 190-1 and TS 103 190-2)"
        LIBNAME "${_iclforge_ac4dec_pc_libname}"
        REQUIRES iclforge-ac4
        STATIC_REQUIRES iclforge-ac4core)

    install(TARGETS ${_iclforge_ac4enc_install_targets}
        EXPORT ac4Targets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT library
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT libruntime NAMELINK_COMPONENT library
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT library)

    install(DIRECTORY "${PROJECT_SOURCE_DIR}/src/ac4enc/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT library)

    install(FILES "${CMAKE_BINARY_DIR}/src/ac4enc/generated/iclforge/ac4enc/export.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/iclforge/ac4enc"
        COMPONENT library)

    # As the decoder's: the encoder's header includes the inspector's, each encoder library links
    # the inspector of its own kind, and libac4enc_static.a calls into the core's archive.
    iclforge_pkgconfig_libname(_iclforge_ac4enc_pc_libname iclforge_ac4enc_shared iclforge_ac4enc iclforge_ac4enc_static
        "${_iclforge_ac4enc_install_targets}")
    iclforge_install_pkgconfig(
        NAME iclforge-ac4enc
        DESCRIPTION "AC-4 encoder (ETSI TS 103 190-1 and TS 103 190-2)"
        LIBNAME "${_iclforge_ac4enc_pc_libname}"
        REQUIRES iclforge-ac4
        STATIC_REQUIRES iclforge-ac4core)

    if("iclforge_ac4dec_static" IN_LIST _iclforge_ac4dec_install_targets OR
       "iclforge_ac4enc_static" IN_LIST _iclforge_ac4enc_install_targets)
        install(TARGETS iclforge_ac4core
            EXPORT ac4Targets
            ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT library)

        iclforge_install_pkgconfig(
            NAME iclforge-ac4core
            DESCRIPTION "The tables and transforms libac4dec_static.a and libac4enc_static.a link (no headers)"
            LIBNAME iclforge_ac4core_static)
    endif()
endif()

# iclforge::c is an optional component (ICLFORGE_BUILD_CAPI, see the root CMakeLists.txt) - same
# shape as iclforge::matroska/iclforge::mp4/iclforge::mpegts above. Roadmap item F1's whole point is a
# stable C-callable surface for OTHER toolchains, so its header (iclforge_c/iclforge.h) installs
# to its own include/iclforge_c/ subdirectory rather than under include/ac3/ - a C or non-C++
# consumer has no reason to see (or accidentally #include) any C++ header this package ships.
if(ICLFORGE_BUILD_CAPI)
    install(TARGETS ${_iclforge_capi_install_targets}
        EXPORT capiTargets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT library
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT libruntime NAMELINK_COMPONENT library
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT library)

    install(DIRECTORY "${PROJECT_SOURCE_DIR}/src/capi/include/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        COMPONENT library)

    # iclforge.h #includes both of these, and neither is in the source include/ tree the
    # install(DIRECTORY) above copies: export.h is generate_export_header()'s output and
    # version.h is configure_file()'d from version.h.in (which that install does copy, as the
    # template), each into src/capi's own binary dir. Without version.h, every
    # #include <iclforge_c/iclforge.h> against an installed prefix fails to compile. Same
    # reason ac3/version.hpp and ac3/export.hpp are installed by name for iclforge::ac3 above.
    install(FILES
            "${CMAKE_BINARY_DIR}/src/capi/generated/iclforge_c/export.h"
            "${CMAKE_BINARY_DIR}/src/capi/generated/iclforge_c/version.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/iclforge_c"
        COMPONENT library)

    # STATIC_REQUIRES iclforge: libiclforge_c_static.a calls into libiclforge_ac3_static.a (capiTargets
    # carries $<LINK_ONLY:iclforge::ac3_static> for it), and only a static-only install names the
    # archive here. iclforge_install_pkgconfig() says why that goes to Requires.private:
    # libiclforge_c.so embeds the codec and needs no libiclforge_ac3.so beside it.
    iclforge_pkgconfig_libname(_iclforge_capi_pc_libname iclforge_capi_shared iclforge_c iclforge_c_static
        "${_iclforge_capi_install_targets}")
    # iclforge.h declares its AC-4 section either way; with ICLFORGE_BUILD_AC4 off those functions
    # return ICLFORGE_ERROR_UNSUPPORTED (src/capi/src/ac4_absent.cpp). With it on, the archive also
    # calls into the decoder's and the encoder's (capiTargets carries their $<LINK_ONLY:...> archives
    # too), and the .pc of each brings the inspector and the core.
    if(ICLFORGE_BUILD_AC4)
        set(_iclforge_capi_pc_description
            "Stable C11 API over the AC-3, E-AC-3 and AC-4 encoders and decoders")
        set(_iclforge_capi_pc_static_requires iclforge-ac3 iclforge-ac4dec iclforge-ac4enc)
    else()
        set(_iclforge_capi_pc_description
            "Stable C11 API over the AC-3 and E-AC-3 encoders and decoders")
        set(_iclforge_capi_pc_static_requires iclforge-ac3)
    endif()
    iclforge_install_pkgconfig(
        NAME iclforge-c
        DESCRIPTION "${_iclforge_capi_pc_description}"
        LIBNAME "${_iclforge_capi_pc_libname}"
        STATIC_REQUIRES ${_iclforge_capi_pc_static_requires})
endif()

# The config file find_package(iclforge) actually loads. No find_dependency()
# calls needed in iclforgeConfig.cmake.in: the platform-audio code is
# physically in a separate, non-exported target (iclforge::audio), and {fmt}, the
# one third-party library iclforge::ac3 and iclforge::mp4 use, is compiled into their
# object files as a private copy (iclforge::fmt_private, cmake/Fmt.cmake) instead of linked. A
# shared library absorbs a linked {fmt} at its own link step; a static archive
# cannot, so a linked {fmt} would leave its consumers an undefined fmt:: symbol
# that this package names nowhere. tools/checks/check_install_consumer.sh links
# every installed archive whole, so a symbol that neither the package nor the
# C/C++ runtime supplies fails there.
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/iclforgeConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/iclforgeConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/iclforge")

# SameMajorVersion, not exact: pre-1.0, there is no ABI-compatibility promise
# across any two releases (see src/ac3/CMakeLists.txt's SOVERSION comment for
# the full reasoning), but SameMajorVersion is the conventional default and
# is what actually governs here - find_package()'s own version matching
# against a requested `find_package(iclforge X.Y.Z)`, not the .so's SONAME
# (which is set separately, to the full version, precisely because 0.x has
# no narrower compatible range to express).
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/iclforgeConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion)

install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/iclforgeConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/iclforgeConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/iclforge"
    COMPONENT library)

install(EXPORT ac3Targets
    FILE ac3Targets.cmake
    NAMESPACE iclforge::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/iclforge"
    COMPONENT library)

install(EXPORT signingTargets
    FILE signingTargets.cmake
    NAMESPACE iclforge::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/iclforge"
    COMPONENT library)

if(ICLFORGE_BUILD_MATROSKA)
    install(EXPORT matroskaTargets
        FILE matroskaTargets.cmake
        NAMESPACE iclforge::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/iclforge"
        COMPONENT library)
endif()

if(ICLFORGE_BUILD_MP4)
    install(EXPORT mp4Targets
        FILE mp4Targets.cmake
        NAMESPACE iclforge::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/iclforge"
        COMPONENT library)
endif()

if(ICLFORGE_BUILD_MPEGTS)
    install(EXPORT mpegtsTargets
        FILE mpegtsTargets.cmake
        NAMESPACE iclforge::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/iclforge"
        COMPONENT library)
endif()

if(ICLFORGE_BUILD_IAB)
    install(EXPORT iabTargets
        FILE iabTargets.cmake
        NAMESPACE iclforge::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/iclforge"
        COMPONENT library)
endif()

if(ICLFORGE_BUILD_ADM)
    install(EXPORT admTargets
        FILE admTargets.cmake
        NAMESPACE iclforge::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/iclforge"
        COMPONENT library)

    # ac3:: namespace, not ac3admbridge:: - matches its in-tree alias (iclforge::admbridge_shared,
    # see src/admbridge/CMakeLists.txt), the same way capiTargets uses ac3:: below for forge_c.
    install(EXPORT admbridgeTargets
        FILE admbridgeTargets.cmake
        NAMESPACE iclforge::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/iclforge"
        COMPONENT library)
endif()

if(ICLFORGE_BUILD_IAMF)
    install(EXPORT iamfTargets
        FILE iamfTargets.cmake
        NAMESPACE iclforge::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/iclforge"
        COMPONENT library)
endif()

if(ICLFORGE_BUILD_AC4)
    install(EXPORT ac4Targets
        FILE ac4Targets.cmake
        NAMESPACE iclforge::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/iclforge"
        COMPONENT library)
endif()

if(ICLFORGE_BUILD_CAPI)
    install(EXPORT capiTargets
        FILE capiTargets.cmake
        NAMESPACE iclforge::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/iclforge"
        COMPONENT library)
endif()
