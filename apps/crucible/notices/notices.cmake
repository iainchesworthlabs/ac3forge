# ---------------------------------------------------------------------------
# This build's NOTICES.txt (docs/crucible/promotion.md, Phase 6).
#
# include()d from ../CMakeLists.txt once the two build facts that change the
# file are known - whether a Qt kit was found, and whether it has Quick 3D -
# and before anything reads AC3CRUCIBLE_NOTICES_FILE: the resource embeddings
# (the window's and the test binary's) and the install rules.
#
# The platform is a directory, platform/<os>/components.cmake, chosen by
# AC3CRUCIBLE_PLATFORM_DIR, which the engine's platform arms set - the same
# rule as engine/platform/ and ui/platform/: no fragment, QML or C++ file
# tests the operating system. The two fragments inserted here depend on a
# build option, not a platform, and are gated by the same variables that
# already gate Room3DView.qml and ac3::tracy.
# ---------------------------------------------------------------------------
include(Notices)

if(NOT AC3CRUCIBLE_PLATFORM_DIR)
    message(FATAL_ERROR
        "notices: AC3CRUCIBLE_PLATFORM_DIR is not set, so there is no "
        "notices/platform/<os>/components.cmake for this operating system")
endif()
set(AC3CRUCIBLE_NOTICES_DIR "${CMAKE_CURRENT_LIST_DIR}")
include("${AC3CRUCIBLE_NOTICES_DIR}/platform/${AC3CRUCIBLE_PLATFORM_DIR}/components.cmake")

# The room's 3D view: the same fact that adds Room3DView.qml. Its section
# follows the platform's Qt section, whichever that is.
if(Qt6Quick3D_FOUND)
    set(AC3CRUCIBLE_NOTICES_INSERT_AT -1)
    set(AC3CRUCIBLE_NOTICES_INDEX 0)
    foreach(fragment IN LISTS AC3CRUCIBLE_NOTICE_FRAGMENTS)
        math(EXPR AC3CRUCIBLE_NOTICES_INDEX "${AC3CRUCIBLE_NOTICES_INDEX} + 1")
        if(fragment MATCHES "^qt-" AND AC3CRUCIBLE_NOTICES_INSERT_AT EQUAL -1)
            set(AC3CRUCIBLE_NOTICES_INSERT_AT ${AC3CRUCIBLE_NOTICES_INDEX})
        endif()
    endforeach()
    if(AC3CRUCIBLE_NOTICES_INSERT_AT EQUAL -1)
        list(APPEND AC3CRUCIBLE_NOTICE_FRAGMENTS qt-quick3d)
    else()
        list(INSERT AC3CRUCIBLE_NOTICE_FRAGMENTS ${AC3CRUCIBLE_NOTICES_INSERT_AT} qt-quick3d)
    endif()
endif()
if(AC3FORGE_ENABLE_TRACY)
    list(APPEND AC3CRUCIBLE_NOTICE_FRAGMENTS tracy)
endif()
# An engine-and-runner build: no window, so no Qt, no fonts, and nothing the
# Qt deployment tool placed beside the executable.
if(NOT Qt6_FOUND)
    list(REMOVE_ITEM AC3CRUCIBLE_NOTICE_FRAGMENTS qt-bundled qt-system qt-quick3d fonts windows-runtime)
endif()

# The versions, from what CMake already holds: the kit's (cmake/FindQt6.cmake),
# {fmt}'s from its package or the pinned fallback (cmake/Fmt.cmake), PipeWire's
# from pkg-config (../CMakeLists.txt), Tracy's from its package (cmake/Tracy.cmake).
if(fmt_VERSION)
    set(AC3CRUCIBLE_FMT_VERSION "${fmt_VERSION}")
else()
    set(AC3CRUCIBLE_FMT_VERSION "${AC3FORGE_FMT_VERSION}")
endif()
if(Tracy_VERSION)
    set(AC3CRUCIBLE_TRACY_VERSION "${Tracy_VERSION}")
else()
    set(AC3CRUCIBLE_TRACY_VERSION "(version not reported by the Tracy package)")
endif()
string(REGEX MATCH "^[0-9]+\\.[0-9]+" AC3CRUCIBLE_QT_SERIES "${Qt6_VERSION}")

set(AC3CRUCIBLE_NOTICES_FILE "${CMAKE_CURRENT_BINARY_DIR}/notices/NOTICES.txt")
ac3_generate_notices("${AC3CRUCIBLE_NOTICES_FILE}"
    FRAGMENT_DIR "${AC3CRUCIBLE_NOTICES_DIR}/fragments"
    FRAGMENTS ${AC3CRUCIBLE_NOTICE_FRAGMENTS}
    TOKENS
        "VERSION=${PROJECT_VERSION_FULL}"
        "PLATFORM=${AC3CRUCIBLE_NOTICES_PLATFORM}"
        "LOCATION=${AC3CRUCIBLE_NOTICES_LOCATION}"
        "QT_VERSION=${Qt6_VERSION}"
        "QT_SERIES=${AC3CRUCIBLE_QT_SERIES}"
        "FMT_VERSION=${AC3CRUCIBLE_FMT_VERSION}"
        "PIPEWIRE_VERSION=${AC3CRUCIBLE_PIPEWIRE_VERSION}"
        "TRACY_VERSION=${AC3CRUCIBLE_TRACY_VERSION}"
    FILES
        "LGPL3=${AC3CRUCIBLE_NOTICES_DIR}/licences/LGPL-3.0.txt"
        "OFL=${CMAKE_SOURCE_DIR}/apps/gui/fonts/OFL.txt"
        "MSPL=${CMAKE_SOURCE_DIR}/apps/windows/driver/LICENSE"
        "FMT_MIT=${AC3CRUCIBLE_NOTICES_DIR}/licences/MIT-fmt.txt"
        "PW_MIT=${AC3CRUCIBLE_NOTICES_DIR}/licences/MIT-pipewire.txt"
        "TRACY_BSD=${AC3CRUCIBLE_NOTICES_DIR}/licences/BSD-3-Clause-Tracy.txt"
        "MESA_MIT=${AC3CRUCIBLE_NOTICES_DIR}/licences/MIT-mesa.txt"
        "DXC_NCSA=${AC3CRUCIBLE_NOTICES_DIR}/licences/NCSA-dxc.txt")
message(STATUS "Crucible notices: ${AC3CRUCIBLE_NOTICES_PLATFORM} build, sections: ${AC3CRUCIBLE_NOTICE_FRAGMENTS}")
