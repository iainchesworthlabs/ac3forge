"""The exported-symbol allowlists of the old layout against the ones the moved layout writes.

    abi_compare.py <old allowlist dir> <new allowlist dir> [--map l2|identity]
                   [--rewrite cuts,names,idents,ac3ns]

tools/ci/abi-allowlist holds one `<library>.so.txt` per shared library, the demangled names it
exports (tools/ci/check_abi_symbols.py). Stage S2 renames every library (`libac3iab.so` becomes
`libiclforge_iab.so`) and splits `libac3forge.so` into six, so the files are written again from the
shared tree (`check_abi_symbols.py --update`). This is the check that nothing was lost or gained on
the way: each old library must hold the same names as the new one that replaces it, and the union of
the six must be what `libac3forge.so` exported. It prints one line per old library, lists the names
that differ, and exits 1 when any does. The one difference S2 is expected to show is `has_avx2()`,
which the split makes cross a library boundary (base to ac3) and so export.

  --map l2         the libraries of S2 (the default): `libac3forge.so` against the six it became
  --map identity   every library of the old directory is the same file in the new one (S3, S4)
  --rewrite names  the old names are rewritten first, the way n1b_names.py rewrites the source
                   (export_diff.rewrite), so a namespace rename shows no difference: the
                   allowlists hold demangled names, which unlike the mangled ones can be rewritten
                   as text (S3)
  --rewrite idents the brand in the old names is rewritten the way n1b_idents.py rewrites an
                   identifier, so the C API's `ac3forge_*` names show no difference against
                   `iclforge_*` (S4); it combines with names (`--rewrite names,idents`)
  --rewrite ac3ns  the AC-3 library's names nest under `iclforge::ac3`: the old names are
                   rewritten the way n1b_ac3ns.py rewrites a qualified name (S6), so the allowlists
                   `check_abi_symbols.py --update` writes from the new build show no difference
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import export_diff

# old library -> the libraries that replace it
LIBRARIES: dict[str, list[str]] = {
    "libac3forge.so": [
        "libiclforge_ac3.so",
        "libiclforge_base.so",
        "libiclforge_dsp.so",
        "libiclforge_objects.so",
        "libiclforge_render.so",
        "libiclforge_iec61937.so",
    ],
    "libac3forge_c.so": ["libiclforge_c.so"],
    "libac3iab.so": ["libiclforge_iab.so"],
    "libac3signing.so": ["libiclforge_signing.so"],
    "libac4.so": ["libiclforge_ac4.so"],
    "libac4dec.so": ["libiclforge_ac4dec.so"],
    "libac4enc.so": ["libiclforge_ac4enc.so"],
    "libiamf.so": ["libiclforge_iamf.so"],
    "libmatroska.so": ["libiclforge_matroska.so"],
    "libmp4.so": ["libiclforge_mp4.so"],
    "libmpegts.so": ["libiclforge_mpegts.so"],
}


def read(directory: Path, library: str) -> set[str]:
    path = directory / f"{library}.txt"
    if not path.is_file():
        return set()
    return {line for line in path.read_text(encoding="utf-8").splitlines() if line}


def identity(old_dir: Path) -> dict[str, list[str]]:
    """Every library of the old directory, replaced by the file of the same name."""
    names = [p.name.removesuffix(".txt") for p in sorted(old_dir.glob("*.so.txt"))]
    return {name: [name] for name in names}


def compare(
    old_dir: Path,
    new_dir: Path,
    out=sys.stdout,
    mapping: dict[str, list[str]] | None = None,
    kinds: set[str] | None = None,
) -> int:
    """Print the comparison; return the number of old libraries whose names changed."""
    changed = 0
    for old, replacements in (LIBRARIES if mapping is None else mapping).items():
        before = {export_diff.rewrite(n, kinds or set()) for n in read(old_dir, old)}
        parts = {name: read(new_dir, name) for name in replacements}
        after = set().union(*parts.values())
        lost, gained = sorted(before - after), sorted(after - before)
        shown = ", ".join(
            f"{name.removeprefix('libiclforge_').removesuffix('.so')} {len(names)}"
            for name, names in parts.items()
        )
        plural = "y" if len(replacements) == 1 else "ies"
        print(
            f"{old} ({len(before)}) <- {len(replacements)} librar{plural} "
            f"({len(after)}: {shown}): -{len(lost)} +{len(gained)}",
            file=out,
        )
        for name in lost:
            print(f"    only in old: {name}", file=out)
        for name in gained:
            print(f"    only in new: {name}", file=out)
        twice = sorted(n for n in after if sum(n in names for names in parts.values()) > 1)
        if twice:
            head = f"    exported by more than one of them: {len(twice)}"
            print(f"{head}, e.g. {twice[:3]}", file=out)
        if lost or gained:
            changed += 1
    return changed


def main() -> None:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("old", type=Path)
    ap.add_argument("new", type=Path)
    ap.add_argument("--map", choices=["l2", "identity"], default="l2")
    ap.add_argument("--rewrite", default="", help="comma-separated: cuts, names, idents, ac3ns")
    a = ap.parse_args()
    kinds = {k for k in a.rewrite.split(",") if k}
    unknown = kinds - {"cuts", "names", "idents", "ac3ns"}
    if unknown:
        sys.exit(f"abi_compare: unknown --rewrite {sorted(unknown)}")
    mapping = identity(a.old) if a.map == "identity" else None
    sys.exit(1 if compare(a.old, a.new, mapping=mapping, kinds=kinds) else 0)


if __name__ == "__main__":
    main()
