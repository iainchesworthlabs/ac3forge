"""The includes that stop src/forge from splitting: those the dependency table forbids.

    violations.py --graph <include_graph.json> [--table tools/checks/layering.json] [--out <file>]

Reads the include graph include_graph.py wrote (every include of every C/C++ file, resolved) and
lists, per pair of libraries, the include directives that go from a library into one its row of the
table does not list. Libraries are what layoutdef.library_of names, so this is the list of cuts the
split needs, before they are made (layout-inventory.md section B.4). tools/checks/check_layering.py
is the check that keeps the count at zero afterwards; this script only reports it from a graph that
has already been built.
"""

from __future__ import annotations

import json
from collections import defaultdict
from pathlib import Path

from layoutdef import library_of
from n1b_lib import base_parser, emit

DEFAULT_TABLE = Path(__file__).resolve().parents[1] / "checks" / "layering.json"


def violations(graph: dict, allowed: dict[str, list[str]]) -> list[tuple[str, str, str, str, str]]:
    out = []
    for f, includes in graph["per_file_includes"].items():
        a = library_of(f) if f.startswith("src/") else None
        if a is None:
            continue
        for spelling, target, _owner in includes:
            b = library_of(target) if target.startswith("src/") else None
            if b is None or b == a:
                continue
            if b not in allowed.get(a, []):
                out.append((a, b, f, target, spelling))
    return out


def main() -> None:
    ap = base_parser(__doc__)
    ap.add_argument("--graph", required=True, help="include_graph.json from include_graph.py")
    ap.add_argument("--table", default=str(DEFAULT_TABLE), help="the dependency table")
    a = ap.parse_args()
    graph = json.loads(Path(a.graph).read_text(encoding="utf-8"))
    table = json.loads(Path(a.table).read_text(encoding="utf-8"))["libraries"]
    found = violations(graph, table)
    by_pair: dict[tuple[str, str], list[tuple[str, str, str]]] = defaultdict(list)
    for a_lib, b_lib, f, t, spelling in found:
        by_pair[(a_lib, b_lib)].append((f, t, spelling))
    lines = [f"{len(found)} violating include directives in {len({x[2] for x in found})} files\n\n"]
    for (a_lib, b_lib), items in sorted(by_pair.items()):
        lines.append(f"## {a_lib} -> {b_lib}: {len(items)}\n")
        seen = set()
        for f, _t, spelling in items:
            if (f, spelling) in seen:
                continue
            seen.add((f, spelling))
            lines.append(f"  {f.replace('src/forge/', '')} -> {spelling}\n")
        lines.append("\n")
    emit("".join(lines), a.out)


if __name__ == "__main__":
    main()
