"""Unit tests for append_object_quality_history.py.

Object-reconstruction rows sit at ~10-23 dB, so this series has its own hard
tier of 5 dB (not the codec legs' 10 dB): a 6 dB collapse must publish
hard_regression=true and ::error::, a 1 dB drop only warn. Branch names with
'/' must land in one flat file rather than a subdirectory, and a null
leak_db is carried through as null.

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

import append_object_quality_history as aoq


def obj_row(variant, snr, leg="eac3_joc_768", **extra):
    return {"leg": leg, "variant": variant, "bitrate_kbps": 768, "snr_db": snr,
            "lsd_db": 3.0, **extra}


class ObjectHistory(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.objects = self.tmp / "objects.json"
        self.hist_dir = self.tmp / "hist"
        self.history = self.hist_dir / "object-quality-feat_x.jsonl"
        self.output = self.tmp / "out"

    def tearDown(self):
        self._tmp.cleanup()

    def run_main(self, rows):
        self.objects.write_text(json.dumps({"rows": rows}))
        argv = ["x", "--objects-json", str(self.objects), "--history-dir", str(self.hist_dir),
                "--branch", "feat/x", "--commit", "c", "--commit-date", "d"]
        buf = io.StringIO()
        with mock.patch.object(sys, "argv", argv), \
                mock.patch.dict(os.environ, {"GITHUB_OUTPUT": str(self.output)}), \
                contextlib.redirect_stdout(buf):
            rc = aoq.main()
        return rc, buf.getvalue(), (self.output.read_text() if self.output.exists() else "")

    def seed(self, variant, values):
        self.hist_dir.mkdir(parents=True, exist_ok=True)
        with self.history.open("a") as f:
            for v in values:
                f.write(json.dumps({"commit": "o", "leg": "eac3_joc_768",
                                    "variant": variant, "snr_db": v}) + "\n")
            f.write("\n")

    def test_branch_slug(self):
        self.assertEqual(aoq.branch_slug("feat/a\\b"), "feat_a_b")

    def test_empty_rows(self):
        rc, out, verdict = self.run_main([])
        self.assertEqual(rc, 0)
        self.assertIn("nothing to append", out)
        self.assertEqual(verdict, "")

    def test_flat_file_and_null_leak(self):
        rc, _, verdict = self.run_main([obj_row("scene", 20.0, leak_db=None, mos_lqo=4.1)])
        self.assertEqual(rc, 0)
        self.assertTrue(self.history.exists())
        rec = json.loads(self.history.read_text())
        self.assertIsNone(rec["leak_db"])
        self.assertEqual(rec["mos_lqo"], 4.1)
        self.assertEqual(verdict, "hard_regression=false\n")

    def test_five_db_collapse_is_hard(self):
        self.seed("obj0", [20.0, 20.0])
        self.seed("obj1", [15.0])
        _rc, out, verdict = self.run_main([obj_row("obj0", 14.0), obj_row("obj1", 14.0),
                                          obj_row("obj2", 1.0)])
        self.assertIn("::error title=Object-quality trend hard regression::eac3_joc_768/obj0",
                      out)
        self.assertIn("::warning title=Object-quality trend regression::eac3_joc_768/obj1", out)
        self.assertNotIn("obj2", out)   # no baseline yet: nothing to regress against
        self.assertEqual(verdict, "hard_regression=true\n")

    def test_trailing_mean_missing_series(self):
        self.seed("obj0", [1.0])
        self.assertIsNone(aoq.trailing_mean(self.history, "eac3_joc_768", "nope", 10))

    def test_emit_output_noop(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            aoq.emit_github_output("a", "b")


if __name__ == "__main__":
    unittest.main()
