"""Names that mean something else after the move, which a compiler does not report.

The compiler loop of S6 (ac3ns_fix.py) edits where a name is no longer found. A name that is still
found, but is another declaration now, builds, and only a test notices. The move changes what an
unqualified name finds from inside the library in three ways:

  shadowed   code in a namespace both the library and another library declare into (`emdf`, `oba`,
             `render`, `internal`, `detail`, the root) found the other library's name there, because
             the two halves were one namespace. The other library's half is `iclforge::emdf` now and
             the code is in `iclforge::ac3::emdf`, which is not inside it: the name is found in a
             wider scope instead, when the library declares one of the same name there. The first
             case the tests found: `kSyncWord` in `iclforge::ac3::emdf` was the E-AC-3 container's
             0x5838 (objects) and became the AC-3 sync word 0x0B77 (the library's own, now in
             `iclforge::ac3`), so the walker looked for the wrong word and signed nothing.
  both       a name both halves of one namespace declare (overloads): the code sees the library's
             half only, as the nearer scope hides the wider one.
  spelled    `emdf::build_container` finds the objects library's function while the library's own
             `iclforge::ac3::emdf` is not declared in the translation unit, and nothing once it is:
             what a file includes first decides, and the compiler reports the second case only. The
             spelling that cannot depend on that has the root in front.

`collisions` lists the first two from the table alone, for every namespace the library has code in,
and says where each name now comes from; there are few and each is looked at. `spellings` finds the
third in a file, `anchor` writes the root in front of them and a second run changes nothing.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from ac3ns_core import (
    LIBRARY_PREFIX,
    NEST,
    ROOT_NS,
    Change,
    Context,
    Path_,
    Table,
    mask,
    path_to_key,
)

TESTS_PREFIX = "tests/ac3/"


@dataclass(frozen=True)
class Collision:
    kind: str  # shadowed | both
    scope: Path_  # the namespace the other library's name is declared in
    name: str
    now: tuple[Path_, ...]  # the scopes that hold a declaration of the name by the library
    elsewhere: bool  # ... and a declaration of another library in the root
    code_in: tuple[Path_, ...]  # the namespaces of the library whose code is affected


def other_half(table: Table, scope: Path_) -> set[str]:
    """The names other libraries declare in `scope` that the library does not."""
    return table.others.get(scope, set()) - table.declared.get(scope, set())


def collisions(table: Table) -> list[Collision]:
    """Names that resolve differently from inside the library after the move (see the module)."""
    root_others = table.others.get((), set())
    found: dict[tuple, set[Path_]] = {}
    for loc in sorted(table.known | {()}, key=lambda p: (len(p), p)):
        chain = [loc[:i] for i in range(len(loc), -1, -1)]  # the old scopes, nearest first
        for at, scope in enumerate(chain):
            for name in sorted(table.others.get(scope, ())):
                if name in table.declared.get(scope, ()):
                    if (*scope, name) in table.known:
                        continue  # a namespace both open: that is what a shared one is
                    found.setdefault(("both", scope, name, (), False), set()).add(loc)
                    continue
                now = tuple(t for t in chain[at + 1 :] if name in table.declared.get(t, ()))
                elsewhere = bool(scope) and name in root_others
                if now or elsewhere:
                    found.setdefault(("shadowed", scope, name, now, elsewhere), set()).add(loc)
    return [
        Collision(kind, scope, name, now, elsewhere, tuple(sorted(locs)))
        for (kind, scope, name, now, elsewhere), locs in sorted(found.items(), key=_order)
    ]


def _order(item: tuple) -> tuple:
    (kind, scope, name, _now, _else), _locs = item
    return (kind, scope, name)


def describe(c: Collision) -> str:
    where = ", ".join(path_to_key(p) for p in c.code_in)
    here = f"{ROOT_NS}::" + "::".join(c.scope) if c.scope else ROOT_NS
    if c.kind == "both":
        return f"both halves of {here} declare {c.name}; code in {where} sees the library's"
    now = [f"{ROOT_NS}::{NEST}" + ("::" + "::".join(p) if p else "") for p in c.now]
    if c.elsewhere:
        now.append(f"{ROOT_NS} (another library's)")
    return (
        f"{here}::{c.name} (another library's) is found from code in {where}; "
        f"now {c.name} is also in {', '.join(now)}"
    )


# --- the spellings --------------------------------------------------------------------------------


@dataclass(frozen=True)
class Spelling:
    line: int
    column: int  # 1-based, of the first character of the spelling
    scope: Path_  # the shared namespace the spelling goes through
    name: str  # the other library's name after it


def _shared_scopes(table: Table) -> list[Path_]:
    """The namespaces below the root that the library and another library declare into."""
    return sorted(
        (p for p in table.names if p and table.others.get(p)),
        key=lambda p: (-len(p), p),
    )


def spellings(text: str, table: Table) -> list[Spelling]:
    """`<namespace>::<name>` spelled with no root before it, where the name is another library's."""
    masked = mask(text)
    out: list[Spelling] = []
    for scope in _shared_scopes(table):
        theirs = other_half(table, scope)
        pattern = re.compile(r"(?<![\w:])" + "::".join(map(re.escape, scope)) + r"::([A-Za-z_]\w*)")
        for m in pattern.finditer(masked):
            name = m.group(1)
            if name not in theirs or (*scope, name) in table.known:
                continue  # not theirs, or a namespace both open: the next name decides
            line = masked.count("\n", 0, m.start()) + 1
            column = m.start() - (masked.rfind("\n", 0, m.start()) + 1) + 1
            out.append(Spelling(line, column, scope, name))
    return sorted(set(out), key=lambda s: (s.line, s.column))


def anchor(text: str, table: Table) -> tuple[str, list[Change]]:
    """Write the root in front of each spelling `spellings` finds."""
    found = spellings(text, table)
    if not found:
        return text, []
    starts = [0]
    for m in re.finditer("\n", text):
        starts.append(m.end())
    ctx = Context(text)
    changes: list[Change] = []
    pieces: list[str] = []
    last = 0
    for s in found:
        at = starts[s.line - 1] + s.column - 1
        pieces += [text[last:at], ROOT_NS + "::"]
        last = at
        spelled = "::".join(s.scope) + "::" + s.name
        changes.append(Change("anchor", s.line, ctx.kind(at), spelled, f"{ROOT_NS}::{spelled}"))
    pieces.append(text[last:])
    return "".join(pieces), changes


# --- the phase ------------------------------------------------------------------------------------


def reads(path: str) -> bool:
    """The library's own files and its tests: the code that is written from inside it."""
    return path.startswith((LIBRARY_PREFIX, TESTS_PREFIX)) and path.endswith(
        (".cpp", ".hpp", ".h", ".inl", ".ipp", ".hpp.in", ".h.in")
    )


def main_shadows(root: Path, files: list[str], table: Table, apply: bool, dry_run: bool) -> int:
    found = collisions(table)
    print(f"{len(found)} names the move could rebind (collisions):")
    for c in found:
        print("  " + describe(c))
    total = 0
    touched = 0
    for f in files:
        if not reads(f):
            continue
        try:
            text = (root / f).read_bytes().decode("utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        new, changes = anchor(text, table)
        if not changes:
            continue
        touched += 1
        total += len(changes)
        for c in changes:
            print(f"  {f}:{c.line}: {c.context}: {c.before} -> {c.after}")
        if apply and not dry_run:
            (root / f).write_bytes(new.encode("utf-8"))
    verb = "anchored" if apply and not dry_run else "would anchor"
    print(f"{verb} {total} spellings in {touched} files")
    return 0
