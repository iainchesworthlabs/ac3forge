"""The AC-3 codec's names, stage S6 of the plan: `iclforge::FrameEncoder` becomes
`iclforge::ac3::FrameEncoder`.

    n1b_ac3ns.py --root <worktree> --phase decl|uses|all [--dry-run] [--report <file>]
                 [--json <file>]
    n1b_ac3ns.py --root <worktree> --phase fix --log <build log> ... [--dry-run] [--list]
    n1b_ac3ns.py --root <worktree> --phase loop --build <build dir> [--rounds 6] [--target all]
    n1b_ac3ns.py --root <worktree> --phase record --base <rev> --sites <sites.json>
    n1b_ac3ns.py --root <worktree> --phase sites --sites <sites.json> [--dry-run]

S3 put every library's namespace under the family root `iclforge`; the AC-3 and E-AC-3 codec stayed
in the root itself (`iclforge::FrameEncoder`, `iclforge::eac3::...`, `iclforge::meta::...`). This
stage nests it as a peer of the other codecs, `iclforge::ac3`, so that the root holds one namespace
per library and declares nothing of the codec's. The sub-namespaces keep their names
(`iclforge::ac3::eac3`, `::meta`, `::io`, `::plan` ...) and nothing is aliased or re-exported under
the old spelling.

What moves is decided by a table (`ac3ns_symbols.json`, made by `ac3ns_census.py` from the
compiler's own view of the headers): for each namespace the library declares into, who else does.

  exclusive   only the AC-3 library declares into it (`meta`, `io`, `eac3`, `plan` ...): every name
              under `iclforge::<path>` moves, so a use is rewritten by its prefix.
  shared      another library declares into it too (the root, `oba`, `emdf`, `render`,
              `internal`, `detail`): only the names the AC-3 library declares move, and a use is
              rewritten when its name is one of them (`iclforge::oba::AtmosEncoder` moves,
              `iclforge::oba::Position` is the objects library's and stays).

Phases, each committed alone:

  decl     every `namespace iclforge ...` block of the library's own files (src/ac3, with its
           variants, private headers and sources) opens under `iclforge::ac3`, and its closing
           comment says so. A file outside the library that declares one of the library's names in
           one of its namespaces (a test's stand-in for a function the library defines) follows by
           the paths the table lists for it; the namespaces such a file opens for its own sake do
           not move.
  uses     every qualified spelling in every tracked text file gets the new namespace, by the
           rules above: C++, its comments and strings, the generated-header templates, the Qt
           catalogues and the pages. The history (`CHANGELOG.md`, `planning/`, the byte-exact
           trees, this directory), the Rust crate (whose own module is called `ac3`), CMake (whose
           `iclforge::<library>` is a target) and what a build writes (the ABI allowlists) are
           not read.
  fix      what the compiler finds after those two: ac3ns_fix.py says how.
  loop     a build, `fix` on its log, a build again, until nothing is left that the table explains.
  record   the net edit of the loop against a commit, as data.
  sites    that record applied to a tree that has the same lines, so that the commit of the loop
           can be made again on its parent with no build.

Every change `decl` and `uses` make is reported (`--report`): the file and line, whether the place
is code, a comment or a string, and the name before and after. A string that prints a qualified
name (an exception message, a test's name) is the only text that may change what a program says.
"""

from __future__ import annotations

import json
import sys
from collections import Counter
from pathlib import Path

import ac3ns_fix as fix
import n1b_docs as docs
from ac3ns_core import DEFAULT_TABLE, LIBRARY_PREFIX, Table, declare, qualify
from n1b_lib import Repo, base_parser

# --- which files ----------------------------------------------------------------------------------

CPP_SUFFIXES = (".cpp", ".cc", ".cxx", ".c", ".hpp", ".h", ".hh", ".hxx", ".inl", ".ipp", ".mm")
TEMPLATE_SUFFIXES = (".hpp.in", ".h.in")
# What a build writes and a person does not edit: the ABI allowlists hold the demangled names of the
# shared libraries (`check_abi_symbols.py --update` writes them from a build; an old file rewritten
# as text is what `abi_compare.py --rewrite ac3ns` holds the new one to).
GENERATED_PREFIXES = ("tools/ci/abi-allowlist/",)


def is_cpp(path: str) -> bool:
    return path.endswith(CPP_SUFFIXES + TEMPLATE_SUFFIXES)


def read_text(root: Path, rel: str) -> str | None:
    try:
        return (root / rel).read_bytes().decode("utf-8")
    except (OSError, UnicodeDecodeError):
        return None


def reads_uses(path: str) -> bool:
    """Does the `uses` phase read this file? Every tracked text file but the history, the pages
    that narrate a past tree, the binary ones, what a build writes, CMake and Rust."""
    if docs.is_history(path) or path in docs.RECORD_PAGES or path.endswith(docs.SKIP_SUFFIXES):
        return False
    if path.startswith(GENERATED_PREFIXES):
        return False
    return not path.endswith((".rs", ".cmake")) and path.rsplit("/", 1)[-1] != "CMakeLists.txt"


def run_text_phases(a, table: Table) -> int:
    root = Path(a.root)
    repo = Repo(a.root)
    phases = ["decl", "uses"] if a.phase == "all" else [a.phase]
    report: list[str] = []
    counts: Counter[str] = Counter()
    touched: dict[str, set[str]] = {p: set() for p in phases}
    for f in repo.files:
        text = read_text(root, f)
        if text is None:
            continue
        original = text
        for phase in phases:
            if phase == "decl":
                if not is_cpp(f):
                    continue
                new, changes = declare(text, table, f, f.startswith(LIBRARY_PREFIX))
            else:
                if not reads_uses(f):
                    continue
                new, changes = qualify(text, table)
            if changes:
                touched[phase].add(f)
                for c in changes:
                    counts[f"{phase}: places"] += 1
                    counts[f"{phase}: in {c.context}"] += 1
                    report.append(f"{phase}\t{f}:{c.line}\t{c.context}\t{c.before}\t{c.after}")
            text = new
        if text != original and not a.dry_run:
            (root / f).write_bytes(text.encode("utf-8"))
    verb = "would change" if a.dry_run else "changed"
    for phase in phases:
        print(f"{verb} {len(touched[phase])} files in the {phase} phase")
    summary = dict(sorted(counts.items()))
    print(json.dumps(summary, indent=1))
    if a.report:
        Path(a.report).write_text("\n".join(report) + "\n", encoding="utf-8", newline="\n")
    if a.json:
        Path(a.json).write_text(
            json.dumps(summary, indent=1) + "\n", encoding="utf-8", newline="\n"
        )
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = base_parser(__doc__)
    ap.add_argument(
        "--phase", choices=["decl", "uses", "all", "fix", "loop", "record", "sites"], default="all"
    )
    ap.add_argument("--table", default=str(DEFAULT_TABLE))
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--report", default=None, help="write every change to this file")
    ap.add_argument("--json", default=None, help="write the counts to this file")
    ap.add_argument("--log", action="append", default=[], help="a build log (fix)")
    ap.add_argument("--list", action="store_true", help="list the diagnostics left (fix)")
    ap.add_argument(
        "--path-map", action="append", default=[], metavar="OLD=NEW", help="a path prefix in a log"
    )
    ap.add_argument("--build", default=None, help="the build directory (loop)")
    ap.add_argument("--target", default="all")
    ap.add_argument("--rounds", type=int, default=6)
    ap.add_argument("--jobs", type=int, default=12)
    ap.add_argument("--work", default=None, help="where the loop keeps its logs")
    ap.add_argument("--base", default="HEAD", help="the commit a record is made against")
    ap.add_argument("--sites", default=None, help="the record (record, sites)")
    a = ap.parse_args(argv)
    root = Path(a.root)
    table = Table.load(Path(a.table))
    if a.phase in ("decl", "uses", "all"):
        return run_text_phases(a, table)
    if a.phase == "fix":
        path_map = [tuple(m.split("=", 1)) for m in a.path_map]
        return fix.main_fix(root, table, a.log, a.dry_run, a.list, path_map)
    if a.phase == "loop":
        work = Path(a.work or ".")
        work.mkdir(parents=True, exist_ok=True)
        rounds = fix.run_loop(root, table, a.build, a.target, a.rounds, a.jobs, work)
        print(f"{rounds} rounds wrote edits")
        return 0
    if a.phase == "record":
        return fix.main_record(root, a.base, Path(a.sites))
    return fix.main_sites(root, Path(a.sites), a.dry_run)


if __name__ == "__main__":
    sys.exit(main())
