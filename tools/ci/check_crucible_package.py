#!/usr/bin/env python3
"""Assert the AC3Forge Crucible's package carries a window that can start.

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
"""

from __future__ import annotations

import sys
import tarfile
import zipfile

# What has to be there for the window to exist and start at all: itself, the
# console runner beside it, the driver scripts its Settings page points at,
# the qt.conf that makes the layout resolve, and the Windows platform plugin
# without which Qt aborts on launch.
REQUIRED = (
    "bin/ac3crucible.exe",
    "bin/ac3crucible-run.exe",
    "bin/driver/install.ps1",
    "bin/driver/remove.ps1",
    "bin/driver/NullSinkDevice.ps1",
    "bin/qt.conf",
    "plugins/platforms/qwindows.dll",
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
# AppStream record, and the icon in the hicolor theme. And one thing must NOT
# be there: the driver's PowerShell scripts, which are Windows and would only
# ever confuse a reader of a .deb. The .tar.gz mirrors the install tree the
# .deb and .rpm carry, and Python can read it without any of dpkg.
REQUIRED_LINUX = (
    "bin/ac3crucible",
    "bin/ac3crucible-run",
    "share/applications/ac3crucible.desktop",
    "share/metainfo/ac3crucible.metainfo.xml",
    "share/icons/hicolor/256x256/apps/ac3crucible.png",
    "share/icons/hicolor/32x32/apps/ac3crucible.png",
)
FORBIDDEN_LINUX = ("driver/install.ps1", "driver/remove.ps1", "driver/NullSinkDevice.ps1")


def check_linux(path: str) -> int:
    # CPack may or may not put a top-level package directory in the tarball
    # (CPACK_INCLUDE_TOPLEVEL_DIRECTORY; the component archives here do not).
    # Normalise to an install prefix either way: keep a name that already
    # starts at an install directory, and strip one leading segment from one
    # that does not. Stripping unconditionally turned bin/ac3crucible-run into
    # ac3crucible-run and reported everything missing on a correct archive.
    install_dirs = ("bin/", "share/", "lib/", "include/", "libexec/")
    with tarfile.open(path) as archive:
        names = []
        for member in archive.getmembers():
            name = member.name.removeprefix("./")
            if not name.startswith(install_dirs) and "/" in name:
                name = name.split("/", 1)[1]
            names.append(name)
        total_mb = round(sum(m.size for m in archive.getmembers()) / 1048576)
    print(f"{path}: {len(names)} entries, {total_mb} MB unpacked")
    problems = [f"missing {name}" for name in REQUIRED_LINUX if name not in names]
    problems += [f"Windows driver script shipped in a Linux package: {name}"
                 for name in names for pattern in FORBIDDEN_LINUX if name.endswith(pattern)]
    for problem in problems:
        print(f"::error::{problem}")
    if problems:
        return 1
    print("ok: the Linux package holds the window, the runner and the menu entry, and no driver scripts")
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

    qml = [n for n in names if n.startswith("qml/")]
    print(f"{path}: {len(names)} entries, {total_mb} MB unpacked, {len(qml)} QML files")

    problems = [f"missing {name}" for name in REQUIRED if name not in names]
    problems += [
        f"no {prefix} module - the Qt deploy step did not bring what the window imports"
        for prefix in REQUIRED_QML
        if not any(n.startswith(prefix) for n in names)
    ]
    for problem in problems:
        print(f"::error::{path}: {problem}")
    return 1 if problems else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
