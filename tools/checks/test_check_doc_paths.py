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

Four classes cover the #anchor half. HeadingSlugs is the load-bearing one: a
table of headings and the ids the site actually publishes for them, every value
taken from the renderer rather than predicted, because an anchor check built on
an approximate slugifier fails valid links and gets reverted.
SlugifierMatchesMkdocs is the differential that table was cut from, run against
every page in docs/ when mkdocs' renderer is installed and skipped in
script-lint, which installs no mkdocs. LinkAnchors is the behaviour - what
counts as an anchor, and what the check declines to judge - and
MergedBranchAnchors is the shape that motivated the whole thing, two branches
that each pass alone and fail merged.

Run: python3 -m unittest discover -s tools/checks -p 'test_*.py'
"""

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path
from typing import ClassVar

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
        page = "See [target](target.md) and [other](sub/other.md#other).\n"
        _write(self.root, "docs/page.md", page)
        _write(self.root, "README.md", "[docs](docs/target.md) and [dir](docs/sub/)\n")
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(report.problems, [])
        self.assertEqual(report.checked, 5)  # four paths, and the one anchor

    def test_missing_target_fails_with_file_and_line(self) -> None:
        _write(self.root, "docs/page.md", "ok [t](target.md)\n\nbad [gone](gone.md#anchor)\n")
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(len(report.problems), 1)
        self.assertIn("docs/page.md:3:", report.problems[0])
        self.assertIn("gone.md#anchor", report.problems[0])

    def test_anchor_only_link_passes(self) -> None:
        text = "# Section\n\n[jump](#section) and [web](https://x.test/a.md)\n"
        _write(self.root, "docs/page.md", text)
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(report.problems, [])
        self.assertEqual(report.checked, 1)  # no path to resolve, but the anchor is one

    def test_links_in_code_are_not_links(self) -> None:
        text = "```\n[x](nowhere.md)\n```\n`[y](nowhere.md)` is syntax\n"
        _write(self.root, "docs/page.md", text)
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(report.problems, [])

    def test_roadmap_absolute_urls_and_anchors_pass(self) -> None:
        """Its links are absolute URLs because it is also a docs-site snippet."""
        roadmap = "## A section\n\n[a](https://x.test/y) and [b](#a-section)\n"
        _write(self.root, "ROADMAP.md", roadmap)
        _write(self.root, "docs/roadmap.md", '--8<-- "ROADMAP.md"\n')
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(report.problems, [])
        self.assertEqual(report.checked, 3)  # two links, and the anchor one of them names

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


class HeadingSlugs(unittest.TestCase):
    """The slug rules, pinned against the renderer that actually publishes the site.

    Every expectation below was produced by rendering that heading through the
    extension stack mkdocs.yml configures, at the markdown version
    requirements/requirements-docs.txt pins - not by reading the algorithm and
    predicting it. They are here rather than only in SlugifierMatchesMkdocs
    because script-lint installs no mkdocs, so the differential test skips
    there and this table is what holds the line.

    The cases are the ones where an approximation of GitHub's slugifier - the
    obvious thing to reach for, and what a first draft of this used - gives a
    different answer, so a regression to one would fail here rather than in the
    gate: a run of hyphens collapses, non-ASCII folds to ASCII rather than
    surviving, a repeat is numbered `_1` and not `-1`, and a code span is
    literal, keeping the `<...>` and the leading `_` that the surrounding
    markdown would otherwise lose.
    """

    CASES: ClassVar[dict[str, str]] = {
        # The two anchors this check was written for.
        "Objects, and what it took to fit them": "objects-and-what-it-took-to-fit-them",
        "Objects": "objects",
        "How much memory there actually is": "how-much-memory-there-actually-is",
        "Per-application capture: the Core Audio process tap":
            "per-application-capture-the-core-audio-process-tap",
        # A code span is literal: neither the angle brackets nor the leading
        # underscore is touched by the tag strip or the emphasis strip.
        "`objects=<layout>`": "objectslayout",
        "The `_into` decode forms": "the-_into-decode-forms",
        "Live options (`live`): `capture2=`, `objects=`, `downmix=`":
            "live-options-live-capture2-objects-downmix",
        "`--fast-mdct` flag": "-fast-mdct-flag",
        "Section 3 with `code` here": "section-3-with-code-here",
        "2 `a` 1 `b` 0": "2-a-1-b-0",
        # An underscore inside a word is a word character and stays; a pair
        # around a word is emphasis and goes.
        "AC3FORGE_STAGE_TIMERS and the stage timers": "ac3forge_stage_timers-and-the-stage-timers",
        "_emphasis_ around a word": "emphasis-around-a-word",
        "**bold** and *italic* together": "bold-and-italic-together",
        # A run of hyphens or spaces collapses to one hyphen. GitHub keeps it.
        "Step 1 - overview": "step-1-overview",
        "Step 2 -- double dash": "step-2-double-dash",
        "E-AC-3 / AC-3": "e-ac-3-ac-3",
        "---": "-",
        "trailing hyphen -": "trailing-hyphen-",
        "— leading em dash": "leading-em-dash",
        # Non-ASCII folds to its ASCII decomposition, or vanishes when it has none.
        "naive resume (naïve résumé)": "naive-resume-naive-resume",
        "žlutý kůň": "zluty-kun",
        "\U0001f534 A red circle": "a-red-circle",
        # A link in a heading contributes its text, never its URL.
        "[A link](https://example.com/x-y) in a heading": "a-link-in-a-heading",
        "<span>tagged</span> heading": "tagged-heading",
        # Punctuation this repo's headings are full of.
        "C++23 and the /std:c++latest flag": "c23-and-the-stdclatest-flag",
        "Tears of Steel: 5.1 at 640 kbit/s": "tears-of-steel-51-at-640-kbits",
        "fgaincod: SNR and MOS opposed": "fgaincod-snr-and-mos-opposed",
        "Bit-rate (kbps)": "bit-rate-kbps",
        "100% of PRs": "100-of-prs",
        "Q&A and R&D": "qa-and-rd",
        "50/50 split": "5050-split",
        "Encoder / decoder mirror": "encoder-decoder-mirror",
        "Where the numbers came from, and what they mean":
            "where-the-numbers-came-from-and-what-they-mean",
    }

    def test_mkdocs_slugs(self) -> None:
        for heading, expected in self.CASES.items():
            with self.subTest(heading=heading):
                text = check_doc_paths.heading_text(heading)
                self.assertEqual(check_doc_paths.slug_mkdocs(text), expected)

    def test_repeated_heading_is_numbered_with_an_underscore(self) -> None:
        """And the numbering re-reads its own suffix, so `Status_1` is pushed past it."""
        lines = ["## Status", "", "## Status", "", "## Status", "", "## Status_1"]
        used: set[str] = set()
        slugs = [
            check_doc_paths.unique_mkdocs(
                check_doc_paths.slug_mkdocs(check_doc_paths.heading_text(raw)), used)
            for raw in check_doc_paths.raw_headings(lines)
        ]
        self.assertEqual(slugs, ["status", "status_1", "status_2", "status_3"])

    def test_github_numbers_a_repeat_with_a_hyphen(self) -> None:
        counts: dict[str, int] = {}
        slugs = [check_doc_paths.unique_github("status", counts) for _ in range(3)]
        self.assertEqual(slugs, ["status", "status-1", "status-2"])

    def test_github_keeps_what_mkdocs_folds(self) -> None:
        """The differences that make accepting either spelling a decision, not an accident."""
        for heading, github in {
            "[0.4.0-beta.1] - 2026-08-14": "040-beta1---2026-08-14",
            "žlutý kůň": "žlutý-kůň",
            "Step 2 -- double dash": "step-2----double-dash",
        }.items():
            with self.subTest(heading=heading):
                text = check_doc_paths.heading_text(heading)
                self.assertEqual(check_doc_paths.slug_github(text), github)
                self.assertNotEqual(check_doc_paths.slug_mkdocs(text), github)


class SlugifierMatchesMkdocs(unittest.TestCase):
    """The differential the table above was cut from, when the renderer is installed.

    Skipped in script-lint, which installs no mkdocs; it runs in a docs
    environment and locally, and is what to reach for when a heading shape
    turns up that HeadingSlugs has no case for.
    """

    # The subset of mkdocs.yml's markdown_extensions that changes what counts as
    # a heading or what its id is. superfences earns its place: without it a
    # ``` block is not a fence, and every `# comment` in a shell transcript
    # renders as a heading, which would compare this against the wrong answer.
    # snippets is here for docs/contributing.md and docs/roadmap.md, which have
    # no headings of their own and are entirely a root file included verbatim.
    EXTENSIONS = (
        "attr_list", "md_in_html", "pymdownx.superfences", "pymdownx.snippets", "tables", "toc",
    )

    def _ids(self, page: Path, root: Path) -> list[str]:
        import re  # noqa: PLC0415 - kept beside the optional import below

        import markdown  # noqa: PLC0415 - optional; the test skips when it is absent

        converter = markdown.Markdown(
            extensions=list(self.EXTENSIONS),
            extension_configs={"pymdownx.snippets": {"base_path": [str(root)]}},
        )
        html = converter.convert(page.read_text(encoding="utf-8"))
        return re.findall(r'<h[1-6][^>]*\bid="([^"]*)"', html)

    def _mkdocs_ids(self, page: Path, root: Path) -> list[str]:
        """What this module would compute, mkdocs side only, in document order."""
        used: set[str] = set()
        ids = []
        for heading in check_doc_paths.raw_headings(check_doc_paths.page_lines(page, root)):
            attrs = check_doc_paths.ATTR_LIST_TAIL.search(heading)
            explicit = None
            if attrs and attrs.group(1).strip():
                for token in attrs.group(1).split():
                    if token.startswith("#"):
                        explicit = token[1:]
                heading = heading[: attrs.start()]  # noqa: PLW2901 - the id replaces the slug
            if explicit:
                ids.append(explicit)
                used.add(explicit)
                continue
            text = check_doc_paths.heading_text(heading)
            ids.append(check_doc_paths.unique_mkdocs(check_doc_paths.slug_mkdocs(text), used))
        return ids

    def test_every_heading_in_the_tree(self) -> None:
        from importlib.util import find_spec  # noqa: PLC0415 - it decides the imports in _ids

        if find_spec("markdown") is None or find_spec("pymdownx") is None:  # pragma: no cover
            self.skipTest("mkdocs' renderer is not installed; HeadingSlugs pins the rules instead")
        root = Path(__file__).resolve().parents[2]
        pages = sorted(p for p in root.glob("docs/**/*.md") if p.is_file())
        self.assertTrue(pages, "no docs pages found to check")
        for page in pages:
            with self.subTest(page=page.relative_to(root).as_posix()):
                # Exactly, not as a subset: the id the site publishes is the one
                # this computes, or a link that resolves there fails here.
                self.assertEqual(self._mkdocs_ids(page, root), self._ids(page, root))


class LinkAnchors(unittest.TestCase):
    """A #fragment names a heading on the page it points at, or the link is dead."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def test_live_anchor_passes_and_dead_one_fails_with_file_and_line(self) -> None:
        _write(self.root, "docs/target.md", "# Target\n\n## How it fits\n")
        _write(
            self.root,
            "docs/page.md",
            "ok [a](target.md#how-it-fits)\n\nbad [b](target.md#how-it-used-to-fit)\n",
        )
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(len(report.problems), 1)
        self.assertIn("docs/page.md:3:", report.problems[0])
        self.assertIn("link anchor does not exist", report.problems[0])
        self.assertIn("target.md#how-it-used-to-fit", report.problems[0])

    def test_same_page_anchor_is_checked(self) -> None:
        """The case the check used to skip outright rather than resolve."""
        _write(self.root, "docs/page.md", "## Here\n\n[a](#here) and [b](#gone)\n")
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(len(report.problems), 1)
        self.assertIn("#gone", report.problems[0])

    def test_a_dead_path_is_reported_once_not_twice(self) -> None:
        """No anchor verdict on a page that is not there; the missing file is the finding."""
        _write(self.root, "docs/page.md", "[a](gone.md#somewhere)\n")
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(len(report.problems), 1)
        self.assertIn("link target does not exist", report.problems[0])

    def test_github_spelling_passes_on_a_page_github_renders(self) -> None:
        """CHANGELOG.md is read on GitHub, where ' - ' is three hyphens, not one."""
        changelog = "## [0.4.0-beta.1] - 2026-08-14\n\n[a](#040-beta1---2026-08-14)\n"
        _write(self.root, "CHANGELOG.md", changelog)
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(report.problems, [])

    def test_explicit_and_html_ids_are_anchors(self) -> None:
        page = (
            "## Named { #chosen-id }\n\n"
            '<div id="trend-app"></div>\n\n'
            "[a](#chosen-id) and [b](#trend-app) and [c](#named)\n"
        )
        _write(self.root, "docs/page.md", page)
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(len(report.problems), 1)
        self.assertIn("#named", report.problems[0])  # attr_list replaces the slug

    def test_snippet_headings_belong_to_the_including_page(self) -> None:
        """docs/contributing.md has no headings of its own; it is CONTRIBUTING.md."""
        _write(self.root, "CONTRIBUTING.md", "## How to build\n")
        _write(self.root, "docs/contributing.md", '--8<-- "CONTRIBUTING.md"\n')
        _write(self.root, "docs/page.md", "[a](contributing.md#how-to-build)\n")
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(report.problems, [])

    def test_headings_in_code_and_raw_html_are_not_anchors(self) -> None:
        page = (
            "```\n## Fenced heading\n```\n\n"
            "<style>\n#css-selector { color: red; }\n</style>\n\n"
            "[a](#fenced-heading)\n"
        )
        _write(self.root, "docs/page.md", page)
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(len(report.problems), 1)
        self.assertIn("#fenced-heading", report.problems[0])

    def test_setext_headings_are_anchors(self) -> None:
        _write(self.root, "docs/page.md", "Underlined\n==========\n\n[a](#underlined)\n")
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(report.problems, [])

    def test_a_fragment_on_a_non_markdown_target_is_not_judged(self) -> None:
        """Nothing here can read that file's headings, so it says nothing."""
        _write(self.root, "docs/assets/demo.html", "<h2 id='x'>x</h2>\n")
        _write(self.root, "docs/page.md", "[a](assets/demo.html#anything-at-all)\n")
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(report.problems, [])

    def test_percent_encoded_fragment_resolves(self) -> None:
        """A non-ASCII anchor arrives percent-encoded and has to be decoded first."""
        _write(self.root, "planning/target.md", "## žlutý kůň\n")
        _write(
            self.root,
            "planning/page.md",
            "[a](target.md#%C5%BElut%C3%BD-k%C5%AF%C5%88)\n",
        )
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(report.problems, [])


class MergedBranchAnchors(unittest.TestCase):
    """The shape only a merge produces, which is why this runs in the merge queue.

    Reduced from the pair of pull requests that motivated the check: #603
    renamed docs/platforms/esp32.md's '## Objects, and what it took to fit them'
    to '## Objects', and #605 added a link to the old anchor from
    docs/performance-trend.md. Each branch is internally consistent and passes
    on its own. The link is dead only in the tree the two of them make, which no
    per-branch run ever builds and the merge queue always does.
    """

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    OLD_HEADING = "## Objects, and what it took to fit them\n"
    NEW_HEADING = "## Objects\n"
    OLD_LINK = "[the page](platforms/esp32.md#objects-and-what-it-took-to-fit-them)\n"
    NEW_LINK = "[the page](platforms/esp32.md#objects)\n"

    def _tree(self, heading: str, link: str) -> check_doc_paths.Report:
        _write(self.root, "docs/platforms/esp32.md", f"# ESP32-S3\n\n{heading}")
        _write(self.root, "docs/performance-trend.md", f"# Trend\n\n{link}")
        return check_doc_paths.check_tree(self.root)

    def test_either_branch_alone_passes(self) -> None:
        self.assertEqual(self._tree(self.OLD_HEADING, self.OLD_LINK).problems, [])
        self.assertEqual(self._tree(self.NEW_HEADING, self.NEW_LINK).problems, [])

    def test_the_merge_of_the_two_fails(self) -> None:
        report = self._tree(self.NEW_HEADING, self.OLD_LINK)
        self.assertEqual(len(report.problems), 1)
        self.assertIn("docs/performance-trend.md:3:", report.problems[0])
        self.assertIn("objects-and-what-it-took-to-fit-them", report.problems[0])


class PlanningAnchors(unittest.TestCase):
    """planning/ is unpublished but links into docs/, one directory up."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def test_a_plan_link_into_docs_is_checked(self) -> None:
        _write(self.root, "docs/platforms/esp32.md", "## How much memory there actually is\n")
        _write(
            self.root,
            "planning/topology.md",
            "[live](../docs/platforms/esp32.md#how-much-memory-there-actually-is)\n\n"
            "[dead](../docs/platforms/esp32.md#how-much-memory-there-is)\n",
        )
        report = check_doc_paths.check_tree(self.root)
        self.assertEqual(len(report.problems), 1)
        self.assertIn("planning/topology.md:3:", report.problems[0])


class Main(unittest.TestCase):
    def test_exit_code_follows_findings(self) -> None:
        """main() prints an ::error:: line for the deliberately broken link below;
        captured rather than left on stdout so it doesn't read as a real CI
        annotation when this suite runs inside the script-lint job."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            _write(root, "docs/a.md", "[b](b.md)\n")
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(check_doc_paths.main(["check", "--root", tmp]), 1)
            _write(root, "docs/b.md", "# b\n")
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(check_doc_paths.main(["check", "--root", tmp]), 0)


if __name__ == "__main__":
    unittest.main()
