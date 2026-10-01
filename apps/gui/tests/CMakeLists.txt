# ---------------------------------------------------------------------------
# forge_gui_qmltests - Qt Quick Test coverage for the interactive surfaces in
# ../qml/. Drives the REAL EncoderController, the same singleton forge-gui
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
# This embeds a second, independent copy of the ForgeGui QML module - not a
# shared library with forge-gui - for the same WHOLE_ARCHIVE reason
# ../CMakeLists.txt gives for embedding it directly into the executable
# rather than a backing plugin. AC3_QML_FILES is the same list forge-gui's own
# qt_add_qml_module call uses, set once in ../CMakeLists.txt, so the two
# embeddings cannot list a different set of files without both being edited.
# ---------------------------------------------------------------------------
find_package(Qt6 6.5 REQUIRED COMPONENTS QuickTest)

qt_add_executable(forge_gui_qmltests
    tests/qml_test_main.cpp
    encoder_controller.cpp
    encoder_controller.hpp
    qc_controller.cpp
    qc_controller.hpp
    object_decode_controller.cpp
    object_decode_controller.hpp
    stream_player_controller.cpp
    stream_player_controller.hpp
    background_jobs.hpp
    channel_geometry.cpp
    channel_geometry.hpp
    gui_diagnostics.cpp
    gui_diagnostics.hpp
    ac4_encode_settings.cpp
    ac4_encode_settings.hpp
    ac4_presentations.cpp
    ac4_presentations.hpp
    ../common/ac4_sync_word.hpp
    ../common/fmp4_folder_writer.cpp
    ../common/fmp4_folder_writer.hpp
    ../common/recording_sink.cpp
    ../common/recording_sink.hpp
    ../common/container_input.cpp
    ../common/container_input.hpp
    ../common/ac4_encode_core.cpp
    ../common/ac4_encode_core.hpp
    ../common/ac4_objects_core.cpp
    ../common/ac4_objects_core.hpp
    ../common/ac4_channels.hpp
    system_theme.cpp
    system_theme.hpp
    language_manager.cpp
    language_manager.hpp)

# apps/common, for the same reason ../CMakeLists.txt names it: RecordingSink
# and Fmp4FolderWriter moved there when forge's own takes started streaming
# through them (wide-layout record/live paths), and container_input.cpp joined them the same
# way for container readers (mkv/mp4/ts). This target compiles the same files in, exactly as it
# always did - qc_controller.cpp/object_decode_controller.cpp call
# ac3::apps::elementary_stream_from_bytes too, so it needs the same
# definition forge-gui's own target links.
target_include_directories(forge_gui_qmltests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../common")

qt_add_qml_module(forge_gui_qmltests
    URI ForgeGui
    VERSION 1.0
    # Both targets embed the same URI; without this they'd both compute the
    # same "<binary dir>/ForgeGui" output directory and collide - forge-gui's
    # own module lives there already.
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests/ForgeGui"
    QML_FILES ${AC3_QML_FILES})

# Forces this target's own POST_BUILD windeployqt step (below) to run only
# after forge-gui's has finished, not just after forge-gui's LINK step - confirmed
# necessary on a real "build everything" CI run: both windeployqt processes
# copy the same Qt DLLs into the same shared bin/ output directory, and two
# of them racing to write e.g. Qt6Core.dll at once fails with "Cannot create
# a file when that file already exists." A local build of one target at a
# time never triggers this, which is why it went unnoticed until now.
add_dependencies(forge_gui_qmltests forge-gui)

# ---------------------------------------------------------------------------
# GUI localisation: a second, independent .qm embedding for this target - same
# rationale as the AC3_QML_FILES embedding above, and for the same reason
# that comment gives: a shared library between the two targets would be more
# machinery than what it wraps. lrelease only, not lupdate - the QML files
# are the identical list forge-gui already scans (AC3_QML_FILES), so a second
# extraction pass would just regenerate byte-identical .ts content; running
# it twice buys nothing and doubles the target-name bookkeeping. The pseudo-
# locale QA fixture (AC3_PSEUDO_TS_FILE) is added here ONLY - see that
# variable's own comment in ../CMakeLists.txt for why production forge-gui.exe
# doesn't carry it.
#
# OUTPUT_LOCATION is set on the .ts sources between the two qt6_add_lrelease
# calls (forge-gui's own, inside qt_add_translations() above, already ran with
# it unset) rather than left at the shared default: this file is include()d
# into ../CMakeLists.txt's own scope, not a real subdirectory (see this
# file's header comment), so CMAKE_CURRENT_BINARY_DIR here is the SAME
# directory forge-gui's own .qm files already land in - without a distinct
# output location the two custom commands would both target
# ".../forge_gui_fr.qm" and Ninja would refuse the second one as "multiple
# rules generate the same output".
# ---------------------------------------------------------------------------
set_source_files_properties(${AC3_TS_FILES} ${AC3_PSEUDO_TS_FILE} PROPERTIES
    OUTPUT_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/tests")

qt6_add_lrelease(forge_gui_qmltests
    TS_FILES ${AC3_TS_FILES} ${AC3_PSEUDO_TS_FILE}
    LRELEASE_TARGET forge_gui_qmltests_lrelease
    QM_FILES_OUTPUT_VARIABLE ICLFORGE_GUI_TEST_QM_FILES)

qt6_add_resources(forge_gui_qmltests "forge_gui_qmltests_translations"
    PREFIX "/i18n"
    BASE "${CMAKE_CURRENT_BINARY_DIR}/tests"
    FILES ${ICLFORGE_GUI_TEST_QM_FILES})

# The standard Qt Quick Test hookup: QUICK_TEST_MAIN's generated main() reads
# this macro to find the tst_*.qml files at run time. -input on the ctest
# COMMAND line below is a second, redundant way to say the same thing -
# belt and suspenders, since this is the one part of the setup this project
# had no prior example of to copy from.
target_compile_definitions(forge_gui_qmltests PRIVATE
    QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/qml")

# The byte-equality suites (tst_e2e_ac4.qml) run the command line a page
# echoes through this build's own forge (qml_test_main.cpp's CliRunner), so
# the binary is built first and named here. apps/cli is configured before
# apps/gui (the root CMakeLists.txt), so the target exists by now wherever the
# build has one; without it the path is empty and the suites skip. The choice
# is made here rather than behind an #ifdef, which
# tools/checks/check_platform_macros.ps1 refuses.
if(TARGET forge)
    target_compile_definitions(forge_gui_qmltests PRIVATE
        ICLFORGE_GUI_TEST_CLI="$<TARGET_FILE:forge>")
    add_dependencies(forge_gui_qmltests forge)
else()
    target_compile_definitions(forge_gui_qmltests PRIVATE ICLFORGE_GUI_TEST_CLI="")
endif()

# Same rationale as ../CMakeLists.txt's identical loop: Qt's generated
# moc/rcc/qmlcachegen output for THIS target compiles under the strict
# warning set unless excluded, and qmlcachegen is known to emit at least one
# warning clang-cl/GCC's -Wconversion rejects (see ../CMakeLists.txt).
get_target_property(ICLFORGE_GUI_TEST_SOURCES forge_gui_qmltests SOURCES)
set(ICLFORGE_GUI_TEST_GENERATED_SOURCES "")
foreach(source IN LISTS ICLFORGE_GUI_TEST_SOURCES)
    if(source MATCHES "^${CMAKE_CURRENT_BINARY_DIR}/.*\\.(cpp|cxx|cc)$")
        list(APPEND ICLFORGE_GUI_TEST_GENERATED_SOURCES "${source}")
    endif()
endforeach()
if(ICLFORGE_GUI_TEST_GENERATED_SOURCES)
    set_source_files_properties(${ICLFORGE_GUI_TEST_GENERATED_SOURCES}
        PROPERTIES COMPILE_OPTIONS "${AC3_WARNINGS_OFF_FLAG}")
endif()

target_link_libraries(forge_gui_qmltests
    PRIVATE
        iclforge::ac3
        iclforge::audio
        iclforge::matroska
        iclforge::mp4
        iclforge::mpegts
        iclforge::ac4
        iclforge::ac4dec
        iclforge::ac4enc
        iclforge::warnings
        iclforge::fmt
        # This target recompiles encoder_controller.cpp/stream_player_controller.cpp
        # from scratch (this file's own header comment), so it needs the same
        # profiling header pair ../CMakeLists.txt's forge-gui target links -
        # otherwise a Tracy-enabled configure fails only on this target.
        iclforge::tracy
        # Inert unless ICLFORGE_ENABLE_COVERAGE is on - see cmake/Coverage.cmake:
        # linking an instrumented library gives this target only the gcov
        # runtime, not instrumentation of the controller sources it
        # recompiles, so without this a gcovr filter for apps/gui measured
        # nothing these suites ran (Crucible's qmltests link it the same way).
        iclforge::coverage
        Qt6::Core
        Qt6::Gui
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickTest
        Qt6::Concurrent)
target_include_directories(forge_gui_qmltests PRIVATE
    "$<IF:$<BOOL:${ICLFORGE_ENABLE_TRACY}>,${CMAKE_SOURCE_DIR}/src/base/variants/profiling-tracy_enabled,${CMAKE_SOURCE_DIR}/src/base/variants/profiling-tracy_disabled>")

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
# - ctest's "Start 1: forge_gui_qml_tests" with zero further output for the
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
set(ICLFORGE_GUI_QMLTESTS_ENV "QT_QPA_PLATFORM=offscreen;QT_FORCE_STDERR_LOGGING=1")
if(APPLE)
    list(APPEND ICLFORGE_GUI_QMLTESTS_ENV "QSG_RENDER_LOOP=basic")
endif()

# One ctest entry per tst_*.qml suite rather than a single forge_gui_qml_tests
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
file(GLOB ICLFORGE_GUI_QML_TEST_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/qml/tst_*.qml")

foreach(ICLFORGE_GUI_QML_TEST_FILE IN LISTS ICLFORGE_GUI_QML_TEST_FILES)
    get_filename_component(ICLFORGE_GUI_QML_TEST_NAME "${ICLFORGE_GUI_QML_TEST_FILE}" NAME_WE)
    string(REGEX REPLACE "^tst_" "" ICLFORGE_GUI_QML_TEST_SUITE "${ICLFORGE_GUI_QML_TEST_NAME}")
    set(ICLFORGE_GUI_QML_TEST_CASE "forge_gui_qml_tests_${ICLFORGE_GUI_QML_TEST_SUITE}")

    # tst_localisation_pipeline.qml is the one suite that needs a locale
    # forced deterministically rather than whatever this machine's own
    # system locale happens to be - LanguageManager::applyInitialLanguage()
    # reads ICLFORGE_GUI_LOCALE ahead of the persisted setting/system locale for
    # exactly this (language_manager.cpp), and "xx" selects the pseudo-
    # locale QA fixture (translations/forge_gui_xx.ts) every other suite has no
    # reason to load.
    set(ICLFORGE_GUI_QML_TEST_ENV "${ICLFORGE_GUI_QMLTESTS_ENV}")
    if(ICLFORGE_GUI_QML_TEST_SUITE STREQUAL "localisation_pipeline")
        list(APPEND ICLFORGE_GUI_QML_TEST_ENV "ICLFORGE_GUI_LOCALE=xx")
    endif()

    add_test(NAME ${ICLFORGE_GUI_QML_TEST_CASE}
        COMMAND forge_gui_qmltests -input "${ICLFORGE_GUI_QML_TEST_FILE}" -platform offscreen)
    set_tests_properties(${ICLFORGE_GUI_QML_TEST_CASE} PROPERTIES
        ENVIRONMENT "${ICLFORGE_GUI_QML_TEST_ENV}"
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
# already covers it, exactly as it does for forge-gui and for --smoke today.
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

        add_custom_command(TARGET forge_gui_qmltests POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E env "VCINSTALLDIR=${AC3_VCINSTALLDIR}"
                "${AC3_WINDEPLOYQT_EXECUTABLE}"
                --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/qml"
                --compiler-runtime
                "$<TARGET_FILE:forge_gui_qmltests>"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${AC3_QT_BIN_DIR}/../plugins/platforms/qoffscreen$<$<CONFIG:Debug>:d>.dll"
                "$<TARGET_FILE_DIR:forge_gui_qmltests>/platforms/"
            COMMENT "Deploying Qt runtime dependencies for forge_gui_qmltests")
    else()
        message(WARNING
            "windeployqt not found next to Qt6 (looked in ${AC3_QT_BIN_DIR}); "
            "forge_gui_qmltests.exe will not run standalone from the build tree.")
    endif()
endif()
