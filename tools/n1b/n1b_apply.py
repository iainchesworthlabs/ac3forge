"""Prototype executor for the recommended layout (L2): moves, include spellings, export macros.

    n1b_apply.py --root <worktree> --phase plan|moves|includes|all [--scope src] [--quiet]

Phases (each idempotent on an already-processed tree; run them in this order):
  plan      print what would move and which include spellings would change; touches nothing
  moves     `git mv` every file layoutdef.l2_new() relocates (scope src: only under src/)
  includes  rewrite #include spellings from the old tree's resolution, and the per-library export
  macros

The include rewrite resolves every include of the OLD tree the way the compiler would (quote-
relative, then the spelling index) and rewrites it only when the target moved and its spelling
changed, so an include that stays valid keeps its text. It must run on the tree BEFORE `moves`;
`all` does both. Namespaces are a separate script (n1b_names.py).
"""

from __future__ import annotations

import json
import posixpath
import re
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

import layoutdef
from include_graph import build_index, resolve
from n1b_lib import CPP_EXT, INCLUDE_RE, Repo, base_parser

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
    ".flac",
}

# old export-header spelling -> new spelling (a forge file's own library decides `ac3/export.hpp`)
EXPORT_SIMPLE = {
    "ac3adm/export.hpp": "iclforge/adm/export.hpp",
    "ac3iab/export.hpp": "iclforge/iab/export.hpp",
    "ac3/admbridge/export.hpp": "iclforge/admbridge/export.hpp",
    "ac3/signing/export.hpp": "iclforge/signing/export.hpp",
    "ac3forge_c/export.h": "iclforge_c/export.h",
    "ac4/export.hpp": "iclforge/ac4/export.hpp",
    "ac4dec/export.hpp": "iclforge/ac4dec/export.hpp",
    "ac4enc/export.hpp": "iclforge/ac4enc/export.hpp",
    "iamf/export.hpp": "iclforge/iamf/export.hpp",
    "mp4/export.hpp": "iclforge/mp4/export.hpp",
    "mpegts/export.hpp": "iclforge/mpegts/export.hpp",
    "matroska/export.hpp": "iclforge/matroska/export.hpp",
}
SPLIT_LIBS = ("ac3", "base", "dsp", "render", "objects", "iec61937")
EXPORT_MACROS = (
    "AC3FORGE_EXPORT",
    "AC3FORGE_TEMPLATE_CLASS",
    "AC3FORGE_TEMPLATE_IMPORT",
    "AC3FORGE_TEMPLATE_INSTANTIATE",
)


def new_lib_of(path: str) -> str | None:
    p = path.split("/")
    if p[0] in ("src", "libs") and len(p) > 2:
        return p[1]
    return None


def is_text(repo: Repo, f: str) -> bool:
    if repo.ext(f) in BINARY_EXT:
        return False
    binary_tree = f.startswith(("fuzz/seeds/", "fuzz/regressions/", "tests/golden/"))
    return not (binary_tree and repo.ext(f) not in CPP_EXT)


def compute_moves(repo: Repo, scope: str) -> dict[str, str]:
    moves = {}
    for f in repo.files:
        n = layoutdef.l2_new(f, True)
        if n and n != f and (scope != "src" or f.startswith("src/")):
            moves[f] = n
    return moves


def plan_include_edits(repo: Repo, moves: dict[str, str], quiet: bool):
    """Per old file: {old spelling: new spelling}, plus report lines."""
    index, _ = build_index(repo)
    edits: dict[str, dict[str, str]] = defaultdict(dict)
    stats = Counter()
    problems = []
    global_map: dict[str, str] = {}
    for old, new in moves.items():
        so, sn = layoutdef.spelling_of(old), layoutdef.spelling_of(new)
        if so and sn and so != sn:
            global_map[so] = sn
    for f in repo.files:
        if repo.ext(f) not in CPP_EXT and not f.endswith((".hpp.in", ".h.in")):
            continue
        text = repo.read(f)
        f_new = moves.get(f, f)
        from_lib = f.split("/")[1] if f.startswith("src/") and f.count("/") > 2 else None
        for m in INCLUDE_RE.finditer(text):
            sp = m.group(2).strip()
            if sp in edits[f]:
                continue
            # generated headers
            if sp == "ac3/export.hpp":
                lib = new_lib_of(f_new) if new_lib_of(f_new) in SPLIT_LIBS else "ac3"
                edits[f][sp] = f"iclforge/{lib}/export.hpp"
                stats["export"] += 1
                continue
            if sp in EXPORT_SIMPLE:
                edits[f][sp] = EXPORT_SIMPLE[sp]
                stats["export"] += 1
                continue
            own_lib = f.split("/")[1] if from_lib else ""
            _owner, target, _is_pub = resolve(repo, index, f, sp, own_lib)
            if target is None:
                if sp in global_map:  # a generated header (version.hpp, version.h)
                    edits[f][sp] = global_map[sp]
                    stats["generated"] += 1
                continue
            t_new = moves.get(target, target)
            sn = layoutdef.spelling_of(t_new)
            so = layoutdef.spelling_of(target)
            # quote-relative include that stays valid because both files moved together
            cand = posixpath.normpath(posixpath.join(posixpath.dirname(f), sp))
            moved_together = (
                posixpath.normpath(posixpath.join(posixpath.dirname(f_new), sp)) == t_new
            )
            if cand == target and moved_together:
                continue
            if sn and sn != sp:
                edits[f][sp] = sn
                stats["public" if so else "private->detail"] += 1
            elif sn is None and t_new != target:
                # target moved and is private: valid only if the includer stays in the same library
                if new_lib_of(f_new) != new_lib_of(t_new):
                    problems.append((f, sp, target, t_new))
    return edits, stats, problems, global_map


def do_moves(root: Path, moves: dict[str, str]) -> None:
    n = 0
    for old, new in sorted(moves.items()):
        if not (root / old).exists() and (root / new).exists():
            continue
        (root / new).parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "-C", str(root), "mv", old, new], check=True, capture_output=True)
        n += 1
    print(f"moved {n} files")


def apply_include_edits(root: Path, repo: Repo, moves, edits, global_map) -> int:
    changed = 0
    for old, m in edits.items():
        if not m:
            continue
        path = root / moves.get(old, old)
        text = path.read_bytes().decode("utf-8")

        def sub(mo, rewrites=m):
            sp = mo.group(2).strip()
            new = rewrites.get(sp)
            if not new:
                return mo.group(0)
            return mo.group(0).replace(mo.group(2), new)

        out = INCLUDE_RE.sub(sub, text)
        # per-library export macros for the files of the split
        if old.startswith("src/forge/"):
            lib = new_lib_of(moves.get(old, old)) or "ac3"
            for mac in EXPORT_MACROS:
                out = re.sub(
                    rf"\b{mac}\b", mac.replace("AC3FORGE_", f"ICLFORGE_{lib.upper()}_"), out
                )
        if out != text:
            path.write_bytes(out.encode("utf-8"))
            changed += 1
    return changed


def rewrite_docs_and_scripts(root: Path, repo: Repo, moves, global_map) -> int:
    """Non-C++ text: only `#include` lines, by the global spelling map."""
    changed = 0
    for f in repo.files:
        if repo.ext(f) in CPP_EXT or not is_text(repo, f):
            continue
        p = root / moves.get(f, f)
        try:
            text = p.read_bytes().decode("utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        if "#include" not in text and "#  include" not in text:
            continue

        def sub(mo):
            sp = mo.group(2).strip()
            return (
                mo.group(0).replace(mo.group(2), global_map[sp])
                if sp in global_map
                else mo.group(0)
            )

        out = INCLUDE_RE.sub(sub, text)
        if out != text:
            p.write_bytes(out.encode("utf-8"))
            changed += 1
    return changed


def main() -> None:
    ap = base_parser(__doc__)
    ap.add_argument("--phase", choices=["plan", "moves", "includes", "all"], default="plan")
    ap.add_argument("--scope", choices=["src", "all"], default="src")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--json", default=None, help="write the plan to this file")
    a = ap.parse_args()
    root = Path(a.root)
    repo = Repo(a.root)
    moves = compute_moves(repo, a.scope)
    edits, stats, problems, global_map = plan_include_edits(repo, moves, a.quiet)
    print(
        f"{len(moves)} moves; {sum(len(v) for v in edits.values())} include rewrites in "
        f"{sum(1 for v in edits.values() if v)} files; kinds {dict(stats)}; "
        f"{len(problems)} includes reach a moved private header from another library"
    )
    for f, sp, t, tn in problems[:40]:
        print("  PROBLEM", f, "includes", repr(sp), "->", t, "now", tn)
    if a.json:
        Path(a.json).write_text(
            json.dumps(
                {
                    "moves": moves,
                    "edits": {k: v for k, v in edits.items() if v},
                    "problems": problems,
                },
                indent=1,
            ),
            encoding="utf-8",
        )
    if a.phase == "plan":
        return
    if (
        a.phase in ("includes", "all")
        and any((root / m).exists() for m in moves.values())
        and a.phase == "includes"
    ):
        sys.exit("includes must run before moves (it resolves the old tree); use --phase all")
    if a.phase in ("moves", "all"):
        do_moves(root, moves)
    if a.phase in ("includes", "all"):
        n = apply_include_edits(root, repo, moves, edits, global_map)
        d = rewrite_docs_and_scripts(root, repo, moves, global_map)
        print(f"rewrote includes in {n} C/C++ files and {d} other text files")


if __name__ == "__main__":
    main()
