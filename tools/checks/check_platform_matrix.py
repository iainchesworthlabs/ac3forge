#!/usr/bin/env python3
"""Assert every platform page is reachable from the platform selection matrix.

docs/platforms/index.md routes a reader from what they have to the page that
covers it. A new platform page is useless to that reader until it appears
there, and nothing about adding one makes anybody remember. This makes
forgetting a red job. Stdlib-only, run from ci.yml's script-lint job and
runnable the same way locally:

    python3 tools/checks/check_platform_matrix.py [--root <repo>]

One check: every docs/platforms/*.md other than index.md itself is linked at
least once from index.md. Links inside fenced code blocks and inline code
spans are ignored, the same way check_doc_paths.py ignores them - they are
examples of the syntax, not routes to a page.

Deliberately not checked here:

  - That the links resolve. check_doc_paths.py already asserts that for every
    relative Markdown link under docs/, this page included, and two scripts
    disagreeing about the same rule is worse than one owning it.
  - That a page's row says anything true. The matrix summarises prose, and no
    script can read a status column and tell whether "confirmed on real
    hardware" is still the case. That stays a human's job at review; what this
    guards is the cheaper failure, where the row is absent entirely.
  - Anything about mkdocs.yml's nav. Pages deliberately sit in other nav
    sections (wasm.md under Library, the Crucible pages under Crucible) while
    still being routed to from the matrix, so nav membership and matrix
    membership are different questions.

A page that genuinely should not be routed to goes in UNLISTED below, with the
reason, rather than being dropped silently.

Exit 1 with one ::error:: line per unlisted page.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from urllib.parse import unquote

MATRIX = Path("docs/platforms/index.md")
PLATFORM_GLOB = "docs/platforms/*.md"

# Platform pages the matrix deliberately does not route to, each with the
# reason. Empty today: every page under docs/platforms/ is reachable from the
# matrix, the two Crucible ones through the paragraph below the first table
# rather than through a row of their own.
UNLISTED: dict[str, str] = {}

# ```fenced``` blocks and `inline code`, stripped before links are read.
FENCE_RE = re.compile(r"^\s*(```+|~~~+)")
INLINE_CODE_RE = re.compile(r"`[^`]*`")
# [text](target) - target taken up to whitespace so a "(path 'title')" form
# keeps only the path.
LINK_RE = re.compile(r"\[[^\]]*\]\(\s*([^)\s]+)")


def links_in(text: str) -> set[str]:
    """Every link target in `text`, skipping code fences and inline spans.

    Fences and inline code are stripped per line, but the links themselves are
    read from the joined result: prose wraps, so `[the null-sink driver on\\nACX]`
    is one link split over two lines and matching line-by-line would miss it.
    """
    kept: list[str] = []
    in_fence = False
    for line in text.splitlines():
        if FENCE_RE.match(line):
            in_fence = not in_fence
            continue
        if not in_fence:
            kept.append(INLINE_CODE_RE.sub("", line))

    targets: set[str] = set()
    for match in LINK_RE.finditer("\n".join(kept)):
        target = match.group(1)
        if target.startswith(("http://", "https://", "mailto:", "#")):
            continue
        # Anchors and query strings are not part of the path.
        targets.add(unquote(target.split("#", 1)[0].split("?", 1)[0]))
    return targets


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    root: Path = args.root.resolve()

    matrix = root / MATRIX
    if not matrix.is_file():
        print(f"::error file={MATRIX.as_posix()}::the platform selection matrix is missing")
        return 1

    # Link targets in the matrix are relative to docs/platforms/, so resolve
    # them there and compare resolved paths - "windows.md" and "./windows.md"
    # and "../platforms/windows.md" are all the same page.
    linked = set()
    for target in links_in(matrix.read_text(encoding="utf-8")):
        linked.add((matrix.parent / target).resolve())

    pages = sorted(p for p in root.glob(PLATFORM_GLOB) if p.name != "index.md")
    if not pages:
        print(f"::error file={MATRIX.as_posix()}::no platform pages found under {PLATFORM_GLOB}")
        return 1

    missing = []
    for page in pages:
        rel = page.relative_to(root).as_posix()
        if rel in UNLISTED:
            print(f"info: {rel} is deliberately unlisted - {UNLISTED[rel]}")
            continue
        if page.resolve() not in linked:
            missing.append(rel)

    for rel in missing:
        print(
            f"::error file={MATRIX.as_posix()}::{rel} is not linked from the platform selection "
            f"matrix - add a row for it, or list it in UNLISTED in "
            f"tools/checks/check_platform_matrix.py with the reason"
        )

    print(
        f"platform matrix: {len(pages) - len(UNLISTED)} page(s) checked, "
        f"{len(missing)} unlisted"
    )
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
