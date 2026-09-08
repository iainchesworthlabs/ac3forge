#!/usr/bin/env python3
"""Assert the paths the documentation and the CI plumbing name still exist.

A file that moves takes its README row, its workflow step and the comments
that cite it along only when someone remembers. This makes forgetting a red
job. Three checks, all stdlib-only, run from ci.yml's script-lint job and
runnable the same way locally:

    python3 tools/checks/check_doc_paths.py [--root <repo>]

(a) Every relative Markdown link under docs/**/*.md and in README.md,
    CONTRIBUTING.md, SECURITY.md and CHANGELOG.md resolves to a file or a
    directory. Anchors are stripped, http(s)/mailto targets are skipped, and
    links inside fenced code blocks and inline code spans are ignored (they
    are examples of the syntax, not links). A link whose text wraps across a
    line break is still one link, so links are matched over a whole paragraph
    rather than a line at a time, and reported against the line each one opens
    on. ROADMAP.md is held to the opposite rule: it is read both on GitHub and
    as a snippet included into docs/roadmap.md, so a relative link there can
    only resolve from one of the two places. Every link in it must be an
    absolute http(s) URL or a bare #anchor (docs/roadmap.md states that rule),
    and anything else fails.

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
FOREIGN_PATHS = {
    "src/main.ts": "actions/setup-python's own source, cited by _build.yml",
    "src/main.cpp": "linuxdeploy's own source, cited by _build.yml",
    "src/core/generate-excludelist.sh": "linuxdeploy's own source, cited by _build.yml",
    "src/deployers/PlatformPluginsDeployer.cpp":
        "linuxdeploy-plugin-qt's own source, cited by _build.yml",
    "src/qml.cpp": "linuxdeploy-plugin-qt's own source, cited by _build.yml",
    "src/qml.h": "linuxdeploy-plugin-qt's own source, cited by _build.yml",
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
        if HTTP_URL.match(target) or target.startswith("#"):
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


def check_markdown(path: Path, root: Path, report: Report) -> None:
    lines = path.read_text(encoding="utf-8").splitlines()
    where = _display(path, root)
    for number, raw in link_targets(lines):
        target = raw.strip("<>")
        if SCHEME.match(target):
            continue
        target = unquote(target.split("#", 1)[0])
        if not target:
            continue  # an anchor within the same page
        report.checked += 1
        resolved = root / target.lstrip("/") if target.startswith("/") else path.parent / target
        if not resolved.exists():
            report.problems.append(f"{where}:{number}: link target does not exist: {raw}")


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
