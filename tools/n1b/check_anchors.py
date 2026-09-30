"""Check the relative links and anchors of the study's pages, as GitHub and MkDocs slug headings.

    check_anchors.py <repo root>

Every `](target)` whose target is a relative .md file, a bare #anchor, or file.md#anchor is
resolved; a missing file or anchor is printed. Slugs: lower case, backticks and punctuation dropped
(letters, digits, spaces, hyphens and underscores stay), spaces become hyphens, a repeated heading
gets -1, -2.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

PAGES = [
    "planning/layout.md",
    "planning/layout-inventory.md",
    "planning/ac4.md",
    "planning/README.md",
]


def slug(text: str) -> str:
    text = re.sub(r"`", "", text)
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = text.strip().lower()
    text = re.sub(r"[^\w\- ]", "", text)
    return text.replace(" ", "-")


def anchors(path: Path) -> set[str]:
    seen: dict[str, int] = {}
    out = set()
    in_fence = False
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        m = re.match(r"^(#{1,6})\s+(.*?)\s*#*\s*$", line)
        if m:
            s = slug(m.group(2))
            n = seen.get(s, 0)
            seen[s] = n + 1
            out.add(s if n == 0 else f"{s}-{n}")
    return out


def main() -> int:
    root = Path(sys.argv[1])
    bad = 0
    cache: dict[Path, set[str]] = {}
    for page in PAGES:
        p = root / page
        in_fence = False
        for no, line in enumerate(p.read_text(encoding="utf-8").splitlines(), 1):
            if line.lstrip().startswith("```"):
                in_fence = not in_fence
                continue
            if in_fence:
                continue
            for m in re.finditer(r"\]\(([^)\s]+)\)", line):
                target = m.group(1)
                if re.match(r"^[a-z]+://", target) or target.startswith("mailto:"):
                    continue
                file_part, _, frag = target.partition("#")
                dest = p if not file_part else (p.parent / file_part).resolve()
                if not dest.exists():
                    print(f"{page}:{no}: missing file {target}")
                    bad += 1
                    continue
                if frag and dest.suffix == ".md":
                    if dest not in cache:
                        cache[dest] = anchors(dest)
                    if frag not in cache[dest]:
                        print(f"{page}:{no}: missing anchor {target}")
                        bad += 1
    print(f"{bad} problems")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
