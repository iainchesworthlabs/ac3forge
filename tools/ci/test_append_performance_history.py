"""Unit tests for append_performance_history.py, the post-merge throughput
trend writer.

What it must do: append exactly one record per (leg, config) - the FASTEST
repetition's whole record, not per-field minima - to the published history,
and flag a slowdown against the trailing mean: >=20% warns, >=100% emits an
::error:: and publishes hard_regression=true (while STILL writing the data,
so a big regression is never un-recorded).

Run: python3 -m unittest discover -s tools/ci -p 'test_*.py'
"""

import contextlib
import io
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import append_performance_history as aph


def write_leg(results: Path, leg: str, runs: list[list[dict]], environment=None):
    d = results / f"performance-{leg}"
    d.mkdir(parents=True, exist_ok=True)
    for i, results_list in enumerate(runs):
        (d / f"bench{i}.json").write_text(json.dumps({
            "real_time_budget_ms_per_frame": 32.0, "results": results_list}))
    if environment is not None:
        (d / "environment.json").write_text(json.dumps(environment))


def row(name, ms, **extra):
    return {"name": name, "frames": 100, "total_ms": ms * 100, "ms_per_frame": ms, **extra}


class LoadLegResults(unittest.TestCase):
    def test_minimum_run_record_is_taken_whole(self):
        with tempfile.TemporaryDirectory() as tmp:
            res = Path(tmp)
            (res / "stray.txt").write_text("not a leg")
            write_leg(res, "linux", [
                [row("ac3", 2.0, p95_ms_per_frame=9.0, max_ms_per_frame=10.0)],
                [row("ac3", 1.0, p95_ms_per_frame=1.5, max_ms_per_frame=20.0)],
            ], environment={"cpu_model": "EPYC", "runner_image": "ubuntu-24.04"})
            write_leg(res, "old", [[row("ac3", 0.0)]])
            recs = list(aph.load_leg_results(res))
        self.assertEqual(len(recs), 2)
        linux = recs[0]
        self.assertEqual(linux["leg"], "linux")
        self.assertEqual((linux["ms_per_frame"], linux["p95_ms_per_frame"],
                          linux["max_ms_per_frame"]), (1.0, 1.5, 20.0))
        self.assertEqual(linux["runs"], 2)
        self.assertAlmostEqual(linux["spread"], 1.0)
        self.assertEqual(linux["cpu_model"], "EPYC")
        old = recs[1]
        # Older binaries emit no tail: recorded as None, not zero.
        self.assertIsNone(old["p95_ms_per_frame"])
        self.assertEqual((old["spread"], old["cpu_model"]), (0.0, ""))


class TrailingMean(unittest.TestCase):
    def test_window_and_matching(self):
        with tempfile.TemporaryDirectory() as tmp:
            h = Path(tmp) / "h.jsonl"
            self.assertIsNone(aph.trailing_mean(h, "l", "c", 3))
            lines = [json.dumps({"leg": "l", "config": "c", "ms_per_frame": v})
                     for v in (100.0, 1.0, 2.0, 3.0)]
            lines.append(json.dumps({"leg": "other", "config": "c", "ms_per_frame": 50}))
            h.write_text("\n".join(lines) + "\n\n")
            self.assertEqual(aph.trailing_mean(h, "l", "c", 3), 2.0)
            self.assertIsNone(aph.trailing_mean(h, "l", "nope", 3))


class Main(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.results = self.tmp / "results"
        self.history_dir = self.tmp / "history"
        self.history = self.history_dir / "performance-main.jsonl"
        self.output = self.tmp / "gh_output"

    def tearDown(self):
        self._tmp.cleanup()

    def seed(self, ms_values, leg="linux", config="ac3"):
        self.history_dir.mkdir(parents=True, exist_ok=True)
        self.history.write_text("".join(json.dumps(
            {"commit": f"c{i}", "leg": leg, "config": config, "ms_per_frame": v}) + "\n"
            for i, v in enumerate(ms_values)))

    def run_main(self):
        argv = ["x", "--results-dir", str(self.results), "--history-dir", str(self.history_dir),
                "--branch", "main", "--commit", "abc", "--commit-date", "2026-01-01T00:00:00Z"]
        buf = io.StringIO()
        with mock.patch.object(sys, "argv", argv), \
                mock.patch.dict(os.environ, {"GITHUB_OUTPUT": str(self.output)}), \
                contextlib.redirect_stdout(buf):
            rc = aph.main()
        return rc, buf.getvalue(), (self.output.read_text() if self.output.exists() else "")

    def test_no_results_is_a_warning_not_an_append(self):
        self.results.mkdir()
        rc, out, verdict = self.run_main()
        self.assertEqual(rc, 0)
        self.assertIn("nothing to append", out)
        self.assertFalse(self.history.exists())
        self.assertEqual(verdict, "")

    def test_first_record_is_appended_with_no_verdict_trip(self):
        write_leg(self.results, "linux", [[row("ac3", 1.0)]])
        rc, out, verdict = self.run_main()
        self.assertEqual(rc, 0)
        rec = json.loads(self.history.read_text())
        self.assertEqual((rec["commit"], rec["branch"], rec["leg"], rec["config"]),
                         ("abc", "main", "linux", "ac3"))
        self.assertEqual(verdict, "hard_regression=false\n")
        self.assertNotIn("::warning", out)

    def test_doubling_is_hard_but_still_recorded(self):
        self.seed([1.0] * 5)
        write_leg(self.results, "linux", [[row("ac3", 2.0)]])
        rc, out, verdict = self.run_main()
        self.assertEqual(rc, 0)
        self.assertIn("::error title=Performance trend hard regression::linux/ac3", out)
        self.assertEqual(verdict, "hard_regression=true\n")
        self.assertEqual(len(self.history.read_text().splitlines()), 6)

    def test_soft_tier_only_warns(self):
        self.seed([1.0] * 5)
        write_leg(self.results, "linux", [[row("ac3", 1.25)]])
        _, out, verdict = self.run_main()
        self.assertIn("::warning title=Performance trend regression::", out)
        self.assertEqual(verdict, "hard_regression=false\n")

    def test_below_soft_tier_is_quiet(self):
        self.seed([1.0] * 5)
        write_leg(self.results, "linux", [[row("ac3", 1.19)]])
        _, out, _verdict = self.run_main()
        self.assertNotIn("::warning", out)
        self.assertNotIn("::error", out)

    def test_emit_output_is_a_noop_locally(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            aph.emit_github_output("x", "y")   # must not raise


if __name__ == "__main__":
    unittest.main()
