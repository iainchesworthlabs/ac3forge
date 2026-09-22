"""Unit tests for check_cli_docs_package.py, the ac3cli man page/completions gate.

stdlib `unittest`, not pytest, for the reason test_write_measurement_badges.py
gives: this runs in ci.yml's script-lint job, which installs nothing beyond its
linters, and the script under test is stdlib-only itself.

Two things are worth holding down here. The first is the gate's whole point:
an archive that looks complete - the binary is there, the licence is there -
and carries none of the five generated files still has to fail, because that
is exactly the shape every Linux and macOS package had while the guard in
apps/cli/CMakeLists.txt tested CMAKE_CROSSCOMPILING (see the script header).
The second is the top-level-directory normalisation, which is where the
equivalent Crucible gate had a real bug: a correct archive was reported as
entirely missing. Both archive shapes are built here, with and without a
wrapping directory, and macOS's root-level ac3gui.app is included because it
is the case that makes a per-name strip give the wrong answer.

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

import check_cli_docs_package as gate

# What a runtime package holds besides the five files under test: the binary,
# the licence, and - on macOS - the GUI bundle at the archive root, which is
# the entry that does not start at an install directory.
OTHER_FILES = ("bin/ac3cli", "share/doc/ac3forge/LICENSE.txt")
MACOS_BUNDLE = "ac3gui.app/Contents/MacOS/ac3gui"


def entries(omit=(), extra=()):
    """The five required names minus `omit`, plus the ordinary package contents."""
    return [name for name in gate.REQUIRED if name not in omit] + list(OTHER_FILES) + list(extra)


def make_tar(directory, names, prefix=""):
    path = os.path.join(directory, "ac3forge-0.7.0-Linux-x86_64.tar.gz")
    with tarfile.open(path, "w:gz") as archive:
        for name in names:
            info = tarfile.TarInfo(prefix + name)
            info.size = 0
            archive.addfile(info, io.BytesIO(b""))
    return path


def make_zip(directory, names, prefix=""):
    path = os.path.join(directory, "ac3forge-0.7.0-Darwin.zip")
    with zipfile.ZipFile(path, "w") as archive:
        for name in names:
            archive.writestr(prefix + name, "")
    return path


def make_tree(directory, names):
    root = os.path.join(directory, "merged")
    for name in names:
        target = os.path.join(root, *name.split("/"))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        Path(target).write_text("", encoding="utf-8")
    return root


def run(path):
    """The gate's exit code and everything it printed."""
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        code = gate.main(["check_cli_docs_package.py", path])
    return code, out.getvalue()


class CompletePackageTest(unittest.TestCase):

    def test_every_shape_passes_when_all_five_are_present(self):
        with tempfile.TemporaryDirectory() as directory:
            for path in (
                make_tar(directory, entries()),
                make_zip(directory, entries(extra=[MACOS_BUNDLE])),
                make_tree(directory, entries()),
            ):
                code, out = run(path)
                self.assertEqual(code, 0, out)
                self.assertIn("ok:", out)


class MissingFilesTest(unittest.TestCase):

    def test_the_package_that_shipped_for_months_fails(self):
        # A plausible package: the binary and the licence, and not one of the
        # five generated files. This is what every Linux .tar.gz, .deb and
        # .rpm and the universal .dmg actually contained.
        with tempfile.TemporaryDirectory() as directory:
            code, out = run(make_tar(directory, list(OTHER_FILES)))
            self.assertEqual(code, 1)
            for name in gate.REQUIRED:
                self.assertIn(f"missing {name}", out)
            # The failure says where the files come from, not only that they
            # are absent - the guard is the thing a reader has to look at.
            self.assertIn("apps/cli/CMakeLists.txt", out)

    def test_one_missing_completion_fails(self):
        # The man page alone passing is the regression this would otherwise
        # miss: four of five is still a broken package.
        with tempfile.TemporaryDirectory() as directory:
            omitted = "share/zsh/site-functions/_ac3cli"
            code, out = run(make_tar(directory, entries(omit=[omitted])))
            self.assertEqual(code, 1)
            self.assertIn(f"missing {omitted}", out)
            self.assertNotIn("missing share/man/man1/ac3cli.1", out)

    def test_missing_man_page_fails_on_every_shape(self):
        with tempfile.TemporaryDirectory() as directory:
            omitted = ["share/man/man1/ac3cli.1"]
            for path in (
                make_tar(directory, entries(omit=omitted)),
                make_zip(directory, entries(omit=omitted, extra=[MACOS_BUNDLE])),
                make_tree(directory, entries(omit=omitted)),
            ):
                code, out = run(path)
                self.assertEqual(code, 1, out)
                self.assertIn("missing share/man/man1/ac3cli.1", out)


class TopLevelDirectoryTest(unittest.TestCase):
    """CPACK_INCLUDE_TOPLEVEL_DIRECTORY, both ways round, on both archives."""

    def test_wrapped_archive_is_read_relative_to_the_prefix(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix = "ac3forge-0.7.0-Linux-x86_64/"
            code, out = run(make_tar(directory, entries(), prefix=prefix))
            self.assertEqual(code, 0, out)
            code, out = run(make_zip(directory, entries(), prefix=prefix))
            self.assertEqual(code, 0, out)

    def test_a_wrapped_archive_still_fails_when_a_file_is_missing(self):
        # The normalisation must not turn "wrapped" into "everything found".
        with tempfile.TemporaryDirectory() as directory:
            code, out = run(
                make_tar(directory, entries(omit=["share/man/man1/ac3cli.1"]), prefix="ac3forge/")
            )
            self.assertEqual(code, 1)
            self.assertIn("missing share/man/man1/ac3cli.1", out)

    def test_root_level_app_bundle_does_not_trigger_a_strip(self):
        # macOS's ac3gui.app sits beside bin/ and share/, so it does not start
        # at an install directory. Stripping per name would rewrite the whole
        # archive off its prefix; the archive-wide decision leaves it alone.
        with tempfile.TemporaryDirectory() as directory:
            names = entries(extra=[MACOS_BUNDLE])
            code, out = run(make_zip(directory, names))
            self.assertEqual(code, 0, out)
            self.assertIn(MACOS_BUNDLE, gate.names_in(make_zip(directory, names)))


if __name__ == "__main__":
    unittest.main()
