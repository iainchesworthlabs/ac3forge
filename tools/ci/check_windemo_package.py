#!/usr/bin/env python3
"""Assert the Desktop Atmos Demo's package carries a window that can start.

The archive cpack produces for the `windemo` component (cmake/Packaging.cmake)
is the demo's release asset. Its failure mode is not an empty file - it is a
plausible-looking archive holding ac3desk.exe with no Qt beside it, which
installs and then does not start, because the Qt deploy script
(apps/windows/CMakeLists.txt) was filed under the wrong CPack component or
did not run. That is what this checks, from CI (.github/workflows/_build.yml)
and from a local `cpack --preset pack-windows-msvc` run just the same:

    python tools/ci/check_windemo_package.py packages/ac3forge-desktop-atmos-*.zip

The layout it expects is the one qt_generate_deploy_qml_app_script() produces
and the existing runtime archive already uses: the binaries in bin/ beside a
qt.conf whose `Prefix = ..` sends Qt to the sibling plugins/, qml/ and
translations/ directories.
"""

from __future__ import annotations

import sys
import zipfile

# What has to be there for the window to exist and start at all: itself, the
# console runner beside it, the driver scripts its Settings page points at,
# the qt.conf that makes the layout resolve, and the Windows platform plugin
# without which Qt aborts on launch.
REQUIRED = (
    "bin/ac3desk.exe",
    "bin/ac3windemo.exe",
    "bin/driver/install.ps1",
    "bin/driver/remove.ps1",
    "bin/qt.conf",
    "plugins/platforms/qwindows.dll",
)

# QML modules the window imports directly. QtQuick3D earns its own line: the
# room's 3D view is optional at build time (apps/windows/CMakeLists.txt skips
# it with a warning when the Quick3D module is absent from the kit), so its
# absence here means the package shipped a 2D-only room and nothing else
# would have said so.
REQUIRED_QML = ("qml/QtQuick/", "qml/QtQuick3D/")


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"usage: {argv[0]} <archive.zip>", file=sys.stderr)
        return 2
    path = argv[1]
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
