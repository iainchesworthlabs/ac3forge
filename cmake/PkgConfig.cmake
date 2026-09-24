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

# What a link has to add for the C++ objects in a static archive when a C compiler drives it - a
# C program, or a Makefile or Meson build linking with cc: the C++ runtime, and libm for the
# archives that call it. A C++ driver adds both itself. The runtime's name depends on the
# toolchain (libstdc++ or libc++, and the NDK and Apple differ again), so it is read from what
# CMake recorded for the compiler that built the archives, in CMAKE_CXX_IMPLICIT_LINK_LIBRARIES.
# That list also holds what a C driver links anyway (c, gcc, gcc_s), which stays out. It is empty
# for MSVC, which has no pkg-config consumer.
set(_AC3FORGE_PC_CXX_RUNTIME_LIBS "")
foreach(_ac3forge_pc_lib IN LISTS CMAKE_CXX_IMPLICIT_LINK_LIBRARIES)
    if(_ac3forge_pc_lib MATCHES "^(stdc\\+\\+|supc\\+\\+|c\\+\\+|c\\+\\+abi|m)$")
        list(APPEND _AC3FORGE_PC_CXX_RUNTIME_LIBS "-l${_ac3forge_pc_lib}")
    endif()
endforeach()
list(REMOVE_DUPLICATES _AC3FORGE_PC_CXX_RUNTIME_LIBS)
unset(_ac3forge_pc_lib)

# NAME: pkg-config name, e.g. `pkg-config --libs ac3forge` - matches the shared OUTPUT_NAME
# convention (see e.g. src/forge/CMakeLists.txt), which is also the on-disk library basename
# whenever the shared variant is what's actually installed.
# LIBNAME: the `-l<LIBNAME>` this component's install actually provides - see
# ac3forge_pkgconfig_libname() below for how callers derive this correctly for whichever
# linkage(s) got installed.
# REQUIRES: other .pc names this one's Requires: line should chain to (space-separated), for a
# genuine PUBLIC/usage-requirement dependency - e.g. ac3signing requires ac3forge because
# signing_static/signing_shared PUBLIC-link ac3::forge_static/ac3::forge_shared.
# STATIC_REQUIRES: .pc names a static archive of this component calls into, for a dependency that
# is PRIVATE in CMake - ac3forge_c, whose libac3forge_c_static.a holds calls into
# libac3forge_static.a. An archive is not linked when it is built, so nothing in it records that
# dependency, where a shared library records its own. It is written to Requires.private, which
# pkg-config follows for the link line only with --static, and only when LIBNAME is a static
# archive. A Requires: line would make every consumer of the shared libac3forge_c.so depend on
# libac3forge.so as well, although that library embeds the codec so that it is the one library to
# load (src/capi/CMakeLists.txt).
#
# A .pc that names a static archive (its LIBNAME ends in _static, the name that
# ac3forge_pkgconfig_libname() below picks) also gets Libs.private with the C++ runtime and libm,
# unless it requires another package: that package carries them, and there they follow every
# archive on the link line, which they have to. A linker running with --as-needed, as GCC on
# Ubuntu does by default, drops a shared library that no earlier input needs, so an archive named
# after -lm finds no libm.
function(ac3forge_install_pkgconfig)
    cmake_parse_arguments(ARG "" "NAME;DESCRIPTION;LIBNAME" "REQUIRES;STATIC_REQUIRES" ${ARGN})

    set(AC3FORGE_PC_PREFIX "${_AC3FORGE_PC_PREFIX}")
    set(AC3FORGE_PC_NAME "${ARG_NAME}")
    set(AC3FORGE_PC_DESCRIPTION "${ARG_DESCRIPTION}")
    set(AC3FORGE_PC_LIBNAME "${ARG_LIBNAME}")
    string(REPLACE ";" " " AC3FORGE_PC_REQUIRES "${ARG_REQUIRES}")

    set(AC3FORGE_PC_REQUIRES_PRIVATE "")
    set(AC3FORGE_PC_LIBS_PRIVATE "")
    if(ARG_LIBNAME MATCHES "_static$")
        string(REPLACE ";" " " AC3FORGE_PC_REQUIRES_PRIVATE "${ARG_STATIC_REQUIRES}")
        if(NOT ARG_REQUIRES AND NOT ARG_STATIC_REQUIRES)
            string(REPLACE ";" " " AC3FORGE_PC_LIBS_PRIVATE "${_AC3FORGE_PC_CXX_RUNTIME_LIBS}")
        endif()
    endif()

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
