"""The exported-symbol allowlists of the old layout against the ones the moved layout writes.

    abi_compare.py <old allowlist dir> <new allowlist dir>

tools/ci/abi-allowlist holds one `<library>.so.txt` per shared library, the demangled names it
exports (tools/ci/check_abi_symbols.py). Stage S2 renames every library (`libac3iab.so` becomes
`libiclforge_iab.so`) and splits `libac3forge.so` into six, so the files are written again from the
shared tree (`check_abi_symbols.py --update`). This is the check that nothing was lost or gained on
the way: each old library must hold the same names as the new one that replaces it, and the union of
the six must be what `libac3forge.so` exported. It prints one line per old library, lists the names
that differ, and exits 1 when any does. The one difference S2 is expected to show is `has_avx2()`,
which the split makes cross a library boundary (base to ac3) and so export.
"""

from __future__ import annotations

import sys
from pathlib import Path

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


def compare(old_dir: Path, new_dir: Path, out=sys.stdout) -> int:
    """Print the comparison; return the number of old libraries whose names changed."""
    changed = 0
    for old, replacements in LIBRARIES.items():
        before = read(old_dir, old)
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
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    sys.exit(1 if compare(Path(sys.argv[1]), Path(sys.argv[2])) else 0)


if __name__ == "__main__":
    main()
