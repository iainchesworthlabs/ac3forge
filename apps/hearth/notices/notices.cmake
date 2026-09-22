# ---------------------------------------------------------------------------
# Hearth's NOTICES.txt: the third-party software src/sendspin brings into Hearth's programs,
# plus - on Windows and macOS - the Qt it bundles (planning/hearth-reference-player.md,
# Dependencies), assembled by cmake/Notices.cmake from the fragments beside this file (and, for
# the Qt section, apps/crucible/notices/fragments/, shared rather than copied - see that file's
# header). The licence texts are the copyright files vcpkg installs with each port, and the
# versions are those vcpkg recorded (Qt's from apps/hearth/ui/CMakeLists.txt's own find_package),
# so the file describes what this build actually links and bundles.
#
# Written to ${CMAKE_BINARY_DIR}/notices/hearth/NOTICES.txt. The `hearth` package component (A7,
# apps/hearth/ui/CMakeLists.txt's install() rules) installs it at the package root on Windows and
# macOS and under share/doc/ac3forge-hearth/ on Linux; the test sink, unpackaged, does not.
#
# include()d from apps/hearth/CMakeLists.txt for a full Sendspin build only, AFTER that file's
# add_subdirectory(ui) - this file's Qt section needs ui/'s own PARENT_SCOPE exports
# (AC3HEARTH_UI_QT_FOUND, AC3HEARTH_UI_QT_VERSION), which only exist once ui/ has run. A
# core-only build (AC3FORGE_SENDSPIN_CORE_ONLY) skips ui/ along with this file entirely, so
# neither var is ever read there.
# ---------------------------------------------------------------------------
include(Notices)

set(AC3HEARTH_NOTICES_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(AC3HEARTH_VCPKG_SHARE "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share")
if(NOT VCPKG_INSTALLED_DIR OR NOT VCPKG_TARGET_TRIPLET OR NOT IS_DIRECTORY "${AC3HEARTH_VCPKG_SHARE}")
    message(FATAL_ERROR
        "notices: Hearth's notices read each dependency's version and licence from vcpkg's installed "
        "tree, and '${AC3HEARTH_VCPKG_SHARE}' is not one. Build Hearth through vcpkg's \"hearth\" "
        "feature (-DVCPKG_MANIFEST_FEATURES=hearth).")
endif()

# A port's version as vcpkg recorded it in the port's SPDX document, without vcpkg's own
# port-version suffix ("#1"), which is not the library's.
function(ac3hearth_port_version port out)
    set(spdx "${AC3HEARTH_VCPKG_SHARE}/${port}/vcpkg.spdx.json")
    if(NOT EXISTS "${spdx}")
        message(FATAL_ERROR "notices: ${spdx} does not exist, so the ${port} section would name no version")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${spdx}")
    file(READ "${spdx}" json)
    string(JSON version GET "${json}" packages 0 versionInfo)
    string(REGEX REPLACE "#[0-9]+$" "" version "${version}")
    set(${out} "${version}" PARENT_SCOPE)
endfunction()

set(AC3HEARTH_NOTICE_FRAGMENTS header cpp-httplib mbedtls mdns libflac)

# Qt, bundled into the package on Windows (beside ac3hearth.exe) and inside
# ac3hearth.app on macOS - apps/hearth/ui/CMakeLists.txt's windeployqt /
# qt_generate_deploy_qml_app_script install rules (A7). Linux links the
# system Qt and ships none, the same split
# apps/crucible/notices/platform/<os>/components.cmake makes per platform for
# Crucible. AC3HEARTH_UI_QT_FOUND/AC3HEARTH_UI_QT_VERSION are that ui/
# directory's own PARENT_SCOPE exports (see its comment); reaching this file
# at all depends on apps/hearth/CMakeLists.txt add_subdirectory()'ing ui/
# first, which the "not defined" branch below is guarding against regressing.
if((WIN32 OR APPLE OR LINUX) AND NOT DEFINED AC3HEARTH_UI_QT_FOUND)
    message(FATAL_ERROR
        "notices: AC3HEARTH_UI_QT_FOUND is not defined. apps/hearth/CMakeLists.txt must "
        "add_subdirectory(ui) before include()ing notices/notices.cmake, or this file "
        "cannot tell whether the hearth package bundles Qt.")
endif()

set(AC3HEARTH_QT_PAYLOAD "")
set(AC3HEARTH_QT_LOOKUP "")
if(AC3HEARTH_UI_QT_FOUND AND (WIN32 OR APPLE))
    # Found and bundled but blank would mean the version export above broke
    # loose from the find_package it travels with - the same failure
    # apps/notices/notices.cmake guards against for apps/gui, and for the
    # same reason: an unfilled {{QT_VERSION}} still passes
    # ac3_generate_notices's leftover check (a token is substituted with
    # nothing, not left as a marker), so a shipped "Qt " with an empty
    # source URL would otherwise pass silently.
    if(NOT AC3HEARTH_UI_QT_VERSION)
        message(FATAL_ERROR
            "notices: Qt6 was found for ac3hearth and this platform bundles it into the "
            "hearth package, but AC3HEARTH_UI_QT_VERSION is empty, so the Qt section "
            "would name no version and its source URL would point nowhere. "
            "apps/hearth/ui/CMakeLists.txt exports it with set(... PARENT_SCOPE) beside "
            "its find_package(Qt6).")
    endif()
    list(INSERT AC3HEARTH_NOTICE_FRAGMENTS 1 qt-bundled)
    if(WIN32)
        set(AC3HEARTH_QT_PAYLOAD
            "ac3hearth.exe loads (bin/Qt6*.dll, plugins/, qml/\nand translations/)")
        set(AC3HEARTH_QT_LOOKUP
            "they are looked up by name from the directories\nbin/qt.conf points at.")
    elseif(APPLE)
        set(AC3HEARTH_QT_PAYLOAD
            "ac3hearth loads from inside its application bundle\n(Contents/Frameworks/, Contents/PlugIns/ and\nContents/Resources/)")
        set(AC3HEARTH_QT_LOOKUP
            "they are loaded from inside the\napplication bundle, by the install names each Mach-O file records.")
    endif()
endif()
string(REGEX MATCH "^[0-9]+\\.[0-9]+" AC3HEARTH_QT_SERIES "${AC3HEARTH_UI_QT_VERSION}")

set(AC3HEARTH_NOTICE_TOKENS "VERSION=${PROJECT_VERSION_FULL}")
set(AC3HEARTH_NOTICE_FILES
    "TIME_FILTER_APACHE=${CMAKE_SOURCE_DIR}/src/sendspin/third_party/time-filter/LICENSE")
foreach(port cpp-httplib mbedtls mdns libflac opus)
    ac3hearth_port_version(${port} version)
    string(TOUPPER "${port}" key)
    string(REPLACE "-" "_" key "${key}")
    list(APPEND AC3HEARTH_NOTICE_TOKENS "${key}_VERSION=${version}")
    list(APPEND AC3HEARTH_NOTICE_FILES "${key}_COPYRIGHT=${AC3HEARTH_VCPKG_SHARE}/${port}/copyright")
endforeach()
# libFLAC's port brings libogg with it, for Ogg FLAC.
if(EXISTS "${AC3HEARTH_VCPKG_SHARE}/libogg/copyright")
    ac3hearth_port_version(libogg version)
    list(APPEND AC3HEARTH_NOTICE_FRAGMENTS libogg)
    list(APPEND AC3HEARTH_NOTICE_TOKENS "LIBOGG_VERSION=${version}")
    list(APPEND AC3HEARTH_NOTICE_FILES "LIBOGG_COPYRIGHT=${AC3HEARTH_VCPKG_SHARE}/libogg/copyright")
endif()
list(APPEND AC3HEARTH_NOTICE_FRAGMENTS opus time-filter trademarks)
list(APPEND AC3HEARTH_NOTICE_TOKENS
    "QT_VERSION=${AC3HEARTH_UI_QT_VERSION}"
    "QT_SERIES=${AC3HEARTH_QT_SERIES}"
    "QT_PAYLOAD=${AC3HEARTH_QT_PAYLOAD}"
    "QT_LOOKUP=${AC3HEARTH_QT_LOOKUP}")
# Read only when qt-bundled is actually in the fragment list above
# (ac3_generate_notices ignores a {{FILE:...}} marker no fragment mentions),
# so this is safe to pass unconditionally even on Linux or a no-Qt build.
list(APPEND AC3HEARTH_NOTICE_FILES
    "LGPL3=${CMAKE_SOURCE_DIR}/apps/crucible/notices/licences/LGPL-3.0.txt")

if(NOT AC3HEARTH_NOTICES_FILE)
    message(FATAL_ERROR
        "notices: AC3HEARTH_NOTICES_FILE is not set. apps/hearth/CMakeLists.txt sets it before "
        "add_subdirectory(ui) and include()ing this file, precisely so ui/'s own install() "
        "rules and this generator agree on the same path without either having to run first.")
endif()
ac3_generate_notices("${AC3HEARTH_NOTICES_FILE}"
    # This directory first, then Crucible's, whose trademark paragraph and Qt section (bundled
    # verbatim, tokenised - cmake/Notices.cmake's header says why it is shared rather than
    # copied) are the same text for every application that includes them.
    FRAGMENT_DIR
        "${AC3HEARTH_NOTICES_DIR}/fragments"
        "${CMAKE_SOURCE_DIR}/apps/crucible/notices/fragments"
    FRAGMENTS ${AC3HEARTH_NOTICE_FRAGMENTS}
    TOKENS ${AC3HEARTH_NOTICE_TOKENS}
    FILES ${AC3HEARTH_NOTICE_FILES})
message(STATUS "Hearth notices : sections: ${AC3HEARTH_NOTICE_FRAGMENTS}")
