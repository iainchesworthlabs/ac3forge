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

Two things to know before reading a regeneration diff:

The stored text is c++filt-version sensitive. The same mangled symbol can
demangle to different text under different binutils - libac3iab.so.txt was
seeded by an older c++filt that printed `std::istream` where the current one
prints `std::basic_istream<char, std::char_traits<char> >`. That shows up in
a diff as one line removed and one added, and reads exactly like a signature
change, but the ELF symbol is identical and nothing about the ABI moved.
Before treating such a pair as a real change, check whether the two lines are
the same function under two renderings.

Regenerate against a build whose toolchain matches CI's (the pinned LLVM in
.github/workflows/_toolchain-versions.yml). The cheap way to prove it does,
before trusting `--update`: run this script in CHECK mode against the fresh
build first and confirm it reproduces the mismatch set CI itself reports.
Libraries that then regenerate byte-identical are the corroboration - if the
local demangler disagreed with the committed files, those would churn too.

This script DOES exit non-zero on a real mismatch - the advisory/non-blocking
behaviour AP4 wants pre-1.0 comes from the calling CI job's own ABI_ENFORCE
switch, not from this script staying silent. That is deliberate: once AP1's
freeze flips that one value, this script's behaviour does not need to change
at all. (tools/ci/compare_performance.py splits the same problem the other
way round - it reports its verdict as a step output and lets a separate gate
job decide - because the job that runs IT is continue-on-error and would
swallow an exit code.)

stdlib-only (argparse/pathlib/subprocess), matching every other script in
this directory - the runner needs no provisioning beyond nm/c++filt, which
binutils already puts on every Linux CI leg.
"""

import argparse
import subprocess
import sys
from pathlib import Path

# Itanium-mangled prefixes for namespaces that are compiler/libstdc++
# implementation detail, not anything AC3FORGE_EXPORT controls. Which
# template gets instantiated - and therefore lands in the dynamic table at
# all, since CXX_VISIBILITY_PRESET hidden does not extend to system-header
# templates - shifts with every compiler/libstdc++ version, unrelated to
# any change this project makes. `_ZSt`/`_ZNSt` cover both the compressed
# "St" substitution and the nested-name form of std::; `_ZN9__gnu_cxx`
# covers libstdc++'s internal namespace (length-prefixed "__gnu_cxx", 9
# chars); `_ZN11__cxxabiv1` covers the C++ ABI runtime (RTTI/exception
# internals another shared library using polymorphism can just as easily
# leak).
#
# The `K` forms matter as much as the plain ones: Itanium mangles a CONST
# member function as `_ZNK<name>`, not `_ZN<name>`, so `std::_Hashtable<...
# >::find(...) const` arrives as `_ZNKSt10_Hashtable...` and slips past a
# `_ZNSt` check entirely. That is not hypothetical - it is how
# `std::_Hashtable<int, std::pair<int const, int>, ...>::find(int const&)
# const` came to be reported as newly-exported project surface on libac4.so
# (2026-08-31). `_ZTV`/`_ZTI`/`_ZTS` are the vtable, typeinfo and
# typeinfo-name of a std:: type, which are emitted into whichever library
# instantiates the type and shift for exactly the same reasons; they carry
# the `St`/`N9__gnu_cxx` substitution one or two characters further in, so
# they need their own entries rather than a longer prefix on the existing
# ones. tools/ci/abi-suppressions.ini encodes the same set for abidiff -
# keep the two in step.
_STDLIB_MANGLED_PREFIXES = (
    "_ZSt", "_ZNSt", "_ZNKSt",
    "_ZN9__gnu_cxx", "_ZNK9__gnu_cxx",
    "_ZN11__cxxabiv1", "_ZNK11__cxxabiv1",
    "_ZTVSt", "_ZTISt", "_ZTSSt",
    "_ZTVNSt", "_ZTINSt", "_ZTSNSt",
    "_ZTVN9__gnu_cxx", "_ZTIN9__gnu_cxx", "_ZTSN9__gnu_cxx",
    "_ZTVN11__cxxabiv1", "_ZTIN11__cxxabiv1", "_ZTSN11__cxxabiv1",
)


def exported_symbols(library: Path) -> list[str]:
    """Demangled dynamic symbols `library` defines, sorted, de-duplicated,
    excluding standard-library/runtime internals (see
    _STDLIB_MANGLED_PREFIXES above).

    nm -D --defined-only lists exactly the dynamic symbol table's defined
    entries - undefined (imported) symbols are excluded by --defined-only,
    and anything hidden by CXX_VISIBILITY_PRESET never appears in the
    dynamic table to begin with.

    The stdlib filter runs on the RAW mangled name, before demangling,
    because a demangled function-template instantiation shows its return
    type ahead of the qualified name (e.g. "long std::__lg<long>(long)") -
    a prefix or substring check on demangled text would either miss these
    or, worse, wrongly exclude a real project export whose signature merely
    mentions a std:: type it returns (e.g. mp4::fragment's
    std::expected<...> return type). The mangled encoding always puts the
    symbol's own qualified name first, so a prefix check there is
    unambiguous. c++filt then demangles only the survivors, so the
    allowlist still reads as C++ names a reviewer can recognise.
    """
    result = subprocess.run(
        ["nm", "-D", "--defined-only", str(library)],
        capture_output=True,
        text=True,
        check=True,
    )
    raw_names = []
    for line in result.stdout.splitlines():
        # Format: "<address> <type-char> <name>" - address and type are not
        # part of the ABI contract this checks (a symbol moving address is
        # not a break; its type char changing kind, e.g. object to function,
        # would already show up as a different signature in the abidiff
        # step this script's own CI job pairs it with).
        parts = line.split(maxsplit=2)
        if len(parts) == 3 and not parts[2].startswith(_STDLIB_MANGLED_PREFIXES):
            raw_names.append(parts[2])
    if not raw_names:
        return []
    demangled = subprocess.run(
        ["c++filt"],
        input="\n".join(raw_names),
        capture_output=True,
        text=True,
        check=True,
    )
    return sorted(set(demangled.stdout.splitlines()))


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
