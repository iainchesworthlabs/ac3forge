"""Unit tests for verify_fate_interop.py, the FFmpeg FATE third-party decode
interop harness.

The network and the decoders are faked. What is tested is the harness's own
decision logic: a pinned SHA-256 that still mismatches after a fresh download
is a hard error (a corrupt cached file is re-fetched once); truncated samples
are trimmed to complete_bytes; FFmpeg's strict decode failing (non-zero OR any
stderr) aborts; an ac3cli decode failure or a compare_wav.py disagreement is
collected and fails the run; compare=False samples skip the diff; and the
compare_wav.py command carries --max-lag-samples 0 and the right floors.

Run: python3 -m unittest discover -s tools/checks -p 'test_*.py'
"""

import contextlib
import hashlib
import io
import os
import subprocess
import sys
import tempfile
import unittest
import unittest.mock as mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import verify_fate_interop as vfi

DATA = {"a.ac3": b"A" * 100, "b.eac3": b"B" * 50, "c.eac3": b"C" * 10}


def sample(name, **extra):
    return {"path": f"x/{name}", "sha256": hashlib.sha256(DATA[name]).hexdigest(),
            "note": f"note {name}", **extra}


SAMPLES = [sample("a.ac3", complete_bytes=64, min_snr_db=22.0),
           sample("b.eac3", max_diff_dbfs=-90.0),
           sample("c.eac3", compare=False)]


class FakeRun:
    def __init__(self, decode_fail=(), compare_fail=(), ffmpeg_stderr=""):
        self.decode_fail, self.compare_fail = decode_fail, compare_fail
        self.ffmpeg_stderr = ffmpeg_stderr
        self.compares = []
        self.decoded_sizes = {}

    def __call__(self, cmd, **kw):
        if cmd[0] == "ffmpeg":
            return subprocess.CompletedProcess(cmd, 0, "", self.ffmpeg_stderr)
        if cmd[0] == sys.executable:
            self.compares.append(cmd)
            name = Path(cmd[2]).name.removesuffix(".ffmpeg.wav")
            rc = 1 if name in self.compare_fail else 0
            return subprocess.CompletedProcess(cmd, rc, "channel 0 (L): 30 dB\nPASS\n", "")
        source = Path(cmd[2])
        self.decoded_sizes[source.name] = len(source.read_bytes())
        rc = 1 if source.name in self.decode_fail else 0
        return subprocess.CompletedProcess(cmd, rc, "", "bad frame" if rc else "")


class Harness(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.cache = self.tmp / "cache"
        self.cache.mkdir()
        for name, data in DATA.items():
            (self.cache / name).write_bytes(data)
        self.cli = self.tmp / "ac3cli"
        self.cli.write_text("")

    def tearDown(self):
        self._tmp.cleanup()

    def run_main(self, fake, *extra, which="/usr/bin/ffmpeg", urlopen=None, env=None):
        argv = ["x", "--cli", str(self.cli), *extra]
        out = io.StringIO()
        patches = [mock.patch.object(sys, "argv", argv),
                   mock.patch.object(vfi, "SAMPLES", SAMPLES),
                   mock.patch.object(vfi.subprocess, "run", fake),
                   mock.patch.object(vfi.shutil, "which", lambda n: which),
                   mock.patch.dict(os.environ, env or {}),
                   contextlib.redirect_stdout(out)]
        if urlopen is not None:
            patches.append(mock.patch.object(vfi.urllib.request, "urlopen", urlopen))
        with contextlib.ExitStack() as stack:
            for p in patches:
                stack.enter_context(p)
            rc = vfi.main()
        return rc, out.getvalue()

    def test_all_samples_pass(self):
        fake = FakeRun()
        rc, out = self.run_main(fake, "--cache-dir", str(self.cache))
        self.assertEqual(rc, 0, out)
        self.assertIn("FATE interop: 3 pinned third-party samples", out)
        self.assertEqual(fake.decoded_sizes["a.ac3"], 64)       # trimmed
        self.assertIn("trimmed to 64 bytes", out)
        self.assertEqual(len(fake.compares), 2)                  # c.eac3 skipped
        self.assertIn("diff: skipped", out)
        a_cmd, b_cmd = fake.compares
        self.assertEqual(a_cmd[a_cmd.index("--max-lag-samples") + 1], "0")
        self.assertEqual(a_cmd[a_cmd.index("--min-snr-db") + 1], "22.0")
        self.assertNotIn("--max-diff-dbfs", a_cmd)
        self.assertEqual(b_cmd[b_cmd.index("--min-snr-db") + 1], "-200.0")
        self.assertEqual(b_cmd[b_cmd.index("--max-diff-dbfs") + 1], "-90.0")

    def test_decode_and_compare_failures_are_collected(self):
        rc, out = self.run_main(FakeRun(decode_fail={"b.eac3"}, compare_fail={"a.ac3"}),
                                env={"FATE_CACHE_DIR": str(self.cache)})
        self.assertEqual(rc, 1)
        self.assertIn("::error::b.eac3: ac3cli decode failed: bad frame", out)
        self.assertIn("::error::a.ac3: decoded audio disagrees with FFmpeg's", out)

    def test_ffmpeg_stderr_is_a_strict_failure(self):
        with self.assertRaisesRegex(SystemExit, "ffmpeg strict decode of a.ac3 failed"):
            self.run_main(FakeRun(ffmpeg_stderr="incomplete frame"),
                          "--cache-dir", str(self.cache))

    def test_corrupt_cache_is_refetched_once(self):
        (self.cache / "a.ac3").write_bytes(b"partial")
        fetched = []

        def urlopen(url, timeout=None):
            fetched.append(url)
            return io.BytesIO(DATA["a.ac3"])
        rc, out = self.run_main(FakeRun(), "--cache-dir", str(self.cache), urlopen=urlopen)
        self.assertEqual(rc, 0)
        self.assertEqual(fetched, [vfi.BASE_URL + "x/a.ac3"])
        self.assertIn("sha256", out)

    def test_upstream_change_is_a_hard_error(self):
        (self.cache / "a.ac3").unlink()
        def urlopen(url, timeout=None):
            return io.BytesIO(b"tampered")
        with self.assertRaisesRegex(SystemExit, "SHA-256 mismatch after a fresh download"):
            self.run_main(FakeRun(), "--cache-dir", str(self.cache), urlopen=urlopen)

    def test_scratch_cache_is_used_and_removed(self):
        made = []
        real_mkdtemp = vfi.tempfile.mkdtemp

        def mkdtemp(prefix):
            path = real_mkdtemp(prefix=prefix, dir=self.tmp)
            made.append(Path(path))
            return path
        def urlopen(url, timeout=None):
            return io.BytesIO(DATA[url.rsplit("/", 1)[1]])
        with mock.patch.object(vfi.tempfile, "mkdtemp", mkdtemp):
            env = {k: v for k, v in os.environ.items() if k != "FATE_CACHE_DIR"}
            with mock.patch.dict(os.environ, env, clear=True):
                rc, _ = self.run_main(FakeRun(), urlopen=urlopen)
        self.assertEqual(rc, 0)
        self.assertEqual(len(made), 2)
        self.assertFalse(any(p.exists() for p in made))

    def test_list_and_preconditions(self):
        rc, out = self.run_main(FakeRun(), "--list")
        self.assertEqual(rc, 0)
        self.assertIn("x/b.eac3\n    note b.eac3", out)
        with self.assertRaisesRegex(SystemExit, "ffmpeg not on PATH"):
            self.run_main(FakeRun(), which=None)
        self.cli.unlink()
        with self.assertRaisesRegex(SystemExit, "set AC3CLI"):
            self.run_main(FakeRun())

    def test_pinned_corpus_is_well_formed(self):
        """Every real sample is gated on something unless it opts out of the
        diff, and is pinned by a full SHA-256."""
        for s in vfi.SAMPLES:
            self.assertRegex(s["sha256"], r"^[0-9a-f]{64}$")
            self.assertTrue("min_snr_db" in s or "max_diff_dbfs" in s
                            or s.get("compare") is False, s["path"])


if __name__ == "__main__":
    unittest.main()
