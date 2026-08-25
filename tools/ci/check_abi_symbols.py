"""Compare a built shared library's exported dynamic symbols against a
checked-in allowlist (roadmap AP4).

Every library here already builds with CXX_VISIBILITY_PRESET hidden, so
nm -D --defined-only is the exact set a consumer can link against - anything
NOT AC3FORGE_EXPORT (or one of the sibling macros) never reaches the dynamic
symbol table at all. This script does not re-derive that set from the source;
it just proves the .so's ACTUAL export set matches what was last reviewed and
committed, so an accidental new export (a missing AC3FORGE_EXPORT removed, a
template instantiation that leaked, a symbol visibility regression) is a
diff a human sees rather than a silent widening of the ABI surface.

One allowlist file per library, named after the .so's basename
(libac3forge.so.txt), one demangled symbol per line, sorted. `--update`
regenerates them from the libraries passed on the command line instead of
checking; run it once to seed a new library or after a deliberate, reviewed
export-set change.

Unlike tools/ci/compare_performance.py, this script DOES exit non-zero on a
real mismatch - the advisory/non-blocking behaviour AP4 wants pre-1.0 comes
from the calling CI job's own `continue-on-error: true`, not from this script
staying silent. That is deliberate: once AP1's freeze removes that one line,
this script's behaviour does not need to change at all.

stdlib-only (argparse/pathlib/subprocess), matching every other script in
this directory - the runner needs no provisioning beyond nm/c++filt, which
binutils already puts on every Linux CI leg.
"""

import argparse
import subprocess
import sys
from pathlib import Path


def exported_symbols(library: Path) -> list[str]:
    """Demangled dynamic symbols `library` defines, sorted, de-duplicated.

    nm -D --defined-only lists exactly the dynamic symbol table's defined
    entries - undefined (imported) symbols are excluded by --defined-only,
    and anything hidden by CXX_VISIBILITY_PRESET never appears in the
    dynamic table to begin with. -C demangles in the same pass, so the
    allowlist reads as C++ names a reviewer can recognise rather than
    mangled ones a separate c++filt pass would otherwise be needed for.
    """
    result = subprocess.run(
        ["nm", "-D", "--defined-only", "-C", str(library)],
        capture_output=True,
        text=True,
        check=True,
    )
    names = set()
    for line in result.stdout.splitlines():
        # Format: "<address> <type-char> <name>" - address and type are not
        # part of the ABI contract this checks (a symbol moving address is
        # not a break; its type char changing kind, e.g. object to function,
        # would already show up as a different signature in the abidiff
        # step this script's own CI job pairs it with).
        parts = line.split(maxsplit=2)
        if len(parts) == 3:
            names.add(parts[2])
    return sorted(names)


def allowlist_path(allowlist_dir: Path, library: Path) -> Path:
    return allowlist_dir / f"{library.name}.txt"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--lib", action="append", required=True, dest="libs",
        help="Path to a built .so; repeatable, one per library.")
    parser.add_argument(
        "--allowlist-dir", required=True, type=Path,
        help="Directory of checked-in <library>.so.txt allowlist files.")
    parser.add_argument(
        "--update", action="store_true",
        help="Regenerate the allowlist files from --lib instead of checking them.")
    args = parser.parse_args()

    args.allowlist_dir.mkdir(parents=True, exist_ok=True)
    libraries = [Path(lib) for lib in args.libs]

    if args.update:
        for library in libraries:
            symbols = exported_symbols(library)
            allowlist_path(args.allowlist_dir, library).write_text(
                "\n".join(symbols) + ("\n" if symbols else ""), encoding="utf-8")
            print(f"Wrote {len(symbols)} symbols for {library.name}")
        return 0

    mismatched = False
    for library in libraries:
        allow_file = allowlist_path(args.allowlist_dir, library)
        actual = set(exported_symbols(library))
        expected = set(
            allow_file.read_text(encoding="utf-8").splitlines()
        ) if allow_file.exists() else set()

        added = sorted(actual - expected)
        removed = sorted(expected - actual)
        if not added and not removed:
            print(f"OK  {library.name}: {len(actual)} exported symbols, matches allowlist")
            continue

        mismatched = True
        print(f"MISMATCH  {library.name} vs {allow_file.name}")
        for symbol in added:
            print(f"  ::warning::+ {symbol} (newly exported, not in allowlist)")
        for symbol in removed:
            print(f"  ::warning::- {symbol} (allowlisted, no longer exported)")

    if mismatched:
        print()
        print("Exported symbols changed. If this is a deliberate, reviewed API "
              "change, regenerate the allowlist with --update and commit it.")
        return 1

    print("All libraries match their allowlist.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
