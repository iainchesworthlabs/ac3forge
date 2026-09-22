#!/usr/bin/env python3
"""Assert the AC3Forge Hearth package carries a window that can start,
and the notices for what it ships.

The archive cpack produces for the `hearth` component (cmake/Packaging.cmake)
is Hearth's release asset - the desktop reference player
(planning/hearth-reference-player.md, phase A7). Its failure mode is not an
empty file - it is a plausible-looking archive holding ac3hearth.exe with no
Qt beside it, which installs and then does not start, because the Qt deploy
script (apps/hearth/ui/CMakeLists.txt) was filed under the wrong CPack
component or did not run. That is what this checks, from CI
(.github/workflows/_ci-windows.yml, _ci-macos.yml, _ci-linux.yml) and from a
local `cpack --preset pack-windows-msvc` run just the same:

    python tools/ci/check_hearth_package.py packages/ac3forge-hearth-*.zip
    python tools/ci/check_hearth_package.py packages/ac3forge-hearth-*-Linux-*.tar.gz
    python tools/ci/check_hearth_package.py packages/ac3forge-hearth-*-Darwin.zip

The layout it expects is the one qt_generate_deploy_qml_app_script()
produces - the same mechanism apps/crucible uses and
tools/ci/check_crucible_package.py already checks: on Windows, the binary in
bin/ beside a qt.conf whose `Prefix = ..` sends Qt to the sibling plugins/,
qml/ and translations/ directories; on macOS, Qt deployed inside
ac3hearth.app itself (Contents/Frameworks/, Contents/PlugIns/ and
Contents/Resources/qml/).

The second thing it checks is the content of NOTICES.txt
(apps/hearth/notices/notices.cmake). Hearth's notices are simpler than
Crucible's - no driver, no Quick 3D room, no platform that gets its own
extra third-party library - but carry the same failure mode: a file whose Qt
section is missing where the platform bundles Qt (Windows, macOS - a Qt
LGPL-compliance gap PR #816 fixed once already) or present where it does not
(Linux, which links the system's Qt and ships none), or whose cpp-httplib/
Mbed TLS/mdns/libFLAC/Opus/Sendspin-time-filter sections exist but carry an
unfilled version because the vcpkg SPDX lookup that fills them silently
returned nothing.

The third is a negative, forward-looking rather than presently exercised:
apps/hearth/ui/CMakeLists.txt runs no StripQtTestDeployment pass yet, because
there is no ac3hearth_qmltests target for Qt's qmlimportscanner to pull
QtTest in for (see that file's own comment). apps/crucible/CMakeLists.txt hit
exactly this leak once qmlimportscanner had a tst_*.qml to find, and
cmake/StripQtTestDeployment.cmake is what removes it there - see
check_crucible_package.py's own comment on FORBIDDEN_WINDOWS_QT_TEST_QML for
the mechanism. Hearth runs no such pass yet because it has nothing yet to
strip; this is asserted anyway, now, so the day a qmltests target lands (the
same windeployqt-chain comment in apps/hearth/ui/CMakeLists.txt that already
expects it), this check is already watching rather than something an audit
discovers later.
"""

from __future__ import annotations

import re
import sys
import tarfile
import zipfile

NOTICES_TXT = "NOTICES.txt"

# What has to be there for the window to exist and start at all: itself, the
# qt.conf that makes the layout resolve, and the Windows platform plugin
# without which Qt aborts on launch. Then the notices and the licence, at
# the archive root beside bin/ (apps/hearth/ui/CMakeLists.txt's install
# rules) - no driver, no console runner: Hearth has neither.
REQUIRED = (
    "bin/ac3hearth.exe",
    "bin/qt.conf",
    "plugins/platforms/qwindows.dll",
    NOTICES_TXT,
    "LICENSE.txt",
)

# Qt's test module is not in the package - forward-looking, unlike the rest
# of this file (see the module docstring's third paragraph). Nothing
# currently pulls it in, so this rule cannot yet fail a real build; it is
# here so it does not have to be invented later, under pressure, once
# something does.
FORBIDDEN_WINDOWS_QT_TEST_QML = "qml/QtTest/"
FORBIDDEN_WINDOWS_QT_TEST_DLLS = ("qt6test.dll", "qt6testd.dll",
                                  "qt6quicktest.dll", "qt6quicktestd.dll")

# The Linux archive is a different shape and a different claim, the same
# split check_crucible_package.py's own REQUIRED_LINUX documents: no bundled
# Qt - the system's own loader finds it - so nothing about qt.conf or a
# platform plugin applies. What has to be there is the binary under bin/ and
# what a freedesktop menu needs under share/: the launcher, the AppStream
# record, and the icon in the hicolor theme at the two sizes the window
# embeds; and under share/doc/<package>/ the notices, the licence, and the
# notices once more as `copyright`, the file dpkg and lintian expect and
# CPack's DEB generator never writes. The .tar.gz mirrors the install tree
# the .deb and .rpm carry, the same reason check_crucible_package.py reads
# it rather than either of those.
NOTICES_LINUX_MEMBER = "share/doc/ac3forge-hearth/NOTICES.txt"
REQUIRED_LINUX = (
    "bin/ac3hearth",
    "share/applications/ac3hearth.desktop",
    "share/metainfo/ac3hearth.metainfo.xml",
    "share/icons/hicolor/256x256/apps/ac3hearth.png",
    "share/icons/hicolor/32x32/apps/ac3hearth.png",
    NOTICES_LINUX_MEMBER,
    "share/doc/ac3forge-hearth/LICENSE.txt",
    "share/doc/ac3forge-hearth/copyright",
)

# The macOS archive is a bundle rather than a folder of DLLs: no qt.conf, no
# plugins/ beside bin/ - Qt is deployed inside ac3hearth.app itself
# (Contents/Frameworks/, Contents/PlugIns/ and Contents/Resources/qml/, the
# same layout ac3crucible.app uses - see check_crucible_package.py's own
# REQUIRED_MACOS). QtCore.framework stands in for "Qt was actually deployed
# into Frameworks/": every Qt6 application links Core, so its absence means
# the deploy step did not run - the same role libqcocoa.dylib plays for
# PlugIns/.
REQUIRED_MACOS = (
    "ac3hearth.app/Contents/MacOS/ac3hearth",
    "ac3hearth.app/Contents/Info.plist",
    "ac3hearth.app/Contents/PlugIns/platforms/libqcocoa.dylib",
    "ac3hearth.app/Contents/Frameworks/QtCore.framework/QtCore",
    NOTICES_TXT,
    "LICENSE.txt",
)

# Qt's test module leaking into a macOS bundle - the same forward-looking
# rule as FORBIDDEN_WINDOWS_QT_TEST_QML above and by the same reasoning; see
# check_crucible_package.py's own comment on FORBIDDEN_MACOS_QT_TEST_QML for
# the three shapes this can take.
FORBIDDEN_MACOS_QT_TEST_QML = "ac3hearth.app/Contents/Resources/qml/QtTest/"
FORBIDDEN_MACOS_QT_TEST_FILES = ("libquicktestplugin.dylib",)
FORBIDDEN_MACOS_QT_TEST_FRAMEWORKS = ("QtTest.framework/", "QtQuickTest.framework/")

# What NOTICES.txt has to say everywhere: apps/hearth/notices/notices.cmake
# includes these fragments (apps/hearth/notices/fragments/) on every
# platform alike - cpp-httplib, Mbed TLS, mdns, libFLAC and Opus each carry a
# version token read from vcpkg's SPDX record, and Sendspin's time filter
# carries none (it is pinned to a source commit, not a port version), so it
# gets a plain presence check instead. A bare name is a weak check on its
# own - it would still pass a section whose version token substituted to
# nothing, the same silent-empty-string failure
# check_crucible_package.py's own QT_VERSION_PATTERN comment describes for
# Qt - so each pattern anchors the name to a digit immediately after it
# rather than just asking that the name appear somewhere in the file.
THIRDPARTY_VERSIONED = (
    ("cpp-httplib", re.compile(r"cpp-httplib \d")),
    ("Mbed TLS", re.compile(r"Mbed TLS \d")),
    ("mdns", re.compile(r"mdns \d")),
    ("libFLAC", re.compile(r"libFLAC \d")),
    ("Opus", re.compile(r"Opus \d")),
)
TIME_FILTER_MARKER = "Sendspin time filter"

# The Qt section (apps/hearth/notices/fragments's qt-bundled entry comes
# from apps/crucible/notices/fragments/qt-bundled.txt, shared rather than
# copied - that file's own header says why): present with a filled version
# and a source URL exactly where the package bundles Qt (Windows, macOS -
# PR #816 fixed this section going missing there), and absent on Linux,
# which links the system's Qt and ships none
# (apps/hearth/notices/notices.cmake's own `if(... AND (WIN32 OR APPLE))`
# guard on inserting the qt-bundled fragment - mirrored here as the
# `qt_bundled` parameter below so the two cannot silently drift apart).
NOTICES_QT_REQUIRED = ("GNU LESSER GENERAL PUBLIC LICENSE", "download.qt.io/archive/qt/")
QT_VERSION_PATTERN = re.compile(r"Qt 6\.\d+")


def check_notices(text: str, qt_bundled: bool) -> list[str]:
    """One problem per rule the notices text breaks.

    `qt_bundled` is whether this platform's package carries Qt at all
    (Windows, macOS) or not (Linux) - there is no payload to cross-check it
    against the way check_crucible_package.py cross-checks Quick 3D, because
    Hearth's Qt bundling is a platform decision, not a per-build one.
    """
    problems = [
        f"NOTICES.txt does not mention {name} with a filled version - either the section is "
        "missing or its version token substituted to nothing"
        for name, pattern in THIRDPARTY_VERSIONED if not pattern.search(text)
    ]
    if TIME_FILTER_MARKER not in text:
        problems.append(f"NOTICES.txt does not mention {TIME_FILTER_MARKER!r}")
    if qt_bundled:
        problems += [
            f"NOTICES.txt does not mention {phrase!r}"
            for phrase in NOTICES_QT_REQUIRED if phrase not in text
        ]
        if not QT_VERSION_PATTERN.search(text):
            problems.append(
                "NOTICES.txt names no Qt 6.x version - the configure-time token was not filled"
            )
    else:
        problems += [
            f"NOTICES.txt mentions {phrase!r}, but this platform's package bundles no Qt"
            for phrase in NOTICES_QT_REQUIRED if phrase in text
        ]
        if QT_VERSION_PATTERN.search(text):
            problems.append(
                "NOTICES.txt names a Qt 6.x version, but this platform's package bundles no Qt - "
                "apps/hearth/notices/notices.cmake should not have inserted the qt-bundled "
                "fragment here"
            )
    return problems


def check_linux(path: str) -> int:
    # CPack may or may not put a top-level package directory in the tarball
    # (CPACK_INCLUDE_TOPLEVEL_DIRECTORY; the component archives here do
    # not). Normalise to an install prefix either way - the same fix
    # check_crucible_package.py's own check_linux() carries, and the same
    # reason: stripping unconditionally turned bin/ac3hearth into ac3hearth
    # and reported everything missing on a correct archive.
    install_dirs = ("bin/", "share/", "lib/", "include/", "libexec/")
    with tarfile.open(path) as archive:
        names = []
        notices = None
        for member in archive.getmembers():
            name = member.name.removeprefix("./")
            if not name.startswith(install_dirs) and "/" in name:
                name = name.split("/", 1)[1]
            names.append(name)
            if name == NOTICES_LINUX_MEMBER and member.isfile():
                stream = archive.extractfile(member)
                if stream is not None:
                    notices = stream.read().decode("utf-8")
        total_mb = round(sum(m.size for m in archive.getmembers()) / 1048576)
    print(f"{path}: {len(names)} entries, {total_mb} MB unpacked")
    problems = [f"missing {name}" for name in REQUIRED_LINUX if name not in names]
    if notices is not None:
        problems += check_notices(notices, qt_bundled=False)
    for problem in problems:
        print(f"::error::{problem}")
    if problems:
        return 1
    print("ok: the Linux package holds the window, the menu entry and a notices file written for "
          "it, and bundles no Qt of its own")
    return 0


def check_windows(path: str) -> int:
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        total_mb = round(sum(info.file_size for info in archive.infolist()) / 1048576)
        notices = archive.read(NOTICES_TXT).decode("utf-8") if NOTICES_TXT in names else None

    print(f"{path}: {len(names)} entries, {total_mb} MB unpacked")

    problems = [f"missing {name}" for name in REQUIRED if name not in names]
    problems += [
        f"the package carries Qt's test module ({name}): nothing a user runs loads it - "
        "apps/hearth/ui/CMakeLists.txt has no ac3hearth_qmltests target yet, so nothing should "
        "have pulled this in (see this script's own header for what to check once one lands)"
        for name in names
        if name.startswith(FORBIDDEN_WINDOWS_QT_TEST_QML)
        or name.rsplit("/", 1)[-1].lower() in FORBIDDEN_WINDOWS_QT_TEST_DLLS
    ]
    if notices is not None:
        problems += check_notices(notices, qt_bundled=True)
    for problem in problems:
        print(f"::error::{path}: {problem}")
    if problems:
        return 1
    print("ok: the Windows package holds the window, its Qt, the licence and a notices file "
          "written for it, and no Qt Test")
    return 0


def check_macos(path: str) -> int:
    # Still a ZIP - cmake/Packaging.cmake's CPACK_GENERATOR starts with
    # "ZIP" on every platform, DragNDrop (the .dmg) is a macOS-only addition
    # to it and only ever carries the whole, monolithic, every-app installer
    # image, never this component alone (cmake/CPackProjectConfig.cmake
    # forces it monolithic) - see check_crucible_package.py's own
    # check_macos() for the same reasoning. main() tells this apart from the
    # Windows zip by the bundle inside, not the extension.
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        total_mb = round(sum(info.file_size for info in archive.infolist()) / 1048576)
        notices = archive.read(NOTICES_TXT).decode("utf-8") if NOTICES_TXT in names else None

    print(f"{path}: {len(names)} entries, {total_mb} MB unpacked")

    problems = [f"missing {name}" for name in REQUIRED_MACOS if name not in names]
    problems += [
        f"the package carries Qt's test module ({name}): nothing a user runs loads it - "
        "apps/hearth/ui/CMakeLists.txt has no ac3hearth_qmltests target yet, so nothing should "
        "have pulled this in (see this script's own header for what to check once one lands)"
        for name in names
        if name.startswith(FORBIDDEN_MACOS_QT_TEST_QML)
        or name.rsplit("/", 1)[-1].lower() in FORBIDDEN_MACOS_QT_TEST_FILES
        or any(f"/Frameworks/{fw}" in name for fw in FORBIDDEN_MACOS_QT_TEST_FRAMEWORKS)
    ]
    if notices is not None:
        problems += check_notices(notices, qt_bundled=True)
    for problem in problems:
        print(f"::error::{path}: {problem}")
    if problems:
        return 1
    print("ok: the macOS package holds the window, its Qt, the licence and a notices file "
          "written for it, and no Qt Test")
    return 0


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"usage: {argv[0]} <archive.zip>", file=sys.stderr)
        return 2
    path = argv[1]
    if path.endswith((".tar.gz", ".tgz", ".tar.xz")):
        return check_linux(path)
    # Windows and macOS are both this shape - cmake/Packaging.cmake names
    # them ac3forge-hearth-<version>-<system>.zip alike, CPACK_SYSTEM_NAME
    # being the only difference (win64/win-arm64/win32 vs Darwin) - so what
    # tells them apart is a top-level "*.app/" entry, the bundle only a
    # macOS archive carries, not the filename.
    with zipfile.ZipFile(path) as archive:
        is_macos = any(name.split("/", 1)[0].endswith(".app") for name in archive.namelist())
    return check_macos(path) if is_macos else check_windows(path)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
