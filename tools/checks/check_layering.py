#!/usr/bin/env python3
"""Fail when a library under src/ includes a header its row of the dependency table forbids.

The libraries under src/ form a graph, and the graph has no cycle today. What keeps it that way
is nothing but the habit of the people editing it, and a habit does not survive the next
library: the day a codec-blind library includes a codec's header, that codec can no longer be
left out of a build that does not want it. This check makes the direction data.
tools/checks/layering.json lists every library and the libraries it may include from; this script
resolves every #include of every C/C++ file under src/ the way the compiler does (relative to the
including file, then by the spelling a header is reached by) and reports each include that
crosses from one library into another the table does not allow. Stdlib-only, run from
_static.yml's static job and runnable the same way locally:

    python3 tools/checks/check_layering.py [--root <repo>] [--table <json>] [--edges]

What counts as a library is the second component of a path under src/, with two adjustments the
table's "layout" section describes, both there because the tree is in the middle of a
re-layout (planning/layout.md): a directory that holds several libraries (src/forge) is split
by path rules, first match wins, and a directory renamed for the library it holds (src/ac3adm
is `adm`) is renamed. When the re-layout is done both sections are empty and the rule is the
plain one: src/<library>/.

Four things fail the check, each with a line that names the file and the line of the include:

  - an include that crosses into a library the table does not list for the includer;
  - a file in a library the table does not know (add the row, and say what it may include);
  - a table that has a cycle, or names a library nothing is filed under;
  - a *known debt* that no longer exists (below).

Known debts. A cut that removes an include the table forbids lands as its own change, and the
include is there until it does. Each such include is listed in a file of
tools/checks/layering_debt/ (one file per cut, so that two cuts landing together never edit the
same lines): a line is `<path of the including file> <the spelling it includes>`, blank lines
and # comments are ignored. A listed include is reported as known and does not fail. A listed
include that is no longer in the tree fails, which is what makes the change that removes it
delete its line, and the one that removes a cut's last include delete the file. With the cuts
landed no file is left, and the directory's README.md keeps it in the tree (git has no empty
directory) and says how a debt is listed.

`--edges` prints the include edges between libraries with their counts and stops, which is how a
table row is written in the first place; `--debt-lines` prints every include the table forbids in
the form a debt file takes, which is how a cut's file is written.
"""

from __future__ import annotations

import argparse
import json
import posixpath
import re
import subprocess
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_TABLE = HERE / "layering.json"
DEFAULT_DEBT = HERE / "layering_debt"

INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*([<"])([^>"\n]+)[>"]', re.MULTILINE)
SOURCE_EXT = {".cpp", ".cc", ".cxx", ".c", ".hpp", ".h", ".hh", ".hxx", ".inl", ".ipp", ".mm"}
HEADER_EXT = {".hpp", ".h", ".hh", ".hxx", ".inl", ".ipp"}
# A private header is reached by the tail of its path, the way an include directory would let a
# source name it; the tail is bounded so a long variant path does not index every suffix.
MAX_SUFFIX_PARTS = 5


@dataclass(frozen=True)
class Table:
    """The dependency table and how a path under src/ is filed under a library."""

    libraries: dict[str, list[str]]
    rename: dict[str, str]
    split: list[tuple[re.Pattern[str], str]]

    def library_of(self, path: str) -> str | None:
        parts = path.split("/")
        if parts[0] != "src" or len(parts) < 3:
            return None
        for pattern, library in self.split:
            if pattern.search(path):
                return library
        return self.rename.get(parts[1], parts[1])


@dataclass(frozen=True)
class Edge:
    file: str
    line: int
    spelling: str
    target: str
    source_library: str
    target_library: str


def load_table(path: Path) -> Table:
    raw = json.loads(path.read_text(encoding="utf-8"))
    layout = raw.get("layout", {})
    split = [(re.compile(p), lib) for p, lib in layout.get("split", [])]
    return Table(
        libraries={lib: list(uses) for lib, uses in raw["libraries"].items()},
        rename=dict(layout.get("rename", {})),
        split=split,
    )


def tracked_src_files(root: Path) -> list[str]:
    """Every file under src/ that git tracks, or every file there when this is not a work tree."""
    try:
        listed = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z", "--", "src"],
            capture_output=True,
            check=True,
        ).stdout.decode("utf-8", "surrogateescape")
    except (OSError, subprocess.CalledProcessError):
        base = root / "src"
        return sorted(p.relative_to(root).as_posix() for p in base.rglob("*") if p.is_file())
    files = sorted(f for f in listed.split("\0") if f)
    # A path git lists and the checkout lacks (a deletion not yet committed) is not a file.
    return [f for f in files if (root / f).is_file()]


def header_spellings(path: str) -> list[str]:
    """The strings an #include can use to reach this header, from outside its own directory."""
    generated = path[:-3] if path.endswith((".hpp.in", ".h.in")) else path
    if "/include/" in generated:
        return [generated.split("/include/", 1)[1]]
    parts = generated.split("/")
    return [
        "/".join(parts[i:])
        for i in range(len(parts))
        if "/".join(parts[i:]).count("/") < MAX_SUFFIX_PARTS
    ]


def build_index(files: list[str]) -> dict[str, list[str]]:
    index: dict[str, list[str]] = defaultdict(list)
    for f in files:
        ext = posixpath.splitext(f)[1].lower()
        if ext not in HEADER_EXT and not f.endswith((".hpp.in", ".h.in")):
            continue
        for spelling in header_spellings(f):
            index[spelling].append(f)
    return index


def resolve(
    fileset: set[str],
    index: dict[str, list[str]],
    table: Table,
    frm: str,
    spelling: str,
) -> str | None:
    """The file an include names, or None for a system, external or generated header."""
    relative = posixpath.normpath(posixpath.join(posixpath.dirname(frm), spelling))
    if relative in fileset:
        return relative
    hits = index.get(spelling)
    if not hits:
        return None
    own_library = table.library_of(frm)
    own = [h for h in hits if table.library_of(h) == own_library]
    if own:
        return sorted(own)[0]
    public = [h for h in hits if "/include/" in h]
    return sorted(public or hits)[0]


def find_edges(root: Path, files: list[str], table: Table) -> list[Edge]:
    fileset = set(files)
    index = build_index(files)
    edges: list[Edge] = []
    for f in files:
        if posixpath.splitext(f)[1].lower() not in SOURCE_EXT:
            continue
        source_library = table.library_of(f)
        if source_library is None:
            continue
        text = (root / f).read_text(encoding="utf-8", errors="replace")
        for match in INCLUDE_RE.finditer(text):
            spelling = match.group(2).strip()
            target = resolve(fileset, index, table, f, spelling)
            if target is None:
                continue
            target_library = table.library_of(target)
            if target_library is None or target_library == source_library:
                continue
            line = text.count("\n", 0, match.start()) + 1
            edges.append(Edge(f, line, spelling, target, source_library, target_library))
    return edges


def load_debt(directory: Path) -> dict[tuple[str, str], str]:
    """The known debts: (including file, spelling) -> the debt file that lists it."""
    debt: dict[tuple[str, str], str] = {}
    if not directory.is_dir():
        return debt
    for path in sorted(directory.glob("*.txt")):
        for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            fields = line.split()
            if len(fields) != 2:
                raise ValueError(f"{path.name}:{number}: want '<file> <spelling>', got {raw!r}")
            debt[(fields[0], fields[1])] = path.name
    return debt


def table_problems(table: Table, files: list[str]) -> list[str]:
    """Problems in the table itself: unknown names, a library with no files, a cycle."""
    problems: list[str] = []
    known = set(table.libraries)
    for library, uses in table.libraries.items():
        for used in uses:
            if used not in known:
                problems.append(f"{library} may use {used}, which is not a library of the table")
        if library in uses:
            problems.append(f"{library} lists itself")
    filed = {table.library_of(f) for f in files}
    for library in sorted(known - filed):
        problems.append(f"{library} is in the table and nothing under src/ is filed under it")
    problems.extend(f"cycle in the table: {' -> '.join(c)}" for c in cycles(table.libraries))
    return problems


def cycles(graph: dict[str, list[str]]) -> list[list[str]]:
    """Every strongly connected set of more than one library, by Tarjan's algorithm."""
    index: dict[str, int] = {}
    low: dict[str, int] = {}
    stack: list[str] = []
    on_stack: set[str] = set()
    found: list[list[str]] = []
    counter = [0]

    def visit(node: str) -> None:
        index[node] = low[node] = counter[0]
        counter[0] += 1
        stack.append(node)
        on_stack.add(node)
        for other in graph.get(node, []):
            if other not in graph:
                continue
            if other not in index:
                visit(other)
                low[node] = min(low[node], low[other])
            elif other in on_stack:
                low[node] = min(low[node], index[other])
        if low[node] == index[node]:
            members = []
            while True:
                member = stack.pop()
                on_stack.discard(member)
                members.append(member)
                if member == node:
                    break
            if len(members) > 1:
                found.append(sorted(members))

    for name in sorted(graph):
        if name not in index:
            visit(name)
    return sorted(found)


def print_edges(edges: list[Edge]) -> None:
    counts: dict[tuple[str, str], int] = defaultdict(int)
    for e in edges:
        counts[(e.source_library, e.target_library)] += 1
    for (source, target), n in sorted(counts.items()):
        print(f"{source} -> {target}: {n}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--root", type=Path, default=HERE.parent.parent, help="repository root")
    parser.add_argument("--table", type=Path, default=DEFAULT_TABLE, help="the dependency table")
    parser.add_argument("--debt", type=Path, default=DEFAULT_DEBT, help="the known-debt directory")
    parser.add_argument("--edges", action="store_true", help="print the library edges and stop")
    parser.add_argument(
        "--debt-lines", action="store_true", help="print the forbidden includes as debt lines"
    )
    args = parser.parse_args(argv)

    table = load_table(args.table)
    files = tracked_src_files(args.root)
    if not files:
        print(f"::error::check_layering: no files under {args.root / 'src'}; nothing was checked")
        return 1
    edges = find_edges(args.root, files, table)
    if args.edges:
        print_edges(edges)
        return 0

    if args.debt_lines:
        for e in edges:
            if e.target_library not in table.libraries.get(e.source_library, []):
                print(f"{e.file} {e.spelling}  # {e.source_library} -> {e.target_library}")
        return 0

    failures = 0
    for problem in table_problems(table, files):
        print(f"::error::check_layering: {problem}")
        failures += 1
    for library in sorted({table.library_of(f) for f in files} - set(table.libraries) - {None}):
        print(f"::error::check_layering: src/ holds files of {library}, which the table lacks")
        failures += 1

    debt = load_debt(args.debt)
    seen_debt: set[tuple[str, str]] = set()
    known = 0
    for e in edges:
        if e.target_library in table.libraries.get(e.source_library, []):
            continue
        key = (e.file, e.spelling)
        if key in debt:
            seen_debt.add(key)
            known += 1
            continue
        print(
            f"::error file={e.file},line={e.line}::check_layering: {e.source_library} may not "
            f"include {e.target_library} ({e.spelling}); the table lists "
            f"{', '.join(table.libraries.get(e.source_library, [])) or 'nothing'} for it"
        )
        failures += 1
    for (path, spelling), origin in sorted(debt.items()):
        if (path, spelling) not in seen_debt:
            print(
                f"::error::check_layering: {origin} lists {path} including {spelling}, which is "
                f"no longer a forbidden include: delete that line (and the file when it is empty)"
            )
            failures += 1

    print(
        f"check_layering: {len(table.libraries)} libraries, {len(edges)} include edges between "
        f"them, {known} known debts, {failures} failures"
    )
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
