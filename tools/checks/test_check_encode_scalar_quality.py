"""Unit tests for check_encode_scalar_quality.py, the float-encoder vs
double-encoder quality gate.

What it must catch: on any stream, the float encoder's worst-channel SNR
(decoded by the double CLI) sitting more than --max-drop-db below the double
encoder's must exit 1; a failed encode/decode, a missing fixture or an
unparseable compare_wav.py output must abort; an optional ffmpeg decode adds a
column and never changes the verdict. A real compare_wav.py run pins the
per-channel line format it parses.

ac3cli/ffmpeg are faked by patching subprocess.run and shutil.which.

Run: python3 -m unittest discover -s tools/checks -p 'test_*.py'
"""

import contextlib
import io
import math
import struct
import subprocess
import sys
import tempfile
import unittest
import unittest.mock as mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_encode_scalar_quality as cesq


def compare_output(channels, worst_name="Ls"):
    lines = [f"channel {i} (c{i}): {v} dB [floor 0 dB, +1.00 dB] [ok]"
             for i, v in enumerate(channels)]
    worst = min(channels, key=float)
    lines.append(f"lag: 0 samples, worst channel: {worst_name} {worst} dB (floor 0 dB)")
    return "\n".join(lines) + "\n"


class FakeRun:
    def __init__(self, float_worst="40.00", fail=None):
        self.float_worst = float_worst
        self.fail = fail
        self.commands = []

    def __call__(self, argv, **kwargs):
        self.commands.append(argv)
        if argv[0] == sys.executable:
            ref, act = Path(argv[2]).name, Path(argv[3]).name
            if act.startswith("float_") and not ref.startswith("double_"):
                return subprocess.CompletedProcess(argv, 0, compare_output(
                    ["50.00", self.float_worst]), "")
            if ref.startswith("double_"):
                return subprocess.CompletedProcess(argv, 0, compare_output(["inf", "90.0"]), "")
            return subprocess.CompletedProcess(argv, 0, compare_output(["50.00", "40.00"]), "")
        rc = 1 if self.fail and self.fail in " ".join(argv) else 0
        return subprocess.CompletedProcess(argv, rc, "", "boom" if rc else "")


class Gate(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.audio = self.tmp / "audio"
        self.audio.mkdir()
        for name in ("reference_51.wav", "reference_stereo.wav"):
            (self.audio / name).write_bytes(b"")
        self.dcli, self.fcli = self.tmp / "double", self.tmp / "float"
        self.dcli.write_text("")
        self.fcli.write_text("")

    def tearDown(self):
        self._tmp.cleanup()

    def run_main(self, fake, *extra, which=None):
        argv = ["x", "--double-cli", str(self.dcli), "--float-cli", str(self.fcli),
                "--workdir", str(self.tmp / "work"), *extra]
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(sys, "argv", argv), \
                mock.patch.object(cesq, "AUDIO", self.audio), \
                mock.patch.object(cesq.subprocess, "run", fake), \
                mock.patch.object(cesq.shutil, "which", lambda name: which), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = cesq.main()
        return rc, out.getvalue(), err.getvalue()

    def test_within_drop_passes_and_decodes_with_double_cli_only(self):
        fake = FakeRun(float_worst="39.60")
        rc, out, err = self.run_main(fake)
        self.assertEqual(rc, 0, err)
        self.assertEqual(out.count("[ok]"), len(cesq.STREAMS))
        self.assertIn("+0.40", out)
        decodes = [c for c in fake.commands if len(c) > 1 and c[1] == "decode"]
        self.assertTrue(decodes)
        self.assertTrue(all(c[0] == str(self.dcli) for c in decodes))
        self.assertIn("within 0.50 dB", out)

    def test_drop_beyond_threshold_fails(self):
        rc, out, err = self.run_main(FakeRun(float_worst="39.00"))
        self.assertEqual(rc, 1)
        self.assertIn("[FAIL]", out)
        self.assertIn("more than 0.50 dB below", err)

    def test_threshold_is_configurable(self):
        rc, _, _ = self.run_main(FakeRun(float_worst="39.00"), "--max-drop-db", "1.5")
        self.assertEqual(rc, 0)

    def test_ffmpeg_column_from_path(self):
        fake = FakeRun()
        rc, out, _ = self.run_main(fake, which="/usr/bin/ffmpeg")
        self.assertEqual(rc, 0)
        self.assertTrue(any(c[0] == "/usr/bin/ffmpeg" for c in fake.commands))
        self.assertIn("40.00 dB", out)

    def test_failures_abort(self):
        with self.assertRaisesRegex(SystemExit, "float encode of ac3 5.1 448 failed"):
            self.run_main(FakeRun(fail=str(self.fcli)))
        (self.audio / "reference_stereo.wav").unlink()
        with self.assertRaisesRegex(SystemExit, "missing fixture source"):
            self.run_main(FakeRun())
        self.fcli.unlink()
        with self.assertRaisesRegex(SystemExit, "no such file"):
            self.run_main(FakeRun())

    def test_unparseable_compare_output_aborts(self):
        def bad(argv, **kw):
            return subprocess.CompletedProcess(argv, 0, "PASS\n", "")
        with self.assertRaisesRegex(SystemExit, "no per-channel lines"):
            self.run_main(bad)


class CompareWavContract(unittest.TestCase):
    def test_real_compare_wav_output_parses(self):
        with tempfile.TemporaryDirectory() as tmp:
            ref, act = Path(tmp) / "r.wav", Path(tmp) / "a.wav"
            sig = [0.5 * math.sin(i * 0.3) for i in range(600)]
            for path, data in ((ref, sig), (act, [v * 1.01 for v in sig])):
                payload = struct.pack(f"<{len(data) * 2}f", *[x for v in data for x in (v, v)])
                fmt = struct.pack("<HHIIHH", 3, 2, 48000, 48000 * 8, 8, 32)
                riff = b"WAVEfmt " + struct.pack("<I", 16) + fmt + b"data" + \
                    struct.pack("<I", len(payload)) + payload
                path.write_bytes(b"RIFF" + struct.pack("<I", len(riff)) + riff)
            per_channel, worst, name = cesq.channel_snrs(ref, act)
            self.assertEqual(len(per_channel), 2)
            self.assertAlmostEqual(worst, 40.0, delta=0.1)
            self.assertIn(name, ("L", "R"))


if __name__ == "__main__":
    unittest.main()
