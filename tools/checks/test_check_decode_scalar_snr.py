"""Unit tests for check_decode_scalar_snr.py, the float32/fixed-point decode
vs double decode gate.

What it must catch: any stream whose worst-channel SNR between the two
decodes falls below the floor (exit 1, naming stream and channel); a failed
decode or a missing stream must abort rather than pass. It parses
compare_wav.py's "worst channel:" line instead of recomputing SNR, so one test
runs the real compare_wav.py to pin that output contract.

Decodes are faked by patching subprocess.run; no ac3cli is needed.

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
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_decode_scalar_snr as cds


def write_wav_f32(path, samples, rate=48000):
    payload = struct.pack(f"<{len(samples)}f", *samples)
    fmt = struct.pack("<HHIIHH", 3, 1, rate, rate * 4, 4, 32)
    riff = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + \
        b"data" + struct.pack("<I", len(payload)) + payload
    path.write_bytes(b"RIFF" + struct.pack("<I", len(riff)) + riff)


class FakeRun:
    """Decode calls succeed (or fail for `fail_cli`); compare_wav calls print
    a worst-channel line from `snr_by_stream`."""

    def __init__(self, snr_by_stream, fail_cli=None, compare_stdout=None):
        self.snr = snr_by_stream
        self.fail_cli = fail_cli
        self.compare_stdout = compare_stdout
        self.decodes = []

    def __call__(self, cmd, **kwargs):
        if cmd[0] == sys.executable:
            stream = Path(cmd[2]).name.removeprefix("scalar_double_").removesuffix(".wav")
            out = self.compare_stdout if self.compare_stdout is not None else \
                f"lag: 0 samples, worst channel: Ls {self.snr[stream]} dB (floor 120 dB)\n"
            return subprocess.CompletedProcess(cmd, 0, out, "")
        self.decodes.append(cmd)
        rc = 3 if cmd[0] == self.fail_cli else 0
        return subprocess.CompletedProcess(cmd, rc, "decoder said no" if rc else "", "")


class Gate(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.work = Path(self._tmp.name)
        for name in cds.STREAMS:
            (self.work / name).write_bytes(b"x")
        self.double = self.work / "double_cli"
        self.float = self.work / "float_cli"
        self.double.write_text("")
        self.float.write_text("")

    def tearDown(self):
        self._tmp.cleanup()

    def run_main(self, fake, *extra):
        argv = ["x", "--double-cli", str(self.double), "--float-cli", str(self.float),
                "--workdir", str(self.work), *extra]
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(sys, "argv", argv), mock.patch.object(cds.subprocess, "run", fake), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = cds.main()
        return rc, out.getvalue(), err.getvalue()

    def test_all_above_floor_passes(self):
        fake = FakeRun(dict.fromkeys(cds.STREAMS, "138.85") | {"gold.ac3": "inf"})
        rc, out, err = self.run_main(fake)
        self.assertEqual(rc, 0, err)
        self.assertEqual(out.count("[ok]"), 4)
        self.assertIn("worst channel Ls at inf dB", out)
        self.assertEqual(len(fake.decodes), 8)   # both builds, every stream

    def test_one_stream_below_floor_fails(self):
        fake = FakeRun(dict.fromkeys(cds.STREAMS, "138.85") | {"gold_ecpl.ec3": "96.10"})
        rc, _, err = self.run_main(fake)
        self.assertEqual(rc, 1)
        self.assertIn("[FAIL] gold_ecpl.ec3: worst channel Ls at 96.10 dB, floor is 120", err)

    def test_floor_is_configurable(self):
        fake = FakeRun(dict.fromkeys(cds.STREAMS, "80.0"))
        rc, _, _ = self.run_main(fake, "--min-snr-db", "75")
        self.assertEqual(rc, 0)

    def test_decode_failure_aborts(self):
        with self.assertRaisesRegex(SystemExit, "decode failed \\(3\\)"):
            self.run_main(FakeRun({}, fail_cli=str(self.float)))

    def test_unparseable_compare_output_aborts(self):
        with self.assertRaisesRegex(SystemExit, "no 'worst channel' line"):
            self.run_main(FakeRun({}, compare_stdout="PASS\n"))

    def test_missing_stream_and_cli(self):
        (self.work / "gold.ec3").unlink()
        rc, _, err = self.run_main(FakeRun(dict.fromkeys(cds.STREAMS, "130")))
        self.assertEqual(rc, 1)
        self.assertIn("gold.ec3 missing", err)
        self.float.unlink()
        with self.assertRaisesRegex(SystemExit, "no such file"):
            self.run_main(FakeRun({}))


class CompareWavContract(unittest.TestCase):
    def test_parses_the_real_compare_wav_output(self):
        """If compare_wav.py's summary line changes shape, this gate would
        abort on every run - pin the contract against the real script."""
        with tempfile.TemporaryDirectory() as tmp:
            ref, act = Path(tmp) / "r.wav", Path(tmp) / "a.wav"
            sig = [0.5 * math.sin(i * 0.3) for i in range(600)]
            write_wav_f32(ref, sig)
            write_wav_f32(act, [v + (1e-3 if i % 2 else -1e-3) for i, v in enumerate(sig)])
            snr, channel = cds.worst_channel_snr(ref, act, 10.0)
            self.assertEqual(channel, "M")
            self.assertAlmostEqual(snr, 51.0, delta=0.5)   # 0.125 / 1e-6
            snr, _ = cds.worst_channel_snr(ref, ref, 10.0)
            self.assertEqual(snr, float("inf"))


if __name__ == "__main__":
    unittest.main()
