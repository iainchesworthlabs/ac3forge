"""Reflow, stage 3b of the plan: wrap the lines the namespace rewrite pushed past the column limit.

    n1b_reflow.py --root <worktree> [--base HEAD] [--limit 100] [--clang-format <exe>] [--dry-run]
                  [--until-stable]

n1b_names.py makes lines longer (`ac3::` becomes `iclforge::`, `mp4::` becomes `iclforge::mp4::`).
The style is the ColumnLimit of `.clang-format`; nothing in CI checks a C++ line (CONTRIBUTING.md
says "run it"), so the pass leaves what it pushed past the limit. This finds those lines, the added
lines of `git diff <base>` that are longer than `--limit` and whose old line was not, and gives them
to clang-format as `--lines` ranges, file by file. A line that was already past the limit before the
pass is left as it is, and so is every line clang-format was not asked about (`--sort-includes=
false` keeps the include lists as they are). What is still past the limit afterwards (a `//
clang-format off` table, a long token) is listed, for a person to wrap.

`--base` is the tree before the pass: `HEAD` while the pass is uncommitted, its parent once it is.
The result depends on the clang-format that runs (the README names the version of the S3 run), so
the reflow commit is the output of this script on the same machine and not a promise about another.
Files keep their line endings: clang-format derives them from the file. Given one line of a
statement that wraps over several, clang-format can leave the wrap of its neighbour for the next
pass; `--until-stable` runs the pass again while it changes a file (S6: two lines of 733 needed it),
so that a second run of the reflow on its own output changes nothing.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

from n1b_lib import CPP_EXT, base_parser

MAX_PASSES = 5
DEFAULT_CLANG_FORMAT = r"C:\Program Files\LLVM\bin\clang-format.exe"
HUNK_RE = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,\d+)? @@")
PATHSPEC = [f"*{ext}" for ext in sorted(CPP_EXT)]


def pushed_over(diff: str, limit: int) -> dict[str, list[int]]:
    """From a `git diff -U0` text: for each file, the new line numbers longer than `limit` whose
    old line (the one at the same place in the hunk) was not. An added line with no old line
    counts as new."""
    found: dict[str, list[int]] = {}
    path = ""
    old: list[str] = []
    new: list[str] = []
    first = 0

    def close() -> None:
        for k, line in enumerate(new):
            was = old[k] if k < len(old) else ""
            if len(line) > limit and len(was) <= limit:
                found.setdefault(path, []).append(first + k)
        old.clear()
        new.clear()

    for raw in diff.split("\n"):
        if raw.startswith("+++ "):
            close()
            path = raw[4:].removeprefix("b/")
        elif raw.startswith("--- "):
            close()
        elif raw.startswith("@@"):
            close()
            m = HUNK_RE.match(raw)
            first = int(m.group(1)) if m else 0
        elif raw.startswith("-"):
            old.append(raw[1:].rstrip("\r"))
        elif raw.startswith("+"):
            new.append(raw[1:].rstrip("\r"))
    close()
    return found


def ranges(lines: list[int]) -> list[tuple[int, int]]:
    out: list[tuple[int, int]] = []
    for n in sorted(set(lines)):
        if out and n <= out[-1][1] + 1:
            out[-1] = (out[-1][0], n)
        else:
            out.append((n, n))
    return out


def new_long_lines(before: bytes, after: bytes, limit: int) -> list[int]:
    """Line numbers of `after` that are longer than `limit` and have no equal line in `before`."""
    seen: dict[str, int] = {}
    for row in before.decode("utf-8", "replace").split("\n"):
        text = row.rstrip("\r")
        if len(text) > limit:
            seen[text] = seen.get(text, 0) + 1
    out: list[int] = []
    for i, row in enumerate(after.decode("utf-8", "replace").split("\n"), 1):
        text = row.rstrip("\r")
        if len(text) > limit:
            if seen.get(text, 0):
                seen[text] -= 1
            else:
                out.append(i)
    return out


def main() -> int:
    ap = base_parser(__doc__)
    ap.add_argument("--base", default="HEAD", help="the tree before the pass (default HEAD)")
    ap.add_argument("--limit", type=int, default=100, help=".clang-format's ColumnLimit")
    ap.add_argument("--clang-format", default=os.environ.get("CLANG_FORMAT", DEFAULT_CLANG_FORMAT))
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument(
        "--until-stable",
        action="store_true",
        help="run again while a pass changed a file (clang-format given one line at a time can "
        f"leave a neighbour's wrap for the next pass), up to {MAX_PASSES} passes",
    )
    a = ap.parse_args()
    for n in range(1, MAX_PASSES + 1):
        changed = reflow_once(Path(a.root), a)
        if not a.until_stable or a.dry_run or not changed or n == MAX_PASSES:
            return 0
        print(f"pass {n} changed {changed} files; again")
    return 0


def reflow_once(root: Path, a: argparse.Namespace) -> int:
    """One pass; the number of files it changed."""
    diff = subprocess.run(
        ["git", "-C", str(root), "diff", "-U0", "--no-color", a.base, "--", *PATHSPEC],
        capture_output=True,
        check=True,
    ).stdout.decode("utf-8", "replace")
    todo = pushed_over(diff, a.limit)
    changed = 0
    residual: list[str] = []
    for f, lines in sorted(todo.items()):
        source = (root / f).read_bytes()
        args = [a.clang_format, "--sort-includes=false", f"--assume-filename={f}"]
        args += [f"--lines={lo}:{hi}" for lo, hi in ranges(lines)]
        done = subprocess.run(args, input=source, capture_output=True, cwd=root, check=False)
        if done.returncode != 0:
            sys.exit(f"clang-format failed on {f}: {done.stderr.decode('utf-8', 'replace')}")
        formatted = done.stdout
        if formatted != source:
            changed += 1
            if not a.dry_run:
                (root / f).write_bytes(formatted)
        # A line that was long before the pass and was not asked about stays long: it is not the
        # pass's. What is long now and is not one of those was asked about and could not be wrapped.
        rows = source.decode("utf-8", "replace").split("\n")
        asked = set(lines)
        left = "\n".join(s for i, s in enumerate(rows, 1) if i not in asked).encode("utf-8")
        shown = formatted.decode("utf-8", "replace").split("\n")
        for n in new_long_lines(left, formatted, a.limit):
            residual.append(f"{f}:{n}: {shown[n - 1].strip()[:90]}")
    verb = "would reflow" if a.dry_run else "reflowed"
    count = sum(len(v) for v in todo.values())
    print(f"{count} lines over {a.limit} columns in {len(todo)} files; {verb} {changed} files")
    if residual:
        print(f"{len(residual)} of them are still over the limit, at:")
        for where in residual:
            print(f"  {where}")
    return changed


if __name__ == "__main__":
    sys.exit(main())
