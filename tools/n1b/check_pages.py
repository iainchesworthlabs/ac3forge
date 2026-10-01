#!/usr/bin/env python3
"""Hold the pages that name headers and CMake targets to the tree.

check_doc_paths.py reads the repository paths a page names (`src/ac3/include/...`). A page also
names a header by the spelling a program includes it by (`iclforge/base/layout.hpp`) and a library
by its target (`iclforge::dsp`), and neither is a path, so after a rename of the libraries nothing
else asks whether those spellings reach a file. This does, for every tracked page outside the
history:

  - each backticked `iclforge/<library>/<file>` and `iclforge_c/<file>` is a header of a library
    (under `src/<library>/include/` or its `variants/`), of the ESP-IDF component, or an export or
    version header the build generates;
  - each backticked `iclforge::<name>` that names a library (`iclforge::dsp`,
    `iclforge::ac3_static`) is a target a CMake file makes;
  - every library of tools/checks/layering.json has a header directory, and the library index
    (docs/library/index.md) names its target.

    python tools/n1b/check_pages.py [--root <worktree>]

The exit status is 1 when a page names something that is not there. `iclforge::mlp` is the library
of a branch that has not merged, and is allowed.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

PLANNED = {"iclforge::mlp"}
HISTORY = (
    "planning/",
    "CHANGELOG.md",
    "tools/n1b/",
    "tests/golden/",
    "docs/history.md",
    "docs/renamed.md",
)
HEADER_RX = re.compile(r"`(iclforge(?:_c)?/[A-Za-z0-9_./-]+\.(?:hpp|h))`")
TARGET_RX = re.compile(r"`(iclforge::[a-z0-9_]+)`")
GENERATED_RX = re.compile(r"^iclforge(?:/[a-z0-9_]+)?/(?:export|version|config)\.(?:hpp|h)$")
NAME_OF = {
    "capi": "c"
}  # the library `capi` is the target iclforge::c and the header root iclforge_c/


def tracked(root: Path) -> list[str]:
    out = subprocess.run(
        ["git", "-C", str(root), "-c", "core.quotepath=off", "ls-files"],
        capture_output=True,
        text=True,
        encoding="utf-8",
        check=True,
    ).stdout
    return out.splitlines()


def libraries(root: Path) -> list[str]:
    table = json.loads((root / "tools/checks/layering.json").read_text(encoding="utf-8"))
    return sorted(table["libraries"])


def headers_of(files: list[str]) -> set[str]:
    found: set[str] = set()
    for f in files:
        if not f.endswith((".hpp", ".h", ".in")):
            continue
        m = re.match(r"src/[^/]+/(?:include|variants/[^/]+)/(iclforge(?:_c)?/.+)$", f)
        m = m or re.match(r"esp-idf/iclforge/include/(iclforge/.+)$", f)
        if m:
            found.add(m.group(1).removesuffix(".in"))
    return found


def targets_of(root: Path, files: list[str], libs: list[str]) -> set[str]:
    text = ""
    for f in files:
        if f.endswith(("CMakeLists.txt", ".cmake", ".cmake.in")):
            text += (root / f).read_text(encoding="utf-8", errors="replace") + "\n"
    alias = r"add_library\(\s*(iclforge::[A-Za-z0-9_]+)\s+(?:ALIAS|INTERFACE|IMPORTED)"
    made = set(re.findall(alias, text))
    for lib in re.findall(r"iclforge_add_library\(\s*([a-z0-9_]+)", text):
        made |= {f"iclforge::{lib}", f"iclforge::{lib}_static", f"iclforge::{lib}_shared"}
    for lib in libs:
        name = NAME_OF.get(lib, lib)
        cmake = root / f"src/{lib}/CMakeLists.txt"
        body = cmake.read_text(encoding="utf-8", errors="replace") if cmake.is_file() else ""
        for suffix in ("", "_static", "_shared"):
            if f"iclforge::{name}{suffix}" in body:
                made.add(f"iclforge::{name}{suffix}")
    return made


def looks_like_library(tail: str, libs: list[str]) -> bool:
    names = {NAME_OF.get(lib, lib) for lib in libs}
    return tail in names or tail.endswith(("_static", "_shared", "_objects")) or tail == "forge"


def header_root(lib: str) -> str:
    return "iclforge_c/" if lib == "capi" else f"iclforge/{lib}/"


def check(root: Path) -> list[str]:
    files = tracked(root)
    libs = libraries(root)
    headers = headers_of(files)
    targets = targets_of(root, files, libs)
    problems: list[str] = []
    index = root / "docs/library/index.md"
    index_text = index.read_text(encoding="utf-8", errors="replace") if index.is_file() else ""
    for lib in libs:
        if not any(h.startswith(header_root(lib)) for h in headers):
            problems.append(f"library {lib}: no header under src/{lib}/include/{header_root(lib)}")
        target = f"iclforge::{NAME_OF.get(lib, lib)}"
        if target not in index_text:
            problems.append(f"docs/library/index.md: the library {lib} is not named ({target})")
    for f in files:
        if f.startswith(HISTORY) or not f.endswith(".md"):
            continue
        text = (root / f).read_text(encoding="utf-8", errors="replace")
        for number, line in enumerate(text.splitlines(), 1):
            for m in HEADER_RX.finditer(line):
                header = m.group(1)
                if header not in headers and not GENERATED_RX.match(header):
                    problems.append(f"{f}:{number}: no such header: {header}")
            for m in TARGET_RX.finditer(line):
                target = m.group(1)
                tail = target.split("::", 1)[1]
                if looks_like_library(tail, libs) and target not in targets | PLANNED:
                    problems.append(f"{f}:{number}: no CMake target makes {target}")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--root", default=str(Path(__file__).resolve().parents[2]))
    args = ap.parse_args()
    problems = check(Path(args.root))
    for p in problems:
        print(p)
    print(f"check_pages: {len(problems)} problem(s)", file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
