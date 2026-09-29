"""Mechanical CMake and script renames: target names, output names and moved file paths.

    n1b_cmake.py --root <worktree> [--dry-run]

Applies, to every tracked build or script file (CMake, presets, workflows, tools/**, packaging
scripts) outside docs/, planning/ and the two history files:
  * library aliases:      ac3::forge -> iclforge::ac3, mp4::mp4 -> iclforge::mp4,
                          ac4::decoder -> iclforge::ac4dec ...
  * raw target names:     forge_static -> iclforge_ac3_static,
                          mp4_objects -> iclforge_mp4_objects ...
  * output (file) names:  OUTPUT_NAME "ac3forge" -> "iclforge_ac3", "mp4" -> "iclforge_mp4" ...
  * moved file paths:     every full old path in the move map that appears in the text
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
)
SKIP_FILES = {"CHANGELOG.md", "ROADMAP.md", "README.md", "CONTRIBUTING.md", "SECURITY.md"}

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
    (r"(?<![\w:.\-/])ac4core(?![\w:.\-/])", "iclforge_ac4core"),
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


MIXED = re.compile(
    r"\b(ac3|ac4)::iclforge_(ac3|ac4dec|ac4enc)_(objects|static|shared)\b"
)  # a raw name inside an old alias
# A repository-relative path starts here: after a separator or quote, or straight after a
# `${CMAKE_SOURCE_DIR}/`-style prefix.
_PATH_START = r"(?:(?<![\w/.\-])|(?<=\}/))"


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


def transform(text: str, moves: dict[str, str], dirs: list[tuple[str, str]] | None = None) -> str:
    for rx, b in _ALIAS_RX:
        text = rx.sub(b, text)
    for rx, r in _RAW_RX:
        text = rx.sub(r, text)
    text = MIXED.sub(r"iclforge::\2_\3", text)
    text = _OUT_RX.sub(lambda m: m.group(1) + OUTPUT.get(m.group(2), m.group(2)) + m.group(3), text)
    for a, b in GENERATED:
        text = text.replace(a, b)
    for old in sorted(moves, key=len, reverse=True):
        if old in text:
            text = re.sub(_PATH_START + re.escape(old) + r"(?![\w.\-])", moves[old], text)
    for old, new in dirs or []:
        if old in text:
            text = re.sub(_PATH_START + re.escape(old) + r"(?![\w.\-])", new, text)
    return text


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
    print(
        f"move map: {len(moves)} files, {len(dirs)} directories "
        f"({len(split)} split between libraries)"
    )
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
        out = transform(text, moves, dirs)
        for old_dir in split:
            if old_dir in text and re.search(
                _PATH_START + re.escape(old_dir) + r"(?![\w.\-])", text
            ):
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
