# ---------------------------------------------------------------------------
# crucible_qmltests - Qt Quick Test coverage for the Crucible's
# window (ui/qml), driving the real CrucibleController the way forge_gui_qmltests
# drives the real EncoderController (apps/gui/tests/CMakeLists.txt, whose
# reasoning this file follows and does not repeat).
#
# include()d from ../../CMakeLists.txt inside its Qt6_FOUND block, after
# crucible is defined, so paths are relative to apps/windows and the
# ICLFORGE_CRUCIBLE_QML_FILES / ICLFORGE_CRUCIBLE_TS_FILES lists are the ones crucible itself
# embeds: the two targets cannot list different files without both being
# edited. Second, independent embedding of the Crucible module, not a
# shared library, for the WHOLE_ARCHIVE reason the GUI gives.
# ---------------------------------------------------------------------------
find_package(Qt6 6.8 REQUIRED COMPONENTS QuickTest)

qt_add_executable(crucible_qmltests
    ui/tests/qml_test_main.cpp
    ui/crucible_controller.cpp
    ui/crucible_controller.hpp
    ui/app_entry.hpp
    ui/app_icon_provider.hpp
    ui/tray_support.hpp
    ../gui/system_theme.cpp
    ../gui/system_theme.hpp
    ../gui/language_manager.cpp
    ../gui/language_manager.hpp)
# The icon provider and the tray answer are per platform, the same way the
# window's own target selects them (apps/crucible/CMakeLists.txt): this target
# embeds the same sources and has to make the same choice. Every file listed
# under ui/platform/ there belongs here too - the controller is compiled into
# this binary, so a seam this target does not choose is an unresolved symbol
# at link time and nothing sooner.
#
# The macOS arm carries the OBJCXX standard and the AppKit link with it, for
# the same reason the window's does: ui/platform/macos/app_icon_provider.mm is
# Objective-C++, and this is a second, independent embedding rather than a
# shared library, so nothing is inherited from the other target.
if(WIN32)
    target_sources(crucible_qmltests PRIVATE ui/platform/windows/app_icon_provider.cpp
                                                ui/platform/windows/tray_support.cpp)
elseif(LINUX)
    target_sources(crucible_qmltests PRIVATE ui/platform/linux/app_icon_provider.cpp
                                                ui/platform/linux/tray_support.cpp)
    target_sources(crucible_qmltests PRIVATE ui/desktop_entries.cpp ui/desktop_entries.hpp)
elseif(APPLE)
    target_sources(crucible_qmltests PRIVATE ui/platform/macos/app_icon_provider.mm
                                                ui/platform/macos/tray_support.cpp)
    set_target_properties(crucible_qmltests PROPERTIES
        OBJCXX_STANDARD 23
        OBJCXX_STANDARD_REQUIRED ON
        OBJCXX_EXTENSIONS OFF)
    # The same AppKit-versus-Qt macro collision the window's own arm names
    # (apps/crucible/CMakeLists.txt's APPLE arms): this target compiles the
    # same .mm, so it needs the same define, and nothing is inherited.
    target_compile_definitions(crucible_qmltests
        PRIVATE __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES=0)
    target_link_libraries(crucible_qmltests PRIVATE "-framework AppKit" "-framework CoreGraphics")
endif()

qt_add_qml_module(crucible_qmltests
    URI Crucible
    VERSION 1.0
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests/Crucible"
    QML_FILES ${ICLFORGE_CRUCIBLE_QML_FILES}
    RESOURCES "${CMAKE_CURRENT_SOURCE_DIR}/ui/qml/tray.svg")

# The notices resource too: CrucibleController::licenceNotices() reads
# :/notices/NOTICES.txt, and tst_about.qml asserts what this build's copy
# says, so this binary embeds the same file crucible does. Without it the
# property is the fallback sentence and that suite fails, by design.
qt6_add_resources(crucible_qmltests "crucible_qmltests_notices"
    PREFIX "/notices"
    BASE "${CMAKE_CURRENT_BINARY_DIR}/notices"
    FILES "${ICLFORGE_CRUCIBLE_NOTICES_FILE}")

# tests/crucible for the header-only fakes the scripted-machine singleton
# builds (ui/tests/qml_test_main.cpp, TestServices): the same
# FakeSessionMonitor/FakeDevices the engine's Catch2 cases use, so the two
# suites cannot disagree about what a scripted machine is.
target_include_directories(crucible_qmltests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/ui" "${CMAKE_CURRENT_SOURCE_DIR}/../gui"
    "${CMAKE_SOURCE_DIR}/tests/crucible")
target_compile_definitions(crucible_qmltests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
    ICLFORGE_CRUCIBLE_DRIVER_SOURCE_DIR="${CMAKE_SOURCE_DIR}/apps/windows/driver"
    ICLFORGE_CRUCIBLE_QUICK3D=${ICLFORGE_CRUCIBLE_QUICK3D}
    QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ui/tests/qml")
target_link_libraries(crucible_qmltests PRIVATE
    iclforge::crucible_engine
    iclforge::coverage
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::QuickTest Qt6::Widgets
    ${ICLFORGE_CRUCIBLE_QUICK3D_LIB})
if(Qt6Svg_FOUND)
    target_link_libraries(crucible_qmltests PRIVATE ${ICLFORGE_CRUCIBLE_SVG_LIB})
endif()
# Into bin/ beside crucible.exe, whose windeployqt step puts the Qt runtime
# there; ordered after it so the two never deploy into bin/ at once.
set_target_properties(crucible_qmltests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
add_dependencies(crucible_qmltests crucible)
# Its own windeployqt pass: crucible's deploys the app's Qt, not
# Qt6Test/Qt6QuickTest, and a missing DLL is not a failure but a modal
# "cannot proceed" dialog nobody sees under ctest - the process sits until
# the 300 s timeout with no output at all, which is how this was found.
if(ICLFORGE_CRUCIBLE_WINDEPLOYQT)
    add_custom_command(TARGET crucible_qmltests POST_BUILD
        COMMAND "${ICLFORGE_CRUCIBLE_WINDEPLOYQT}"
            --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/ui/qml"
            --qmldir "${ICLFORGE_CRUCIBLE_SHARED_DIR}"
            --no-compiler-runtime
            "$<TARGET_FILE:crucible_qmltests>"
        # windeployqt deploys the windows platform plugin only; the suites
        # run under offscreen (apps/gui/tests does the same copy). Without
        # it Qt reports the plugin missing and then waits on a message box.
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${ICLFORGE_CRUCIBLE_QT_BIN_DIR}/../plugins/platforms/qoffscreen$<$<CONFIG:Debug>:d>.dll"
            "$<TARGET_FILE_DIR:crucible_qmltests>/platforms/"
        COMMENT "Deploying Qt runtime dependencies for crucible_qmltests")
endif()

# The translations, compiled a second time into this binary (lrelease only;
# lupdate already ran for crucible over the identical QML list), under their
# own output location so Ninja does not see two rules for one .qm.
set_source_files_properties(${ICLFORGE_CRUCIBLE_TS_FILES} PROPERTIES
    OUTPUT_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/tests")
qt6_add_lrelease(crucible_qmltests
    TS_FILES ${ICLFORGE_CRUCIBLE_TS_FILES}
    LRELEASE_TARGET crucible_qmltests_lrelease
    QM_FILES_OUTPUT_VARIABLE ICLFORGE_CRUCIBLE_TEST_QM_FILES)
qt6_add_resources(crucible_qmltests "crucible_qmltests_translations"
    PREFIX "/i18n"
    BASE "${CMAKE_CURRENT_BINARY_DIR}/tests"
    FILES ${ICLFORGE_CRUCIBLE_TEST_QM_FILES})

# One ctest entry per suite (its own process, its own controller), label
# "crucible-ui" so the coverage preset and tools/checks/coverage_crucible.ps1
# can select it alongside the "crucible" Catch2 cases - both of them read the
# label written below, so renaming it here means editing that script's
# -Labels default and CMakePresets.json's test-windows-llvm-coverage filter
# with it. QT_FORCE_STDERR_LOGGING
# for the reason apps/gui/tests gives: without it Qt on Windows routes the
# PASS/FAIL lines away from stderr when it decides no console is attached.
file(GLOB ICLFORGE_CRUCIBLE_QML_TEST_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/tests/qml/tst_*.qml")
foreach(test_file IN LISTS ICLFORGE_CRUCIBLE_QML_TEST_FILES)
    get_filename_component(test_name "${test_file}" NAME_WE)
    string(REGEX REPLACE "^tst_" "" suite "${test_name}")
    add_test(NAME crucible_qml_tests_${suite}
        COMMAND crucible_qmltests -input "${test_file}" -platform offscreen)
    # QT_QUICK_BACKEND=software beside the offscreen platform. Offscreen
    # stops Qt asking for a window; it does not stop the scene graph asking
    # for a GPU context, and on a runner with no display server that request
    # can hang rather than fail. Read off CI on 2026-09-06: firstrun, room
    # and shell - the three that build the most - timed out at 300 seconds
    # each on both macOS legs while the other eight passed, and setting this
    # cleared the Intel leg. The Linux Crucible pass already sets this pair
    # together for the same reason (.github/workflows/_build.yml). Software
    # rendering is slower and the room's 3D view still runs under it, which
    # the Linux runs show.
    #
    # It did NOT clear the Apple Silicon leg, and that is worth keeping here
    # so nobody reaches for a render-loop setting next time. The same three
    # suites kept timing out with the software backend in place, for an
    # unrelated reason found by sampling the stuck process: the engine's
    # frame thread was inside AudioDeviceCreateIOProcID on a Core Audio
    # process tap that never returned, and the window's own thread was
    # blocked behind it. Two hangs, one symptom. See
    # docs/crucible/design/promotion.md's Phase 5 record.
    set_tests_properties(crucible_qml_tests_${suite} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_FORCE_STDERR_LOGGING=1"
        TIMEOUT 300
        LABELS "crucible-ui")
    # The icon fixture (ui/tests/fixtures/xdg: a hicolor theme and four
    # .desktop entries) is what the Linux icon provider sees as the whole
    # XDG data path, and the data home is an empty build-tree directory so
    # a developer's own ~/.local/share never leaks into a run. Inert on
    # Windows, whose provider reads neither variable (tst_icons.qml).
    set_property(TEST crucible_qml_tests_${suite} APPEND PROPERTY ENVIRONMENT
        "XDG_DATA_DIRS=${CMAKE_CURRENT_SOURCE_DIR}/ui/tests/fixtures/xdg"
        "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/tests/xdg-home")
endforeach()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests/xdg-home")
