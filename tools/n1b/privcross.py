"""Private headers that cross a library boundary under the L2 assignment.

    privcross.py --graph <include_graph.json> [--out <file>]

A header that another library includes has to be reachable by that library, so it becomes a public
header or a `detail/` header of its own (layoutdef.CROSS_DETAIL) when the libraries are split.
This lists them from an include graph that include_graph.py wrote, by owning library and by the
libraries that include each one (layout-inventory.md section B.5). Only includes made from files
under src/ count.
"""

from __future__ import annotations

import json
from collections import defaultdict
from pathlib import Path

from layoutdef import library_of
from n1b_lib import base_parser, emit


def main() -> None:
    ap = base_parser(__doc__)
    ap.add_argument("--graph", required=True, help="include_graph.json from include_graph.py")
    a = ap.parse_args()
    graph = json.loads(Path(a.graph).read_text(encoding="utf-8"))
    by_header: dict[tuple[str, str], dict[str, set[str]]] = defaultdict(lambda: defaultdict(set))
    for f, includes in graph["per_file_includes"].items():
        if not f.startswith("src/"):
            continue
        source = library_of(f)
        for _spelling, target, _owner in includes:
            owner = library_of(target) if target.startswith("src/") else None
            if not owner or not source or source == owner or "/include/" in target:
                continue
            by_header[(owner, target)][source].add(f)
    lines = ["private headers included across libraries from src/ (excluding tests and apps):\n"]
    for (owner, target), users in sorted(by_header.items()):
        used_by = ", ".join(f"{lib} ({len(files)})" for lib, files in sorted(users.items()))
        lines.append(f"  {owner:<10} {target}  <- {used_by}\n")
    emit("".join(lines), a.out)


if __name__ == "__main__":
    main()
