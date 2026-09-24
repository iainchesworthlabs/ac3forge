"""Unit tests for fuzz_encoder_space.py, the AC-3 encoder input-space fuzzer.

ac3cli and ffmpeg are never run: the module's _run() is replaced by a fake
that plays the encoder, the decoder and FFmpeg. What is tested is the
harness's own logic, because that is what decides whether a defect found by
the search is reported:

- the adversarial PCM generator stays in [-1, 1], is deterministic per seed,
  and honours its correlation modes;
- draw_case is a pure function of the seed and stays inside the acceptance
  envelope it documents (rates >= each layout's min, legal source widths,
  streams long enough to clear MIN_STREAM_BYTES);
- classify() accepts ONLY the two recognised refusal messages - any other
  non-zero exit, or a zero exit that wrote nothing, is a failure;
- run_case() reports decode failures, arbitrates an FFmpeg refusal with a
  forced `-f ac3` rerun (misprobed vs fail) and saves a replayable artifact;
- check_envelope() and main() turn those verdicts into the right exit code.

Run: python3 -m unittest discover -s tools/ci -p 'test_*.py'
"""

import contextlib
import io
import json
import random
import struct
import subprocess
import sys
import tempfile
import unittest
import unittest.mock as mock
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import fuzz_encoder_space as fes


def small_case(**overrides):
    fields = {"seed": 12345, "layout": "stereo", "bitrate": 192, "sample_rate": 48000,
              "source_channels": 2, "frames": 1, "pcm16": True, "audio_profile": "cliff",
              "correlation": "pairs", "options": ["couple"]}
    fields.update(overrides)
    return fes.Case(**fields)


class FakeTools:
    """encode/decode/ffmpeg answered from per-stage (rc, stderr) settings."""

    def __init__(self, encode=(0, ""), decode=(0, ""), ffmpeg=(0, ""), forced=(0, ""),
                 write_stream=True, write_pcm=True):
        self.encode, self.decode, self.ffmpeg, self.forced = encode, decode, ffmpeg, forced
        self.write_stream, self.write_pcm = write_stream, write_pcm
        self.calls = []

    def __call__(self, argv, cwd=None):
        self.calls.append(argv)
        if "-xerror" in argv:
            rc, err = self.forced if "-f" in argv[:argv.index("-i")] else self.ffmpeg
        elif argv[1] == "decode":
            rc, err = self.decode
            if self.write_pcm and rc == 0:
                Path(argv[3]).write_bytes(b"RIFFpcm")
        else:
            rc, err = self.encode
            if self.write_stream and rc == 0:
                Path(argv[3]).write_bytes(b"\x0b\x77" * 64)
        return subprocess.CompletedProcess(argv, rc, "", err)


class Generator(unittest.TestCase):
    def test_every_character_fills_one_block_in_range(self):
        rng = random.Random(1)
        out = [0.0] * fes.BLOCK
        for name, fill in fes.CHARACTERS.items():
            params = {"amp": 1.0, "freq": 997.0, "harmonics": 40}
            phase = fill(out, rng, params, 0.3, 48000)
            self.assertTrue(all(-1.0 <= v <= 1.0 for v in out), name)
            self.assertIsInstance(phase, float)
        fes._fill_silence(out, rng, {}, 0.0, 48000)
        self.assertEqual(set(out), {0.0})

    def test_every_profile_is_deterministic_and_bounded(self):
        for profile in [*fes.AUDIO_PROFILES, "mixed"]:
            a = fes.generate_pcm(random.Random(9), 2, 7, 44100, profile, "independent")
            b = fes.generate_pcm(random.Random(9), 2, 7, 44100, profile, "independent")
            self.assertEqual(a, b, profile)
            self.assertEqual([len(c) for c in a], [7 * fes.BLOCK] * 2)
            self.assertTrue(all(-1.0 <= v <= 1.0 for c in a for v in c), profile)

    def test_cliff_ends_each_frame_in_digital_silence_or_not_at_all(self):
        plan = fes._block_plan(random.Random(4), "cliff", 4 * fes.BLOCKS_PER_FRAME)
        for start in range(0, len(plan), fes.BLOCKS_PER_FRAME):
            names = [c for c, _ in plan[start:start + fes.BLOCKS_PER_FRAME]]
            self.assertIn(names[0], fes.DENSE)
            first_silence = names.index("silence") if "silence" in names else len(names)
            self.assertTrue(all(n == "silence" for n in names[first_silence:]))

    def test_correlation_modes(self):
        ident = fes.generate_pcm(random.Random(2), 3, 2, 48000, "steady", "identical")
        self.assertEqual(ident[0], ident[1])
        self.assertEqual(ident[0], ident[2])
        inv = fes.generate_pcm(random.Random(2), 2, 2, 48000, "chaotic", "inverted")
        self.assertEqual(inv[1], [-s for s in inv[0]])
        pairs = fes.generate_pcm(random.Random(3), 6, 2, 48000, "runs", "pairs")
        for odd in (1, 3, 5):
            self.assertIn(pairs[odd], (pairs[odd - 1], [-s for s in pairs[odd - 1]]))

    def test_write_wav_both_formats(self):
        data = [[1.0, -1.0, 0.5], [0.0, 2.0, -0.25]]
        with tempfile.TemporaryDirectory() as tmp:
            pcm = Path(tmp) / "a.wav"
            fes.write_wav(pcm, data, 32000, True)
            with wave.open(str(pcm)) as w:
                self.assertEqual((w.getnchannels(), w.getframerate(), w.getsampwidth()),
                                 (2, 32000, 2))
                frames = struct.unpack("<6h", w.readframes(3))
            self.assertEqual(frames, (32767, 0, -32767, 32767, 16384, -8192))
            flt = Path(tmp) / "b.wav"
            fes.write_wav(flt, data, 48000, False)
            raw = flt.read_bytes()
            self.assertEqual(struct.unpack_from("<HH", raw, 20), (3, 2))
            self.assertEqual(struct.unpack_from("<6f", raw, 44), (1.0, 0.0, -1.0, 1.0, 0.5, -0.25))


class Drawing(unittest.TestCase):
    def test_draw_case_is_pure_and_inside_the_envelope(self):
        for seed in range(300):
            case = fes.draw_case(seed)
            self.assertEqual(case, fes.draw_case(seed))
            info = fes.LAYOUTS[case.layout]
            self.assertGreaterEqual(case.bitrate, info["min"])
            self.assertIn(case.bitrate, fes.LEGAL_RATES)
            self.assertIn(case.source_channels, info["sources"])
            frame_bytes = case.bitrate * 1000 * fes.FRAME / case.sample_rate / 8
            self.assertGreaterEqual(case.frames * frame_bytes, fes.MIN_STREAM_BYTES)
            self.assertGreaterEqual(case.frames, 6)
            if case.layout != "1+1":
                self.assertFalse([o for o in case.options
                                  if o.startswith(("drc2=", "heavy2", "dialnorm2="))])
            args = case.cli_args("in.wav", "out.ac3")
            self.assertEqual(args[:5], ["encode", "in.wav", "out.ac3", str(case.bitrate),
                                        case.layout])

    def test_frames_for_and_case_seed(self):
        self.assertEqual(fes.frames_for(640, 48000, 8), 8)
        self.assertEqual(fes.frames_for(640, 48000, 6), 7)    # 6 x 2560 B < 16 KiB
        self.assertEqual(fes.frames_for(32, 48000, 6), 128)   # 16 KiB / 128-byte frames
        self.assertEqual(fes.case_seed(1, 2), fes.case_seed(1, 2))
        self.assertNotEqual(fes.case_seed(1, 2), fes.case_seed(1, 3))
        self.assertLess(fes.case_seed(7, 0), 2 ** 64)

    def test_describe_and_repro_carry_the_seed(self):
        case = small_case()
        self.assertIn("seed=12345 layout=stereo", fes.describe(case))
        self.assertIn("--replay 12345", fes.repro(case))
        self.assertIn("fmt=float32", fes.describe(small_case(pcm16=False)))


class Classify(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.out = Path(self._tmp.name) / "o.ac3"

    def tearDown(self):
        self._tmp.cleanup()

    def verdict(self, rc, stderr=""):
        return fes.classify(small_case(), subprocess.CompletedProcess([], rc, "", stderr),
                            self.out)

    def test_only_recognised_refusals_are_tolerated(self):
        for reason, message in fes.REFUSALS.items():
            for rc in (1, 5):
                result = self.verdict(rc, f"error: {message} here")
                self.assertEqual((result.status, result.reason), ("refused", reason))
        self.assertEqual(self.verdict(1, "error: something new").status, "fail")
        self.assertEqual(self.verdict(-6, "Assertion failed").status, "fail")

    def test_zero_exit_needs_a_bitstream(self):
        self.assertEqual(self.verdict(0).status, "fail")
        self.out.write_bytes(b"")
        self.assertIn("wrote no bitstream", self.verdict(0).detail)
        self.out.write_bytes(b"x")
        self.assertEqual(self.verdict(0).status, "ok")


class RunCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.work = Path(self._tmp.name) / "work"
        self.work.mkdir()
        self.artifacts = Path(self._tmp.name) / "artifacts"

    def tearDown(self):
        self._tmp.cleanup()

    def run_case(self, fake, ffmpeg="ffmpeg", case=None):
        with mock.patch.object(fes, "_run", fake):
            return fes.run_case("ac3cli", ffmpeg, case or small_case(), self.work, self.artifacts)

    def test_clean_case(self):
        fake = FakeTools()
        result = self.run_case(fake)
        self.assertEqual(result.status, "ok")
        self.assertEqual([c[1] for c in fake.calls][:2], ["encode", "decode"])
        self.assertEqual(list(self.work.iterdir()), [])   # scratch removed

    def test_no_ffmpeg_skips_the_oracle(self):
        fake = FakeTools(ffmpeg=(1, "boom"))
        self.assertEqual(self.run_case(fake, ffmpeg=None).status, "ok")
        self.assertEqual(len(fake.calls), 2)

    def test_refusal_is_not_saved(self):
        result = self.run_case(FakeTools(encode=(1, fes.REFUSALS["header room"])))
        self.assertEqual((result.status, result.reason), ("refused", "header room"))
        self.assertFalse(self.artifacts.exists())

    def test_encode_crash_saves_a_replayable_artifact(self):
        result = self.run_case(FakeTools(encode=(-11, "Segmentation fault")))
        self.assertEqual((result.status, result.stage), ("fail", "encode"))
        dest = self.artifacts / f"{12345:016x}"
        self.assertTrue((dest / "in.wav").exists())
        self.assertEqual(json.loads((dest / "case.json").read_text())["seed"], 12345)
        self.assertIn("--replay 12345", (dest / "failure.txt").read_text())

    def test_decode_failures(self):
        result = self.run_case(FakeTools(decode=(4, "bad frame")))
        self.assertEqual((result.status, result.stage), ("fail", "decode"))
        self.assertIn("ac3cli decode exited 4", result.detail)
        result = self.run_case(FakeTools(write_pcm=False))
        self.assertIn("wrote no PCM", result.detail)

    def test_ffmpeg_refusal_is_arbitrated(self):
        result = self.run_case(FakeTools(ffmpeg=(1, "Invalid data found\nmore"), forced=(0, "")))
        self.assertEqual((result.status, result.detail), ("misprobed", "Invalid data found"))
        result = self.run_case(FakeTools(ffmpeg=(1, ""), forced=(0, "")))
        self.assertEqual((result.status, result.detail), ("misprobed", ""))
        result = self.run_case(FakeTools(ffmpeg=(1, "x"), forced=(8, "crc mismatch")))
        self.assertEqual((result.status, result.stage), ("fail", "ffmpeg"))
        self.assertIn("even with -f ac3 forced\ncrc mismatch", result.detail)

    def test_ffmpeg_invocation_is_strict(self):
        fake = FakeTools()
        with mock.patch.object(fes, "_run", fake):
            fes.ffmpeg_check("ffmpeg", Path("x.ac3"), forced=True)
        argv = fake.calls[0]
        self.assertEqual(argv[:5], ["ffmpeg", "-v", "error", "-xerror", "-err_detect"])
        self.assertIn("crccheck+bitstream+buffer+explode", argv)
        self.assertEqual(argv[argv.index("-i") - 2:argv.index("-i")], ["-f", "ac3"])

    def test_save_artifacts_disabled(self):
        fes.save_artifacts(None, small_case(), fes.Result(small_case(), "fail"), self.work)


def light_pcm(rng, channels, blocks, rate, profile, correlation):
    return [[0.0] * 4 for _ in range(channels)]


class Envelope(unittest.TestCase):
    def run_envelope(self, accepts):
        def fake(argv, cwd=None):
            rate, layout = int(argv[4]), argv[5]
            rc = 0 if accepts(layout, rate, Path(argv[2]).name) else 1
            return subprocess.CompletedProcess(argv, rc, "", "")
        buf = io.StringIO()
        with mock.patch.object(fes, "_run", fake), \
                mock.patch.object(fes, "generate_pcm", light_pcm), \
                contextlib.redirect_stdout(buf):
            rc = fes.check_envelope("ac3cli")
        return rc, buf.getvalue()

    def test_matching_envelope_passes(self):
        rc, out = self.run_envelope(lambda layout, rate, wav: rate >= fes.LAYOUTS[layout]["robust"]
                                    or "steady" in wav)
        self.assertEqual(rc, 0, out)
        self.assertIn("acceptance envelope matches LAYOUTS", out)

    def test_refusal_above_robust_is_a_regression(self):
        rc, out = self.run_envelope(lambda layout, rate, wav: not (layout == "51" and rate == 448
                                                                   and "cliff" in wav))
        self.assertEqual(rc, 1)
        self.assertIn("51: refused at or above robust=112 for rates [448]", out)

    def test_unreachable_min_is_reported(self):
        rc, out = self.run_envelope(lambda layout, rate, wav: rate >= fes.LAYOUTS[layout]["robust"])
        self.assertEqual(rc, 1)
        self.assertIn("51: no probe encodes at all at rates [96]", out)


class Main(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.cli = self.tmp / "ac3cli"
        self.cli.write_text("")

    def tearDown(self):
        self._tmp.cleanup()

    def run_main(self, *extra, run_case=None, which="/usr/bin/ffmpeg"):
        argv = ["x", "--cli", str(self.cli), "--artifacts", str(self.tmp / "art"), *extra]
        buf = io.StringIO()
        code = 0
        patches = [mock.patch.object(sys, "argv", argv),
                   mock.patch.object(fes.shutil, "which", lambda name: which),
                   contextlib.redirect_stdout(buf)]
        if run_case is not None:
            patches.append(mock.patch.object(fes, "run_case", run_case))
        with contextlib.ExitStack() as stack:
            for p in patches:
                stack.enter_context(p)
            try:
                fes.main()
            except SystemExit as exc:
                code = exc.code
        return code, buf.getvalue()

    @staticmethod
    def verdicts(statuses):
        """A run_case stand-in cycling through `statuses` by case seed."""
        def run_case(cli, ffmpeg, case, workdir, artifacts):
            status = statuses[case.seed % len(statuses)]
            reason = "header room" if status == "refused" else ""
            return fes.Result(case, status, "detail line\nmore" if status == "fail" else "",
                              "encode", reason)
        return run_case

    def test_preconditions(self):
        code, _ = self.run_main(which=None)
        self.assertIn("ffmpeg not found", str(code))
        self.cli.unlink()
        code, _ = self.run_main()
        self.assertIn("ac3cli not found", str(code))

    def test_bounded_run_that_passes(self):
        code, out = self.run_main("--cases", "30", "--seed", "7", "--jobs", "2",
                                  run_case=self.verdicts(["ok", "ok", "misprobed", "refused"]))
        self.assertIn(code, (0, None), out)
        self.assertIn("master seed 7, 30 cases, 2 jobs", out)
        self.assertRegex(out, r"30 cases in [\d.]+s: \d+ encoded and decoded cleanly, "
                              r"\d+ refused \(\d+ header room\), \d+ misprobed")

    def test_a_failure_fails_the_run_and_prints_the_seed(self):
        code, out = self.run_main("--cases", "4", "--seed", "1", "--no-ffmpeg",
                                  run_case=self.verdicts(["fail"]))
        self.assertEqual(code, 1)
        self.assertIn("FAIL [encode] seed=", out)
        self.assertIn("--replay", out)
        self.assertIn("ffmpeg=off", out)

    def test_max_failures_stops_early(self):
        code, out = self.run_main("--cases", "50", "--seed", "1", "--jobs", "1",
                                  "--max-failures", "2", run_case=self.verdicts(["fail"]))
        self.assertEqual(code, 1)
        self.assertLess(out.count("FAIL ["), 50)

    def test_mostly_refused_is_not_a_pass(self):
        code, out = self.run_main("--cases", "20", "--seed", "3",
                                  run_case=self.verdicts(["refused"]))
        self.assertEqual(code, 1)
        self.assertIn("only 0 of 20 configurations encoded at all", out)

    def test_zero_budget_is_not_a_pass(self):
        code, out = self.run_main("--seconds", "0", run_case=self.verdicts(["ok"]))
        self.assertEqual(code, 1)
        self.assertIn("no cases ran at all", out)

    def test_zero_cases_is_not_a_pass(self):
        """Regression: the banner tested `if args.cases`, so --cases 0 fell
        through to formatting the unset --seconds (None) and crashed with a
        TypeError instead of reporting an empty run."""
        code, out = self.run_main("--cases", "0", "--seed", "1", run_case=self.verdicts(["ok"]))
        self.assertEqual(code, 1)
        self.assertIn("master seed 1, 0 cases,", out)
        self.assertIn("no cases ran at all", out)

    def test_replay_and_regressions(self):
        code, out = self.run_main("--replay", "42", run_case=self.verdicts(["ok"]))
        self.assertEqual(code, 0)
        self.assertIn(fes.describe(fes.draw_case(42)), out)
        code, out = self.run_main("--replay", "42", run_case=self.verdicts(["fail"]))
        self.assertEqual(code, 1)
        code, out = self.run_main("--regressions", run_case=self.verdicts(["ok"]))
        self.assertEqual(code, 0)
        self.assertEqual(out.count("regression "), len(fes.REGRESSION_SEEDS))
        code, _ = self.run_main("--regressions", run_case=self.verdicts(["fail"]))
        self.assertEqual(code, 1)

    def test_check_envelope_route(self):
        with mock.patch.object(fes, "check_envelope", lambda cli: 0):
            code, out = self.run_main("--check-envelope")
        self.assertEqual(code, 0)
        self.assertIn("acceptance envelope", out)


if __name__ == "__main__":
    unittest.main()
