#!/usr/bin/env python3
"""Assert the AC3Forge Crucible's package carries a window that can start,
and the notices for what it ships.

The archive cpack produces for the `crucible` component (cmake/Packaging.cmake)
is the demo's release asset. Its failure mode is not an empty file - it is a
plausible-looking archive holding ac3crucible.exe with no Qt beside it, which
installs and then does not start, because the Qt deploy script
(apps/crucible/CMakeLists.txt) was filed under the wrong CPack component or
did not run. That is what this checks, from CI (.github/workflows/_build.yml)
and from a local `cpack --preset pack-windows-msvc` run just the same:

    python tools/ci/check_crucible_package.py packages/ac3forge-crucible-*.zip
    python tools/ci/check_crucible_package.py packages/ac3forge-crucible-*-Linux.tar.gz

The layout it expects is the one qt_generate_deploy_qml_app_script() produces
and the existing runtime archive already uses: the binaries in bin/ beside a
qt.conf whose `Prefix = ..` sends Qt to the sibling plugins/, qml/ and
translations/ directories.

The second thing it checks is the content of NOTICES.txt, the third-party
notices apps/crucible/notices/ generates per platform at configure time. A
notices file is easy to get subtly wrong without any name going missing: one
written for the other platform (a PipeWire section in the Windows zip, the
driver's MS-PL text in a .deb), or one whose Qt Quick 3D section disagrees
with whether qml/QtQuick3D/ actually shipped. So the file is read, not just
listed: each platform has phrases it must contain and phrases it must not,
the Qt version token must have been filled, and on Windows the Quick 3D
section must be present exactly when the payload is.
"""

from __future__ import annotations

import re
import sys
import tarfile
import zipfile

# What has to be there for the window to exist and start at all: itself, the
# console runner beside it, the driver scripts its Settings page points at,
# the qt.conf that makes the layout resolve, and the Windows platform plugin
# without which Qt aborts on launch. Then the notices and the licence, at the
# archive root beside bin/ (apps/crucible/CMakeLists.txt's install rules).
REQUIRED = (
    "bin/ac3crucible.exe",
    "bin/ac3crucible-run.exe",
    "bin/driver/install.ps1",
    "bin/driver/remove.ps1",
    "bin/driver/NullSinkDevice.ps1",
    "bin/qt.conf",
    "plugins/platforms/qwindows.dll",
    "NOTICES.txt",
    "LICENSE.txt",
)

# QML modules the window imports directly. QtQuick3D earns its own line: the
# room's 3D view is optional at build time (apps/crucible/CMakeLists.txt skips
# it with a warning when the Quick3D module is absent from the kit), so its
# absence here means the package shipped a 2D-only room and nothing else
# would have said so.
REQUIRED_QML = ("qml/QtQuick/", "qml/QtQuick3D/")

# The Linux archive is a different shape and a different claim. There is no
# bundled Qt - the system's own loader finds it - so nothing about qt.conf or
# a platform plugin applies. What has to be there is the two binaries under
# bin/ and what a freedesktop menu needs under share/: the launcher, the
# AppStream record, and the icon in the hicolor theme; and under
# share/doc/<package>/ the notices, the licence, and the notices once more
# as `copyright`, the file dpkg and lintian expect and CPack's DEB generator
# never writes. And one thing must NOT be there: the driver's PowerShell
# scripts, which are Windows and would only ever confuse a reader of a .deb.
# The .tar.gz mirrors the install tree the .deb and .rpm carry, and Python
# can read it without any of dpkg.
NOTICES_LINUX_MEMBER = "share/doc/ac3forge-crucible/NOTICES.txt"
REQUIRED_LINUX = (
    "bin/ac3crucible",
    "bin/ac3crucible-run",
    "share/applications/ac3crucible.desktop",
    "share/metainfo/ac3crucible.metainfo.xml",
    "share/icons/hicolor/256x256/apps/ac3crucible.png",
    "share/icons/hicolor/32x32/apps/ac3crucible.png",
    NOTICES_LINUX_MEMBER,
    "share/doc/ac3forge-crucible/LICENSE.txt",
    "share/doc/ac3forge-crucible/copyright",
)
FORBIDDEN_LINUX = ("driver/install.ps1", "driver/remove.ps1", "driver/NullSinkDevice.ps1")

# What NOTICES.txt has to say on each platform, and what it must not: the
# phrases are the ones each fragment under apps/crucible/notices/fragments/
# carries and no other fragment does. The Windows zip conveys Qt, so it must
# reproduce the LGPL and say where Qt's source is; it carries the driver's
# scripts, so it must reproduce the MS-PL; it must not credit a library only
# the Linux build links. The Linux tarball is the converse. Both compile
# {fmt} in and embed the OFL faces.
NOTICES_WINDOWS = (
    "GNU LESSER GENERAL PUBLIC LICENSE",
    "download.qt.io/archive/qt/",
    "Microsoft Public License",
    "{fmt}",
    "SIL OPEN FONT LICENSE",
)
NOTICES_NOT_WINDOWS = ("libpipewire",)
NOTICES_LINUX = ("libpipewire", "{fmt}", "SIL OPEN FONT LICENSE")
NOTICES_NOT_LINUX = ("Microsoft Public License", "GNU LESSER GENERAL PUBLIC LICENSE")
# The phrase only the qt-quick3d fragment carries, on either platform.
QUICK3D_MARKER = "Qt Quick 3D"
# The configure-time version token, filled: "Qt 6.8.3", "Qt 6.10.0".
QT_VERSION_PATTERN = re.compile(r"Qt 6\.\d+")


def check_notices(text: str, required: tuple[str, ...], forbidden: tuple[str, ...],
                  ships_quick3d: bool | None) -> list[str]:
    """One problem per rule the notices text breaks.

    `ships_quick3d` is whether the archive carries qml/QtQuick3D/; None skips
    that cross-check, for an archive that carries no Qt at all (Linux), where
    the section's presence is the build's business and not the payload's.
    """
    problems = [
        f"NOTICES.txt does not mention {phrase!r}" for phrase in required if phrase not in text
    ]
    problems += [f"NOTICES.txt mentions {phrase!r}, which belongs to the other platform's package"
                 for phrase in forbidden if phrase in text]
    if not QT_VERSION_PATTERN.search(text):
        problems.append(
            "NOTICES.txt names no Qt 6.x version - the configure-time token was not filled"
        )
    if ships_quick3d is not None:
        named = QUICK3D_MARKER in text
        if ships_quick3d and not named:
            problems.append(
                f"the package ships qml/QtQuick3D/ but NOTICES.txt does not name {QUICK3D_MARKER}"
            )
        elif named and not ships_quick3d:
            problems.append(
                f"NOTICES.txt names {QUICK3D_MARKER} but the package ships no qml/QtQuick3D/"
            )
    return problems


def check_linux(path: str) -> int:
    # CPack may or may not put a top-level package directory in the tarball
    # (CPACK_INCLUDE_TOPLEVEL_DIRECTORY; the component archives here do not).
    # Normalise to an install prefix either way: keep a name that already
    # starts at an install directory, and strip one leading segment from one
    # that does not. Stripping unconditionally turned bin/ac3crucible-run into
    # ac3crucible-run and reported everything missing on a correct archive.
    # The notices member is looked up by the same normalised name, so the
    # toplevel-directory choice cannot move it out of reach.
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
    problems += [f"Windows driver script shipped in a Linux package: {name}"
                 for name in names for pattern in FORBIDDEN_LINUX if name.endswith(pattern)]
    if notices is not None:
        problems += check_notices(notices, NOTICES_LINUX, NOTICES_NOT_LINUX, ships_quick3d=None)
    for problem in problems:
        print(f"::error::{problem}")
    if problems:
        return 1
    print("ok: the Linux package holds the window, the runner, the menu entry and a notices file "
          "written for it, and no driver scripts")
    return 0


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"usage: {argv[0]} <archive.zip>", file=sys.stderr)
        return 2
    path = argv[1]
    if path.endswith((".tar.gz", ".tgz", ".tar.xz")):
        return check_linux(path)
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        total_mb = round(sum(info.file_size for info in archive.infolist()) / 1048576)
        notices = archive.read("NOTICES.txt").decode("utf-8") if "NOTICES.txt" in names else None

    qml = [n for n in names if n.startswith("qml/")]
    print(f"{path}: {len(names)} entries, {total_mb} MB unpacked, {len(qml)} QML files")

    problems = [f"missing {name}" for name in REQUIRED if name not in names]
    problems += [
        f"no {prefix} module - the Qt deploy step did not bring what the window imports"
        for prefix in REQUIRED_QML
        if not any(n.startswith(prefix) for n in names)
    ]
    if notices is not None:
        ships_quick3d = any(n.startswith("qml/QtQuick3D/") for n in names)
        problems += check_notices(notices, NOTICES_WINDOWS, NOTICES_NOT_WINDOWS, ships_quick3d)
    for problem in problems:
        print(f"::error::{path}: {problem}")
    if problems:
        return 1
    print("ok: the Windows package holds the window, its Qt, the driver scripts, the licence and a "
          "notices file written for it")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
