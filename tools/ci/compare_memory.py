"""Compare a pull request's heap churn against its own merge base and render
the deltas as a GitHub job summary.

The memory counterpart to compare_performance.py, closing the same hole one
resource over. tools/ci/append_memory_history.py gates allocs/frame and
bytes/frame against a trailing mean, but the only job that builds or runs
ac3membench is persist-performance-trend, which is gated to pushes on main
(see .github/workflows/ci.yml). So a heap-churn regression is caught after it
has merged, on a commit where a red check blocks nothing and belongs to
whoever pushes next.

That is not hypothetical. E-AC-3 encode churn stepped 67 -> 199 allocs/frame
and Atmos 106 -> 219 at PR #352's per-channel exponent-run planner. The gate
fired exactly as designed:

    ##[error] linux-gcc/eac3_51_encode: 199.1 allocs/frame is 197% above the
             trailing 10-run mean (67.0)

on run 32823518403, event `push`, branch `main`, 2026-08-25 - after the merge.
The regression stayed in the tree and is now issue #544. This script asks the
same question at PR time, where the answer can still stop it.

WHY THIS IS A CHEAPER PRE-MERGE GATE THAN THE SPEED ONE. compare_performance.py
needs PERF_RUNS repetitions per side, an interleaved run order and a
minimum-of-N reduction, all of it to see a signal through a shared runner's
timing noise. Allocation counts have no such noise: for a fixed workload and a
fixed binary ac3membench counts the same allocations every time. One run per
side is the whole measurement, so this job runs its benchmarks once each and
spends its budget on the two builds.

TIERS ARE IMPORTED, NOT RESTATED. REGRESSION_GROWTH_FRACTION (20%, soft) and
HARD_REGRESSION_GROWTH_FRACTION (100%, hard), the per-metric absolute floors,
and the leak thresholds all come from append_memory_history.py, so a PR and the
push that eventually merges it cannot disagree about what counts as a
regression. The near-zero-baseline ladder is mirrored from that script's
check_churn for the same reason: a series sitting under its floor is judged by
the floor rather than by a ratio, because a climb from 2 to 6 allocations is
not the 200% a percentage would call it.

SOFT TIER ADVISORY, HARD TIER BLOCKING - the same split, and the same
mechanism: the hard verdict travels to a separate one-step gate job as a
$GITHUB_OUTPUT value, because the job that runs this script is
`continue-on-error: true` and that flag swallows exit codes. See
report_hard_regression below and ci.yml's memory-gate job.

The runner-noise argument that keeps compare_performance.py's soft tier
advisory does not apply here at all, and it is worth being clear that the soft
tier is advisory for a different reason: a 20% churn step can be a legitimate
trade (a lookup table that removes work, a buffer sized for the worst case),
and 20% of a small count is a handful of allocations. What stays true is that
any delta this script reports is a code change rather than a measurement
artefact, so even an unannotated row is worth a glance.

THE LEAK CHECK IS ATTRIBUTED TO THE PR, not evaluated on the head alone.
steady_live_growth is an absolute signal - bytes still held after ~199
steady-state frames - and needs no baseline to interpret, which is why
append_memory_history.py checks it against fixed thresholds. Doing that here
would annotate every pull request for a condition no pull request introduced:
measured on linux-gcc, the leg this gate runs on, three of the six workloads
retain bytes across their steady state and two of them (eac3_51_encode at
5,296 bytes and atmos_4obj_encode at 5,568) are already past the 4 KiB warn
threshold. A gate that fires on 100% of PRs
teaches people to stop reading it. So the thresholds stay absolute and stay
imported, and what is checked against them is what THIS branch did: crossing a
threshold the merge base was under, or growing by more than a threshold's worth
of retained bytes. The post-merge gate keeps the unconditional absolute view.

stdlib-only (json/argparse/pathlib), matching every other script in this
directory - the runner needs no provisioning beyond the Python it already has.
"""

import argparse
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from append_memory_history import (  # the single definition of every threshold
    CHURN_METRICS,
    HARD_REGRESSION_GROWTH_FRACTION,
    LIVE_GROWTH_HARD_BYTES,
    LIVE_GROWTH_WARN_BYTES,
    REGRESSION_GROWTH_FRACTION,
    load_leg_results,
)

HARD = "**HARD REGRESSION**"


def report_hard_regression(hard: bool) -> None:
    """Publish the verdict on $GITHUB_OUTPUT for ci.yml's memory-gate job.

    A step output rather than this script's own exit code, for the reason
    compare_performance.py's identical function spells out: the job that runs
    it is `continue-on-error: true` and must stay that way, because it builds
    twice and a vcpkg hiccup in a job nobody asked for should not block a PR.
    continue-on-error also discards the exit code, so a verdict carried that
    way would be silently dropped. A separate gate job that builds nothing
    reads this output and fails the check instead.

    A no-op outside Actions, so a local run prints its tables and exits 0.
    """
    path = os.environ.get("GITHUB_OUTPUT")
    if not path:
        return
    with open(path, "a", encoding="utf-8") as handle:
        handle.write("hard_regression=%s\n" % ("true" if hard else "false"))


def load_side(directory: Path):
    """{(leg, config): record} for one side of the comparison.

    Reuses append_memory_history.load_leg_results rather than reading
    ac3membench's JSON again here, so the pre-merge and post-merge gates cannot
    drift on either the results-tree layout (one memory-<leg> subdirectory per
    leg) or the benchmark's schema. A missing directory is an empty side rather
    than a crash - main() reports that as a skip.
    """
    if not directory.is_dir():
        return {}
    return {(rec["leg"], rec["config"]): rec for rec in load_leg_results(directory)}


def fmt(value: float) -> str:
    """Thousands-separated, one decimal. bytes_per_frame runs to six figures
    and `%.4g` would render it as 3.006e+04 in a table meant to be skimmed."""
    return f"{value:,.1f}"


def classify_churn(base: float, head: float, floor: float):
    """(tier, growth) for one metric on one workload.

    Mirrors append_memory_history.check_churn's ladder step for step, including
    its floor branch: under the floor the baseline is too small for a ratio to
    mean anything, so any climb past the floor is a soft finding and everything
    else is quiet. growth is None on that branch, since no ratio was computed.
    """
    if base <= floor:
        if head > floor and head > base:
            return "off a near-zero baseline", None
        return "unchanged" if head == base else ("lower" if head < base else "higher"), None

    growth = (head - base) / base
    if growth >= HARD_REGRESSION_GROWTH_FRACTION:
        return HARD, growth
    if growth >= REGRESSION_GROWTH_FRACTION:
        return "regression", growth
    if head == base:
        return "unchanged", growth
    return ("lower" if head < base else "higher"), growth


def classify_leak(base: int, head: int):
    """(tier, note) for one workload's steady_live_growth, or (None, None).

    Absolute thresholds, applied to what this branch changed - see the module
    docstring for why the head is not judged on its own. Two ways to earn a
    tier, and either alone is enough:

      - CROSSING. The head is past a threshold the merge base was under. This
        branch is the one that took retained bytes past 4 KiB, or past 1 MiB.
      - GROWING. Retained bytes rose by more than a threshold's worth. A
        workload already sitting at 5 KiB can climb to 900 KiB without ever
        crossing the hard threshold, and that is a leak worth the same tier.
    """
    growth = head - base
    if (head >= LIVE_GROWTH_HARD_BYTES > base) or growth >= LIVE_GROWTH_HARD_BYTES:
        return True, (f"{head:,} bytes still live after the steady-state frames, against "
                      f"{base:,} at the merge base - past the {LIVE_GROWTH_HARD_BYTES:,} "
                      "byte hard leak threshold, or grown by more than it.")
    if (head >= LIVE_GROWTH_WARN_BYTES > base) or growth >= LIVE_GROWTH_WARN_BYTES:
        return False, (f"{head:,} bytes still live after the steady-state frames, against "
                       f"{base:,} at the merge base - possible slow leak or unbounded "
                       "cache growth introduced by this branch.")
    return None, None


def churn_table(base_side, head_side, metric: str, label: str, floor: float):
    """One markdown table for one churn metric, plus the annotations it earned."""
    lines = [f"### {label[0].upper()}{label[1:]} (ac3membench)", "",
             "| workload | base | head | delta | |",
             "| --- | ---: | ---: | ---: | --- |"]
    annotations = []

    for key in sorted(set(base_side) | set(head_side)):
        leg, config = key
        name = f"{leg}/{config}"
        base_rec = base_side.get(key)
        head_rec = head_side.get(key)
        if base_rec is None:
            lines.append(f"| `{name}` | - | {fmt(head_rec[metric])} | new | added by this PR |")
            continue
        if head_rec is None:
            lines.append(f"| `{name}` | {fmt(base_rec[metric])} | - | gone | removed by this PR |")
            continue

        base = base_rec[metric]
        head = head_rec[metric]
        tier, growth = classify_churn(base, head, floor)
        delta = "-" if growth is None else f"{growth * 100:+.1f}%"
        lines.append(f"| `{name}` | {fmt(base)} | {fmt(head)} | {delta} | {tier} |")

        if tier == HARD:
            annotations.append(
                f"::warning title=Memory regression (hard tier)::{name}: {fmt(head)} {label} "
                f"vs {fmt(base)} at the merge base, {growth * 100:+.0f}%. Past the "
                f"{HARD_REGRESSION_GROWTH_FRACTION * 100:.0f}% threshold that fails the trend "
                "job once this merges - see docs/performance-trend.md.")
        elif tier == "regression":
            annotations.append(
                f"::warning title=Memory regression::{name}: {fmt(head)} {label} vs "
                f"{fmt(base)} at the merge base, {growth * 100:+.0f}%. Allocation counts are "
                "deterministic for a fixed workload, so this is a change in what the code "
                "allocates rather than runner noise.")
        elif tier == "off a near-zero baseline":
            annotations.append(
                f"::warning title=Memory regression::{name}: {label} climbed to {fmt(head)} "
                f"from {fmt(base)} at the merge base, which was at or under the {fmt(floor)} "
                "floor this metric is judged by. Something started allocating that didn't "
                "before.")

    lines.append("")
    return lines, annotations


def leak_table(base_side, head_side):
    """The steady_live_growth table and its annotations.

    Rendered only when some workload retains bytes on some side. On a tree
    where every workload ends its steady state where it started, six rows of
    zeroes say less than one line does.
    """
    rows = []
    annotations = []
    any_growth = False

    for key in sorted(set(base_side) & set(head_side)):
        leg, config = key
        name = f"{leg}/{config}"
        base = int(base_side[key]["steady_live_growth"])
        head = int(head_side[key]["steady_live_growth"])
        any_growth = any_growth or base != 0 or head != 0
        hard, note = classify_leak(base, head)
        tier = "-" if hard is None else (HARD if hard else "possible leak")
        rows.append(f"| `{name}` | {base:,} | {head:,} | {head - base:+,} | {tier} |")
        if note is not None:
            title = "Memory leak (hard tier)" if hard else "Memory leak"
            annotations.append(f"::warning title={title}::{name}: {note}")

    if not any_growth:
        return ["### Live bytes retained after the steady-state frames", "",
                "No workload retained bytes across its steady-state frames on either side.",
                ""], annotations

    lines = ["### Live bytes retained after the steady-state frames", "",
             "Absolute thresholds, applied to what this branch changed - a workload already "
             f"over the {LIVE_GROWTH_WARN_BYTES:,} byte line at the merge base is the merge "
             "base's finding, not this branch's. See tools/ci/compare_memory.py.", "",
             "| workload | base | head | delta | |",
             "| --- | ---: | ---: | ---: | --- |"]
    lines.extend(rows)
    lines.append("")
    return lines, annotations


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-dir", type=Path, required=True,
                        help="Merge-base results tree, one memory-<leg> subdirectory per leg.")
    parser.add_argument("--head-dir", type=Path, required=True,
                        help="PR-head results tree, same layout.")
    parser.add_argument("--base-ref", default="merge base",
                        help="Human-readable name for the baseline, for the summary heading.")
    parser.add_argument("--summary-out", type=Path,
                        help="Markdown output path. Defaults to $GITHUB_STEP_SUMMARY, or "
                             "stdout when that is unset (a local run).")
    args = parser.parse_args()

    base_side = load_side(args.base_dir)
    head_side = load_side(args.head_dir)

    if not base_side or not head_side:
        # Not a failure: a PR whose merge base predates ac3membench, or a
        # runner that could not build one side, should say so rather than fail
        # a job this script deliberately never fails.
        print("::warning title=Memory comparison skipped::no ac3membench results on one side "
              f"(base={len(base_side)} workload(s), head={len(head_side)}).")
        # Explicitly false rather than merely absent, so a skipped comparison
        # reads as "nothing to block on" instead of leaving the gate job to
        # interpret an unset output.
        report_hard_regression(False)
        return 0

    out = [f"## Memory vs {args.base_ref}", "",
           "One ac3membench run per side, same runner, same job. Lower is better. Allocation "
           "counts are deterministic for a fixed workload, so one run is the whole measurement "
           "and every delta below is a change in what the code allocates. A `regression` row "
           f"(at least {REGRESSION_GROWTH_FRACTION * 100:.0f}% more) is advisory. A "
           f"{HARD} row - at least "
           f"{HARD_REGRESSION_GROWTH_FRACTION * 100:.0f}% more, i.e. churn that at least "
           "doubled - fails the `Memory gate` check; label the PR "
           "`memory-regression-approved` if the increase is intended. See "
           "tools/ci/compare_memory.py.", ""]

    annotations = []
    for metric, label, floor in CHURN_METRICS:
        table, notes = churn_table(base_side, head_side, metric, label, floor)
        out.extend(table)
        annotations.extend(notes)

    table, notes = leak_table(base_side, head_side)
    out.extend(table)
    annotations.extend(notes)

    for note in annotations:
        print(note)

    # Counted across every table: a doubling of bytes/frame is as much a hard
    # regression as a doubling of allocs/frame, and so is a leak that crossed
    # the hard threshold on this branch.
    report_hard_regression(any("(hard tier)" in note for note in annotations))

    text = "\n".join(out) + "\n"
    destination = args.summary_out or (
        Path(os.environ["GITHUB_STEP_SUMMARY"]) if os.environ.get("GITHUB_STEP_SUMMARY") else None)
    if destination is None:
        sys.stdout.write(text)
    else:
        with destination.open("a", encoding="utf-8") as handle:
            handle.write(text)
        print(f"Wrote the comparison tables to {destination}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
