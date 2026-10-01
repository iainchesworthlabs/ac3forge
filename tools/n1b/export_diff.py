"""Compare the exported symbols of the libraries before and after a change (two `symbols` records).

    export_diff.py --old <symbols-msvc.json> --new <symbols-msvc.json> [--map identity|l2]
                   [--rewrite cuts,names,idents,ac3ns] [--copies] [--limit 30]

`baseline.py record --only symbols` writes one record per build: for every shared library, the
names it exports, undecorated. This compares two of them. What a stage may change is named by the
options and nothing else passes.

  --map identity   libraries are the same files in both records (S1, S3, S4 once names have moved)
  --map l2         the libraries of S2: ac3forge.dll is compared with the union of the six it was
                   split into, and every other library with the file its output name becomes
                   (n1b_cmake.OUTPUT)
  --rewrite cuts   the types the seven cuts of S1 moved appear under their new qualified names
  --rewrite names  the namespace root is rewritten the way n1b_names.py rewrites it (S3)
  --rewrite idents the brand in a name is rewritten the way n1b_idents.py rewrites an identifier
                   (S4): the C API's `ac3forge_encoder_create` is `iclforge_encoder_create`, and
                   `sendspin::ac3forge` is `sendspin::player`
  --rewrite ac3ns  the AC-3 library's names nest under `iclforge::ac3` (S6): a name the table of
                   ac3ns_symbols.json lists is rewritten the way n1b_ac3ns.py rewrites a qualified
                   name, so `iclforge::FrameEncoder::encode(iclforge::Acmod)` is
                   `iclforge::ac3::FrameEncoder::encode(iclforge::ac3::Acmod)`, and
                   `iclforge::oba::Position`, which the objects library declares, is itself
  --copies         a name that leaves one library is not a difference when another library of
                   the new record exports it. A shared library that links the codec statically
                   re-exports the members it pulled in (admbridge.dll carried 278 copies of what
                   ac3forge.dll exports), and a change to what its headers include changes
                   which members it pulls.

Prints, per old library, the names only in the old set and only in the new one, and then the union
of every library's names, which no option excuses: what the libraries export together may change
only by the rewrites. Exit status 1 if any name differs. The proof for a split is that the new
union equals the old set plus the functions that now cross a library boundary (the prototype found
one: has_avx2()), which are listed for a person to accept.
"""

from __future__ import annotations

import argparse
import functools
import json
import re
import sys
from pathlib import Path

from ac3ns_core import Table, qualify
from n1b_apply import SPLIT_LIBS
from n1b_cmake import OUTPUT
from n1b_idents import symbol_rename

CUT_RENAMES = [
    (r"\bac3::eac3::chanmap::(Location|Layout)\b", r"ac3::base::\1"),
    (r"\bac3::(PcmBlock|BlockSink)\b", r"ac3::render::\1"),
    (r"\bac3::DownmixTarget\b", "ac3::base::DownmixTarget"),
]
NAME_RENAMES = [
    (r"\bac3iab::", "iclforge::iab::"),
    (r"\bac3adm::", "iclforge::adm::"),
    (r"\bac3::", "iclforge::"),
    (r"\b(ac4|mp4|mpegts|matroska|iamf)::", r"iclforge::\1::"),
]


@functools.lru_cache(maxsize=1)
def ac3ns_table() -> Table:
    return Table.load()


def rewrite(name: str, kinds: set[str]) -> str:
    rules: list[tuple[str, str]] = []
    if "cuts" in kinds:
        rules += CUT_RENAMES
    if "names" in kinds:
        rules += NAME_RENAMES
    for pattern, replacement in rules:
        name = re.sub(pattern, replacement, name)
    if "ac3ns" in kinds:
        name = qualify(name, ac3ns_table())[0]
    if "idents" in kinds:
        name = symbol_rename(name)
    return name


def l2_map(old_libraries: list[str]) -> dict[str, list[str]]:
    """Old library file -> the new files that together export what it did."""
    out = {
        name + ".dll": [new + ".dll"]
        for name, new in OUTPUT.items()
        if not name.endswith(("_static", "_minimal"))
    }
    out["ac3forge.dll"] = [f"iclforge_{lib}.dll" for lib in SPLIT_LIBS]
    return {old: out.get(old, [old]) for old in old_libraries}


def compare(
    old: dict, new: dict, mapping: str, kinds: set[str], limit: int, copies: bool = False
) -> int:
    old_libs, new_libs = old["libraries"], new["libraries"]
    grouping = l2_map(sorted(old_libs)) if mapping == "l2" else {name: [name] for name in old_libs}
    everywhere = {n for names in new_libs.values() for n in names}
    bad = 0
    used: set[str] = set()
    for old_name, new_names in sorted(grouping.items()):
        present = [n for n in new_names if n in new_libs]
        used.update(present)
        want = {rewrite(n, kinds) for n in old_libs[old_name]}
        have = {n for lib in present for n in new_libs[lib]}
        only_old, only_new = sorted(want - have), sorted(have - want)
        kept = [n for n in only_old if n in everywhere] if copies else []
        only_old = [n for n in only_old if n not in kept]
        state = "same" if not only_old and not only_new else f"-{len(only_old)} +{len(only_new)}"
        if kept:
            state += f", and {len(kept)} names that another library still exports have left it"
        print(
            f"{old_name} ({len(want)}) <- {', '.join(present) or 'nothing'} ({len(have)}): {state}"
        )
        for label, names in (("only in old", only_old), ("only in new", only_new)):
            for n in names[:limit]:
                print(f"    {label}: {n[:170]}")
        bad += bool(only_old or only_new)
    for extra in sorted(set(new_libs) - used):
        names = len(new_libs[extra])
        print(f"{extra}: a library the old record has no counterpart for ({names} names)")
        bad += 1
    union_old = {rewrite(n, kinds) for names in old_libs.values() for n in names}
    lost, gained = sorted(union_old - everywhere), sorted(everywhere - union_old)
    print(
        f"all libraries together: {len(union_old)} names before, {len(everywhere)} after; "
        f"-{len(lost)} +{len(gained)}"
    )
    for label, names in (("only in old", lost), ("only in new", gained)):
        for n in names[:limit]:
            print(f"    {label}: {n[:170]}")
    return int(bool(bad or lost or gained))


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--old", required=True, type=Path)
    ap.add_argument("--new", required=True, type=Path)
    ap.add_argument("--map", choices=["identity", "l2"], default="identity")
    ap.add_argument("--rewrite", default="", help="comma-separated: cuts, names, idents, ac3ns")
    ap.add_argument("--copies", action="store_true", help="a name another library exports is kept")
    ap.add_argument("--limit", type=int, default=30)
    a = ap.parse_args()
    kinds = {k for k in a.rewrite.split(",") if k}
    unknown = kinds - {"cuts", "names", "idents", "ac3ns"}
    if unknown:
        sys.exit(f"export_diff: unknown --rewrite {sorted(unknown)}")
    old = json.loads(a.old.read_text(encoding="utf-8"))
    new = json.loads(a.new.read_text(encoding="utf-8"))
    return 1 if compare(old, new, a.map, kinds, a.limit, a.copies) else 0


if __name__ == "__main__":
    sys.exit(main())
