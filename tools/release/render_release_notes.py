"""Render a GitHub Release body from CHANGELOG.md, for a given release tag.

`.github/workflows/release.yml`'s `github-release` job used to call
`gh release create --generate-notes`, which drafts a commit list - useful as a first
draft, but not what actually ships: docs/releasing.md's post-release checklist has
always said the *curated* CHANGELOG.md section is the authoritative record and the
GitHub Release body should mirror it, not the other way round. That curation already
happens where it belongs - in CHANGELOG.md, as part of normal development, grouped by
user-facing area with bold lead-in bullets - so by release time there is nothing left
to draft: the matching `## [x.y.z] - date` section already reads exactly like a release
body wants to. This just extracts it.

What is deliberately NOT reconstructed here, still left as optional human polish per
docs/releasing.md: an "Artifacts" section with per-package checksums (the checksums are
already attached to the release as individual `*.sha512` files and a `SHA512SUMS`
manifest - restating them in prose adds a second place to keep in sync) and the
pre-release caveat blockquote (picking the single biggest open gap to headline is a
judgement call, not an extraction).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# CHANGELOG.md's own heading shape (Keep a Changelog): "## [0.9.0-beta.1] - 2026-08-22".
# `[Unreleased]` uses the same "## [" prefix and is what bounds the newest released
# section from above; every other "## [" heading bounds sections from below.
_SECTION_START_RE = "^## \\[{version}\\].*$"
_ANY_SECTION_HEADING_RE = re.compile(r"^## \[([^\]]+)\]", re.MULTILINE)


class ChangelogSectionNotFound(RuntimeError):
    pass


def find_section(changelog_text: str, version: str) -> tuple[str, str | None]:
    """Return (section_body, previous_version) for `version`'s CHANGELOG.md section.

    `previous_version` is the tag name (without a "v" prefix) of the next section
    below this one, or None if this is the first-ever release.
    """
    start_re = re.compile(_SECTION_START_RE.format(version=re.escape(version)), re.MULTILINE)
    start_match = start_re.search(changelog_text)
    if start_match is None:
        raise ChangelogSectionNotFound(
            f"no CHANGELOG.md section heading '## [{version}]' - update CHANGELOG.md "
            "before tagging (docs/releasing.md's pre-release checklist: move the "
            "section down from ## [Unreleased] first)"
        )

    body_start = start_match.end()
    next_heading = _ANY_SECTION_HEADING_RE.search(changelog_text, pos=body_start)
    body_end = next_heading.start() if next_heading else len(changelog_text)
    previous_version = next_heading.group(1) if next_heading else None

    return changelog_text[body_start:body_end].strip("\n"), previous_version


def render(section_body: str, *, repo: str, tag: str, previous_version: str | None) -> str:
    parts = [section_body]
    if previous_version is not None:
        compare_url = f"https://github.com/{repo}/compare/v{previous_version}...{tag}"
        parts.append(f"**Full Changelog**: {compare_url}")
    return "\n\n".join(parts) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--changelog", type=Path, default=Path("CHANGELOG.md"))
    parser.add_argument("--version", required=True, help="bare version, e.g. 0.9.0-beta.1")
    parser.add_argument("--tag", required=True, help="full tag, e.g. v0.9.0-beta.1")
    parser.add_argument("--repo", required=True, help="owner/repo, e.g. iainchesworthlabs/ac3forge")
    parser.add_argument("--output", type=Path, help="write notes here instead of stdout")
    args = parser.parse_args(argv)

    changelog_text = args.changelog.read_text(encoding="utf-8")
    try:
        section_body, previous_version = find_section(changelog_text, args.version)
    except ChangelogSectionNotFound as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    notes = render(section_body, repo=args.repo, tag=args.tag, previous_version=previous_version)
    if args.output is not None:
        args.output.write_text(notes, encoding="utf-8", newline="\n")
    else:
        print(notes, end="")
    return 0


if __name__ == "__main__":
    sys.exit(main())
