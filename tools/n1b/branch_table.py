"""Render the open-branch measurements adapt_branch.ps1 wrote as planning/layout.md's table.

    branch_table.py --data <dir of result_*.json> [--repo <worktree>]

One row per branch: commits, files it touches, how many of those the scripts rewrite, and the files
that conflict when the branch is merged by hand and when the scripts have run on it first.
"""

from __future__ import annotations

import argparse
import json
import subprocess
from pathlib import Path

from n1b_lib import DEFAULT_ROOT


def git(repo: str, *args: str) -> str:
    result = subprocess.run(
        ["git", "-C", repo, *args], capture_output=True, text=True, encoding="utf-8", check=False
    )
    return result.stdout.strip()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--data", required=True, help="the directory adapt_branch.ps1 -Measure wrote")
    ap.add_argument(
        "--repo", default=DEFAULT_ROOT, help="a worktree that has the branches' commits"
    )
    a = ap.parse_args()
    results = []
    for path in sorted(Path(a.data).glob("result_*.json")):
        results.append(json.loads(path.read_text(encoding="utf-8-sig")))
    rows = []
    for d in sorted(results, key=lambda r: r["Branch"]):
        n = git(a.repo, "rev-list", "--count", f"{d['Base']}..{d['Tip']}")
        rows.append((d["Branch"], d, n))
        print(
            f"| `{d['Branch']}` (`{d['Tip']}`) | {n} | {d['Files']} | {d['ScriptTouchesOfThose']} "
            f"| {d['Naive']} | {d['ScriptFirst']} |"
        )
    print()
    for b, d, _n in rows:
        if d["Naive"]:
            print(f"- `{b}`, by hand: {d['NaivePaths'].replace(';', ', ')}")
        if d["ScriptFirst"]:
            print(f"- `{b}`, scripts first: {d['ScriptFirstPaths'].replace(';', ', ')}")


if __name__ == "__main__":
    main()
