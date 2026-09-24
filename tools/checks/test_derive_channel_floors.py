"""Unit tests for derive_channel_floors.py, which turns quality history into
the per-channel floors verify_gold_reference.sh gates on.

The policy it must implement exactly: floor = floor(min_observed - 1.0 dB),
min over EVERY leg and commit; `_reference` runs fold into their base check;
rows of a different channel count than the newest are dropped; malformed or
channel-less rows are ignored; an empty history is an error.

Run: python3 -m unittest discover -s tools/checks -p 'test_*.py'
"""

import contextlib
import io
import json
import sys
import tempfile
import unittest
import unittest.mock as mock
from pathlib import Path
from typing import ClassVar

sys.path.insert(0, str(Path(__file__).resolve().parent))

import derive_channel_floors as dcf


class Derive(unittest.TestCase):
    def test_floor_is_min_minus_headroom_rounded_down(self):
        records = [{"channels_db": [58.4, 23.0]}, {"channels_db": [60.0, 22.99]}]
        minima, floors, channels = dcf.derive(records)
        self.assertEqual((minima, floors, channels), ([58.4, 22.99], [57, 21], 2))

    def test_other_layouts_are_dropped(self):
        records = [{"channels_db": [1.0] * 6}, {"channels_db": [30.0, 40.0]}]
        self.assertEqual(dcf.derive(records)[1], [29, 39])

    def test_labels(self):
        self.assertEqual(dcf.labels(6)[3], "LFE")
        self.assertEqual(dcf.labels(3), ["ch0", "ch1", "ch2"])


class Main(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.history = Path(self._tmp.name) / "main.jsonl"

    def tearDown(self):
        self._tmp.cleanup()

    def run_main(self, rows, *extra):
        self.history.write_text("\n".join(
            r if isinstance(r, str) else json.dumps(r) for r in rows) + "\n\n")
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(sys, "argv", ["x", "--history", str(self.history), *extra]), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = dcf.main()
        return rc, out.getvalue(), err.getvalue()

    ROWS: ClassVar[list] = [
        {"check": "ac3", "leg": "linux", "commit": "a", "channels_db": [60.0, 55.0],
         "threshold_db": 50.0},
        {"check": "ac3_reference", "leg": "arm64", "commit": "a", "channels_db": [54.1, 56.0]},
        {"codec": "eac3", "leg": "linux", "commit": "b", "channels_db": [30.0, 31.0]},
        "{not json",
        {"check": "ac3", "leg": "linux", "commit": "c"},     # no channels_db
    ]

    def test_prints_vectors_per_check_with_reference_folded_in(self):
        rc, out, _ = self.run_main(self.ROWS)
        self.assertEqual(rc, 0)
        self.assertIn("ac3\n    evidence     : 2 rows, 1 commit(s), 2 leg(s)", out)
        self.assertIn("    vector       : 53,54\n", out)
        self.assertIn("    vector       : 29,30\n", out)
        self.assertNotIn("ac3_reference", out)

    def test_check_filter_and_no_scalar_line_without_threshold(self):
        rc, out, _ = self.run_main(self.ROWS, "--check", "eac3")
        self.assertEqual(rc, 0)
        self.assertNotIn("\nac3\n", "\n" + out)
        self.assertNotIn("scalar", out)

    def test_scalar_delta_line(self):
        rows = [{"check": "x", "channels_db": [60.0], "threshold_db": 50.0}]
        _, out, _ = self.run_main(rows)
        self.assertIn("vs 50 dB scalar:      +9", out)

    def test_empty_history_is_an_error(self):
        rc, _, err = self.run_main(["{broken"])
        self.assertEqual(rc, 1)
        self.assertIn("no usable records", err)


if __name__ == "__main__":
    unittest.main()
