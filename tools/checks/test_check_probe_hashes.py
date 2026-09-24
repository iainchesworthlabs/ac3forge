"""Unit tests for check_probe_hashes.py, the fixed-point tier's
cross-leg PCM-hash identity gate.

What it must catch: a fixture whose hash differs between two legs (or from
the pins), a fixture hashed on only one side WITHOUT a declared
`<codec>.skipped=<reason>`, and a run that printed no hashes at all. A
declared skip (a heap-limited target) is reported, not failed.

Run: python3 -m unittest discover -s tools/checks -p 'test_*.py'
"""

import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_probe_hashes as cph

H1, H2, H3 = "0123456789abcdef", "fedcba9876543210", "00000000ffffffff"


class ProbeHashes(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def write(self, name, text):
        path = self.dir / name
        path.write_text(text)
        return str(path)

    def run_main(self, *args):
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(sys, "argv", ["x", *args]), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = cph.main()
        return rc, out.getvalue(), err.getvalue()

    def test_parse_ignores_noise_and_collects_skips(self):
        path = Path(self.write("r.txt", f"boot banner\n  ac3_51.pcm_hash={H1}  \n"
                                        f"eac3.pcm_hash=XYZ\nac4_714.skipped=heap_238094\n"))
        self.assertEqual(cph.hashes_of_run(path), ({"ac3_51": H1}, {"ac4_714": "heap_238094"}))

    def test_identical_runs_pass(self):
        a = self.write("a.txt", f"ac3.pcm_hash={H1}\neac3.pcm_hash={H2}\n")
        b = self.write("b.txt", f"eac3.pcm_hash={H2}\nac3.pcm_hash={H1}\n")
        rc, out, _ = self.run_main(a, b)
        self.assertEqual(rc, 0)
        self.assertEqual(out.count("[ok]"), 2)

    def test_differing_hash_fails(self):
        a = self.write("a.txt", f"ac3.pcm_hash={H1}\n")
        b = self.write("b.txt", f"ac3.pcm_hash={H3}\n")
        rc, _, err = self.run_main(a, b)
        self.assertEqual(rc, 1)
        self.assertIn(f"[FAIL] ac3: {H1} (a.txt) != {H3} (b.txt)", err)

    def test_undeclared_absence_fails(self):
        a = self.write("a.txt", f"ac3.pcm_hash={H1}\neac3.pcm_hash={H2}\n")
        b = self.write("b.txt", f"ac3.pcm_hash={H1}\n")
        rc, _, err = self.run_main(a, b)
        self.assertEqual(rc, 1)
        self.assertIn("eac3: hashed in a.txt only", err)

    def test_declared_skip_on_second_run_is_excused(self):
        a = self.write("a.txt", f"ac3.pcm_hash={H1}\nac4.pcm_hash={H2}\n")
        b = self.write("b.txt", f"ac3.pcm_hash={H1}\nac4.skipped=heap\n")
        rc, out, _ = self.run_main(a, b)
        self.assertEqual(rc, 0)
        self.assertIn("[skipped]  ac4: b.txt declined it (heap)", out)

    def test_declared_skip_on_first_run_is_excused(self):
        """Regression: main() used to pass only the SECOND run's skips to
        compare(), so a skip declared by the FIRST run was reported as
        hashed-in-one-side-only and failed the gate."""
        a = self.write("a.txt", f"ac3.pcm_hash={H1}\nac4.skipped=heap\n")
        b = self.write("b.txt", f"ac3.pcm_hash={H1}\nac4.pcm_hash={H2}\n")
        rc, out, err = self.run_main(a, b)
        self.assertEqual(rc, 0)
        self.assertIn("[skipped]  ac4: a.txt declined it (heap)", out)
        self.assertNotIn("hashed in", err)

    def test_run_without_hashes_fails(self):
        a = self.write("a.txt", f"ac3.pcm_hash={H1}\n")
        b = self.write("b.txt", "crashed\n")
        rc, _, err = self.run_main(a, b)
        self.assertEqual(rc, 1)
        self.assertIn("no pcm_hash lines in", err)

    def test_expected_mode(self):
        pins = self.write("pins.json", json.dumps({"hashes": {"ac3": H1, "ac4": H2}}))
        good = self.write("run.txt", f"ac3.pcm_hash={H1}\nac4.skipped=heap\n")
        rc, out, _ = self.run_main("--expected", pins, good)
        self.assertEqual(rc, 0)
        self.assertIn("[skipped]  ac4", out)
        bad = self.write("bad.txt", f"ac3.pcm_hash={H3}\nac4.pcm_hash={H2}\n")
        rc, _, err = self.run_main("--expected", pins, bad)
        self.assertEqual(rc, 1)
        self.assertIn("[FAIL] ac3", err)
        empty = self.write("empty.txt", "")
        rc, _, err = self.run_main("--expected", pins, empty)
        self.assertEqual(rc, 1)

    def test_bad_usage_prints_doc(self):
        rc, _, err = self.run_main("only-one")
        self.assertEqual(rc, 2)
        self.assertIn("Usage:", err)


if __name__ == "__main__":
    unittest.main()
