#!/usr/bin/env python3
"""The blocking half of the merge queue's comparisons (_compare.yml's two gate jobs).

    python3 tools/ci/compare_gate.py --kind performance --verdict "$VERDICT" \\
        --repo "$REPO" --pr "$PR_NUMBER"

The comparison jobs build and measure with continue-on-error, so a flaky build never
blocks a merge, and hand their verdict over as an output: `true` when a workload is at
least twice as slow (or its heap churn at least doubled), empty when the job was
skipped, cancelled or died before it had a measurement. This script turns that verdict
into a pass or a failure:

  not `true`     pass. A job that produced no measurement must not fail an entry on a
                 measurement it does not have.
  `true`, and the pull request carries the kind's approval label
                 pass, with a warning that names the label. The summary table still
                 shows the hard row; the label only stops it failing.
  `true`, no label
                 fail.

The labels are `perf-regression-approved` and `memory-regression-approved`, read from the
pull request the queue entry belongs to (`--pr`, taken from the merge group's ref by
pr-gate.yml). When they cannot be read, after three attempts, the regression counts as
not approved: the entry fails and is queued again once the API answers, which costs one
comparison, while letting a real doubling through on a hiccup costs a regression on main.
An empty `--pr` (a dispatch that named no pull request) has nothing to approve it.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from collections.abc import Callable, Sequence
from dataclasses import dataclass

Runner = Callable[[Sequence[str]], str]


class CommandError(Exception):
    def __init__(self, cmd: Sequence[str], returncode: int, stderr: str):
        super().__init__(f"{' '.join(cmd[:3])}... exited {returncode}: {stderr.strip()[:200]}")
        self.stderr = stderr


def shell(cmd: Sequence[str]) -> str:
    res = subprocess.run(list(cmd), capture_output=True, text=True, encoding="utf-8", check=False)
    if res.returncode != 0:
        raise CommandError(cmd, res.returncode, res.stderr)
    return res.stdout


@dataclass(frozen=True)
class Kind:
    title: str  # "performance": the word the annotations use
    label: str  # the pull request label that approves a hard regression
    summary: str  # the comparison job whose summary says which workload and by how much
    what: str  # what got worse


KINDS = {
    "performance": Kind(
        title="performance",
        label="perf-regression-approved",
        summary="Performance vs base",
        what="A workload takes at least twice as long as at the base",
    ),
    "memory": Kind(
        title="memory",
        label="memory-regression-approved",
        summary="Memory vs base",
        what=(
            "A workload's heap churn at least doubled against the base, "
            "or it started retaining megabytes it did not before"
        ),
    ),
}

ATTEMPTS = 3
PAUSE_SECONDS = 5.0


def read_labels(
    sh: Runner, repo: str, pr: str, *, attempts: int = ATTEMPTS, pause: float = PAUSE_SECONDS,
    sleep: Callable[[float], None] = time.sleep,
) -> list[str]:  # fmt: skip
    """The label names on a pull request. Raises CommandError after `attempts` failures."""
    failure: CommandError | None = None
    cmd = ["gh", "api", "--paginate", f"repos/{repo}/issues/{pr}/labels", "--jq", ".[].name"]
    for attempt in range(attempts):
        try:
            out = sh(cmd)
        except CommandError as e:
            failure = e
            if attempt + 1 < attempts:
                sleep(pause)
            continue
        return out.splitlines()
    assert failure is not None
    raise failure


def gate(kind: Kind, verdict: str, repo: str, pr: str, sh: Runner = shell, **retry) -> int:
    """Print what the gate found, as workflow annotations, and return the exit status."""
    print(f"hard_regression={verdict or '<none>'}  pull request={pr or '<none>'}")
    if verdict != "true":
        print(f"No hard {kind.title} regression to gate on.")
        return 0

    approved = False
    unreadable = ""
    if pr:
        try:
            approved = kind.label in read_labels(sh, repo, pr, **retry)
        except CommandError as e:
            unreadable = (e.stderr.strip().splitlines() or [str(e)])[0][:160]
    if approved:
        print(
            f"::warning title=Hard {kind.title} regression (approved)::{kind.what}. Allowed "
            f"through by the '{kind.label}' label - see the '{kind.summary}' job summary for "
            "which workload and by how much."
        )
        return 0

    if unreadable:
        why = (
            f" The labels of pull request {pr} could not be read ({unreadable}), so it counts "
            "as not approved: queue the pull request again."
        )
    elif not pr:
        why = " No pull request was named, so nothing can approve it."
    else:
        why = (
            f" If it is deliberate, label pull request {pr} '{kind.label}' and put it back "
            "in the merge queue."
        )
    print(
        f"::error title=Hard {kind.title} regression::{kind.what}. See the '{kind.summary}' job "
        "summary for which one and by how much. This is the same 100% threshold that fails "
        f"the trend job on main once merged.{why}"
    )
    return 1


def main(argv: Sequence[str], sh: Runner = shell) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--kind", choices=sorted(KINDS), required=True)
    parser.add_argument("--verdict", default="", help="the comparison job's hard_regression output")
    parser.add_argument("--repo", default="")
    parser.add_argument("--pr", default="", help="the pull request whose labels approve")
    args = parser.parse_args(list(argv))
    return gate(KINDS[args.kind], args.verdict, args.repo, args.pr, sh)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
