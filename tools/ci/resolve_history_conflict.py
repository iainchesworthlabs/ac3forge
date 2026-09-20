#!/usr/bin/env python3
"""Resolve a rebase conflict between two runs of one trend publisher.

The four trend publishers (quality, performance, external-comparison and
object-quality) all commit to the `quality-history` branch, and each retries a
rejected push by rebasing onto whatever landed first. Between two *different*
publishers that rebase is conflict-free, because each writes its own files.
Between two runs of the *same* publisher it is not: both add records at the end
of the same `<branch>.jsonl`, and git sees two different last lines. That is
what failed main's "Publish quality trend" job on 2026-09-18 (run 35317914064):

    CONFLICT (content): Merge conflict in main.jsonl
    CONFLICT (content): Merge conflict in main.recent.jsonl

The records themselves do not disagree. Each run appends rows for its own
commit, so the answer is to keep both sides' rows and derive the sidecar window
again. This does that for every unmerged path git reports, leaving the caller
to run `git rebase --continue`:

- **`<name>.jsonl`**: the union. The side already on the branch first, in its
  own order, then the rows the replayed run adds that neither the merge base
  nor that side already carries. Rows are whole JSONL lines, one per
  (commit, series), so equal lines are the same record and are kept once.
- **`<name>.recent.jsonl`**: written again from the resolved full file by
  `append_quality_history.write_recent_window`, the same helper every append
  producer calls, so the window is what a single run would have produced.
- **Anything else** (the spectrogram PNGs, which each run overwrites rather
  than appends to): the replayed run's version, which is the later of the two.

It refuses anything it cannot account for - a `.jsonl` line that is not a
record with a commit, or a stage git does not offer - rather than writing a
guess into a measurement history. A refusal fails the publishing job, which is
what happened before this script existed.

    python3 tools/ci/resolve_history_conflict.py [--history-dir DIR]

Standard library only, like the append producers it sits beside.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

from append_quality_history import write_recent_window

RECENT_SUFFIX = ".recent.jsonl"


class Unresolvable(Exception):
    """A conflict this script will not guess at."""


def git(args: list[str], *, cwd: Path, capture: bool = True) -> bytes:
    """Run one git command in `cwd`, returning its standard output."""
    result = subprocess.run(
        ["git", *args],
        cwd=cwd,
        check=False,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE,
    )
    if result.returncode != 0:
        message = result.stderr.decode("utf-8", "replace").strip()
        raise Unresolvable(f"git {' '.join(args)} failed: {message}")
    return result.stdout if capture else b""


def unmerged_paths(history: Path) -> list[str]:
    """Every path git reports as unmerged, in git's own order."""
    out = git(["diff", "--name-only", "--diff-filter=U", "-z"], cwd=history)
    return [name for name in out.decode("utf-8").split("\0") if name]


def stage(history: Path, number: int, path: str) -> bytes | None:
    """One merge stage of `path`: 1 the merge base, 2 the side already on the
    branch, 3 the run being replayed. None when that side has no such file."""
    result = subprocess.run(
        ["git", "show", f":{number}:{path}"],
        cwd=history,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    return result.stdout if result.returncode == 0 else None


def records(text: str) -> list[str]:
    """One side's JSONL lines, blank ones dropped."""
    return [line.strip() for line in text.splitlines() if line.strip()]


def checked(lines: list[str], *, path: str) -> list[str]:
    """The lines this run would add, refused unless each is a record with a
    commit. Only the added side is checked: a line already on the branch stays
    whatever it is, since `write_recent_window` keeps a malformed line out of
    the window and the full file is authoritative."""
    for number, line in enumerate(lines, start=1):
        try:
            record = json.loads(line)
        except json.JSONDecodeError as error:
            raise Unresolvable(f"{path}: this run's line {number} is not JSON: {error}") from error
        if not isinstance(record, dict) or "commit" not in record:
            raise Unresolvable(f"{path}: this run's line {number} has no commit field")
    return lines


def union(base: list[str], ours: list[str], theirs: list[str]) -> tuple[list[str], list[str]]:
    """`ours` as it stands, then each of `theirs` that the merge base and
    `ours` do not already have, and those added lines on their own. Order
    within each side is kept: these files are read in the order they were
    written."""
    held = set(ours)
    dropped = set(base) - held
    added = []
    for line in theirs:
        if line in held or line in dropped:
            continue
        held.add(line)
        added.append(line)
    return ours + added, added


def resolve_jsonl(history: Path, path: str) -> None:
    """Write the union of both sides' records to `path`."""
    sides = {number: stage(history, number, path) for number in (1, 2, 3)}
    if sides[2] is None or sides[3] is None:
        raise Unresolvable(f"{path} is missing from one side of the rebase")
    base = records((sides[1] or b"").decode("utf-8"))
    ours = records(sides[2].decode("utf-8"))
    theirs = records(sides[3].decode("utf-8"))
    merged, added = union(base, ours, theirs)
    checked(added, path=path)
    (history / path).write_text("\n".join(merged) + ("\n" if merged else ""), encoding="utf-8")
    print(f"{path}: kept {len(ours)} record(s) already on the branch and added {len(added)} from "
          f"this run")


def resolve_other(history: Path, path: str) -> None:
    """Take the replayed run's version of a file each run overwrites."""
    content = stage(history, 3, path)
    if content is None:
        content = stage(history, 2, path)
    if content is None:
        raise Unresolvable(f"{path} has no side to take")
    (history / path).write_bytes(content)
    print(f"{path}: took this run's version")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--history-dir",
        type=Path,
        default=Path("."),
        help="the quality-history checkout where the rebase stopped (default: the "
             "working directory)",
    )
    args = parser.parse_args(argv)
    history = args.history_dir

    try:
        paths = unmerged_paths(history)
        if not paths:
            raise Unresolvable(
                "git reports no unmerged path, so the rebase stopped for some other reason"
            )
        # The sidecars are derived, so they are written from the full files
        # rather than merged: resolving the full files first means an unmerged
        # sidecar is already correct by the time it is staged.
        full = [path for path in paths if path.endswith(".jsonl")
                and not path.endswith(RECENT_SUFFIX)]
        for path in full:
            resolve_jsonl(history, path)
        for path in paths:
            if path in full or path.endswith(RECENT_SUFFIX):
                continue
            resolve_other(history, path)

        # A conflicted sidecar whose own full file merged cleanly still has to
        # be written again, so both sources of one are gathered here.
        sidecars = {path[: -len(RECENT_SUFFIX)] + ".jsonl"
                    for path in paths if path.endswith(RECENT_SUFFIX)}
        for path in sorted(set(full) | sidecars):
            write_recent_window(history / path)

        # Everything, rather than the conflicted paths: a sidecar this leaves
        # out of the window is deleted, and the checkout holds nothing else.
        git(["add", "-A", "--", "."], cwd=history)
    except Unresolvable as error:
        print(f"::error title=quality-history::{error}", file=sys.stderr)
        return 1
    print(f"Resolved {len(paths)} conflicted path(s); the rebase can continue.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
