"""Unit tests for append_ac4_quality_history.py, the AC-4 decode quality trend
writer.

What it must do: append one record per scored stream, judge each against the
stream's own trailing history from before this run, warn on a soft drift, and
on a hard one publish ac4_hard_regression=true while still writing the data.

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

import append_ac4_quality_history as aq


def history_line(leg, snr, lsd, commit="old"):
    return json.dumps({"branch": "main", "commit": commit, "commit_date": "2026-09-01T00:00:00Z",
                       "leg": leg, "min_snr_db": snr, "lsd_db": lsd, "mos_lqo": 4.0})


class Verdict(unittest.TestCase):
    def test_tiers_and_direction(self):
        self.assertIsNone(aq.verdict(20.0, 20.2, 0.5, 3.0, rising_is_worse=False))
        self.assertEqual(aq.verdict(19.4, 20.0, 0.5, 3.0, rising_is_worse=False), "soft")
        self.assertEqual(aq.verdict(17.0, 20.0, 0.5, 3.0, rising_is_worse=False), "hard")
        self.assertEqual(aq.verdict(3.0, 1.0, 0.5, 2.0, rising_is_worse=True), "hard")
        self.assertIsNone(aq.verdict(0.5, 1.0, 0.5, 2.0, rising_is_worse=True))
        self.assertIsNone(aq.verdict(None, 1.0, 0.5, 2.0, rising_is_worse=True))
        self.assertIsNone(aq.verdict(1.0, None, 0.5, 2.0, rising_is_worse=True))


class Main(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)
        self.history = self.dir / "history"
        self.history.mkdir()
        self.output = self.dir / "github_output"
        self.output.write_text("")

    def tearDown(self):
        self._tmp.cleanup()

    def run_main(self, legs):
        json_in = self.dir / "in.json"
        json_in.write_text(json.dumps({"legs": legs}))
        buf = io.StringIO()
        with mock.patch.dict(os.environ, {"GITHUB_OUTPUT": str(self.output)}), \
                contextlib.redirect_stdout(buf):
            rc = aq.main(["--json-in", str(json_in), "--history-dir", str(self.history),
                          "--branch", "main", "--commit", "abc", "--commit-date",
                          "2026-09-27T00:00:00Z"])
        self.assertEqual(rc, 0)
        return buf.getvalue()

    def records(self):
        path = self.history / "ac4-quality-main.jsonl"
        return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]

    def test_first_run_appends_with_no_verdict(self):
        out = self.run_main([{"leg": "dee-2.0", "min_snr_db": 20.0, "lsd_db": 1.0,
                              "mos_lqo": 4.2}])
        self.assertEqual(len(self.records()), 1)
        self.assertNotIn("regression", out)
        self.assertIn("ac4_hard_regression=false", self.output.read_text())
        # Within write_recent_window's window, so the page reads the full file.
        self.assertFalse((self.history / "ac4-quality-main.recent.jsonl").exists())

    def test_hard_drop_is_recorded_and_flagged(self):
        (self.history / "ac4-quality-main.jsonl").write_text(
            "\n".join(history_line("dee-2.0", 20.0, 1.0) for _ in range(3)) + "\n")
        out = self.run_main([{"leg": "dee-2.0", "min_snr_db": 16.0, "lsd_db": 1.0,
                              "mos_lqo": 3.9}])
        self.assertIn("::error title=AC-4 quality hard regression::", out)
        self.assertEqual(self.records()[-1]["min_snr_db"], 16.0)
        self.assertIn("ac4_hard_regression=true", self.output.read_text())

    def test_soft_lsd_rise_only_warns(self):
        (self.history / "ac4-quality-main.jsonl").write_text(history_line("dee-5.1", 25.0, 1.0)
                                                             + "\n")
        out = self.run_main([{"leg": "dee-5.1", "min_snr_db": 25.0, "lsd_db": 1.8,
                              "mos_lqo": 4.0}])
        self.assertIn("::warning title=AC-4 quality soft regression::dee-5.1 LSD", out)
        self.assertIn("ac4_hard_regression=false", self.output.read_text())

    def test_baseline_is_per_stream_and_excludes_this_run(self):
        """Two streams in one run: neither is judged against the other, nor
        against its own new record."""
        (self.history / "ac4-quality-main.jsonl").write_text(
            history_line("dee-2.0", 20.0, 1.0) + "\n" + history_line("dee-5.1", 30.0, 1.0) + "\n")
        out = self.run_main([
            {"leg": "dee-2.0", "min_snr_db": 20.0, "lsd_db": 1.0, "mos_lqo": 4.0},
            {"leg": "dee-5.1", "min_snr_db": 30.0, "lsd_db": 1.0, "mos_lqo": 4.0},
        ])
        self.assertNotIn("regression", out)
        self.assertEqual(len(self.records()), 4)

    def test_no_legs_is_a_warning_not_an_append(self):
        out = self.run_main([])
        self.assertIn("::warning", out)
        self.assertFalse((self.history / "ac4-quality-main.jsonl").exists())


if __name__ == "__main__":
    unittest.main()
