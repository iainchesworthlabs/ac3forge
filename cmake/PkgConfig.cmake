# ---------------------------------------------------------------------------
# PkgConfig.cmake
#
# One .pc file per installed library component (see cmake/InstallLibrary.cmake, which calls
# ac3forge_install_pkgconfig() once at the end of each component's own install block), for a
# non-CMake consumer - `pkg-config --cflags --libs ac3forge`, or a Makefile/autotools/meson build
# that discovers dependencies that way rather than via find_package(). One .pc per component
# mirrors the one-export-set-per-component shape InstallLibrary.cmake already uses; there is no
# umbrella "ac3forge.pc" pulling everything in, the same way there is no single combined CMake
# export set either.
#
# @AC3FORGE_PC_PREFIX@ deliberately does NOT resolve to a build-time-baked CMAKE_INSTALL_PREFIX:
# this project's primary distribution shape is a relocatable ZIP/TGZ archive
# (docs/releasing.md's "What gets published"), unpacked by an end user to an arbitrary directory
# that has nothing to do with the machine this was configured on. A plain `prefix=/some/build/
# time/path` in the .pc file would be silently wrong for every such consumer. Instead this uses
# pkg-config's own `${pcfiledir}` builtin (supported by both pkg-config >= 0.27 and pkgconf,
# widely relied on for exactly this) plus a configure-time-computed relative path back from
# "<libdir>/pkgconfig" to the prefix root, so the prefix resolves correctly wherever the .pc file
# itself physically ends up - a real system install (CMAKE_INSTALL_PREFIX honoured, same as
# ac3forgeConfig.cmake.in's own @PACKAGE_INIT@ relocation) or an unpacked archive alike.
# ---------------------------------------------------------------------------

file(RELATIVE_PATH _ac3forge_pc_prefix_rel
    "/_ac3forge_pkgconfig_dummy_root/${CMAKE_INSTALL_LIBDIR}/pkgconfig"
    "/_ac3forge_pkgconfig_dummy_root")
set(_AC3FORGE_PC_PREFIX "\${pcfiledir}/${_ac3forge_pc_prefix_rel}")
unset(_ac3forge_pc_prefix_rel)

# NAME: pkg-config name, e.g. `pkg-config --libs ac3forge` - matches the shared OUTPUT_NAME
# convention (see e.g. src/forge/CMakeLists.txt), which is also the on-disk library basename
# whenever the shared variant is what's actually installed.
# LIBNAME: the `-l<LIBNAME>` this component's install actually provides - see
# ac3forge_pkgconfig_libname() below for how callers derive this correctly for whichever
# linkage(s) got installed.
# REQUIRES: other .pc names this one's Requires: line should chain to (space-separated), for a
# genuine PUBLIC/usage-requirement dependency - e.g. ac3signing requires ac3forge because
# signing_static/signing_shared PUBLIC-link ac3::forge_static/ac3::forge_shared.
function(ac3forge_install_pkgconfig)
    cmake_parse_arguments(ARG "" "NAME;DESCRIPTION;LIBNAME" "REQUIRES" ${ARGN})

    set(AC3FORGE_PC_PREFIX "${_AC3FORGE_PC_PREFIX}")
    set(AC3FORGE_PC_NAME "${ARG_NAME}")
    set(AC3FORGE_PC_DESCRIPTION "${ARG_DESCRIPTION}")
    set(AC3FORGE_PC_LIBNAME "${ARG_LIBNAME}")
    string(REPLACE ";" " " AC3FORGE_PC_REQUIRES "${ARG_REQUIRES}")

    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PkgConfig.pc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/pkgconfig/${ARG_NAME}.pc"
        @ONLY)

    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/pkgconfig/${ARG_NAME}.pc"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
        COMPONENT library)
endfunction()

# Picks the correct `-l` name for a component that installs either or both of its static/shared
# variants (AC3FORGE_INSTALL_BOTH_LINKAGES/BUILD_SHARED_LIBS - see the target-list selection at
# the top of cmake/InstallLibrary.cmake): the shared OUTPUT_NAME when the shared target is
# actually being installed, else the static one - matching what's genuinely on disk, since this
# project names its static variant "<name>_static" and never installs a plain "<name>" archive
# when only the static variant is present.
function(ac3forge_pkgconfig_libname OUT_VAR SHARED_TARGET SHARED_NAME STATIC_NAME INSTALL_TARGETS)
    if("${SHARED_TARGET}" IN_LIST INSTALL_TARGETS)
        set("${OUT_VAR}" "${SHARED_NAME}" PARENT_SCOPE)
    else()
        set("${OUT_VAR}" "${STATIC_NAME}" PARENT_SCOPE)
    endif()
endfunction()
