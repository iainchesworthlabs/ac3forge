#!/usr/bin/env python3
"""Assert the paths the documentation and the CI plumbing name still exist.

A file that moves takes its README row, its workflow step and the comments
that cite it along only when someone remembers. This makes forgetting a red
job. Three checks, all stdlib-only, run from ci.yml's script-lint job and
runnable the same way locally:

    python3 tools/checks/check_doc_paths.py [--root <repo>]

(a) Every relative Markdown link under docs/**/*.md and in README.md,
    CONTRIBUTING.md, SECURITY.md and CHANGELOG.md resolves to a file or a
    directory, and its #anchor names a heading on the page it lands on.
    http(s)/mailto targets are skipped, and links inside fenced code blocks and
    inline code spans are ignored (they are examples of the syntax, not links).
    A link whose text wraps across a line break is still one link, so links are
    matched over a whole paragraph rather than a line at a time, and reported
    against the line each one opens on. ROADMAP.md is held to the opposite
    rule: it is read both on GitHub and as a snippet included into
    docs/roadmap.md, so a relative link there can only resolve from one of the
    two places. Every link in it must be an absolute http(s) URL or a bare
    #anchor (docs/roadmap.md states that rule), and anything else fails.

    The anchor half is the reason this script runs where it does. A heading
    renamed in one branch and linked from another leaves both branches
    internally consistent and the merge of them broken, which is a state no
    per-branch run ever builds. ci.yml's script-lint job has no event gating,
    so main's merge queue evaluates this over each entry's merged tree and the
    pair is caught there; an ordinary link to a heading that never existed is
    caught on the pull request, before that. What a heading's id is - the two
    slugifiers this repo's pages are read through, and which of their
    disagreements matter - is set out at ATX_HEADING below, and pinned against
    the real renderer by test_check_doc_paths.py's HeadingSlugs.

(b) Every path a documentation page names in its own prose or a code span,
    as opposed to in a link, resolves. A page can go on citing a directory for
    years after the tree moved it, because nothing was reading those. Fenced
    blocks are exempt (shell transcripts and source listings, not claims about
    the tree), and so are the pages in PROSE_PATHS_UNCHECKED below: a plan
    proposing a layout, a phase record describing the tree before a rename,
    and CHANGELOG.md, whose released entries are immutable. Their links are
    still checked; only their prose is exempt.

(c) Every path literal starting docs/, apps/, src/ or tools/ inside
    .github/workflows/*.yml, cmake/**/*.cmake, CMakePresets.json and
    tools/**/*.{py,sh,ps1} names something that exists. Conservative on
    purpose. A token has to start at a word boundary, contain a slash and end
    at whitespace, a quote or a bracket; a trailing #anchor is a section
    reference, not part of the path, and is stripped before the check. Four
    shapes are skipped rather than judged, each printed as one info line so
    what the check declines to answer stays visible:

      - globs (* ?), placeholders (${...}, $var, %var%) and identifiers a
        comment wrapped mid-token, which name no single path. A brace group is
        no longer one of these: `a/{b,c}/d` expands to one path per alternative
        and each is checked, since skipping the token meant none of the
        siblings ever was;
      - anything .gitignore covers, which is generated rather than stale:
        docs/spec/'s standards documents, build/ outputs, the Android
        signing-key asset a runner materialises, src/quarantine;
      - the tokens in FOREIGN_PATHS below, which are references into another
        project's source tree or into a subdirectory of this one, and are
        listed one by one with their reason rather than guessed at;
      - test_*.py under tools/, whose path literals are fixtures for a
        temporary tree and not references into this one.

Exit 1 with one ::error:: line per missing target, naming file:line.
"""

from __future__ import annotations

import argparse
import re
import sys
import unicodedata
from dataclasses import dataclass, field
from fnmatch import fnmatch
from pathlib import Path
from urllib.parse import unquote

MARKDOWN_GLOBS = ("docs/**/*.md", "planning/*.md")
MARKDOWN_FILES = ("README.md", "CONTRIBUTING.md", "SECURITY.md", "CHANGELOG.md", "ROADMAP.md")
# Files whose links must be absolute rather than resolvable from this tree,
# with the reason the inverted rule applies to them.
ABSOLUTE_LINKS_ONLY = {
    "ROADMAP.md": "also a snippet in docs/roadmap.md, where a relative link cannot resolve",
}
LITERAL_GLOBS = (
    ".github/workflows/*.yml",
    "cmake/**/*.cmake",
    "CMakePresets.json",
    "tools/**/*.py",
    "tools/**/*.sh",
    "tools/**/*.ps1",
)
# The prefixes the literal check treats as a repo-relative path.
LITERAL_PREFIXES = ("docs", "apps", "src", "tools")

# Tokens that read as repo-relative paths but are not. Each is a deliberate
# exception with its reason, printed on every run so the list stays under the
# same scrutiny as the checks themselves.
_LINUXDEPLOY_PLUGIN_QT_SOURCE = "linuxdeploy-plugin-qt's own source, cited by _build.yml"
FOREIGN_PATHS = {
    "src/main.ts": "actions/setup-python's own source, cited by _build.yml",
    "src/main.cpp": "linuxdeploy's own source, cited by _build.yml",
    "src/core/generate-excludelist.sh": "linuxdeploy's own source, cited by _build.yml",
    "src/deployers/PlatformPluginsDeployer.cpp": _LINUXDEPLOY_PLUGIN_QT_SOURCE,
    "src/qml.cpp": _LINUXDEPLOY_PLUGIN_QT_SOURCE,
    "src/qml.h": _LINUXDEPLOY_PLUGIN_QT_SOURCE,
    "src/main/assets": "relative to the Android app module, not the repo root",
    "src/main/assets/signing.key": "relative to the Android app module, not the repo root",
}

# Paths a plan proposes but the tree does not have yet. A brace token expands to
# one path per alternative (see expand_braces), so these are listed individually
# and, like FOREIGN_PATHS, printed on every run. A path that lands should be
# deleted from here, which is what makes the plan's own prose fall due.
PLANNED_PATHS = {
    "apps/forge/cli": "proposed by the recasting plan, not created yet",
    "apps/forge/gui": "proposed by the recasting plan, not created yet",
    "apps/forge/common": "proposed by the recasting plan, not created yet",
    "apps/hearth/platform/linux": "proposed by the playback-appliance plan, not created yet",
    "apps/hearth/platform/windows": "proposed by the playback-appliance plan, not created yet",
    "apps/hearth/platform/macos": "proposed by the playback-appliance plan, not created yet",
    "apps/crucible/platform": "proposed by the playback-appliance plan, not created yet",
    "apps/windows/engine": "the pre-promotion layout the promotion record names",
    "apps/windows/runner": "the pre-promotion layout the promotion record names",
    "apps/windows/ui": "the pre-promotion layout the promotion record names",
    "apps/windows/translations": "the pre-promotion layout the promotion record names",
    "apps/windows/spikes": "the pre-promotion layout the promotion record names",
}

# Markdown pages whose prose deliberately names paths that do not exist: a plan
# proposing a layout, or a phase record describing the tree as it was before a
# rename. Their *links* are still checked - only the paths written in prose and
# code spans are exempt, because those pages are not claiming the tree looks
# like that today.
PROSE_PATHS_UNCHECKED = {
    "planning/README.md": "index of plans; names the directories they propose",
    "planning/recasting.md": "plan; proposes a layout that does not exist yet",
    "planning/topology.md": "plan; proposes applications that do not exist yet",
    "planning/player-appliance.md": "plan; proposes an apps tree that does not exist",
    "planning/host-plugin.md": "study; proposes an Assay component and its own docs tree",
    "planning/qc-report.md": "plan; proposes source files it would add",
    "docs/crucible/promotion.md": "phase record; names the pre-promotion apps/windows layout",
    "docs/platforms/windows-demo.md": "phase record; names the pre-promotion apps/windows layout",
    "CHANGELOG.md": "released entries are an immutable record of the tree as it was",
}

# Link text runs to the next ']' but may not contain a '[', which is CommonMark's
# rule that a ']' closes the most recent unclosed '[' and not some earlier one.
# It matters because the text may now span lines: without it the '[' of a prose
# interval like [0, 32) would pair with the ']' of a real link further down the
# paragraph, and the link would be reported against the interval's line.
INLINE_LINK = re.compile(r"\[[^\[\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
REFERENCE_LINK = re.compile(r"^ {0,3}\[[^\]]+\]:\s*(\S+)")
CODE_SPAN = re.compile(r"`[^`\n]*`")
FENCE = re.compile(r"^ {0,3}(```|~~~)")
SCHEME = re.compile(r"^[a-z][a-z0-9+.-]*:")
HTTP_URL = re.compile(r"^https?://", re.IGNORECASE)

# --- Heading anchors -------------------------------------------------------
#
# What a #fragment has to match. A heading's id is not the heading, it is what
# a slugifier made of it, and this repo's pages are read through two of them:
# mkdocs builds docs/, GitHub renders the root pages and planning/. The two
# agree on ordinary prose and differ on the edges - mkdocs (python-markdown's
# `toc`, which mkdocs.yml selects by configuring `toc` with nothing but
# `permalink`) folds non-ASCII to ASCII, collapses a run of hyphens to one and
# disambiguates a repeated heading with `_1`; GitHub keeps the run, keeps the
# non-ASCII and counts with `-1`. So a fragment is accepted when it matches
# under either. That is deliberately the loose reading: CONTRIBUTING.md and
# ROADMAP.md are genuinely read both ways (docs/ includes them verbatim via
# pymdownx.snippets), CHANGELOG.md and planning/ only ever on GitHub, and a
# check that picked one slugifier per file would fail a link that resolves
# perfectly well where the file is actually read. The cases this gate exists
# for - a heading renamed out from under a link, in one branch or across two -
# fail under both spellings, so nothing that matters is lost by accepting both.
ATX_HEADING = re.compile(r"^ {0,3}(#{1,6})(.*?)#*\s*$")
SETEXT_RULE = re.compile(r"^ {0,3}(=+|-+)\s*$")
FENCE_RUN = re.compile(r"^ {0,3}(`{3,}|~{3,})")
# <script>/<style> hold CSS and JS, where a `#id` selector is not a heading and
# `document.getElementById` is not a link. python-markdown stashes both as raw
# HTML and never looks inside; neither does this.
RAW_HTML_OPEN = re.compile(r"<(script|style)\b", re.IGNORECASE)
RAW_HTML_CLOSE = re.compile(r"</(script|style)\s*>", re.IGNORECASE)
# `--8<-- "FILE"`: pymdownx.snippets, which is how docs/contributing.md and
# docs/roadmap.md are the root files. The included headings are the page's.
SNIPPET_LINE = re.compile(r"^\s*-{2}8<-{2}\s+[\"']?([^\"'\s]+)[\"']?\s*$")
# attr_list, which mkdocs.yml enables: `## Heading { #explicit-id }` names its
# own id and skips the slugifier entirely.
ATTR_LIST_TAIL = re.compile(r"\{:?\s*([^}]*)\}\s*$")
# An id a page writes by hand, on the <div> a chart mounts into or a bare <a>.
HTML_ANCHOR = re.compile(r"<[a-zA-Z][^>]*?\b(?:id|name)\s*=\s*[\"']([^\"']+)[\"']")
HEADING_CODE_SPAN = re.compile(r"(`+)(.+?)\1")
HEADING_IMAGE = re.compile(r"!\[([^\]]*)\]\([^)]*\)")
HEADING_INLINE_LINK = re.compile(r"\[([^\]]*)\]\([^)]*\)")
HEADING_REFERENCE_LINK = re.compile(r"\[([^\]]*)\]\[[^\]]*\]")
HEADING_HTML_TAG = re.compile(r"<[^>]+>")
HEADING_STARS = re.compile(r"\*{1,3}")
# `_` is a word character, so the slugifier keeps the one in AC3FORGE_STAGE_TIMERS
# and drops the pair around _emphasis_. Markdown draws that line at word
# boundaries, and so does this: a run of underscores goes only where it opens or
# closes emphasis rather than sitting inside an identifier.
HEADING_UNDERSCORES = re.compile(r"(?<![A-Za-z0-9])_{1,3}|_{1,3}(?![A-Za-z0-9])")
HEADING_ESCAPE = re.compile(r"\\(.)")
# U+E000 is private-use: it cannot occur in a heading, so a stashed code span
# cannot be confused with the digits of a heading that has its own numbers in it.
CODE_SENTINEL = chr(0xE000)
STASHED_SPAN = re.compile(CODE_SENTINEL + r"(\d+)" + CODE_SENTINEL)
ID_COUNT = re.compile(r"^(.*)_([0-9]+)$")

# A token starts at a word boundary (so the docs/ inside a URL's /blob/main/docs/
# is not one), and ends at whitespace, a quote, a bracket or a punctuation mark
# that never appears inside a path here. PowerShell writes its paths with
# backslashes, so the .ps1 variant accepts either separator and normalises.
#
# A brace group is the one place a comma belongs to the path rather than ending
# it: a token of the form <prefix>/x/{a,b}/y names two paths, not one path and
# some prose. Without the first branch below the token stopped at the comma,
# leaving a dangling open brace that was then declined as a placeholder, so
# expand_braces never saw a group at all and the expansion was dead code.
TOKEN_CHAR = r"[^\s\"'`\[\]()<>,;:|{}]"
TOKEN_TAIL = r"(?:\{" + TOKEN_CHAR + r"*(?:," + TOKEN_CHAR + r"*)*\}|" + TOKEN_CHAR + r")+"
PREFIX_ALTERNATION = "|".join(LITERAL_PREFIXES)
PATH_TOKEN = re.compile(r"(?<![\w./\\-])((?:" + PREFIX_ALTERNATION + r")/" + TOKEN_TAIL + ")")
PS1_TOKEN = re.compile(r"(?<![\w./\\-])((?:" + PREFIX_ALTERNATION + r")[/\\]" + TOKEN_TAIL + ")")
GLOB_CHARS = ("*", "?")
PLACEHOLDER_MARKS = ("${", "$", "%")
BRACE_GROUP = re.compile(r"\{([^{}]+)\}")


def expand_braces(token: str) -> list[str]:
    """`a/{b,c}/d` -> [`a/b/d`, `a/c/d`]. Docs use the shell's own shorthand for
    a set of sibling paths, and skipping the whole token as a placeholder meant
    none of the siblings was ever checked - which is how a scalar-type seam that
    had moved directories survived on the ESP32-S3 page."""
    match = BRACE_GROUP.search(token)
    if not match:
        return [token]
    out: list[str] = []
    for raw_alternative in match.group(1).split(","):
        alternative = raw_alternative.strip()
        if not alternative:
            return [token]  # `{}` is not a set of alternatives; leave it alone
        out.extend(expand_braces(token[: match.start()] + alternative + token[match.end() :]))
    return out


@dataclass
class Report:
    """What one run found: the failures, the shapes it declined to judge, and how much it read."""

    problems: list[str] = field(default_factory=list)
    skipped: list[str] = field(default_factory=list)
    checked: int = 0
    # Heading anchors, by resolved page: a hub page's link targets are read once
    # per run rather than once per link into them.
    anchors: dict[Path, set[str]] = field(default_factory=dict)


def _display(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


def ignore_patterns(root: Path) -> list[str]:
    """.gitignore's positive patterns, leading slash removed."""
    path = root / ".gitignore"
    if not path.is_file():
        return []
    patterns = []
    for line in path.read_text(encoding="utf-8").splitlines():
        entry = line.strip()
        if not entry or entry.startswith(("#", "!")):
            continue
        patterns.append(entry.lstrip("/"))
    return patterns


def is_ignored(token: str, patterns: list[str]) -> bool:
    """Whether .gitignore covers this path, by gitignore's own anchoring rule.

    A pattern containing a slash is anchored at the repository root; one
    without a slash matches a path component anywhere. Enough of the format
    to answer "is this generated rather than stale", which is the only
    question asked of it here.
    """
    for pattern in patterns:
        body = pattern.rstrip("/")
        if "/" in body:
            if token == body or token.startswith(body + "/"):
                return True
        elif any(fnmatch(part, body) for part in token.split("/")):
            return True
    return False


def markdown_files(root: Path) -> list[Path]:
    files = [p for pattern in MARKDOWN_GLOBS for p in root.glob(pattern) if p.is_file()]
    files += [root / name for name in MARKDOWN_FILES if (root / name).is_file()]
    return sorted(set(files))


def literal_files(root: Path) -> list[Path]:
    files = [p for pattern in LITERAL_GLOBS for p in root.glob(pattern) if p.is_file()]
    return sorted(p for p in set(files) if not p.name.startswith("test_"))


def scrubbed_lines(lines: list[str]) -> list[str]:
    """Those lines with fenced code blocks and inline code spans blanked out.

    Blanked rather than dropped: every line keeps its position, and a code span
    is replaced by as many spaces as it occupied, so an offset into the joined
    text still names the line and column it came from.
    """
    scrubbed: list[str] = []
    in_fence = False
    for line in lines:
        if FENCE.match(line):
            in_fence = not in_fence
            scrubbed.append("")
        elif in_fence:
            scrubbed.append("")
        else:
            scrubbed.append(CODE_SPAN.sub(lambda span: " " * len(span.group()), line))
    return scrubbed


def paragraphs(lines: list[str]) -> list[tuple[int, str]]:
    """The blank-line-separated blocks of those lines, each with the line it starts on."""
    blocks: list[tuple[int, str]] = []
    start = 0
    block: list[str] = []
    for number, line in enumerate(lines, start=1):
        if line.strip():
            if not block:
                start = number
            block.append(line)
        elif block:
            blocks.append((start, "\n".join(block)))
            block = []
    if block:
        blocks.append((start, "\n".join(block)))
    return blocks


def link_targets(lines: list[str]) -> list[tuple[int, str]]:
    """The (line, target) pairs of every link outside fenced code and code spans.

    Inline links are matched over a whole paragraph, not a line at a time,
    because Markdown lets the text of one wrap across a line break and the
    result is still a single link. A blank line ends a paragraph and so cannot
    occur inside a link, which is what bounds the span any one match may cover.
    Pairs come back in source order, each numbered by the line its link opens
    on rather than the line its target sits on.
    """
    scrubbed = scrubbed_lines(lines)
    found: list[tuple[int, int, str]] = []
    for number, line in enumerate(scrubbed, start=1):
        reference = REFERENCE_LINK.match(line)
        if reference:
            found.append((number, reference.start(1), reference.group(1)))
            scrubbed[number - 1] = ""  # a definition, now taken, and not paragraph text
    for start, block in paragraphs(scrubbed):
        for match in INLINE_LINK.finditer(block):
            opens = match.start()
            line_break = block.rfind("\n", 0, opens)
            found.append(
                (start + block.count("\n", 0, opens), opens - line_break - 1, match.group(1))
            )
    return [(number, target) for number, _, target in sorted(found)]


def check_absolute_links(path: Path, root: Path, reason: str, report: Report) -> None:
    """The inverted rule: every link is an absolute http(s) URL or a bare #anchor."""
    lines = path.read_text(encoding="utf-8").splitlines()
    where = _display(path, root)
    for number, raw in link_targets(lines):
        target = raw.strip("<>")
        report.checked += 1
        if target.startswith("#"):
            # The one relative form the rule allows, because it resolves the
            # same in both places the page is read. It still has to land.
            check_fragment(unquote(target[1:]), path, where, number, raw, root, report)
            continue
        if HTTP_URL.match(target):
            continue
        report.problems.append(f"{where}:{number}: link must be an absolute URL ({reason}): {raw}")


def check_markdown_prose(path: Path, root: Path, patterns: list[str], report: Report) -> None:
    """Paths written in a page's prose and code spans, not just in its links.

    Markdown links were always checked; a path in a code span was not, which is
    how the ESP32-S3 page went on naming an internal directory one level up from
    where the tree actually has it. Fenced blocks are skipped -
    they hold shell transcripts and source listings, not claims about the tree.
    """
    where = _display(path, root)
    if where in PROSE_PATHS_UNCHECKED:
        report.skipped.append(f"{where}: prose paths ({PROSE_PATHS_UNCHECKED[where]})")
        return
    fenced = False
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        if FENCE.match(line):
            fenced = not fenced
            continue
        if fenced:
            continue
        for span in CODE_SPAN.finditer(line):
            for match in PATH_TOKEN.finditer(span.group(0)):
                token = match.group(1).rstrip(".").split("#", 1)[0]
                if not token or classify_token(token, patterns):
                    continue
                for expansion in expand_braces(token):
                    candidate = expansion.rstrip("/")
                    if not candidate or candidate in FOREIGN_PATHS or candidate in PLANNED_PATHS:
                        continue
                    if is_ignored(candidate, patterns):
                        continue
                    report.checked += 1
                    if not (root / candidate).exists():
                        shown = token if candidate == token else f"{candidate} (from {token})"
                        report.problems.append(
                            f"{where}:{number}: path does not exist: {shown}")


def slug_mkdocs(text: str) -> str:
    """python-markdown's `toc.slugify`, which is what mkdocs.yml's `toc` selects.

    Folds to ASCII, drops everything that is not a word character, whitespace or
    a hyphen, and collapses each run of the two into one hyphen.
    """
    folded = unicodedata.normalize("NFKD", text).encode("ascii", "ignore").decode("ascii")
    return re.sub(r"[-\s]+", "-", re.sub(r"[^\w\s-]", "", folded).strip().lower())


def slug_github(text: str) -> str:
    """GitHub's, which the root pages and planning/ are read through.

    Keeps the non-ASCII and, unlike mkdocs', keeps every hyphen it is given: the
    ' - ' in a CHANGELOG heading stays three hyphens rather than becoming one.
    """
    return re.sub(r"\s", "-", re.sub(r"[^\w\s-]", "", text.strip().lower()))


def heading_text(raw: str) -> str:
    """The plain text a heading renders to, which is what the slugifier is given.

    Code spans are stashed before anything else and restored last, so that what
    is inside one stays literal - `objects=<layout>` keeps its angle brackets
    instead of losing them to the tag strip, and `_into` keeps its underscore
    instead of losing it to the emphasis strip. Both are real headings here.
    """
    spans: list[str] = []

    def stash(match: re.Match[str]) -> str:
        spans.append(match.group(2))
        return f"{CODE_SENTINEL}{len(spans) - 1}{CODE_SENTINEL}"

    text = HEADING_CODE_SPAN.sub(stash, raw)
    text = HEADING_IMAGE.sub(r"\1", text)
    text = HEADING_INLINE_LINK.sub(r"\1", text)
    text = HEADING_REFERENCE_LINK.sub(r"\1", text)
    text = HEADING_HTML_TAG.sub("", text)
    text = HEADING_STARS.sub("", text)
    text = HEADING_UNDERSCORES.sub("", text)
    text = HEADING_ESCAPE.sub(r"\1", text)
    text = STASHED_SPAN.sub(lambda m: spans[int(m.group(1))], text)
    return " ".join(text.split())


def page_lines(path: Path, root: Path, seen: frozenset[Path] = frozenset()) -> list[str]:
    """The page's lines with its `--8<--` snippets spliced in, as the site reads it."""
    lines: list[str] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        match = SNIPPET_LINE.match(line)
        if match:
            included = (root / match.group(1)).resolve()
            if included.is_file() and included not in seen:
                lines.extend(page_lines(included, root, seen | {included}))
                continue
        lines.append(line)
    return lines


def raw_headings(lines: list[str]) -> list[str]:
    """Every heading's own Markdown, in document order, outside code and raw HTML."""
    headings: list[str] = []
    fence: str | None = None
    previous: str | None = None
    raw_html = False
    for line in lines:
        if raw_html:
            if RAW_HTML_CLOSE.search(line):
                raw_html = False
            previous = None
            continue
        run = FENCE_RUN.match(line)
        if fence is not None:
            if run and run.group(1)[0] == fence[0] and len(run.group(1)) >= len(fence):
                fence = None
            previous = None
            continue
        if run:
            fence = run.group(1)
            previous = None
            continue
        if RAW_HTML_OPEN.search(line):
            raw_html = not RAW_HTML_CLOSE.search(line)
            previous = None
            continue
        atx = ATX_HEADING.match(line)
        if atx:
            headings.append(atx.group(2))
            previous = None
            continue
        if SETEXT_RULE.match(line) and previous and previous.strip():
            headings.append(previous)
            previous = None
            continue
        previous = line
    return headings


def unique_mkdocs(slug: str, used: set[str]) -> str:
    """python-markdown's `toc.unique`: a repeat becomes `_1`, then `_2`.

    It re-reads the suffix it just wrote rather than counting, so a heading that
    ends in `_1` of its own collides with the numbering and is pushed to `_2`.
    """
    while slug in used or not slug:
        match = ID_COUNT.match(slug)
        slug = f"{match.group(1)}_{int(match.group(2)) + 1}" if match else f"{slug}_1"
    used.add(slug)
    return slug


def unique_github(slug: str, counts: dict[str, int]) -> str:
    """GitHub's: the nth repeat of a slug is that slug with `-<n-1>` after it."""
    seen = counts.get(slug, 0)
    counts[slug] = seen + 1
    return slug if seen == 0 else f"{slug}-{seen}"


def page_anchors(path: Path, root: Path) -> set[str]:
    """Every fragment the page answers to, under either slugifier.

    Explicit ids win outright: attr_list's `{ #id }` on a heading, and the id or
    name attribute of any element the page writes by hand - the <div> a trend
    chart mounts into is as real a link target as a heading is.
    """
    lines = page_lines(path, root)
    anchors: set[str] = {match.group(1) for line in lines for match in HTML_ANCHOR.finditer(line)}
    used_mkdocs: set[str] = set()
    counts_github: dict[str, int] = {}
    for heading in raw_headings(lines):
        explicit = None
        attrs = ATTR_LIST_TAIL.search(heading)
        if attrs and attrs.group(1).strip():
            for token in attrs.group(1).split():
                if token.startswith("#"):
                    explicit = token[1:]
            heading = heading[: attrs.start()]  # noqa: PLW2901 - the id replaces the slug
        if explicit:
            anchors.add(explicit)
            used_mkdocs.add(explicit)
            continue
        text = heading_text(heading)
        anchors.add(unique_mkdocs(slug_mkdocs(text), used_mkdocs))
        anchors.add(unique_github(slug_github(text), counts_github))
    return anchors


def anchors_of(path: Path, root: Path, cache: dict[Path, set[str]]) -> set[str]:
    key = path.resolve()
    if key not in cache:
        try:
            cache[key] = page_anchors(path, root)
        except OSError:
            cache[key] = set()
    return cache[key]


def check_fragment(
    fragment: str,
    destination: Path,
    where: str,
    number: int,
    raw: str,
    root: Path,
    report: Report,
) -> None:
    """A #fragment names a heading on the destination page, or the link is dead.

    Only Markdown destinations are judged. A fragment on anything else - the
    HTML the site builds, an asset - is a target this cannot read the headings
    of, and a guess either way would be worse than saying nothing.
    """
    if destination.suffix.lower() != ".md" or not destination.is_file():
        return
    report.checked += 1
    if fragment not in anchors_of(destination, root, report.anchors):
        report.problems.append(f"{where}:{number}: link anchor does not exist: {raw}")


def check_markdown(path: Path, root: Path, report: Report) -> None:
    lines = path.read_text(encoding="utf-8").splitlines()
    where = _display(path, root)
    for number, raw in link_targets(lines):
        target = raw.strip("<>")
        if SCHEME.match(target):
            continue
        head, _, fragment = target.partition("#")
        head, fragment = unquote(head), unquote(fragment)
        if not head:
            # An anchor within the same page: no path to resolve, but the
            # heading it names still has to be there.
            if fragment:
                check_fragment(fragment, path, where, number, raw, root, report)
            continue
        report.checked += 1
        resolved = root / head.lstrip("/") if head.startswith("/") else path.parent / head
        if not resolved.exists():
            report.problems.append(f"{where}:{number}: link target does not exist: {raw}")
            continue
        if fragment:
            check_fragment(fragment, resolved, where, number, raw, root, report)


def classify_token(token: str, patterns: list[str]) -> str | None:
    """Why a token is not checked, or None when it should be."""
    if any(mark in token for mark in GLOB_CHARS):
        return "glob"
    if any(mark in token for mark in PLACEHOLDER_MARKS):
        return "placeholder"
    if token.endswith(("_", "-")):
        return "line-wrapped identifier"
    if token in FOREIGN_PATHS:
        return FOREIGN_PATHS[token]
    if is_ignored(token, patterns):
        return "gitignored, so generated rather than stale"
    return None


def check_literals(path: Path, root: Path, patterns: list[str], report: Report) -> None:
    pattern = PS1_TOKEN if path.suffix == ".ps1" else PATH_TOKEN
    where = _display(path, root)
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        for match in pattern.finditer(line):
            token = match.group(1).replace("\\", "/").rstrip(".")
            token = token.split("#", 1)[0]  # a trailing #anchor names a section, not a path
            if not token:
                continue
            reason = classify_token(token, patterns)
            if reason:
                report.skipped.append(f"{where}:{number}: {token} ({reason})")
                continue
            for expansion in expand_braces(token):
                candidate = expansion.rstrip("/")
                if not candidate:
                    continue
                if candidate in FOREIGN_PATHS:
                    report.skipped.append(
                        f"{where}:{number}: {candidate} ({FOREIGN_PATHS[candidate]})")
                    continue
                if candidate in PLANNED_PATHS:
                    report.skipped.append(
                        f"{where}:{number}: {candidate} ({PLANNED_PATHS[candidate]})")
                    continue
                if is_ignored(candidate, patterns):
                    report.skipped.append(
                        f"{where}:{number}: {candidate} "
                        "(gitignored, so generated rather than stale)")
                    continue
                report.checked += 1
                if not (root / candidate).exists():
                    shown = token if candidate == token else f"{candidate} (from {token})"
                    report.problems.append(f"{where}:{number}: path does not exist: {shown}")


def check_tree(root: Path) -> Report:
    report = Report()
    patterns = ignore_patterns(root)
    for path in markdown_files(root):
        reason = ABSOLUTE_LINKS_ONLY.get(_display(path, root))
        if reason:
            check_absolute_links(path, root, reason, report)
            continue
        check_markdown(path, root, report)
        check_markdown_prose(path, root, patterns, report)
    for path in literal_files(root):
        check_literals(path, root, patterns, report)
    return report


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="repository root (default: two directories above this script)",
    )
    args = parser.parse_args(argv[1:])
    root = args.root.resolve()
    report = check_tree(root)
    for entry in report.skipped:
        print(f"info: skipped {entry}")
    for problem in report.problems:
        print(f"::error::{problem}")
    print(
        f"{len(report.problems)} missing, {report.checked} checked, "
        f"{len(report.skipped)} skipped under {root}"
    )
    return 1 if report.problems else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
