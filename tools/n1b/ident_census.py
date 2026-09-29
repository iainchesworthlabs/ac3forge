"""Census of every identifier-like token that contains 'ac3' (any case), by class and place.

'eac3' tokens are the codec E-AC-3 and are counted apart: they are not the family brand.

Usage: ident_census.py [--root R] [--json out.json] [--md out.md] [--top N]
"""

from __future__ import annotations

import json
import re
from collections import Counter, defaultdict

from n1b_lib import Repo, base_parser, emit, md_table

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
    ".jpeg",
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
    ".ts_bin",
    ".oam",
    ".jocdata",
    ".tsv",
}
TOKEN = re.compile(r"[A-Za-z0-9_]*[Aa][Cc]3[A-Za-z0-9_]*")


def place_of(path: str) -> str:
    p = path.split("/")
    if p[0] == "src" and len(p) > 2:
        return "src/" + p[1]
    if p[0] == "apps" and len(p) > 2:
        return "apps/" + p[1]
    if p[0] == "docs" and len(p) > 2:
        return "docs/" + p[1] if len(p) > 3 else "docs"
    if p[0] in (
        "tests",
        "fuzz",
        "examples",
        "python",
        "rust",
        "js",
        "esp-idf",
        "esphome",
        "packaging",
        "tools",
        "cmake",
        ".github",
        "planning",
        "docs-snippets",
    ):
        return p[0]
    return "(root)"


def classify(tok: str) -> str:
    low = tok.lower()
    if "eac3" in low.replace("e_ac3", "eac3"):
        return "codec: E-AC-3 (eac3*)"
    if tok.startswith("AC3FORGE_") or tok == "AC3FORGE":
        return "brand: AC3FORGE_* (options, macros, env, Kconfig)"
    if tok.startswith("CONFIG_AC3FORGE"):
        return "brand: AC3FORGE_* (options, macros, env, Kconfig)"
    if low.startswith("ac3forge_c"):
        return "brand: ac3forge_c* (C API)"
    if low.startswith("ac3forge") or low.startswith("_ac3forge"):
        return "brand: ac3forge* (package, module, crate, identifiers)"
    if (
        tok.startswith("AC3HEARTH_")
        or tok.startswith("AC3CRUCIBLE_")
        or tok.startswith("AC3GUI_")
        or tok.startswith("AC3CLI_")
        or tok.startswith("AC3TESTS")
    ):
        return "program: AC3<PROGRAM>_* variables"
    for prog in ("ac3cli", "ac3gui", "ac3hearth", "ac3crucible", "ac3tests"):
        if low.startswith(prog):
            return "program: " + prog + "*"
    if tok.startswith("AC3_") or tok == "AC3":
        return "codec or brand macro: AC3_ / AC3"
    for lib in (
        "ac3audio",
        "ac3sendspin",
        "ac3iab",
        "ac3adm",
        "ac3signing",
        "ac3probe",
        "ac3shield",
        "ac3fuzz",
        "ac3nullsink",
        "ac3test",
    ):
        if low.startswith(lib):
            return "library/target: " + lib + "*"
    if low == "ac3":
        return "ac3 (namespace, path, codec word)"
    if low.startswith("ac3_") or low.endswith("_ac3") or "_ac3_" in low:
        return "codec: ac3_* / *_ac3 identifiers"
    if tok.startswith("Ac3") or "Ac3" in tok:
        return "codec: Ac3* identifiers"
    if low.startswith("ac3"):
        return "ac3<word> (other)"
    return "contains ac3 (other)"


def main() -> None:
    ap = base_parser(__doc__)
    ap.add_argument("--json", default=None)
    ap.add_argument("--md", default=None)
    ap.add_argument("--top", type=int, default=12)
    a = ap.parse_args()
    repo = Repo(a.root)
    tok_files = defaultdict(set)
    tok_count = Counter()
    class_files = defaultdict(set)
    class_count = Counter()
    class_place = defaultdict(Counter)
    tok_place = defaultdict(Counter)
    skipped = 0
    for f in repo.files:
        if repo.ext(f) in BINARY_EXT:
            skipped += 1
            continue
        t = repo.read(f)
        if "\0" in t[:4096]:
            skipped += 1
            continue
        for m in TOKEN.finditer(t):
            tok = m.group(0)
            c = classify(tok)
            tok_files[tok].add(f)
            tok_count[tok] += 1
            class_files[c].add(f)
            class_count[c] += 1
            class_place[c][place_of(f)] += 1
            tok_place[tok][place_of(f)] += 1
    classes = []
    for c, n in class_count.most_common():
        toks = [(t, tok_count[t], len(tok_files[t])) for t in tok_count if classify(t) == c]
        toks.sort(key=lambda x: -x[1])
        classes.append(
            {
                "class": c,
                "occurrences": n,
                "files": len(class_files[c]),
                "places": dict(class_place[c].most_common(8)),
                "distinct_tokens": len(toks),
                "top_tokens": toks[: a.top],
            }
        )
    if a.json:
        emit(
            json.dumps(
                {
                    "skipped_binary": skipped,
                    "classes": classes,
                    "tokens": {
                        t: {
                            "n": tok_count[t],
                            "files": len(tok_files[t]),
                            "places": dict(tok_place[t]),
                        }
                        for t in tok_count
                    },
                },
                indent=1,
            ),
            a.json,
        )
    lines = [f"skipped {skipped} binary files\n\n"]
    lines.append(
        md_table(
            ["class", "occurrences", "files", "distinct tokens"],
            [[c["class"], c["occurrences"], c["files"], c["distinct_tokens"]] for c in classes],
            ["l", "r", "r", "r"],
        )
    )
    for c in classes:
        lines.append(f"\n### {c['class']}\n")
        lines.append("places: " + ", ".join(f"{k} {v}" for k, v in c["places"].items()) + "\n")
        lines.append("top: " + ", ".join(f"`{t}` {n}/{f}" for t, n, f in c["top_tokens"]) + "\n")
    text = "".join(lines)
    if a.md:
        emit(text, a.md)
    else:
        emit(text, a.out)


if __name__ == "__main__":
    main()
