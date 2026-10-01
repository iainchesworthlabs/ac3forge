"""What the S6 scripts share: the table of the AC-3 library's names, a reader of C++ text that
knows a comment, a string and a namespace block from code, and the two text passes.

`Table` is `ac3ns_symbols.json` (made by `ac3ns_census.py`; see n1b_ac3ns.py for what it says).
`mask` blanks the inside of every comment and literal, offsets and newlines kept. `blocks` finds the
brace blocks of a file and the namespace each one opens, by its absolute path, so that
`enclosing_path` can say which namespace a position of the file is in.
"""

from __future__ import annotations

import bisect
import json
import re
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_TABLE = HERE / "ac3ns_symbols.json"

ROOT_NS = "iclforge"
NEST = "ac3"
LIBRARY_PREFIX = "src/ac3/"  # the files that are the library's own
ROOT_KEY = "<root>"  # the table's key for the root namespace itself

Path_ = tuple[
    str, ...
]  # a namespace path relative to the root: () is `iclforge`, ("meta",) is `iclforge::meta`


# --- the table ------------------------------------------------------------------------------------


@dataclass
class Table:
    """What the AC-3 library declares, namespace by namespace (see the module docstring)."""

    exclusive: set[Path_] = field(default_factory=set)
    names: dict[Path_, set[str]] = field(
        default_factory=dict
    )  # a shared namespace: the names that move
    others: dict[Path_, set[str]] = field(
        default_factory=dict
    )  # a shared namespace: the names that stay
    known: set[Path_] = field(default_factory=set)  # every namespace the table lists
    followers: dict[str, set[Path_]] = field(
        default_factory=dict
    )  # file outside the library -> paths
    declared: dict[Path_, set[str]] = field(
        default_factory=dict
    )  # every namespace: all the names the library declares in it, the namespaces it opens included

    @classmethod
    def from_json(cls, data: dict) -> Table:
        t = cls()
        for key, row in data["namespaces"].items():
            path = key_to_path(key)
            t.known.add(path)
            t.declared.setdefault(path, set()).update(row["names"])
            if path:  # a namespace the library opens is a name in the one around it
                t.declared.setdefault(path[:-1], set()).add(path[-1])
            # the root holds every library's namespace, so it is never the library's outright
            if row["owner"] == "exclusive" and path:
                t.exclusive.add(path)
            else:
                t.names[path] = set(row["names"])
                t.others[path] = {n for names in row.get("others", {}).values() for n in names}
        for f, paths in data.get("followers", {}).items():
            t.followers[f] = {key_to_path(p) for p in paths}
        return t

    @classmethod
    def load(cls, path: Path = DEFAULT_TABLE) -> Table:
        return cls.from_json(json.loads(path.read_text(encoding="utf-8")))

    def moves(self, chain: list[str]) -> bool:
        """Does `iclforge::<chain>` name something the library declares?

        `chain` is the qualified name after `iclforge::`, split at `::`. Walk it one name at a
        time: a namespace the library owns outright takes everything below it; in a shared one the
        name decides; a namespace the table lists but does not own is walked into."""
        here: Path_ = ()
        for name in chain:
            nxt = (*here, name)
            if nxt in self.exclusive:
                return True
            if name in self.names.get(here, ()):
                return True
            if nxt not in self.known:
                return False
            here = nxt
        return False

    def owns_path(self, path: Path_) -> bool:
        """Is a `namespace iclforge::<path>` block, in a file outside the library, the library's?"""
        return path in self.exclusive


def key_to_path(key: str) -> Path_:
    return () if key == ROOT_KEY else tuple(key.split("::"))


def path_to_key(path: Path_) -> str:
    return "::".join(path) if path else ROOT_KEY


# --- what is code, what is a comment, what is a string --------------------------------------------

_LEXEME = re.compile(
    r"""
      (?P<comment>//(?:[^\\\n]|\\\r?\n|\\)*|/\*.*?\*/)
    | (?P<string>(?:u8|u|U|L)?R"(?P<delim>[^()\\\s"]{0,16})\(.*?\)(?P=delim)"
                |(?:u8|u|U|L)?"(?:\\.|[^"\\\n])*")
    | (?P<char>(?<![\w'])(?:u8|u|U|L)?'(?:\\.|[^'\\\n])+')
    """,
    re.S | re.X,
)


def lexemes(text: str) -> list[tuple[int, int, str]]:
    """The comments and the string and character literals of C++ text: (start, end, kind), kind
    being 'comment' or 'string'. A digit separator (`1'000`) is not a character literal."""
    out: list[tuple[int, int, str]] = []
    for m in _LEXEME.finditer(text):
        out.append((m.start(), m.end(), "comment" if m.lastgroup == "comment" else "string"))
    return out


def _blank(s: str) -> str:
    return re.sub(r"[^\r\n]", " ", s)


def mask(text: str) -> str:
    """`text` with the inside of every comment and literal blanked, offsets and newlines kept."""
    pieces: list[str] = []
    last = 0
    for start, end, kind in lexemes(text):
        pieces.append(text[last:start])
        if kind == "string":  # the quotes stay: `"   "` is still a literal
            q = min(i for i in (text.find('"', start, end), text.find("'", start, end)) if i >= 0)
            pieces.append(text[start : q + 1] + _blank(text[q + 1 : end - 1]) + text[end - 1])
        else:
            pieces.append(_blank(text[start:end]))
        last = end
    pieces.append(text[last:])
    return "".join(pieces)


class Context:
    """Where a position of a file is: code, a comment or a string."""

    def __init__(self, text: str) -> None:
        self._spans = lexemes(text)
        self._starts = [s for s, _, _ in self._spans]

    def kind(self, pos: int) -> str:
        i = bisect.bisect_right(self._starts, pos) - 1
        if i >= 0 and self._spans[i][0] <= pos < self._spans[i][1]:
            return self._spans[i][2]
        return "code"


# --- namespace blocks -----------------------------------------------------------------------------

_NS_HEAD = re.compile(
    r"\s*(?:inline\s+)?(?:\[\[[^\]]*\]\]\s*)*"
    r"((?:[A-Za-z_]\w*\s*::\s*)*[A-Za-z_]\w*)?\s*([{=])"
)
_TOKEN = re.compile(r"[{}]|\bnamespace\b")


@dataclass
class Block:
    comps: tuple[str, ...]  # as the head spells it: `namespace a::b {` is ("a", "b"); `{` is ()
    path: Path_  # the absolute path of the namespace the block opens, anonymous ones included
    named: bool  # a `namespace` block (not a function or class body)
    open: int  # offset of the `{`
    close: int  # offset of the matching `}` (len(text) when it is not found)
    head: int  # offset of the `namespace` keyword (-1 for a plain brace)


def blocks(text: str, masked: str | None = None) -> list[Block]:
    """Every brace block of `text` in the order it opens, with the namespace it opens if any."""
    m = masked if masked is not None else mask(text)
    pending: dict[int, tuple[tuple[str, ...], int]] = {}
    out: list[Block] = []
    stack: list[Block] = []
    for t in _TOKEN.finditer(m):
        if t.group() == "namespace":
            h = _NS_HEAD.match(m, t.end())
            if h and h.group(2) == "{":
                name = h.group(1)
                comps = tuple(re.sub(r"\s+", "", name).split("::")) if name else ()
                pending[h.end() - 1] = (comps, t.start())
        elif t.group() == "{":
            comps, head = pending.get(t.start(), ((), -1))
            named = t.start() in pending
            parent = stack[-1].path if stack else ()
            blk = Block(comps, (*parent, *comps), named, t.start(), len(m), head)
            out.append(blk)
            stack.append(blk)
        elif stack:
            stack.pop().close = t.start()
    return out


def enclosing_path(bs: list[Block], pos: int) -> Path_:
    """The absolute namespace path at `pos`: the path of the innermost block that holds it. A
    function or class body inherits its parent's path, which is how `blocks` made it."""
    best: Path_ = ()
    for b in bs:
        if b.open < pos < b.close:
            best = b.path
        elif b.open > pos:
            break
    return best


# --- the two text passes --------------------------------------------------------------------------

_CHAIN = re.compile(
    r"(?<![\w:])(?P<global>::)?" + ROOT_NS + r"::(?P<chain>[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)"
)
# `namespace iclforge...{` at the start of a line, and its closing comment
_DECL_HEAD = re.compile(
    r"^(?P<pre>[ \t]*(?:inline[ \t]+)?namespace[ \t]+)"
    + ROOT_NS
    + r"(?P<rest>(?:::\w+)*)(?P<tail>[ \t]*\{)",
    re.M,
)
_DECL_CLOSE = re.compile(
    r"(?P<pre>\}[ \t]*//[ \t]*namespace[ \t]+)"
    + ROOT_NS
    + r"(?P<rest>(?:::\w+)*)(?P<tail>[ \t]*(?:\r?$))",
    re.M,
)


@dataclass
class Change:
    kind: str  # decl | use
    line: int
    context: str  # code | comment | string
    before: str
    after: str


def declare(text: str, table: Table, path: str, own: bool) -> tuple[str, list[Change]]:
    """Open every `namespace iclforge...` block of the file under `iclforge::ac3`. `own` is a file
    of the library (all of them move); otherwise only the paths the table lists for the file."""
    follow = table.followers.get(path, set())
    if not own and not follow:
        return text, []
    changes: list[Change] = []

    def moved(rest: str) -> bool:
        if rest == f"::{NEST}" or rest.startswith(f"::{NEST}::"):
            return False  # already under iclforge::ac3
        if own:
            return True
        parts = tuple(p for p in rest.split("::") if p)
        return any(parts[: len(f)] == f for f in follow)

    def rewrite(kind: str, m: re.Match[str]) -> str:
        if not moved(m.group("rest")):
            return m.group(0)
        line = text.count("\n", 0, m.start()) + 1
        new = f"{m.group('pre')}{ROOT_NS}::{NEST}{m.group('rest')}{m.group('tail')}"
        changes.append(Change("decl", line, kind, m.group(0).strip(), new.strip()))
        return new

    text = _DECL_HEAD.sub(lambda m: rewrite("code", m), text)
    text = _DECL_CLOSE.sub(lambda m: rewrite("comment", m), text)
    return text, changes


def qualify(text: str, table: Table) -> tuple[str, list[Change]]:
    """Write `iclforge::ac3::` where a qualified name is one the library declares."""
    changes: list[Change] = []
    ctx: Context | None = None
    pieces: list[str] = []
    last = 0
    for m in _CHAIN.finditer(text):
        if not table.moves(m.group("chain").split("::")):
            continue
        if ctx is None:
            ctx = Context(text)
        at = m.start("chain")
        pieces += [text[last:at], NEST + "::"]
        last = at
        line = text.count("\n", 0, m.start()) + 1
        before = f"{ROOT_NS}::{m.group('chain')}"
        changes.append(
            Change(
                "use", line, ctx.kind(m.start()), before, f"{ROOT_NS}::{NEST}::{m.group('chain')}"
            )
        )
    pieces.append(text[last:])
    return "".join(pieces), changes
