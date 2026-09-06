# ---------------------------------------------------------------------------
# Install-time removal of Qt's test module from a packaged application.
#
# Run with install(SCRIPT) immediately AFTER the qt_generate_deploy_qml_app_script()
# script of the target it belongs to (apps/crucible/CMakeLists.txt,
# apps/gui/CMakeLists.txt), so it deletes what that script has just written.
#
# WHY THIS EXISTS
#
# Measured 2026-09-06 against C:/Qt/6.8.3/msvc2022_64 and the build tree's own
# generated scan results. Qt's install-time deployment runs, in this order:
#
#   qt6_deploy_qml_imports(TARGET <app>)      # copies whole QML modules
#   qt6_deploy_runtime_dependencies(EXECUTABLE $<TARGET_FILE:<app>>
#                                   ADDITIONAL_MODULES <the plugins that found>)
#
# and the QML module list it works from is whatever qmlimportscanner recorded
# for the target at build time. Qt runs that scanner with
#
#   -rootPath $<TARGET_PROPERTY:<app>,SOURCE_DIR>
#
# hard-wired (Qt6QmlMacros.cmake, _qt_internal_scan_qml_imports; there is no
# keyword, property or variable to change it, and the target's SOURCE_DIR is
# read-only). That root is scanned RECURSIVELY for .qml files - so it also
# reads the Qt Quick Test suites that live under the same application
# directory (apps/crucible/ui/tests/qml/tst_*.qml, apps/gui/tests/qml/), every
# one of which opens with `import QtTest`. The scanner cannot tell an
# application's QML from its tests': they are both .qml under the root it was
# given. Confirmed in the generated files themselves - both
# .qt/qml_imports/<app>_build.cmake list a QtTest entry naming
# C:/Qt/6.8.3/msvc2022_64/qml/QtTest and quicktestplugin - so the deploy
# copies qml/QtTest/ into the package, hands quicktestplugin.dll to
# windeployqt as an extra binary to resolve, and windeployqt then brings
# Qt6Test.dll and Qt6QuickTest.dll into bin/ beside the application. Nothing a
# user runs loads any of it; only the Qt Quick Test binary does, and that
# binary is never install()'d.
#
# WHAT THIS IS NOT
#
# It is NOT about the test executable sharing the build tree's bin/ with the
# application. That was the first theory and it is wrong: windeployqt resolves
# the dependencies of the binary it is given and does not read the rest of the
# directory - checked directly with
#
#   windeployqt <build>/bin/ac3gui.exe --dry-run --list source ...
#
# in a bin/ that already held Qt6Test.dll and Qt6QuickTest.dll, which listed
# neither. So the Qt Quick Test binaries stay where their own CMakeLists put
# them (beside the application, so one windeployqt pass serves both), and
# nothing about where they land needs to move to fix this.
#
# WHY REMOVAL RATHER THAN NOT DEPLOYING IT
#
# qmlimportscanner has an `-exclude <directory>` option, and Qt's CMake never
# passes it. Short of moving every tst_*.qml out from under the application's
# own directory - splitting each suite from the CMakeLists.txt and main.cpp
# that define it, and leaving the next .qml added anywhere under apps/<app>/
# free to put the payload back silently - deleting the deployed copy is the
# surgical fix. tools/ci/check_crucible_package.py asserts the absence on the
# Crucible zip in CI, so this staying wired is checked rather than assumed.
#
# Windows only in effect: it is the platform whose packages carry their own Qt
# at all (Linux finds the system's), and a macOS .app keeps its deployed Qt
# inside the bundle - Contents/Frameworks/ and Contents/Resources/qml/ - which
# is not where any path below looks. apps/gui narrows its call to WIN32 for
# exactly that reason.
#
# apps/crucible's is NOT narrowed, and now reaches macOS: the top-level
# CMakeLists.txt adds Crucible under WIN32 OR APPLE OR LINUX since the macOS
# platform half landed on 2026-09-06, so this script's install(SCRIPT) sits
# inside a WIN32 OR APPLE deploy block whose APPLE arm both macOS CI legs have
# now configured through. Neither has reached an install step yet, so this file
# has never executed on macOS, and what it would do there is nothing: every
# path below is the Windows package layout, so each if(EXISTS) is false and a
# .app keeps its deployed Qt Test, the same way ac3gui's .dmg does. That is a
# gap rather than a hazard - it
# removes nothing it should not - and it stays open for the same reason
# apps/gui's does: nobody here has a Mac to check where those files land in a
# bundle. It is also not reachable through CPack yet, since
# cmake/Packaging.cmake still adds the crucible component only under
# WIN32 OR LINUX. Narrowing this call to WIN32 would match apps/gui and lose
# nothing; leaving it is what lets a macOS bundle be fixed here once someone
# can see one.
# ---------------------------------------------------------------------------

# $ENV{DESTDIR} the way every hand-written install script has to: install(FILES)
# prepends it for you, install(SCRIPT) does not, and CPack's own staging install
# does not set it at all - so this is correct under both.
set(_ac3_prefix "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}")

# "bin" rather than ${CMAKE_INSTALL_BINDIR}: an install script sees neither the
# cache nor the configure-time scope, GNUInstallDirs hard-codes "bin" on Windows
# anyway, and tools/ci/check_crucible_package.py already spells the same path
# out for the same reason.
set(_ac3_qt_test_files
    "${_ac3_prefix}/bin/Qt6Test.dll"
    "${_ac3_prefix}/bin/Qt6Testd.dll"
    "${_ac3_prefix}/bin/Qt6QuickTest.dll"
    "${_ac3_prefix}/bin/Qt6QuickTestd.dll")

foreach(_ac3_file IN LISTS _ac3_qt_test_files)
    if(EXISTS "${_ac3_file}")
        message(STATUS "Removing Qt Test from the package: ${_ac3_file}")
        file(REMOVE "${_ac3_file}")
    endif()
endforeach()

if(EXISTS "${_ac3_prefix}/qml/QtTest")
    message(STATUS "Removing Qt Test from the package: ${_ac3_prefix}/qml/QtTest")
    file(REMOVE_RECURSE "${_ac3_prefix}/qml/QtTest")
endif()

unset(_ac3_file)
unset(_ac3_qt_test_files)
unset(_ac3_prefix)
