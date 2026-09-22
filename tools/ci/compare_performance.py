"""Compare a pull request's encoder throughput against its own merge base and
render the deltas as a GitHub job summary.

Fills the hole between the two performance checks this repository already has.
tests/performance/test_performance.cpp's ac3perf is an ABSOLUTE real-time gate
and runs on every PR, but its budget carries enough headroom that a change
could double ms/frame and still pass. tools/ci/append_performance_history.py is
RELATIVE and would catch that, but it only ever runs on pushes to develop/main
(see .github/workflows/ci.yml's persist-performance-trend job) - so today a PR
that halves the encoder's speed is discovered after it has already merged.

This script is the relative check moved to PR time. It reads ac3bench and
ac3kernelbench JSON from two directories - one built at the merge base, one at
the PR head, on the same runner in the same job - and prints a markdown table
of per-workload and per-kernel deltas.

DELIBERATELY NON-BLOCKING. It always exits 0 on a successful comparison; a
regression is reported as a ::warning:: annotation and a row in the summary
table, never as a failed job. A shared CI runner's timings are noisy enough
that a hard PR-blocking threshold would cost more in re-runs than it caught,
and the two gates that DO block (ac3perf's absolute budget on every PR, the
trend appender's hard tier on merge) are unchanged by this. What this adds is
the number a reviewer needs in front of them at review time.

The soft and hard tiers are imported from append_performance_history.py rather
than restated, so a PR and its eventual merge cannot disagree about what counts
as a regression.

Reduction across the N repetitions of each benchmark is the MINIMUM, not the
mean. Timing noise on a shared runner is one-sided - a co-tenant can only ever
make a run slower, never faster - so the fastest of N is the closest estimate
of what the machine can do, and the mean would mostly measure the neighbours.
The spread is printed alongside so a reader can tell a real move from a noisy
one; a delta smaller than the base's own run-to-run spread is marked as such
instead of being dressed up as a finding.

stdlib-only (json/argparse/pathlib), matching every other script in this
directory - the runner needs no provisioning beyond the Python it already has.
"""

import argparse
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from append_performance_history import (  # the single definition of both tiers
    HARD_REGRESSION_SLOWDOWN_FRACTION,
    REGRESSION_SLOWDOWN_FRACTION,
)

# Rows whose delta is under this are reported as unchanged rather than as a
# tiny improvement/regression. Well below the soft tier and roughly the
# run-to-run floor a hosted runner manages even on a quiet build.
NOISE_FLOOR_FRACTION = 0.03


def load_runs(directory: Path, filename_glob: str, entries_key: str, value_key: str):
    """Return ({workload name: [value per run]}, file count) for `directory`.

    One JSON file per repetition, each holding the same workload names. The
    two producers this reads do NOT share a schema:
    bench_encoder.cpp's --json-out writes {"real_time_budget_ms_per_frame":
    ..., "results": [...]}, kernel_bench.cpp's writes {"kernels": [...]} - so
    the entries key has to come from the caller rather than be guessed at
    (an earlier version guessed, and a synthetic test fixture that happened
    to use a bare list masked the KeyError this caused on the real kernel
    JSON in CI).
    """
    runs: dict[str, list[float]] = {}
    files = sorted(directory.glob(filename_glob))
    for path in files:
        payload = json.loads(path.read_text())
        entries = payload[entries_key]
        for entry in entries:
            runs.setdefault(entry["name"], []).append(float(entry[value_key]))
    return runs, len(files)


def spread_fraction(values: list[float]) -> float:
    """(max - min) / min: this workload's own run-to-run noise on this runner."""
    low = min(values)
    return (max(values) - low) / low if low > 0 else 0.0


def classify(delta: float, base_spread: float) -> str:
    """Tier label for one row. Ordered worst-first, so a hard regression is
    never written off as noise just because the runner was also noisy."""
    if delta >= HARD_REGRESSION_SLOWDOWN_FRACTION:
        return "**HARD REGRESSION**"
    if delta >= REGRESSION_SLOWDOWN_FRACTION:
        return "regression"
    if abs(delta) <= max(NOISE_FLOOR_FRACTION, base_spread):
        return "unchanged"
    return "faster" if delta < 0 else "slower"


def compare(base_runs, head_runs, unit: str, label: str):
    """One markdown table, plus the annotations its rows earned."""
    lines = [f"### {label}", "",
             f"| workload | base {unit} | head {unit} | delta | |",
             "| --- | ---: | ---: | ---: | --- |"]
    annotations = []

    for name in sorted(set(base_runs) | set(head_runs)):
        base_values = base_runs.get(name)
        head_values = head_runs.get(name)
        if not base_values:
            lines.append(f"| `{name}` | - | {min(head_values):.4g} | new | added by this PR |")
            continue
        if not head_values:
            lines.append(f"| `{name}` | {min(base_values):.4g} | - | gone | removed by this PR |")
            continue

        base = min(base_values)
        head = min(head_values)
        delta = (head - base) / base if base > 0 else 0.0
        base_spread = spread_fraction(base_values)
        tier = classify(delta, base_spread)
        lines.append(f"| `{name}` | {base:.4g} | {head:.4g} | {delta * 100:+.1f}% | {tier} |")

        if tier == "**HARD REGRESSION**":
            annotations.append(
                f"::warning title=Performance regression (hard tier)::{name}: {head:.4g} {unit} "
                f"vs {base:.4g} at the merge base, {delta * 100:+.0f}%. Past the "
                f"{HARD_REGRESSION_SLOWDOWN_FRACTION * 100:.0f}% threshold that fails the "
                "trend job once this merges - see docs/performance-trend.md.")
        elif tier == "regression":
            annotations.append(
                f"::warning title=Performance regression::{name}: {head:.4g} {unit} vs "
                f"{base:.4g} at the merge base, {delta * 100:+.0f}% (this workload's own "
                f"run-to-run spread here was {base_spread * 100:.0f}%).")

    lines.append("")
    return lines, annotations


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-dir", type=Path, required=True,
                        help="Directory of merge-base result JSON, one file per repetition.")
    parser.add_argument("--head-dir", type=Path, required=True,
                        help="Directory of PR-head result JSON, one file per repetition.")
    parser.add_argument("--base-ref", default="merge base",
                        help="Human-readable name for the baseline, for the summary heading.")
    parser.add_argument("--summary-out", type=Path,
                        help="Markdown output path. Defaults to $GITHUB_STEP_SUMMARY, or "
                             "stdout when that is unset (a local run).")
    args = parser.parse_args()

    base_bench, base_n = load_runs(args.base_dir, "bench*.json", "results", "ms_per_frame")
    head_bench, head_n = load_runs(args.head_dir, "bench*.json", "results", "ms_per_frame")
    base_kern, _ = load_runs(args.base_dir, "kernels*.json", "kernels", "ns_per_call")
    head_kern, _ = load_runs(args.head_dir, "kernels*.json", "kernels", "ns_per_call")

    if not base_bench or not head_bench:
        # Not a failure: a PR whose merge base predates ac3bench, or a runner
        # that could not build one side, should say so rather than fail a job
        # this script deliberately never fails.
        print("::warning title=Performance comparison skipped::no ac3bench results on one "
              f"side (base={len(base_bench)} workloads from {base_n} run(s), "
              f"head={len(head_bench)} from {head_n}).")
        return 0

    out = [f"## Performance vs {args.base_ref}", "",
           f"Fastest of {base_n} run(s) at the base and {head_n} at the head, same runner, "
           "same job. Lower is better. This table is informational - it never fails the "
           "build; see tools/ci/compare_performance.py for why.", ""]

    annotations = []
    for base_runs, head_runs, unit, label in (
            (base_bench, head_bench, "ms/frame", "Whole-frame encode (ac3bench)"),
            (base_kern, head_kern, "ns/call", "Per-kernel (ac3kernelbench)")):
        if not base_runs and not head_runs:
            continue
        table, notes = compare(base_runs, head_runs, unit, label)
        out.extend(table)
        annotations.extend(notes)

    for note in annotations:
        print(note)

    text = "\n".join(out) + "\n"
    destination = args.summary_out or (
        Path(os.environ["GITHUB_STEP_SUMMARY"]) if os.environ.get("GITHUB_STEP_SUMMARY") else None)
    if destination is None:
        sys.stdout.write(text)
    else:
        with destination.open("a", encoding="utf-8") as handle:
            handle.write(text)
        print(f"Wrote the comparison table to {destination}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
