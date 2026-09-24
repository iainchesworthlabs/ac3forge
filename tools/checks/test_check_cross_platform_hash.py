"""Unit tests for check_cross_platform_hash.py, the per-leg bitstream pin.

What it must do: a pinned (kernel, mode, stream) whose hash changed is a hard
failure; an unpinned key is reported, not failed; a missing stream (the gold
reference step did not run) is a failure; and the key is built from the
kernel `ac3cli --version` reports plus the transform-mode suffix.

ac3cli is not run: subprocess.run is patched to answer --version.

Run: python3 -m unittest discover -s tools/checks -p 'test_*.py'
"""

import contextlib
import hashlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
import unittest.mock as mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_cross_platform_hash as cph


def version_output(text):
    return lambda cmd, **kw: subprocess.CompletedProcess(cmd, 0, stdout=text, stderr="")


class CrossPlatformHash(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.work = Path(self._tmp.name)
        for i, name in enumerate(cph.STREAMS.values()):
            (self.work / name).write_bytes(bytes([i]) * 64)
        self.pins = self.work / "pins.json"

    def tearDown(self):
        self._tmp.cleanup()

    def digest(self, label):
        return hashlib.sha256((self.work / cph.STREAMS[label]).read_bytes()).hexdigest()

    def run_main(self, *extra, version="ac3cli 1.0\n  kernels: avx2\n"):
        argv = ["x", "--cli", "ac3cli", "--workdir", str(self.work), "--pins", str(self.pins),
                *extra]
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(sys, "argv", argv), \
                mock.patch.object(cph.subprocess, "run", version_output(version)), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = cph.main()
        return rc, out.getvalue(), err.getvalue()

    def test_all_pinned_and_matching_passes(self):
        self.pins.write_text(json.dumps({f"avx2/reference/{k}": self.digest(k)
                                         for k in cph.STREAMS}))
        rc, out, err = self.run_main("--label-suffix", "_reference")
        self.assertEqual(rc, 0, err)
        self.assertEqual(out.count("[ok]"), 3)
        self.assertEqual(err, "")

    def test_mismatch_fails(self):
        pins = {f"avx2/fast/{k}": self.digest(k) for k in cph.STREAMS}
        pins["avx2/fast/eac3"] = "0" * 64
        self.pins.write_text(json.dumps(pins))
        rc, _, err = self.run_main()
        self.assertEqual(rc, 1)
        self.assertIn("[MISMATCH] avx2/fast/eac3: pinned " + "0" * 64, err)

    def test_unpinned_is_reported_not_failed(self):
        rc, out, err = self.run_main()   # no pin file at all
        self.assertEqual(rc, 0)
        self.assertIn(f"[unpinned] avx2/fast/ac3 = {self.digest('ac3')}", out)
        self.assertIn("not a failure by itself", err)

    def test_missing_stream_fails(self):
        (self.work / "gold_cpl.ec3").unlink()
        rc, _, err = self.run_main()
        self.assertEqual(rc, 1)
        self.assertIn("gold_cpl.ec3 missing", err)

    def test_version_without_kernels_line_raises(self):
        with self.assertRaisesRegex(RuntimeError, "no 'kernels:' line"):
            self.run_main(version="ac3cli 1.0\n")


if __name__ == "__main__":
    unittest.main()
