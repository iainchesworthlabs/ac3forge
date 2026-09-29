"""Dry run of the three candidate layouts: what moves, what is edited in place, how deep it goes.

Usage: layout_dryrun.py [--root R] [--graph include_graph.json] [--pathkeys path_keyed.json]
                        [--overlap-tokens] [--json out.json] [--md out.md] [--moves-out DIR]
It writes nothing into the repository.
"""

from __future__ import annotations

import json
import os
from collections import Counter
from pathlib import Path

from layoutdef import apply, library_of
from n1b_lib import CPP_EXT, Repo, base_parser, emit, md_table
from overlap import BINARY_EXT, N1B

L1_KEYS = [
    "src/forge",
    "src/ac3adm",
    "src/ac3iab",
    "include/ac3",
    "esp-idf/ac3forge",
    "rust/ac3forge",
    "rust/ac3forge-sys",
]
L2_TEST_KEYS = [
    f"tests/{d}"
    for d in (
        "core",
        "decoder",
        "encoder",
        "io",
        "meta",
        "oba",
        "quality",
        "verify",
        "emdf",
        "analysis",
        "dsp",
        "render",
        "spatial",
        "iec61937",
        "ac3iab",
        "adm",
        "admbridge",
        "audio",
        "backend",
        "capi",
        "iamf",
        "sendspin",
        "signing",
        "ac4",
        "ac4core",
        "ac4dec",
        "ac4enc",
        "containers",
    )
]
L2_KEYS = L1_KEYS + L2_TEST_KEYS + ["src/ac4core", "src/forge/include", "src/forge/src"]
L3_KEYS = (
    L2_KEYS
    + [
        f"src/{d}"
        for d in (
            "audio",
            "sendspin",
            "signing",
            "admbridge",
            "arithmetic",
            "capi",
            "iamf",
            "matroska",
            "mp4",
            "mpegts",
            "ac4",
            "ac4dec",
            "ac4enc",
        )
    ]
    + ["fuzz/seeds", "fuzz/regressions", "tests/performance"]
)


def depth(p: str) -> int:
    return p.count("/") + 1


def primary_libs(repo: Repo, graph: dict) -> dict[str, str]:
    """For each test .cpp, the new-library it includes most (L2 assignment)."""
    out = {}
    for f, incs in graph["per_file_includes"].items():
        if not f.startswith("tests/") or not f.endswith(".cpp"):
            continue
        c = Counter()
        for _sp, t, _o in incs:
            lib = library_of(t) if t.startswith("src/") else None
            if lib:
                c[lib] += 1
        if c:
            out[f] = c.most_common(1)[0][0]
    return out


def main() -> None:
    ap = base_parser(__doc__)
    ap.add_argument("--graph", required=True, help="include_graph.json from include_graph.py")
    ap.add_argument("--pathkeys", required=True, help="path_keyed.json from path_keyed.py")
    ap.add_argument("--json", default=None)
    ap.add_argument("--md", default=None)
    ap.add_argument("--moves-out", default=None)
    a = ap.parse_args()
    repo = Repo(a.root)
    graph = json.loads(Path(a.graph).read_text(encoding="utf-8"))
    pk = json.loads(Path(a.pathkeys).read_text(encoding="utf-8"))

    # per-file library of test files, for L2/L3 placement of mixed directories
    prim = primary_libs(repo, graph)
    per_file = {}
    for f, lib in prim.items():
        parts = f.split("/")
        if (
            len(parts) >= 3
            and parts[1] in ("containers", "core", "io", "oba", "emdf")
            and lib
            in (
                "base",
                "dsp",
                "arithmetic",
                "render",
                "objects",
                "iec61937",
                "mp4",
                "mpegts",
                "matroska",
            )
        ):
            per_file[f] = lib
    # containers tests go with their library
    for f in repo.files:
        if f.startswith("tests/containers/") and f in prim:
            per_file[f] = prim[f]

    # N1B token files (text): the set every layout edits in place
    token_files = set()
    for f in repo.files:
        if repo.ext(f) in BINARY_EXT:
            continue
        t = repo.read(f)
        if "\0" in t[:2048]:
            continue
        if N1B.search(t):
            token_files.add(f)

    layouts = {}
    for name, keys in (("L1", L1_KEYS), ("L2", L2_KEYS), ("L3", L3_KEYS)):
        moves = apply(name, repo, per_file)
        # path-keyed files: files that name a key path
        keyed = set()
        for k in keys:
            if k in pk:
                for fs in pk[k]["by_category"].values():
                    keyed.update(fs)
        # a moved file that also has its content edited needs the second (rewrite) commit
        edited = token_files | keyed
        by_area = Counter()
        for old in moves:
            by_area[old.split("/")[0]] += 1
        new_paths = [moves.get(f, f) for f in repo.files]
        deepest_after = max(new_paths, key=depth)
        src_before = max((f for f in repo.files if f.startswith("src/")), key=depth)
        after_src = [
            moves.get(f, f)
            for f in repo.files
            if (f.startswith("src/") or (f in moves and moves[f].startswith(("src/", "libs/"))))
        ]
        src_after = max(after_src, key=depth)
        cpp_edit = sum(
            1 for f in edited if repo.ext(f) in CPP_EXT or f.endswith((".hpp.in", ".h.in"))
        )
        layouts[name] = {
            "moved": len(moves),
            "by_area": dict(by_area.most_common()),
            "edited_in_place": len(edited - set(moves)),
            "edited_total": len(edited),
            "moved_and_edited": len(edited & set(moves)),
            "cpp_edited": cpp_edit,
            "max_depth_src_before": depth(src_before),
            "max_depth_src_after": depth(src_after),
            "deepest_src_after": src_after,
            "src_before": src_before,
            "deepest_tree_after": deepest_after,
            "keyed_only": len(keyed - token_files),
        }
        if a.moves_out:
            os.makedirs(a.moves_out, exist_ok=True)
            with open(
                os.path.join(a.moves_out, f"moves_{name}.tsv"), "w", encoding="utf-8", newline="\n"
            ) as fh:
                for o, n in sorted(moves.items()):
                    fh.write(f"{o}\t{n}\n")
    data = {
        "layouts": layouts,
        "token_files": len(token_files),
        "text_files": sum(1 for f in repo.files if repo.ext(f) not in BINARY_EXT),
    }
    if a.json:
        emit(json.dumps(data, indent=1), a.json)
    rows = []
    for k, label in (
        ("moved", "files moved (git mv)"),
        ("edited_in_place", "other files edited in place"),
        ("moved_and_edited", "moved files that also change content"),
        ("cpp_edited", "C/C++ files with rewritten includes or names"),
        ("keyed_only", "files edited only because they name a moved path"),
    ):
        rows.append([label] + [layouts[n][k] for n in ("L1", "L2", "L3")])
    rows.append(
        ["max path depth in src/ (libs/) before"]
        + [layouts[n]["max_depth_src_before"] for n in ("L1", "L2", "L3")]
    )
    rows.append(
        ["max path depth in src/ (libs/) after"]
        + [layouts[n]["max_depth_src_after"] for n in ("L1", "L2", "L3")]
    )
    text = md_table(["", "L1 regroup", "L2 peers", "L3 libs"], rows, ["l", "r", "r", "r"])
    text += "\n" + json.dumps(
        {
            n: {
                k: v
                for k, v in layouts[n].items()
                if k in ("by_area", "deepest_src_after", "src_before")
            }
            for n in layouts
        },
        indent=1,
    )
    if a.md:
        emit(text, a.md)
    else:
        emit(text, a.out)


if __name__ == "__main__":
    main()
