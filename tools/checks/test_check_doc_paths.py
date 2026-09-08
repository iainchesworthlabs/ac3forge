"""Unit tests for check_doc_paths.py, the doc-and-script path check.

stdlib `unittest`, not pytest, for the same reason the script itself is
stdlib-only: this runs in ci.yml's script-lint job, which installs ruff,
shellcheck and actionlint and nothing else.

Each test builds a small temporary tree and runs the check over it, so the
cases are the rules the script's header states: a good relative link passes, a
missing target fails and names file:line, an anchor-only link passes,
ROADMAP.md's inverted rule fails a relative link even when its target exists, a
glob literal is skipped and reported as such rather than failing, and each of
the four shapes the literal check declines to judge is declined for its own
stated reason. Two later additions have their own classes: brace groups expand
to one path per alternative, and a path named in a page's prose is checked the
way one in a link is - except on the pages exempted by name, whose links are
still checked.

WrappedLinks covers the one rule that cannot be stated a line at a time: a link
whose text wraps across a line break is one link, is checked, and is reported
against the line it opens on — and the two boundaries that keep matching a
paragraph at a time from over-reaching, a blank line and a stray '['.

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

    def test_roadmap_absolute_urls_and_anchors_pass(self) -> None:
        """Its links are absolute URLs because it is also a docs-site snippet."""
        _write(self.root, "ROADMAP.md", "[a](https://x.test/y) and [b](#a-section)\n")
        _write(self.root, "docs/roadmap.md", '--8<-- "ROADMAP.md"\n')
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(report.problems, [])
        self.assertEqual(report.checked, 2)

    def test_roadmap_relative_link_fails_even_when_it_resolves(self) -> None:
        """The rule is the link's form, not whether the target happens to exist."""
        _write(self.root, "ROADMAP.md", "ok\n[here](docs/target.md)\n")
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(len(report.problems), 1)
        self.assertIn("ROADMAP.md:2:", report.problems[0])
        self.assertIn("must be an absolute URL", report.problems[0])


class WrappedLinks(unittest.TestCase):
    """A link whose text wraps across a line break, and the bounds on that."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        _write(self.root, "docs/target.md", "# target\n")
        self.addCleanup(self._tmp.cleanup)

    def test_wrapped_link_is_checked_and_resolves(self) -> None:
        _write(self.root, "docs/page.md", "See [the target page, named\nhere](target.md).\n")
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(report.problems, [])
        self.assertEqual(report.checked, 1)

    def test_wrapped_link_to_a_missing_target_fails_on_its_opening_line(self) -> None:
        text = "intro\n\nSee [the page that\nwent away](gone.md) for why.\n"
        _write(self.root, "docs/page.md", text)
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(len(report.problems), 1)
        self.assertIn("docs/page.md:3:", report.problems[0])
        self.assertIn("gone.md", report.problems[0])

    def test_wrapped_link_inside_a_fence_is_still_syntax(self) -> None:
        _write(self.root, "docs/page.md", "```\n[an example that\nwraps](nowhere.md)\n```\n")
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(report.problems, [])
        self.assertEqual(report.checked, 0)

    def test_a_blank_line_ends_the_paragraph_and_so_the_link(self) -> None:
        """Markdown stops a link at a paragraph break, so no match may cross one."""
        targets = check_doc_paths.link_targets(["[text that stops", "", "here](nowhere.md)"])
        self.assertEqual(targets, [])

    def test_a_prose_bracket_does_not_capture_a_later_link(self) -> None:
        """A ']' closes the most recent '[': the interval must not swallow the link below it."""
        lines = [
            "The key space is exactly [0, 32), so a flat array indexes it",
            "directly and the lookup costs nothing.",
            "- [CONTRIBUTING.md](CONTRIBUTING.md) records the rule.",
        ]
        self.assertEqual(check_doc_paths.link_targets(lines), [(3, "CONTRIBUTING.md")])

    def test_two_links_on_one_line_keep_their_order(self) -> None:
        lines = ["[first](a.md) then [second](b.md)"]
        self.assertEqual(check_doc_paths.link_targets(lines), [(1, "a.md"), (1, "b.md")])

    def test_roadmaps_absolute_rule_reaches_a_wrapped_link(self) -> None:
        _write(self.root, "ROADMAP.md", "A [link whose text\nwraps](docs/target.md) here.\n")
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(len(report.problems), 1)
        self.assertIn("ROADMAP.md:1:", report.problems[0])
        self.assertIn("must be an absolute URL", report.problems[0])


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


class BraceExpansion(unittest.TestCase):
    """`a/{b,c}` is a set of sibling paths, not a placeholder: each side is checked."""

    def test_every_alternative_is_expanded(self) -> None:
        self.assertEqual(
            check_doc_paths.expand_braces("src/x/{a,b}/y"),
            ["src/x/a/y", "src/x/b/y"],
        )

    def test_nested_groups_expand_to_the_cross_product(self) -> None:
        self.assertEqual(
            check_doc_paths.expand_braces("{a,b}/{c,d}"),
            ["a/c", "a/d", "b/c", "b/d"],
        )

    def test_token_without_a_group_is_returned_unchanged(self) -> None:
        self.assertEqual(check_doc_paths.expand_braces("src/plain"), ["src/plain"])

    def test_empty_group_is_left_alone_rather_than_expanded(self) -> None:
        self.assertEqual(check_doc_paths.expand_braces("src/{}"), ["src/{}"])

    def test_a_missing_alternative_fails_and_names_the_whole_token(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write(root, "src/here/f.txt", "")
            _write(root, "tools/t.py", 'P = "src/{here,gone}/f.txt"\n')
            report = check_doc_paths.check_tree(root)
            self.assertEqual(len(report.problems), 1)
            self.assertIn("src/gone/f.txt", report.problems[0])
            self.assertIn("src/{here,gone}/f.txt", report.problems[0])

    def test_an_unbalanced_brace_never_becomes_a_failure(self) -> None:
        """A brace survives the token regex only inside a balanced group, so a
        dangling one yields no token rather than a spurious missing path."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write(root, "tools/t.py", 'P = "src/{unclosed"\n')
            report = check_doc_paths.check_tree(root)
            self.assertEqual(report.problems, [])


class ProsePaths(unittest.TestCase):
    """Paths a page names in its own prose, as opposed to in a link."""

    def test_a_path_in_a_code_span_is_checked(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write(root, "docs/page.md", "The seam lives in `src/gone/`.\n")
            report = check_doc_paths.check_tree(root)
            self.assertEqual(len(report.problems), 1)
            self.assertIn("docs/page.md:1:", report.problems[0])
            self.assertIn("src/gone", report.problems[0])

    def test_a_path_that_exists_passes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write(root, "src/real/f.hpp", "")
            _write(root, "docs/page.md", "The seam lives in `src/real/`.\n")
            self.assertEqual(check_doc_paths.check_tree(root).problems, [])

    def test_fenced_blocks_are_exempt(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write(root, "docs/page.md", "```\ncd src/gone && make\n```\n")
            self.assertEqual(check_doc_paths.check_tree(root).problems, [])

    def test_an_exempt_page_keeps_its_links_checked(self) -> None:
        """A plan may name a directory it proposes; a broken *link* still fails."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write(root, "docs/plan.md", "proposes `src/gone/` and links [x](nowhere.md)\n")
            check_doc_paths.PROSE_PATHS_UNCHECKED["docs/plan.md"] = "test fixture"
            self.addCleanup(check_doc_paths.PROSE_PATHS_UNCHECKED.pop, "docs/plan.md", None)
            report = check_doc_paths.check_tree(root)
            self.assertEqual(len(report.problems), 1)
            self.assertIn("nowhere.md", report.problems[0])


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
