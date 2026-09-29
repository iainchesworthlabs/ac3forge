"""Check that every Markdown table in the given files has the same number of cells in each row."""

import re
import sys
from pathlib import Path


def check(path: str) -> int:
    lines = Path(path).read_text(encoding="utf-8").splitlines()
    bad = 0
    in_fence = False
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
        if not in_fence and line.startswith("|"):
            j = i
            counts = []
            while j < len(lines) and lines[j].startswith("|"):
                cells = re.split(r"(?<!\\)\|", lines[j].strip())
                counts.append(len(cells) - 2)
                j += 1
            if len(set(counts)) != 1:
                bad += 1
                print(f"{path}:{i + 1}: table rows have {sorted(set(counts))} cells")
            i = j
            continue
        i += 1
    return bad


if __name__ == "__main__":
    total = sum(check(p) for p in sys.argv[1:])
    print(f"{total} tables with uneven rows")
    sys.exit(1 if total else 0)
