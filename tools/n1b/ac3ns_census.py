"""The census of stage S6: the namespaces the AC-3 library declares into, what it declares there,
and who else declares into them. It writes `ac3ns_symbols.json`, the table `n1b_ac3ns.py` reads.

    ac3ns_census.py --root <worktree> --compile-commands <compile_commands.json> --out <table.json>
                    [--like tests/ac3/decoder/test_decoder.cpp] [--md <table.md>] [--work <dir>]
                    [--ast <dump.json>] [-I <dir> ...]

The names come from the compiler. One translation unit includes every public header under
`src/*/include/` and every private header of `src/ac3/src/`, and clang dumps its AST as JSON
(`-Xclang -ast-dump=json -Xclang -ast-dump-filter=iclforge`, with the command line of a test unit
of the configured tree, so that the include directories are the build's). Every declaration at
namespace scope is attributed to the file it is in: types, enumerations and the enumerators of an
unscoped one, functions, templates, variables, aliases, using-declarations. A scanner reading the
text (symtab.py) finds about 92% of them and misses the constants and the template functions, which
is why it is not the source. The scanner is used for what the compiler was not shown: the sources
and the files outside `src/`, to find who else declares into a namespace the library declares into.

A namespace the library declares into is `exclusive` when nobody else does, and `shared` otherwise
(another library's namespace opened inside it counts: the objects `iclforge::internal::arch`).
A file outside the library that declares a name the library also declares, in one of its
namespaces (`tests/ac3/core/avx2/absent/avx2_tier.cpp` defines `avx2_probe_matches_expected`, the
library's own function, for a build that has no AVX2 tier), is a follower: its block follows the
library, and its other blocks (`iclforge::test::avx2`) do not. A file outside the library that
opens one of the library's exclusive namespaces for any other reason is listed in
`outside_openers` for a person.

`--ast` reads a dump made earlier instead of running clang. Everything is relative to the root
namespace: `iclforge::oba::joc` is the key `oba::joc` and the root itself is `<root>`. With
`--root` an export of a commit (a scratch repository made from `git archive`), the compile command
is the configured tree's with its source tree replaced by that one.
"""

from __future__ import annotations

import json
import re
import shlex
import subprocess
import sys
import time
from collections import defaultdict
from pathlib import Path

import symtab
from ac3ns_core import LIBRARY_PREFIX, ROOT_KEY, ROOT_NS, blocks, mask, path_to_key
from n1b_lib import CPP_EXT, HEADER_EXT, Repo, base_parser

NsPath = tuple[str, ...]
TEMPLATES = (".hpp.in", ".h.in")

SCHEMA = 1
DEFAULT_LIKE = "tests/ac3/decoder/test_decoder.cpp"
# the kinds of declaration at namespace scope that bind a name a use can name
BINDING_KINDS = {
    "CXXRecordDecl", "ClassTemplateDecl", "EnumDecl", "TypedefDecl", "TypeAliasDecl",
    "TypeAliasTemplateDecl", "FunctionDecl", "FunctionTemplateDecl", "VarDecl", "VarTemplateDecl",
    "NamespaceAliasDecl", "UsingDecl", "UsingEnumDecl",
}  # fmt: skip


# --- the compiler's view --------------------------------------------------------------------------


def stream(path: Path):
    """The JSON values of a clang dump made with a filter: several, one after the other."""
    text = path.read_text(encoding="utf-8")
    dec = json.JSONDecoder()
    i = 0
    while True:
        while i < len(text) and text[i] in " \r\n\t":
            i += 1
        if i >= len(text):
            return
        obj, i = dec.raw_decode(text, i)
        yield obj


class Walker:
    """Walks the nodes of the dumps in document order, keeping the file the compiler last named (it
    prints a file name only when it changes), and collects the declarations at namespace scope."""

    def __init__(self) -> None:
        self.file = ""
        self.line = 0
        self.decls: list[
            tuple[tuple[str, ...], str, str, str, int]
        ] = []  # path, name, kind, file, line

    def _loc(self, d: object) -> None:
        if not isinstance(d, dict):
            return
        if "expansionLoc" in d:
            self._loc(d.get("spellingLoc"))
            self._loc(d["expansionLoc"])
            return
        self.file = d.get("file", self.file)
        self.line = d.get("line", self.line)

    def node(self, n: dict, path: tuple[str, ...]) -> None:
        self._loc(n.get("loc"))
        at = (self.file, self.line)
        rng = n.get("range")
        if isinstance(rng, dict):
            self._loc(rng.get("begin"))
            self._loc(rng.get("end"))
        kind, name = n.get("kind"), n.get("name")
        if kind == "NamespaceDecl":
            if name == ROOT_NS and not path:
                sub = path
            else:
                sub = (*path, name or "(anon)")
                if name:
                    self.decls.append((path, name, "NamespaceDecl", *at))
            for child in n.get("inner", []):
                self.node(child, sub)
            return
        if kind == "LinkageSpecDecl":
            for child in n.get("inner", []):
                self.node(child, path)
            return
        if n.get("isImplicit") or kind not in BINDING_KINDS:
            return
        if kind == "UsingDecl":
            name = (name or "").split("::")[-1]
        if not name:
            return
        self.decls.append((path, name, kind, *at))
        if kind == "EnumDecl" and not n.get("scopedEnumTag"):
            for child in n.get("inner", []):
                if child.get("kind") == "EnumConstantDecl" and child.get("name"):
                    self._loc(child.get("loc"))
                    self.decls.append(
                        (path, child["name"], "EnumConstantDecl", self.file, self.line)
                    )


def ast_declarations(dump: Path, root: Path) -> list[tuple[tuple[str, ...], str, str, str, int]]:
    """(path relative to the root namespace, name, kind, file relative to `root`, line)."""
    w = Walker()
    for top in stream(dump):
        # the filter also matches global declarations with the word in their name (the C API's)
        if top.get("kind") == "NamespaceDecl" and top.get("name") == ROOT_NS:
            w.node(top, ())
    base = str(root).replace("\\", "/").rstrip("/") + "/"
    out = []
    for path, name, kind, file, line in w.decls:
        f = file.replace("\\", "/")
        if f.lower().startswith(base.lower()):
            out.append((path, name, kind, f[len(base) :], line))
    return out


def census_unit(repo: Repo) -> str:
    """The translation unit: every public header, and the private headers of the library. A header
    the build generates from a template (`version.hpp.in`) is not in it: the scanner reads the
    template."""
    heads = []
    for f in repo.files:
        if "/variants/" in f or "third_party" in f:
            continue
        public = f.startswith("src/") and f.split("/")[2] == "include"
        if f.endswith(tuple(HEADER_EXT)) and (public or f.startswith(LIBRARY_PREFIX + "src/")):
            heads.append(f)
    root = str(repo.root).replace("\\", "/")
    lines = [f'#include "{root}/{h}"' for h in heads]
    return "// generated by ac3ns_census.py\n" + "\n".join(lines) + "\n"


def clang_command(
    entry: dict, unit: Path, extra: list[str], like: str = "", root: Path | None = None
) -> list[str] | str:
    """The compile command of a test unit with the census's flags, as a string (Windows) or a list.
    With `root`, the tree the command was made in is replaced by it: a census of an export of the
    commit, with the configured tree's generated headers."""
    cmd = entry.get("command") or " ".join(shlex.quote(a) for a in entry["arguments"])
    if root is not None and like:
        made_in = entry["file"].replace("\\", "/")
        made_in = made_in[: len(made_in) - len(like)].rstrip("/")
        here = str(root).replace("\\", "/").rstrip("/")
        for old, new in ((made_in, here), (made_in.replace("/", "\\"), here.replace("/", "\\"))):
            cmd = cmd.replace(old, new)
    cmd = re.sub(r"\s/F[od]\S+", "", cmd)
    cmd = re.sub(r"\s-o\s+\S+", "", cmd)
    cmd = re.sub(r"\s-c\s+--\s+.*$", "", cmd)
    cmd = re.sub(r"\s-c\s+\S+\s*$", "", cmd)
    cmd = cmd.replace("-Werror", "")
    msvc_style = "clang-cl" in cmd.split()[0].lower() or "/nologo" in cmd
    ast = " -Xclang -ast-dump-filter=" + ROOT_NS + " -Xclang -ast-dump=json"
    cmd += (
        " " + " ".join(extra) + ast + (" /Zs -- " if msvc_style else " -fsyntax-only ") + str(unit)
    )
    return cmd if sys.platform == "win32" else shlex.split(cmd)


def run_clang(root: Path, compile_commands: Path, like: str, extra: list[str], work: Path) -> Path:
    entries = json.loads(compile_commands.read_text(encoding="utf-8"))
    entry = next(e for e in entries if e["file"].replace("\\", "/").endswith(like))
    work.mkdir(parents=True, exist_ok=True)
    unit, dump = work / "census_unit.cpp", work / "census_ast.json"
    unit.write_text(census_unit(Repo(str(root))), encoding="ascii", newline="\n")
    started = time.time()
    with open(dump, "wb") as out, open(work / "census_ast.err", "wb") as err:
        done = subprocess.run(
            clang_command(entry, unit, extra, like, root),
            stdout=out,
            stderr=err,
            cwd=entry["directory"],
            check=False,
        )
    print(
        f"clang: exit {done.returncode}, {time.time() - started:.1f}s, {dump.stat().st_size} bytes"
    )
    if done.returncode != 0:
        sys.exit(f"the census unit does not compile: see {work / 'census_ast.err'}")
    return dump


# --- the scanner's view ---------------------------------------------------------------------------


def library_of(path: str) -> str:
    p = path.split("/")
    if p[0] == "src" and len(p) > 2:
        return "src/" + p[1]
    if p[0] == "apps" and len(p) > 2:
        return "apps/" + p[1]
    return p[0]


def relative(abs_path: tuple[str, ...]) -> tuple[str, ...] | None:
    """A namespace path as the root-relative one, or None when it is not under the root."""
    return abs_path[1:] if abs_path and abs_path[0] == ROOT_NS else None


def scan_files(repo: Repo):
    """Every C++ file's namespaces (root-relative), and the names the scanner finds in them."""
    for f in repo.files:
        if repo.ext(f) not in CPP_EXT and not f.endswith(TEMPLATES):
            continue
        text = repo.read(f)
        if "namespace" not in text:
            continue
        opened = set()
        for b in blocks(text, mask(text)):
            rel = relative(b.path) if b.named and b.comps else None
            if rel is not None and b.comps[-1] != "":
                opened.add(rel)
        names: dict[tuple[str, ...], set[str]] = defaultdict(set)
        for ns, name, _kind in symtab.scan_flat(text):
            parts = tuple(ns.split("."))
            if parts[0] == ROOT_NS:
                names[parts[1:]].add(name)
        yield f, opened, names


# --- the table ------------------------------------------------------------------------------------


def build_table(repo: Repo, decls, parent: str) -> dict:
    ac3_names: dict[NsPath, set[str]] = defaultdict(set)
    declared: dict[str, dict[NsPath, set[str]]] = defaultdict(lambda: defaultdict(set))
    for path, name, kind, f, _line in decls:
        if name.startswith("operator"):
            continue
        if f.startswith(LIBRARY_PREFIX):
            if kind != "NamespaceDecl":  # a namespace is not a name that moves: its path decides
                ac3_names[path].add(name)
        else:
            declared[f][path].add(name)  # a namespace another library opens is one of its names

    opened: dict[str, set[NsPath]] = {}
    for f, ns_opened, names in scan_files(repo):
        opened[f] = ns_opened
        if not f.startswith(LIBRARY_PREFIX):
            for p, ns in names.items():
                declared[f][p] |= ns
        elif f.endswith(TEMPLATES):  # the compiler is not shown the header the build makes of it
            for p, ns in names.items():
                ac3_names[p] |= ns

    paths: set[NsPath] = set(ac3_names)
    for f, ns_opened in opened.items():
        if f.startswith(LIBRARY_PREFIX):
            paths |= ns_opened

    # a file outside the library that declares a name the library also declares, in one of its
    # namespaces, defines the library's own: its block there follows the library
    followers: dict[str, set[NsPath]] = defaultdict(set)
    for f, by_path in declared.items():
        for p, ns in by_path.items():
            if p in paths and ns & ac3_names.get(p, set()):
                followers[f].add(p)

    rows: dict[str, dict] = {}
    outside: dict[str, list[str]] = {}
    for p in sorted(paths, key=lambda q: (len(q), q)):
        others: dict[str, set[str]] = defaultdict(set)
        openers: set[str] = set()
        for f, by_path in declared.items():
            if p in followers.get(f, ()):
                continue
            if by_path.get(p):
                others[library_of(f)] |= by_path[p]
        for f, ns_opened in opened.items():
            if f.startswith(LIBRARY_PREFIX) or p in followers.get(f, ()):
                continue
            if p in ns_opened:
                openers.add(f)
            for o in ns_opened:  # a namespace another library opens inside this one
                if len(o) > len(p) and o[: len(p)] == p and o[: len(p) + 1] not in paths:
                    others[library_of(f)].add(o[len(p)])
        # the root holds every library's namespace, so it is never the library's outright
        row: dict = {
            "owner": "shared" if others or not p else "exclusive",
            "names": sorted(ac3_names.get(p, ())),
        }
        if others:
            row["others"] = {lib: sorted(ns) for lib, ns in sorted(others.items())}
        rows[path_to_key(p)] = row
        # a file outside the library that opens an exclusive namespace and declares nothing there
        if not others and p and openers:
            outside[path_to_key(p)] = sorted(openers)
    return {
        "schema": SCHEMA,
        "root": ROOT_NS,
        "nest": "ac3",
        "library": LIBRARY_PREFIX.rstrip("/"),
        "parent": parent,
        "namespaces": rows,
        "followers": {f: sorted(path_to_key(p) for p in ps) for f, ps in sorted(followers.items())},
        "outside_openers": outside,
    }


def render_markdown(table: dict) -> str:
    """The table a person reads: one row per namespace the library declares into."""
    lines = [
        "| namespace | owner | names the library declares | declared by others (library: names) |",
        "|---|---|---:|---|",
    ]
    for key, row in table["namespaces"].items():
        shown = "iclforge" if key == ROOT_KEY else f"iclforge::{key}"
        others = ", ".join(f"{lib}: {len(ns)}" for lib, ns in row.get("others", {}).items()) or "-"
        lines.append(f"| `{shown}` | {row['owner']} | {len(row['names'])} | {others} |")
    return "\n".join(lines) + "\n"


def main() -> None:
    ap = base_parser(__doc__)
    ap.add_argument("--compile-commands", default=None)
    ap.add_argument("--like", default=DEFAULT_LIKE)
    ap.add_argument("--ast", default=None, help="a dump made earlier")
    ap.add_argument("--work", default=None, help="where the unit and the dump are written")
    ap.add_argument("--md", default=None)
    ap.add_argument("--parent", default="")
    ap.add_argument("-I", dest="include", action="append", default=[])
    a = ap.parse_args()
    root = Path(a.root)
    repo = Repo(a.root)
    work = Path(a.work) if a.work else Path(a.out or ".").resolve().parent
    if a.ast:
        dump = Path(a.ast)
    elif a.compile_commands:
        extra = [f"-I{d}" for d in a.include]
        dump = run_clang(root, Path(a.compile_commands), a.like, extra, work)
    else:
        sys.exit("ac3ns_census: give --compile-commands (a configured tree) or --ast (a dump)")
    parent = (
        a.parent
        or subprocess.run(
            ["git", "-C", str(root), "rev-parse", "HEAD"],
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
    )
    table = build_table(repo, ast_declarations(dump, root), parent)
    text = json.dumps(table, indent=1) + "\n"
    if a.out:
        Path(a.out).parent.mkdir(parents=True, exist_ok=True)
        Path(a.out).write_text(text, encoding="utf-8", newline="\n")
    else:
        sys.stdout.write(text)
    if a.md:
        Path(a.md).write_text(render_markdown(table), encoding="utf-8", newline="\n")
    rows = table["namespaces"]
    print(
        f"{len(rows)} namespaces: "
        f"{sum(r['owner'] == 'exclusive' for r in rows.values())} exclusive, "
        f"{sum(r['owner'] == 'shared' for r in rows.values())} shared; "
        f"{sum(len(r['names']) for r in rows.values())} names; "
        f"{len(table['followers'])} followers; "
        f"{sum(len(v) for v in table['outside_openers'].values())} outside openers",
        file=sys.stderr,
    )


if __name__ == "__main__":
    main()
