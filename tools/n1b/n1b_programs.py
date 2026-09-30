"""The program names, stage N1A of the plan: `ac3cli` becomes `forge`, `ac3gui` `forge-gui`,
`ac3hearth` `hearth`, `ac3crucible` `crucible`, and everything they register follows.

    n1b_programs.py --root <worktree> --phase mv|text|pages|all [--dry-run] [--report <file>]
                    [--json <file>] [--table]

Three phases. `mv` is the `git mv` of every tracked file that is named for a program or for the
family's brand in a program's tree (the translation catalogues, the desktop entries, the icons, the
Kotlin sources of the Android package): the renames are staged and nothing else is, so that a
`git commit` with nothing added is the commit of the renames alone (all R100). `text` is the
rewrite of the text of every file, which `git add -A` then commits. `pages` names the moved files
by their new paths in the pages (`check_doc_paths.py` asks that a path a page names exists), and
touches nothing else there: the pages are stage S5's. `all` is the three, in that order, and the
commit of the renames is made before the rest of its run is added.

The one table is PROGRAMS: for each program the name it takes (`forge-gui`), the stem it takes when
it is joined to a word by an underscore (`forge_gui_qmltests`), the C++ namespace it becomes where
it is one (`forge_gui`; never a bare `forge` or `hearth`, which are aliases and sub-namespaces
elsewhere), and the stem of the CMake variables and environment variables (`ICLFORGE_GUI_*`). What
the table does not say is in the other tables of this file, each a named decision:

  EXPLICIT    names Qt and CMake derive from a target's name (`forge-gui_lupdate`), and the ones
              that have to be told (`AC3GUI_TEST_AC3CLI`).
  LITERALS    a fragment that is one decision by itself (the man page's title).
  MODULES     the QML module URIs (`Ac3ForgeHearth` becomes `Hearth`), which both the build and
              every `import` read.
  brand_replacement()
              what the identifier pass of S4 kept for N1A (`n1a-*` in its report) and this pass
              renames: the packages and icons named for a program, the Android package and its JNI
              names, the display name `AC3Forge` by context ("AC3Forge Hearth" becomes "Hearth",
              the family "ICL Forge", the GUI "Forge"), the organisation a program's settings are
              stored under.
  DRIVER_*    the Windows driver's identity, which is installed on machines and matched by name.

The place decides where the old name alone does not: a namespace is a different word from an
executable (`ac3cli::` is `forge_cli::`, `ac3cli encode` is `forge encode`), and so is a name the
language needs as an identifier, where a hyphen is not allowed (a Python parameter or attribute
that was `ac3tests` is `iclforge_tests`; the flag `--ac3tests` that sets it is `--iclforge-tests`,
which argparse reads as that attribute). A Python source is read by its tokens for that, from
Python 3.12 on, where the name inside an f-string's field is a token too, and a source that parsed
before the pass and would not after it is not written: the run fails and lists it.

`--report` lists every place the brand was decided, renamed or kept; `--json` writes the old-to-new
table and the moves as data, for the documentation pass; `--table` prints the table. Pages (`.md`),
the history, the byte-exact trees and this migration's own scripts are not read, as in
n1b_idents.py. A second run changes nothing.
"""

from __future__ import annotations

import ast
import importlib
import io
import json
import re
import subprocess
import sys
import tokenize
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

import n1b_idents as idents
from n1b_lib import Repo, base_parser

# --- the table ------------------------------------------------------------------------------------


@dataclass(frozen=True)
class Program:
    old: str
    name: str  # the program, its executable and its CMake target
    family: str  # cli | gui | hearth | crucible | internal | android | driver
    ns: str | None = None  # the C++ namespace it becomes, where it is one
    var: str | None = None  # the stem of its CMake and environment variables

    @property
    def ident(self) -> str:
        """The name as the stem of an identifier that joins it to a word with an underscore."""
        return self.name.replace("-", "_")


PROGRAMS: tuple[Program, ...] = (
    Program("ac3cli", "forge", "cli", ns="forge_cli", var="CLI"),
    Program("ac3gui", "forge-gui", "gui", ns="forge_gui", var="GUI"),
    Program("ac3hearth", "hearth", "hearth", var="HEARTH"),
    Program("ac3hearth-render", "hearth-render", "hearth"),
    Program("ac3hearth-testsink", "hearth-testsink", "hearth"),
    Program("ac3hearth-testserver", "hearth-testserver", "hearth"),
    Program("ac3crucible", "crucible", "crucible", var="CRUCIBLE"),
    Program("ac3crucible-run", "crucible-run", "crucible"),
    # the internal programs (planning/layout.md decision 13)
    Program("ac3tests", "iclforge-tests", "internal"),
    Program("ac3probe", "iclforge-probe", "internal", ns="iclforge_probe"),
    Program("ac3perf", "iclforge-perf", "internal"),
    Program("ac3bench", "iclforge-bench", "internal"),
    Program("ac3membench", "iclforge-membench", "internal"),
    Program("ac3kernelbench", "iclforge-kernelbench", "internal"),
    Program("ac3fuzz", "iclforge-fuzz", "internal", ns="iclforge_fuzz"),
    Program("ac3test", "iclforge-test", "internal", ns="iclforge_test"),
    # the Android app keeps the word `shield`; the driver's C++ namespace is the only name of its
    # own this pass renames (its installed identity is kept, see DRIVER_TREES)
    Program("ac3shield", "shield", "android", ns="shield"),
    Program("ac3nullsink", "iclforge-nullsink", "driver", ns="iclforge_nullsink"),
)
BY_OLD = {p.old: p for p in PROGRAMS}

# Crucible's CMake variables were `AC3DESK_*` (the desktop demo's name) as well as `AC3CRUCIBLE_*`.
VAR_STEMS = {
    "CLI": "CLI",
    "GUI": "GUI",
    "HEARTH": "HEARTH",
    "CRUCIBLE": "CRUCIBLE",
    "DESK": "CRUCIBLE",
}

# Names Qt and CMake derive from a target's name are the target's own spelling: `qt_add_translations
# (forge-gui ...)` makes `forge-gui_lupdate`, where the same words joined by this pass elsewhere
# would be `forge_gui_...`. Whole identifier runs, exact.
EXPLICIT: dict[str, str] = {
    "ac3gui_autogen": "forge-gui_autogen",
    "ac3gui_lupdate": "forge-gui_lupdate",
    "ac3gui_lrelease": "forge-gui_lrelease",
    # a variable that names the CLI inside another program's variable
    "AC3GUI_TEST_AC3CLI": "ICLFORGE_GUI_TEST_CLI",
    # a static library file: lib + the target's name
    "libac3crucible_engine": "libcrucible_engine",
}

# A fragment that is one decision by itself, by the path it is in (a prefix; "" is every file).
# Applied before anything else, in order.
LITERALS: tuple[tuple[str, str, str], ...] = (
    # the man page's title is the command's name in capitals
    ("", ".TH AC3CLI 1", ".TH FORGE 1"),
    # the translation catalogues are named for the program with an underscore (`forge_gui_fr.qm`),
    # so the base name the loader is given is that spelling, not the program's (`forge-gui`)
    ("apps/gui/language_manager.hpp", '"ac3gui"', '"forge_gui"'),
    # the GUI's own name as Qt has it (the organisation is the family's, `iclforge`)
    (
        "apps/gui/main.cpp",
        'setApplicationName(QStringLiteral("ac3forge"))',
        'setApplicationName(QStringLiteral("forge-gui"))',
    ),
    # words that name the GUI window or a page's title, in a comment or a page's head
    ("apps/gui/qml/PreferencesDialog.qml", "When ac3forge opens", "When Forge opens"),
    # S4 renamed the brand in this translation as a compound of a file name (`ac3forge-Demo...`)
    # while the source's `ac3forge` was left for this stage: the source now says "ICL Forge", and
    # the catalogue's own check wants the family's name in the translation, hyphenated in German
    (
        "apps/crucible/translations/crucible_de.ts",
        "Eine iclforge-Demonstration",
        "Eine ICL-Forge-Demonstration",
    ),
    (
        "apps/crucible/ui/qml/AboutDialog.qml",
        "as the ac3forge GUI's About",
        "as the Forge GUI's About",
    ),
    (
        "esp-idf/iclforge/ui/iclforge_ui.html",
        "<title>ac3forge player</title>",
        "<title>iclforge player</title>",
    ),
)

# --- QML modules ----------------------------------------------------------------------------------

MODULES: dict[str, str] = {
    "Ac3ForgeCrucibleLanguage": "CrucibleLanguage",
    "Ac3ForgeCrucibleTest": "CrucibleTest",
    "Ac3ForgeCrucible": "Crucible",
    "Ac3ForgeHearthLanguage": "HearthLanguage",
    "Ac3ForgeHearthTest": "HearthTest",
    "Ac3ForgeHearth": "Hearth",
    "Ac3Forge": "ForgeGui",
}

# --- what the driver keeps ------------------------------------------------------------------------

# The Windows driver `Ac3ForgeNullSink` is installed on machines: its hardware id, service name,
# INF, SYS and CAT file names and everything Crucible finds it by stay exactly as they are. These
# are the tokens of that identity, and the .NET namespace `Ac3Forge` that the driver's scripts
# compile for themselves (only those scripts read it). Every other word of the driver's trees is
# renamed as anywhere else: a display string of the INF, the notices, the programs the scripts
# deploy.
DRIVER_TREES = ("apps/windows/driver/", "apps/windows/driver-vm/")
_DRIVER_TOKEN = re.compile(r"(?i)nullsink|Wdk")


def is_driver_token(tok: str) -> bool:
    return bool(_DRIVER_TOKEN.search(tok))


# --- which files are read -------------------------------------------------------------------------

# A file the hand-written commit of this stage writes with the OLD names on purpose: the settings
# migration reads the store the programs used before, so it names `ac3forge` where it must.
FORMER_NAME_FILES = (
    "apps/gui/settings_migration.hpp",
    "apps/gui/settings_migration.cpp",
    "tests/gui/test_settings_migration.cpp",
)


def in_scope(path: str) -> bool:
    if path in FORMER_NAME_FILES:
        return False
    return idents.in_scope(path)


# --- the renames of program names -----------------------------------------------------------------

_NAMES = sorted((p.old for p in PROGRAMS), key=len, reverse=True)
# The name must not sit against a letter or digit, except after a troff font escape (`\fBac3cli`)
# or a C escape (`\nac3cli`). `lib` in front is a static library's file name.
_PROGRAM_RX = re.compile(
    r"(?:(?<![A-Za-z0-9])|(?<=\\f[BIRP])|(?<=\\[ntr]))"
    r"(?P<lib>lib)?(?P<name>" + "|".join(map(re.escape, _NAMES)) + r")(?![A-Za-z0-9])"
)
_EXPLICIT_RX = re.compile(
    r"(?<![A-Za-z0-9_])(?:"
    + "|".join(sorted(map(re.escape, EXPLICIT), key=len, reverse=True))
    + r")"
    r"(?![A-Za-z0-9_])"
)
# `AC3GUI_*` and `AC3_GUI_*` (the GUI's CMake variables have both spellings)
_VAR_RX = re.compile(r"(?<![A-Za-z0-9])AC3_?(?P<var>CLI|GUI|HEARTH|CRUCIBLE|DESK)(?![A-Za-z0-9])")
_NAMESPACE_LEFT = re.compile(r"\bnamespace[ \t]+(?:\w+[ \t]*=[ \t]*)?$")
# the article before a name that now begins with a consonant sound
_AN_RX = re.compile(
    r"\b([Aa]n)"
    r"( (?:forge|forge-gui|hearth|hearth-\w+|crucible|crucible-run|shield)(?![A-Za-z0-9_]))"
)


def _program_new(m: re.Match, line: str) -> str:
    prog = BY_OLD[m.group("name")]
    lib = m.group("lib") or ""
    right = line[m.end() :]
    left = line[: m.start()]
    if not lib and prog.ns and (right.startswith("::") or _NAMESPACE_LEFT.search(left)):
        return prog.ns
    if right.startswith("_"):
        return lib + prog.ident
    return lib + prog.name


def rename_programs(line: str, counts: Counter | None = None, hits: list | None = None) -> str:
    """The program names of one line of text."""

    def explicit(m: re.Match) -> str:
        if counts is not None:
            counts["explicit"] += 1
        if hits is not None:
            hits.append(("renamed", "explicit", m.group(0), EXPLICIT[m.group(0)]))
        return EXPLICIT[m.group(0)]

    original = line
    line = _EXPLICIT_RX.sub(explicit, line)
    source = line

    def program(m: re.Match) -> str:
        new = _program_new(m, source)
        if counts is not None:
            counts[BY_OLD[m.group("name")].family] += 1
        if hits is not None:
            hits.append(("renamed", BY_OLD[m.group("name")].family, m.group(0), new))
        return new

    line = _PROGRAM_RX.sub(program, line)
    source = line

    def variable(m: re.Match) -> str:
        stem = VAR_STEMS[m.group("var")]
        if counts is not None:
            counts["variable"] += 1
        embedded = m.start() > 0 and source[m.start() - 1] == "_"
        new = stem if embedded else f"ICLFORGE_{stem}"
        if hits is not None:
            hits.append(("renamed", "variable", m.group(0), new))
        return new

    line = _VAR_RX.sub(variable, line)
    # "an ac3cli binary" was right for the old name (a vowel sound) and is not for `forge`
    if line != original:
        line = _AN_RX.sub(lambda m: ("A" if m.group(1) == "An" else "a") + m.group(2), line)
    return line


# --- the brand, where the program owns it ---------------------------------------------------------

_CASE = {"ac3forge": "iclforge", "AC3FORGE": "ICLFORGE", "Ac3Forge": "IclForge"}

# `ac3forge` followed by these is a package, an icon, the Android package or a file of the family's
# mark: the family's own name, as in every name of a file (n1b_idents.py's n1a-token).
_FAMILY_NAME_RIGHT = re.compile(
    r"^(?:[./]shield|_shield|-shield|_jni|-crucible|-hearth|-nullsink|_crucible_sink|"
    r"-32\.png|-256\.png|\.ico|\.icns|-icon\.svg|-tests(?![\w-]))"
)
_FAMILY_NAME_LEFT = re.compile(r"(?:com[./]|org\.)$")
_LIBRARY_RIGHT = re.compile(r"^ library\b")


def _kind_of_file(path: str) -> str:
    """qml | kt | text (a file whose words people read) | code"""
    if path.endswith(".qml"):
        return "qml"
    if path.endswith(".kt"):
        return "kt"
    # the INF and the version resource of the driver carry the provider and the file description
    if path.endswith((".html", ".ts", ".metainfo.xml", ".inx", ".rc")):
        return "text"
    if path.endswith(".txt") and "/notices/fragments/" in path:
        return "text"
    return "code"


def is_display(path: str, line: str, start: int) -> bool:
    """The occurrence at `start` is in text a person reads, not in code or a comment."""
    kind = _kind_of_file(path)
    stripped = line.strip()
    if kind == "text":
        return not re.match(r"\s*<(?:id|binary|launchable)\b", line)
    if kind == "qml":
        return "qsTr(" in line or stripped.startswith(('+ "', '"'))
    if kind == "kt":
        return line[:start].count('"') % 2 == 1
    return False


def in_gui_window(path: str) -> bool:
    """A file whose words are the GUI window's own: its QML and its translation catalogues. The
    window says "Forge" (the pair of the CLI and the GUI); the rest of the family's words are
    "ICL Forge"."""
    if path.startswith(("apps/gui/", "tests/gui/")) and path.endswith(".qml"):
        return True
    return bool(re.search(r"/(?:forge_gui|ac3gui)_\w+\.ts$", path))


def brand_replacement(
    path: str, line: str, start: int, end: int, hint: str | None = None
) -> tuple[str, int] | None:
    """What the brand at line[start:end] becomes, and where the replaced text ends; None keeps it.
    Only called for the occurrences the identifier pass kept for N1A. `hint` is what the bare word
    became in the source string a translation translates: a translation names the thing the source
    names (the library `iclforge`, not the family's display name), in whatever word order."""
    word = line[start:end]
    tok, left, right = idents._run(line, start, end)

    if is_driver_token(tok):
        return None
    if word == "Ac3Forge" and path.startswith(DRIVER_TREES):
        return None  # the .NET namespace the driver's scripts compile for themselves
    if word == "Ac3Forge" and tok in MODULES:
        return MODULES[tok], start + len(tok)
    if word == "AC3Forge":
        for prog in ("Crucible", "Hearth"):
            if right.startswith(f" {prog}") and not right[1 + len(prog) :][:1].isalnum():
                return prog, end + 1 + len(prog)
        if right.startswith(".Stream"):
            return "IclForge", end  # the file type's ProgID
        if right.startswith("-"):
            return "ICL-Forge", end  # a compound in a language that hyphenates them
        return "ICL Forge", end
    if word == "AC3FORGE":
        return "FORGE", end
    if word == "ac3forge":
        if _FAMILY_NAME_RIGHT.match(right) or _FAMILY_NAME_LEFT.search(left) or tok != word:
            return _CASE[word], end
        if hint is not None:
            return hint, end
        if _LIBRARY_RIGHT.match(right):
            return "iclforge", end
        if is_display(path, line, start):
            return ("Forge" if in_gui_window(path) else "ICL Forge"), end
        return "iclforge", end
    if word in _CASE:
        return _CASE[word], end
    return None


# --- a name the language needs as an identifier ---------------------------------------------------


def python_identifiers(
    path: str, text: str, hits: list | None = None, counts: Counter | None = None
) -> str:
    """The program names that are names of a Python source (its NAME tokens: a string, a comment
    and a docstring are not) in the stem form, which a hyphen cannot be part of: `run(ac3tests)`
    and `arguments.ac3tests` are `run(iclforge_tests)` and `arguments.iclforge_tests`. The same
    word in a string or a comment is the program's name, which the line rules give its own form."""
    if not path.endswith(".py") or not _PROGRAM_RX.search(text):
        return text
    try:
        tokens = list(tokenize.generate_tokens(io.StringIO(text).readline))
    except (tokenize.TokenError, IndentationError, SyntaxError):
        return text  # not Python the pass can read: its words are text, as in any other file
    rows = text.split("\n")
    for tok in sorted(tokens, key=lambda t: t.start, reverse=True):
        if tok.type != tokenize.NAME or tok.string not in BY_OLD:
            continue
        prog = BY_OLD[tok.string]
        (row, start), (_, end) = tok.start, tok.end
        if rows[row - 1][start:end] != tok.string:
            continue  # a column the tokenizer counts differently: parses() finds what this leaves
        rows[row - 1] = rows[row - 1][:start] + prog.ident + rows[row - 1][end:]
        if counts is not None:
            counts[prog.family] += 1
        if hits is not None:
            hits.append(("renamed", prog.family, tok.string, prog.ident))
    return "\n".join(rows)


def parses(text: str) -> bool:
    try:
        ast.parse(text)
    except (SyntaxError, ValueError, RecursionError):
        return False
    return True


def breaks_python(path: str, before: str, after: str) -> bool:
    """A Python source that parsed before the pass and does not after it: a name the language
    needs, renamed to one it cannot have, is found here and not by a person on the first run."""
    return path.endswith(".py") and after != before and parses(before) and not parses(after)


# --- one file -------------------------------------------------------------------------------------


def source_form(path: str, line: str) -> str | None:
    """What the bare word `ac3forge` becomes in the source string on `line` of a catalogue, when it
    becomes one thing there."""
    forms = set()
    for m in idents.BRAND.finditer(line):
        if m.group(0) != "ac3forge":
            continue
        if idents.RULES[idents.decide(path, line, m.start(), m.end())].group != "n1a":
            continue
        done = brand_replacement(path, line, m.start(), m.end())
        if done is not None:
            forms.add(done[0])
    return forms.pop() if len(forms) == 1 else None


def transform_line(
    path: str,
    line: str,
    hits: list | None = None,
    counts: Counter | None = None,
    hint: str | None = None,
) -> str:
    # a line that says what the old names were, on purpose (n1b_idents.FORMER_NAME_LINES)
    if any(fragment in line for fragment in idents.FORMER_NAME_LINES.get(path, ())):
        return line
    for prefix, old, new in LITERALS:
        if path.startswith(prefix) and old in line:
            line = line.replace(old, new)
    pieces: list[str] = []
    cursor = 0
    for m in idents.BRAND.finditer(line):
        start, end = m.span()
        if start < cursor:
            continue
        rule = idents.decide(path, line, start, end)
        if idents.RULES[rule].group != "n1a":
            continue
        done = brand_replacement(path, line, start, end, hint)
        if done is None:
            if hits is not None:
                hits.append(("kept", rule, line[start:end], line.strip()[:120]))
            continue
        replacement, stop = done
        if hits is not None:
            hits.append(("renamed", rule, line[start:stop], replacement))
        if counts is not None:
            counts["brand:" + rule] += 1
        pieces.append(line[cursor:start])
        pieces.append(replacement)
        cursor = stop
    pieces.append(line[cursor:])
    return rename_programs("".join(pieces), counts, hits)


def transform(path: str, text: str, hits: list | None = None, counts: Counter | None = None) -> str:
    text = python_identifiers(path, text, hits, counts)
    if not path.endswith(".ts"):
        return "\n".join(transform_line(path, line, hits, counts) for line in text.split("\n"))
    # A Qt catalogue: a translation says what its source says (the `<source>` line of a message
    # comes before its `<translation>`), so the bare word in it follows the source's.
    out: list[str] = []
    hint: str | None = None
    for line in text.split("\n"):
        if "<source>" in line:
            hint = source_form(path, line)
            out.append(transform_line(path, line, hits, counts))
        else:
            out.append(transform_line(path, line, hits, counts, hint))
        if "</message>" in line:
            hint = None
    return "\n".join(out)


# --- the files that move --------------------------------------------------------------------------


def moved_path(path: str) -> str | None:
    """The new path of a tracked file named for a program, or for the brand in a program's tree,
    or None."""
    if not in_scope(path) or path.startswith(DRIVER_TREES):
        return None

    def brand(m: re.Match) -> str:
        return _CASE.get(m.group(0), m.group(0))

    new = idents.BRAND.sub(brand, path)
    new = rename_programs(new)
    return None if new == path else new


def compute_moves(repo: Repo) -> dict[str, str]:
    moves = {}
    for f in repo.files:
        new = moved_path(f)
        if new:
            moves[f] = new
    return moves


def remove_emptied_directories(root: Path, moves: dict[str, str]) -> int:
    """`git mv` leaves the directories it emptied, and an on-disk check of a path
    (check_doc_paths.py) finds them. Bottom-up, only those that hold nothing."""
    dirs = {Path(old).parent for old in moves}
    removed = 0
    for d in sorted(dirs, key=lambda p: len(p.parts), reverse=True):
        for parent in (d, *d.parents):
            full = root / parent
            if parent == Path(".") or not full.is_dir() or any(full.iterdir()):
                break
            full.rmdir()
            removed += 1
    return removed


def do_moves(root: Path, moves: dict[str, str]) -> None:
    n = 0
    for old, new in sorted(moves.items()):
        if not (root / old).exists() and (root / new).exists():
            continue
        (root / new).parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "-C", str(root), "mv", old, new], check=True, capture_output=True)
        n += 1
    removed = remove_emptied_directories(root, moves)
    print(f"moved {n} files, removed {removed} emptied directories")


# --- the pages ------------------------------------------------------------------------------------

# The pages (S5's) name files by their repository path, and check_doc_paths.py asks that each path a
# page names exists. After the move the old path does not: this writes the new one, in the pages the
# check reads, and changes nothing else in them.
PAGES_KEEP_OLD = (
    "CHANGELOG.md",  # released entries are immutable
    "planning/layout.md",
    "planning/layout-inventory.md",
    "docs/assets/data/",  # the generated catalogue
)


def unchecked_pages(root: Path) -> set[str]:
    """The pages whose prose check_doc_paths.py does not read: plans that propose a tree and
    records of the tree before a rename. A path in them is history and stays as it was written."""
    sys.path.insert(0, str(root / "tools" / "checks"))
    try:
        return set(importlib.import_module("check_doc_paths").PROSE_PATHS_UNCHECKED)
    except (ImportError, AttributeError):
        return set()
    finally:
        sys.path.pop(0)


def is_page(path: str, unchecked: set[str] = frozenset()) -> bool:
    if path.startswith(PAGES_KEEP_OLD) or path.startswith("tools/n1b/") or path in unchecked:
        return False
    return path.endswith(".md")


def rewrite_page_paths(text: str, moves: dict[str, str]) -> str:
    """Every whole old path of `moves` in `text` becomes its new path."""
    for old in sorted(moves, key=len, reverse=True):
        if old in text:
            new = moves[old]
            # not the tail of a longer path (`x/apps/...`), not the head of a longer name
            text = re.sub(
                r"(?<![A-Za-z0-9_-])(?<![A-Za-z0-9_-]/)"
                + re.escape(old)
                + r"(?![A-Za-z0-9_]|\.[A-Za-z0-9])",
                lambda _m, new=new: new,
                text,
            )
    return text


# --- the table as data ----------------------------------------------------------------------------


def table_as_data() -> dict:
    return {
        "programs": [
            {
                "old": p.old,
                "name": p.name,
                "ident": p.ident,
                "family": p.family,
                "namespace": p.ns,
                "variable_stem": f"ICLFORGE_{p.var}_" if p.var else None,
            }
            for p in PROGRAMS
        ],
        "variables": {f"AC3{k}_": f"ICLFORGE_{v}_" for k, v in VAR_STEMS.items()}
        | {"AC3_GUI_": "ICLFORGE_GUI_"},
        "explicit": EXPLICIT,
        "qml_modules": MODULES,
        # the display name, by what follows it (and, for the GUI window, by where it is written)
        "display": {
            "AC3Forge Hearth": "Hearth",
            "AC3Forge Crucible": "Crucible",
            "AC3Forge": "ICL Forge",
            "ac3forge (the GUI window's own text and catalogues)": "Forge",
            "AC3FORGE (the GUI window's kicker)": "FORGE",
            "ac3forge library": "iclforge library",
            "AC3Forge.Stream (the Windows file type's ProgID)": "IclForge.Stream",
        },
        # names that are the family's and sit in a program's tree: packages, icons, the Android app
        "family_names": {
            "ac3forge-crucible": "iclforge-crucible",
            "ac3forge-hearth": "iclforge-hearth",
            "ac3forge-shield": "iclforge-shield",
            "ac3forge-nullsink-driver-testsigned": "iclforge-nullsink-driver-testsigned",
            "ac3forge-32.png, ac3forge-256.png, ac3forge.ico, ac3forge.icns, ac3forge-icon.svg": (
                "iclforge-32.png, iclforge-256.png, iclforge.ico, iclforge.icns, iclforge-icon.svg"
            ),
            "com.ac3forge.shield (and Java_com_ac3forge_shield_*, ac3forge_jni)": (
                "com.iclforge.shield (and Java_com_iclforge_shield_*, iclforge_jni)"
            ),
            "org.ac3forge.CrucibleFixture": "org.iclforge.CrucibleFixture",
            "ac3forge_crucible_sink": "iclforge_crucible_sink",
        },
        # QSettings: the organisation the three programs store under, and their application names
        "settings": {
            "organisation": {"ac3forge": "iclforge"},
            "applications": {"ac3forge": "forge-gui", "Hearth": "Hearth", "Crucible": "Crucible"},
        },
        # the bare program names printed or registered by a program as a file or command
        "translation_catalogues": {
            "ac3gui_<code>.ts": "forge_gui_<code>.ts",
            "ac3hearth_<code>.ts": "hearth_<code>.ts",
            "ac3crucible_<code>.ts": "crucible_<code>.ts",
        },
        "kept": {
            "driver": "Ac3ForgeNullSink, ac3forgenullsink, Ac3ForgeWdkNuGetVersion and the .NET "
            "namespace Ac3Forge of the driver's scripts: the installed identity",
        },
    }


# --- the run --------------------------------------------------------------------------------------


def run_text(root: Path, repo: Repo, moves: dict[str, str], dry_run: bool, report: list[str]):
    """Rewrite the text of every file in scope. `moves` names where a file will be, for a run made
    before the files have moved; after the moves the files are where they are, and it is empty."""
    changed = 0
    counts: Counter = Counter()
    broken: list[str] = []
    for f in repo.files:
        path = moves.get(f, f)
        if not in_scope(path):
            continue
        p = root / f
        try:
            data = p.read_bytes()
        except OSError:
            continue
        if b"\0" in data[:8192]:
            continue
        try:
            before = data.decode("utf-8")
        except UnicodeDecodeError:
            continue
        hits: list = []
        after = transform(path, before, hits, counts)
        if breaks_python(path, before, after):
            broken.append(path)
            continue
        for kind, rule, old, new in hits:
            report.append("\t".join((kind, rule, path, old, new)))
        if after != before:
            changed += 1
            if not dry_run:
                p.write_bytes(after.encode("utf-8"))
    return changed, counts, broken


def run_pages(root: Path, repo: Repo, moves: dict[str, str], dry_run: bool) -> int:
    changed = 0
    unchecked = unchecked_pages(root)
    for f in repo.files:
        if not is_page(f, unchecked):
            continue
        p = root / f
        try:
            before = p.read_bytes().decode("utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        after = rewrite_page_paths(before, moves)
        if after != before:
            changed += 1
            if not dry_run:
                p.write_bytes(after.encode("utf-8"))
    return changed


def main() -> int:
    ap = base_parser(__doc__)
    ap.add_argument("--phase", choices=["mv", "text", "pages", "all"], default="all")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--report", default=None, help="write what was decided here")
    ap.add_argument("--json", default=None, help="write the old-to-new table and the moves here")
    ap.add_argument("--table", action="store_true", help="print the table and stop")
    ap.add_argument(
        "--moves",
        default=None,
        help="the moves of a finished `mv` phase (json); `pages` needs them after the commit",
    )
    a = ap.parse_args()
    if a.table:
        print(json.dumps(table_as_data(), indent=1))
        return 0
    if sys.version_info < (3, 12):
        sys.exit("n1b_programs.py needs Python 3.12 or later: an f-string's names are tokens there")
    root = Path(a.root)
    repo = Repo(a.root)
    moves = compute_moves(repo)
    if a.moves:
        moves = json.loads(Path(a.moves).read_text(encoding="utf-8"))["moves"]
    if a.phase in ("mv", "all"):
        print(f"{len(moves)} files move")
        if not a.dry_run:
            do_moves(root, moves)
            repo = Repo(a.root)
    report: list[str] = []
    if a.phase in ("text", "all"):
        changed, counts, broken = run_text(root, repo, moves, a.dry_run, report)
        print(f"{'would change' if a.dry_run else 'changed'} {changed} files")
        for family, n in sorted(counts.items()):
            print(f"  {family:22s} {n:6d}")
        if broken:
            print(f"{len(broken)} Python sources would no longer parse, and were not written:")
            for path in broken:
                print(f"  {path}")
            return 1
    if a.phase in ("pages", "all"):
        changed = run_pages(root, repo, moves, a.dry_run)
        print(f"{'would change' if a.dry_run else 'changed'} {changed} pages")
    if a.report:
        Path(a.report).write_text("\n".join(sorted(report)) + "\n", encoding="utf-8", newline="\n")
    if a.json:
        data = table_as_data()
        data["moves"] = moves
        Path(a.json).write_text(json.dumps(data, indent=1), encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
