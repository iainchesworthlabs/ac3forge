"""How many files N1A (program names) and N1B (library names, layout) touch, and where they overlap.

A file is an N1A file when it names a program (ac3cli, ac3gui, ac3hearth, ac3crucible, ac3tests, the
internal programs). It is an N1B-name file when it carries a library/family brand token (ac3forge*,
AC3FORGE_*, ac3:: qualifiers and namespace declarations, ac3/ include roots, ac3iab, ac3adm,
ac3audio, ac3sendspin, ac3signing, libac3forge...). Paths are counted separately (path_keyed.py).

Usage: overlap.py [--root R] [--json out.json] [--md out.md]
"""

from __future__ import annotations

import json
import re
from collections import Counter, defaultdict

from n1b_lib import CPP_EXT, Repo, base_parser, emit, md_table
from path_keyed import category

BINARY_EXT = {
    ".png",
    ".ttf",
    ".bin",
    ".wav",
    ".ec3",
    ".ac3",
    ".ac4",
    ".wasm",
    ".jpg",
    ".ico",
    ".icns",
    ".gif",
    ".woff",
    ".woff2",
    ".pdf",
    ".zip",
    ".gz",
    ".map",
    ".qm",
    ".svg",
    ".mkv",
    ".mp4",
    ".m4a",
    ".mka",
    ".tsv",
}

N1A = re.compile(
    r"(?i)\bac3(cli|gui|hearth|crucible|tests|probe|shield|nullsink|fuzz|bench|membench|perf|kernelbench|space|assay|desk|windemo)\w*|\bAC3(CLI|GUI|HEARTH|CRUCIBLE|TESTS)_\w+"
)
N1B = re.compile(
    r"(?i)ac3forge|\bac3::|\bnamespace\s+ac3\b|[<\"]ac3/|\bac3(iab|adm|audio|sendspin|signing)\w*|\bAC3(IAB|ADM|SIGNING)\w*|"
    r"\bac3::forge|libac3forge|\bac4::|[<\"]ac4(dec|enc)?/|\bmp4::|\bmpegts::|\bmatroska::|\biamf::"
)


def main() -> None:
    ap = base_parser(__doc__)
    ap.add_argument("--json", default=None)
    ap.add_argument("--md", default=None)
    a = ap.parse_args()
    repo = Repo(a.root)
    tab = defaultdict(Counter)
    total = Counter()
    for f in repo.files:
        if repo.ext(f) in BINARY_EXT:
            continue
        t = repo.read(f)
        if "\0" in t[:2048]:
            continue
        cat = category(f)
        if repo.ext(f) in CPP_EXT or f.endswith((".hpp.in", ".h.in")):
            cat = "C/C++ source and headers"
        n1a = bool(N1A.search(t))
        n1b = bool(N1B.search(t))
        if n1a or n1b:
            tab[cat]["either"] += 1
        if n1a:
            tab[cat]["N1A"] += 1
        if n1b:
            tab[cat]["N1B"] += 1
        if n1a and n1b:
            tab[cat]["both"] += 1
        tab[cat]["files"] += 1
    rows = []
    for cat, c in sorted(tab.items(), key=lambda kv: -kv[1]["either"]):
        rows.append([cat, c["files"], c["N1A"], c["N1B"], c["both"], c["either"]])
        for k in ("files", "N1A", "N1B", "both", "either"):
            total[k] += c[k]
    rows.append(
        ["**total**", total["files"], total["N1A"], total["N1B"], total["both"], total["either"]]
    )
    text = md_table(
        ["category", "files scanned", "N1A tokens", "N1B tokens", "both", "either"],
        rows,
        ["l", "r", "r", "r", "r", "r"],
    )
    if a.json:
        emit(json.dumps({r[0]: r[1:] for r in rows}, indent=1), a.json)
    if a.md:
        emit(text, a.md)
    else:
        emit(text, a.out)


if __name__ == "__main__":
    main()
