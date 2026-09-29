"""Symbol table of the `ac3` namespace family: which file declares each name, by namespace.

A small brace-depth scanner (comments and string/char literals stripped) that records, for each
`namespace ac3...` block, the names declared directly in it: struct, class, enum (class), union,
using, namespace-scope constexpr/inline variables and functions. It is good enough to decide which
new library namespace a symbol moves to when its declaring file moves; anything it misses shows up
as a compile error in the prototype, which is the point of building one.

Usage: symtab.py [--root R] [--json out.json] [--summary]
"""

from __future__ import annotations

import json
import re
from collections import Counter, defaultdict

from n1b_lib import HEADER_EXT, Repo, base_parser, emit

NS_OPEN = re.compile(r"\bnamespace\s+((?:[A-Za-z_]\w*)(?:\s*::\s*[A-Za-z_]\w*)*)\s*\{")
DECL_KW = re.compile(
    r"\b(?:struct|class|union)\s+(?:\[\[[^\]]*\]\]\s*)?(?:[A-Z_][A-Z0-9_]*(?:\([^)]*\))?\s+)*([A-Za-z_]\w*)\s*(?:final\s*)?(?::[^;{]*)?[{;]"
)
ENUM = re.compile(r"\benum\s+(?:class\s+|struct\s+)?([A-Za-z_]\w*)\s*(?::\s*[\w:]+\s*)?[{;]")
USING = re.compile(r"\busing\s+([A-Za-z_]\w*)\s*=")
VAR = re.compile(
    r"\b(?:inline\s+)?(?:static\s+)?constexpr\s+[^;={(]*?\b([A-Za-z_]\w*)\s*(?:=|\{|\[)"
)
FUNC = re.compile(
    r"(?:^|\n)[ \t]*(?:template\s*<[^>]*>\s*)?(?:\[\[[^\]]*\]\]\s*)*"
    r"(?:inline\s+|constexpr\s+|static\s+|[A-Z_]+_EXPORT\s+)*[\w:<>,\s\*&]+?\b([A-Za-z_]\w*)\s*"
    r"\([^;{]*\)\s*(?:const\s*)?(?:noexcept\s*)?(?:->\s*[\w:<>,\s\*&]+)?\s*[;{]"
)


def strip(text: str) -> str:
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            out.append('""')
            i = j + 1
        elif c == "'":
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == "\\" else 1
            out.append("''")
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def scan_flat(text: str):
    """Second method: statement-level scan at namespace scope using brace depth only."""
    text = strip(text)
    out = []
    ns: list[str] = []
    brace_kind: list[
        list[str]
    ] = []  # for each open brace: namespace comps added, or None for non-namespace brace
    i = 0
    stmt_start = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "{":
            head = text[stmt_start:i]
            m = re.search(r"\bnamespace\s+((?:[A-Za-z_]\w*)(?:\s*::\s*[A-Za-z_]\w*)*)\s*$", head)
            in_ns_scope = all(b is not None for b in brace_kind)
            if m and in_ns_scope:
                comps = [x.strip() for x in m.group(1).split("::")]
                brace_kind.append(comps)
                ns.extend(comps)
            else:
                if in_ns_scope and ns:
                    # declaration header
                    for rx, kind in ((ENUM, "enum"), (DECL_KW, "type")):
                        mm = rx.search(head + "{")
                        if mm:
                            out.append((".".join(ns), mm.group(1), kind))
                            break
                    else:
                        mm = FUNC.search("\n" + head + "{")
                        if mm and mm.group(1) not in (
                            "if",
                            "for",
                            "while",
                            "switch",
                            "return",
                            "sizeof",
                            "catch",
                        ):
                            out.append((".".join(ns), mm.group(1), "func"))
                brace_kind.append(None)
            stmt_start = i + 1
        elif c == "}":
            if brace_kind:
                comps = brace_kind.pop()
                if comps:
                    for _ in comps:
                        ns.pop()
            stmt_start = i + 1
        elif c == ";":
            in_ns_scope = all(b is not None for b in brace_kind)
            if in_ns_scope and ns:
                stmt = text[stmt_start : i + 1]
                for rx, kind in ((USING, "using"), (VAR, "var")):
                    mm = rx.search(stmt)
                    if mm:
                        out.append((".".join(ns), mm.group(1), kind))
                        break
                else:
                    mm = re.match(
                        r"\s*(?:struct|class|enum(?:\s+class)?)\s+([A-Za-z_]\w*)\s*;", stmt
                    )
                    if mm:
                        out.append((".".join(ns), mm.group(1), "fwd"))
                    else:
                        mm = FUNC.search("\n" + stmt)
                        if mm and mm.group(1) not in (
                            "if",
                            "for",
                            "while",
                            "switch",
                            "return",
                            "sizeof",
                        ):
                            out.append((".".join(ns), mm.group(1), "func"))
            stmt_start = i + 1
        i += 1
    return out


def main() -> None:
    ap = base_parser(__doc__)
    ap.add_argument("--json", default=None)
    ap.add_argument("--summary", action="store_true")
    a = ap.parse_args()
    repo = Repo(a.root)
    table = defaultdict(lambda: defaultdict(set))  # ns -> name -> files
    for f in repo.files:
        if repo.ext(f) not in HEADER_EXT:
            continue
        t = repo.read(f)
        if "namespace ac3" not in t and "namespace ac4" not in t:
            continue
        for ns, name, _kind in scan_flat(t):
            top = ns.split(".")[0]
            if top in ("ac3",):
                table[ns][name].add(f)
    data = {ns: {n: sorted(fs) for n, fs in names.items()} for ns, names in table.items()}
    if a.json:
        emit(json.dumps(data, indent=1), a.json)
    if a.summary:
        cnt = Counter({ns: len(names) for ns, names in table.items()})
        lines = [f"{ns}: {c} names\n" for ns, c in sorted(cnt.items())]
        emit("".join(lines), a.out)


if __name__ == "__main__":
    main()
