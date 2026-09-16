# ---------------------------------------------------------------------------
# Hearth's NOTICES.txt: the third-party software src/sendspin brings into Hearth's programs
# (planning/hearth-reference-player.md, Dependencies), assembled by cmake/Notices.cmake from the
# fragments beside this file. The licence texts are the copyright files vcpkg installs with each
# port, and the versions are those vcpkg recorded, so the file describes what this build links.
#
# Written to ${CMAKE_BINARY_DIR}/notices/hearth/NOTICES.txt and installed nowhere yet: the test
# sink is never packaged. ac3hearth's About page (A5) and the `hearth` package component (A7) take
# the file from there.
#
# include()d from apps/hearth/CMakeLists.txt for a full Sendspin build only; a core-only build
# (AC3FORGE_SENDSPIN_CORE_ONLY) links none of these.
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

set(AC3HEARTH_NOTICES_FILE "${CMAKE_BINARY_DIR}/notices/hearth/NOTICES.txt")
ac3_generate_notices("${AC3HEARTH_NOTICES_FILE}"
    # This directory first, then Crucible's, whose trademark paragraph is the same for every
    # application (cmake/Notices.cmake's header says why it is shared rather than copied).
    FRAGMENT_DIR
        "${AC3HEARTH_NOTICES_DIR}/fragments"
        "${CMAKE_SOURCE_DIR}/apps/crucible/notices/fragments"
    FRAGMENTS ${AC3HEARTH_NOTICE_FRAGMENTS}
    TOKENS ${AC3HEARTH_NOTICE_TOKENS}
    FILES ${AC3HEARTH_NOTICE_FILES})
message(STATUS "Hearth notices : sections: ${AC3HEARTH_NOTICE_FRAGMENTS}")
