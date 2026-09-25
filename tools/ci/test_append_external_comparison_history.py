"""Unit tests for append_external_comparison_history.py.

What it must get right:
- only `landscape` rows get vs_ffmpeg/vs_dee deltas, each only when BOTH
  sides have a real number, and never against an `unverified` DEE entry;
- SNR drops tier at 0.5 dB (warn) / 10 dB (hard: ::error:: + output);
- MOS-LQO drops >= 0.15 warn but never fail, and null MOS history rows are
  skipped rather than averaged in as zero.

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

import append_external_comparison_history as aec

MANIFEST = {"baseline_version": 2, "legs": {
    "eac3_51": {"scores": {"ffmpeg": {"snr_db": 20.0, "lsd_db": 2.0, "mos_lqo": 4.0},
                           "dee": {"status": "unverified"}}},
    "ac3_20": {"scores": {"ffmpeg": {"snr_db": 30.0}, "dee": {"snr_db": 31.0, "lsd_db": None}}},
}}


def trend_row(leg, variant, snr, lsd=1.5, mos=None, codec="eac3"):
    return {"leg": leg, "variant": variant, "codec": codec, "bitrate_kbps": 384,
            "snr_db": snr, "lsd_db": lsd, "hf_db": -3.0, "mos_lqo": mos}


class ExternalHistory(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.trend = self.tmp / "trend.json"
        self.manifest = self.tmp / "manifest.json"
        self.manifest.write_text(json.dumps(MANIFEST))
        self.hist_dir = self.tmp / "h"
        self.history = self.hist_dir / "external-comparison-main.jsonl"
        self.output = self.tmp / "out"

    def tearDown(self):
        self._tmp.cleanup()

    def seed(self, rows):
        self.hist_dir.mkdir(parents=True, exist_ok=True)
        self.history.write_text("".join(json.dumps(r) + "\n" for r in rows) + "\n")

    def run_main(self, rows):
        self.trend.write_text(json.dumps({"rows": rows}))
        argv = ["x", "--trend-json", str(self.trend), "--manifest-json", str(self.manifest),
                "--history-dir", str(self.hist_dir), "--branch", "main",
                "--commit", "c", "--commit-date", "d"]
        buf = io.StringIO()
        with mock.patch.object(sys, "argv", argv), \
                mock.patch.dict(os.environ, {"GITHUB_OUTPUT": str(self.output)}), \
                contextlib.redirect_stdout(buf):
            rc = aec.main()
        recs = [json.loads(line) for line in self.history.read_text().splitlines() if line] \
            if self.history.exists() else []
        return rc, buf.getvalue(), (self.output.read_text() if self.output.exists() else ""), recs

    def test_empty_rows(self):
        rc, out, verdict, recs = self.run_main([])
        self.assertEqual((rc, verdict, recs), (0, "", []))
        self.assertIn("nothing to append", out)

    def test_branch_slug(self):
        self.assertEqual(aec.branch_slug("a/b\\c"), "a_b_c")

    def test_landscape_deltas_only_where_both_sides_are_real(self):
        rc, _, verdict, recs = self.run_main([
            trend_row("eac3_51", "landscape", 22.0, lsd=1.5, mos=4.2),
            trend_row("eac3_51", "spx", 21.0),
            trend_row("ac3_20", "landscape", 33.0, lsd=None, codec="ac3"),
            trend_row("unknown_leg", "landscape", 10.0),
        ])
        self.assertEqual(rc, 0)
        self.assertEqual(verdict, "hard_regression=false\n")
        land, spx, ac3, unknown = recs
        self.assertAlmostEqual(land["vs_ffmpeg_snr_db"], 2.0)
        self.assertAlmostEqual(land["vs_ffmpeg_lsd_db"], -0.5)
        self.assertAlmostEqual(land["vs_ffmpeg_mos_lqo"], 0.2)
        self.assertNotIn("vs_dee_snr_db", land)          # unverified DEE
        self.assertEqual(land["baseline_version"], 2)
        self.assertFalse(any(k.startswith("vs_") for k in spx))
        self.assertAlmostEqual(ac3["vs_dee_snr_db"], 2.0)
        self.assertNotIn("vs_dee_lsd_db", ac3)
        self.assertNotIn("vs_ffmpeg_mos_lqo", ac3)
        self.assertNotIn("baseline_version", unknown)

    def test_tiers(self):
        self.seed([{"commit": "o", "leg": "eac3_51", "variant": v, "snr_db": 30.0,
                    "mos_lqo": m} for v in ("landscape", "spx", "aht") for m in (None, 4.5)])
        _, out, verdict, _ = self.run_main([
            trend_row("eac3_51", "landscape", 19.0, mos=4.5),   # hard SNR
            trend_row("eac3_51", "spx", 29.0, mos=4.2),         # soft SNR + MOS
            trend_row("eac3_51", "aht", 29.8, mos=4.45),        # quiet
        ])
        self.assertEqual(verdict, "hard_regression=true\n")
        self.assertIn("::error title=External-comparison trend hard regression::eac3_51/landscape",
                      out)
        self.assertIn("::warning title=External-comparison trend regression::eac3_51/spx", out)
        self.assertIn("MOS regression::eac3_51/spx: MOS-LQO 4.20 is 0.30 below", out)
        self.assertNotIn("eac3_51/aht", out)

    def test_null_mos_history_is_skipped(self):
        self.seed([{"leg": "l", "variant": "v", "snr_db": 1.0, "mos_lqo": None}])
        history = aec.load_history(self.history)
        self.assertEqual(history[("l", "v")], {"snr_db": [1.0]})
        self.assertIsNone(aec.trailing_mean(history, "l", "v", 10, metric="mos_lqo"))
        self.assertEqual(aec.load_history(self.tmp / "missing.jsonl"), {})

    def test_emit_output_noop(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            aec.emit_github_output("a", "b")


if __name__ == "__main__":
    unittest.main()
