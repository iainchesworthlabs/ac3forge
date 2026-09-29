"""Everything keyed on a path: which tracked files name each directory, by category.

For every directory key (default: each src/<lib>, tests/<sub>, apps/<x>, and the other top-level
trees that could move) it runs `git grep` for the path followed by a non-word character, and files
the hits under a category (CMake, CI workflow, CI script, check script, docs, ...).

Usage: path_keyed.py [--root R] [--keys a,b,c] [--json out.json] [--md out.md] [--self]
  --self  count files under the key itself too (default: excluded, they move with it).
"""

from __future__ import annotations

import json
import re
import subprocess
from collections import defaultdict

from n1b_lib import Repo, base_parser, emit, md_table


def category(p: str) -> str:
    name = p.rsplit("/", 1)[-1]
    if p.startswith(".github/"):
        return "CI workflow / action"
    if p.startswith("tools/ci/"):
        return "CI script (tools/ci)"
    if p.startswith("tools/checks/"):
        return "check script (tools/checks)"
    if p.startswith("tools/"):
        return "other tools"
    if (
        name in ("CMakeLists.txt", "CMakePresets.json")
        or name.endswith(".cmake")
        or name.endswith(".cmake.in")
        or p.startswith("cmake/")
        or name.startswith("Kconfig")
        or name.endswith(".projbuild")
        or name in ("idf_component.yml", "component.mk")
    ):
        return "CMake / Kconfig"
    if p.startswith(("docs/", "docs-snippets/", "overrides/")) or p in (
        "mkdocs.yml",
        "README.md",
        "CONTRIBUTING.md",
        "SECURITY.md",
    ):
        return "docs"
    if p.startswith("planning/") or p in ("CHANGELOG.md", "ROADMAP.md"):
        return "planning / history (leave)"
    if p.startswith(("packaging/", "esphome/")):
        return "packaging"
    if p.startswith("esp-idf/"):
        return "esp-idf component"
    if p.startswith(("python/", "rust/", "js/")):
        return "bindings"
    if p.startswith("apps/"):
        return "apps"
    if p.startswith(("tests/", "fuzz/", "examples/")):
        return "tests / fuzz / examples"
    if p.startswith("src/"):
        return "src"
    return "repo config (root)"


def default_keys(repo: Repo) -> list[str]:
    keys = set()
    for f in repo.files:
        p = f.split("/")
        if len(p) < 3:
            continue
        if p[0] in (
            "src",
            "tests",
            "apps",
            "tools",
            "esp-idf",
            "rust",
            "python",
            "js",
            "packaging",
            "fuzz",
            "examples",
            "esphome",
            "cmake",
        ):
            keys.add(f"{p[0]}/{p[1]}")
    keys |= {
        "src/forge/include",
        "src/forge/src",
        "esp-idf/ac3forge",
        "rust/ac3forge",
        "rust/ac3forge-sys",
        "python/src",
        "js/src",
        "include/ac3",
    }
    return sorted(keys)


def main() -> None:
    ap = base_parser(__doc__)
    ap.add_argument("--keys", default=None)
    ap.add_argument("--json", default=None)
    ap.add_argument("--md", default=None)
    ap.add_argument("--self", action="store_true", dest="include_self")
    a = ap.parse_args()
    repo = Repo(a.root)
    keys = a.keys.split(",") if a.keys else default_keys(repo)
    result = {}
    for k in keys:
        pat = re.escape(k) + r"([^A-Za-z0-9_]|$)"
        out = subprocess.run(
            ["git", "-C", str(repo.root), "grep", "-c", "-I", "-E", pat],
            capture_output=True,
            check=False,
        ).stdout.decode("utf-8", "replace")
        hits = {}
        for line in out.splitlines():
            if ":" not in line:
                continue
            path, n = line.rsplit(":", 1)
            if not a.include_self and (path == k or path.startswith(k + "/")):
                continue
            hits[path] = int(n)
        by_cat = defaultdict(dict)
        for p, n in hits.items():
            by_cat[category(p)][p] = n
        result[k] = {
            "files": len(hits),
            "hits": sum(hits.values()),
            "by_category": dict(sorted(by_cat.items())),
        }
    if a.json:
        emit(json.dumps(result, indent=1), a.json)
    lines = [
        md_table(
            ["key (old path)", "files naming it", "hits"],
            [[k, v["files"], v["hits"]] for k, v in result.items()],
            ["l", "r", "r"],
        )
    ]
    text = "".join(lines)
    if a.md:
        emit(text, a.md)
    else:
        emit(text, a.out)


if __name__ == "__main__":
    main()
