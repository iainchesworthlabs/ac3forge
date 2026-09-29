"""Namespace census: every namespace the C++ declares, where, and how often the tree names it.

Usage: namespaces.py [--root R] [--json out.json] [--md out.md]
"""

from __future__ import annotations

import json
import re
from collections import Counter, defaultdict

from n1b_lib import CPP_EXT, Repo, base_parser, emit, md_table

DECL = re.compile(r"^[ \t]*(?:inline[ \t]+)?namespace[ \t]+([A-Za-z_][\w:]*)[ \t]*\{", re.M)
# nested spelling namespace a { namespace b {
NESTED = re.compile(
    r"namespace[ \t]+([A-Za-z_]\w*)[ \t]*\{[ \t]*(?:\r?\n[ \t]*)?"
    r"namespace[ \t]+([A-Za-z_]\w*)[ \t]*\{"
)


def lib_of(path: str) -> str:
    p = path.split("/")
    if p[0] == "src" and len(p) > 2:
        return "src/" + p[1]
    if p[0] in ("apps",) and len(p) > 2:
        return "apps/" + p[1]
    return p[0]


def main() -> None:
    ap = base_parser(__doc__)
    ap.add_argument("--json", default=None)
    ap.add_argument("--md", default=None)
    a = ap.parse_args()
    repo = Repo(a.root)
    decl_files = defaultdict(lambda: defaultdict(set))  # ns -> place -> files
    for f in repo.files:
        if repo.ext(f) not in CPP_EXT:
            continue
        t = repo.read(f)
        names = set()
        for m in DECL.finditer(t):
            names.add(m.group(1))
        for m in NESTED.finditer(t):
            names.add(m.group(1) + "::" + m.group(2))
        for n in names:
            decl_files[n][lib_of(f)].add(f)
    rows = []
    for n in sorted(decl_files):
        places = decl_files[n]
        total = sum(len(v) for v in places.values())
        rows.append((n, total, {k: len(v) for k, v in sorted(places.items())}))
    top = Counter()
    for n, total, _places in rows:
        top[n.split("::")[0]] += total
    data = {
        "declared": [{"ns": n, "files": t, "places": p} for n, t, p in rows],
        "top": dict(top.most_common()),
    }
    if a.json:
        emit(json.dumps(data, indent=1), a.json)
    lines = ["top-level namespaces by declaring files: " + repr(dict(top.most_common(40))) + "\n\n"]
    interesting = [
        (n, t, p)
        for (n, t, p) in rows
        if n.split("::")[0]
        in (
            "ac3",
            "ac4",
            "ac3iab",
            "ac3adm",
            "ac3forge_c",
            "iamf",
            "mp4",
            "mpegts",
            "matroska",
            "adm",
        )
    ]
    lines.append(
        md_table(
            ["namespace", "files", "places (files)"],
            [[n, t, ", ".join(f"{k} {v}" for k, v in p.items())] for n, t, p in interesting],
            ["l", "r", "l"],
        )
    )
    text = "".join(lines)
    if a.md:
        emit(text, a.md)
    else:
        emit(text, a.out)


if __name__ == "__main__":
    main()
