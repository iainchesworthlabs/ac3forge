"""Unit tests for append_quality_history.py, the gold-reference SNR trend
writer.

The regressions it must catch, beyond appending the record:
- a >=10 dB drop of worst_db against the trailing mean is a HARD regression
  (hard_regression=true + ::error::), 0.5 dB a soft warning;
- a collapse of a NON-worst channel (e.g. the centre channel of a 5.1 fixture
  falling 35 dB while the dither-dominated surrounds stay worst) must still
  trip the tier, via the per-channel trailing means;
- series are keyed on (leg, codec, check) so an unrelated check's floor is
  never averaged into another's baseline.

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

import append_quality_history as aqh


def result(worst, channels, **extra):
    return {"codec": "ac3", "bitrate_kbps": 448, "worst_db": worst,
            "channels_db": channels, "threshold_db": 20.0, **extra}


class Trailing(unittest.TestCase):
    def test_trailing_mean_keys_on_check(self):
        with tempfile.TemporaryDirectory() as tmp:
            h = Path(tmp) / "h.jsonl"
            self.assertIsNone(aqh.trailing_mean(h, "l", "ac3", "ac3", 10))
            self.assertIsNone(aqh.trailing_channel_means(h, "l", "ac3", "ac3", 10))
            h.write_text("\n".join(json.dumps(r) for r in [
                {"leg": "l", "codec": "ac3", "check": "ac3", "worst_db": 50.0,
                 "channels_db": [50.0, 60.0]},
                {"leg": "l", "codec": "ac3", "check": "ac3_ext", "worst_db": 15.0,
                 "channels_db": [15.0, 15.0]},
                {"leg": "l", "codec": "ac3", "worst_db": 1.0},   # pre-"check" row
                {"leg": "l", "codec": "ac3", "check": "ac3", "worst_db": 52.0,
                 "channels_db": [52.0, 62.0]},
            ]) + "\n\n")
            self.assertEqual(aqh.trailing_mean(h, "l", "ac3", "ac3", 10), 51.0)
            self.assertEqual(aqh.trailing_channel_means(h, "l", "ac3", "ac3", 10), [51.0, 61.0])
            self.assertIsNone(aqh.trailing_mean(h, "l", "eac3", "eac3", 10))
            self.assertIsNone(aqh.trailing_channel_means(h, "l", "eac3", "eac3", 10))

    def test_channel_means_drop_rows_of_another_layout(self):
        with tempfile.TemporaryDirectory() as tmp:
            h = Path(tmp) / "h.jsonl"
            h.write_text("\n".join(json.dumps(
                {"leg": "l", "codec": "c", "check": "k", "worst_db": 0, "channels_db": ch})
                for ch in ([10.0] * 6, [20.0, 30.0], [40.0, 50.0])) + "\n")
            self.assertEqual(aqh.trailing_channel_means(h, "l", "c", "k", 10), [30.0, 40.0])


class Main(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.results = self.tmp / "results"
        self.history_dir = self.tmp / "hist"
        self.history = self.history_dir / "main.jsonl"
        self.output = self.tmp / "out"

    def tearDown(self):
        self._tmp.cleanup()

    def write_result(self, check, rec, leg="linux"):
        d = self.results / f"gold-reference-{leg}"
        d.mkdir(parents=True, exist_ok=True)
        (d / f"{check}.json").write_text(json.dumps(rec))

    def seed(self, rows):
        self.history_dir.mkdir(parents=True, exist_ok=True)
        self.history.write_text("".join(json.dumps(r) + "\n" for r in rows))

    def history_row(self, worst, channels, check="ac3", commit="old"):
        return {"commit": commit, "leg": "linux", "codec": "ac3", "check": check,
                "worst_db": worst, "channels_db": channels}

    def run_main(self):
        argv = ["x", "--results-dir", str(self.results), "--history-dir", str(self.history_dir),
                "--branch", "main", "--commit", "new", "--commit-date", "2026-01-01"]
        buf = io.StringIO()
        with mock.patch.object(sys, "argv", argv), \
                mock.patch.dict(os.environ, {"GITHUB_OUTPUT": str(self.output)}), \
                contextlib.redirect_stdout(buf):
            rc = aqh.main()
        return rc, buf.getvalue(), (self.output.read_text() if self.output.exists() else "")

    def test_empty_results_append_nothing(self):
        self.results.mkdir()
        (self.results / "loose.json").write_text("{}")
        rc, out, _verdict = self.run_main()
        self.assertEqual(rc, 0)
        self.assertIn("nothing to append", out)
        self.assertFalse(self.history.exists())

    def test_record_carries_check_and_optional_per_channel_fields(self):
        self.write_result("ac3_51", result(23.0, [58.0, 23.0], channel_labels=["C", "Ls"],
                                           thresholds_db=[52.0, 16.0], tightest_channel=0))
        _rc, _out, verdict = self.run_main()
        rec = json.loads(self.history.read_text())
        self.assertEqual((rec["check"], rec["leg"], rec["commit"]), ("ac3_51", "linux", "new"))
        self.assertEqual(rec["channel_labels"], ["C", "Ls"])
        self.assertNotIn("headroom_db", rec)   # absent in, absent out
        self.assertEqual(verdict, "hard_regression=false\n")

    def test_worst_channel_collapse_is_hard(self):
        self.seed([self.history_row(50.0, [50.0, 60.0])] * 3)
        self.write_result("ac3", result(35.0, [35.0, 60.0]))
        _, out, verdict = self.run_main()
        self.assertIn("::error title=Quality trend hard regression::linux/ac3", out)
        self.assertEqual(verdict, "hard_regression=true\n")
        self.assertEqual(len(self.history.read_text().splitlines()), 4)

    def test_non_worst_channel_collapse_is_caught_and_named(self):
        """worst_db (the surround) does not move; the centre falls 35 dB."""
        self.seed([self.history_row(23.0, [58.0, 23.0])] * 3)
        self.write_result("ac3", result(23.0, [23.5, 23.0], channel_labels=["C", "Ls"]))
        _, out, verdict = self.run_main()
        self.assertEqual(verdict, "hard_regression=true\n")
        self.assertIn("channel C fell 34.50 dB", out)

    def test_unlabelled_channels_fall_back_to_indices(self):
        self.seed([self.history_row(23.0, [58.0, 23.0])] * 3)
        self.write_result("ac3", result(23.0, [57.0, 23.0]))
        _, out, verdict = self.run_main()
        self.assertIn("::warning title=Quality trend regression::", out)
        self.assertIn("channel ch0 fell 1.00 dB", out)
        self.assertEqual(verdict, "hard_regression=false\n")

    def test_small_noise_is_quiet_and_other_checks_do_not_pollute(self):
        """A ~15 dB interop check in the same codec must not become the
        baseline for a ~55 dB round-trip check."""
        self.seed([self.history_row(15.0, [15.0], check="ac3_ext")] * 3 +
                  [self.history_row(55.0, [55.0])] * 3)
        self.write_result("ac3", result(54.8, [54.8]))
        _, out, verdict = self.run_main()
        self.assertNotIn("::warning", out)
        self.assertNotIn("::error", out)
        self.assertEqual(verdict, "hard_regression=false\n")

    def test_emit_output_noop_without_env(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            aqh.emit_github_output("a", "b")


if __name__ == "__main__":
    unittest.main()
