#!/usr/bin/env python3
"""Fail when a public header declares into a namespace that is not its library's.

Every library under src/ is one namespace under the family root `iclforge`: the AC-3 and E-AC-3
codec is `iclforge::ac3`, AC-4 is `iclforge::ac4`, the MP4 writer is `iclforge::mp4`
(planning/layout.md). A codec that declares into the root itself, as the AC-3 library did until the
namespaces were nested, is a name every other library and every consumer shares with it, and a
consumer cannot tell from the spelling which library a name belongs to. This check makes the
mapping data: tools/checks/namespaces.json lists each library and the namespaces under `iclforge`
its public headers (src/<library>/include/iclforge/<library>/, and the templates `*.hpp.in` that
the build turns into headers) may open, and this script reads every such header the way the
compiler sees its braces (comments and literals blanked) and reports

  - a `namespace iclforge::<x>` block whose first name `<x>` is not one the table lists for the
    library that owns the header;
  - a block `namespace iclforge { ... }` that declares something itself, between its nested
    namespaces: the root holds one namespace per library and no declaration of its own;
  - a library with public headers that the table does not list, and a table row for a library that
    has none.

Known debts. A header the table's `debts` names declares into the root today, and moving the
declaration changes every user of it, so it is listed with the reason, the way
tools/checks/layering_debt/ lists an include a cut has yet to remove. A listed header is reported
as known and does not fail. A listed header that no longer breaks the rule fails, which is what
makes the change that fixes it delete the row. Stdlib-only, run from _static.yml's static job and
runnable the same way locally:

    python3 tools/checks/check_namespaces.py [--root <repo>] [--table <json>] [--census]

`--census` prints, per library, the namespaces its public headers open: how a table row is written
in the first place.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_TABLE = HERE / "namespaces.json"
ROOT_NS = "iclforge"
HEADER_SUFFIXES = (".hpp", ".h", ".hh", ".hxx", ".inl", ".ipp", ".hpp.in", ".h.in")

# --- reading C++ braces ---------------------------------------------------------------------------

_LEXEME = re.compile(
    r"""
      (?P<comment>//(?:[^\\\n]|\\\r?\n|\\)*|/\*.*?\*/)
    | (?P<string>(?:u8|u|U|L)?R"(?P<delim>[^()\\\s"]{0,16})\(.*?\)(?P=delim)"
                |(?:u8|u|U|L)?"(?:\\.|[^"\\\n])*")
    | (?P<char>(?<![\w'])(?:u8|u|U|L)?'(?:\\.|[^'\\\n])+')
    """,
    re.S | re.X,
)
_NAMESPACE_HEAD = re.compile(
    r"\s*(?:inline\s+)?(?:\[\[[^\]]*\]\]\s*)*((?:[A-Za-z_]\w*\s*::\s*)*[A-Za-z_]\w*)?\s*([{=])"
)
_TOKEN = re.compile(r"[{}]|\bnamespace\b")
_DIRECTIVE = re.compile(r"^[ \t]*#(?:[^\n]|\\\n)*", re.M)


def blank(text: str) -> str:
    return re.sub(r"[^\r\n]", " ", text)


def mask(text: str) -> str:
    """`text` with the inside of every comment and literal blanked, offsets and newlines kept."""
    out: list[str] = []
    last = 0
    for m in _LEXEME.finditer(text):
        out.append(text[last : m.start()])
        if m.lastgroup == "comment":
            out.append(blank(m.group()))
        else:
            q = min(
                i
                for i in (text.find('"', m.start(), m.end()), text.find("'", m.start(), m.end()))
                if i >= 0
            )
            out.append(
                text[m.start() : q + 1] + blank(text[q + 1 : m.end() - 1]) + text[m.end() - 1]
            )
        last = m.end()
    out.append(text[last:])
    return "".join(out)


@dataclass
class Block:
    path: tuple[str, ...]  # the absolute namespace path of the block
    named: bool  # opened by `namespace name {`
    head: int  # offset of the `namespace` keyword (-1 for another brace)
    open: int
    close: int


def blocks(masked: str) -> list[Block]:
    """The brace blocks of masked text in the order they open, each with its namespace path."""
    pending: dict[int, tuple[tuple[str, ...], int]] = {}
    out: list[Block] = []
    stack: list[Block] = []
    for t in _TOKEN.finditer(masked):
        if t.group() == "namespace":
            h = _NAMESPACE_HEAD.match(masked, t.end())
            if h and h.group(2) == "{" and h.group(1):
                names = tuple(re.sub(r"\s+", "", h.group(1)).split("::"))
                pending[h.end() - 1] = (names, t.start())
        elif t.group() == "{":
            names, head = pending.get(t.start(), ((), -1))
            parent = stack[-1].path if stack else ()
            blk = Block((*parent, *names), t.start() in pending, head, t.start(), len(masked))
            out.append(blk)
            stack.append(blk)
        elif stack:
            stack.pop().close = t.start()
    return out


def declares_in_the_root(masked: str, bs: list[Block]) -> bool:
    """Does a `namespace iclforge { }` block hold something besides namespaces and directives?"""
    for root in bs:
        if not (root.named and root.path == (ROOT_NS,)):
            continue
        body = list(masked[root.open + 1 : root.close])
        for inner in bs:  # every namespace nested in it, blanked whole
            if inner.named and root.open < inner.open < root.close:
                for i in range(inner.head, inner.close + 1):
                    body[i - root.open - 1] = " "
        if _DIRECTIVE.sub("", "".join(body)).strip():
            return True
    return False


# --- the table and the tree -----------------------------------------------------------------------


@dataclass(frozen=True)
class Table:
    libraries: dict[str, list[str]]  # library -> the names under `iclforge` its headers may open
    debts: dict[str, str]  # header -> why it is allowed to declare into the root

    @classmethod
    def load(cls, path: Path) -> Table:
        data = json.loads(path.read_text(encoding="utf-8"))
        return cls(
            {k: list(v) for k, v in data["libraries"].items()},
            {row["file"]: row["reason"] for row in data.get("debts", [])},
        )


def public_headers(files: list[str]) -> dict[str, list[str]]:
    """library -> its public headers: src/<library>/include/iclforge/<library>/..."""
    out: dict[str, list[str]] = defaultdict(list)
    for f in files:
        parts = f.split("/")
        if (
            len(parts) > 5
            and parts[0] == "src"
            and parts[2] == "include"
            and parts[3] == ROOT_NS
            and parts[4] == parts[1]
            and f.endswith(HEADER_SUFFIXES)
        ):
            out[parts[1]].append(f)
    return out


def opened(masked: str) -> tuple[list[str], list[Block]]:
    """The first names under `iclforge` that the file's namespace blocks open, and the blocks."""
    bs = blocks(masked)
    firsts = sorted({b.path[1] for b in bs if b.named and len(b.path) > 1 and b.path[0] == ROOT_NS})
    return firsts, bs


@dataclass
class Report:
    problems: list[str]
    known: list[str]
    headers: int
    libraries: int


def check(root: Path, table: Table, files: list[str]) -> Report:
    problems: list[str] = []
    known: list[str] = []
    by_library = public_headers(files)
    for lib in sorted(by_library):
        if lib not in table.libraries:
            problems.append(f"{lib}: public headers, and no row of the table for the library")
    for lib in sorted(table.libraries):
        if lib not in by_library:
            problems.append(f"{lib}: a row of the table for a library with no public headers")
    seen_debts: set[str] = set()
    count = 0
    for lib, headers in sorted(by_library.items()):
        allowed = set(table.libraries.get(lib, []))
        for f in sorted(headers):
            count += 1
            masked = mask((root / f).read_bytes().decode("utf-8", "replace"))
            firsts, bs = opened(masked)
            wrong = [x for x in firsts if x not in allowed]
            in_root = declares_in_the_root(masked, bs)
            broken = [f"{f}: opens iclforge::{x}, no namespace of library {lib}" for x in wrong]
            if in_root:
                broken.append(f"{f}: declares into the root namespace iclforge itself")
            if f in table.debts:
                seen_debts.add(f)
                if broken:
                    known.append(f"{f}: {table.debts[f]}")
                else:
                    problems.append(
                        f"{f}: listed as a debt and no longer breaks the rule: delete its row"
                    )
            else:
                problems.extend(broken)
    for f in sorted(set(table.debts) - seen_debts):
        problems.append(f"{f}: listed as a debt and is not a public header of the tree")
    return Report(problems, known, count, len(by_library))


def tracked(root: Path) -> list[str]:
    """The files under src/: what git tracks, or what is there when the tree is no repository."""
    try:
        out = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z", "--", "src"],
            capture_output=True,
            check=True,
        ).stdout.decode("utf-8", "surrogateescape")
        return [f for f in out.split("\0") if f]
    except (subprocess.CalledProcessError, FileNotFoundError):
        walked = (p.relative_to(root).as_posix() for p in (root / "src").rglob("*") if p.is_file())
        return sorted(walked)


def census(root: Path, files: list[str]) -> str:
    lines = []
    for lib, headers in sorted(public_headers(files).items()):
        names: dict[str, int] = defaultdict(int)
        root_files = []
        for f in headers:
            masked = mask((root / f).read_bytes().decode("utf-8", "replace"))
            firsts, bs = opened(masked)
            for x in firsts:
                names[x] += 1
            if declares_in_the_root(masked, bs):
                root_files.append(f)
        shown = ", ".join(f"{x} ({n})" for x, n in sorted(names.items())) or "-"
        lines.append(f"{lib}: {len(headers)} headers open {shown}")
        lines += [f"    declares into the root: {f}" for f in root_files]
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--root", default=str(HERE.parents[1]), help="the repository")
    ap.add_argument("--table", default=str(DEFAULT_TABLE))
    ap.add_argument("--census", action="store_true", help="print what each library's headers open")
    a = ap.parse_args(argv)
    root = Path(a.root)
    files = tracked(root)
    if a.census:
        sys.stdout.write(census(root, files))
        return 0
    report = check(root, Table.load(Path(a.table)), files)
    for line in report.known:
        print(f"known debt: {line}")
    for line in report.problems:
        print(f"FAIL: {line}")
    print(
        f"{report.headers} public headers in {report.libraries} libraries: "
        f"{len(report.problems)} failures, {len(report.known)} known debts"
    )
    return 1 if report.problems else 0


if __name__ == "__main__":
    sys.exit(main())
