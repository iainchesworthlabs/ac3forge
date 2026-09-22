"""Unit tests for check_platform_matrix.py, the platform-matrix coverage check.

stdlib `unittest`, not pytest, for the same reason the script itself is
stdlib-only: this runs in ci.yml's script-lint job, which installs ruff,
shellcheck and actionlint and nothing else.

Each test builds a small temporary tree and runs the check over it, so the
cases are the rules the script's header states: a page linked from the matrix
passes, an unlisted page fails and names the page, a page listed in UNLISTED is
declined rather than failed, a link whose text wraps across a newline still
counts (the shape that a line-by-line matcher missed), a link inside a fence
does not count, and a missing or empty matrix fails rather than passing
vacuously.

Run: python3 -m unittest discover -s tools/checks -p 'test_*.py'
"""

import io
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_platform_matrix


def _write(root: Path, relative: str, text: str = "") -> Path:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


class PlatformMatrixCoverage(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        _write(self.root, "docs/platforms/windows.md", "# Windows\n")
        _write(self.root, "docs/platforms/linux.md", "# Linux\n")
        self.addCleanup(self._tmp.cleanup)

    def _run(self, unlisted: dict[str, str] | None = None) -> tuple[int, str]:
        original = check_platform_matrix.UNLISTED
        check_platform_matrix.UNLISTED = unlisted if unlisted is not None else {}
        buffer = io.StringIO()
        try:
            with redirect_stdout(buffer):
                code = check_platform_matrix.main()
        finally:
            check_platform_matrix.UNLISTED = original
        return code, buffer.getvalue()

    def _matrix(self, text: str) -> None:
        _write(self.root, "docs/platforms/index.md", text)

    def run_with_root(self, unlisted: dict[str, str] | None = None) -> tuple[int, str]:
        argv = sys.argv
        sys.argv = ["check_platform_matrix.py", "--root", str(self.root)]
        try:
            return self._run(unlisted)
        finally:
            sys.argv = argv

    def test_every_page_linked_passes(self) -> None:
        self._matrix("# Platforms\n\n[Windows](windows.md) and [Linux](linux.md).\n")
        code, out = self.run_with_root()
        self.assertEqual(code, 0, out)
        self.assertIn("2 page(s) checked, 0 unlisted", out)

    def test_unlisted_page_fails_and_names_it(self) -> None:
        self._matrix("# Platforms\n\nOnly [Windows](windows.md).\n")
        code, out = self.run_with_root()
        self.assertEqual(code, 1)
        self.assertIn("docs/platforms/linux.md is not linked", out)
        self.assertNotIn("docs/platforms/windows.md is not linked", out)
        # The annotation path is posix even when the check runs on Windows.
        self.assertIn("::error file=docs/platforms/index.md::", out)

    def test_declared_unlisted_page_is_declined_not_failed(self) -> None:
        self._matrix("# Platforms\n\nOnly [Windows](windows.md).\n")
        code, out = self.run_with_root({"docs/platforms/linux.md": "covered elsewhere"})
        self.assertEqual(code, 0, out)
        self.assertIn("deliberately unlisted - covered elsewhere", out)

    def test_link_text_wrapping_a_newline_still_counts(self) -> None:
        # The shape a line-by-line matcher missed: prose wraps mid-link-text.
        self._matrix("# Platforms\n\n[Windows](windows.md) and [the Linux\npage](linux.md).\n")
        code, out = self.run_with_root()
        self.assertEqual(code, 0, out)

    def test_link_inside_a_fence_does_not_count(self) -> None:
        self._matrix(
            "# Platforms\n\n[Windows](windows.md)\n\n```md\n[Linux](linux.md)\n```\n"
        )
        code, out = self.run_with_root()
        self.assertEqual(code, 1)
        self.assertIn("docs/platforms/linux.md is not linked", out)

    def test_anchors_and_relative_prefixes_resolve_to_the_same_page(self) -> None:
        self._matrix(
            "# Platforms\n\n[a](./windows.md#toolchains) and [b](../platforms/linux.md)\n"
        )
        code, out = self.run_with_root()
        self.assertEqual(code, 0, out)

    def test_missing_matrix_fails(self) -> None:
        code, out = self.run_with_root()
        self.assertEqual(code, 1)
        self.assertIn("the platform selection matrix is missing", out)

    def test_no_platform_pages_fails_rather_than_passing_vacuously(self) -> None:
        for page in (self.root / "docs/platforms").glob("*.md"):
            page.unlink()
        self._matrix("# Platforms\n")
        code, out = self.run_with_root()
        self.assertEqual(code, 1)
        self.assertIn("no platform pages found", out)


if __name__ == "__main__":
    unittest.main()
