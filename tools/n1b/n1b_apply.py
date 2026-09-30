"""Executor for the recommended layout (L2): moves, include spellings, export macros.

    n1b_apply.py --root <worktree> --phase plan|all [--scope src,tests] [--json <plan file>]
                 [--quiet]

Phases:
  plan   print what would move and which include spellings would change; touches nothing
  all    `git mv` every file the scope relocates, then rewrite the #include spellings from the old
         tree's resolution and the export macros of each library. The renames are staged and the
         edits wait in the working tree, so that a `git commit` with nothing added is the commit
         of the renames alone. The tree is resolved as it is before the moves, so the two halves
         are one run: a second run on the finished tree finds nothing to do.

The scope names what moves, and is a comma-separated list of
  src       the libraries under src/ (layoutdef.l2_new): the split of src/forge, the flat layout,
            the header root `iclforge/<library>/`, the variant trees
  tests     the tests, mirroring src/ (layoutdef.mirrored_tests_new): tests/core to tests/ac3/core,
            a test of a codec-blind library to that library's directory, tests/containers to the
            container it tests
  packages  the directory renames of the bindings and packages (layoutdef.package_new), which
            belong to stage S4, not S2
`all` is the three. The default is `src,tests`, what stage S2 moves.

The include rewrite resolves every include of the OLD tree the way the compiler would (quote-
relative, then the spelling index) and rewrites it only when the target moved and its spelling
changed, so an include that stays valid keeps its text; a quote-relative include of a header that
no longer sits beside the includer becomes a relative path. `--json` writes the plan for
n1b_cmake.py, which reads the moves: the moves, the edits, the includes left for a person to
resolve, and the spelling map that stage S5 applies to the docs. Namespaces are a separate script
(n1b_names.py).

Text that is not C or C++ has its `#include` lines rewritten by the same spelling map (generators
and templates emit them), except the pages and files that quote the old layout as history: see
KEEP_OLD_SPELLINGS.
"""

from __future__ import annotations

import json
import posixpath
import re
import subprocess
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

SCOPES = ("src", "tests", "packages")
DEFAULT_SCOPE = "src,tests"

# The directories of tests/ that hold the tests of several libraries, and the libraries a test in
# one of them can go with: it goes with the library whose files it includes most. A test of the
# AC-3 codec proper stays in its directory, under tests/ac3/.
MIXED_TEST_DIRS = ("containers", "core", "io", "oba", "emdf")
MIXED_TEST_LIBS = (
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

# Files whose `#include` lines quote the old layout as history or are the scripts that name both
# layouts: the rewrite of non-C++ text leaves them alone. The plans under planning/ describe the
# tree as it was when they were written.
KEEP_OLD_SPELLINGS = (
    "tools/n1b/",
    "tools/checks/layering_debt/",
    "planning/",
    "CHANGELOG.md",
)

# In text that is not C++ an include is named anywhere on a line (a generator's string, a
# comment, a code span in a page), not only at the start of one.
TEXT_INCLUDE_RE = re.compile(r'#[ \t]*include[ \t]*([<"])([^>"\n]+)[>"]')


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


def parse_scope(text: str) -> set[str]:
    """`src,tests` or `all`: the parts of the tree that move."""
    if text == "all":
        return set(SCOPES)
    parts = {p for p in text.split(",") if p}
    unknown = parts - set(SCOPES)
    if unknown or not parts:
        raise SystemExit(
            f"n1b_apply: unknown scope {sorted(unknown) or text!r}; "
            f"the scopes are {', '.join(SCOPES)} and all"
        )
    return parts


def test_libraries(repo: Repo, index) -> dict[str, str]:
    """The library each test source of a mixed directory goes with, where it is not the directory's.

    A test goes with the library of the new layout whose files it includes most, ties broken by
    name; only the directories of MIXED_TEST_DIRS are decided this way, and only a library of
    MIXED_TEST_LIBS moves a test out of them (tests/containers goes wherever its includes point).
    """
    out: dict[str, str] = {}
    for f in repo.files:
        parts = f.split("/")
        if (
            len(parts) < 3
            or parts[0] != "tests"
            or parts[1] not in MIXED_TEST_DIRS
            or repo.ext(f) != ".cpp"
        ):
            continue
        counts: Counter[str] = Counter()
        for m in INCLUDE_RE.finditer(repo.read(f)):
            _owner, target, _public = resolve(repo, index, f, m.group(2).strip(), "")
            lib = layoutdef.library_of(target) if target and target.startswith("src/") else None
            if lib:
                counts[lib] += 1
        if not counts:
            continue
        lib = min(counts, key=lambda k: (-counts[k], k))
        if parts[1] == "containers" or lib in MIXED_TEST_LIBS:
            out[f] = lib
    return out


def compute_moves(
    repo: Repo, scope: set[str], per_file_lib: dict[str, str] | None = None
) -> dict[str, str]:
    """Old path -> new path for everything the scope moves."""
    moves = {}
    for f in repo.files:
        new = None
        if f.startswith("src/"):
            new = layoutdef.l2_new(f, True) if "src" in scope else None
        elif f.startswith("tests/"):
            new = layoutdef.mirrored_tests_new(f, per_file_lib) if "tests" in scope else None
        # the packages and the files named for the wire extension: some sit under src/ and tests/
        if new is None and "packages" in scope:
            new = layoutdef.package_new(f)
        if new and new != f:
            moves[f] = new
    return moves


def plan_include_edits(repo: Repo, moves: dict[str, str], quiet: bool, index=None):
    """Per old file: {old spelling: new spelling}, plus report lines."""
    if index is None:
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
                elif cand == target:
                    # a quote-relative include of a header that no longer sits beside the includer
                    # (a test and its helper that went to different directories)
                    edits[f][sp] = posixpath.relpath(t_new, posixpath.dirname(f_new))
                    stats["relative"] += 1
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
        if repo.ext(f) in CPP_EXT or not is_text(repo, f) or f.startswith(KEEP_OLD_SPELLINGS):
            continue
        p = root / moves.get(f, f)
        try:
            text = p.read_bytes().decode("utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        if "include" not in text:
            continue

        def sub(mo):
            sp = mo.group(2).strip()
            return (
                mo.group(0).replace(mo.group(2), global_map[sp])
                if sp in global_map
                else mo.group(0)
            )

        out = TEXT_INCLUDE_RE.sub(sub, text)
        if out != text:
            p.write_bytes(out.encode("utf-8"))
            changed += 1
    return changed


def run(
    root: Path, scope: set[str], phase: str, json_path: str | None = None, quiet: bool = False
) -> dict:
    """Plan what the scope moves and rewrites, print the summary, and carry out `phase`."""
    repo = Repo(str(root))
    index, _ = build_index(repo)
    per_file_lib = test_libraries(repo, index) if "tests" in scope else {}
    moves = compute_moves(repo, scope, per_file_lib)
    edits, stats, problems, global_map = plan_include_edits(repo, moves, quiet, index)
    by_area = Counter(old.split("/")[0] for old in moves)
    print(
        f"{len(moves)} moves ({', '.join(f'{k} {v}' for k, v in sorted(by_area.items()))}); "
        f"{sum(len(v) for v in edits.values())} include rewrites in "
        f"{sum(1 for v in edits.values() if v)} files; kinds {dict(stats)}; "
        f"{len(problems)} includes reach a moved private header from another library"
    )
    for f, sp, t, tn in problems[:40]:
        print("  PROBLEM", f, "includes", repr(sp), "->", t, "now", tn)
    plan = {
        "scope": sorted(scope),
        "moves": moves,
        "edits": {k: v for k, v in edits.items() if v},
        "problems": problems,
        "spellings": global_map,
    }
    if json_path:
        Path(json_path).write_text(json.dumps(plan, indent=1), encoding="utf-8")
    if phase == "plan":
        return plan
    do_moves(root, moves)
    n = apply_include_edits(root, repo, moves, edits, global_map)
    d = rewrite_docs_and_scripts(root, repo, moves, global_map)
    print(f"rewrote includes in {n} C/C++ files and {d} other text files")
    return plan


def main() -> None:
    ap = base_parser(__doc__)
    ap.add_argument("--phase", choices=["plan", "all"], default="plan")
    ap.add_argument(
        "--scope",
        default=DEFAULT_SCOPE,
        help="what moves: src, tests, packages (comma-separated) or all",
    )
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--json", default=None, help="write the plan to this file")
    a = ap.parse_args()
    run(Path(a.root), parse_scope(a.scope), a.phase, a.json, a.quiet)


if __name__ == "__main__":
    main()
