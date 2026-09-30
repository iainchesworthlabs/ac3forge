"""Deep-compare two data directories written by the census scripts and print where they differ.

json_diff.py <old dir> <new dir> [--limit N]
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

FILES = [
    "include_graph.json",
    "inventory.json",
    "namespaces.json",
    "ident_census.json",
    "naming_counts.json",
    "path_keyed.json",
    "overlap.json",
    "dryrun.json",
    "symtab.json",
]
TEXT = ["violations_v2.txt", "privcross.txt", "reflow.md"]


def walk(a, b, path, out):
    if type(a) is not type(b):
        out.append((path, repr(a)[:80], repr(b)[:80]))
    elif isinstance(a, dict):
        for k in sorted(set(a) | set(b), key=str):
            if k not in a:
                out.append((f"{path}/{k}", "<absent>", repr(b[k])[:80]))
            elif k not in b:
                out.append((f"{path}/{k}", repr(a[k])[:80], "<absent>"))
            else:
                walk(a[k], b[k], f"{path}/{k}", out)
    elif isinstance(a, list):
        if len(a) != len(b):
            out.append((path + " (length)", str(len(a)), str(len(b))))
        for i, (x, y) in enumerate(zip(a, b, strict=False)):
            walk(x, y, f"{path}[{i}]", out)
    elif a != b:
        out.append((path, repr(a)[:80], repr(b)[:80]))


def main() -> None:
    old, new = Path(sys.argv[1]), Path(sys.argv[2])
    limit = int(sys.argv[sys.argv.index("--limit") + 1]) if "--limit" in sys.argv else 12
    for name in FILES:
        a = json.loads((old / name).read_text(encoding="utf-8"))
        b = json.loads((new / name).read_text(encoding="utf-8"))
        out: list = []
        walk(a, b, "", out)
        print(f"{name}: {len(out)} differences")
        for p, x, y in out[:limit]:
            print(f"    {p}: {x} -> {y}")
    for name in TEXT:
        a = (old / name).read_text(encoding="utf-8-sig", errors="replace")
        b = (new / name).read_text(encoding="utf-8-sig", errors="replace")
        print(f"{name}: {'same' if a.strip() == b.strip() else 'DIFFERENT'}")


if __name__ == "__main__":
    main()
