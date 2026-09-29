"""What the TrueHD branch's files include from outside themselves, and where it lands in L2.

    truehd_includes.py [--repo <worktree>] [--branch github/feature/truehd-atmos-support]

The branch adds `src/forge/{include/ac3,src}/mlp/`; its files are read with `git show`, so nothing
is checked out. A header of the old tree is mapped to its L2 library with layoutdef.l2_new (the
export header, generated per library, is counted apart). Output is the text appendix.py puts in B.6.
"""

from __future__ import annotations

import argparse
import re
import subprocess
from collections import Counter

import layoutdef
from n1b_lib import DEFAULT_ROOT

MLP = re.compile(r"^src/forge/(include/ac3|src)/mlp/")
INC = re.compile(r'^\s*#\s*include\s+"([^"]+)"')


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo", default=DEFAULT_ROOT, help="a worktree that has the branch's commits")
    ap.add_argument("--branch", default="github/feature/truehd-atmos-support")
    ap.add_argument("--main", default="github/main", help="the ref the branch is measured against")
    a = ap.parse_args()

    def git(*args: str) -> str:
        return subprocess.run(
            ["git", "-C", a.repo, *args],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        ).stdout

    tip = git("rev-parse", "--short", "--verify", "--quiet", a.branch).strip()
    if not tip:
        raise SystemExit(f"truehd_includes: {a.branch} is not a ref of {a.repo}; fetch it first")
    base = git("merge-base", a.main, a.branch).strip()
    files = [f for f in git("diff", "--name-only", base, a.branch).splitlines() if MLP.match(f)]
    main_files = set(git("ls-tree", "-r", "--name-only", a.main).splitlines())
    counts: Counter = Counter()
    target: dict[str, str] = {}
    for f in files:
        for line in git("show", f"{a.branch}:{f}").splitlines():
            m = INC.match(line)
            if not m:
                continue
            spelling = m.group(1)
            if not spelling.startswith("ac3/") or spelling.startswith("ac3/mlp/"):
                continue
            counts[spelling] += 1
            if spelling == "ac3/export.hpp":
                target[spelling] = "its own library's export header"
                continue
            old = "src/forge/include/" + spelling
            if old not in main_files:
                target[spelling] = "(not on main)"
                continue
            new = layoutdef.l2_new(old)
            target[spelling] = new.split("/")[1] if new else "ac3"
    print(
        f"The {len(files)} `mlp` files of `feature/truehd-atmos-support` (tip {tip}, merge base "
        f"with main {base[:9]}) include these headers from outside `mlp`, with the library each "
        "lands in under L2:"
    )
    print()
    print("| header (old spelling) | directives | library under L2 |")
    print("| --- | ---: | --- |")
    for spelling, n in counts.most_common():
        print(f"| `{spelling}` | {n} | {target[spelling]} |")


if __name__ == "__main__":
    main()
