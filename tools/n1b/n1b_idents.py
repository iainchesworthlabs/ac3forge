"""The identifier pass, stage 4 of the plan: the brand `ac3forge` becomes `iclforge`.

    n1b_idents.py --root <worktree> [--dry-run] [--report <file>] [--json <file>]

`ac3forge` and `iclforge`, `AC3FORGE` and `ICLFORGE` are the same length, so the brand itself
reflows no line. What the pass renames is every place the old brand is an identifier, a name or a
string that both ends of something read: the C API (`ac3forge_*`, `AC3FORGE_*`, `AC3FORGEC_EXPORT`),
CMake options, variables and helper functions, the package config, Kconfig, the environment
variables, the wire and format strings (`_ac3forge_player@v1`, `ac3forge.probe/1`, the OTA project
name), file and package names, the bindings (the Python module, the crates, the npm package and its
JS factories), the ESP-IDF component's namespace (merged into the root), and the entries of the Qt
catalogues that quote one. Besides the brand it moves four families that carry a prefix of their
own: the per-library export macros (`MP4_EXPORT` becomes `ICLFORGE_MP4_EXPORT`, the pattern
iclforge_add_library() makes), the CMake helper targets (`ac3::warnings` becomes
`iclforge::warnings`), the profiling macros (`AC3_ZONE_BEGIN` becomes `ICLFORGE_ZONE_BEGIN`), and
the AC-3 library's files as comments still name them (`libac3forge.so` becomes
`libiclforge_ac3.so`).

What it does not rename, and says so in its report (`--report`), is in four groups.

  External identities: the repository slug and the Pages address, the SonarCloud project, the tap
  repository, and the paths a runner derives from the repository's name. They change with the
  repository (stage S5) and are the owner's. The winget package is not one of them: its
  submission was closed unmerged, so what the bump script writes for a later release names the new
  identity, and the manifests already made stay where they are (packaging/winget/manifests is not
  read).
  What N1A renames: the programs and what they register with the system (a program's QSettings
  organisation, registry keys and user-data directories, its icons, its window titles and every
  string a person reads, the QML module URIs, the Android package and its JNI names, the Windows
  driver, the packages named for a program). In the trees that are a program's (PROGRAM_TREES), and
  in QML, HTML and the Qt catalogues, the bare word `ac3forge` is the program's; the identifiers
  and the strings both ends of the wire read are still renamed there.
  What reaches a signature: the example key of examples/object_signing.cpp is a key's own bytes.
  The old name written on purpose: what the stage's hand-written commit says about the past (the
  PyPI description "formerly ac3forge", the winget identity of the released manifests, the old
  names the Homebrew tap maps, the subjects of the commits `.git-blame-ignore-revs` lists). They
  are a few lines named in FORMER_NAME_LINES, and two files that are read nowhere, so that a run
  after the hand-written commit does not undo it.

Pages (`.md`), the history (CHANGELOG, planning, the scripts of this migration) and the byte-exact
trees (tests/golden) are not read. Every decision has a name, listed in RULES with its reason;
`--report` lists the occurrences that were kept. A second run finds nothing to rename, on the
commit it made and on the tree after the hand-written one.
"""

from __future__ import annotations

import json
import re
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path

from n1b_lib import Repo, base_parser

BRAND = re.compile(r"(?i)ac3forge")
IDENT = re.compile(r"[A-Za-z0-9_]")

# --- which files are read -----------------------------------------------------------------------

# Pages, the history, and the byte-exact trees. `.md` is read nowhere: the pages are stage S5's.
SKIP_PREFIXES = (
    "docs/",
    "planning/",
    "tools/n1b/",
    "docs-snippets/",
    "overrides/",
    "tests/golden/",
    "packaging/winget/manifests/",
    "CHANGELOG.md",
    "ROADMAP.md",
    "README.md",
    "CONTRIBUTING.md",
    "SECURITY.md",
    "mkdocs.yml",
    # the old names are what these two files are about: the subjects of the commits that were
    # rewritten, and the keys of the tap's migration map
    ".git-blame-ignore-revs",
    "packaging/homebrew/tap_migrations.json",
)
SKIP_SUFFIXES = (".md",)

# The old name written on purpose by the stage's hand-written commit, line by line: a fragment of
# the line, in the file that has it. A fragment is what is said about the past, never a name the
# tree still uses, so none of them is in the tree the pass runs on first.
FORMER_NAME_LINES: dict[str, tuple[str, ...]] = {
    "python/pyproject.toml": ("(formerly ac3forge)",),
    ".github/workflows/manifest-bump.yml": (
        "old names (ac3forge, ac3gui)",
        "git rm -q --ignore-unmatch Formula/ac3forge.rb Casks/ac3gui.rb",
    ),
    "tools/checks/check_packaging_versions.sh": (
        "identity iainchesworthlabs.ac3forge and stay as they were made",
        "for winget_package in ac3forge iclforge; do",
        "{ac3forge,iclforge}",
    ),
    "tools/release/bump_manifests.py": (
        "iainchesworthlabs.ac3forge stay in the ac3forge directory",
    ),
}

# The committed fallbacks of the docs site's WASM demos are copies of apps/wasm and js/ (docs.yml
# checks the page files byte for byte, and replaces the rest with a fresh build at every deploy).
READ_ANYWAY_PREFIXES = ("docs/assets/wasm-decode-demo/", "docs/assets/wasm-encode-demo/")

# The trees that are a program's. In them, and in QML and HTML anywhere, the bare word `ac3forge`
# and the hyphenated names (`ac3forge-32.png`, `ac3forge-tests`) are the program's own: what it
# calls itself and what it registers. N1A renames them with the program.
PROGRAM_TREES = (
    "apps/gui/",
    "apps/crucible/",
    "apps/hearth/",
    "apps/windows/",
    "apps/android/",
    "apps/notices/",
    "apps/linux/",
    "apps/cli/",
    "tests/gui/",
    "tests/crucible/",
    "tests/hearth/",
    "tests/cli/",
    "assets/",
    "apps/wasm/tests/device-ui/",
)
PROGRAM_SUFFIXES = (".qml", ".html")


def is_program_file(path: str) -> bool:
    return path.startswith(PROGRAM_TREES) or path.endswith(PROGRAM_SUFFIXES)


# The Qt translation catalogues carry program strings; a TypeScript file has the same suffix.
def is_qt_catalogue(path: str) -> bool:
    return "/translations/" in path and path.endswith(".ts")


# --- the decisions --------------------------------------------------------------------------------

KEEP, RENAME = "keep", "rename"


@dataclass(frozen=True)
class Rule:
    name: str
    action: str
    group: str  # external | n1a | signature | brand
    reason: str


RULES: dict[str, Rule] = {
    r.name: r
    for r in (
        # external identities
        Rule(
            "slug", KEEP, "external", "the repository slug, the Pages address, the tap repository"
        ),
        Rule("sonar", KEEP, "external", "the SonarCloud project key and name"),
        Rule(
            "runner-path",
            KEEP,
            "external",
            "a path or a reference a runner derives from the repository name",
        ),
        Rule(
            "old-release", KEEP, "external", "the name of an asset of a release that already exists"
        ),
        # N1A
        Rule("n1a-token", KEEP, "n1a", "a name of a program, a registration or an asset of one"),
        Rule(
            "n1a-display",
            KEEP,
            "n1a",
            "display prose (AC3Forge, and the bare word in a program's own files)",
        ),
        Rule(
            "n1a-mixed",
            KEEP,
            "n1a",
            "a mixed-case name: a QML module URI, the driver, a display name",
        ),
        # what reaches a signature
        Rule("signature-key", KEEP, "signature", "the bytes of an example signing key"),
        # the past, written on purpose
        Rule(
            "former-name",
            KEEP,
            "former",
            "the old name in a line that says what it was (FORMER_NAME_LINES)",
        ),
        # the brand
        Rule("wire", RENAME, "brand", "a name both ends of a wire or a file format read"),
        Rule(
            "identifier",
            RENAME,
            "brand",
            "an identifier: C API, macro, CMake, Kconfig, environment",
        ),
        Rule(
            "namespace", RENAME, "brand", "the ESP-IDF component's namespace, merged into the root"
        ),
        Rule("path-name", RENAME, "brand", "a file, directory or package name"),
        Rule("js-name", RENAME, "brand", "a name of the JS package and its WASM factories"),
        Rule("prose", RENAME, "brand", "the bare word in code, comments and messages"),
    )
}

# The mixed-case names of the JS package, its WASM factories and the ESPHome component are
# identifiers (`createAc3ForgeModule`); every other `Ac3Forge...` is a QML module or the driver.
_JS_MIXED = re.compile(
    r"^(?:create)?Ac3Forge(?:DecoderNodeOptions|DecoderNode|EmbindModule|EncodeModule|Ac4Module|"
    r"ModuleFactory|SourceProcessor|ProcessorOptions|Component|Module)$"
)

# Patterns on what sits before and after an occurrence, and on the identifier run around it.
_EXTERNAL_LEFT = re.compile(
    r"(?:iainchesworthlabs(?:\.github\.io)?/|iainchesworthlabs_|homebrew-)$"
)
_RUNNER_LEFT = re.compile(r"(?:/__w/|/work/)(?:ac3forge/)?$")
_RUNNER_RIGHT = re.compile(r"#\d")
_CLONE_DIR_LEFT = re.compile(r"\bcd $")
_SONAR_LINE = re.compile(r"^\s*sonar\.project(?:Key|Name)\s*=")
_OLD_RELEASE_LINE = re.compile(r"releases/download/v\d")
_N1A_LEFT = re.compile(r"(?:com[./]|org\.)$")
_N1A_RIGHT = re.compile(
    r"^(?:[./]shield|_shield|-shield|_jni|-crucible|-hearth|-nullsink|_crucible_sink|"
    r"-32\.png|-256\.png|\.ico|\.icns|-icon\.svg)"
)
# a test organisation's name for QSettings (`ac3forge-tests`), in a program's own tests only
_N1A_TESTS_RIGHT = re.compile(r"^-tests(?![\w-])")
_N1A_TOKEN = re.compile(r"(?i)nullsink|Wdk|^Java_com_")
_SIGNATURE_RIGHT = re.compile(r"^-example-key-DO-NOT-USE")
# a newline, tab or carriage return escape after the word, not a path separator
_ESCAPE_RIGHT = re.compile(r"\\[ntr](?![a-z])")


def _run(line: str, start: int, end: int) -> tuple[str, str, str]:
    """The identifier run around [start, end) and what is on each side of it."""
    left, right = line[:start], line[end:]
    lrun = re.search(r"[A-Za-z0-9_]*$", left).group(0)
    # A backslash escape can sit against the word (`\nac3forge`): its letter is not part of a name.
    if lrun and left[: len(left) - len(lrun)].endswith("\\") and lrun[0] in "ntrfbav":
        lrun = lrun[1:]
    rrun = re.match(r"[A-Za-z0-9_]*", right).group(0)
    return lrun + line[start:end] + rrun, left, right


def decide(path: str, line: str, start: int, end: int) -> str:
    """The name of the rule that decides the occurrence at line[start:end], a case of `ac3forge`."""
    word = line[start:end]
    tok, left, right = _run(line, start, end)
    program = is_program_file(path) or is_qt_catalogue(path)

    # what is not ours to change
    if any(fragment in line for fragment in FORMER_NAME_LINES.get(path, ())):
        return "former-name"
    if _EXTERNAL_LEFT.search(left):
        return "slug"
    if _SONAR_LINE.match(line):
        return "sonar"
    if _RUNNER_LEFT.search(left) or _RUNNER_RIGHT.match(right) or _CLONE_DIR_LEFT.search(left):
        return "runner-path"
    if _OLD_RELEASE_LINE.search(line) and right.startswith("-"):
        return "old-release"
    if _SIGNATURE_RIGHT.match(right):
        return "signature-key"
    if word == "AC3Forge":
        return "n1a-display"
    if _N1A_LEFT.search(left) or _N1A_RIGHT.match(right) or _N1A_TOKEN.search(tok):
        return "n1a-token"
    if program and _N1A_TESTS_RIGHT.match(right):
        return "n1a-token"
    if word == "Ac3Forge":
        return "js-name" if _JS_MIXED.match(tok) else "n1a-mixed"
    if word == "Ac3forge":
        return "identifier"

    # the brand as a name both ends read
    if word == "AC3FORGE":
        if right[:1] == "_" or right[:2] == "C_" or left.endswith(("D", "CONFIG_", "_", "$")):
            return "identifier"
        return "n1a-display" if program else "prose"
    if left.endswith("_") or right.startswith("_"):
        return "wire" if "_ac3forge_player" in tok else "identifier"
    if right.startswith("::"):
        return "namespace"
    if left.endswith("[") and right.startswith("]"):
        return "identifier"  # a Catch2 tag
    if left.endswith("find_package(") or left.endswith("check_required_components("):
        return "identifier"
    if re.match(r"\.[a-z][a-z.]*/\d", right):
        return "wire"
    if (
        left.endswith(("/", "\\"))
        or right.startswith("/")
        or (right.startswith("\\") and not _ESCAPE_RIGHT.match(right))
    ):
        return "path-name"
    if re.match(r"-[A-Za-z0-9{]", right) or re.match(r"\.[A-Za-z]", right):
        return "path-name"  # ac3forge-dev-*, ac3forge.h, ac3forge.ac4
    if tok != word:
        return "identifier"
    return "n1a-display" if program else "prose"


# --- the renames ---------------------------------------------------------------------------------

_CASE = {
    "ac3forge": "iclforge",
    "AC3FORGE": "ICLFORGE",
    "Ac3Forge": "IclForge",
    "Ac3forge": "Iclforge",
}


def rename_of(word: str) -> str:
    try:
        return _CASE[word]
    except KeyError:
        raise ValueError(f"no rule for the spelling {word!r}") from None


def symbol_rename(name: str) -> str:
    """The name of an exported symbol after the pass: every brand in it is an identifier's, since
    no program, registration or slug is exported. export_diff.py and abi_compare.py rewrite the
    old record with this, and the new one must equal it. The extension role's namespace goes first
    (n1b_sendspin.py), then the brand."""
    name = name.replace("sendspin::ac3forge", "sendspin::player")
    return BRAND.sub(lambda m: rename_of(m.group(0)), name)


# The per-library export macros, by the base name generate_export_header() was given. The new base
# is what iclforge_add_library() makes: ICLFORGE_<LIBRARY>.
EXPORT_BASES = {
    "MP4": "ICLFORGE_MP4",
    "MPEGTS": "ICLFORGE_MPEGTS",
    "MATROSKA": "ICLFORGE_MATROSKA",
    "IAMF": "ICLFORGE_IAMF",
    "AC4": "ICLFORGE_AC4",
    "AC4DEC": "ICLFORGE_AC4DEC",
    "AC4ENC": "ICLFORGE_AC4ENC",
    "AC3IAB": "ICLFORGE_IAB",
    "AC3ADM": "ICLFORGE_ADM",
    "AC3ADMBRIDGE": "ICLFORGE_ADMBRIDGE",
    "ADMBRIDGE": "ICLFORGE_ADMBRIDGE",
    "AC3SIGNING": "ICLFORGE_SIGNING",
    "AC3FORGEC": "ICLFORGE_C",
}
_EXPORT_SUFFIXES = (
    "EXPORT",
    "NO_EXPORT",
    "STATIC_DEFINE",
    "BUILDING_SHARED",
    "DEPRECATED",
    "DEPRECATED_EXPORT",
    "DEPRECATED_NO_EXPORT",
)
_EXPORT_RX = re.compile(
    r"(?<![A-Za-z0-9_])(?P<base>"
    + "|".join(sorted(EXPORT_BASES, key=len, reverse=True))
    + r")_(?P<suffix>"
    + "|".join(sorted(_EXPORT_SUFFIXES, key=len, reverse=True))
    + r")(?![A-Za-z0-9_])"
)
# `-DAC4_STATIC_DEFINE`: the D of a compiler definition sits against the name.
_EXPORT_D_RX = re.compile(
    r"(?<=-)D(?P<base>"
    + "|".join(sorted(EXPORT_BASES, key=len, reverse=True))
    + r")_(?P<suffix>"
    + "|".join(sorted(_EXPORT_SUFFIXES, key=len, reverse=True))
    + r")(?![A-Za-z0-9_])"
)

# The old single library's macros, still named by the tools that syntax-check with them.
OLD_LIBRARY_MACROS = {
    "AC3FORGE_EXPORT": "ICLFORGE_AC3_EXPORT",
    "AC3FORGE_STATIC_DEFINE": "ICLFORGE_AC3_STATIC_DEFINE",
    "AC3FORGE_BUILDING_SHARED": "ICLFORGE_AC3_BUILDING_SHARED",
    "AC3FORGE_TEMPLATE_CLASS": "ICLFORGE_AC3_TEMPLATE_CLASS",
    "AC3FORGE_TEMPLATE_IMPORT": "ICLFORGE_AC3_TEMPLATE_IMPORT",
    "AC3FORGE_TEMPLATE_INSTANTIATE": "ICLFORGE_AC3_TEMPLATE_INSTANTIATE",
}
_OLD_LIBRARY_RX = re.compile(
    r"(?<![A-Za-z0-9_])(D?)(" + "|".join(OLD_LIBRARY_MACROS) + r")(?![A-Za-z0-9_])"
)

# The CMake helper targets: the alias and the real target.
HELPER_TARGETS = (
    "warnings",
    "coverage",
    "fmt_private",
    "fmt",
    "tracy",
    "minimal_profile",
    "crucible_engine",
)
_HELPER_ALIAS_RX = re.compile(
    r"(?<![A-Za-z0-9_])(?<!::)ac3::(" + "|".join(HELPER_TARGETS) + r")(?![A-Za-z0-9_])"
)
_HELPER_REAL_RX = re.compile(
    r"(?<![A-Za-z0-9_])ac3_(warnings|coverage|fmt_private|fmt|tracy|minimal_profile)"
    r"(?![A-Za-z0-9_])"
)
# The profiling macros of the base library.
_PROFILING_RX = re.compile(
    r"(?<![A-Za-z0-9_])AC3_((?:ZONE|FRAME_MARK|PROFILING)[A-Z0-9_]*)(?![a-z])"
)

# The AC-3 library's files, as comments name them by the old spelling.
_OLD_LIBRARY_FILES = (
    (re.compile(r"(?<![A-Za-z0-9_])libac3forge_static(?![A-Za-z0-9_])"), "libiclforge_ac3_static"),
    (
        re.compile(r"(?<![A-Za-z0-9_])libac3forge(?=\.(?:so|dylib|a)(?![A-Za-z0-9_]))"),
        "libiclforge_ac3",
    ),
    (
        re.compile(r"(?<![A-Za-z0-9_/])ac3forge_static(?=\.lib(?![A-Za-z0-9_]))"),
        "iclforge_ac3_static",
    ),
    (re.compile(r"(?<![A-Za-z0-9_/])ac3forge(?=\.(?:dll|lib)(?![A-Za-z0-9_]))"), "iclforge_ac3"),
)


# generate_export_header(... BASE_NAME <BASE> ...) makes <BASE>_EXPORT, <BASE>_NO_EXPORT and the
# rest: the base a library was given moves with the macros the sources name.
_BASE_NAME_RX = re.compile(
    r"(?<![A-Za-z0-9_])(?P<key>BASE_NAME\s+)(?P<base>"
    + "|".join(sorted(EXPORT_BASES, key=len, reverse=True))
    + r")(?![A-Za-z0-9_])"
)


def _base_name_sub(m: re.Match) -> str:
    return f"{m.group('key')}{EXPORT_BASES[m.group('base')]}"


def _export_sub(m: re.Match) -> str:
    return f"{EXPORT_BASES[m.group('base')]}_{m.group('suffix')}"


def _export_d_sub(m: re.Match) -> str:
    return f"D{EXPORT_BASES[m.group('base')]}_{m.group('suffix')}"


def prefix_families(text: str, counts: Counter | None = None) -> str:
    """The families that carry a prefix of their own, before the brand's own pass reads the rest.
    `counts` gets how many each family made."""
    steps = (
        ("library files", [(rx, new) for rx, new in _OLD_LIBRARY_FILES]),
        (
            "old library macros",
            [(_OLD_LIBRARY_RX, lambda m: m.group(1) + OLD_LIBRARY_MACROS[m.group(2)])],
        ),
        ("export macros", [(_EXPORT_RX, _export_sub), (_EXPORT_D_RX, _export_d_sub)]),
        ("export base names", [(_BASE_NAME_RX, _base_name_sub)]),
        (
            "helper targets",
            [(_HELPER_ALIAS_RX, r"iclforge::\1"), (_HELPER_REAL_RX, r"iclforge_\1")],
        ),
        ("profiling macros", [(_PROFILING_RX, r"ICLFORGE_\1")]),
    )
    for family, rules in steps:
        for rx, new in rules:
            text, n = rx.subn(new, text)
            if counts is not None and n:
                counts[family] += n
    return text


def _is_c_library_prefix(line: str, end: int) -> bool:
    """`AC3FORGE` at line[:end] is the start of `AC3FORGEC_...` or of a bare `AC3FORGEC`."""
    if line[end : end + 2] == "C_":
        return True
    return line[end : end + 1] == "C" and not IDENT.match(line[end + 1 : end + 2] or " ")


def transform(
    path: str, text: str, hits: list | None = None, families: Counter | None = None
) -> str:
    """The text with the decisions carried out. Every occurrence of the brand is recorded in `hits`
    as (rule, line number, line) when a list is given, and the families that carry a prefix of
    their own are counted into `families`."""
    text = prefix_families(text, families)
    out: list[str] = []
    for number, line in enumerate(text.split("\n"), 1):
        pieces: list[str] = []
        cursor = 0
        for m in BRAND.finditer(line):
            start, end = m.span()
            if start < cursor:
                continue  # consumed by the C in `AC3FORGEC_`
            rule = decide(path, line, start, end)
            word = line[start:end]
            replacement = word
            stop = end
            if RULES[rule].action == RENAME:
                replacement = rename_of(word)
                if word == "AC3FORGE" and _is_c_library_prefix(line, end):
                    replacement, stop = "ICLFORGE_C", end + 1
            if hits is not None:
                hits.append((rule, number, line))
            pieces.append(line[cursor:start])
            pieces.append(replacement)
            cursor = stop
        pieces.append(line[cursor:])
        out.append("".join(pieces))
    return "\n".join(out)


def in_scope(path: str) -> bool:
    if path.startswith(READ_ANYWAY_PREFIXES) and not path.endswith(".wasm"):
        return True
    return not (path.startswith(SKIP_PREFIXES) or path.endswith(SKIP_SUFFIXES))


def main() -> None:
    ap = base_parser(__doc__)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--report", default=None, help="write the decisions to this file")
    ap.add_argument("--json", default=None, help="write the decisions as JSON to this file")
    a = ap.parse_args()
    root = Path(a.root)
    repo = Repo(a.root)
    changed = 0
    by_rule: Counter[str] = Counter()
    families: Counter[str] = Counter()
    files_by_rule: dict[str, Counter[str]] = defaultdict(Counter)
    kept: list[tuple[str, str, int, str]] = []
    for f in repo.files:
        if not in_scope(f):
            continue
        data = (root / f).read_bytes() if (root / f).is_file() else b""
        if b"\0" in data[:8192]:
            continue
        try:
            before = data.decode("utf-8")
        except UnicodeDecodeError:
            continue
        hits: list = []
        after = transform(f, before, hits, families)
        for rule, number, line in hits:
            by_rule[rule] += 1
            files_by_rule[rule][f] += 1
            if RULES[rule].action == KEEP:
                kept.append((rule, f, number, line.strip()[:160]))
        if after != before:
            changed += 1
            if not a.dry_run:
                (root / f).write_bytes(after.encode("utf-8"))
    print(f"{'would change' if a.dry_run else 'changed'} {changed} files")
    for rule, n in sorted(by_rule.items(), key=lambda kv: (RULES[kv[0]].group, -kv[1])):
        r = RULES[rule]
        print(
            f"  {r.group:9s} {rule:14s} {r.action:6s} {n:6d} in {len(files_by_rule[rule]):4d} files"
        )
    for family, n in sorted(families.items()):
        print(f"  {'prefix':9s} {family:18s} rename {n:6d}")
    if a.report:
        lines = []
        for rule, f, number, text in sorted(kept):
            lines.append(f"{RULES[rule].group}/{rule}\t{f}:{number}\t{text}")
        Path(a.report).write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    if a.json:
        Path(a.json).write_text(
            json.dumps(
                {
                    "rules": {k: [v.action, v.group, v.reason] for k, v in RULES.items()},
                    "counts": dict(by_rule),
                    "prefix_families": dict(families),
                    "files": {k: dict(v) for k, v in files_by_rule.items()},
                },
                indent=1,
            ),
            encoding="utf-8",
            newline="\n",
        )


if __name__ == "__main__":
    sys.exit(main())
