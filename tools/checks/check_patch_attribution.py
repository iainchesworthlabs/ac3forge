#!/usr/bin/env python3
"""Reject AI self-attribution in a commit series, and confirm the author (and,
once applied, the committer) is who they claim to be.

A parallel session hands back a `git format-patch` series to review before it
is applied and merged. This is the mechanical half of that review - the part
that should never rely on a human (or another session) remembering to eyeball
the last few lines of every commit message. Two input modes:

    python3 tools/checks/check_patch_attribution.py --patch some.patch
    python3 tools/checks/check_patch_attribution.py --range main..HEAD

`--patch` reads one or more commits straight out of a `git format-patch`/
`git am` mbox file, without needing them applied anywhere - cheap, and the
first thing to run against whatever a parallel session hands back. `--range`
reads already-committed history via `git log` (`--no-merges`), for confirming
what actually landed after `git am`; it is the only mode that also has a
committer to check, since a raw patch file does not carry one. Exactly one of
the two is required. `--root` picks the repository for `--range` and for the
default identity (below); it defaults to this script's own repo.

Each commit's subject+body is checked for:

  - a `Co-authored-by:` trailer
  - a "Generated with Claude Code" / similar line, or the accompanying robot
    emoji
  - a bare mention of "Claude" or "Anthropic" (word-boundary, case-insensitive)
  - an `@anthropic.com` address

matching this project's standing rule: no AI self-attribution anywhere, ever,
not just the obvious trailer. Deliberately broad - a commit that genuinely
needs to say "claude" for an unrelated reason should be reworded, the same
call this project already makes everywhere else.

Each commit's author (`--range`: and committer) name and email is checked
against `--expect-name`/`--expect-email`, which default to this repository's
own `git config user.name`/`user.email` - the identity every real commit in
`git log` already carries. A patch whose author is correct but whose email
has drifted (or vice versa) fails just as loudly as an AI-attributed one.

Deliberately not checked here:

  - Mentions of "session" in commit text (a Claude session standing in for a
    human, e.g. "owned by another session"). Also against this project's
    rules, but the word is used for legitimate technical things often enough
    (network session, user session) that a mechanical check would misfire
    more than it would catch; that one stays a human's job at review.
  - The diff content itself. This checks the commit message, not the code it
    describes - a legitimate identifier or comment in the changed files is
    not this script's concern, and scanning it would risk exactly the kind of
    false positive the point above avoids.
  - Merge commits in `--range` mode (excluded with `--no-merges`): GitHub's
    merge-button commits carry a `github.com`-issued committer identity by
    design, which is a different question from whether the feature commits
    underneath were attributed correctly.

Exit 1 with one ::error:: line per violation.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

FROM_LINE_RE = re.compile(r"^From [0-9a-f]{40} .+$", re.MULTILINE)
SUBJECT_PREFIX_RE = re.compile(r"^\[PATCH[^\]]*\]\s*")
FROM_HEADER_RE = re.compile(r"(.*)\s<(.*)>$")

FIELD_SEP = "\x1f"
RECORD_SEP = "\x1e"

_GENERATED_WITH_CLAUDE = re.compile(
    r"generated (with|by)\b.{0,20}claude", re.IGNORECASE | re.DOTALL
)

ATTRIBUTION_PATTERNS: list[tuple[re.Pattern[str], str]] = [
    (re.compile(r"^Co-authored-by:", re.IGNORECASE | re.MULTILINE), "a Co-authored-by trailer"),
    (_GENERATED_WITH_CLAUDE, '"Generated with Claude Code"'),
    (re.compile(r"\U0001f916"), "the robot-emoji attribution line"),
    (re.compile(r"\bclaude\b", re.IGNORECASE), 'a mention of "Claude"'),
    (re.compile(r"\banthropic\b", re.IGNORECASE), 'a mention of "Anthropic"'),
    (re.compile(r"@anthropic\.com", re.IGNORECASE), "an @anthropic.com address"),
]


@dataclass
class Commit:
    label: str
    text: str  # subject + body, for the attribution scan
    author_name: str
    author_email: str
    committer_name: str | None = None
    committer_email: str | None = None


def git_config(root: Path, key: str) -> str:
    result = subprocess.run(
        ["git", "config", key], cwd=root, capture_output=True, text=True, check=False
    )
    return result.stdout.strip()


def commits_from_patch(path: Path) -> list[Commit]:
    text = path.read_text(encoding="utf-8")
    starts = [m.start() for m in FROM_LINE_RE.finditer(text)]
    if not starts:
        return []
    ends = [*starts[1:], len(text)]
    return [_parse_patch_chunk(text[a:b]) for a, b in zip(starts, ends, strict=True)]


def _parse_patch_chunk(chunk: str) -> Commit:
    lines = chunk.splitlines()[1:]  # drop the "From <sha> <date>" line
    headers: dict[str, list[str]] = {}
    last_header: str | None = None
    end = len(lines)
    for i, line in enumerate(lines):
        if line == "":
            end = i + 1
            break
        if line[:1] in (" ", "\t") and last_header is not None:
            headers[last_header].append(line.strip())
            continue
        name, _, value = line.partition(":")
        last_header = name.strip()
        headers.setdefault(last_header, []).append(value.strip())

    from_header = " ".join(headers.get("From", [""]))
    match = FROM_HEADER_RE.match(from_header)
    author_name, author_email = (match.group(1), match.group(2)) if match else (from_header, "")

    subject = SUBJECT_PREFIX_RE.sub("", " ".join(headers.get("Subject", [])))

    body_lines: list[str] = []
    for line in lines[end:]:
        if line == "---":
            break
        body_lines.append(line)

    text = subject + "\n" + "\n".join(body_lines)
    return Commit(subject[:72] or "(no subject)", text, author_name, author_email)


def commits_from_range(root: Path, rev_range: str) -> list[Commit]:
    fmt = FIELD_SEP.join(["%H", "%an", "%ae", "%cn", "%ce", "%s", "%b"]) + RECORD_SEP
    # --end-of-options (git 2.24+) keeps the caller's range from being read as an option: a
    # range of `--output=<file>` would otherwise have git log write to that file. After it, a
    # leading dash just makes a revision that does not exist.
    result = subprocess.run(
        ["git", "log", "--no-merges", f"--format={fmt}", "--end-of-options", rev_range],
        cwd=root,
        capture_output=True,
        text=True,
        check=True,
    )
    commits = []
    for raw in result.stdout.split(RECORD_SEP):
        record = raw.strip("\n")
        if not record:
            continue
        fields = record.split(FIELD_SEP, 6)
        sha, author_name, author_email, committer_name, committer_email, subject, body = fields
        label = f"{sha[:10]} {subject[:60]}"
        text = subject + "\n" + body
        commit = Commit(label, text, author_name, author_email, committer_name, committer_email)
        commits.append(commit)
    return commits


def check_commit(commit: Commit, expect_name: str, expect_email: str) -> list[str]:
    violations = []
    for pattern, description in ATTRIBUTION_PATTERNS:
        if pattern.search(commit.text):
            violations.append(f"::error::{commit.label}: {description} in the commit message")

    def identity_ok(name: str, email: str) -> bool:
        return name == expect_name and email.lower() == expect_email.lower()

    if not identity_ok(commit.author_name, commit.author_email):
        violations.append(
            f"::error::{commit.label}: author is {commit.author_name} <{commit.author_email}>, "
            f"expected {expect_name} <{expect_email}>"
        )
    committer_email = commit.committer_email or ""
    committer_ok = identity_ok(commit.committer_name or "", committer_email)
    if commit.committer_name is not None and not committer_ok:
        violations.append(
            f"::error::{commit.label}: committer is {commit.committer_name} <{committer_email}>, "
            f"expected {expect_name} <{expect_email}>"
        )
    return violations


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--patch", type=Path, help="a git-format-patch/git-am mbox file to check")
    parser.add_argument(
        "--range", dest="rev_range", help="a git rev-range already applied, e.g. main..HEAD"
    )
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--expect-name", help="default: this repo's git config user.name")
    parser.add_argument("--expect-email", help="default: this repo's git config user.email")
    args = parser.parse_args()

    if bool(args.patch) == bool(args.rev_range):
        parser.error("pass exactly one of --patch or --range")

    root: Path = args.root.resolve()
    expect_name = args.expect_name or git_config(root, "user.name")
    expect_email = args.expect_email or git_config(root, "user.email")
    if not expect_name or not expect_email:
        print(
            "::error::no --expect-name/--expect-email given, and "
            "git config user.name/user.email is unset"
        )
        return 1

    if args.patch:
        commits = commits_from_patch(args.patch)
    else:
        commits = commits_from_range(root, args.rev_range)

    if not commits:
        print("::error::no commits found to check")
        return 1

    violations: list[str] = []
    for commit in commits:
        violations.extend(check_commit(commit, expect_name, expect_email))

    for violation in violations:
        print(violation)
    print(f"patch attribution: {len(commits)} commit(s) checked, {len(violations)} violation(s)")
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
