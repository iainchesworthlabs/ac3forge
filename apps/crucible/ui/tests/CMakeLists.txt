# ---------------------------------------------------------------------------
# ac3crucible_qmltests - Qt Quick Test coverage for the AC3Forge Crucible's
# window (ui/qml), driving the real CrucibleController the way ac3gui_qmltests
# drives the real EncoderController (apps/gui/tests/CMakeLists.txt, whose
# reasoning this file follows and does not repeat).
#
# include()d from ../../CMakeLists.txt inside its Qt6_FOUND block, after
# ac3crucible is defined, so paths are relative to apps/windows and the
# AC3DESK_QML_FILES / AC3DESK_TS_FILES lists are the ones ac3crucible itself
# embeds: the two targets cannot list different files without both being
# edited. Second, independent embedding of the Ac3ForgeCrucible module, not a
# shared library, for the WHOLE_ARCHIVE reason the GUI gives.
# ---------------------------------------------------------------------------
find_package(Qt6 6.5 REQUIRED COMPONENTS QuickTest)

qt_add_executable(ac3crucible_qmltests
    ui/tests/qml_test_main.cpp
    ui/crucible_controller.cpp
    ui/crucible_controller.hpp
    ui/app_entry.hpp
    ui/app_icon_provider.hpp
    ../gui/system_theme.cpp
    ../gui/system_theme.hpp
    ../gui/language_manager.cpp
    ../gui/language_manager.hpp)
# The icon provider is per platform, the same way the window's own target
# selects it (apps/crucible/CMakeLists.txt): this target embeds the same
# sources and has to make the same choice.
if(WIN32)
    target_sources(ac3crucible_qmltests PRIVATE ui/platform/windows/app_icon_provider.cpp)
elseif(LINUX)
    target_sources(ac3crucible_qmltests PRIVATE ui/platform/linux/app_icon_provider.cpp)
    target_sources(ac3crucible_qmltests PRIVATE ui/desktop_entries.cpp ui/desktop_entries.hpp)
endif()

qt_add_qml_module(ac3crucible_qmltests
    URI Ac3ForgeCrucible
    VERSION 1.0
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests/Ac3ForgeCrucible"
    QML_FILES ${AC3DESK_QML_FILES}
    RESOURCES "${CMAKE_CURRENT_SOURCE_DIR}/ui/qml/tray.svg")

# The notices resource too: CrucibleController::licenceNotices() reads
# :/notices/NOTICES.txt, and tst_about.qml asserts what this build's copy
# says, so this binary embeds the same file ac3crucible does. Without it the
# property is the fallback sentence and that suite fails, by design.
qt6_add_resources(ac3crucible_qmltests "ac3crucible_qmltests_notices"
    PREFIX "/notices"
    BASE "${CMAKE_CURRENT_BINARY_DIR}/notices"
    FILES "${AC3CRUCIBLE_NOTICES_FILE}")

target_include_directories(ac3crucible_qmltests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/ui" "${CMAKE_CURRENT_SOURCE_DIR}/../gui")
target_compile_definitions(ac3crucible_qmltests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
    AC3DESK_DRIVER_SOURCE_DIR="${CMAKE_SOURCE_DIR}/apps/windows/driver"
    AC3DESK_QUICK3D=${AC3DESK_QUICK3D}
    QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ui/tests/qml")
target_link_libraries(ac3crucible_qmltests PRIVATE
    ac3::crucible_engine
    ac3::coverage
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::QuickTest ${AC3DESK_QUICK3D_LIB})
if(Qt6Svg_FOUND)
    target_link_libraries(ac3crucible_qmltests PRIVATE ${AC3DESK_SVG_LIB})
endif()
# Into bin/ beside ac3crucible.exe, whose windeployqt step puts the Qt runtime
# there; ordered after it so the two never deploy into bin/ at once.
set_target_properties(ac3crucible_qmltests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
add_dependencies(ac3crucible_qmltests ac3crucible)
# Its own windeployqt pass: ac3crucible's deploys the app's Qt, not
# Qt6Test/Qt6QuickTest, and a missing DLL is not a failure but a modal
# "cannot proceed" dialog nobody sees under ctest - the process sits until
# the 300 s timeout with no output at all, which is how this was found.
if(AC3DESK_WINDEPLOYQT)
    add_custom_command(TARGET ac3crucible_qmltests POST_BUILD
        COMMAND "${AC3DESK_WINDEPLOYQT}"
            --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/ui/qml"
            --qmldir "${AC3DESK_SHARED_DIR}"
            --no-compiler-runtime
            "$<TARGET_FILE:ac3crucible_qmltests>"
        # windeployqt deploys the windows platform plugin only; the suites
        # run under offscreen (apps/gui/tests does the same copy). Without
        # it Qt reports the plugin missing and then waits on a message box.
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${AC3DESK_QT_BIN_DIR}/../plugins/platforms/qoffscreen$<$<CONFIG:Debug>:d>.dll"
            "$<TARGET_FILE_DIR:ac3crucible_qmltests>/platforms/"
        COMMENT "Deploying Qt runtime dependencies for ac3crucible_qmltests")
endif()

# The translations, compiled a second time into this binary (lrelease only;
# lupdate already ran for ac3crucible over the identical QML list), under their
# own output location so Ninja does not see two rules for one .qm.
set_source_files_properties(${AC3DESK_TS_FILES} PROPERTIES
    OUTPUT_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/tests")
qt6_add_lrelease(ac3crucible_qmltests
    TS_FILES ${AC3DESK_TS_FILES}
    LRELEASE_TARGET ac3crucible_qmltests_lrelease
    QM_FILES_OUTPUT_VARIABLE AC3DESK_TEST_QM_FILES)
qt6_add_resources(ac3crucible_qmltests "ac3crucible_qmltests_translations"
    PREFIX "/i18n"
    BASE "${CMAKE_CURRENT_BINARY_DIR}/tests"
    FILES ${AC3DESK_TEST_QM_FILES})

# One ctest entry per suite (its own process, its own controller), label
# "desk" so the coverage preset and tools/checks/coverage_crucible.ps1 can
# select it alongside the "crucible" Catch2 cases. QT_FORCE_STDERR_LOGGING
# for the reason apps/gui/tests gives: without it Qt on Windows routes the
# PASS/FAIL lines away from stderr when it decides no console is attached.
file(GLOB AC3DESK_QML_TEST_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/tests/qml/tst_*.qml")
foreach(test_file IN LISTS AC3DESK_QML_TEST_FILES)
    get_filename_component(test_name "${test_file}" NAME_WE)
    string(REGEX REPLACE "^tst_" "" suite "${test_name}")
    add_test(NAME ac3crucible_qml_tests_${suite}
        COMMAND ac3crucible_qmltests -input "${test_file}" -platform offscreen)
    set_tests_properties(ac3crucible_qml_tests_${suite} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_FORCE_STDERR_LOGGING=1"
        TIMEOUT 300
        LABELS "crucible-ui")
    # The icon fixture (ui/tests/fixtures/xdg: a hicolor theme and four
    # .desktop entries) is what the Linux icon provider sees as the whole
    # XDG data path, and the data home is an empty build-tree directory so
    # a developer's own ~/.local/share never leaks into a run. Inert on
    # Windows, whose provider reads neither variable (tst_icons.qml).
    set_property(TEST ac3crucible_qml_tests_${suite} APPEND PROPERTY ENVIRONMENT
        "XDG_DATA_DIRS=${CMAKE_CURRENT_SOURCE_DIR}/ui/tests/fixtures/xdg"
        "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/tests/xdg-home")
endforeach()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests/xdg-home")
