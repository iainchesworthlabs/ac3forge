"""Unit tests for check_crucible_package.py, the Crucible package gate.

stdlib `unittest`, not pytest, for the reason test_write_measurement_badges.py
gives: this runs in ci.yml's script-lint job, which installs nothing beyond
its linters, and the script under test is stdlib-only itself.

The rules this file holds down are the content rules: the gate reads
NOTICES.txt rather than only listing it, because the ways a notices file goes
wrong leave every file name in place. A notices file assembled from the other
platform's component list (apps/crucible/notices/platform/<os>/) still
appears as NOTICES.txt; so does one whose Qt Quick 3D section is missing while
qml/QtQuick3D/ shipped, or present while it did not. Each case here builds
the smallest archive that has the right shape and the wrong words, and
asserts the gate refuses it and says why. test_good_shapes_pass guards the
existing name checks against the same refactor.

Run: python3 -m unittest discover -s tools/ci -p 'test_*.py'
"""

import contextlib
import io
import os
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_crucible_package as gate

# The smallest texts that satisfy each platform's rules: one line per phrase
# the fragments carry, plus a filled Qt version token.
WINDOWS_NOTICES = (
    "AC3Forge Crucible 0.10.0 - third-party notices, Windows build\n"
    "This package includes the Qt 6.8.3 libraries.\n"
    "https://download.qt.io/archive/qt/6.8/6.8.3/single/\n"
    "GNU LESSER GENERAL PUBLIC LICENSE\n"
    "Microsoft Public License (MS-PL)\n"
    "{fmt} 12.2.0\n"
    "SIL OPEN FONT LICENSE Version 1.1\n"
)
QUICK3D_SECTION = "Qt Quick 3D 6.8.3\n"
LINUX_NOTICES = (
    "AC3Forge Crucible 0.10.0 - third-party notices, Linux build\n"
    "Built against Qt 6.10.0.\n"
    "ac3crucible links libpipewire-0.3\n"
    "{fmt} 12.2.0\n"
    "SIL OPEN FONT LICENSE Version 1.1\n"
)


def windows_zip(directory, notices, quick3d_payload=True):
    """A zip with every required name, the two QML modules, and this NOTICES.txt."""
    path = os.path.join(directory, "ac3forge-crucible-test-win64.zip")
    with zipfile.ZipFile(path, "w") as archive:
        for name in gate.REQUIRED:
            archive.writestr(name, notices if name == "NOTICES.txt" else "")
        archive.writestr("qml/QtQuick/Controls/qmldir", "")
        if quick3d_payload:
            archive.writestr("qml/QtQuick3D/qmldir", "")
    return path


def linux_tar(directory, notices, omit=()):
    """A tarball with the Linux install layout and this NOTICES.txt, minus `omit`."""
    path = os.path.join(directory, "ac3forge-crucible-test-Linux-x86_64.tar.gz")
    with tarfile.open(path, "w:gz") as archive:
        for name in gate.REQUIRED_LINUX:
            if name in omit:
                continue
            data = notices.encode("utf-8") if name.endswith(("NOTICES.txt", "copyright")) else b""
            info = tarfile.TarInfo(name)
            info.size = len(data)
            archive.addfile(info, io.BytesIO(data))
    return path


def run(path):
    """The gate's exit code and everything it printed."""
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        code = gate.main(["check_crucible_package.py", path])
    return code, out.getvalue()


class NoticesContentTest(unittest.TestCase):

    def test_good_shapes_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            code, out = run(windows_zip(directory, WINDOWS_NOTICES + QUICK3D_SECTION))
            self.assertEqual(code, 0, out)
            self.assertIn("notices", out)
            code, out = run(linux_tar(directory, LINUX_NOTICES))
            self.assertEqual(code, 0, out)
            self.assertIn("notices", out)

    def test_windows_zip_with_linux_notices_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            # The Linux component list, packaged as the Windows zip: the
            # PipeWire section is there and the LGPL, the source location
            # and the MS-PL are not.
            code, out = run(windows_zip(directory, LINUX_NOTICES + QUICK3D_SECTION))
            self.assertEqual(code, 1)
            self.assertIn("::error::", out)
            self.assertIn("libpipewire", out)
            self.assertIn("GNU LESSER GENERAL PUBLIC LICENSE", out)
            # A Windows file that is right in every way but one still fails.
            code, out = run(windows_zip(directory, WINDOWS_NOTICES + QUICK3D_SECTION + "libpipewire-0.3\n"))
            self.assertEqual(code, 1)
            self.assertIn("libpipewire", out)

    def test_quick3d_payload_and_notices_must_agree(self):
        with tempfile.TemporaryDirectory() as directory:
            # Shipping the module without saying so.
            code, out = run(windows_zip(directory, WINDOWS_NOTICES, quick3d_payload=True))
            self.assertEqual(code, 1)
            self.assertIn("does not name Qt Quick 3D", out)
            # Saying so without shipping the module (which the name check
            # refuses on its own; the notices rule names the same thing).
            code, out = run(windows_zip(directory, WINDOWS_NOTICES + QUICK3D_SECTION, quick3d_payload=False))
            self.assertEqual(code, 1)
            self.assertIn("QtQuick3D", out)
            # Matched: present with the payload.
            code, out = run(windows_zip(directory, WINDOWS_NOTICES + QUICK3D_SECTION, quick3d_payload=True))
            self.assertEqual(code, 0, out)

    def test_unfilled_qt_version_token_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            unfilled = WINDOWS_NOTICES.replace("Qt 6.8.3", "Qt {{QT_VERSION}}") + QUICK3D_SECTION
            code, out = run(windows_zip(directory, unfilled))
            self.assertEqual(code, 1)
            self.assertIn("no Qt 6.x version", out)

    def test_linux_tar_with_windows_notices_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            code, out = run(linux_tar(directory, WINDOWS_NOTICES))
            self.assertEqual(code, 1)
            self.assertIn("::error::", out)
            self.assertIn("Microsoft Public License", out)
            self.assertIn("GNU LESSER GENERAL PUBLIC LICENSE", out)
            self.assertIn("libpipewire", out)
            # The LGPL heading alone is enough.
            code, out = run(linux_tar(directory, LINUX_NOTICES + "GNU LESSER GENERAL PUBLIC LICENSE\n"))
            self.assertEqual(code, 1)
            self.assertIn("GNU LESSER GENERAL PUBLIC LICENSE", out)

    def test_linux_notices_may_name_quick3d_either_way(self):
        # No Qt ships in the tarball, so there is no payload to compare the
        # section against: the build's own tests hold it to has3D instead.
        with tempfile.TemporaryDirectory() as directory:
            code, out = run(linux_tar(directory, LINUX_NOTICES + QUICK3D_SECTION))
            self.assertEqual(code, 0, out)

    def test_linux_tar_needs_debian_copyright(self):
        with tempfile.TemporaryDirectory() as directory:
            code, out = run(linux_tar(directory, LINUX_NOTICES, omit=("share/doc/ac3forge-crucible/copyright",)))
            self.assertEqual(code, 1)
            self.assertIn("::error::missing share/doc/ac3forge-crucible/copyright", out)

    def test_missing_notices_is_reported_by_name(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "ac3forge-crucible-bare-win64.zip")
            with zipfile.ZipFile(path, "w") as archive:
                for name in gate.REQUIRED:
                    if name != "NOTICES.txt":
                        archive.writestr(name, "")
                archive.writestr("qml/QtQuick/qmldir", "")
                archive.writestr("qml/QtQuick3D/qmldir", "")
            code, out = run(path)
            self.assertEqual(code, 1)
            self.assertIn("missing NOTICES.txt", out)


if __name__ == "__main__":
    unittest.main()
