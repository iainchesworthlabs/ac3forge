# ---------------------------------------------------------------------------
# This build's NOTICES.txt (docs/crucible/design/promotion.md, Phase 6).
#
# include()d from ../CMakeLists.txt once the two build facts that change the
# file are known - whether a Qt kit was found, and whether it has Quick 3D -
# and before anything reads ICLFORGE_CRUCIBLE_NOTICES_FILE: the resource embeddings
# (the window's and the test binary's) and the install rules.
#
# The platform is a directory, platform/<os>/components.cmake, chosen by
# ICLFORGE_CRUCIBLE_PLATFORM_DIR, which the engine's platform arms set - the same
# rule as engine/platform/ and ui/platform/: no fragment, QML or C++ file
# tests the operating system. The two fragments inserted here depend on a
# build option, not a platform, and are gated by the same variables that
# already gate Room3DView.qml and iclforge::tracy.
# ---------------------------------------------------------------------------
include(Notices)

if(NOT ICLFORGE_CRUCIBLE_PLATFORM_DIR)
    message(FATAL_ERROR
        "notices: ICLFORGE_CRUCIBLE_PLATFORM_DIR is not set, so there is no "
        "notices/platform/<os>/components.cmake for this operating system")
endif()
set(ICLFORGE_CRUCIBLE_NOTICES_DIR "${CMAKE_CURRENT_LIST_DIR}")
include("${ICLFORGE_CRUCIBLE_NOTICES_DIR}/platform/${ICLFORGE_CRUCIBLE_PLATFORM_DIR}/components.cmake")

# The room's 3D view: the same fact that adds Room3DView.qml. Its section
# follows the platform's Qt section, whichever that is.
if(Qt6Quick3D_FOUND)
    set(ICLFORGE_CRUCIBLE_NOTICES_INSERT_AT -1)
    set(ICLFORGE_CRUCIBLE_NOTICES_INDEX 0)
    foreach(fragment IN LISTS ICLFORGE_CRUCIBLE_NOTICE_FRAGMENTS)
        math(EXPR ICLFORGE_CRUCIBLE_NOTICES_INDEX "${ICLFORGE_CRUCIBLE_NOTICES_INDEX} + 1")
        if(fragment MATCHES "^qt-" AND ICLFORGE_CRUCIBLE_NOTICES_INSERT_AT EQUAL -1)
            set(ICLFORGE_CRUCIBLE_NOTICES_INSERT_AT ${ICLFORGE_CRUCIBLE_NOTICES_INDEX})
        endif()
    endforeach()
    if(ICLFORGE_CRUCIBLE_NOTICES_INSERT_AT EQUAL -1)
        list(APPEND ICLFORGE_CRUCIBLE_NOTICE_FRAGMENTS qt-quick3d)
    else()
        list(INSERT ICLFORGE_CRUCIBLE_NOTICE_FRAGMENTS ${ICLFORGE_CRUCIBLE_NOTICES_INSERT_AT} qt-quick3d)
    endif()
endif()
if(ICLFORGE_ENABLE_TRACY)
    list(APPEND ICLFORGE_CRUCIBLE_NOTICE_FRAGMENTS tracy)
endif()
# An engine-and-runner build: no window, so no Qt, no fonts, and nothing the
# Qt deployment tool placed beside the executable.
if(NOT Qt6_FOUND)
    list(REMOVE_ITEM ICLFORGE_CRUCIBLE_NOTICE_FRAGMENTS qt-bundled qt-system qt-quick3d fonts windows-runtime)
endif()

# The versions, from what CMake already holds: the kit's (cmake/FindQt6.cmake),
# {fmt}'s from its package or the pinned fallback (cmake/Fmt.cmake), PipeWire's
# from pkg-config (../CMakeLists.txt), Tracy's from its package (cmake/Tracy.cmake).
if(fmt_VERSION)
    set(ICLFORGE_CRUCIBLE_FMT_VERSION "${fmt_VERSION}")
else()
    set(ICLFORGE_CRUCIBLE_FMT_VERSION "${ICLFORGE_FMT_VERSION}")
endif()
if(Tracy_VERSION)
    set(ICLFORGE_CRUCIBLE_TRACY_VERSION "${Tracy_VERSION}")
else()
    set(ICLFORGE_CRUCIBLE_TRACY_VERSION "(version not reported by the Tracy package)")
endif()
string(REGEX MATCH "^[0-9]+\\.[0-9]+" ICLFORGE_CRUCIBLE_QT_SERIES "${Qt6_VERSION}")

set(ICLFORGE_CRUCIBLE_NOTICES_FILE "${CMAKE_CURRENT_BINARY_DIR}/notices/NOTICES.txt")
ac3_generate_notices("${ICLFORGE_CRUCIBLE_NOTICES_FILE}"
    FRAGMENT_DIR "${ICLFORGE_CRUCIBLE_NOTICES_DIR}/fragments"
    FRAGMENTS ${ICLFORGE_CRUCIBLE_NOTICE_FRAGMENTS}
    TOKENS
        "VERSION=${PROJECT_VERSION_FULL}"
        "PLATFORM=${ICLFORGE_CRUCIBLE_NOTICES_PLATFORM}"
        "LOCATION=${ICLFORGE_CRUCIBLE_NOTICES_LOCATION}"
        # Which binaries the shared fmt and fonts fragments are talking about.
        # Those two name no application otherwise, so apps/notices/ (Forge)
        # takes them verbatim through cmake/Notices.cmake's FRAGMENT_DIR search
        # path and passes its own values here - see that module's header. The
        # fonts one stays "The executable" rather than naming crucible: this
        # package has two binaries and only the window carries the faces, and
        # the reader is holding the window.
        "FMT_USERS=crucible and crucible-run"
        "FONT_USER=The executable"
        "QT_VERSION=${Qt6_VERSION}"
        "QT_SERIES=${ICLFORGE_CRUCIBLE_QT_SERIES}"
        # Where this build's Qt sits and how the loader finds it: two
        # sentences of the shared qt-bundled fragment that are the platform's
        # to write, set by platform/<os>/components.cmake. Empty on Linux,
        # whose package bundles no Qt and therefore carries no such fragment -
        # ac3_generate_notices ignores a token no fragment mentions, which is
        # why this is passed unconditionally.
        "QT_PAYLOAD=${ICLFORGE_CRUCIBLE_QT_PAYLOAD}"
        "QT_LOOKUP=${ICLFORGE_CRUCIBLE_QT_LOOKUP}"
        "FMT_VERSION=${ICLFORGE_CRUCIBLE_FMT_VERSION}"
        "PIPEWIRE_VERSION=${ICLFORGE_CRUCIBLE_PIPEWIRE_VERSION}"
        "TRACY_VERSION=${ICLFORGE_CRUCIBLE_TRACY_VERSION}"
        # Named explicitly, not left implicit, now that apps/hearth/notices/notices.cmake and
        # apps/notices/notices.cmake share this fragment too (search their own FRAGMENT_DIR
        # lists) and each has a different answer.
        "TRACY_USERS=crucible and crucible-run"
    FILES
        "LGPL3=${ICLFORGE_CRUCIBLE_NOTICES_DIR}/licences/LGPL-3.0.txt"
        "OFL=${CMAKE_SOURCE_DIR}/apps/gui/fonts/OFL.txt"
        "MSPL=${CMAKE_SOURCE_DIR}/apps/windows/driver/LICENSE"
        "FMT_MIT=${ICLFORGE_CRUCIBLE_NOTICES_DIR}/licences/MIT-fmt.txt"
        "PW_MIT=${ICLFORGE_CRUCIBLE_NOTICES_DIR}/licences/MIT-pipewire.txt"
        "TRACY_BSD=${ICLFORGE_CRUCIBLE_NOTICES_DIR}/licences/BSD-3-Clause-Tracy.txt"
        "MESA_MIT=${ICLFORGE_CRUCIBLE_NOTICES_DIR}/licences/MIT-mesa.txt"
        "DXC_NCSA=${ICLFORGE_CRUCIBLE_NOTICES_DIR}/licences/NCSA-dxc.txt")
message(STATUS "Crucible notices: ${ICLFORGE_CRUCIBLE_NOTICES_PLATFORM} build, sections: ${ICLFORGE_CRUCIBLE_NOTICE_FRAGMENTS}")
