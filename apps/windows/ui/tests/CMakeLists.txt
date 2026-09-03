# ---------------------------------------------------------------------------
# ac3desk_qmltests - Qt Quick Test coverage for the Desktop Atmos Demo's
# window (ui/qml), driving the real DeskController the way ac3gui_qmltests
# drives the real EncoderController (apps/gui/tests/CMakeLists.txt, whose
# reasoning this file follows and does not repeat).
#
# include()d from ../../CMakeLists.txt inside its Qt6_FOUND block, after
# ac3desk is defined, so paths are relative to apps/windows and the
# AC3DESK_QML_FILES / AC3DESK_TS_FILES lists are the ones ac3desk itself
# embeds: the two targets cannot list different files without both being
# edited. Second, independent embedding of the Ac3ForgeDesk module, not a
# shared library, for the WHOLE_ARCHIVE reason the GUI gives.
# ---------------------------------------------------------------------------
find_package(Qt6 6.5 REQUIRED COMPONENTS QuickTest)

qt_add_executable(ac3desk_qmltests
    ui/tests/qml_test_main.cpp
    ui/desk_controller.cpp
    ui/desk_controller.hpp
    ../gui/system_theme.cpp
    ../gui/system_theme.hpp
    ../gui/language_manager.cpp
    ../gui/language_manager.hpp)

qt_add_qml_module(ac3desk_qmltests
    URI Ac3ForgeDesk
    VERSION 1.0
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests/Ac3ForgeDesk"
    QML_FILES ${AC3DESK_QML_FILES}
    RESOURCES "${CMAKE_CURRENT_SOURCE_DIR}/ui/qml/tray.svg")

target_include_directories(ac3desk_qmltests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/ui" "${CMAKE_CURRENT_SOURCE_DIR}/../gui")
target_compile_definitions(ac3desk_qmltests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
    AC3DESK_DRIVER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/driver"
    AC3DESK_QUICK3D=${AC3DESK_QUICK3D}
    QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ui/tests/qml")
target_link_libraries(ac3desk_qmltests PRIVATE
    ac3::windemo_engine
    ac3::coverage
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::QuickTest ${AC3DESK_QUICK3D_LIB})
# Into bin/ beside ac3desk.exe, whose windeployqt step puts the Qt runtime
# there; ordered after it so the two never deploy into bin/ at once.
set_target_properties(ac3desk_qmltests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
add_dependencies(ac3desk_qmltests ac3desk)
# Its own windeployqt pass: ac3desk's deploys the app's Qt, not
# Qt6Test/Qt6QuickTest, and a missing DLL is not a failure but a modal
# "cannot proceed" dialog nobody sees under ctest - the process sits until
# the 300 s timeout with no output at all, which is how this was found.
if(AC3DESK_WINDEPLOYQT)
    add_custom_command(TARGET ac3desk_qmltests POST_BUILD
        COMMAND "${AC3DESK_WINDEPLOYQT}"
            --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/ui/qml"
            --qmldir "${AC3DESK_SHARED_DIR}"
            --no-compiler-runtime
            "$<TARGET_FILE:ac3desk_qmltests>"
        # windeployqt deploys the windows platform plugin only; the suites
        # run under offscreen (apps/gui/tests does the same copy). Without
        # it Qt reports the plugin missing and then waits on a message box.
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${AC3DESK_QT_BIN_DIR}/../plugins/platforms/qoffscreen$<$<CONFIG:Debug>:d>.dll"
            "$<TARGET_FILE_DIR:ac3desk_qmltests>/platforms/"
        COMMENT "Deploying Qt runtime dependencies for ac3desk_qmltests")
endif()

# The translations, compiled a second time into this binary (lrelease only;
# lupdate already ran for ac3desk over the identical QML list), under their
# own output location so Ninja does not see two rules for one .qm.
set_source_files_properties(${AC3DESK_TS_FILES} PROPERTIES
    OUTPUT_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/tests")
qt6_add_lrelease(ac3desk_qmltests
    TS_FILES ${AC3DESK_TS_FILES}
    LRELEASE_TARGET ac3desk_qmltests_lrelease
    QM_FILES_OUTPUT_VARIABLE AC3DESK_TEST_QM_FILES)
qt6_add_resources(ac3desk_qmltests "ac3desk_qmltests_translations"
    PREFIX "/i18n"
    BASE "${CMAKE_CURRENT_BINARY_DIR}/tests"
    FILES ${AC3DESK_TEST_QM_FILES})

# One ctest entry per suite (its own process, its own controller), label
# "desk" so the coverage preset and tools/checks/coverage_windemo.ps1 can
# select it alongside the "windemo" Catch2 cases. QT_FORCE_STDERR_LOGGING
# for the reason apps/gui/tests gives: without it Qt on Windows routes the
# PASS/FAIL lines away from stderr when it decides no console is attached.
file(GLOB AC3DESK_QML_TEST_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/tests/qml/tst_*.qml")
foreach(test_file IN LISTS AC3DESK_QML_TEST_FILES)
    get_filename_component(test_name "${test_file}" NAME_WE)
    string(REGEX REPLACE "^tst_" "" suite "${test_name}")
    add_test(NAME ac3desk_qml_tests_${suite}
        COMMAND ac3desk_qmltests -input "${test_file}" -platform offscreen)
    set_tests_properties(ac3desk_qml_tests_${suite} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_FORCE_STDERR_LOGGING=1"
        TIMEOUT 300
        LABELS "desk")
endforeach()
