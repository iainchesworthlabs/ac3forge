"""Unit tests for compare_performance.py, the PR-time throughput gate.

The regression this exists to catch is a pull request that doubles the
encoder's (or one kernel's) time per frame: it must publish
hard_regression=true on $GITHUB_OUTPUT, because that output - not this
script's exit code - is what ci.yml's performance-gate job fails on. A 20%
slowdown must only warn, noise must read as unchanged, and a missing side
must read as an explicit hard_regression=false rather than an unset output.

stdlib unittest only; ac3bench/ac3kernelbench JSON is synthesised in a temp
dir in the two (different) schemas the real producers write.

Run: python3 -m unittest discover -s tools/ci -p 'test_*.py'
"""

import contextlib
import io
import json
import os
import sys
import tempfile
import unittest
import unittest.mock as mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import append_performance_history as aph
import compare_performance as cp


def write_bench(directory: Path, index: int, workloads: dict) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    (directory / f"bench{index}.json").write_text(json.dumps({
        "real_time_budget_ms_per_frame": 32.0,
        "results": [{"name": n, "ms_per_frame": v} for n, v in workloads.items()]}))


def write_kernels(directory: Path, index: int, kernels: dict) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    (directory / f"kernels{index}.json").write_text(json.dumps({
        "kernels": [{"name": n, "ns_per_call": v} for n, v in kernels.items()]}))


class Classify(unittest.TestCase):
    def test_tiers_are_ordered_worst_first(self):
        self.assertEqual(cp.classify(1.0, 0.0), "**HARD REGRESSION**")
        # A hard regression is never excused by a noisy runner.
        self.assertEqual(cp.classify(1.5, 5.0), "**HARD REGRESSION**")
        self.assertEqual(cp.classify(0.2, 0.0), "regression")
        self.assertEqual(cp.classify(0.02, 0.0), "unchanged")
        self.assertEqual(cp.classify(0.10, 0.15), "unchanged")  # inside base spread
        self.assertEqual(cp.classify(-0.10, 0.0), "faster")
        self.assertEqual(cp.classify(0.10, 0.0), "slower")

    def test_spread_fraction(self):
        self.assertAlmostEqual(cp.spread_fraction([1.0, 1.5, 1.2]), 0.5)
        self.assertEqual(cp.spread_fraction([0.0, 1.0]), 0.0)

    def test_thresholds_are_shared_with_the_post_merge_gate(self):
        self.assertIs(cp.HARD_REGRESSION_SLOWDOWN_FRACTION, aph.HARD_REGRESSION_SLOWDOWN_FRACTION)
        self.assertIs(cp.REGRESSION_SLOWDOWN_FRACTION, aph.REGRESSION_SLOWDOWN_FRACTION)


class LoadRuns(unittest.TestCase):
    def test_collects_every_repetition_per_workload(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            write_bench(d, 1, {"a": 2.0, "b": 1.0})
            write_bench(d, 2, {"a": 1.5, "b": 1.1})
            runs, n = cp.load_runs(d, "bench*.json", "results", "ms_per_frame")
            self.assertEqual(n, 2)
            self.assertEqual(runs, {"a": [2.0, 1.5], "b": [1.0, 1.1]})


class Compare(unittest.TestCase):
    def test_new_and_removed_workloads_are_rows_not_errors(self):
        lines, notes = cp.compare({"old": [1.0]}, {"new": [2.0]}, "ms/frame", "X")
        text = "\n".join(lines)
        self.assertIn("| `new` | - | 2 | new |", text)
        self.assertIn("| `old` | 1 | - | gone |", text)
        self.assertEqual(notes, [])

    def test_minimum_of_n_is_compared(self):
        """One slow outlier on the head side must not fake a regression."""
        lines, notes = cp.compare({"w": [1.0, 1.02]}, {"w": [5.0, 1.01]}, "ms/frame", "X")
        self.assertIn("unchanged", lines[-2])
        self.assertEqual(notes, [])

    def test_zero_base_does_not_divide(self):
        lines, _ = cp.compare({"w": [0.0]}, {"w": [1.0]}, "ns/call", "X")
        self.assertIn("+0.0%", lines[-2])


class Main(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.base, self.head = self.tmp / "base", self.tmp / "head"
        self.output = self.tmp / "gh_output"

    def tearDown(self):
        self._tmp.cleanup()

    def run_main(self, *extra, env=None):
        argv = ["compare_performance.py", "--base-dir", str(self.base),
                "--head-dir", str(self.head), *extra]
        environ = {"GITHUB_OUTPUT": str(self.output)}
        environ.update(env or {})
        buf = io.StringIO()
        with mock.patch.object(sys, "argv", argv), \
                mock.patch.dict(os.environ, environ, clear=False), \
                contextlib.redirect_stdout(buf):
            if "GITHUB_STEP_SUMMARY" not in environ:
                os.environ.pop("GITHUB_STEP_SUMMARY", None)
            rc = cp.main()
        verdict = self.output.read_text() if self.output.exists() else ""
        return rc, buf.getvalue(), verdict

    def test_hard_regression_is_published_and_never_exits_nonzero(self):
        write_bench(self.base, 1, {"ac3_51": 1.0})
        write_bench(self.head, 1, {"ac3_51": 2.1})
        rc, out, verdict = self.run_main()
        self.assertEqual(rc, 0)
        self.assertEqual(verdict, "hard_regression=true\n")
        self.assertIn("::warning title=Performance regression (hard tier)::ac3_51", out)
        self.assertIn("**HARD REGRESSION**", out)

    def test_kernel_doubling_alone_is_a_hard_regression(self):
        write_bench(self.base, 1, {"w": 1.0})
        write_bench(self.head, 1, {"w": 1.0})
        write_kernels(self.base, 1, {"mdct": 100.0})
        write_kernels(self.head, 1, {"mdct": 250.0})
        rc, out, verdict = self.run_main("--base-ref", "abc123")
        self.assertEqual(rc, 0)
        self.assertEqual(verdict, "hard_regression=true\n")
        self.assertIn("## Performance vs abc123", out)
        self.assertIn("Per-kernel (ac3kernelbench)", out)

    def test_soft_regression_warns_without_blocking(self):
        write_bench(self.base, 1, {"w": 1.0})
        write_bench(self.head, 1, {"w": 1.3})
        _rc, out, verdict = self.run_main()
        self.assertEqual(verdict, "hard_regression=false\n")
        self.assertIn("::warning title=Performance regression::w", out)
        self.assertNotIn("hard tier", out)

    def test_missing_side_skips_with_explicit_false(self):
        write_bench(self.base, 1, {"w": 1.0})
        self.head.mkdir()
        rc, out, verdict = self.run_main()
        self.assertEqual(rc, 0)
        self.assertEqual(verdict, "hard_regression=false\n")
        self.assertIn("Performance comparison skipped", out)

    def test_summary_goes_to_step_summary_file(self):
        write_bench(self.base, 1, {"w": 1.0})
        write_bench(self.head, 1, {"w": 0.5})
        summary = self.tmp / "summary.md"
        _rc, out, verdict = self.run_main(env={"GITHUB_STEP_SUMMARY": str(summary)})
        self.assertEqual(verdict, "hard_regression=false\n")
        self.assertIn("faster", summary.read_text())
        self.assertIn("Wrote the comparison table", out)

    def test_summary_out_overrides_and_local_run_writes_no_output(self):
        write_bench(self.base, 1, {"w": 1.0})
        write_bench(self.head, 1, {"w": 1.0})
        summary = self.tmp / "explicit.md"
        argv = ["compare_performance.py", "--base-dir", str(self.base),
                "--head-dir", str(self.head), "--summary-out", str(summary)]
        env = {k: v for k, v in os.environ.items()
               if k not in ("GITHUB_OUTPUT", "GITHUB_STEP_SUMMARY")}
        with mock.patch.object(sys, "argv", argv), mock.patch.dict(os.environ, env, clear=True), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(cp.main(), 0)
        self.assertIn("unchanged", summary.read_text())
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
