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
# Windows AND macOS: both platforms' packages carry their own Qt (Linux finds
# the system's, so this file never runs there - apps/gui's and apps/crucible's
# install(SCRIPT) calls each sit inside a WIN32 OR APPLE deploy block). A
# macOS .app keeps its deployed Qt inside the bundle rather than beside the
# executable - Contents/PlugIns/ for QML plugins, Contents/Resources/qml/ for
# QML modules - but qmlimportscanner's recursion into tests/qml/tst_*.qml is
# not platform-specific, so qt_generate_deploy_qml_app_script() stages
# Contents/PlugIns/libquicktestplugin.dylib and
# Contents/Resources/qml/QtTest/{libquicktestplugin.dylib,qmldir} into the
# .app exactly as it stages Qt6Test.dll and qml/QtTest/ on Windows. Confirmed
# 2026-09-16 against a real packages-macos-llvm CI artifact built from main
# (ac3forge-0.0.0-Darwin.zip): both paths were there, and nothing under
# Contents/Frameworks/ was named Test or QuickTest - the QML plugin is the
# only carrier, unlike Windows where windeployqt also resolves the plugin's
# own Qt6Test.dll/Qt6QuickTest.dll dependencies into bin/.
#
# WHERE THE BUNDLE NAME COMES FROM
#
# install(SCRIPT) runs this file as its own `cmake -P` pass at install time -
# after the generate step, in a scope that inherits none of the calling
# CMakeLists.txt's variables or target properties, and cannot evaluate a
# generator expression ($<TARGET_FILE_NAME:...> and friends only work at
# generate time). So the caller cannot just write
# ${CMAKE_INSTALL_PREFIX}/ac3gui.app inline here the way
# qt_generate_deploy_qml_app_script()'s OWN generated script can (it bakes
# the resolved path in via file(GENERATE), which this file is not). What DOES
# cross that boundary is a variable set by an install(CODE) call placed
# immediately before install(SCRIPT): CMake concatenates every install() rule
# from one directory into a single per-directory cmake_install.cmake,
# executed top to bottom as one script, so a set() from that install(CODE)
# is still visible when this file's include() runs a moment later. apps/gui
# and apps/crucible each do exactly that - install(CODE "set(_ac3_macos_
# bundle_name \"ac3gui\")") (or "ac3crucible") right before their own
# install(SCRIPT .../StripQtTestDeployment.cmake ...) - with the SAME
# COMPONENT keyword on both calls, since a component-scoped install (`cmake
# --install --component runtime`, what the package-macos-universal job's
# lipo-merge actually runs) executes only the install() rules tagged for
# that component; a mismatched COMPONENT would silently leave the variable
# unset for exactly the install that matters. The literal target name, not
# MACOSX_BUNDLE_BUNDLE_NAME, is what is passed: that variable only feeds
# CFBundleName inside Info.plist: the CI artifact above is ac3gui.app despite
# ac3crucible's MACOSX_BUNDLE_BUNDLE_NAME being "Crucible", not
# ac3crucible's own target name. A plain literal, not a generator
# expression, because the target name has no per-config variation on either
# target - nothing here needs file(GENERATE).
#
# WHY THE PlugIns FILE'S GUARD IS EXISTS OR IS_SYMLINK, NOT EXISTS ALONE
#
# Every other removal in this file - including the macOS qml/QtTest
# directory below - is measured with if(EXISTS) purely to keep the STATUS
# message honest, since file(REMOVE)/file(REMOVE_RECURSE) already silently
# no-op on a path that was never there. if(EXISTS) alone is not safe for
# Contents/PlugIns/libquicktestplugin.dylib today: qt_generate_deploy_qml_app_script()
# on Homebrew's Qt6 deploys every QtQuick/QML plugin in this bundle - QtTest's
# included - as a relative symlink back into Homebrew's own Cellar (a
# separate, longstanding Homebrew/Qt6 defect, tracked and being fixed
# independently in PR #728 - unrelated to the qmlimportscanner over-broad
# recursion this file exists for), dangling from the moment it is created
# since cpack stages the bundle under the CI workspace, not /usr/local.
# if(EXISTS) resolves a symlink to its target before answering, so it
# reports a DANGLING symlink as absent - checked directly under WSL with
# CMake 4.2: a symlink to a real file reads EXISTS TRUE, the identical
# symlink repointed at a missing target reads EXISTS FALSE, even though the
# symlink itself still sits right there on disk. Gating this removal on
# if(EXISTS) alone would make it a silent no-op for exactly the shape this
# plugin currently has - and start working again, unremarked, the moment
# PR #728 fixes the symlink's target, which is the opposite of a check worth
# having. IS_SYMLINK does not share that blind spot (confirmed the same way:
# TRUE for both the live and the dangling symlink, FALSE only when the path
# truly is not there), so the guard is EXISTS OR IS_SYMLINK; the file(REMOVE)
# itself needs no extra care either way - it unlinks a dangling symlink by
# its own path without following it, same as any other file.
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

# macOS: _ac3_macos_bundle_name arrives via an install(CODE) set() just
# before this file's install(SCRIPT) - see the header comment for why that is
# the only way this file learns the bundle's name at all, and only DEFINED on
# APPLE since Windows callers never set it.
if(APPLE AND DEFINED _ac3_macos_bundle_name)
    set(_ac3_macos_bundle "${_ac3_prefix}/${_ac3_macos_bundle_name}.app")
    set(_ac3_macos_plugin "${_ac3_macos_bundle}/Contents/PlugIns/libquicktestplugin.dylib")

    # EXISTS OR IS_SYMLINK, not EXISTS alone - see the header comment on why
    # this plugin's current dangling-symlink shape under Homebrew's Qt6
    # reads as absent to EXISTS but not to IS_SYMLINK.
    if(EXISTS "${_ac3_macos_plugin}" OR IS_SYMLINK "${_ac3_macos_plugin}")
        message(STATUS "Removing Qt Test from the package: ${_ac3_macos_plugin}")
        file(REMOVE "${_ac3_macos_plugin}")
    endif()

    if(EXISTS "${_ac3_macos_bundle}/Contents/Resources/qml/QtTest")
        message(STATUS "Removing Qt Test from the package: ${_ac3_macos_bundle}/Contents/Resources/qml/QtTest")
        file(REMOVE_RECURSE "${_ac3_macos_bundle}/Contents/Resources/qml/QtTest")
    endif()
endif()

# unset(_ac3_macos_bundle_name) here is not just the same tidiness as the
# other unsets below - it is load-bearing. install(SCRIPT)/install(CODE) are
# plain include()s into one flat, shared variable scope that runs from the
# top-level cmake_install.cmake down through every subdirectory's own, in
# add_subdirectory() order - unlike the CONFIGURE step, include() opens no
# new scope. A full (non-component-scoped) install, or any CPack generator
# that installs every component in one pass, walks apps/gui's directory and
# apps/crucible's in that one process: without this unset, ac3gui's bundle
# name would still be sitting in this variable when apps/crucible's own
# install(CODE) set() has not run yet for whatever reason, or when a third
# caller is added later and forgets to set it at all. Confirmed with a
# throwaway two-target CMakeLists.txt: a value set by one directory's
# install(CODE) was still readable, unmodified, from a second, sibling
# directory's install(SCRIPT) later in the same install.
unset(_ac3_file)
unset(_ac3_qt_test_files)
unset(_ac3_macos_bundle)
unset(_ac3_macos_plugin)
unset(_ac3_macos_bundle_name)
unset(_ac3_prefix)
