# ---------------------------------------------------------------------------
# ac3hearth - the desktop reference player's window
# (planning/hearth-reference-player.md, A5). Qt is a PREBUILT dependency
# discovered by cmake/FindQt6.cmake, never vcpkg (qt-integration-convention),
# the same as apps/gui and apps/crucible. QUIET, and a warning rather than a
# hard failure, so a core-only or Hearth-engine-only build still configures
# on a machine with no Qt kit - apps/crucible/CMakeLists.txt's own arm is
# the precedent this follows.
#
# included from ../CMakeLists.txt, itself only reached under the root's
# `AC3FORGE_BUILD_HEARTH AND NOT AC3FORGE_SENDSPIN_PLAYER_ONLY` guard - which
# the ESP-IDF component never satisfies (feedback-root-cmake-guards-need-
# esp-idf-build: CONFIG_AC3FORGE_SENDSPIN pre-seeds AC3FORGE_BUILD_HEARTH=ON
# together with AC3FORGE_SENDSPIN_PLAYER_ONLY=ON, and every other board shape
# leaves AC3FORGE_BUILD_HEARTH at its default OFF). So this file is never
# entered by a board configure, and needs no guard of its own for that - see
# this change's PR for the WSL configure-only check that confirmed it.
# ---------------------------------------------------------------------------
find_package(Qt6 6.8 QUIET COMPONENTS Core Gui Qml Quick QuickControls2 LinguistTools)
if(NOT Qt6_FOUND)
    message(WARNING "Qt6 not found: ac3hearth is skipped; the engine and its tests still build.")
else()
    qt_standard_project_setup(REQUIRES 6.8)

    # The shared family components, copied from apps/gui/qml the way
    # apps/crucible/CMakeLists.txt does (see its own comment for why this
    # generates into the source tree, ignored by git, rather than the build
    # tree). Edit apps/gui/qml/*.qml and every app that copies it follows.
    set(AC3HEARTH_SHARED_QML Theme.qml Card.qml RailBlock.qml SegmentedControl.qml FocusRing.qml)
    set(AC3HEARTH_SHARED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/qml/shared")
    file(MAKE_DIRECTORY "${AC3HEARTH_SHARED_DIR}")
    set(AC3HEARTH_SHARED_OUT)
    foreach(name IN LISTS AC3HEARTH_SHARED_QML)
        set(src "${CMAKE_CURRENT_SOURCE_DIR}/../../gui/qml/${name}")
        set(dst "${AC3HEARTH_SHARED_DIR}/${name}")
        file(READ "${src}" contents)
        string(REPLACE "import Ac3Forge\n" "import Ac3ForgeHearth\n" contents "${contents}")
        file(WRITE "${dst}" "${contents}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${src}")
        list(APPEND AC3HEARTH_SHARED_OUT "${dst}")
    endforeach()

    qt_add_executable(ac3hearth
        main.cpp
        hearth_controller.cpp
        hearth_controller.hpp
        item_loader.cpp
        item_loader.hpp)
    target_include_directories(ac3hearth PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../engine")
    target_link_libraries(ac3hearth PRIVATE
        ac3hearth_engine
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2)

    set(AC3HEARTH_QML_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/Main.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/PlayPage.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/PlaceholderPage.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/qml/TransportBar.qml"
        ${AC3HEARTH_SHARED_OUT})
    foreach(qml_file IN LISTS AC3HEARTH_QML_FILES)
        get_filename_component(qml_name "${qml_file}" NAME)
        set_source_files_properties("${qml_file}" PROPERTIES QT_RESOURCE_ALIAS "${qml_name}")
    endforeach()
    set_source_files_properties("${AC3HEARTH_SHARED_DIR}/Theme.qml" PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

    qt_add_qml_module(ac3hearth
        URI Ac3ForgeHearth
        VERSION 1.0
        QML_FILES ${AC3HEARTH_QML_FILES})

    # The family's own faces and application icon, reused rather than
    # duplicated - apps/crucible/CMakeLists.txt does the same for the same
    # reason.
    qt_add_resources(ac3hearth "ac3hearth_fonts"
        PREFIX "/fonts"
        BASE "${CMAKE_SOURCE_DIR}/apps/gui/fonts"
        FILES
            "${CMAKE_SOURCE_DIR}/apps/gui/fonts/Archivo-Regular.ttf"
            "${CMAKE_SOURCE_DIR}/apps/gui/fonts/Archivo-Medium.ttf"
            "${CMAKE_SOURCE_DIR}/apps/gui/fonts/Archivo-SemiBold.ttf"
            "${CMAKE_SOURCE_DIR}/apps/gui/fonts/Archivo-ExtraBold.ttf")
    qt_add_resources(ac3hearth "ac3hearth_icons"
        PREFIX "/icons"
        BASE "${CMAKE_SOURCE_DIR}/apps/gui/icons"
        FILES
            "${CMAKE_SOURCE_DIR}/apps/gui/icons/ac3forge-32.png"
            "${CMAKE_SOURCE_DIR}/apps/gui/icons/ac3forge-256.png")

    set_target_properties(ac3hearth PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    if(WIN32)
        enable_language(RC)
        set(AC3_GUI_ICON_ICO "${CMAKE_SOURCE_DIR}/apps/gui/icons/ac3forge.ico")
        file(TO_CMAKE_PATH "${AC3_GUI_ICON_ICO}" AC3_GUI_ICON_ICO)
        configure_file("${CMAKE_SOURCE_DIR}/apps/gui/icons/ac3gui.rc.in" "${CMAKE_CURRENT_BINARY_DIR}/ac3hearth.rc" @ONLY)
        target_sources(ac3hearth PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/ac3hearth.rc")
        set_target_properties(ac3hearth PROPERTIES WIN32_EXECUTABLE ON)
    elseif(APPLE)
        set(AC3HEARTH_ICON_ICNS "${CMAKE_SOURCE_DIR}/apps/gui/icons/ac3forge.icns")
        set_source_files_properties("${AC3HEARTH_ICON_ICNS}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        target_sources(ac3hearth PRIVATE "${AC3HEARTH_ICON_ICNS}")
        set_target_properties(ac3hearth PROPERTIES MACOSX_BUNDLE ON MACOSX_BUNDLE_BUNDLE_NAME "Hearth"
            MACOSX_BUNDLE_ICON_FILE "ac3forge.icns")
    endif()

    # Qt runtime next to the executable, as apps/gui and apps/crucible do it,
    # so the window runs from the build tree. Windows only - see
    # apps/crucible/CMakeLists.txt's own comment on why a Linux build needs
    # nothing copied.
    if(WIN32)
        get_filename_component(AC3HEARTH_QT_BIN_DIR "${Qt6_DIR}/../../../bin" ABSOLUTE)
        find_program(AC3HEARTH_WINDEPLOYQT NAMES windeployqt6 windeployqt HINTS "${AC3HEARTH_QT_BIN_DIR}")
        if(AC3HEARTH_WINDEPLOYQT)
            add_custom_command(TARGET ac3hearth POST_BUILD
                COMMAND "${AC3HEARTH_WINDEPLOYQT}"
                    --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/qml"
                    --qmldir "${AC3HEARTH_SHARED_DIR}"
                    --compiler-runtime
                    "$<TARGET_FILE:ac3hearth>"
                COMMENT "Deploying Qt runtime dependencies for ac3hearth")
        else()
            message(WARNING "windeployqt not found next to Qt6; ac3hearth.exe will not run standalone from the build tree.")
        endif()
    endif()
endif()
