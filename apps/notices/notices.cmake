# ---------------------------------------------------------------------------
# Forge's NOTICES.txt and LICENSE.txt - what the `runtime` component (forge
# and forge-gui) actually installs, on every platform that packages it.
#
# Until this existed, no Forge package carried either file. cmake/Packaging.cmake's
# CPACK_RESOURCE_FILE_LICENSE only makes the NSIS and DragNDrop installers
# DISPLAY the licence while installing; nothing landed on disk, so a .zip, a
# .deb, a .rpm, a .dmg or the AppImage could be unpacked and read end to end
# without finding the GPL text, the LGPL text for the Qt forge-gui ships on
# Windows and macOS, the SIL OFL for the typefaces it embeds, or the MIT text
# for the {fmt} compiled into both binaries.
#
# include()d from the top-level CMakeLists.txt, after apps/cli and apps/gui
# have added their targets - so ICLFORGE_BUILD_CLI/ICLFORGE_BUILD_GUI are
# settled and every one of those directories' own install() rules has run -
# and before include(Packaging), which needs the install rules below to exist
# for the component to contain them.
#
# The platform is a directory, platform/<os>/components.cmake, the same rule
# apps/crucible/notices/ follows and the same rule the C++ platform trees
# follow (tools/checks/check_platform_macros.ps1): the per-platform strings
# live in one small file per operating system rather than in an if() chain
# threaded through the prose. What is NOT a platform - whether the GUI was
# built at all - is decided here, because it applies to all three.
#
# Qt6_VERSION cannot be read here: apps/gui's find_package(Qt6) ran inside
# add_subdirectory()'s own scope, which does not reach the top level. That is
# what ICLFORGE_GUI_QT_VERSION is for - apps/gui/CMakeLists.txt exports it
# PARENT_SCOPE next to its find_package call, the same shape apps/crucible
# already uses for ICLFORGE_CRUCIBLE_X11_BACKEND.
# ---------------------------------------------------------------------------
include(Notices)
include(GNUInstallDirs)

if(WIN32)
    set(ICLFORGE_NOTICES_PLATFORM_DIR windows)
elseif(APPLE)
    set(ICLFORGE_NOTICES_PLATFORM_DIR macos)
elseif(LINUX)
    set(ICLFORGE_NOTICES_PLATFORM_DIR linux)
else()
    # Loud rather than silent, and deliberately not a fallback to the Linux
    # directory: that file says where a .deb keeps its documentation and what
    # the AppImage carries, neither of which would be true. Shipping a package
    # with no notices at all is the exact bug this file exists to fix, so an
    # unrecognised system stops the configure and says what to add. Windows,
    # macOS and Linux are the three this project verifies (docs/building.md,
    # "Verified configuration").
    message(FATAL_ERROR
        "notices: no apps/notices/platform/<os>/components.cmake for "
        "CMAKE_SYSTEM_NAME '${CMAKE_SYSTEM_NAME}'. Add one - it is three "
        "set() calls; copy platform/linux/components.cmake and correct the "
        "location the package installs NOTICES.txt to.")
endif()

set(ICLFORGE_NOTICES_DIR "${CMAKE_CURRENT_LIST_DIR}")
include("${ICLFORGE_NOTICES_DIR}/platform/${ICLFORGE_NOTICES_PLATFORM_DIR}/components.cmake")

# A CLI-only build carries no Qt and no typefaces: forge links neither, and
# a package that named them would be describing files it does not contain.
# ICLFORGE_BUILD_GUI is the whole test - apps/gui's find_package(Qt6 6.5
# REQUIRED ...) means the option being ON and a Qt kit being absent cannot
# both be true; the configure fails there first.
if(NOT ICLFORGE_BUILD_GUI)
    list(REMOVE_ITEM ICLFORGE_NOTICE_FRAGMENTS
        qt-windows qt-macos qt-linux windows-runtime fonts)
endif()

# Tracy's client library: apps/gui/CMakeLists.txt's forge-gui links iclforge::tracy
# unconditionally, which only pulls in Tracy::TracyClient - and so is only worth
# disclosing - when ICLFORGE_ENABLE_TRACY is on (cmake/Tracy.cmake); forge links no
# such thing, so a CLI-only build (ICLFORGE_BUILD_GUI off) never carries this section
# either way. Same fact, same fragment (found via the apps/crucible/notices/fragments
# FRAGMENT_DIR entry below, not copied) and same conditional as
# apps/crucible/notices/notices.cmake's and apps/hearth/notices/notices.cmake's own
# tracy sections.
if(ICLFORGE_BUILD_GUI AND ICLFORGE_ENABLE_TRACY)
    list(APPEND ICLFORGE_NOTICE_FRAGMENTS tracy)
endif()

# Who each section is about. The header names the programs the reader has;
# FMT_USERS and FONT_USER fill the two fragments shared verbatim with
# apps/crucible/notices/fragments/ (see the FRAGMENT_DIR search path below,
# and cmake/Notices.cmake's header for why those two are shared and the Qt
# sections are not).
if(ICLFORGE_BUILD_CLI AND ICLFORGE_BUILD_GUI)
    set(ICLFORGE_NOTICES_PROGRAMS "forge, the command-line tool, and forge-gui, the window")
    set(ICLFORGE_NOTICES_FMT_USERS "forge and forge-gui")
elseif(ICLFORGE_BUILD_GUI)
    set(ICLFORGE_NOTICES_PROGRAMS "forge-gui, the window")
    set(ICLFORGE_NOTICES_FMT_USERS "forge-gui")
else()
    set(ICLFORGE_NOTICES_PROGRAMS "forge, the command-line tool")
    set(ICLFORGE_NOTICES_FMT_USERS "forge")
endif()

# The versions, from what CMake already holds: {fmt}'s from its package or the
# pinned fallback (cmake/Fmt.cmake), Qt's from apps/gui (see the header above).
if(fmt_VERSION)
    set(ICLFORGE_NOTICES_FMT_VERSION "${fmt_VERSION}")
else()
    set(ICLFORGE_NOTICES_FMT_VERSION "${ICLFORGE_FMT_VERSION}")
endif()
# Tracy's version, from its package - the same Tracy_VERSION/"not reported" choice
# apps/crucible/notices/notices.cmake makes for the identical shared fragment.
if(Tracy_VERSION)
    set(ICLFORGE_NOTICES_TRACY_VERSION "${Tracy_VERSION}")
else()
    set(ICLFORGE_NOTICES_TRACY_VERSION "(version not reported by the Tracy package)")
endif()
# The one value here that crosses an add_subdirectory() boundary, checked
# rather than trusted. An empty Qt version passes the generator's leftover
# check untouched - the {{QT_VERSION}} marker is still substituted, just
# with nothing - so the failure would be a shipped notice reading "Qt " and
# a source URL with an empty version in the middle of it, which nobody
# reading a configure log would see. Moving apps/gui behind another
# directory level, or include()ing it instead of add_subdirectory()ing it,
# is all it takes to break the PARENT_SCOPE export that fills this.
if(ICLFORGE_BUILD_GUI AND NOT ICLFORGE_GUI_QT_VERSION)
    message(FATAL_ERROR
        "notices: ICLFORGE_BUILD_GUI is ON but ICLFORGE_GUI_QT_VERSION is empty, so "
        "the Qt section would name no version and its source URL would point nowhere. "
        "apps/gui/CMakeLists.txt exports it with set(... PARENT_SCOPE) beside its "
        "find_package(Qt6), which reaches this file only while apps/gui is "
        "add_subdirectory()'d straight from the top-level CMakeLists.txt.")
endif()
string(REGEX MATCH "^[0-9]+\\.[0-9]+" ICLFORGE_NOTICES_QT_SERIES "${ICLFORGE_GUI_QT_VERSION}")

set(ICLFORGE_NOTICES_FILE "${CMAKE_BINARY_DIR}/notices/NOTICES.txt")
ac3_generate_notices("${ICLFORGE_NOTICES_FILE}"
    # This directory first, apps/crucible/notices/fragments second: fmt,
    # fonts and trademarks are the same paragraphs for both applications and
    # are taken from there rather than copied, while header and the Qt
    # sections describe what a Forge package contains and are this
    # directory's own. The licence texts come from the same place for the
    # same reason - apps/crucible/notices/licences/ is where the LGPL, the
    # {fmt} MIT, the Mesa MIT and the NCSA texts already live, and a second
    # byte-identical copy is a second thing to keep current.
    FRAGMENT_DIR
        "${ICLFORGE_NOTICES_DIR}/fragments"
        "${CMAKE_SOURCE_DIR}/apps/crucible/notices/fragments"
    FRAGMENTS ${ICLFORGE_NOTICE_FRAGMENTS}
    TOKENS
        "VERSION=${PROJECT_VERSION_FULL}"
        "PLATFORM=${ICLFORGE_NOTICES_PLATFORM}"
        "LOCATION=${ICLFORGE_NOTICES_LOCATION}"
        "PROGRAMS=${ICLFORGE_NOTICES_PROGRAMS}"
        "QT_VERSION=${ICLFORGE_GUI_QT_VERSION}"
        "QT_SERIES=${ICLFORGE_NOTICES_QT_SERIES}"
        "FMT_VERSION=${ICLFORGE_NOTICES_FMT_VERSION}"
        "FMT_USERS=${ICLFORGE_NOTICES_FMT_USERS}"
        "FONT_USER=forge-gui"
        "TRACY_VERSION=${ICLFORGE_NOTICES_TRACY_VERSION}"
        # Always forge-gui, unlike FMT_USERS above: forge links no iclforge::tracy in any
        # configuration, so the tracy fragment only ever names the one binary that does.
        "TRACY_USERS=forge-gui"
    FILES
        "LGPL3=${CMAKE_SOURCE_DIR}/apps/crucible/notices/licences/LGPL-3.0.txt"
        "OFL=${CMAKE_SOURCE_DIR}/apps/gui/fonts/OFL.txt"
        "FMT_MIT=${CMAKE_SOURCE_DIR}/apps/crucible/notices/licences/MIT-fmt.txt"
        "MESA_MIT=${CMAKE_SOURCE_DIR}/apps/crucible/notices/licences/MIT-mesa.txt"
        "DXC_NCSA=${CMAKE_SOURCE_DIR}/apps/crucible/notices/licences/NCSA-dxc.txt"
        "TRACY_BSD=${CMAKE_SOURCE_DIR}/apps/crucible/notices/licences/BSD-3-Clause-Tracy.txt")
message(STATUS "Forge notices  : ${ICLFORGE_NOTICES_PLATFORM} build, sections: ${ICLFORGE_NOTICE_FRAGMENTS}")

# ---------------------------------------------------------------------------
# Where the two files land. COMPONENT runtime for the pair above - the same
# component forge, forge-gui, the man page, the completions and the XDG files
# already install under, so every generator picks them up with the binaries
# rather than needing a component of their own (cmake/Packaging.cmake).
#
# The licence also goes into the two library components, `library` (the
# headers, the import library and the CMake package - the iclforge-dev-*
# archive, libiclforge-dev, iclforge-devel) and `libruntime` (the shared
# object alone - libiclforge0). Both reach someone who never downloads the
# runtime archive, and a library handed over under the GPL with no copy of
# the licence beside it is the omission this file exists to close. Debian
# policy asks for a copyright file in every binary package, not only the one
# carrying the binaries.
#
# They take the licence alone, not the notices: the notices describe what a
# built application bundles - Qt, the fonts, the Windows runtime - and none
# of that is in a library package.
# ---------------------------------------------------------------------------
# Each component's copyright goes under its OWN package name, because that
# is the only path dpkg and lintian look at: /usr/share/doc/<binary package>/
# copyright. A copy under share/doc/iclforge/ satisfies the runtime package
# and nothing else, so libiclforge-dev and libiclforge0 would each ship a
# GPL library with no copyright file of their own - which is the omission
# this block exists to close, appearing to be closed. The names come from
# cmake/Packaging.cmake's CPACK_DEBIAN_<COMPONENT>_PACKAGE_NAME.
#
# LICENSE.txt is a different matter: it is for a person who unpacked an
# archive, so it goes where they will look, and one shared directory is
# right for it.
foreach(_iclforge_lib_component library libruntime)
    if(_iclforge_lib_component STREQUAL "library")
        set(_iclforge_lib_package "libiclforge-dev")
    else()
        set(_iclforge_lib_package "libiclforge0")
    endif()
    if(WIN32 OR APPLE)
        install(FILES "${CMAKE_SOURCE_DIR}/LICENSE"
            DESTINATION "." RENAME "LICENSE.txt" COMPONENT ${_iclforge_lib_component})
    else()
        install(FILES "${CMAKE_SOURCE_DIR}/LICENSE"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/iclforge" RENAME "LICENSE.txt"
            COMPONENT ${_iclforge_lib_component})
        install(FILES "${CMAKE_SOURCE_DIR}/LICENSE"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/${_iclforge_lib_package}"
            RENAME "copyright" COMPONENT ${_iclforge_lib_component})
    endif()
endforeach()
if(WIN32 OR APPLE)
    # The archive/installer root and the .dmg root, beside bin/ and (on
    # macOS) forge-gui.app - the same place apps/crucible puts its pair in the
    # Windows zip, and where someone who unpacked a download looks first.
    install(FILES "${ICLFORGE_NOTICES_FILE}" DESTINATION "." COMPONENT runtime)
    install(FILES "${CMAKE_SOURCE_DIR}/LICENSE"
        DESTINATION "." RENAME "LICENSE.txt" COMPONENT runtime)
else()
    # share/doc/<package>/ - where a .deb and a .rpm keep documentation, and
    # what lands at usr/share/doc/iclforge/ inside the AppImage. iclforge is
    # the runtime component's Debian and RPM package name, set in
    # cmake/Packaging.cmake (CPACK_DEBIAN_RUNTIME_PACKAGE_NAME /
    # CPACK_RPM_RUNTIME_PACKAGE_NAME) - the directory has to match the
    # package name, or Debian policy's own documentation-path rule is broken
    # and lintian says so.
    install(FILES "${ICLFORGE_NOTICES_FILE}"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/iclforge" COMPONENT runtime)
    install(FILES "${CMAKE_SOURCE_DIR}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/iclforge"
        RENAME "LICENSE.txt" COMPONENT runtime)
    # The same text once more as `copyright`, the file dpkg and lintian look
    # for at exactly this path and CPack's DEB generator never writes - the
    # same rule apps/crucible/CMakeLists.txt already follows for its own .deb.
    # Harmless in the .rpm and the tarball, which carry one more file.
    install(FILES "${ICLFORGE_NOTICES_FILE}"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/iclforge"
        RENAME "copyright" COMPONENT runtime)
endif()
