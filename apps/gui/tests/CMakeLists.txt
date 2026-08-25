# ---------------------------------------------------------------------------
# ac3gui_qmltests - Qt Quick Test coverage for the interactive surfaces in
# ../qml/. Drives the REAL EncoderController, the same singleton ac3gui
# itself runs - not a mock: every leaf QML component already reads it
# directly rather than through passed-in properties, and this project's own
# code deliberately avoids a second, parallel API that the real one could
# silently disagree with (see encoder_controller.hpp's own header comment).
#
# Included (not add_subdirectory()'d) from ../CMakeLists.txt, so every path
# below is written relative to apps/gui, not apps/gui/tests - see the comment
# at that include() call for why that distinction is load-bearing here and
# not just a style choice.
#
# This embeds a second, independent copy of the Ac3Forge QML module - not a
# shared library with ac3gui - for the same WHOLE_ARCHIVE reason
# ../CMakeLists.txt gives for embedding it directly into the executable
# rather than a backing plugin. AC3_QML_FILES is the same list ac3gui's own
# qt_add_qml_module call uses, set once in ../CMakeLists.txt, so the two
# embeddings cannot list a different set of files without both being edited.
# ---------------------------------------------------------------------------
find_package(Qt6 6.5 REQUIRED COMPONENTS QuickTest)

qt_add_executable(ac3gui_qmltests
    tests/qml_test_main.cpp
    encoder_controller.cpp
    encoder_controller.hpp
    qc_controller.cpp
    qc_controller.hpp
    object_decode_controller.cpp
    object_decode_controller.hpp
    stream_player_controller.cpp
    stream_player_controller.hpp
    channel_geometry.cpp
    channel_geometry.hpp
    ../common/fmp4_folder_writer.cpp
    ../common/fmp4_folder_writer.hpp
    ../common/recording_sink.cpp
    ../common/recording_sink.hpp
    ../common/container_input.cpp
    ../common/container_input.hpp
    system_theme.cpp
    system_theme.hpp)

# apps/common, for the same reason ../CMakeLists.txt names it: RecordingSink
# and Fmp4FolderWriter moved there when ac3cli's own takes started streaming
# through them (roadmap IO9), and container_input.cpp joined them the same
# way for roadmap IO2. This target compiles the same files in, exactly as it
# always did - qc_controller.cpp/object_decode_controller.cpp call
# ac3::apps::elementary_stream_from_bytes too, so it needs the same
# definition ac3gui's own target links.
target_include_directories(ac3gui_qmltests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../common")

qt_add_qml_module(ac3gui_qmltests
    URI Ac3Forge
    VERSION 1.0
    # Both targets embed the same URI; without this they'd both compute the
    # same "<binary dir>/Ac3Forge" output directory and collide - ac3gui's
    # own module lives there already.
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests/Ac3Forge"
    QML_FILES ${AC3_QML_FILES})

# Forces this target's own POST_BUILD windeployqt step (below) to run only
# after ac3gui's has finished, not just after ac3gui's LINK step - confirmed
# necessary on a real "build everything" CI run: both windeployqt processes
# copy the same Qt DLLs into the same shared bin/ output directory, and two
# of them racing to write e.g. Qt6Core.dll at once fails with "Cannot create
# a file when that file already exists." A local build of one target at a
# time never triggers this, which is why it went unnoticed until now.
add_dependencies(ac3gui_qmltests ac3gui)

# The standard Qt Quick Test hookup: QUICK_TEST_MAIN's generated main() reads
# this macro to find the tst_*.qml files at run time. -input on the ctest
# COMMAND line below is a second, redundant way to say the same thing -
# belt and suspenders, since this is the one part of the setup this project
# had no prior example of to copy from.
target_compile_definitions(ac3gui_qmltests PRIVATE
    QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/qml")

# Same rationale as ../CMakeLists.txt's identical loop: Qt's generated
# moc/rcc/qmlcachegen output for THIS target compiles under the strict
# warning set unless excluded, and qmlcachegen is known to emit at least one
# warning clang-cl/GCC's -Wconversion rejects (see ../CMakeLists.txt).
get_target_property(AC3_GUI_TEST_SOURCES ac3gui_qmltests SOURCES)
set(AC3_GUI_TEST_GENERATED_SOURCES "")
foreach(source IN LISTS AC3_GUI_TEST_SOURCES)
    if(source MATCHES "^${CMAKE_CURRENT_BINARY_DIR}/.*\\.(cpp|cxx|cc)$")
        list(APPEND AC3_GUI_TEST_GENERATED_SOURCES "${source}")
    endif()
endforeach()
if(AC3_GUI_TEST_GENERATED_SOURCES)
    set_source_files_properties(${AC3_GUI_TEST_GENERATED_SOURCES}
        PROPERTIES COMPILE_OPTIONS "${AC3_WARNINGS_OFF_FLAG}")
endif()

target_link_libraries(ac3gui_qmltests
    PRIVATE
        ac3::forge
        ac3::audio
        matroska::matroska
        mp4::mp4
        mpegts::mpegts
        ac3::warnings
        ac3::fmt
        Qt6::Core
        Qt6::Gui
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickTest
        Qt6::Concurrent)

# QT_QPA_PLATFORM=offscreen on the test itself, not just -platform offscreen
# on the command line: belt and suspenders again, and it means running the
# binary by hand with no arguments (as a developer debugging a failure would)
# still behaves headlessly instead of trying to open a real window.
#
# QT_FORCE_STDERR_LOGGING=1 is not optional, confirmed the hard way: Qt's
# default Windows behaviour is to decide whether a "real" console is attached
# and, if it decides no, silently reroute qWarning/qCritical/QtTest's own
# PASS/FAIL/Totals output to OutputDebugString instead of stderr - the exit
# code still correctly reflects pass/fail, but every diagnostic that would
# say WHY a failure happened is invisible to ctest's captured log, a build
# log, or a developer's terminal. Without this, a red CI run shows nothing
# but "failed" and someone has to already know this flag exists to find out
# what broke.
# QSG_RENDER_LOOP=basic on APPLE only: the macos-llvm CI leg's first-ever run
# building the GUI (see .github/workflows/_build.yml) hung this test outright
# - ctest's "Start 1: ac3gui_qml_tests" with zero further output for the
# entire 6-hour job timeout, no crash, no QML warning. Qt Quick's default
# "threaded" render loop opens a dedicated render thread synchronized with
# the GUI thread at each frame; that handshake is a known deadlock risk under
# a platform plugin with no real compositor to drive vsync/frame callbacks,
# and QSG_RENDER_LOOP=basic (single-threaded, synchronous render-on-sync) is
# the standard workaround. Confirmed to matter for real: with a bounded
# TIMEOUT added (below) but this env var not yet, the very next run reached
# the SAME "zero output" state and was killed by that timeout instead of the
# 6-hour ceiling - still stuck at the first test. With this var added, the
# same run reached test #30+ before hitting a second, unrelated hang (see
# qml_test_main.cpp's own comment on that one - a native-style/offscreen
# issue this harness alone was exposed to, now fixed there instead of here).
# Linux's offscreen plugin does not appear to hit the render-loop deadlock
# (linux-gcc/linux-llvm's own gui: true legs pass today without it), so this
# stays scoped to APPLE rather than applied everywhere untested.
set(AC3_GUI_QMLTESTS_ENV "QT_QPA_PLATFORM=offscreen;QT_FORCE_STDERR_LOGGING=1")
if(APPLE)
    list(APPEND AC3_GUI_QMLTESTS_ENV "QSG_RENDER_LOOP=basic")
endif()

# One ctest entry per tst_*.qml suite rather than a single ac3gui_qml_tests
# entry driving all ~160 test cases in one process: that monolithic entry
# measured ~580s on an idle Windows MSVC debug build (real encoder work, not
# overhead - the "~15s" figure a previous version of this comment quoted was
# stale, left over from when the suite was much smaller), a bare 3% under
# its own 600s TIMEOUT. Under CPU contention from another build/test job on
# the same machine - unremarkable on a shared dev box - that margin
# disappears and ctest kills the process mid-suite: no FAIL: line, no
# Totals: line, and no way to tell from the output alone which of the ~18
# QML suites was running when it died. -input accepts a single .qml file as
# well as a directory (Qt Quick Test's own suffix check on the argument
# handles both), so each suite becomes its own ctest entry below instead.
# This gets back the same failure-isolation a killed entry name already
# gives every other ctest case, and - since each entry is a fresh process -
# each suite now gets its OWN EncoderController singleton rather than the
# eighteen suites sharing one for the whole run, tightening isolation rather
# than loosening it. Measured per-suite split (JUnit XML `-o file,junitxml`
# timings from a real run, not a guess): heaviest is GuidedWizard at ~65s in
# that run, roughly a quarter of the whole suite's total - scaled to the
# ~580s idle baseline that puts its own worst case around two minutes, so a
# flat 300s TIMEOUT per entry leaves every suite, including the heaviest,
# comfortable headroom even under contention, while no single entry needs
# anywhere near the old 600s ceiling. Splitting also means these entries are
# independent by construction, so `ctest -L gui -j N` runs them concurrently
# without disturbing the rest of the suite's default serial execution
# (`ctest --preset test-<leg>` is unaffected - confirmed on a real 18-entry
# run: ~277s serial, 77s at -j4).
file(GLOB AC3_GUI_QML_TEST_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/qml/tst_*.qml")

foreach(AC3_GUI_QML_TEST_FILE IN LISTS AC3_GUI_QML_TEST_FILES)
    get_filename_component(AC3_GUI_QML_TEST_NAME "${AC3_GUI_QML_TEST_FILE}" NAME_WE)
    string(REGEX REPLACE "^tst_" "" AC3_GUI_QML_TEST_SUITE "${AC3_GUI_QML_TEST_NAME}")
    set(AC3_GUI_QML_TEST_CASE "ac3gui_qml_tests_${AC3_GUI_QML_TEST_SUITE}")

    add_test(NAME ${AC3_GUI_QML_TEST_CASE}
        COMMAND ac3gui_qmltests -input "${AC3_GUI_QML_TEST_FILE}" -platform offscreen)
    set_tests_properties(${AC3_GUI_QML_TEST_CASE} PROPERTIES
        ENVIRONMENT "${AC3_GUI_QMLTESTS_ENV}"
        # A safety net independent of whatever actually caused the macOS
        # hangs above, sized per-suite rather than for the whole harness -
        # see the comment above this loop for the measurement behind 300s.
        # A CTest TIMEOUT still counts as a captured failure, so
        # outputOnFailure (CMakePresets.json's test presets) prints
        # whatever the process wrote before it was killed - unlike an
        # external job-level cancellation, which discards it entirely,
        # exactly how the original macOS hang was first found.
        TIMEOUT 300
        LABELS "gui")
endforeach()

# ---------------------------------------------------------------------------
# Windows-only Qt runtime deployment, identical in spirit to ../CMakeLists.txt's
# POST_BUILD windeployqt step and for the same reason: without it the loader
# cannot find Qt6Core/Qt6Quick and QtQuick.Controls imports fail to resolve,
# offscreen or not. Linux/macOS need nothing here - the Qt kit's own RPATH
# already covers it, exactly as it does for ac3gui and for --smoke today.
# ---------------------------------------------------------------------------
if(WIN32)
    get_filename_component(AC3_QT_BIN_DIR "${Qt6_DIR}/../../../bin" ABSOLUTE)
    find_program(AC3_WINDEPLOYQT_EXECUTABLE
        NAMES windeployqt6 windeployqt
        HINTS "${AC3_QT_BIN_DIR}")

    if(AC3_WINDEPLOYQT_EXECUTABLE)
        set(AC3_VCINSTALLDIR "$ENV{VCINSTALLDIR}")
        string(REGEX REPLACE "[\\\\/]+$" "" AC3_VCINSTALLDIR "${AC3_VCINSTALLDIR}")
        file(TO_CMAKE_PATH "${AC3_VCINSTALLDIR}" AC3_VCINSTALLDIR)

        add_custom_command(TARGET ac3gui_qmltests POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E env "VCINSTALLDIR=${AC3_VCINSTALLDIR}"
                "${AC3_WINDEPLOYQT_EXECUTABLE}"
                --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/qml"
                --compiler-runtime
                "$<TARGET_FILE:ac3gui_qmltests>"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${AC3_QT_BIN_DIR}/../plugins/platforms/qoffscreen$<$<CONFIG:Debug>:d>.dll"
                "$<TARGET_FILE_DIR:ac3gui_qmltests>/platforms/"
            COMMENT "Deploying Qt runtime dependencies for ac3gui_qmltests")
    else()
        message(WARNING
            "windeployqt not found next to Qt6 (looked in ${AC3_QT_BIN_DIR}); "
            "ac3gui_qmltests.exe will not run standalone from the build tree.")
    endif()
endif()
