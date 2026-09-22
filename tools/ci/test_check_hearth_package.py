"""Unit tests for check_hearth_package.py, the Hearth package gate.

stdlib `unittest`, not pytest, for the reason test_write_measurement_badges.py
gives: this runs in ci.yml's script-lint job, which installs nothing beyond
its linters, and the script under test is stdlib-only itself.

The rules this file holds down are the content rules: the gate reads
NOTICES.txt rather than only listing it, because the ways a notices file goes
wrong leave every file name in place. Hearth's Qt section (apps/hearth/
notices/notices.cmake) is present with a filled version and source URL only
where the package bundles Qt (Windows, macOS - the gap PR #816 fixed) and
absent on Linux, which links the system's own Qt; the five vcpkg-sourced
third-party sections (cpp-httplib, Mbed TLS, mdns, libFLAC, Opus) can each
go quietly wrong the same way Crucible's Qt version can - a version token
that substituted to nothing - without any file name going missing.
test_good_shapes_pass guards the existing name checks against a refactor,
and two more guard the rules that are negatives, both currently unreachable
from a real build and asserted anyway (see check_hearth_package.py's own
module docstring): no part of Qt's test module is in the package
(test_windows_zip_must_not_carry_qt_test, test_macos_zip_must_not_carry_qt_test).
test_dispatch_is_by_bundle_content_not_filename guards the thing that tells a
macOS archive from a Windows one in the first place - both are
ac3forge-hearth-<version>-<system>.zip alike, so main() has to look inside.

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

import check_hearth_package as gate

# The smallest texts that satisfy the rules: one line per section the
# fragments carry, each with a filled version where the section has one.
# Windows and macOS check identically here (qt_bundled=True on both) -
# unlike Crucible, Hearth has no driver and no platform-only library, so
# apps/hearth/notices/notices.cmake's Qt section is the same text on both
# (apps/crucible/notices/fragments/qt-bundled.txt, shared rather than
# copied, only its {{QT_PAYLOAD}}/{{QT_LOOKUP}} prose differs between them,
# and this gate does not check that prose) - one shared fixture stands in
# for both.
QT_SECTION = (
    "This package includes the Qt 6.8.3 libraries.\n"
    "https://download.qt.io/archive/qt/6.8/6.8.3/single/\n"
    "GNU LESSER GENERAL PUBLIC LICENSE\n"
)
THIRDPARTY_SECTIONS = (
    "cpp-httplib 0.15.3\n"
    "Mbed TLS 3.6.2\n"
    "mdns 1.7\n"
    "libFLAC 1.4.3\n"
    "Opus 1.5.2\n"
    "Sendspin time filter\n"
)
WINDOWS_NOTICES = (
    "AC3Forge Hearth 0.10.0 - third-party notices\n" + QT_SECTION + THIRDPARTY_SECTIONS
)
MACOS_NOTICES = WINDOWS_NOTICES
LINUX_NOTICES = (
    "AC3Forge Hearth 0.10.0 - third-party notices\n" + THIRDPARTY_SECTIONS
)


def windows_zip(directory, notices, extra=()):
    """A zip with every required name and this NOTICES.txt.

    `extra` adds members the required list does not name, which is how the
    Qt-Test rule is exercised: that rule is about what must NOT be there.
    """
    path = os.path.join(directory, "ac3forge-hearth-test-win64.zip")
    with zipfile.ZipFile(path, "w") as archive:
        for name in gate.REQUIRED:
            archive.writestr(name, notices if name == "NOTICES.txt" else "")
        for name in extra:
            archive.writestr(name, "")
    return path


def macos_zip(directory, notices, extra=()):
    """A zip with every required name and this NOTICES.txt, macOS bundle shape.

    Same `extra` role as windows_zip(): members the required list does not
    name, for exercising the Qt-Test negative rule.
    """
    path = os.path.join(directory, "ac3forge-hearth-test-Darwin.zip")
    with zipfile.ZipFile(path, "w") as archive:
        for name in gate.REQUIRED_MACOS:
            archive.writestr(name, notices if name == "NOTICES.txt" else "")
        for name in extra:
            archive.writestr(name, "")
    return path


def linux_tar(directory, notices, omit=()):
    """A tarball with the Linux install layout and this NOTICES.txt, minus `omit`."""
    path = os.path.join(directory, "ac3forge-hearth-test-Linux-x86_64.tar.gz")
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
        code = gate.main(["check_hearth_package.py", path])
    return code, out.getvalue()


class NoticesContentTest(unittest.TestCase):

    def test_good_shapes_pass(self):
        with tempfile.TemporaryDirectory() as directory:
            code, out = run(windows_zip(directory, WINDOWS_NOTICES))
            self.assertEqual(code, 0, out)
            self.assertIn("notices", out)
            code, out = run(linux_tar(directory, LINUX_NOTICES))
            self.assertEqual(code, 0, out)
            self.assertIn("notices", out)
            code, out = run(macos_zip(directory, MACOS_NOTICES))
            self.assertEqual(code, 0, out)
            self.assertIn("notices", out)

    def test_linux_notices_in_windows_zip_fails(self):
        # The Linux section list, packaged as the Windows zip: no Qt section
        # at all, so the LGPL heading, the source URL and a filled Qt
        # version are all missing.
        with tempfile.TemporaryDirectory() as directory:
            code, out = run(windows_zip(directory, LINUX_NOTICES))
            self.assertEqual(code, 1)
            self.assertIn("::error::", out)
            self.assertIn("GNU LESSER GENERAL PUBLIC LICENSE", out)
            self.assertIn("no Qt 6.x version", out)

    def test_windows_notices_on_linux_fails(self):
        # The converse: a Qt section that has no business in the Linux
        # tarball, which links the system's own Qt and bundles none.
        with tempfile.TemporaryDirectory() as directory:
            code, out = run(linux_tar(directory, WINDOWS_NOTICES))
            self.assertEqual(code, 1)
            self.assertIn("::error::", out)
            self.assertIn("bundles no Qt", out)

    def test_unfilled_qt_version_token_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            unfilled = WINDOWS_NOTICES.replace("Qt 6.8.3", "Qt {{QT_VERSION}}")
            code, out = run(windows_zip(directory, unfilled))
            self.assertEqual(code, 1)
            self.assertIn("no Qt 6.x version", out)

    def test_unfilled_thirdparty_version_fails(self):
        # Each of the five vcpkg-sourced sections separately, the same
        # per-payload subTest shape test_windows_zip_must_not_carry_qt_test
        # uses below: a check that only ever saw them blanked out together
        # would not prove each one is actually read.
        cases = (
            ("cpp-httplib 0.15.3\n", "cpp-httplib\n"),
            ("Mbed TLS 3.6.2\n", "Mbed TLS\n"),
            ("mdns 1.7\n", "mdns\n"),
            ("libFLAC 1.4.3\n", "libFLAC\n"),
            ("Opus 1.5.2\n", "Opus\n"),
        )
        for filled, blanked in cases:
            with self.subTest(section=blanked), tempfile.TemporaryDirectory() as directory:
                unfilled = WINDOWS_NOTICES.replace(filled, blanked)
                code, out = run(windows_zip(directory, unfilled))
                self.assertEqual(code, 1)
                self.assertIn("does not mention", out)
                self.assertIn("filled version", out)

    def test_missing_time_filter_section_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            without = WINDOWS_NOTICES.replace("Sendspin time filter\n", "")
            code, out = run(windows_zip(directory, without))
            self.assertEqual(code, 1)
            self.assertIn("Sendspin time filter", out)

    def test_linux_tar_needs_debian_copyright(self):
        with tempfile.TemporaryDirectory() as directory:
            code, out = run(
                linux_tar(directory, LINUX_NOTICES, omit=("share/doc/ac3forge-hearth/copyright",))
            )
            self.assertEqual(code, 1)
            self.assertIn("::error::missing share/doc/ac3forge-hearth/copyright", out)

    def test_windows_zip_must_not_carry_qt_test(self):
        # Currently unreachable from a real build (apps/hearth/ui/
        # CMakeLists.txt has no ac3hearth_qmltests target for
        # qmlimportscanner to find a `import QtTest` beneath - see the
        # module docstring), asserted anyway so it is already watching the
        # day one lands. Each payload separately, the same reasoning
        # test_check_crucible_package.py's own version of this test gives:
        # a check that only ever sees them together would pass a partial
        # removal.
        payloads = (
            "qml/QtTest/qmldir",
            "qml/QtTest/quicktestplugin.dll",
            "bin/Qt6Test.dll",
            "bin/Qt6QuickTest.dll",
        )
        for payload in payloads:
            with self.subTest(payload=payload), tempfile.TemporaryDirectory() as directory:
                code, out = run(windows_zip(directory, WINDOWS_NOTICES, extra=(payload,)))
                self.assertEqual(code, 1)
                self.assertIn("carries Qt's test module", out)
                self.assertIn(payload, out)
        # And the passing arm: a zip with nothing of Qt's test module in it.
        with tempfile.TemporaryDirectory() as directory:
            code, out = run(windows_zip(directory, WINDOWS_NOTICES))
            self.assertEqual(code, 0, out)
            self.assertIn("no Qt Test", out)

    def test_macos_zip_must_not_carry_qt_test(self):
        # The three shapes Qt's test module can leak into a macOS bundle in
        # - the QML module itself, the flat PlugIns copy of its plugin, and
        # either of the two Frameworks it depends on - each checked
        # separately, the same reasoning as the Windows version above.
        payloads = (
            "ac3hearth.app/Contents/Resources/qml/QtTest/qmldir",
            "ac3hearth.app/Contents/PlugIns/libquicktestplugin.dylib",
            "ac3hearth.app/Contents/Frameworks/QtTest.framework/QtTest",
            "ac3hearth.app/Contents/Frameworks/QtQuickTest.framework/QtQuickTest",
        )
        for payload in payloads:
            with self.subTest(payload=payload), tempfile.TemporaryDirectory() as directory:
                code, out = run(macos_zip(directory, MACOS_NOTICES, extra=(payload,)))
                self.assertEqual(code, 1)
                self.assertIn("carries Qt's test module", out)
                self.assertIn(payload, out)
        with tempfile.TemporaryDirectory() as directory:
            code, out = run(macos_zip(directory, MACOS_NOTICES))
            self.assertEqual(code, 0, out)
            self.assertIn("no Qt Test", out)

    def test_dispatch_is_by_bundle_content_not_filename(self):
        # Windows and macOS packages are both ac3forge-hearth-<version>-
        # <system>.zip (cmake/Packaging.cmake) - main() tells them apart by
        # a top-level "*.app/" entry, not by name. Proved here by renaming a
        # macOS-shaped archive to something that says nothing about the
        # platform and confirming it still gets the macOS rules (the
        # success message names which rules ran).
        with tempfile.TemporaryDirectory() as directory:
            macos_path = macos_zip(directory, MACOS_NOTICES)
            renamed = os.path.join(directory, "ac3forge-hearth-test.zip")
            os.replace(macos_path, renamed)
            code, out = run(renamed)
            self.assertEqual(code, 0, out)
            self.assertIn("the macOS package holds", out)

    def test_missing_notices_is_reported_by_name(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "ac3forge-hearth-bare-win64.zip")
            with zipfile.ZipFile(path, "w") as archive:
                for name in gate.REQUIRED:
                    if name != "NOTICES.txt":
                        archive.writestr(name, "")
            code, out = run(path)
            self.assertEqual(code, 1)
            self.assertIn("missing NOTICES.txt", out)
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "ac3forge-hearth-bare-Darwin.zip")
            with zipfile.ZipFile(path, "w") as archive:
                for name in gate.REQUIRED_MACOS:
                    if name != "NOTICES.txt":
                        archive.writestr(name, "")
            code, out = run(path)
            self.assertEqual(code, 1)
            self.assertIn("missing NOTICES.txt", out)


if __name__ == "__main__":
    unittest.main()
