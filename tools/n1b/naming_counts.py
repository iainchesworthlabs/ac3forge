"""Counts for the naming map: how many files and hits carry each family-scoped name.

Each row is a `git grep -c -I -E` over tracked files, optionally scoped to path globs. The second
pair of columns repeats the count outside docs/, planning/, CHANGELOG.md and ROADMAP.md (the files a
rewrite treats as history or prose). `distinct` counts the different identifiers the pattern
matched.

Usage: naming_counts.py [--root R] [--md out.md] [--json out.json]
"""

from __future__ import annotations

import contextlib
import json
import re
import subprocess
from collections import OrderedDict

from n1b_lib import Repo, base_parser, emit, md_table

CPP = ("*.cpp", "*.hpp", "*.h", "*.mm", "*.inl", "*.hpp.in", "*.h.in", "*.c")
CMAKE = ("*.cmake", "*CMakeLists.txt", "CMakePresets.json", "*.cmake.in", "cmake/*")
CI = (".github/*",)
ANY_IDENT = re.compile(r"(CONFIG_AC3FORGE_\w+|AC3FORGE_\w+|ac3forge_\w+|ac3::\w+|AC3\w*_EXPORT)")

# label: (pattern, pathspecs or None, distinct)
ROWS = OrderedDict(
    [
        ("C++ qualifier `ac3::`", (r"\bac3::", CPP, False)),
        ("C++ `namespace ac3` declarations", (r"^\s*namespace\s+ac3\b", CPP, False)),
        ("include root `ac3/`", (r"#\s*include\s*[<\"]ac3/", None, False)),
        ("include root `ac3forge_c/`", (r"[<\"]ac3forge_c/", None, False)),
        ("include roots `ac3iab/`, `ac3adm/`", (r"[<\"]ac3(iab|adm)/", None, False)),
        ("include roots `ac4/`, `ac4dec/`, `ac4enc/`", (r"[<\"]ac4(dec|enc)?/", None, False)),
        (
            "include roots `mp4/` `mpegts/` `matroska/` `iamf/`",
            (r"[<\"](mp4|mpegts|matroska|iamf)/", None, False),
        ),
        ("qualifier `ac4::`", (r"\bac4::", CPP, False)),
        (
            "qualifiers `mp4::` `mpegts::` `matroska::` `iamf::`",
            (r"\b(mp4|mpegts|matroska|iamf)::", CPP, False),
        ),
        ("qualifiers `ac3iab::` `ac3adm::`", (r"\bac3(iab|adm)::", CPP, False)),
        (
            "app namespaces `ac3cli` `ac3gui` `ac3probe` `ac3shield` `ac3fuzz` `ac3nullsink`",
            (r"\bnamespace\s+ac3(cli|gui|probe|shield|fuzz|nullsink)\b", None, False),
        ),
        (
            "C API identifiers `ac3forge_*` (C, C++, Rust, Python, JS, Kotlin)",
            (r"\bac3forge_[a-z0-9_]+", (*CPP, "*.rs", "*.py", "*.js", "*.ts", "*.kt"), True),
        ),
        ("macros `AC3FORGE_*` in C and C++", (r"\bAC3FORGE_[A-Z0-9_]+", CPP, True)),
        ("export macros `AC3*_EXPORT`", (r"\bAC3[A-Z]*_EXPORT\b", None, True)),
        (
            "CMake options and variables `AC3FORGE_*` (CMake files, presets, workflows)",
            (r"\bAC3FORGE_[A-Z0-9_]+", CMAKE + CI, True),
        ),
        ("CMake `find_package(ac3forge`", (r"find_package\(\s*ac3forge", None, False)),
        ("CMake targets `ac3::*` (CMake files, workflows)", (r"\bac3::[a-z_]+", CMAKE + CI, True)),
        (
            "CMake targets `forge_objects|static|shared`",
            (r"\bforge_(objects|static|shared)\b", CMAKE + CI + ("*.py", "*.sh", "*.ps1"), False),
        ),
        (
            "CMake targets `ac3audio` `ac3sendspin*` `ac3_arithmetic`",
            (r"\b(ac3audio|ac3sendspin\w*|ac3_arithmetic)\b", None, False),
        ),
        ("`project(ac3forge`", (r"project\(\s*ac3forge", None, False)),
        (
            "library file names `libac3forge*`, `ac3forge*.dll|lib|so|a|dylib`",
            (
                r"\b(libac3forge\w*|ac3forge(_static|_c|_minimal)?\.(dll|lib|so|a|dylib))",
                None,
                False,
            ),
        ),
        ("pkg-config names `ac3forge*.pc`", (r"\bac3forge[a-z_\-]*\.pc\b", None, False)),
        (
            "Python `import ac3forge` / `from ac3forge`",
            (r"\b(import|from)\s+_?ac3forge", None, False),
        ),
        ("Python `_ac3forge` extension module", (r"\b_ac3forge\b", None, False)),
        (
            "Rust `ac3forge` and `ac3forge-sys` under rust/",
            (r"\bac3forge(-sys|_sys)?\b", ("rust/*",), False),
        ),
        (
            "npm and wasm `Ac3Forge` factory and package names",
            (
                r"\b(createAc3Forge\w*|Ac3Forge\w*|@ac3forge/\w+)\b",
                ("js/*", "apps/wasm/*", "docs/*"),
                False,
            ),
        ),
        ("Kconfig `CONFIG_AC3FORGE_*`", (r"\bCONFIG_AC3FORGE_\w+", None, True)),
        ("ESP-IDF component path `esp-idf/ac3forge`", (r"esp-idf/ac3forge", None, False)),
        ("repository `iainchesworthlabs/ac3forge`", (r"iainchesworthlabs/ac3forge", None, False)),
        (
            "Pages URL `iainchesworthlabs.github.io/ac3forge`",
            (r"iainchesworthlabs\.github\.io/ac3forge", None, False),
        ),
        ("wire string `_ac3forge_player`", (r"_ac3forge_player", None, False)),
        ("OTA project name `ac3forge_hearth_sink`", (r"ac3forge_hearth_sink", None, False)),
        (
            "format and schema ids `ac3forge.<x>/<n>`",
            (r"\bac3forge\.[a-z_\-]+/[0-9]+", None, False),
        ),
        (
            "environment variables read at run time",
            (r"(getenv|environ|process\.env|\$ENV\{|env\[).{0,40}AC3FORGE_[A-Z0-9_]+", None, True),
        ),
    ]
)

DOC_EXCL = (
    ":(exclude)docs",
    ":(exclude)planning",
    ":(exclude)CHANGELOG.md",
    ":(exclude)ROADMAP.md",
    ":(exclude)docs-snippets",
)


def grep_counts(repo: Repo, pat: str, excl: bool, paths):
    cmd = ["git", "-C", str(repo.root), "grep", "-c", "-I", "-E", "-e", pat, "--"]
    cmd += list(paths) if paths else ["."]
    if excl:
        cmd += list(DOC_EXCL)
    out = subprocess.run(cmd, capture_output=True, check=False).stdout.decode("utf-8", "replace")
    files = hits = 0
    for line in out.splitlines():
        if ":" in line:
            files += 1
            with contextlib.suppress(ValueError):
                hits += int(line.rsplit(":", 1)[1])
    return files, hits


def grep_distinct(repo: Repo, pat: str, paths) -> int:
    cmd = ["git", "-C", str(repo.root), "grep", "-h", "-o", "-I", "-E", "-e", pat, "--"]
    cmd += list(paths) if paths else ["."]
    done = subprocess.run(cmd, capture_output=True, check=False)
    out = done.stdout.decode("utf-8", "replace").splitlines()
    names = set()
    for o in out:
        m = ANY_IDENT.search(o)
        names.add(m.group(1) if m else o)
    return len(names)


def main() -> None:
    ap = base_parser(__doc__)
    ap.add_argument("--json", default=None)
    ap.add_argument("--md", default=None)
    a = ap.parse_args()
    repo = Repo(a.root)
    rows = []
    data = {}
    for label, (pat, paths, distinct) in ROWS.items():
        f_all, h_all = grep_counts(repo, pat, False, paths)
        f_code, h_code = grep_counts(repo, pat, True, paths)
        d = grep_distinct(repo, pat, paths) if distinct else ""
        rows.append([label, f_all, h_all, f_code, h_code, d])
        data[label] = {
            "pattern": pat,
            "files": f_all,
            "hits": h_all,
            "files_code": f_code,
            "hits_code": h_code,
            "distinct": d,
        }
    text = md_table(
        [
            "name family",
            "files",
            "hits",
            "files (not docs/planning)",
            "hits (not docs/planning)",
            "distinct",
        ],
        rows,
        ["l", "r", "r", "r", "r", "r"],
    )
    if a.json:
        emit(json.dumps(data, indent=1), a.json)
    if a.md:
        emit(text, a.md)
    else:
        emit(text, a.out)


if __name__ == "__main__":
    main()
