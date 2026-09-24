"""Unit tests for append_kernel_history.py, the per-kernel ns/call trend
writer.

This series is deliberately NON-GATING: even a doubling of one kernel must
only annotate (a ::warning:: tagged non-gating), never ::error:: and never
publish a failing output. What it must still do is append one record per
(leg, kernel), keyed so one kernel's trailing mean never mixes with another's.

Run: python3 -m unittest discover -s tools/ci -p 'test_*.py'
"""

import contextlib
import io
import json
import sys
import tempfile
import unittest
import unittest.mock as mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import append_kernel_history as akh


class KernelHistory(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.results = self.tmp / "results"
        self.hist_dir = self.tmp / "hist"
        self.history = self.hist_dir / "kernels-main.jsonl"

    def tearDown(self):
        self._tmp.cleanup()

    def write(self, kernels, leg="linux"):
        d = self.results / f"kernels-{leg}"
        d.mkdir(parents=True, exist_ok=True)
        (d / "k.json").write_text(json.dumps({"kernels": [
            {"name": n, "iters": 1000, "ns_per_call": v} for n, v in kernels.items()]}))

    def seed(self, rows):
        self.hist_dir.mkdir(parents=True, exist_ok=True)
        self.history.write_text("".join(json.dumps(r) + "\n" for r in rows) + "\n")

    def run_main(self):
        argv = ["x", "--results-dir", str(self.results), "--history-dir", str(self.hist_dir),
                "--branch", "main", "--commit", "c", "--commit-date", "d"]
        buf = io.StringIO()
        with mock.patch.object(sys, "argv", argv), contextlib.redirect_stdout(buf):
            return akh.main(), buf.getvalue()

    def test_empty_results_append_nothing(self):
        self.results.mkdir()
        (self.results / "file.txt").write_text("")
        rc, out = self.run_main()
        self.assertEqual(rc, 0)
        self.assertIn("nothing to append", out)
        self.assertFalse(self.history.exists())

    def test_records_are_appended_per_kernel(self):
        self.write({"mdct": 100.0, "bitalloc": 50.0})
        rc, out = self.run_main()
        self.assertEqual(rc, 0)
        recs = [json.loads(line) for line in self.history.read_text().splitlines()]
        self.assertEqual([(r["leg"], r["kernel"], r["iters"]) for r in recs],
                         [("linux", "mdct", 1000), ("linux", "bitalloc", 1000)])
        self.assertIn("Appended 2 record(s)", out)

    def test_doubling_only_warns_non_gating(self):
        self.seed([{"commit": "o", "leg": "linux", "kernel": "mdct", "ns_per_call": 100.0}] * 3 +
                  [{"commit": "o", "leg": "linux", "kernel": "other", "ns_per_call": 1.0}])
        self.write({"mdct": 250.0})
        rc, out = self.run_main()
        self.assertEqual(rc, 0)
        self.assertIn("Kernel trend hard regression (non-gating)::linux/mdct", out)
        self.assertNotIn("::error", out)

    def test_soft_tier_and_quiet_tier(self):
        self.seed([{"commit": "o", "leg": "linux", "kernel": "a", "ns_per_call": 100.0},
                   {"commit": "o", "leg": "linux", "kernel": "b", "ns_per_call": 100.0}])
        self.write({"a": 125.0, "b": 110.0})
        _, out = self.run_main()
        self.assertIn("::warning title=Kernel trend regression::linux/a", out)
        self.assertNotIn("linux/b", out)

    def test_trailing_mean_edge_cases(self):
        self.assertIsNone(akh.trailing_mean(self.history, "l", "k", 10))
        self.seed([{"leg": "l", "kernel": "k", "ns_per_call": v} for v in (9.0, 1.0, 3.0)])
        self.assertEqual(akh.trailing_mean(self.history, "l", "k", 2), 2.0)
        self.assertIsNone(akh.trailing_mean(self.history, "l", "zz", 2))


if __name__ == "__main__":
    unittest.main()
