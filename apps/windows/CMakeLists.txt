# The Windows Desktop Atmos Demo (roadmap UX11): docs/platforms/windows-demo.md.
#
# engine/ is the headless part - what the UI drives and the spikes proved -
# split in two the way the CLI's own platform/ directory is: the pure modules
# (slot plan, bed mixer, placement, output policy) have no Windows in them and
# are ALSO compiled into ac3tests on every platform (tests/CMakeLists.txt);
# the modules under engine/platform/windows/ talk to the session manager, the
# policy-config interface and the shell, and only ever build here. The rest
# of engine/ (taps, output stage, signing hook, the frame loop) is written
# against ac3::audio and ac3::signing and would compile anywhere, but only
# makes sense here, so it rides the same option.

add_library(ac3windemo_engine STATIC
    engine/slots.cpp
    engine/bed_mixer.cpp
    engine/placement.cpp
    engine/output_policy.cpp
    engine/tap_pool.cpp
    engine/output_stage.cpp
    engine/signing_hook.cpp
    engine/engine.cpp)
add_library(ac3::windemo_engine ALIAS ac3windemo_engine)
target_include_directories(ac3windemo_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/engine")
target_compile_features(ac3windemo_engine PUBLIC cxx_std_23)
# ac3::coverage is inert unless AC3FORGE_ENABLE_COVERAGE is on
# (config-windows-llvm-coverage); then this library, the window and its
# tests are the instrumented set tools/checks/coverage_windemo.ps1 reports.
target_link_libraries(ac3windemo_engine
    PUBLIC ac3::forge ac3::audio ac3::signing ac3::tracy
    PRIVATE ac3::warnings ac3::coverage)

# The platform half, selected the way apps/cli/CMakeLists.txt selects its
# platform/ directory. This whole directory is already WIN32-only (the root
# CMakeLists.txt's guard), so the branch has one arm today; it is a branch so
# that a second platform is a directory, not a conditional in a source file.
if(WIN32)
    target_sources(ac3windemo_engine PRIVATE
        engine/platform/windows/session_monitor.cpp
        engine/platform/windows/default_device.cpp
        engine/platform/windows/driver_tools.cpp
        engine/platform/windows/driver_tools.hpp
        engine/platform/windows/wasapi_devices.cpp
        engine/platform/windows/foreground.cpp)
    target_compile_definitions(ac3windemo_engine PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    # COM for the session manager and policy config, the shell for the
    # full-screen query and the Settings fallback.
    target_link_libraries(ac3windemo_engine PRIVATE ole32 shell32 user32 version)
target_include_directories(ac3windemo_engine PUBLIC
    "$<IF:$<BOOL:${AC3FORGE_ENABLE_TRACY}>,${CMAKE_SOURCE_DIR}/src/forge/src/internal/profiling/tracy_enabled,${CMAKE_SOURCE_DIR}/src/forge/src/internal/profiling/tracy_disabled>")
endif()

# The console runner: Phase 2's exit criterion, and the thing the UI replaces.
add_executable(ac3windemo runner/main.cpp)
target_link_libraries(ac3windemo PRIVATE ac3::windemo_engine ac3::warnings)
target_compile_definitions(ac3windemo PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(ac3windemo PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ---------------------------------------------------------------------------
# The window: ac3desk, Qt Quick, module Ac3ForgeDesk (Phase 3).
#
# Qt is a prebuilt kit found the way apps/gui finds it (cmake/FindQt6.cmake).
# It is optional here so that a build with the engine and runner but no Qt
# kit still configures: the UI is simply skipped, with a warning.
#
# Theme, Card, RailBlock and SegmentedControl are the GUI's own files, not
# copies: each is read at configure time, its `import Ac3Forge` rewritten to
# this module's URI, and the result written to an ignored directory under
# ui/qml/ and registered here. Edit apps/gui/qml/*.qml and both apps follow. The Theme
# needs the GUI's SystemTheme singleton (the desktop accent colour), so that
# C++ is compiled into this target too, where QML_ELEMENT registers it in
# this module.
# ---------------------------------------------------------------------------
find_package(Qt6 6.5 QUIET COMPONENTS Core Gui Qml Quick QuickControls2 LinguistTools)
if(NOT Qt6_FOUND)
    message(WARNING "Qt6 not found: the Desktop Atmos Demo's window (ac3desk) is skipped; "
                    "the engine and the ac3windemo runner still build.")
else()
    qt_standard_project_setup(REQUIRES 6.5)

    # Qt Quick 3D is optional: the room's 3D picture (ui/qml/Room3DView.qml)
    # is built when the kit has it and the page hides its toggle when not.
    # The value reaches the controller as a compile-time constant, not a
    # conditional: DeskController::has3D() returns AC3DESK_QUICK3D.
    find_package(Qt6 6.5 QUIET COMPONENTS Quick3D)
    if(Qt6Quick3D_FOUND)
        set(AC3DESK_QUICK3D 1)
        set(AC3DESK_QUICK3D_LIB Qt6::Quick3D)
        set(AC3DESK_QUICK3D_QML "${CMAKE_CURRENT_SOURCE_DIR}/ui/qml/Room3DView.qml")
    else()
        set(AC3DESK_QUICK3D 0)
        set(AC3DESK_QUICK3D_LIB)
        set(AC3DESK_QUICK3D_QML)
        message(STATUS "Qt Quick 3D not found: the Desktop Atmos Demo's 3D room is left out")
    endif()

    set(AC3DESK_SHARED_QML Theme.qml Card.qml RailBlock.qml SegmentedControl.qml)
    # Generated INTO the source tree (ignored by git, see ui/qml/shared/.gitignore)
    # rather than the build tree: the QML ahead-of-time compiler names its
    # cache files after the path relative to the source directory, and a
    # build-tree path produced an unusable name with a drive letter in the
    # middle of it.
    set(AC3DESK_SHARED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ui/qml/shared")
    file(MAKE_DIRECTORY "${AC3DESK_SHARED_DIR}")
    set(AC3DESK_SHARED_OUT)
    foreach(name IN LISTS AC3DESK_SHARED_QML)
        set(src "${CMAKE_CURRENT_SOURCE_DIR}/../gui/qml/${name}")
        set(dst "${AC3DESK_SHARED_DIR}/${name}")
        file(READ "${src}" contents)
        string(REPLACE "import Ac3Forge\n" "import Ac3ForgeDesk\n" contents "${contents}")
        file(WRITE "${dst}" "${contents}")
        # Re-run configure when the GUI's file changes, so the rewrite follows it.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${src}")
        list(APPEND AC3DESK_SHARED_OUT "${dst}")
    endforeach()

    set(AC3DESK_QML_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/ui/qml/Main.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/ui/qml/RoomPage.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/ui/qml/RoomView.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/ui/qml/AppRow.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/ui/qml/BedChip.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/ui/qml/Monogram.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/ui/qml/AboutDialog.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/ui/qml/AppIcon.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/ui/qml/DeskButton.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/ui/qml/DeskCheck.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/ui/qml/OutputPage.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/ui/qml/SignalPath.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/ui/qml/SettingsPage.qml"
        ${AC3DESK_QUICK3D_QML}
        ${AC3DESK_SHARED_OUT})
    foreach(qml_file IN LISTS AC3DESK_QML_FILES)
        get_filename_component(qml_name "${qml_file}" NAME)
        set_source_files_properties("${qml_file}" PROPERTIES QT_RESOURCE_ALIAS "${qml_name}")
    endforeach()
    set_source_files_properties("${AC3DESK_SHARED_DIR}/Theme.qml"
        PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/ui/qml/tray.svg"
        PROPERTIES QT_RESOURCE_ALIAS "tray.svg")

    qt_add_executable(ac3desk
        ui/main.cpp
        ui/desk_controller.cpp
        ui/desk_controller.hpp
        ../gui/system_theme.cpp
        ../gui/system_theme.hpp
        ../gui/language_manager.cpp
        ../gui/language_manager.hpp
        "${CMAKE_CURRENT_SOURCE_DIR}/ui/app_icon_provider.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/ui/app_icon_provider.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/ui/app_entry.hpp"
    )
    qt_add_qml_module(ac3desk
        URI Ac3ForgeDesk
        VERSION 1.0
        QML_FILES ${AC3DESK_QML_FILES}
        RESOURCES "${CMAKE_CURRENT_SOURCE_DIR}/ui/qml/tray.svg")
    # ui/ for the generated type registrations to find desk_controller.hpp,
    # ../gui for system_theme.hpp.
    target_include_directories(ac3desk PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ui" "${CMAKE_CURRENT_SOURCE_DIR}/../gui")
    # The driver folder in the source tree is the default place the
    # Settings screen's install/remove buttons look when no driver/ folder
    # sits beside the executable (a build-tree run).
    target_compile_definitions(ac3desk PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
        AC3DESK_DRIVER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/driver"
        AC3DESK_QUICK3D=${AC3DESK_QUICK3D})
    # ac3::warnings is deliberately not linked: Qt's generated sources would
    # be compiled under it (apps/gui carries a workaround for exactly that).
    target_link_libraries(ac3desk PRIVATE
        ac3::windemo_engine
        ac3::coverage
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 ${AC3DESK_QUICK3D_LIB})
    qt_add_resources(ac3desk "ac3desk_icons"
        PREFIX "/icons"
        BASE "${CMAKE_SOURCE_DIR}/apps/gui/icons"
        FILES
            "${CMAKE_SOURCE_DIR}/apps/gui/icons/ac3forge-32.png"
            "${CMAKE_SOURCE_DIR}/apps/gui/icons/ac3forge-256.png")
    # The GUI app's faces, so this window renders as designed on a machine
    # that never installed Archivo, and so Arabic, Hebrew and Yiddish have
    # glyphs (the shared LanguageManager selects the Noto faces by name).
    # SIL OFL 1.1, attributed in the About box; the licence texts ship
    # beside the files.
    qt_add_resources(ac3desk "ac3desk_fonts"
        PREFIX "/fonts"
        BASE "${CMAKE_SOURCE_DIR}/apps/gui/fonts"
        FILES
            "${CMAKE_SOURCE_DIR}/apps/gui/fonts/Archivo-Regular.ttf"
            "${CMAKE_SOURCE_DIR}/apps/gui/fonts/Archivo-Medium.ttf"
            "${CMAKE_SOURCE_DIR}/apps/gui/fonts/Archivo-SemiBold.ttf"
            "${CMAKE_SOURCE_DIR}/apps/gui/fonts/Archivo-ExtraBold.ttf"
            "${CMAKE_SOURCE_DIR}/apps/gui/fonts/NotoSansArabic.ttf"
            "${CMAKE_SOURCE_DIR}/apps/gui/fonts/NotoSansHebrew.ttf")
    # The .exe's own icon (Explorer, the taskbar before the window shows):
    # the GUI app's resource script, configured with this target's name.
    set(AC3_GUI_ICON_ICO "${CMAKE_SOURCE_DIR}/apps/gui/icons/ac3forge.ico")
    file(TO_CMAKE_PATH "${AC3_GUI_ICON_ICO}" AC3_GUI_ICON_ICO)
    configure_file("${CMAKE_SOURCE_DIR}/apps/gui/icons/ac3gui.rc.in" "${CMAKE_CURRENT_BINARY_DIR}/ac3desk.rc" @ONLY)
    target_sources(ac3desk PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/ac3desk.rc")
    set_target_properties(ac3desk PROPERTIES
        WIN32_EXECUTABLE ON
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

    # The same language set as the GUI (roadmap UX3), the same pipeline:
    # lupdate scans this target's QML and C++, lrelease compiles the .ts
    # into :/i18n/ac3desk_<code>.qm, which the shared LanguageManager loads
    # (main.cpp passes it the "ac3desk" basename). Regenerate with
    # `cmake --build <dir> --target ac3desk_lupdate`; the translations are
    # mechanical for now (docs/gui/localisation.md).
    set(AC3DESK_TS_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/translations/ac3desk_fr.ts"
        "${CMAKE_CURRENT_SOURCE_DIR}/translations/ac3desk_de.ts"
        "${CMAKE_CURRENT_SOURCE_DIR}/translations/ac3desk_es.ts"
        "${CMAKE_CURRENT_SOURCE_DIR}/translations/ac3desk_ar.ts"
        "${CMAKE_CURRENT_SOURCE_DIR}/translations/ac3desk_he.ts"
        "${CMAKE_CURRENT_SOURCE_DIR}/translations/ac3desk_yi.ts")
    qt_add_translations(ac3desk
        IMMEDIATE_CALL
        SOURCE_TARGETS ac3desk
        TS_FILES ${AC3DESK_TS_FILES}
        LUPDATE_TARGET ac3desk_lupdate
        LRELEASE_TARGET ac3desk_lrelease)

    # Qt runtime next to the executable, as apps/gui does it, so the demo
    # runs from the build tree.
    get_filename_component(AC3DESK_QT_BIN_DIR "${Qt6_DIR}/../../../bin" ABSOLUTE)
    find_program(AC3DESK_WINDEPLOYQT NAMES windeployqt6 windeployqt HINTS "${AC3DESK_QT_BIN_DIR}")
    if(AC3DESK_WINDEPLOYQT)
        add_custom_command(TARGET ac3desk POST_BUILD
            COMMAND "${AC3DESK_WINDEPLOYQT}"
                --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/ui/qml"
                --qmldir "${AC3DESK_SHARED_DIR}"
                --compiler-runtime
                "$<TARGET_FILE:ac3desk>"
            COMMENT "Deploying Qt runtime dependencies for ac3desk")
    else()
        message(WARNING "windeployqt not found next to Qt6; ac3desk.exe will not run standalone from the build tree.")
    endif()

    # Four windeployqt passes share bin/: ac3gui's, ac3gui_qmltests's (ordered
    # after it, apps/gui/tests/CMakeLists.txt says why), this one, and
    # ac3desk_qmltests's (ordered after this one, below). The GUI's pair and
    # the demo's pair were not ordered against each other, and on a real
    # "build everything" run two of them copying the same Qt file into bin/
    # at once fail with "Cannot create a file when that file already
    # exists" - seen on both Windows CI legs the day the demo joined them,
    # each time the other application losing. This chains the demo's pair
    # after whichever of the GUI's is last, so the four run one at a time.
    if(TARGET ac3gui_qmltests)
        add_dependencies(ac3desk ac3gui_qmltests)
    elseif(TARGET ac3gui)
        add_dependencies(ac3desk ac3gui)
    endif()

    # The window's Qt Quick Test suites (ui/tests), include()d rather than
    # add_subdirectory()'d so they see AC3DESK_QML_FILES and AC3DESK_TS_FILES
    # as this scope set them.
    if(AC3FORGE_BUILD_TESTS)
        include(ui/tests/CMakeLists.txt)
    endif()

    # Install + Qt runtime deployment for packaging, the shape apps/gui uses
    # (see its own header for why the _qml_ variant is the one that works and
    # why this is separate from the POST_BUILD windeployqt above, which
    # targets the build tree). COMPONENT windemo, not runtime: the demo is
    # its own download rather than something an ac3cli/ac3gui user is given
    # (cmake/Packaging.cmake).
    qt_generate_deploy_qml_app_script(
        TARGET ac3desk
        OUTPUT_SCRIPT AC3DESK_DEPLOY_SCRIPT
        NO_UNSUPPORTED_PLATFORM_ERROR)
    install(TARGETS ac3desk RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT windemo)
    install(SCRIPT "${AC3DESK_DEPLOY_SCRIPT}" COMPONENT windemo)
endif()

# ---------------------------------------------------------------------------
# Install (CPack component "windemo", cmake/Packaging.cmake).
#
# The driver's own scripts travel with the application because
# DeskController::driverDir() looks for install.ps1 in a "driver" directory
# beside the executable before it falls back to the source tree: a packaged
# copy then points its Settings page at scripts that are actually there.
# The driver BINARY is deliberately absent - it is test-signed only, so it
# would not load on a normal machine; shipping it waits on the EV
# certificate and attestation signing (docs/platforms/windows-demo.md,
# "Phase 6"), after which the installer installs it and removes it on
# uninstall rather than the Settings page asking anyone to.
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS ac3windemo RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT windemo)
install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/driver/install.ps1"
        "${CMAKE_CURRENT_SOURCE_DIR}/driver/remove.ps1"
    DESTINATION "${CMAKE_INSTALL_BINDIR}/driver" COMPONENT windemo)
