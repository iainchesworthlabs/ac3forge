"""Unit tests for check_drc.py, the DRC / compr / downmix / dialnorm metadata
gate against FFmpeg.

Every check in that script is DISCRIMINATING: it compares a decode that
applies the metadata with one that does not and fails when they agree. These
tests hold down exactly those decisions - each check must FAIL on the dead or
wrong-metadata reading it exists to catch and pass on the healthy one.

Neither ac3cli nor ffmpeg is run, and numpy - which the script uses for its
signal measurements, but which the CI step's system Python does not have - is
not needed: the measurement helpers (read_wav_f32 / rms_db / peak_db /
tone_db / write_programme) are replaced by fakes that return scripted levels
per decoded file, subprocess is faked, and when numpy is absent a stand-in
module satisfies the import. The numpy measurement helpers themselves are
therefore not covered here.

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
    check_drc = importlib.import_module("check_drc")
except ImportError:
    with mock.patch.dict(sys.modules, {"numpy": mock.MagicMock(name="numpy")}):
        check_drc = importlib.import_module("check_drc")

AMPLITUDE_DB = check_drc.db(0.40 / math.sqrt(2.0))


class Decoded:
    """What the fake read_wav_f32 returns: indexing yields (file, key) so the
    fake measurement helpers can look up a scripted level per file."""

    def __init__(self, name):
        self.name = name

    def __getitem__(self, key):
        return (self.name, key)


class Rig:
    """Scripted measurements plus a subprocess fake, installed together."""

    def __init__(self, rms=None, peak=None, tone=None, loudness="-20.02", ebur="-20.05",
                 carried="20", channels=None, ffmpeg_stderr=""):
        self.rms = rms or {}
        self.peak = peak or {}
        self.tone = tone or {}
        self.loudness, self.ebur, self.carried = loudness, ebur, carried
        self.channels = channels or {}
        self.ffmpeg_stderr = ffmpeg_stderr
        self.commands = []

    @staticmethod
    def name_key(x):
        return (x.name, None) if isinstance(x, Decoded) else x

    def rms_db(self, x):
        """A (loud, quiet) tuple answers the loud or quiet window by where the
        slice starts (the programme alternates every 4 s); a scalar answers
        any read of that file."""
        name, key = self.name_key(x)
        level = self.rms[name]
        if isinstance(level, tuple):
            return level[0 if key.start < 4 * 48000 else 1]
        return level

    def peak_db(self, x):
        return self.peak[self.name_key(x)[0]]

    def tone_db(self, x, freq, rate):
        return AMPLITUDE_DB + self.tone[self.name_key(x)[0]]

    def run(self, cmd, **kwargs):
        cmd = [str(c) for c in cmd]
        self.commands.append(cmd)
        if cmd[1:2] == ["loudness"]:
            return subprocess.CompletedProcess(cmd, 0, f"integrated {self.loudness} LKFS\n", "")
        if cmd[0] == "ffmpeg" and "ebur128=framelog=quiet" in cmd:
            err = "" if self.ebur is None else f"  Integrated loudness:\n    I:  {self.ebur} LUFS\n"
            return subprocess.CompletedProcess(cmd, 0, "", err)
        if cmd[1:2] == ["decode"]:
            return subprocess.CompletedProcess(cmd, 0, f"bsi dialnorm {self.carried}\n", "")
        if cmd[0] == "ffprobe":
            name = Path(cmd[-1]).stem.removeprefix("mix_")
            return subprocess.CompletedProcess(cmd, 0, self.channels.get(name, ""), "")
        if cmd[0] == "ffmpeg" and cmd[-2:] == ["null", "-"]:
            return subprocess.CompletedProcess(cmd, 0, "", self.ffmpeg_stderr)
        return subprocess.CompletedProcess(cmd, 0, "", "")

    @contextlib.contextmanager
    def installed(self):
        stub_np = mock.MagicMock(name="np")
        stub_np.stack.return_value.astype.return_value.tobytes.return_value = b""
        out = io.StringIO()
        with mock.patch.object(check_drc, "FAILURES", []) as failures, \
                mock.patch.object(check_drc.subprocess, "run", self.run), \
                mock.patch.object(check_drc, "read_wav_f32",
                                  lambda path: (Decoded(Path(path).stem), 48000)), \
                mock.patch.object(check_drc, "rms_db", self.rms_db), \
                mock.patch.object(check_drc, "peak_db", self.peak_db), \
                mock.patch.object(check_drc, "tone_db", self.tone_db), \
                mock.patch.object(check_drc, "write_programme", lambda *a, **k: None), \
                mock.patch.object(check_drc, "np", stub_np), \
                contextlib.redirect_stdout(out):
            self.out = out
            yield failures


class Dynrng(unittest.TestCase):
    GOOD: ClassVar[dict] = {"prog_s0": (-10.0, -45.0), "prog_s1": (-16.0, -38.0),
                            "plain_s0": -20.0, "plain_s1": -20.0}

    def run_check(self, rms):
        rig = Rig(rms=rms)
        with tempfile.TemporaryDirectory() as tmp, rig.installed() as failures:
            check_drc.check_dynrng("ac3cli", Path(tmp))
        return failures, rig

    def test_working_drc_passes(self):
        failures, rig = self.run_check(self.GOOD)
        self.assertEqual(failures, [])
        encodes = [c for c in rig.commands if c[1:2] == ["encode"]]
        self.assertIn("drc=film-standard", encodes[0])
        self.assertNotIn("drc=film-standard", encodes[1])

    def test_dead_metadata_fails_every_assertion(self):
        dead = dict(self.GOOD, prog_s1=(-10.0, -45.0))
        failures, _ = self.run_check(dead)
        self.assertEqual(failures, ["loud passage is attenuated", "quiet passage is lifted",
                                    "range is reduced"])

    def test_a_stream_without_drc_must_not_respond(self):
        failures, _ = self.run_check(dict(self.GOOD, plain_s1=-19.5))
        self.assertEqual(failures, ["a stream without DRC does not respond"])


class Compr(unittest.TestCase):
    def run_check(self, h0, h1):
        rig = Rig(peak={"hot_h0": h0, "hot_h1": h1})
        with tempfile.TemporaryDirectory() as tmp, rig.installed() as failures:
            check_drc.check_compr("ac3cli", Path(tmp))
        return failures

    def test_ceiling_held_with_little_headroom_passes(self):
        # RF-mode peak = -4.6 + (-7 + 11) = -0.6 dBFS against a -0.5 ceiling.
        self.assertEqual(self.run_check(-0.1, -4.6), [])

    def test_inert_word_fails(self):
        self.assertIn("heavy compression changes the decode", self.run_check(-4.6, -4.6))

    def test_overshoot_fails_both_ceiling_checks(self):
        failures = self.run_check(-0.1, -4.0)     # RF peak 0.0 > -0.5
        self.assertIn("the ceiling holds everywhere, transitions included", failures)
        self.assertIn("the ceiling is not overshot by more than one quantiser step", failures)

    def test_too_much_unused_headroom_fails(self):
        self.assertEqual(self.run_check(-0.1, -6.0),
                         ["the ceiling is not overshot by more than one quantiser step"])


class Downmix(unittest.TestCase):
    EXPECTED: ClassVar[dict] = {
        "dm_cmixlev_-3": -3.01, "dm_cmixlev_-4.5": -4.52, "dm_cmixlev_-6": -6.02,
        "dm_surmixlev_-3": -3.01, "dm_surmixlev_-6": -6.02, "dm_off": -60.0}

    def run_check(self, tone):
        rig = Rig(tone=tone)
        with tempfile.TemporaryDirectory() as tmp, rig.installed() as failures:
            check_drc.check_downmix("ac3cli", Path(tmp))
        return failures, rig

    def test_table_levels_pass(self):
        failures, rig = self.run_check(self.EXPECTED)
        self.assertEqual(failures, [])
        downmixes = [c for c in rig.commands if c[0] == "ffmpeg" and "-ac" in c]
        self.assertEqual(len(downmixes), 6)

    def test_a_level_code_that_does_nothing_fails(self):
        failures, _ = self.run_check(dict(self.EXPECTED, **{"dm_cmixlev_-6": -3.01,
                                                            "dm_off": -3.01}))
        self.assertEqual(failures, ["cmixlev=-6 gives -6.02 dB",
                                    "surmixlev=off drops the surrounds"])


class Dialnorm(unittest.TestCase):
    def run_check(self, **rig_args):
        rig = Rig(**rig_args)
        with tempfile.TemporaryDirectory() as tmp, rig.installed() as failures:
            check_drc.check_dialnorm("ac3cli", Path(tmp))
        return failures, rig

    def test_calibrated_and_carried(self):
        failures, rig = self.run_check()
        self.assertEqual(failures, [])
        self.assertTrue(any("dialnorm=auto" in c for c in rig.commands))

    def test_miscalibration_disagreement_and_lost_value(self):
        failures, _ = self.run_check(loudness="-23.0", ebur=None, carried="31")
        self.assertEqual(failures, ["the -20 dBFS 1 kHz calibration reads -20 LKFS",
                                    "agrees with ffmpeg's ebur128",
                                    "dialnorm=auto reaches bsi"])


class Eac3(unittest.TestCase):
    CHANNELS: ClassVar[dict] = {"stereo": "2\n", "51": "6\n", "71": "8\n", "512": "8\n",
                                "514": "10\n"}

    def run_check(self, **rig_args):
        rig = Rig(**rig_args)
        with tempfile.TemporaryDirectory() as tmp, rig.installed() as failures:
            check_drc.check_eac3("ac3cli", Path(tmp))
        return failures, rig

    def test_applied_dynrng_and_clean_layouts(self):
        failures, rig = self.run_check(rms={"e51_s0": -20.0, "e51_s1": -25.0},
                                       channels=self.CHANNELS)
        self.assertEqual(failures, [])
        self.assertIn("E-AC-3 compr is unverifiable here", rig.out.getvalue())

    def test_inert_dynrng_wrong_channels_and_noisy_decode(self):
        failures, _ = self.run_check(rms={"e51_s0": -20.0, "e51_s1": -20.0},
                                     channels=dict(self.CHANNELS, **{"514": "8\n", "71": ""}),
                                     ffmpeg_stderr="")
        self.assertEqual(failures, ["E-AC-3 dynrng is applied", "71 with mixmdate decodes cleanly",
                                    "514 with mixmdate decodes cleanly"])
        failures, _ = self.run_check(rms={"e51_s0": -20.0, "e51_s1": -25.0},
                                     channels=self.CHANNELS, ffmpeg_stderr="error in frame")
        self.assertEqual(len(failures), 5)


class Plumbing(unittest.TestCase):
    def test_run_raises_with_the_command_and_stderr(self):
        def failing(args, **kw):
            return subprocess.CompletedProcess(args, 1, "", "no such option")
        with mock.patch.object(check_drc.subprocess, "run", failing), \
                self.assertRaisesRegex(SystemExit, "command failed: ffmpeg -y -loglevel error "
                                                   "-i x\nno such option"):
            check_drc.ffmpeg("-i", "x")

    def test_db_floor(self):
        self.assertEqual(check_drc.db(0.0), -240.0)
        self.assertAlmostEqual(check_drc.db(0.5), -6.0206, places=4)

    def test_check_records_failures(self):
        with mock.patch.object(check_drc, "FAILURES", []) as failures, \
                contextlib.redirect_stdout(io.StringIO()) as out:
            check_drc.check("a", True, "fine")
            check_drc.check("b", False, "broken")
        self.assertEqual(failures, ["b"])
        self.assertIn("  FAIL  b: broken", out.getvalue())


class ReadWav(unittest.TestCase):
    """The RIFF walk in read_wav_f32 (numpy's frombuffer stubbed): ffmpeg
    writes WAVE_FORMAT_EXTENSIBLE for anything past stereo, and the real
    format tag then lives in the SubFormat GUID - misreading it would make
    every downmix measurement fail on a correct stream."""

    def wav(self, tag, bits, channels=6, extensible=False, pad_chunk=False):
        fmt = struct.pack("<HHIIHH", 0xFFFE if extensible else tag, channels, 48000,
                          48000 * channels * bits // 8, channels * bits // 8, bits)
        if extensible:
            fmt += struct.pack("<HHI", 22, bits, 0) + struct.pack("<H", tag) + bytes(14)
        body = b"WAVE"
        if pad_chunk:
            body += b"LIST" + struct.pack("<I", 3) + b"abc\0"
        body += b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + \
            struct.pack("<I", 8) + bytes(8)
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        path = Path(tmp.name) / "x.wav"
        path.write_bytes(b"RIFF" + struct.pack("<I", len(body)) + body)
        return path

    def read(self, path):
        stub = mock.MagicMock(name="np")
        with mock.patch.object(check_drc, "np", stub):
            samples, rate = check_drc.read_wav_f32(path)
        return stub, samples, rate

    def test_extensible_float_is_accepted(self):
        stub, samples, rate = self.read(self.wav(3, 32, extensible=True, pad_chunk=True))
        self.assertEqual(rate, 48000)
        stub.frombuffer.return_value.reshape.assert_called_once_with(-1, 6)
        self.assertIs(samples, stub.frombuffer.return_value.reshape.return_value)

    def test_integer_pcm_is_refused(self):
        with self.assertRaisesRegex(AssertionError, "expected float32, got tag 1 bits 16"):
            self.read(self.wav(1, 16, channels=2))
        with self.assertRaises(AssertionError):
            self.read(self.wav(1, 16, extensible=True))


class Main(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.cli = self.tmp / "ac3cli"
        self.cli.write_text("")

    def tearDown(self):
        self._tmp.cleanup()

    def run_main(self, *extra, fail=None, which="/usr/bin/ffmpeg"):
        order = []

        def make(name):
            def fake(cli, tmp):
                order.append(name)
                self.assertEqual(cli, str(self.cli))
                if name == fail:
                    check_drc.FAILURES.append(f"{name} broke")
            return fake
        scratch = self.tmp / "scratch"
        scratch.mkdir(exist_ok=True)
        code = 0
        out = io.StringIO()
        with mock.patch.object(sys, "argv", ["x", "--cli", str(self.cli), *extra]), \
                mock.patch.object(check_drc, "FAILURES", []), \
                mock.patch.object(check_drc.shutil, "which", lambda n: which), \
                mock.patch.object(check_drc.tempfile, "mkdtemp", lambda prefix: str(scratch)), \
                contextlib.ExitStack() as stack:
            for name in ("dialnorm", "dynrng", "compr", "downmix", "eac3"):
                stack.enter_context(mock.patch.object(check_drc, f"check_{name}", make(name)))
            stack.enter_context(contextlib.redirect_stdout(out))
            try:
                check_drc.main()
            except SystemExit as exc:
                code = exc.code
        return code, out.getvalue(), order, scratch

    def test_all_checks_run_and_scratch_is_removed(self):
        code, out, order, scratch = self.run_main()
        self.assertIn(code, (0, None))
        self.assertEqual(order, ["dialnorm", "dynrng", "compr", "downmix", "eac3"])
        self.assertIn("all checks passed", out)
        self.assertFalse(scratch.exists())

    def test_one_failure_fails_the_run_and_keep_keeps_files(self):
        code, out, _, scratch = self.run_main("--keep", fail="compr")
        self.assertEqual(code, 1)
        self.assertIn("1 check(s) failed: compr broke", out)
        self.assertIn("files kept in", out)
        self.assertTrue(scratch.exists())

    def test_preconditions(self):
        code, *_ = self.run_main(which=None)
        self.assertIn("ffmpeg not on PATH", str(code))
        self.cli.unlink()
        code, *_ = self.run_main()
        self.assertIn("ac3cli not found", str(code))


if __name__ == "__main__":
    unittest.main()
