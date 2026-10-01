# ---------------------------------------------------------------------------
# hearth_qmltests - Qt Quick Test coverage for the Hearth window (ui/qml),
# driving the real HearthController the way crucible_qmltests drives the
# real CrucibleController and forge_gui_qmltests drives the real
# EncoderController (apps/crucible/ui/tests/CMakeLists.txt's own header,
# whose reasoning this file follows and does not repeat) - closer template
# than apps/gui/tests/CMakeLists.txt since Hearth, like Crucible, keeps its
# window under its own ui/ subdirectory rather than inline in the app's
# top-level CMakeLists.txt. apps/hearth/ui/CMakeLists.txt's own comment, next
# to the windeployqt chain (search "A5's exit"), has anticipated this file's
# arrival since Hearth's window was first scaffolded.
#
# include()d from ../CMakeLists.txt inside its Qt6_FOUND block, after
# hearth is defined, so paths are relative to apps/hearth/ui and the
# ICLFORGE_HEARTH_QML_FILES / ICLFORGE_HEARTH_SHARED_DIR variables are the ones hearth
# itself embeds: the two targets cannot list different files without both
# being edited. Second, independent embedding of the Hearth module,
# not a shared library, for the WHOLE_ARCHIVE reason the GUI gives.
#
# Unlike crucible_qmltests, this target carries no tray, no icon provider
# and no per-platform sources: apps/hearth/ui/CMakeLists.txt's own qt_add_executable(hearth ...)
# lists only main.cpp, hearth_controller.cpp/.hpp, item_loader.cpp/.hpp,
# network_controller.cpp/.hpp and apps/common/container_input.cpp/.hpp - this
# target embeds the same controller/loader sources (main.cpp is excluded: it
# is hearth's own entry point, and qml_test_main.cpp is this target's).
# network_controller.cpp/.hpp are included from tst_network_sink_settings.qml
# onwards (issue #875): NetworkController is a second QML_SINGLETON, and
# without its own sources here QML fails at runtime with "NetworkController
# is not defined" rather than at configure or compile time - moc/qmltyperegistrar
# only register a QML_ELEMENT class that is actually part of the target it runs
# against. apps/gui/language_manager.cpp/.hpp is the third, for the same
# reason: Settings.qml imports HearthLanguage, whose LanguageManager
# main.cpp registers as a singleton instance.
# ---------------------------------------------------------------------------
find_package(Qt6 6.8 REQUIRED COMPONENTS QuickTest Test)

qt_add_executable(hearth_qmltests
    tests/qml_test_main.cpp
    # TestServices' fakes (tests/test_room.hpp): a fake device room for
    # HearthController::set_test_outputs() and an in-process test sink for
    # NetworkController::sinks_for_test() - kept in their own translation
    # unit, apart from qml_test_main.cpp's Qt headers, for the `slots` macro
    # reason test_room.hpp gives.
    tests/test_room.cpp
    tests/test_room.hpp
    test_outputs.hpp
    hearth_controller.cpp
    hearth_controller.hpp
    item_loader.cpp
    item_loader.hpp
    network_controller.cpp
    network_controller.hpp
    # Settings.qml imports HearthLanguage now, so the suite needs the
    # same LanguageManager hearth itself compiles in - a third singleton,
    # registered the same way and failing the same way without its sources.
    "${PROJECT_SOURCE_DIR}/apps/gui/language_manager.cpp"
    "${PROJECT_SOURCE_DIR}/apps/gui/language_manager.hpp"
    "${PROJECT_SOURCE_DIR}/apps/common/container_input.cpp"
    "${PROJECT_SOURCE_DIR}/apps/common/container_input.hpp")

qt_add_qml_module(hearth_qmltests
    URI Hearth
    VERSION 1.0
    # Both targets embed the same URI; without this they'd both compute the
    # same "<binary dir>/Hearth" output directory and collide -
    # hearth's own module lives there already.
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests/Hearth"
    QML_FILES ${ICLFORGE_HEARTH_QML_FILES})

# Forces this target's own POST_BUILD windeployqt step (below) to run only
# after hearth's has finished - apps/gui/tests/CMakeLists.txt's own
# add_dependencies(forge_gui_qmltests forge-gui) comment explains why: two
# windeployqt passes racing to write the same shared bin/ fails with "Cannot
# create a file when that file already exists" on a real "build everything"
# run.
add_dependencies(hearth_qmltests hearth)

target_include_directories(hearth_qmltests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/apps/gui"
    "${CMAKE_SOURCE_DIR}/apps/common")
target_compile_definitions(hearth_qmltests PRIVATE
    QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/qml")
target_link_libraries(hearth_qmltests PRIVATE
    hearth_engine
    # apps/hearth/testsink's library, for TestServices.startTestSink() - the
    # same in-process Sink tests/hearth/test_testsink.cpp pairs over loopback.
    hearth_testsink
    # The AC-4 encoder, for TestServices.writeAc4Stream(): the AC-4 decoder
    # page's suite plays tones it writes (planning/ac4.md, I2).
    iclforge::ac4enc
    iclforge::matroska iclforge::mp4 iclforge::mpegts
    iclforge::warnings iclforge::fmt
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::QuickTest
    # QTest::keyClick() on a QWindow, for TestServices.keyClick() (see
    # qml_test_main.cpp).
    Qt6::Test)

# Same rationale as apps/gui/tests/CMakeLists.txt's identical loop: Qt's
# generated moc/rcc/qmlcachegen output for THIS target compiles under the
# strict warning set unless excluded, and qmlcachegen is known to emit at
# least one warning clang-cl/GCC's -Wconversion rejects.
get_target_property(ICLFORGE_HEARTH_QMLTEST_SOURCES hearth_qmltests SOURCES)
set(ICLFORGE_HEARTH_QMLTEST_GENERATED_SOURCES "")
foreach(source IN LISTS ICLFORGE_HEARTH_QMLTEST_SOURCES)
    if(source MATCHES "^${CMAKE_CURRENT_BINARY_DIR}/.*\\.(cpp|cxx|cc)$")
        list(APPEND ICLFORGE_HEARTH_QMLTEST_GENERATED_SOURCES "${source}")
    endif()
endforeach()
if(ICLFORGE_HEARTH_QMLTEST_GENERATED_SOURCES)
    set_source_files_properties(${ICLFORGE_HEARTH_QMLTEST_GENERATED_SOURCES}
        PROPERTIES COMPILE_OPTIONS "${AC3_WARNINGS_OFF_FLAG}")
endif()

# Into bin/ beside hearth.exe, whose windeployqt step puts the Qt runtime
# there; ordered after it (add_dependencies above) so the two never deploy
# into bin/ at once.
set_target_properties(hearth_qmltests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Its own windeployqt pass: hearth's deploys the app's own Qt, not
# Qt6Test/Qt6QuickTest, and a missing DLL is not a failure but a modal
# "cannot proceed" dialog nobody sees under ctest - apps/crucible/ui/tests's
# own comment on this exact trap applies here unchanged.
if(WIN32)
    get_filename_component(ICLFORGE_HEARTH_QMLTEST_QT_BIN_DIR "${Qt6_DIR}/../../../bin" ABSOLUTE)
    find_program(ICLFORGE_HEARTH_QMLTEST_WINDEPLOYQT NAMES windeployqt6 windeployqt
        HINTS "${ICLFORGE_HEARTH_QMLTEST_QT_BIN_DIR}")
    if(ICLFORGE_HEARTH_QMLTEST_WINDEPLOYQT)
        add_custom_command(TARGET hearth_qmltests POST_BUILD
            COMMAND "${ICLFORGE_HEARTH_QMLTEST_WINDEPLOYQT}"
                --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/qml"
                --qmldir "${ICLFORGE_HEARTH_SHARED_DIR}"
                --no-compiler-runtime
                "$<TARGET_FILE:hearth_qmltests>"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${ICLFORGE_HEARTH_QMLTEST_QT_BIN_DIR}/../plugins/platforms/qoffscreen$<$<CONFIG:Debug>:d>.dll"
                "$<TARGET_FILE_DIR:hearth_qmltests>/platforms/"
            COMMENT "Deploying Qt runtime dependencies for hearth_qmltests")
    else()
        message(WARNING
            "windeployqt not found next to Qt6; hearth_qmltests.exe will not run standalone from the build tree.")
    endif()
endif()

# One ctest entry per tst_*.qml suite, each its own process and its own fresh
# HearthController singleton - apps/gui/tests/CMakeLists.txt's own comment on
# why (failure isolation, tighter per-suite isolation than one shared
# process) applies unchanged. QT_QPA_PLATFORM=offscreen and
# QT_FORCE_STDERR_LOGGING=1 for the reasons both existing suites give:
# without the platform, Qt asks for a real window; without the logging
# override, Windows can silently reroute PASS/FAIL output away from stderr.
# QT_QUICK_BACKEND=software: apps/crucible/ui/tests/CMakeLists.txt's own
# comment records real macOS hangs under the default GPU-accelerated backend
# for suites that build heavily on an offscreen runner with no display
# server - applied here from the start rather than waiting to rediscover it.
file(GLOB ICLFORGE_HEARTH_QML_TEST_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/qml/tst_*.qml")
foreach(test_file IN LISTS ICLFORGE_HEARTH_QML_TEST_FILES)
    get_filename_component(test_name "${test_file}" NAME_WE)
    string(REGEX REPLACE "^tst_" "" suite "${test_name}")
    add_test(NAME hearth_qml_tests_${suite}
        COMMAND hearth_qmltests -input "${test_file}" -platform offscreen)
    set_tests_properties(hearth_qml_tests_${suite} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_FORCE_STDERR_LOGGING=1"
        TIMEOUT 300
        LABELS "hearth-ui")
endforeach()
