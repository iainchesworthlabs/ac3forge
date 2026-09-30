"""Mechanical CMake and script renames: target names, output names and moved file paths.

    n1b_cmake.py --root <worktree> [--dry-run]

Applies, to every tracked build or script file (CMake, presets, workflows, tools/**, packaging
scripts) outside docs/, planning/ and the two history files:
  * library aliases:      ac3::forge -> iclforge::ac3, mp4::mp4 -> iclforge::mp4,
                          ac4::decoder -> iclforge::ac4dec ...
  * raw target names:     forge_static -> iclforge_ac3_static,
                          mp4_objects -> iclforge_mp4_objects ...
  * output (file) names:  OUTPUT_NAME "ac3forge" -> "iclforge_ac3", "mp4" -> "iclforge_mp4" ...
  * moved file paths:     every full old path in the move map that appears in the text, and the
                          directories the moves keep together; in tests/CMakeLists.txt also the
                          paths it names relative to itself
It leaves tools/n1b/ alone (the scripts and baselines name the tree they were written for), and it
drops the `layout` section of tools/checks/layering.json, which the split no longer needs.
The dev-only interface targets (ac3::warnings, coverage, tracy, fmt_private, minimal_profile) keep
their names until the identifier stage.
"""

from __future__ import annotations

import json
import posixpath
import re
from pathlib import Path

import layoutdef
from n1b_lib import Repo, base_parser

BUILD_FILES = re.compile(
    r"(^|/)(CMakeLists\.txt|CMakePresets\.json)$|\.cmake(\.in)?$|\.ya?ml$|\.sh$|\.ps1$|\.py$|\.projbuild$|\.txt$|\.json$|\.toml$|\.properties$"
)
SKIP_PREFIX = (
    "docs/",
    "planning/",
    "docs-snippets/",
    "overrides/",
    "tests/golden/",
    "fuzz/seeds/",
    "fuzz/regressions/",
    "tools/n1b/",  # the scripts and the baselines name the tree they were written for
)
LAYERING_TABLE = "tools/checks/layering.json"
SKIP_FILES = {"CHANGELOG.md", "ROADMAP.md", "README.md", "CONTRIBUTING.md", "SECURITY.md"}
# Directories whose build files name their own files relative to themselves (see relative_rules).
RELATIVE_BASES = ("tests",)

LIBS = [  # (old raw stem, new lib name, old alias namespace::name)
    ("forge", "ac3"),
    ("signing", "signing"),
    ("admbridge", "admbridge"),
    ("iamf", "iamf"),
    ("matroska", "matroska"),
    ("mp4", "mp4"),
    ("mpegts", "mpegts"),
    ("ac3iab", "iab"),
    ("ac3adm", "adm"),
    ("ac4", "ac4"),
    ("ac4dec", "ac4dec"),
    ("ac4enc", "ac4enc"),
    ("forge_c", "capi"),
]

ALIASES = [
    ("ac3::forge_minimal", "iclforge::ac3_minimal"),
    ("ac3::forge_c_static", "iclforge::c_static"),
    ("ac3::forge_c_shared", "iclforge::c_shared"),
    ("ac3::forge_c", "iclforge::c"),
    ("ac3::forge_static", "iclforge::ac3_static"),
    ("ac3::forge_shared", "iclforge::ac3_shared"),
    ("ac3::forge", "iclforge::ac3"),
    ("ac3::audio", "iclforge::audio"),
    ("ac3::sendspin", "iclforge::sendspin"),
    ("ac3::signing_static", "iclforge::signing_static"),
    ("ac3::signing_shared", "iclforge::signing_shared"),
    ("ac3::signing", "iclforge::signing"),
    ("ac3::admbridge_static", "iclforge::admbridge_static"),
    ("ac3::admbridge_shared", "iclforge::admbridge_shared"),
    ("ac3::admbridge", "iclforge::admbridge"),
    ("ac3::arithmetic", "iclforge::arithmetic"),
    ("matroska::matroska_static", "iclforge::matroska_static"),
    ("matroska::matroska_shared", "iclforge::matroska_shared"),
    ("matroska::matroska", "iclforge::matroska"),
    ("mp4::mp4_static", "iclforge::mp4_static"),
    ("mp4::mp4_shared", "iclforge::mp4_shared"),
    ("mp4::mp4", "iclforge::mp4"),
    ("mpegts::mpegts_static", "iclforge::mpegts_static"),
    ("mpegts::mpegts_shared", "iclforge::mpegts_shared"),
    ("mpegts::mpegts", "iclforge::mpegts"),
    ("iamf::iamf_static", "iclforge::iamf_static"),
    ("iamf::iamf_shared", "iclforge::iamf_shared"),
    ("iamf::iamf", "iclforge::iamf"),
    ("ac3iab::ac3iab_static", "iclforge::iab_static"),
    ("ac3iab::ac3iab_shared", "iclforge::iab_shared"),
    ("ac3iab::ac3iab", "iclforge::iab"),
    ("ac3adm::ac3adm_static", "iclforge::adm_static"),
    ("ac3adm::ac3adm_shared", "iclforge::adm_shared"),
    ("ac3adm::ac3adm", "iclforge::adm"),
    ("ac4::ac4_static", "iclforge::ac4_static"),
    ("ac4::ac4_shared", "iclforge::ac4_shared"),
    ("ac4::ac4", "iclforge::ac4"),
    ("ac4::decoder_static", "iclforge::ac4dec_static"),
    ("ac4::decoder_shared", "iclforge::ac4dec_shared"),
    ("ac4::decoder", "iclforge::ac4dec"),
    ("ac4::encoder_static", "iclforge::ac4enc_static"),
    ("ac4::encoder_shared", "iclforge::ac4enc_shared"),
    ("ac4::encoder", "iclforge::ac4enc"),
    ("ac4::core", "iclforge::ac4core"),
]

RAW = [
    (r"(?<![\w/.\-])(?<!::)forge_simd_avx2\b", "iclforge_ac3_simd_avx2"),
    (r"(?<![\w/.\-])(?<!::)forge_minimal\b", "iclforge_ac3_minimal"),
    (r"(?<![\w/.\-])(?<!::)forge_c_(objects|static|shared)\b", r"iclforge_capi_\1"),
    (r"(?<![\w/.\-])(?<!::)forge_(objects|static|shared)\b", r"iclforge_ac3_\1"),
    (r"(?<![\w/.\-])(?<!::)ac3_arithmetic\b", "iclforge_arithmetic"),
    (r"(?<![\w/.\-])(?<!::)ac3audio\b", "iclforge_audio"),
    (r"(?<![\w/.\-])(?<!::)ac3sendspin(_time_filter|_crypto|_httplib)?\b", r"iclforge_sendspin\1"),
    (
        r"(?<![\w/.\-])(?<!::)(signing|admbridge|iamf|matroska|mp4|mpegts)_(objects|static|shared)\b",
        r"iclforge_\1_\2",
    ),
    (r"(?<![\w/.\-])(?<!::)ac3iab_(objects|static|shared)\b", r"iclforge_iab_\1"),
    (r"(?<![\w/.\-])(?<!::)ac3adm_(objects|static|shared)\b", r"iclforge_adm_\1"),
    (r"(?<![\w/.\-])(?<!::)(ac4|ac4dec|ac4enc)_(objects|static|shared)\b", r"iclforge_\1_\2"),
    # Not a quoted word or one between bars: `REPO / "src" / "ac4core"` and a regex of directory
    # names name the directory, which keeps its name.
    (r"""(?<![\w:.\-/"'|])ac4core(?![\w:.\-/"'|])""", "iclforge_ac4core"),
]

OUTPUT = {
    "ac3forge": "iclforge_ac3",
    "ac3forge_static": "iclforge_ac3_static",
    "ac3forge_minimal": "iclforge_ac3_minimal",
    "ac3signing": "iclforge_signing",
    "ac3signing_static": "iclforge_signing_static",
    "admbridge": "iclforge_admbridge",
    "admbridge_static": "iclforge_admbridge_static",
    "iamf": "iclforge_iamf",
    "iamf_static": "iclforge_iamf_static",
    "matroska": "iclforge_matroska",
    "matroska_static": "iclforge_matroska_static",
    "mp4": "iclforge_mp4",
    "mp4_static": "iclforge_mp4_static",
    "mpegts": "iclforge_mpegts",
    "mpegts_static": "iclforge_mpegts_static",
    "ac3iab": "iclforge_iab",
    "ac3iab_static": "iclforge_iab_static",
    "ac3adm": "iclforge_adm",
    "ac3adm_static": "iclforge_adm_static",
    "ac4": "iclforge_ac4",
    "ac4_static": "iclforge_ac4_static",
    "ac4dec": "iclforge_ac4dec",
    "ac4dec_static": "iclforge_ac4dec_static",
    "ac4enc": "iclforge_ac4enc",
    "ac4enc_static": "iclforge_ac4enc_static",
    "ac3forge_c": "iclforge_c",
    "ac3forge_c_static": "iclforge_c_static",
    "ac4core_static": "iclforge_ac4core_static",
}

GENERATED = [  # generate_export_header paths and install destinations of the generated headers
    ("generated/ac3/signing/", "generated/iclforge/signing/"),
    ("generated/ac3/admbridge/", "generated/iclforge/admbridge/"),
    ("generated/ac3adm/", "generated/iclforge/adm/"),
    ("generated/ac3iab/", "generated/iclforge/iab/"),
    ("generated/ac3forge_c/", "generated/iclforge_c/"),
    ("generated/ac3/", "generated/iclforge/ac3/"),
    ("generated/mp4/", "generated/iclforge/mp4/"),
    ("generated/mpegts/", "generated/iclforge/mpegts/"),
    ("generated/matroska/", "generated/iclforge/matroska/"),
    ("generated/iamf/", "generated/iclforge/iamf/"),
    ("generated/ac4/", "generated/iclforge/ac4/"),
    ("generated/ac4dec/", "generated/iclforge/ac4dec/"),
    ("generated/ac4enc/", "generated/iclforge/ac4enc/"),
]
_ALIAS_RX = [(re.compile(r"(?<![\w])(?<!::)" + re.escape(a) + r"(?![\w:])"), b) for a, b in ALIASES]
_RAW_RX = [(re.compile(p), r) for p, r in RAW]
_OUT_RX = re.compile(r'(OUTPUT_NAME\s+")([A-Za-z0-9_]+)(")')
# cmake/InstallLibrary.cmake gives cmake/PkgConfig.cmake the file names of a library apart from
# OUTPUT_NAME: as the two names ac3forge_pkgconfig_libname() chooses between (its third and fourth
# arguments), and as the LIBNAME of a library that has one linkage. The `-l` line of the .pc file is
# made from them, so they follow the output names.
_PC_CHOICE_RX = re.compile(r"(ac3forge_pkgconfig_libname\(\s*\S+\s+\S+\s+)(\S+)(\s+)(\S+)")
_PC_LIBNAME_RX = re.compile(r"(\bLIBNAME\s+)([A-Za-z0-9_]+)\b")


MIXED = re.compile(
    r"\b(ac3|ac4)::iclforge_(ac3|ac4dec|ac4enc)_(objects|static|shared)\b"
)  # a raw name inside an old alias
# A repository-relative path starts here: after a separator or quote, or straight after a
# `${CMAKE_SOURCE_DIR}/`-style prefix, a `../` one or the `blob/main/` of a link into the
# repository.
_PATH_START = r"(?:(?<![\w/.\-])|(?<=\}/)|(?<=\.\./)|(?<=blob/main/)|(?<=tree/main/))"
# ... and ends here: not before a name character or a hyphen, and not before a dot that
# goes on with one (`mdct.cpp.bak`, `x.hpp.in`), so that the full stop that ends a sentence
# does not keep a path from matching.
_PATH_END = r"(?![\w\-]|\.[\w\-])"


def dir_rules(moves: dict[str, str]) -> tuple[list[tuple[str, str]], dict[str, dict[str, int]]]:
    """Directory-level rewrites derived from the file moves.

    A build file names directories as well as files (`target_include_directories(...
    src/forge/src/core)`, a workflow's `src/forge/include/ac3/decoder/**`). Two kinds are derived:
    the root of a flattened variant tree (`.../profiling/tracy_enabled` ->
    `src/base/variants/profiling-tracy_enabled`), and every private or public directory whose last
    component survives the move. A directory whose files went to more than one place takes the
    destination most of them went to, and is returned in `split` so the caller can list it for hand
    review.
    """
    votes: dict[str, dict[str, int]] = {}

    def vote(old_dir: str, new_dir: str) -> None:
        votes.setdefault(old_dir, {}).setdefault(new_dir, 0)
        votes[old_dir][new_dir] += 1

    # (the return annotation names the second value: {old dir: {"to", "files", "elsewhere"}})

    for old, new in moves.items():
        for rx, _axis, _lib in layoutdef._VARIANT_AXES:
            m = rx.match(old)
            if m:
                vote(old[: m.end("c")], new.split("/" + layoutdef.FAMILY + "/")[0])
        d_old, d_new = posixpath.dirname(old), posixpath.dirname(new)
        if posixpath.basename(d_old) != posixpath.basename(d_new):
            # a directory that went wholesale to a directory of another name:
            # src/forge/src/spatial -> src/render/src
            vote(d_old, d_new)
        while d_old and d_new and posixpath.basename(d_old) == posixpath.basename(d_new):
            vote(d_old, d_new)
            d_old, d_new = posixpath.dirname(d_old), posixpath.dirname(d_new)
    rules, split = [], {}
    for old_dir, dests in votes.items():
        best = max(dests.items(), key=lambda kv: kv[1])[0]
        below = [f for f in moves if f.startswith(old_dir + "/")]
        away = [f for f in below if not moves[f].startswith(best + "/")]
        if away:
            split[old_dir] = {"to": best, "files": len(below), "elsewhere": len(away)}
        if len(away) * 5 >= len(below) * 2:  # 40% or more went elsewhere: leave it to a person
            continue
        rules.append((old_dir, best))
    rules.sort(key=lambda r: len(r[0]), reverse=True)
    return rules, split


def relative_rules(
    moves: dict[str, str], base: str
) -> tuple[dict[str, str], list[tuple[str, str]]]:
    """The moves inside `base/`, spelled the way a build file inside it names them.

    `tests/CMakeLists.txt` lists `core/test_crc16.cpp` and adds `core/avx2/absent` as an include
    directory: paths relative to itself, which the repository-relative rules never see. Returns the
    file moves (old relative path -> new) and the directory rules of two or more components; a
    single name (`core`) is also an ordinary word in a build file, so it is left to a person.
    """
    prefix = base.rstrip("/") + "/"
    inside = {
        old[len(prefix) :]: new[len(prefix) :]
        for old, new in moves.items()
        if old.startswith(prefix) and new.startswith(prefix)
    }
    rules, _split = dir_rules(inside)
    return inside, [(old, new) for old, new in rules if "/" in old]


def path_index(files: list[str]) -> set[str]:
    """Every tracked file and every directory that holds one, as repository paths."""
    known = set(files)
    for f in files:
        parent = posixpath.dirname(f)
        while parent and parent not in known:
            known.add(parent)
            parent = posixpath.dirname(parent)
    return known


def rewrite_paths(
    text: str,
    moves: dict[str, str],
    dirs: list[tuple[str, str]] | None,
    hold: list[str] | None = None,
    known: set[str] | None = None,
) -> str:
    """Every full old repository path of the move map, and each directory rule, in `text`.

    A path is taken whole: it starts after a separator, a quote, `${CMAKE_SOURCE_DIR}/` or `../`,
    and does not go on with a name character, so `other/src/forge/x` and `src/forge/x_y` are left
    alone. The files come first, so that a moved file named at the end of a sentence is not taken
    for a directory's. `hold` lists directories whose files went to several libraries and that no
    rule covers: a bare mention of one is kept as it is, and not carried along by the rule of the
    directory above it. With `known` (path_index of the tree after the moves) a directory rule is
    applied to a path only when the result exists: `src/forge/src/spatial/` under the rule for
    `src/forge/src` would be `src/ac3/src/spatial/`, which is nowhere, so it stays as it was for a
    person to see. Used on build files here and on every other text file by n1b_paths.py.
    """
    for old in sorted(moves, key=len, reverse=True):
        if old in text:
            text = re.sub(_PATH_START + re.escape(old) + _PATH_END, moves[old], text)
    held: dict[str, str] = {}
    for i, kept in enumerate(hold or []):
        if kept in text:
            mark = f"held{i}"
            # only a mention that ends at the directory: the longer path that runs on through it
            # (`src/forge/src/internal/profiling/x`) has a rule of its own
            text, count = re.subn(
                _PATH_START + re.escape(kept) + _PATH_END + r"(?!/[\w.\-])", mark, text
            )
            if count:
                held[mark] = kept
    for old, new in dirs or []:
        if old not in text:
            continue
        if known is None:
            text = re.sub(_PATH_START + re.escape(old) + _PATH_END, new, text)
            continue

        def follow(m: re.Match, new: str = new) -> str:
            rest = m.group("rest")
            tail = len(rest) - len(rest.rstrip("."))  # a full stop after the path is not part of it
            target = new + (rest[: len(rest) - tail] if tail else rest)
            return new + rest if target in known else m.group(0)

        text = re.sub(
            _PATH_START + re.escape(old) + r"(?P<rest>(?:/[\w.\-]+)*)" + _PATH_END, follow, text
        )
    for mark, kept in held.items():
        text = text.replace(mark, kept)
    return text


def held_dirs(rules: list[tuple[str, str]], split: dict[str, dict[str, int]]) -> list[str]:
    """The split directories that dir_rules left for a person: no rule rewrites them."""
    applied = {old for old, _new in rules}
    return sorted(d for d in split if d not in applied)


def transform(
    text: str,
    moves: dict[str, str],
    dirs: list[tuple[str, str]] | None = None,
    relative: tuple[dict[str, str], list[tuple[str, str]]] | None = None,
    hold: list[str] | None = None,
    known: set[str] | None = None,
) -> str:
    for rx, b in _ALIAS_RX:
        text = rx.sub(b, text)
    for rx, r in _RAW_RX:
        text = rx.sub(r, text)
    text = MIXED.sub(r"iclforge::\2_\3", text)
    text = _OUT_RX.sub(lambda m: m.group(1) + OUTPUT.get(m.group(2), m.group(2)) + m.group(3), text)
    text = _PC_CHOICE_RX.sub(
        lambda m: m.group(1)
        + OUTPUT.get(m.group(2), m.group(2))
        + m.group(3)
        + OUTPUT.get(m.group(4), m.group(4)),
        text,
    )
    text = _PC_LIBNAME_RX.sub(lambda m: m.group(1) + OUTPUT.get(m.group(2), m.group(2)), text)
    for a, b in GENERATED:
        text = text.replace(a, b)
    text = rewrite_paths(text, moves, dirs, hold, known)
    if relative:
        rel_files, rel_dirs = relative
        for old in sorted(rel_files, key=len, reverse=True):
            if old in text:
                text = re.sub(_PATH_START + re.escape(old) + _PATH_END, rel_files[old], text)
        for old, new in rel_dirs:
            if old in text:
                text = re.sub(_PATH_START + re.escape(old) + _PATH_END, new, text)
    return text


def retire_layout_section(text: str) -> str:
    """The dependency table without its `layout` section, and the sentence of its comment about it.

    tools/checks/layering.json files a path under a library by rules for src/forge while the
    directory holds six libraries; once they are directories of their own the plain rule
    (`src/<library>/`) is right and the section is dead. It is the last key of the file.
    """
    newline = "\r\n" if "\r\n" in text else "\n"
    m = re.search(r',[ \t]*\r?\n[ \t]*"layout"[ \t]*:[ \t]*\{', text)
    if m:
        text = text[: m.start()] + newline + "}" + newline
    return re.sub(r" layout: how a path under src/.*?re-layout is done\.", "", text, flags=re.S)


def main() -> None:
    ap = base_parser(__doc__)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument(
        "--plan", required=True, help="the plan n1b_apply.py --json wrote before it moved the files"
    )
    a = ap.parse_args()
    root = Path(a.root)
    repo = Repo(a.root)
    moves = json.loads(Path(a.plan).read_text(encoding="utf-8"))["moves"]
    dirs, split = dir_rules(moves)
    hold = held_dirs(dirs, split)
    known = path_index(repo.files)
    print(
        f"move map: {len(moves)} files, {len(dirs)} directories "
        f"({len(split)} split between libraries)"
    )
    relative = {base: relative_rules(moves, base) for base in RELATIVE_BASES}
    changed = 0
    hits: dict[str, list[str]] = {}
    for f in repo.files:
        if f in SKIP_FILES or f.startswith(SKIP_PREFIX) or not BUILD_FILES.search(f):
            continue
        p = root / f
        try:
            text = p.read_bytes().decode("utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        if f == LAYERING_TABLE:
            out = retire_layout_section(text)
        else:
            out = transform(text, moves, dirs, relative.get(posixpath.dirname(f)), hold, known)
        for old_dir in split:
            if old_dir in out and re.search(_PATH_START + re.escape(old_dir) + _PATH_END, out):
                hits.setdefault(old_dir, []).append(f)
        if out != text:
            changed += 1
            if not a.dry_run:
                p.write_bytes(out.encode("utf-8"))
    print(f"{'would change' if a.dry_run else 'changed'} {changed} files")
    if hits:
        print("directories split between libraries and named in a build file (review each):")
        applied = {r[0] for r in dirs}
        for old_dir, files in sorted(hits.items()):
            s = split[old_dir]
            state = "rewritten to" if old_dir in applied else "left as is; candidate"
            print(
                f"  {old_dir}: {state} {s['to']} ({s['elsewhere']} of {s['files']} files went "
                f"elsewhere): {', '.join(files[:4])}"
            )


if __name__ == "__main__":
    main()
