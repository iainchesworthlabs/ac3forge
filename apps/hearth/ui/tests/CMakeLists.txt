# ---------------------------------------------------------------------------
# ac3hearth_qmltests - Qt Quick Test coverage for the Hearth window (ui/qml),
# driving the real HearthController the way ac3crucible_qmltests drives the
# real CrucibleController and ac3gui_qmltests drives the real
# EncoderController (apps/crucible/ui/tests/CMakeLists.txt's own header,
# whose reasoning this file follows and does not repeat) - closer template
# than apps/gui/tests/CMakeLists.txt since Hearth, like Crucible, keeps its
# window under its own ui/ subdirectory rather than inline in the app's
# top-level CMakeLists.txt. apps/hearth/ui/CMakeLists.txt's own comment, next
# to the windeployqt chain (search "A5's exit"), has anticipated this file's
# arrival since Hearth's window was first scaffolded.
#
# include()d from ../CMakeLists.txt inside its Qt6_FOUND block, after
# ac3hearth is defined, so paths are relative to apps/hearth/ui and the
# AC3HEARTH_QML_FILES / AC3HEARTH_SHARED_DIR variables are the ones ac3hearth
# itself embeds: the two targets cannot list different files without both
# being edited. Second, independent embedding of the Ac3ForgeHearth module,
# not a shared library, for the WHOLE_ARCHIVE reason the GUI gives.
#
# Unlike ac3crucible_qmltests, this target carries no tray, no icon provider
# and no per-platform sources: apps/hearth/ui/CMakeLists.txt's own qt_add_executable(ac3hearth ...)
# lists only main.cpp, hearth_controller.cpp/.hpp, item_loader.cpp/.hpp,
# network_controller.cpp/.hpp and apps/common/container_input.cpp/.hpp - this
# target embeds the same controller/loader sources (main.cpp is excluded: it
# is ac3hearth's own entry point, and qml_test_main.cpp is this target's).
# network_controller.cpp is excluded too - no tst_*.qml here drives the
# Network page yet, and hearth_controller.cpp does not depend on it.
# ---------------------------------------------------------------------------
find_package(Qt6 6.8 REQUIRED COMPONENTS QuickTest)

qt_add_executable(ac3hearth_qmltests
    tests/qml_test_main.cpp
    hearth_controller.cpp
    hearth_controller.hpp
    item_loader.cpp
    item_loader.hpp
    # Settings.qml imports Ac3ForgeHearthLanguage now, so the suite needs the
    # same LanguageManager ac3hearth itself compiles in.
    "${PROJECT_SOURCE_DIR}/apps/gui/language_manager.cpp"
    "${PROJECT_SOURCE_DIR}/apps/gui/language_manager.hpp"
    "${PROJECT_SOURCE_DIR}/apps/common/container_input.cpp"
    "${PROJECT_SOURCE_DIR}/apps/common/container_input.hpp")

qt_add_qml_module(ac3hearth_qmltests
    URI Ac3ForgeHearth
    VERSION 1.0
    # Both targets embed the same URI; without this they'd both compute the
    # same "<binary dir>/Ac3ForgeHearth" output directory and collide -
    # ac3hearth's own module lives there already.
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests/Ac3ForgeHearth"
    QML_FILES ${AC3HEARTH_QML_FILES})

# Forces this target's own POST_BUILD windeployqt step (below) to run only
# after ac3hearth's has finished - apps/gui/tests/CMakeLists.txt's own
# add_dependencies(ac3gui_qmltests ac3gui) comment explains why: two
# windeployqt passes racing to write the same shared bin/ fails with "Cannot
# create a file when that file already exists" on a real "build everything"
# run.
add_dependencies(ac3hearth_qmltests ac3hearth)

target_include_directories(ac3hearth_qmltests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/apps/gui"
    "${CMAKE_SOURCE_DIR}/apps/common")
target_compile_definitions(ac3hearth_qmltests PRIVATE
    QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/qml")
target_link_libraries(ac3hearth_qmltests PRIVATE
    ac3hearth_engine
    matroska::matroska mp4::mp4 mpegts::mpegts
    ac3::warnings ac3::fmt
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::QuickTest)

# Same rationale as apps/gui/tests/CMakeLists.txt's identical loop: Qt's
# generated moc/rcc/qmlcachegen output for THIS target compiles under the
# strict warning set unless excluded, and qmlcachegen is known to emit at
# least one warning clang-cl/GCC's -Wconversion rejects.
get_target_property(AC3HEARTH_QMLTEST_SOURCES ac3hearth_qmltests SOURCES)
set(AC3HEARTH_QMLTEST_GENERATED_SOURCES "")
foreach(source IN LISTS AC3HEARTH_QMLTEST_SOURCES)
    if(source MATCHES "^${CMAKE_CURRENT_BINARY_DIR}/.*\\.(cpp|cxx|cc)$")
        list(APPEND AC3HEARTH_QMLTEST_GENERATED_SOURCES "${source}")
    endif()
endforeach()
if(AC3HEARTH_QMLTEST_GENERATED_SOURCES)
    set_source_files_properties(${AC3HEARTH_QMLTEST_GENERATED_SOURCES}
        PROPERTIES COMPILE_OPTIONS "${AC3_WARNINGS_OFF_FLAG}")
endif()

# Into bin/ beside ac3hearth.exe, whose windeployqt step puts the Qt runtime
# there; ordered after it (add_dependencies above) so the two never deploy
# into bin/ at once.
set_target_properties(ac3hearth_qmltests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Its own windeployqt pass: ac3hearth's deploys the app's own Qt, not
# Qt6Test/Qt6QuickTest, and a missing DLL is not a failure but a modal
# "cannot proceed" dialog nobody sees under ctest - apps/crucible/ui/tests's
# own comment on this exact trap applies here unchanged.
if(WIN32)
    get_filename_component(AC3HEARTH_QMLTEST_QT_BIN_DIR "${Qt6_DIR}/../../../bin" ABSOLUTE)
    find_program(AC3HEARTH_QMLTEST_WINDEPLOYQT NAMES windeployqt6 windeployqt
        HINTS "${AC3HEARTH_QMLTEST_QT_BIN_DIR}")
    if(AC3HEARTH_QMLTEST_WINDEPLOYQT)
        add_custom_command(TARGET ac3hearth_qmltests POST_BUILD
            COMMAND "${AC3HEARTH_QMLTEST_WINDEPLOYQT}"
                --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/qml"
                --qmldir "${AC3HEARTH_SHARED_DIR}"
                --no-compiler-runtime
                "$<TARGET_FILE:ac3hearth_qmltests>"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${AC3HEARTH_QMLTEST_QT_BIN_DIR}/../plugins/platforms/qoffscreen$<$<CONFIG:Debug>:d>.dll"
                "$<TARGET_FILE_DIR:ac3hearth_qmltests>/platforms/"
            COMMENT "Deploying Qt runtime dependencies for ac3hearth_qmltests")
    else()
        message(WARNING
            "windeployqt not found next to Qt6; ac3hearth_qmltests.exe will not run standalone from the build tree.")
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
file(GLOB AC3HEARTH_QML_TEST_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/qml/tst_*.qml")
foreach(test_file IN LISTS AC3HEARTH_QML_TEST_FILES)
    get_filename_component(test_name "${test_file}" NAME_WE)
    string(REGEX REPLACE "^tst_" "" suite "${test_name}")
    add_test(NAME ac3hearth_qml_tests_${suite}
        COMMAND ac3hearth_qmltests -input "${test_file}" -platform offscreen)
    set_tests_properties(ac3hearth_qml_tests_${suite} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_FORCE_STDERR_LOGGING=1"
        TIMEOUT 300
        LABELS "hearth-ui")
endforeach()
