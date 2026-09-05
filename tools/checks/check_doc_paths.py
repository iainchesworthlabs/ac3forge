#!/usr/bin/env python3
"""Assert the paths the documentation and the CI plumbing name still exist.

A file that moves takes its README row, its workflow step and the comments
that cite it along only when someone remembers. This makes forgetting a red
job. Two checks, both stdlib-only, run from ci.yml's script-lint job and
runnable the same way locally:

    python3 tools/checks/check_doc_paths.py [--root <repo>]

(a) Every relative Markdown link under docs/**/*.md and in README.md,
    CONTRIBUTING.md, SECURITY.md and CHANGELOG.md resolves to a file or a
    directory. Anchors are stripped, http(s)/mailto targets are skipped, and
    links inside fenced code blocks and inline code spans are ignored (they
    are examples of the syntax, not links). ROADMAP.md is skipped on purpose,
    and says so in the run's output: it is read both on GitHub and as a
    snippet included into docs/roadmap.md, so its links must be absolute URLs
    (docs/roadmap.md states that rule) and a relative-link check would be
    wrong in one of the two places.

(b) Every path literal starting docs/, apps/, src/ or tools/ inside
    .github/workflows/*.yml, cmake/*.cmake, CMakePresets.json and
    tools/**/*.{py,sh,ps1} names something that exists. Conservative on
    purpose. A token has to start at a word boundary, contain a slash and end
    at whitespace, a quote or a bracket; a trailing #anchor is a section
    reference, not part of the path, and is stripped before the check. Four
    shapes are skipped rather than judged, each printed as one info line so
    what the check declines to answer stays visible:

      - globs (* ?), placeholders (${...}, $var, %var%) and identifiers a
        comment wrapped mid-token, which name no single path;
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

MARKDOWN_GLOBS = ("docs/**/*.md",)
MARKDOWN_FILES = ("README.md", "CONTRIBUTING.md", "SECURITY.md", "CHANGELOG.md", "ROADMAP.md")
MARKDOWN_SKIP = {
    "ROADMAP.md": "included into docs/roadmap.md as a snippet, so its links are absolute URLs",
}
LITERAL_GLOBS = (
    ".github/workflows/*.yml",
    "cmake/*.cmake",
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

INLINE_LINK = re.compile(r"\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
REFERENCE_LINK = re.compile(r"^ {0,3}\[[^\]]+\]:\s*(\S+)")
CODE_SPAN = re.compile(r"`[^`\n]*`")
FENCE = re.compile(r"^ {0,3}(```|~~~)")
SCHEME = re.compile(r"^[a-z][a-z0-9+.-]*:")

# A token starts at a word boundary (so the docs/ inside a URL's /blob/main/docs/
# is not one), and ends at whitespace, a quote, a bracket or a punctuation mark
# that never appears inside a path here. PowerShell writes its paths with
# backslashes, so the .ps1 variant accepts either separator and normalises.
TOKEN_TAIL = r"[^\s\"'`\[\]()<>,;:|]+"
PREFIX_ALTERNATION = "|".join(LITERAL_PREFIXES)
PATH_TOKEN = re.compile(r"(?<![\w./\\-])((?:" + PREFIX_ALTERNATION + r")/" + TOKEN_TAIL + ")")
PS1_TOKEN = re.compile(r"(?<![\w./\\-])((?:" + PREFIX_ALTERNATION + r")[/\\]" + TOKEN_TAIL + ")")
GLOB_CHARS = ("*", "?")
PLACEHOLDER_MARKS = ("${", "{", "}", "$", "%")


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


def link_targets(lines: list[str]) -> list[tuple[int, str]]:
    """The (line, target) pairs of every link outside fenced code and code spans."""
    targets: list[tuple[int, str]] = []
    in_fence = False
    for number, line in enumerate(lines, start=1):
        if FENCE.match(line):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        reference = REFERENCE_LINK.match(line)
        if reference:
            targets.append((number, reference.group(1)))
            continue
        for match in INLINE_LINK.finditer(CODE_SPAN.sub("", line)):
            targets.append((number, match.group(1)))
    return targets


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
            report.checked += 1
            if not (root / token).exists():
                report.problems.append(f"{where}:{number}: path does not exist: {token}")


def check_tree(root: Path) -> Report:
    report = Report()
    patterns = ignore_patterns(root)
    for path in markdown_files(root):
        reason = MARKDOWN_SKIP.get(_display(path, root))
        if reason:
            report.skipped.append(f"{_display(path, root)}: whole file ({reason})")
            continue
        check_markdown(path, root, report)
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
