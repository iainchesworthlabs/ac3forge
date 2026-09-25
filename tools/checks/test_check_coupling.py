"""Unit tests for the two coupling gates CI runs against FFmpeg:
check_coupling.py (does coupling keep each channel's HF envelope, and leave
the band below the coupling frequency alone?) and check_coupling_level.py
(does coupling preserve absolute LEVEL on anti-correlated content, where a
coordinate clamped at 0.96875 loses 6 dB?).

Both scripts measure with numpy, which the CI step's system Python does not
have; these tests replace the measurement (analyse / tone_amplitude) with
scripted values, fake the encode/decode subprocesses, and - when numpy is
absent - satisfy the import with a stand-in module. What is tested is each
script's verdict: the exit status for a healthy reading, and for the exact
failure each gate exists to catch. The numpy measurement helpers themselves
are not covered.

Run: python3 -m unittest discover -s tools/checks -p 'test_*.py'
"""

import contextlib
import importlib
import io
import math
import struct
import subprocess
import sys
import tempfile
import unittest
import unittest.mock as mock
from pathlib import Path
from typing import ClassVar

sys.path.insert(0, str(Path(__file__).resolve().parent))

try:
    importlib.import_module("numpy")  # only whether it imports matters
    cc = importlib.import_module("check_coupling")
    ccl = importlib.import_module("check_coupling_level")
except ImportError:
    with mock.patch.dict(sys.modules, {"numpy": mock.MagicMock(name="numpy")}):
        cc = importlib.import_module("check_coupling")
        ccl = importlib.import_module("check_coupling_level")


def np_stub():
    stub = mock.MagicMock(name="np")
    stub.log10 = math.log10
    stub.pi = math.pi
    return stub


class Recorder:
    """subprocess.run stand-in: records argv, writes the output file each
    command names, and fails any command containing `fail_on`."""

    def __init__(self, fail_on=None):
        self.commands = []
        self.fail_on = fail_on

    def __call__(self, cmd, **kwargs):
        cmd = [str(c) for c in cmd]
        self.commands.append(cmd)
        if self.fail_on and self.fail_on in cmd:
            return subprocess.CompletedProcess(cmd, 1, "partial", "decoder error")
        Path(cmd[3] if cmd[1] == "encode" else cmd[-1]).write_bytes(b"x" * 100)
        return subprocess.CompletedProcess(cmd, 0, "", "")


class CouplingEnvelope(unittest.TestCase):
    def run_main(self, results, fail_on=None):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        rec = Recorder(fail_on)
        out = io.StringIO()
        with mock.patch.object(cc, "BUILD", Path(tmp.name)), \
                mock.patch.object(cc, "np", np_stub()), \
                mock.patch.object(cc, "write_wav_f32", lambda *a: None), \
                mock.patch.object(cc.subprocess, "run", rec), \
                mock.patch.object(cc, "analyse", lambda tag, path: results[tag]), \
                contextlib.redirect_stdout(out):
            try:
                cc.main()
                code = 0
            except SystemExit as exc:
                code = exc.code
        return code, out.getvalue(), rec

    def test_envelope_kept_and_low_band_untouched_passes(self):
        code, out, rec = self.run_main({"without coupling": (40.0, 1.0),
                                        "with coupling": (18.0, 1.05)})
        self.assertEqual(code, 0, out)
        self.assertIn("COUPLING ENVELOPE: PASS", out)
        encodes = [c for c in rec.commands if c[1] == "encode"]
        self.assertEqual([c[5:] for c in encodes], [[], ["couple"]])
        decodes = [c for c in rec.commands if c[0] == "ffmpeg"]
        self.assertTrue(all("-xerror" in c for c in decodes))

    def test_collapsed_separation_fails(self):
        code, out, _ = self.run_main({"without coupling": (40.0, 1.0),
                                      "with coupling": (3.0, 1.0)})
        self.assertEqual(code, 1)
        self.assertIn("COUPLING ENVELOPE: FAIL", out)

    def test_disturbed_low_band_fails(self):
        code, _, _ = self.run_main({"without coupling": (40.0, 1.0),
                                    "with coupling": (18.0, 1.5)})   # +1.76 dB
        self.assertEqual(code, 1)

    def test_a_failed_strict_decode_aborts_with_its_output(self):
        code, _, _ = self.run_main({}, fail_on="-xerror")
        self.assertRegex(str(code), "failed: ffmpeg .*\npartialdecoder error")

    def test_analyse_reports_hf_separation_in_db(self):
        class Segment:
            def __getitem__(self, key):
                return self if not isinstance(key, tuple) else ("L", "R")[key[1]]
        energies = {("L", cc.HIGH_HZ - 600): 100.0, ("R", cc.HIGH_HZ - 600): 1.0,
                    ("L", cc.LOW_HZ - 200): 10.0}
        with mock.patch.object(cc, "np", np_stub()), \
                mock.patch.object(cc, "read_wav_f32", lambda p: Segment()), \
                mock.patch.object(cc, "band_energy", lambda x, lo, hi: energies[(x, lo)]), \
                contextlib.redirect_stdout(io.StringIO()) as out:
            ratio, low = cc.analyse("with coupling", "x.wav")
        self.assertAlmostEqual(ratio, 20.0)
        self.assertEqual(low, 10.0)
        self.assertIn("HF L/R separation    20.0 dB", out.getvalue())


class ReferenceArray:
    """Indexable stand-in for the decoded (samples, channels) array."""

    def __init__(self, name):
        self.name = name

    def __getitem__(self, key):
        return (self.name, key[1])


class CouplingLevel(unittest.TestCase):
    def run_main(self, amplitudes):
        """`amplitudes` maps (file stem, channel) to the tone amplitude read."""
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        out = io.StringIO()
        with mock.patch.object(ccl, "BUILD", Path(tmp.name)), \
                mock.patch.object(ccl, "np", np_stub()), \
                mock.patch.object(ccl, "write_wav_f32", lambda *a: None), \
                mock.patch.object(ccl, "read_wav_f32", lambda p: ReferenceArray(Path(p).stem)), \
                mock.patch.object(ccl, "tone_amplitude", lambda x: amplitudes[x]), \
                mock.patch.object(ccl.subprocess, "run", Recorder()), \
                contextlib.redirect_stdout(out):
            try:
                ccl.main()
                code = 0
            except SystemExit as exc:
                code = exc.code
        return code, out.getvalue()

    BASE: ClassVar[dict] = {("cpl_level", 0): 0.5, ("cpl_level", 1): 0.25,
                            ("cpl_level_n", 0): 0.5, ("cpl_level_n", 1): 0.25}

    def test_level_preserved_passes(self):
        code, out = self.run_main({**self.BASE, ("cpl_level_c", 0): 0.49,
                                                     ("cpl_level_c", 1): 0.26})
        self.assertEqual(code, 0, out)
        self.assertIn("COUPLING LEVEL: PASS", out)

    def test_clamped_coordinate_loses_six_db_and_fails(self):
        """The defect this gate exists for: a fixed 1/8 coupling-channel scale
        caps the coordinate at 0.96875 and the left channel comes out ~6.3 dB
        quiet."""
        code, out = self.run_main({**self.BASE, ("cpl_level_c", 0): 0.24,
                                                     ("cpl_level_c", 1): 0.25})
        self.assertEqual(code, 1)
        self.assertIn("L level error  -6.38 dB", out)
        self.assertIn("COUPLING LEVEL: FAIL", out)

    def test_silent_output_is_a_failure_not_a_crash(self):
        code, _ = self.run_main({**self.BASE, ("cpl_level_c", 0): 0.0,
                                                   ("cpl_level_c", 1): 0.25})
        self.assertEqual(code, 1)

    def test_run_reports_the_failing_command(self):
        def failing(cmd, **kw):
            return subprocess.CompletedProcess(cmd, 2, "out", "err")
        with mock.patch.object(ccl.subprocess, "run", failing), \
                self.assertRaisesRegex(SystemExit, "failed: ac3cli encode\nouterr"):
            ccl.run(["ac3cli", "encode"])


class WavIo(unittest.TestCase):
    """Both scripts' hand-rolled float32 WAV writer/reader, with numpy's
    buffer calls stubbed: the header they write must be a valid 2-channel
    IEEE-float fmt chunk, and the reader must hand numpy the data chunk's
    exact offset, sample count and channel count."""

    def test_writer_header_and_reader_offsets(self):
        for module in (cc, ccl):
            stub = np_stub()
            stub.empty.return_value.tobytes.return_value = bytes(16)
            with tempfile.TemporaryDirectory() as tmp, \
                    mock.patch.object(module, "np", stub):
                path = Path(tmp) / "x.wav"
                module.write_wav_f32(path, mock.MagicMock(size=2), mock.MagicMock())
                raw = path.read_bytes()
                self.assertEqual(raw[:4] + raw[8:16], b"RIFFWAVEfmt ")
                self.assertEqual(struct.unpack_from("<I", raw, 4)[0], 36 + 16)
                self.assertEqual(struct.unpack_from("<HHIIHH", raw, 20),
                                 (3, 2, 48000, 48000 * 8, 8, 32))
                self.assertEqual(raw[36:40], b"data")
                self.assertEqual(len(raw), 44 + 16)
                module.read_wav_f32(path)
                _, kwargs = stub.frombuffer.call_args
                self.assertEqual((kwargs["count"], kwargs["offset"]), (4, 44))
                stub.frombuffer.return_value.reshape.assert_called_once_with(-1, 2)


if __name__ == "__main__":
    unittest.main()
