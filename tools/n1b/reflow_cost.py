"""How many source lines overflow the 100-column limit if 'ac3::' becomes a longer root?

Counts, over C++ files, the lines that name `ac3::` and are within the limit now but would exceed it
once each `ac3` is replaced by a root of a given length (and `#include` lines, which clang-format
never reflows, are skipped). The total is an upper bound: it assumes every `ac3::` is rewritten.

Usage: reflow_cost.py [--root R] [--limit 100]
"""

from __future__ import annotations

import re
from collections import Counter

from n1b_lib import CPP_EXT, Repo, base_parser, emit, md_table

QUAL = re.compile(r"\bac3::")
ROOTS = {"icl": 3, "forge": 5, "iclforge": 8, "ac3 (unchanged)": 3}


def main() -> None:
    ap = base_parser(__doc__)
    ap.add_argument("--limit", type=int, default=100)
    a = ap.parse_args()
    repo = Repo(a.root)
    lines_with = 0
    over = Counter()
    over_files = {k: set() for k in ROOTS}
    for f in repo.files:
        if repo.ext(f) not in CPP_EXT:
            continue
        for line in repo.read(f).splitlines():
            n = len(QUAL.findall(line))
            if not n or line.lstrip().startswith("#include"):
                continue
            lines_with += 1
            base = len(line.rstrip("\r"))
            for name, w in ROOTS.items():
                if base <= a.limit < base + n * (w - 3):
                    over[name] += 1
                    over_files[name].add(f)
    rows = [[k, ROOTS[k], over[k], len(over_files[k])] for k in ROOTS]
    text = f"lines naming ac3:: (excluding #include): {lines_with}\n\n" + md_table(
        ["new root", "chars", f"lines newly over {a.limit} cols", "files"],
        rows,
        ["l", "r", "r", "r"],
    )
    emit(text, a.out)


if __name__ == "__main__":
    main()
