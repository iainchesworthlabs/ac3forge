# The Windows AC3Forge Crucible (roadmap UX11): docs/platforms/windows-demo.md.
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

add_library(ac3crucible_engine STATIC
    engine/slots.cpp
    engine/bed_mixer.cpp
    engine/placement.cpp
    engine/output_policy.cpp
    engine/tap_pool.cpp
    engine/output_stage.cpp
    engine/signing_hook.cpp
    engine/library_devices.cpp
    engine/diagnostics.cpp
    engine/engine.cpp)
add_library(ac3::crucible_engine ALIAS ac3crucible_engine)
target_include_directories(ac3crucible_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/engine")
target_compile_features(ac3crucible_engine PUBLIC cxx_std_23)
# ac3::coverage is inert unless AC3FORGE_ENABLE_COVERAGE is on
# (config-windows-llvm-coverage); then this library, the window and its
# tests are the instrumented set tools/checks/coverage_crucible.ps1 reports.
target_link_libraries(ac3crucible_engine
    PUBLIC ac3::forge ac3::audio ac3::signing ac3::tracy
    PRIVATE ac3::warnings ac3::coverage)

# The platform half, selected the way apps/cli/CMakeLists.txt selects its
# platform/ directory: a platform is a directory, never a conditional inside
# a source file. Two arms now (roadmap UX12) - Windows over COM and WASAPI,
# Linux over PipeWire. The application talks to PipeWire directly here for
# the same reason it talks to COM directly on Windows: session enumeration,
# the default device and the silent device are demo policy rather than audio
# I/O, and ac3::audio deliberately does not carry them.
if(WIN32)
    target_sources(ac3crucible_engine PRIVATE
        engine/platform/windows/session_monitor.cpp
        engine/platform/windows/default_device.cpp
        engine/platform/windows/driver_tools.cpp
        engine/platform/windows/driver_tools.hpp
        engine/platform/windows/foreground.cpp)
    target_compile_definitions(ac3crucible_engine PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    # COM for the session manager and policy config, the shell for the
    # full-screen query and the Settings fallback.
    target_link_libraries(ac3crucible_engine PRIVATE ole32 shell32 user32 version)
elseif(UNIX AND NOT APPLE)
    # Crucible needs a per-application tap, and on Linux only the PipeWire
    # backend has one - ALSA has no per-application concept at all, which is
    # why its process_loopback capability is a flat no rather than a gap.
    # ac3audio compiles exactly one backend, and ALSA wins when both sets of
    # headers are present (docs/building.md, "Why ALSA still comes first"), so
    # a machine with libasound2-dev installed - a Raspberry Pi, most desktops -
    # would otherwise configure a Crucible that can list applications and never
    # capture one. That is a worse outcome than refusing, so refuse, and say
    # which flags fix it.
    if(NOT AC3FORGE_AUDIO_BACKEND STREQUAL "pipewire")
        message(FATAL_ERROR
            "AC3FORGE_BUILD_CRUCIBLE=ON needs the PipeWire audio backend on Linux, but "
            "ac3audio selected '${AC3FORGE_AUDIO_BACKEND}'. Crucible taps each application "
            "separately and only PipeWire can do that. Reconfigure with "
            "-DAC3FORGE_WITH_ALSA=OFF -DAC3FORGE_WITH_PIPEWIRE=ON, or turn the application "
            "off with -DAC3FORGE_BUILD_CRUCIBLE=OFF. Note that this trades away the "
            "passthrough path confirmed against a real receiver (ALSA iec958) for one that "
            "is not yet - see docs/crucible/promotion.md, \"ALSA or PipeWire\".")
    endif()
    target_sources(ac3crucible_engine PRIVATE
        engine/platform/linux/session_monitor.cpp
        engine/platform/linux/default_device.cpp
        engine/platform/linux/foreground.cpp
        engine/platform/linux/virtual_device.cpp)
    # The library's own PipeWire helpers (for_each_audio_node, the node
    # property readers) are reused rather than reimplemented, so this needs
    # both that private header's directory and libpipewire itself.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(AC3CRUCIBLE_PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
    target_include_directories(ac3crucible_engine
        PRIVATE "${CMAKE_SOURCE_DIR}/src/audio/src/backend/pipewire")
    target_link_libraries(ac3crucible_engine PRIVATE PkgConfig::AC3CRUCIBLE_PIPEWIRE)
    # X11 full-screen detection (docs/crucible/promotion.md, Phase 4's X11
    # addendum). libxcb, not libX11: thread-safe, no global error handler,
    # and already on every machine that runs Qt's xcb platform plugin, so
    # the .deb's shlibdeps add nothing new. Optional the way the audio
    # backends are (src/audio/CMakeLists.txt): AUTO takes it when the
    # headers are there, ON demands them, OFF leaves it out. Without it the
    # Foreground says so at runtime and the rule is off; exactly one of the
    # two readers is compiled, the same rule as a platform directory.
    set(AC3FORGE_CRUCIBLE_X11 "AUTO" CACHE STRING
        "Crucible's X11 full-screen check: AUTO uses libxcb when found, ON requires it, OFF never builds it")
    set_property(CACHE AC3FORGE_CRUCIBLE_X11 PROPERTY STRINGS AUTO ON OFF)
    if(AC3FORGE_CRUCIBLE_X11 STREQUAL "OFF")
        set(AC3CRUCIBLE_XCB_FOUND FALSE)
    elseif(AC3FORGE_CRUCIBLE_X11 STREQUAL "ON")
        pkg_check_modules(AC3CRUCIBLE_XCB REQUIRED IMPORTED_TARGET xcb)
    else()
        pkg_check_modules(AC3CRUCIBLE_XCB QUIET IMPORTED_TARGET xcb)
    endif()
    target_sources(ac3crucible_engine PRIVATE engine/platform/linux/x11_foreground.cpp)
    if(AC3CRUCIBLE_XCB_FOUND)
        target_sources(ac3crucible_engine PRIVATE engine/platform/linux/xcb_window_reader.cpp)
        target_link_libraries(ac3crucible_engine PRIVATE PkgConfig::AC3CRUCIBLE_XCB)
        set(AC3FORGE_CRUCIBLE_X11_BACKEND xcb PARENT_SCOPE)
    else()
        target_sources(ac3crucible_engine PRIVATE engine/platform/linux/no_xcb_window_reader.cpp)
        set(AC3FORGE_CRUCIBLE_X11_BACKEND none PARENT_SCOPE)
        message(STATUS "libxcb not found: Crucible's X11 full-screen check is left out "
                       "(libxcb1-dev on Debian/Ubuntu, libxcb-devel on Fedora; "
                       "-DAC3FORGE_CRUCIBLE_X11=OFF silences this)")
    endif()
endif()
# The profiling header pair, on every platform: the engine loop and the
# controller carry Tracy zones whichever platform half is compiled in. This
# sat inside the Windows arm when there was only one arm, and lived inside
# the Linux arm for a while after the second was added - which is how a
# Windows build came to lose ac3/internal/profiling.hpp.
target_include_directories(ac3crucible_engine PUBLIC
    "$<IF:$<BOOL:${AC3FORGE_ENABLE_TRACY}>,${CMAKE_SOURCE_DIR}/src/forge/src/internal/profiling/tracy_enabled,${CMAKE_SOURCE_DIR}/src/forge/src/internal/profiling/tracy_disabled>")

# The console runner: Phase 2's exit criterion, and the thing the UI replaces.
add_executable(ac3crucible-run runner/main.cpp)
target_link_libraries(ac3crucible-run PRIVATE ac3::crucible_engine ac3::warnings)
target_compile_definitions(ac3crucible-run PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(ac3crucible-run PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# ---------------------------------------------------------------------------
# The window: ac3crucible, Qt Quick, module Ac3ForgeCrucible (Phase 3).
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
# Widgets is for the tray alone - Qt.labs.platform's SystemTrayIcon is
# QSystemTrayIcon underneath - and is what lets it exist on Linux.
find_package(Qt6 6.5 QUIET COMPONENTS Core Gui Widgets Qml Quick QuickControls2 LinguistTools)
if(NOT Qt6_FOUND)
    message(WARNING "Qt6 not found: the AC3Forge Crucible's window (ac3crucible) is skipped; "
                    "the engine and the ac3crucible-run runner still build.")
else()
    qt_standard_project_setup(REQUIRES 6.5)

    # Qt Quick 3D is optional: the room's 3D picture (ui/qml/Room3DView.qml)
    # is built when the kit has it and the page hides its toggle when not.
    # The value reaches the controller as a compile-time constant, not a
    # conditional: CrucibleController::has3D() returns AC3DESK_QUICK3D.
    find_package(Qt6 6.5 QUIET COMPONENTS Quick3D)
    if(Qt6Quick3D_FOUND)
        set(AC3DESK_QUICK3D 1)
        set(AC3DESK_QUICK3D_LIB Qt6::Quick3D)
        set(AC3DESK_QUICK3D_QML "${CMAKE_CURRENT_SOURCE_DIR}/ui/qml/Room3DView.qml")
    else()
        set(AC3DESK_QUICK3D 0)
        set(AC3DESK_QUICK3D_LIB)
        set(AC3DESK_QUICK3D_QML)
        message(STATUS "Qt Quick 3D not found: the AC3Forge Crucible's 3D room is left out")
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
        string(REPLACE "import Ac3Forge\n" "import Ac3ForgeCrucible\n" contents "${contents}")
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
        "${CMAKE_CURRENT_SOURCE_DIR}/ui/qml/CrucibleButton.qml"
        "${CMAKE_CURRENT_SOURCE_DIR}/ui/qml/CrucibleCheck.qml"
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

    qt_add_executable(ac3crucible
        ui/main.cpp
        ui/crucible_controller.cpp
        ui/crucible_controller.hpp
        ../gui/system_theme.cpp
        ../gui/system_theme.hpp
        ../gui/language_manager.cpp
        ../gui/language_manager.hpp
        "${CMAKE_CURRENT_SOURCE_DIR}/ui/app_icon_provider.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/ui/app_entry.hpp"
    )
    # The application-icon provider is the one part of the window that talks
    # to the operating system directly, so it is a platform directory like
    # engine/platform/: Windows asks the shell for an executable's icon,
    # Linux has no single call for that and returns none (the monogram shows
    # instead - see ui/platform/linux/app_icon_provider.cpp for what doing it
    # properly involves).
    if(WIN32)
        target_sources(ac3crucible PRIVATE ui/platform/windows/app_icon_provider.cpp)
    elseif(LINUX)
        target_sources(ac3crucible PRIVATE ui/platform/linux/app_icon_provider.cpp)
    endif()
    qt_add_qml_module(ac3crucible
        URI Ac3ForgeCrucible
        VERSION 1.0
        QML_FILES ${AC3DESK_QML_FILES}
        RESOURCES "${CMAKE_CURRENT_SOURCE_DIR}/ui/qml/tray.svg")
    # ui/ for the generated type registrations to find crucible_controller.hpp,
    # ../gui for system_theme.hpp.
    target_include_directories(ac3crucible PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ui" "${CMAKE_CURRENT_SOURCE_DIR}/../gui")
    # The driver folder in the source tree is the default place the
    # Settings screen's install/remove buttons look when no driver/ folder
    # sits beside the executable (a build-tree run).
    target_compile_definitions(ac3crucible PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
        AC3DESK_DRIVER_SOURCE_DIR="${CMAKE_SOURCE_DIR}/apps/windows/driver"
        AC3DESK_QUICK3D=${AC3DESK_QUICK3D})
    # ac3::warnings is deliberately not linked: Qt's generated sources would
    # be compiled under it (apps/gui carries a workaround for exactly that).
    target_link_libraries(ac3crucible PRIVATE
        ac3::crucible_engine
        ac3::coverage
        Qt6::Core Qt6::Gui Qt6::Qml Qt6::Widgets Qt6::Quick Qt6::QuickControls2 ${AC3DESK_QUICK3D_LIB})
    qt_add_resources(ac3crucible "ac3crucible_icons"
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
    qt_add_resources(ac3crucible "ac3crucible_fonts"
        PREFIX "/fonts"
        BASE "${CMAKE_SOURCE_DIR}/apps/gui/fonts"
        FILES
            "${CMAKE_SOURCE_DIR}/apps/gui/fonts/Archivo-Regular.ttf"
            "${CMAKE_SOURCE_DIR}/apps/gui/fonts/Archivo-Medium.ttf"
            "${CMAKE_SOURCE_DIR}/apps/gui/fonts/Archivo-SemiBold.ttf"
            "${CMAKE_SOURCE_DIR}/apps/gui/fonts/Archivo-ExtraBold.ttf"
            "${CMAKE_SOURCE_DIR}/apps/gui/fonts/NotoSansArabic.ttf"
            "${CMAKE_SOURCE_DIR}/apps/gui/fonts/NotoSansHebrew.ttf")
    set_target_properties(ac3crucible PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
    # The executable's own icon, per platform, the same three-way split as
    # apps/gui/CMakeLists.txt. Windows: a resource script compiled in (RC has
    # to be enabled by hand under Ninja). macOS: the .icns copied into the
    # bundle. Linux: nothing here - the hicolor theme install below is how a
    # freedesktop menu finds it.
    if(WIN32)
        enable_language(RC)
        set(AC3_GUI_ICON_ICO "${CMAKE_SOURCE_DIR}/apps/gui/icons/ac3forge.ico")
        file(TO_CMAKE_PATH "${AC3_GUI_ICON_ICO}" AC3_GUI_ICON_ICO)
        configure_file("${CMAKE_SOURCE_DIR}/apps/gui/icons/ac3gui.rc.in" "${CMAKE_CURRENT_BINARY_DIR}/ac3crucible.rc" @ONLY)
        target_sources(ac3crucible PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/ac3crucible.rc")
        set_target_properties(ac3crucible PROPERTIES WIN32_EXECUTABLE ON)
    elseif(APPLE)
        set(AC3CRUCIBLE_ICON_ICNS "${CMAKE_SOURCE_DIR}/apps/gui/icons/ac3forge.icns")
        set_source_files_properties("${AC3CRUCIBLE_ICON_ICNS}" PROPERTIES
            MACOSX_PACKAGE_LOCATION Resources)
        target_sources(ac3crucible PRIVATE "${AC3CRUCIBLE_ICON_ICNS}")
        set_target_properties(ac3crucible PROPERTIES
            MACOSX_BUNDLE ON
            MACOSX_BUNDLE_BUNDLE_NAME "Crucible"
            MACOSX_BUNDLE_ICON_FILE "ac3forge.icns")
    endif()

    # The same language set as the GUI (roadmap UX3), the same pipeline:
    # lupdate scans this target's QML and C++, lrelease compiles the .ts
    # into :/i18n/ac3crucible_<code>.qm, which the shared LanguageManager loads
    # (main.cpp passes it the "ac3crucible" basename). Regenerate with
    # `cmake --build <dir> --target ac3crucible_lupdate`; the translations are
    # mechanical for now (docs/gui/localisation.md).
    set(AC3DESK_TS_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/translations/ac3crucible_fr.ts"
        "${CMAKE_CURRENT_SOURCE_DIR}/translations/ac3crucible_de.ts"
        "${CMAKE_CURRENT_SOURCE_DIR}/translations/ac3crucible_es.ts"
        "${CMAKE_CURRENT_SOURCE_DIR}/translations/ac3crucible_ar.ts"
        "${CMAKE_CURRENT_SOURCE_DIR}/translations/ac3crucible_he.ts"
        "${CMAKE_CURRENT_SOURCE_DIR}/translations/ac3crucible_yi.ts")
    qt_add_translations(ac3crucible
        IMMEDIATE_CALL
        SOURCE_TARGETS ac3crucible
        TS_FILES ${AC3DESK_TS_FILES}
        LUPDATE_TARGET ac3crucible_lupdate
        LRELEASE_TARGET ac3crucible_lrelease)

    # Qt runtime next to the executable, as apps/gui does it, so the window
    # runs from the build tree. Windows only: a Linux build finds Qt through
    # the system's own loader and needs nothing copied, and a warning that
    # "ac3crucible.exe will not run standalone" there would be wrong twice.
    if(WIN32)
        get_filename_component(AC3DESK_QT_BIN_DIR "${Qt6_DIR}/../../../bin" ABSOLUTE)
        find_program(AC3DESK_WINDEPLOYQT NAMES windeployqt6 windeployqt HINTS "${AC3DESK_QT_BIN_DIR}")
        if(AC3DESK_WINDEPLOYQT)
            add_custom_command(TARGET ac3crucible POST_BUILD
                COMMAND "${AC3DESK_WINDEPLOYQT}"
                    --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/ui/qml"
                    --qmldir "${AC3DESK_SHARED_DIR}"
                    --compiler-runtime
                    "$<TARGET_FILE:ac3crucible>"
                COMMENT "Deploying Qt runtime dependencies for ac3crucible")
        else()
            message(WARNING "windeployqt not found next to Qt6; ac3crucible.exe will not run standalone from the build tree.")
        endif()
    endif()

    # Four windeployqt passes share bin/: ac3gui's, ac3gui_qmltests's (ordered
    # after it, apps/gui/tests/CMakeLists.txt says why), this one, and
    # ac3crucible_qmltests's (ordered after this one, below). The GUI's pair and
    # the demo's pair were not ordered against each other, and on a real
    # "build everything" run two of them copying the same Qt file into bin/
    # at once fail with "Cannot create a file when that file already
    # exists" - seen on both Windows CI legs the day the demo joined them,
    # each time the other application losing. This chains the demo's pair
    # after whichever of the GUI's is last, so the four run one at a time.
    if(TARGET ac3gui_qmltests)
        add_dependencies(ac3crucible ac3gui_qmltests)
    elseif(TARGET ac3gui)
        add_dependencies(ac3crucible ac3gui)
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
    # targets the build tree). COMPONENT crucible, not runtime: the demo is
    # its own download rather than something an ac3cli/ac3gui user is given
    # (cmake/Packaging.cmake).
    install(TARGETS ac3crucible RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT crucible)
    # Only where the package carries its own Qt. On Linux the system's Qt is
    # the one the binary was built against and the loader finds it, so the
    # package ships none (docs/releasing.md); left to run there, this script
    # copies the distribution's QML plugins into the package and then fails
    # to rewrite an RPATH they never had - which is how the first cpack on
    # the Raspberry Pi died.
    if(WIN32 OR APPLE)
        qt_generate_deploy_qml_app_script(
            TARGET ac3crucible
            OUTPUT_SCRIPT AC3DESK_DEPLOY_SCRIPT
            NO_UNSUPPORTED_PLATFORM_ERROR)
        install(SCRIPT "${AC3DESK_DEPLOY_SCRIPT}" COMPONENT crucible)
    endif()
endif()

# ---------------------------------------------------------------------------
# Install (CPack component "crucible", cmake/Packaging.cmake).
#
# The driver's own scripts travel with the application because
# CrucibleController::driverDir() looks for install.ps1 in a "driver" directory
# beside the executable before it falls back to the source tree: a packaged
# copy then points its Settings page at scripts that are actually there.
# The driver BINARY is deliberately absent - it is test-signed only, so it
# would not load on a normal machine; shipping it waits on the EV
# certificate and attestation signing (docs/platforms/windows-demo.md,
# "Phase 6"), after which the installer installs it and removes it on
# uninstall rather than the Settings page asking anyone to.
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS ac3crucible-run RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT crucible)
if(WIN32)
    install(FILES
            "${CMAKE_SOURCE_DIR}/apps/windows/driver/install.ps1"
            "${CMAKE_SOURCE_DIR}/apps/windows/driver/remove.ps1"
            "${CMAKE_SOURCE_DIR}/apps/windows/driver/NullSinkDevice.ps1"
        DESTINATION "${CMAKE_INSTALL_BINDIR}/driver" COMPONENT crucible)
elseif(LINUX)
    # What a freedesktop menu needs: the launcher, the AppStream record a
    # software centre reads, and the icon in the hicolor theme at the two
    # sizes the window itself embeds. Same layout apps/gui installs under
    # `runtime`; this is `crucible` because the application is its own
    # package (cmake/Packaging.cmake). No driver scripts: Linux needs no
    # driver, and a PowerShell script in a .deb is a mistake waiting for a
    # reader.
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/ac3crucible.desktop"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications" COMPONENT crucible)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/ac3crucible.metainfo.xml"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo" COMPONENT crucible)
    install(FILES "${CMAKE_SOURCE_DIR}/apps/gui/icons/ac3forge-256.png"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps"
        RENAME "ac3crucible.png" COMPONENT crucible)
    install(FILES "${CMAKE_SOURCE_DIR}/apps/gui/icons/ac3forge-32.png"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/32x32/apps"
        RENAME "ac3crucible.png" COMPONENT crucible)
endif()
