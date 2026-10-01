"""The compiler loop of stage S6: what a build says about the names the text passes could not see.

    n1b_ac3ns.py --root <worktree> --phase fix --log <build log> ... [--dry-run] [--list]
    n1b_ac3ns.py --root <worktree> --phase loop --build <build dir> [--rounds 6] [--target all]
    n1b_ac3ns.py --root <worktree> --phase record --base <rev> --sites <sites.json>
    n1b_ac3ns.py --root <worktree> --phase sites --sites <sites.json>

After `decl` and `uses` a name is wrong in one of two ways, and the compiler says which at the
line and column of the name.

  A use that names the library's namespace through something that no longer holds it. Written
  from outside the library, inside `iclforge::mp4` or `iclforge::hearth` or after
  `using namespace iclforge;`, `FrameEncoder` and `meta::QcPreset` were found in the root and now
  are not (`use of undeclared identifier`, `no member named ... in namespace 'iclforge::oba'`).
  The qualification goes in front of the name: `ac3::` (found through the enclosing `iclforge`),
  with the namespaces the name sits in when it is not the root's (`ac3::oba::AtmosEncoder` for the
  `AtmosEncoder` that code inside `iclforge::oba` reached unqualified), or a second
  `using namespace iclforge::ac3;` beside the file's `using namespace iclforge;`.

  A use inside the library of a name another library declares in a namespace the two share. The
  library's own `iclforge::ac3::internal` now hides `iclforge::internal`, which the arithmetic and
  DSP libraries declare into, so `Fixed32` and `internal::arch::i32x4` written inside the library
  find the library's copy first and stop (`no member named 'arch' in namespace
  'iclforge::ac3::internal'`). The anchor `iclforge::` goes in front: `iclforge::internal::Fixed32`.

Both are written only when the table says the name is one of the library's (the first) or one of
another library's in that namespace (the second); a diagnostic the table cannot explain, including
every error that follows from an earlier one and every chain that starts at a namespace alias, is
listed and left. Every edit is an insertion, and a round of the loop is a build with `-k 0` (every
unit that can be compiled is), the edits of the diagnostics it printed, and a build again, until a
build prints none the table explains.

`record` writes the loop's net edit against a commit as data: for every hunk of `git diff -U0`,
the file, the line it starts at, the lines it replaces and the lines it puts. `sites` applies such
a record to a tree that has the same lines, so that the commit the loop made can be made again on
its parent with no build; a line that is not the one the record names stops the run with the file
and the line, and writes nothing.
"""

from __future__ import annotations

import json
import posixpath
import re
import subprocess
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

from ac3ns_core import NEST, ROOT_NS, Table, blocks, enclosing_path, mask

# --- the diagnostics ------------------------------------------------------------------------------

LOCATION = [
    # Clang's and MSVC's own format: file(line,col): error: ...
    re.compile(
        r"^(?P<file>.+?)\((?P<line>\d+)(?:,(?P<col>\d+))?\)\s*:\s*(?:fatal )?error"
        r"(?: (?P<code>C\d+))?\s*:\s*(?P<msg>.*)$"
    ),
    # GNU format: file:line:col: error: ...
    re.compile(
        r"^(?P<file>(?:[A-Za-z]:)?[^:\n]+):(?P<line>\d+):(?P<col>\d+):\s*(?:fatal )?error\s*:\s*"
        r"(?P<msg>.*)$"
    ),
]
# GCC in a UTF-8 locale quotes with U+2018 and U+2019, the others with the apostrophe
OPEN = "['" + chr(0x2018) + "]"
CLOSE = "['" + chr(0x2019) + "]"
NAME = rf"{OPEN}(?P<n>[^'{chr(0x2019)}]+){CLOSE}"
SCOPE = rf"{OPEN}(?P<s>[^'{chr(0x2019)}]+){CLOSE}"
TYPES = "(?:a type|a template|a class template|a template type)"
# (pattern, kind): `undeclared` has a name, `member` a name and the scope that lacks it. The longer
# messages come first: GCC's "'N' in namespace 'S' does not name a type" also ends in the shorter's.
MESSAGES = [
    (rf"no (?:member|type|template|namespace) named {NAME} in namespace {SCOPE}", "member"),
    (rf"{NAME} is not a member of {SCOPE}", "member"),
    (rf"{NAME} in namespace {SCOPE} does not name {TYPES}", "member"),
    (rf"{NAME}: is not a member of {SCOPE}", "member"),
    (rf"use of undeclared identifier {NAME}", "undeclared"),
    (rf"unknown type name {NAME}", "undeclared"),
    (rf"no template named {NAME}$", "undeclared"),
    (rf"{NAME} was not declared in this scope", "undeclared"),
    (rf"{NAME} does not name a type", "undeclared"),
    (rf"{NAME} has not been declared", "undeclared"),
    (rf"{NAME}: undeclared identifier", "undeclared"),
    (rf"{NAME}: is not a class or namespace name", "undeclared"),
    (rf"{NAME}: the symbol to the left of a '::' must be a type", "undeclared"),
    ("expected namespace name$", "usingns"),
]
PATTERNS = [(re.compile("^" + pattern), kind) for pattern, kind in MESSAGES]


@dataclass(frozen=True)
class Diag:
    file: str
    line: int
    col: int | None
    kind: str  # undeclared | member | usingns
    name: str
    scope: str = ""  # for `member`: the namespace the compiler looked in, as it printed it


def parse_log(text: str) -> list[Diag]:
    """The diagnostics of a build log the loop can act on, each once."""
    out: dict[Diag, None] = {}
    for raw in text.splitlines():
        for rx in LOCATION:
            m = rx.match(raw.rstrip())
            if not m:
                continue
            for pattern, kind in PATTERNS:
                hit = pattern.match(m.group("msg"))
                if hit:
                    col = int(m.group("col")) if m.group("col") else None
                    d = Diag(
                        m.group("file").strip(),
                        int(m.group("line")),
                        col,
                        kind,
                        hit.groupdict().get("n") or "",
                        hit.groupdict().get("s") or "",
                    )
                    out[d] = None
                    break
            break
    return list(out)


# --- from a diagnostic to an insertion ------------------------------------------------------------


@dataclass(frozen=True)
class Edit:
    file: str  # relative to the root
    offset: int  # character offset in the file's text
    text: str  # inserted there
    why: str


@dataclass(frozen=True)
class Left:
    diag: Diag
    reason: str


CHAIN_BEFORE = re.compile(r"(?P<chain>(?:::)?(?:[A-Za-z_]\w*::)*)$")
WRITTEN = re.compile(
    r"[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*"
)  # the qualified name that starts at a place
ALIAS = re.compile(r"\bnamespace\s+([A-Za-z_]\w*)\s*=")
DIRECTIVE = re.compile(r"\busing\s+namespace\s+(?:::)?" + ROOT_NS + r"(?P<q>(?:::\w+)*)\s*;")


class Loop:
    """Turns diagnostics into edits against the files of a worktree."""

    def __init__(
        self, root: Path, table: Table, path_map: list[tuple[str, str]] | None = None
    ) -> None:
        """`path_map` rewrites the start of a path in the log: a build made in a copy of the tree
        (the WSL one) names its files by the copy's path."""
        self.root = root.resolve()
        self.table = table
        self.path_map = path_map or []
        self._text: dict[str, str] = {}
        self._masked: dict[str, str] = {}
        self._blocks: dict[str, list] = {}

    def relative(self, file: str) -> str | None:
        """The path of a file the compiler named, relative to the root: one spelling for each file,
        whatever way an include directory reached it (`apps/cli/../common/x.hpp`)."""
        here = str(self.root).replace("\\", "/").rstrip("/").lower() + "/"
        f = posixpath.normpath(file.replace("\\", "/"))
        for old, new in self.path_map:
            if f.startswith(old):
                f = new + f[len(old) :]
        if f.lower().startswith(here):
            return f[len(here) :]
        return None

    def text(self, rel: str) -> str:
        if rel not in self._text:
            self._text[rel] = (self.root / rel).read_bytes().decode("utf-8")
        return self._text[rel]

    def blocks_of(self, rel: str):
        if rel not in self._blocks:
            t = self.text(rel)
            self._blocks[rel] = blocks(t, mask(t))
        return self._blocks[rel]

    def locate(self, rel: str, d: Diag) -> int | None:
        """The offset of the name the diagnostic is about: at its column, or the only place on its
        line (a compiler that prints no column)."""
        t = self.text(rel)
        lines = t.split("\n")
        if not 1 <= d.line <= len(lines):
            return None
        start = sum(len(x) + 1 for x in lines[: d.line - 1])
        row = lines[d.line - 1]
        if d.col is not None:
            before = row.encode("utf-8")[: d.col - 1].decode("utf-8", "ignore")
            if row.startswith(d.name, len(before)):
                return start + len(before)
        found = [m.start() for m in re.finditer(rf"(?<![\w:]){re.escape(d.name)}(?!\w)", row)]
        return start + found[0] if len(found) == 1 else None

    def propose(self, d: Diag) -> Edit | Left | None:
        rel = self.relative(d.file)
        if rel is None:
            return None  # a system or third-party header
        t = self.text(rel)
        if d.kind == "usingns":
            return self.usingns(rel, t, d)
        at = self.locate(rel, d)
        if at is None:
            return Left(d, "the name is not where the compiler says")
        if d.kind == "undeclared":
            return self.undeclared(rel, t, at, d)
        return self.member(rel, t, at, d)

    def aliases(self, rel: str, t: str) -> set[str]:
        """The namespace aliases the file declares: a chain that starts at one is not written with
        the namespace's own names, so a qualification put in front of it would be wrong."""
        if rel not in self._masked:
            self._masked[rel] = mask(t)
        return set(ALIAS.findall(self._masked[rel]))

    def directives(self, rel: str, t: str, at: int) -> list[tuple[tuple[str, ...], int, str]]:
        """The `using namespace iclforge[::path];` in scope at `at`, the nearest first: the path,
        the offset of the end of its line and the indentation of that line."""
        if rel not in self._masked:
            self._masked[rel] = mask(t)
        m = self._masked[rel]
        bs = self.blocks_of(rel)
        found = []
        for hit in DIRECTIVE.finditer(m):
            if hit.start() >= at:
                break
            inner = [b for b in bs if b.open < hit.start() < b.close]
            if inner and not inner[-1].open < at < inner[-1].close:
                continue  # the scope of the directive has ended
            end = t.find("\n", hit.end())
            end = len(t) if end < 0 else end - (1 if t[end - 1 : end] == "\r" else 0)
            line = t.rfind("\n", 0, hit.start()) + 1
            indent = t[line : len(t[line:]) - len(t[line:].lstrip(" \t")) + line]
            path = tuple(c for c in hit.group("q").split("::") if c)
            found.append((path, end, indent))
        return found[::-1]

    def usingns(self, rel: str, t: str, d: Diag) -> Edit | Left:
        """`using namespace iclforge::internal;` in a unit that sees only the library's half of the
        namespace: the directive is the library's now."""
        rows = t.split("\n")
        if not 1 <= d.line <= len(rows):
            return Left(d, "the line is not where the compiler says")
        start = sum(len(x) + 1 for x in rows[: d.line - 1])
        hit = DIRECTIVE.search(rows[d.line - 1])
        path = tuple(c for c in hit.group("q").split("::") if c) if hit else ()
        if hit and path and self.table.names.get(path):
            return Edit(rel, start + hit.start("q"), "::" + NEST, "usingns:" + "::".join(path))
        return Left(d, "a namespace that is not one the library declares into")

    def undeclared(self, rel: str, t: str, at: int, d: Diag) -> Edit | Left | None:
        tab = self.table
        if t[:at].rstrip(" \t").endswith("::"):
            return Left(d, "a qualified name the compiler took for an undeclared one")
        here = enclosing_path(self.blocks_of(rel), at)
        name = d.name
        eol = "\r\n" if "\r\n" in t else "\n"
        written = tuple(WRITTEN.match(t, at).group(0).split("::"))  # type: ignore[union-attr]
        if written[0] in self.aliases(rel, t):
            return Left(d, "an alias for a namespace: the alias is what to change")
        if here[:2] == (ROOT_NS, NEST):
            inside = here[2:]
            for i in range(len(inside), -1, -1):
                where = inside[:i]
                if name in tab.others.get(where, ()):
                    return Edit(
                        rel, at, ROOT_NS + "::" + "".join(c + "::" for c in where), "anchor"
                    )
            return Left(d, "no other library declares it in a namespace that encloses this one")
        if here[:1] == (ROOT_NS,):
            inside = here[1:]
            for i in range(len(inside), -1, -1):
                where = inside[:i]
                if tab.moves([*where, *written]):
                    return Edit(rel, at, NEST + "::" + "".join(c + "::" for c in where), "nest")
        for path, end, indent in self.directives(rel, t, at):
            if tab.moves([*path, *written]):
                tail = "::" + "::".join(path) if path else ""
                text = f"{eol}{indent}using namespace {ROOT_NS}::{NEST}{tail};"
                return Edit(rel, end, text, "using")
        return Left(d, "no namespace that encloses this one, and no using-directive, reaches it")

    def member(self, rel: str, t: str, at: int, d: Diag) -> Edit | Left | None:
        tab = self.table
        scope = tuple(d.scope.split("::"))
        if scope[0] != ROOT_NS or not all(re.fullmatch(r"\w+", c) for c in scope):
            return None  # a class, a template instance: what follows from an earlier error
        where = scope[1:]
        row_start = t.rfind("\n", 0, at) + 1
        chain = CHAIN_BEFORE.search(t[row_start:at]).group("chain")  # type: ignore[union-attr]
        start = at - len(chain)
        if chain and not chain.startswith("::") and chain.split("::")[0] in self.aliases(rel, t):
            return Left(d, "an alias for a namespace: the alias is what to change")
        if where[:1] == (NEST,):
            other = where[1:]
            if d.name in tab.others.get(other, ()):
                if chain.startswith("::") or chain.startswith(NEST + "::"):
                    return Left(d, "a qualification that names the library's copy on purpose")
                written = [c for c in chain.split("::") if c]
                missing = other[: max(len(other) - len(written), 0)]
                return Edit(
                    rel, start, ROOT_NS + "::" + "".join(c + "::" for c in missing), "anchor"
                )
            return Left(d, "no other library declares it in that namespace")
        if d.name in tab.names.get(where, ()) or (*where, d.name) in tab.exclusive:
            m = re.match(rf"(?P<g>::)?{ROOT_NS}::", chain)
            if m:  # fully qualified: the new namespace follows the root
                return Edit(rel, start + m.end(), NEST + "::", "nest")
            if chain.startswith("::"):
                return Left(d, "a global qualification that does not start at the root")
            written = [c for c in chain.split("::") if c]
            missing = where[: max(len(where) - len(written), 0)]
            return Edit(rel, start, NEST + "::" + "".join(c + "::" for c in missing), "nest")
        return Left(d, "the library does not declare it in that namespace")


def plan(loop: Loop, diags: list[Diag]) -> tuple[list[Edit], list[Left]]:
    edits: dict[tuple[str, int, str], Edit] = {}
    left: list[Left] = []
    for d in diags:
        r = loop.propose(d)
        if isinstance(r, Edit):
            edits[(r.file, r.offset, r.text)] = r
        elif isinstance(r, Left):
            left.append(r)
    # a directive rewritten to the library's namespace makes the one added beside it redundant
    rewritten = {
        (e.file, e.why.removeprefix("usingns:"))
        for e in edits.values()
        if e.why.startswith("usingns:")
    }
    for key, e in list(edits.items()):
        if e.why == "using" and any(
            e.file == f and e.text.endswith(f"::{NEST}::{p};") for f, p in rewritten
        ):
            del edits[key]
    # two different insertions at one place are a disagreement a person settles
    at: dict[tuple[str, int], list[Edit]] = {}
    for e in edits.values():
        at.setdefault((e.file, e.offset), []).append(e)
    kept: list[Edit] = []
    for (_file, _off), group in at.items():
        if len(group) == 1:
            kept.append(group[0])
        else:
            left.extend(
                Left(Diag(g.file, 0, None, "member", g.text), "conflicting edits") for g in group
            )
    return sorted(kept, key=lambda e: (e.file, e.offset)), left


def sane(text: str, e: Edit) -> bool:
    """Is the place an edit names still where a qualification, or a line, can be put?"""
    here = text[e.offset : e.offset + 1]
    if e.why in ("nest", "anchor") or e.why.startswith("usingns:"):
        return bool(re.match(r"[A-Za-z_:]", here))
    return here in ("", "\r", "\n")


def apply(root: Path, edits: list[Edit], dry_run: bool = False) -> tuple[int, list[str]]:
    """Insert every edit, last offset first so that the earlier ones keep their places. Returns the
    number of files written and the edits that were not made because their place is not what the
    diagnostic said (a log older than the file)."""
    by_file: dict[str, list[Edit]] = {}
    for e in edits:
        by_file.setdefault(e.file, []).append(e)
    skipped: list[str] = []
    for rel, group in by_file.items():
        path = root / rel
        text = path.read_bytes().decode("utf-8")
        for e in sorted(group, key=lambda x: x.offset, reverse=True):
            if not sane(text, e):
                skipped.append(f"{rel}@{e.offset}: {e.text!r}")
                continue
            text = text[: e.offset] + e.text + text[e.offset :]
        if not dry_run:
            path.write_bytes(text.encode("utf-8"))
    return len(by_file), skipped


# --- the record -----------------------------------------------------------------------------------

HUNK = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")


def record(root: Path, base: str) -> list[dict]:
    """The net edit of the working tree against `base`, as hunks: file, the line each starts at in
    `base`, the lines it replaces and the lines it puts."""
    diff = subprocess.run(
        ["git", "-C", str(root), "diff", "-U0", "--no-color", "--no-renames", base],
        capture_output=True,
        check=True,
    ).stdout.decode("utf-8", "replace")
    out: list[dict] = []
    file = ""
    hunk: dict | None = None
    for raw in diff.split("\n"):
        if raw.startswith("+++ "):
            file = raw[4:].removeprefix("b/")
        elif raw.startswith("--- "):
            continue
        elif raw.startswith("@@"):
            m = HUNK.match(raw)
            old_start, old_len = int(m.group(1)), int(m.group(2) or 1)  # type: ignore[union-attr]
            hunk = {"file": file, "line": old_start, "old": [], "new": []}
            if old_len == 0:
                hunk["after"] = True  # a pure insertion: after the line it names
            out.append(hunk)
        elif hunk is not None and raw.startswith("-"):
            hunk["old"].append(raw[1:].rstrip("\r"))
        elif hunk is not None and raw.startswith("+"):
            hunk["new"].append(raw[1:].rstrip("\r"))
    return out


def replay(root: Path, hunks: list[dict], dry_run: bool = False) -> list[str]:
    """Apply a record; the problems found (nothing is written when there is one)."""
    by_file: dict[str, list[dict]] = {}
    for h in hunks:
        by_file.setdefault(h["file"], []).append(h)
    problems: list[str] = []
    result: dict[str, bytes] = {}
    for rel, group in by_file.items():
        raw = (root / rel).read_bytes().decode("utf-8")
        eol = "\r\n" if "\r\n" in raw else "\n"
        lines = raw.split("\n")
        for h in sorted(group, key=lambda x: x["line"], reverse=True):
            first = h["line"] if h.get("after") else h["line"] - 1
            have = [x.rstrip("\r") for x in lines[first : first + len(h["old"])]]
            if have != h["old"]:
                problems.append(f"{rel}:{h['line']}: the line is not the one the record names")
                continue
            new = [x + ("\r" if eol == "\r\n" else "") for x in h["new"]]
            lines[first : first + len(h["old"])] = new
        result[rel] = "\n".join(lines).encode("utf-8")
    if not problems and not dry_run:
        for rel, data in result.items():
            (root / rel).write_bytes(data)
    return problems


# --- the loop -------------------------------------------------------------------------------------


def build(build_dir: str, target: str, log: Path, jobs: int) -> None:
    """`cmake --build` with -k 0: every unit that can be compiled is, whatever fails."""
    cmd = ["cmake", "--build", build_dir]
    if target != "all":
        cmd += ["--target", target]
    cmd += ["--", "-k", "0", "-j", str(jobs)]
    with open(log, "wb") as out:
        subprocess.run(cmd, stdout=out, stderr=subprocess.STDOUT, check=False)


def summary(left: list[Left]) -> Counter[str]:
    return Counter(item.reason for item in left)


def report_round(label: str, diags: int, edits: list[Edit], files: int, left: list[Left]) -> None:
    print(f"{label}: {diags} diagnostics, {len(edits)} edits in {files} files, {len(left)} left")
    for reason, count in summary(left).most_common():
        print(f"    left: {count} x {reason}")


def run_loop(
    root: Path,
    table: Table,
    build_dir: str,
    target: str,
    rounds: int,
    jobs: int,
    work: Path,
) -> int:
    """Build, apply what the diagnostics say, build again; the number of rounds that wrote."""
    wrote = 0
    for n in range(1, rounds + 1):
        log = work / f"loop-{n}.log"
        build(build_dir, target, log, jobs)
        diags = parse_log(log.read_text(encoding="utf-8", errors="replace"))
        edits, left = plan(Loop(root, table), diags)
        files, skipped = apply(root, edits)
        report_round(f"round {n}", len(diags), edits, files, left)
        for item in skipped:
            print("    skipped (the place has changed):", item)
        if not edits:
            break
        wrote += 1
    return wrote


def main_fix(
    root: Path,
    table: Table,
    logs: list[str],
    dry_run: bool,
    listing: bool,
    path_map: list[tuple[str, str]],
) -> int:
    diags: list[Diag] = []
    for f in logs:
        diags += parse_log(Path(f).read_text(encoding="utf-8", errors="replace"))
    edits, left = plan(Loop(root, table, path_map), diags)
    files, skipped = apply(root, edits, dry_run)
    report_round("fix", len(diags), edits, files, left)
    for item in skipped:
        print("    skipped (the place has changed):", item)
    if listing:
        for item in left[:200]:
            d = item.diag
            print(f"    {d.file}:{d.line}:{d.col}: {d.kind} {d.name} {d.scope}: {item.reason}")
    return 1 if skipped else 0


def main_sites(root: Path, sites: Path, dry_run: bool) -> int:
    hunks = json.loads(sites.read_text(encoding="utf-8"))["hunks"]
    problems = replay(root, hunks, dry_run)
    for p in problems:
        print("PROBLEM", p)
    verb = "would apply" if dry_run else "applied"
    print(f"{verb} {len(hunks)} hunks in {len({h['file'] for h in hunks})} files")
    return 1 if problems else 0


def main_record(root: Path, base: str, sites: Path) -> int:
    hunks = record(root, base)
    head = subprocess.run(
        ["git", "-C", str(root), "rev-parse", base], capture_output=True, text=True, check=True
    )
    data = {"base": head.stdout.strip(), "hunks": hunks}
    sites.write_text(json.dumps(data, indent=1) + "\n", encoding="utf-8", newline="\n")
    print(f"{len(hunks)} hunks in {len({h['file'] for h in hunks})} files")
    return 0


if __name__ == "__main__":
    sys.exit("run it through n1b_ac3ns.py")
