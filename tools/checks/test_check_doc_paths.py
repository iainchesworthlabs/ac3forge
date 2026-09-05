"""Unit tests for check_doc_paths.py, the doc-and-script path check.

stdlib `unittest`, not pytest, for the same reason the script itself is
stdlib-only: this runs in ci.yml's script-lint job, which installs ruff,
shellcheck and actionlint and nothing else.

Each test builds a small temporary tree and runs the check over it, so the
cases are the rules the script's header states: a good relative link passes, a
missing target fails and names file:line, an anchor-only link passes, a glob
literal is skipped and reported as such rather than failing, and each of the
four shapes the literal check declines to judge is declined for its own stated
reason.

Run: python3 -m unittest discover -s tools/checks -p 'test_*.py'
"""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_doc_paths


def _write(root: Path, relative: str, text: str = "") -> Path:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


class MarkdownLinks(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        _write(self.root, "docs/target.md", "# target\n")
        _write(self.root, "docs/sub/other.md", "# other\n")
        self.addCleanup(self._tmp.cleanup)

    def test_good_relative_link_passes(self) -> None:
        _write(self.root, "docs/page.md", "See [target](target.md) and [other](sub/other.md#h).\n")
        _write(self.root, "README.md", "[docs](docs/target.md) and [dir](docs/sub/)\n")
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(report.problems, [])
        self.assertEqual(report.checked, 4)

    def test_missing_target_fails_with_file_and_line(self) -> None:
        _write(self.root, "docs/page.md", "ok [t](target.md)\n\nbad [gone](gone.md#anchor)\n")
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(len(report.problems), 1)
        self.assertIn("docs/page.md:3:", report.problems[0])
        self.assertIn("gone.md#anchor", report.problems[0])

    def test_anchor_only_link_passes(self) -> None:
        _write(self.root, "docs/page.md", "[jump](#section) and [web](https://x.test/a.md)\n")
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(report.problems, [])
        self.assertEqual(report.checked, 0)

    def test_links_in_code_are_not_links(self) -> None:
        text = "```\n[x](nowhere.md)\n```\n`[y](nowhere.md)` is syntax\n"
        _write(self.root, "docs/page.md", text)
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(report.problems, [])

    def test_roadmap_is_skipped_as_dual_context(self) -> None:
        """Its links are absolute URLs because it is also a docs-site snippet."""
        _write(self.root, "ROADMAP.md", "[broken](nowhere.md)\n")
        _write(self.root, "docs/roadmap.md", '--8<-- "ROADMAP.md"\n')
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(report.problems, [])
        self.assertTrue(any(entry.startswith("ROADMAP.md:") for entry in report.skipped))


class PathLiterals(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        _write(self.root, "tools/checks/real.py", "")
        _write(self.root, "docs/index.md", "# index\n")
        self.addCleanup(self._tmp.cleanup)

    def test_existing_literal_passes_and_missing_fails(self) -> None:
        _write(
            self.root,
            "tools/ci/script.py",
            'A = "tools/checks/real.py"\nB = "docs/index.md"\nC = "docs/missing.md"\n',
        )
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(len(report.problems), 1)
        self.assertIn("tools/ci/script.py:3:", report.problems[0])
        self.assertIn("docs/missing.md", report.problems[0])
        self.assertEqual(report.checked, 3)  # every token it judged, failures included

    def test_anchor_on_a_literal_names_a_section_not_a_path(self) -> None:
        _write(self.root, "tools/ci/script.sh", "# see docs/index.md#a-heading for why\n")
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(report.problems, [])
        self.assertEqual(report.checked, 1)

    def test_glob_is_skipped_not_failed(self) -> None:
        _write(self.root, ".github/workflows/ci.yml", "paths:\n  - 'docs/**/*.md'\n  - 'src/*'\n")
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(report.problems, [])
        self.assertEqual(report.checked, 0)
        self.assertEqual(len(report.skipped), 2)
        self.assertIn("(glob)", report.skipped[0])

    def test_placeholders_and_wrapped_identifiers_are_skipped(self) -> None:
        text = "x ${DIR}/src/thing\ny tools/ci/${name}.py\nz tools/ci/run_codec_\n"
        _write(self.root, "tools/ci/run.sh", text)
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(report.problems, [])
        reasons = [entry.rsplit("(", 1)[1] for entry in report.skipped]
        self.assertEqual(reasons, ["placeholder)", "line-wrapped identifier)"])

    def test_gitignored_paths_are_generated_not_stale(self) -> None:
        _write(self.root, ".gitignore", "# comment\nbuild/\ndocs/spec/\n!keep\n")
        _write(self.root, "tools/ci/pack.sh", "cp apps/a/build/out.apk .\ncat docs/spec/A52.txt\n")
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(report.problems, [])
        self.assertEqual(len(report.skipped), 2)
        self.assertTrue(all("gitignored" in entry for entry in report.skipped))

    def test_foreign_paths_are_skipped_with_their_reason(self) -> None:
        token, reason = next(iter(check_doc_paths.FOREIGN_PATHS.items()))
        _write(self.root, ".github/workflows/build.yml", f"# confirmed against {token}\n")
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(report.problems, [])
        self.assertEqual(len(report.skipped), 1)
        self.assertIn(reason, report.skipped[0])

    def test_url_path_segments_are_not_tokens(self) -> None:
        text = "# https://example.test/blob/main/docs/nowhere.md\n"
        _write(self.root, "cmake/Thing.cmake", text)
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(report.problems, [])
        self.assertEqual(report.checked, 0)

    def test_ps1_accepts_backslashes(self) -> None:
        _write(self.root, "tools/checks/cov.ps1", "$s = Join-Path $root 'tools\\checks'\n")
        _write(self.root, "tools/checks/bad.ps1", "$s = Join-Path $root 'tools\\gone'\n")
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(len(report.problems), 1)
        self.assertIn("tools/checks/bad.ps1:1:", report.problems[0])

    def test_test_files_are_not_scanned(self) -> None:
        _write(self.root, "tools/checks/test_fixture.py", 'X = "docs/not-here.md"\n')
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(report.problems, [])


class IgnorePatterns(unittest.TestCase):
    """gitignore's anchoring rule: a pattern with a slash is rooted, one without floats."""

    def test_rooted_and_floating_patterns(self) -> None:
        patterns = ["build/", "docs/spec/", "*.user"]
        for token in ("apps/a/build/x.apk", "build", "docs/spec/A52.txt", "src/a.user"):
            self.assertTrue(check_doc_paths.is_ignored(token, patterns), token)
        for token in ("apps/a/builder/x", "docs/specimen.md", "src/spec/a.txt"):
            self.assertFalse(check_doc_paths.is_ignored(token, patterns), token)


class Main(unittest.TestCase):
    def test_exit_code_follows_findings(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write(root, "docs/a.md", "[b](b.md)\n")
            self.assertEqual(check_doc_paths.main(["check", "--root", tmp]), 1)
            _write(root, "docs/b.md", "# b\n")
            self.assertEqual(check_doc_paths.main(["check", "--root", tmp]), 0)


if __name__ == "__main__":
    unittest.main()
