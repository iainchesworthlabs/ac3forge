"""Unit tests for fuzz_eac3_encoder_space.py, the E-AC-3 / atmos-encode
input-space fuzzer.

ac3cli, ffmpeg and ffprobe are never run: _run() is replaced by a fake that
writes real E-AC-3 syncframe headers (so the independent framing walk has
something true or deliberately false to read) and answers ffprobe in its JSON
shape. What is tested is the harness logic that decides whether a defect is
reported:

- frmsiz's 11-bit ceiling arithmetic at the Annex E half rates;
- draw_case purity, legal tool tokens (never a lone `ecpl`), legal VBR specs;
- the oracle model: which cases lose FFmpeg's decode and why;
- syncframe_walk() catching every framing defect it names, and header_check()
  holding ffprobe's packets to the file and the encoder's own unit count;
- classify(): refusals only on exit 1 or 5 with a recognised message - an
  abort (the first defect this harness found) is never a refusal;
- run_case(), check_envelope(), check_oracles() and main() verdicts.

Run: python3 -m unittest discover -s tools/ci -p 'test_*.py'
"""

import contextlib
import io
import json
import random
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import fuzz_eac3_encoder_space as fe3


def syncframe(strmtyp, substreamid, words=16):
    word = (strmtyp << 14) | (substreamid << 11) | (words - 1)
    return bytes([0x0B, 0x77, word >> 8, word & 0xFF]) + bytes(words * 2 - 4)


def stream(units, dependents=0, words=16):
    out = b""
    for _ in range(units):
        out += syncframe(0, 0, words)
        for d in range(dependents):
            out += syncframe(1, d, words)
    return out


def small_case(**overrides):
    fields = {"seed": 77, "command": "eac3-encode", "layout": "51", "bitrate": 384,
              "sample_rate": 48000, "source_channels": 6, "frames": 1, "pcm16": False,
              "audio_profile": "steady", "correlation": "independent", "tools": "cpl+spx",
              "vbr": "off"}
    fields.update(overrides)
    return fe3.Case(**fields)


class FakeTools:
    def __init__(self, encode=(0, ""), decode=(0, ""), ffmpeg=(0, ""), forced=(0, ""),
                 units=3, claim=None, probe=None, body=None, write_pcm=True):
        self.encode, self.decode, self.ffmpeg, self.forced = encode, decode, ffmpeg, forced
        self.units, self.claim, self.probe, self.body = units, claim, probe, body
        self.write_pcm = write_pcm
        self.calls = []

    def __call__(self, argv):
        self.calls.append(argv)
        if "-xerror" in argv:
            rc, err = self.forced if "-f" in argv[:argv.index("-i")] else self.ffmpeg
            return subprocess.CompletedProcess(argv, rc, "", err)
        if "-show_packets" in argv:
            path = Path(argv[-1])
            if self.probe is not None:
                return self.probe(path)
            size = path.stat().st_size
            packets = [{"size": str(size // self.units)} for _ in range(self.units)]
            return subprocess.CompletedProcess(argv, 0, json.dumps(
                {"packets": packets, "streams": [{"sample_rate": "48000"}]}), "")
        if argv[1] == "decode":
            rc, err = self.decode
            if rc == 0 and self.write_pcm:
                Path(argv[3]).write_bytes(b"RIFF")
            return subprocess.CompletedProcess(argv, rc, "", err)
        rc, err = self.encode
        out = ""
        if rc == 0:
            layout = argv[6] if argv[1] == "eac3-encode" else ""
            body = self.body if self.body is not None else \
                stream(self.units, fe3.DEPENDENTS.get(layout, 0))
            Path(argv[3]).write_bytes(body)
            out = f"encoded {self.claim if self.claim is not None else self.units} " \
                  "E-AC-3 access units\n"
        return subprocess.CompletedProcess(argv, rc, out, err)


class Arithmetic(unittest.TestCase):
    def test_frmsiz_ceiling_per_half_rate(self):
        self.assertEqual(fe3.highest_expressible(16000), 320)
        self.assertEqual(fe3.highest_expressible(22050), 448)
        self.assertEqual(fe3.highest_expressible(24000), 512)
        self.assertEqual(fe3.highest_expressible(48000), 640)
        self.assertTrue(fe3.over_ceiling(16000, 384))
        self.assertFalse(fe3.over_ceiling(48000, 640))
        self.assertEqual(fe3.frame_words(48000, 640), 1280)
        with mock.patch.object(fe3, "LEGAL_RATES", [640]):
            self.assertIsNone(fe3.highest_expressible(16000))

    def test_frames_for_and_case_seed(self):
        self.assertEqual(fe3.frames_for(32, 16000, 6), 43)
        self.assertNotEqual(fe3.case_seed(1, 0), __import__("fuzz_encoder_space").case_seed(1, 0))

    def test_units_written(self):
        self.assertEqual(fe3.units_written("x\nencoded 18 E-AC-3 access units (3 KiB)\n"), 18)
        self.assertIsNone(fe3.units_written("encoded many E-AC-3 access units\n"))
        self.assertIsNone(fe3.units_written("nothing here\n"))


class Drawing(unittest.TestCase):
    def test_draw_case_is_pure_and_legal(self):
        commands = set()
        for seed in range(400):
            case = fe3.draw_case(seed)
            self.assertEqual(case, fe3.draw_case(seed))
            commands.add(case.command)
            self.assertIn(case.sample_rate, fe3.SAMPLE_RATES)
            if case.command == "eac3-encode":
                info = fe3.LAYOUTS[case.layout]
                self.assertGreaterEqual(case.bitrate, info["min"])
                self.assertIn(case.source_channels, info["sources"])
                atoms = [t.split(":")[0] for t in case.tools.split("+")]
                if "ecpl" in atoms:
                    self.assertIn("cpl", atoms)          # never a lone ecpl
                self.assertTrue(case.vbr == "off" or case.vbr.startswith("q:"))
                args = case.cli_args("i.wav", "o.ec3")
                self.assertEqual(args[:7], ["eac3-encode", "i.wav", "o.ec3", str(case.bitrate),
                                            case.tools, case.layout, case.vbr])
            else:
                self.assertEqual(case.layout, "")
                self.assertLessEqual(case.objects, 15)
                self.assertEqual(case.cli_args("i", "o")[:5],
                                 ["atmos-encode", "i", "o", str(case.bitrate), str(case.objects)])
        self.assertEqual(commands, {"eac3-encode", "atmos-encode"})

    def test_vbr_bounds_are_ordered(self):
        rng = random.Random(5)
        for _ in range(500):
            spec = fe3.draw_vbr(rng)
            fields = dict(part.split(":") for part in spec.split(",")) if spec != "off" else {}
            if "min" in fields and "max" in fields:
                self.assertLess(int(fields["min"]), int(fields["max"]))

    def test_describe_and_repro(self):
        self.assertIn("layout=51", fe3.describe(small_case()))
        self.assertIn("tools=cpl+spx vbr=off", fe3.describe(small_case()))
        atmos = small_case(command="atmos-encode", layout="", objects=4)
        self.assertIn("objects=4", fe3.describe(atmos))
        self.assertNotIn("tools=", fe3.describe(atmos))
        self.assertIn("ac3cli atmos-encode in.wav out.ec3 384 4", fe3.repro(atmos))


class OracleModel(unittest.TestCase):
    def test_gaps(self):
        self.assertEqual(fe3.oracle_for(small_case()), ("full", []))
        self.assertEqual(fe3.oracle_for(small_case(layout="714")),
                         ("header", ["two dependent substreams"]))
        klass, gaps = fe3.oracle_for(small_case(tools="cpl+ecpl+tpn", sample_rate=22050))
        self.assertEqual(klass, "header")
        self.assertEqual(gaps, ["enhanced coupling (ecpl)", "transient pre-noise (tpn)",
                                "fscod2 half rate"])


class SyncframeWalk(unittest.TestCase):
    def walk(self, data, expected=None):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "s.ec3"
            path.write_bytes(data)
            return fe3.syncframe_walk(path, expected)

    def test_good_streams(self):
        self.assertIsNone(self.walk(stream(3), 3))
        self.assertIsNone(self.walk(stream(2, dependents=2), None))

    def test_every_defect_is_named(self):
        cases = [
            (stream(1) + b"\x0b\x77", "too short to be a syncframe"),
            (stream(1) + b"\x00" * 32, "no syncword at offset 32"),
            (syncframe(1, 0), "precedes any independent one"),
            (syncframe(0, 0) + syncframe(1, 1), "dependent substreamid 1 at offset 32 does "
                                                "not follow -1"),
            (syncframe(2, 0), "strmtyp 2 at offset 0"),
            (syncframe(0, 0)[:30], "runs 2 bytes past the end"),
            (b"", "no independent substream anywhere"),
        ]
        for data, message in cases:
            self.assertIn(message, self.walk(data) or "", message)
        self.assertIn("make 3 access units; the encoder reported writing 4",
                      self.walk(stream(3), 4))


class HeaderCheck(unittest.TestCase):
    def check(self, stdout, rc=0, case=None, expected=2, size=64):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "s.ec3"
            path.write_bytes(bytes(size))
            def fake(argv):
                return subprocess.CompletedProcess(argv, rc, stdout, "probe error")
            with mock.patch.object(fe3, "_run", fake):
                return fe3.header_check("ffprobe", path, case or small_case(), expected)

    def good(self, **over):
        doc = {"packets": [{"size": 32}, {"size": "32"}], "streams": [{"sample_rate": "48000"}]}
        doc.update(over)
        return json.dumps(doc)

    def test_passes_when_packets_tile_the_file(self):
        self.assertIsNone(self.check(self.good()))
        self.assertIsNone(self.check("not even json", case=small_case(layout="714")))

    def test_failures(self):
        self.assertIn("ffprobe exited 1", self.check("", rc=1))
        self.assertIn("will not parse", self.check("{"))
        self.assertIn("no packets at all", self.check(self.good(packets=[])))
        self.assertIn("no usable size", self.check(self.good(packets=[{"pos": 0}])))
        self.assertIn("do not tile the stream", self.check(self.good(), size=65))
        self.assertIn("found 2 access units; the encoder reported writing 3",
                      self.check(self.good(), expected=3))
        self.assertIn("reports sample_rate None", self.check(self.good(streams=[])))


class Classify(unittest.TestCase):
    def verdict(self, rc, stderr, write=True):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "o.ec3"
            if write:
                out.write_bytes(b"x")
            return fe3.classify(small_case(), subprocess.CompletedProcess([], rc, "", stderr),
                                out)

    def test_refusals_only_on_one_or_five(self):
        for reason, message in fe3.REFUSALS.items():
            self.assertEqual(self.verdict(1, message).reason, reason)
            self.assertEqual(self.verdict(5, message).status, "refused")
        abort = self.verdict(-6, fe3.REFUSALS["frmsiz ceiling"])
        self.assertEqual(abort.status, "fail")
        self.assertIn("crash or abort", abort.detail)
        self.assertIn("unrecognised error", self.verdict(1, "brand new error").detail)
        self.assertIn("wrote no bitstream", self.verdict(0, "", write=False).detail)
        self.assertEqual(self.verdict(0, "").status, "ok")


class RunCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.work = Path(self._tmp.name) / "w"
        self.work.mkdir()
        self.art = Path(self._tmp.name) / "art"

    def tearDown(self):
        self._tmp.cleanup()

    def run_case(self, fake, case=None, ffmpeg="ffmpeg", ffprobe="ffprobe"):
        with mock.patch.object(fe3, "_run", fake):
            return fe3.run_case("ac3cli", ffmpeg, ffprobe, case or small_case(), self.work,
                                self.art)

    def test_full_oracle_clean(self):
        fake = FakeTools()
        result = self.run_case(fake)
        self.assertEqual((result.status, result.oracle), ("ok", "full"))
        self.assertTrue(any("-show_packets" in c for c in fake.calls))
        self.assertTrue(any("-xerror" in c for c in fake.calls))

    def test_header_class_stops_before_ffmpeg(self):
        fake = FakeTools()
        result = self.run_case(fake, small_case(layout="714", tools="none"))
        self.assertEqual(result.status, "no-oracle")
        self.assertEqual(result.detail, "two dependent substreams")
        self.assertFalse(any("-xerror" in c for c in fake.calls))
        self.assertFalse(any("-show_packets" in c for c in fake.calls))

    def test_refusal_and_encode_abort(self):
        result = self.run_case(FakeTools(encode=(1, fe3.REFUSALS["object room"])))
        self.assertEqual((result.status, result.reason, result.oracle),
                         ("refused", "object room", "full"))
        result = self.run_case(FakeTools(encode=(-6, "assert")),
                               small_case(tools="tpn"))
        self.assertEqual(result.status, "fail")
        failure = (self.art / f"{77:016x}" / "failure.txt").read_text()
        self.assertIn("oracle: header (gaps: transient pre-noise (tpn))", failure)

    def test_decode_failures(self):
        self.assertEqual(self.run_case(FakeTools(decode=(3, "x"))).stage, "decode")
        self.assertIn("wrote no PCM", self.run_case(FakeTools(write_pcm=False)).detail)

    def test_framing_failures(self):
        result = self.run_case(FakeTools(claim=5))
        self.assertEqual((result.status, result.stage), ("fail", "framing"))
        self.assertIn("encoder reported writing 5", result.detail)
        def bad_probe(path):
            return subprocess.CompletedProcess([], 0, json.dumps(
                    {"packets": [{"size": 1}], "streams": []}), "")
        result = self.run_case(FakeTools(probe=bad_probe))
        self.assertIn("do not tile", result.detail)
        # Without ffprobe the independent walk alone decides.
        self.assertEqual(self.run_case(FakeTools(probe=bad_probe), ffprobe=None).status, "ok")

    def test_ffmpeg_arbitration(self):
        self.assertEqual(self.run_case(FakeTools(ffmpeg=(1, "probe"), forced=(0, ""))).status,
                         "misprobed")
        result = self.run_case(FakeTools(ffmpeg=(1, ""), forced=(1, "bad")))
        self.assertIn("even with -f eac3 forced", result.detail)
        self.assertEqual(self.run_case(FakeTools(ffmpeg=(1, "")), ffmpeg=None).status, "ok")

    def test_save_artifacts_disabled(self):
        fe3.save_artifacts(None, small_case(), fe3.Result(small_case(), "fail"), self.work)


def light_pcm(rng, channels, blocks, rate, profile, correlation):
    return [[0.0] * 4 for _ in range(channels)]


class Envelope(unittest.TestCase):
    def run_envelope(self, accepts, ceiling_rc=1, ceiling_msg=None):
        msg = ceiling_msg if ceiling_msg is not None else fe3.REFUSALS["frmsiz ceiling"]

        def fake(argv):
            rate, tools, layout = int(argv[4]), argv[5], argv[6]
            wav = Path(argv[2]).name
            if wav.startswith("env_ceiling_"):
                sample_rate = int(wav.removeprefix("env_ceiling_").removesuffix(".wav"))
                if fe3.over_ceiling(sample_rate, rate):
                    return subprocess.CompletedProcess(argv, ceiling_rc, "", msg)
                return subprocess.CompletedProcess(argv, 0, "", "")
            rc = 0 if accepts(layout, rate, tools, wav) else 1
            return subprocess.CompletedProcess(argv, rc, "", "")
        buf = io.StringIO()
        with mock.patch.object(fe3, "_run", fake), \
                mock.patch.object(fe3.ac3space, "generate_pcm", light_pcm), \
                contextlib.redirect_stdout(buf):
            rc = fe3.check_envelope("ac3cli", 4)
        return rc, buf.getvalue()

    def test_matching_envelope_passes(self):
        rc, out = self.run_envelope(
            lambda layout, rate, tools, wav: rate >= fe3.LAYOUTS[layout]["robust"]
            or tools == "none")
        self.assertEqual(rc, 0, out)
        self.assertIn("16000 Hz: 320 kbit/s encodes (exit 0), 384 kbit/s refused with exit 1",
                      out)

    def test_regressions_are_reported(self):
        rc, out = self.run_envelope(
            lambda layout, rate, tools, wav: rate >= fe3.LAYOUTS[layout]["robust"]
            and not (layout == "714" and tools == "tpn" and rate == 640),
            ceiling_rc=-6)
        self.assertEqual(rc, 1)
        self.assertIn("714: refused at or above robust=256 for rates [640]", out)
        self.assertIn("stereo: no probe encodes at all at rates [32, 40]", out)
        self.assertIn("it must be refused with exit 1 and a message naming the limit, "
                      "not exit -6", out)

    def test_ceiling_rate_that_fits_must_encode(self):
        with mock.patch.object(fe3, "highest_expressible", lambda rate: 640):
            rc, out = self.run_envelope(lambda *a: True)
        self.assertEqual(rc, 1)
        self.assertIn("640 kbit/s is inside frmsiz's ceiling", out)

    def test_no_straddling_pair(self):
        with mock.patch.object(fe3, "highest_expressible", lambda rate: None):
            _rc, out = self.run_envelope(lambda *a: True)
        self.assertIn("no rate pair straddles the ceiling", out)


class Oracles(unittest.TestCase):
    def run_oracles(self, decodes, encode_fails=()):
        def fake(argv):
            if "-xerror" in argv:
                return subprocess.CompletedProcess(argv, 0 if decodes(argv) else 1, "", "")
            if "-show_packets" in argv:
                size = Path(argv[-1]).stat().st_size
                return subprocess.CompletedProcess(argv, 0, json.dumps(
                    {"packets": [{"size": size // 2}] * 2,
                     "streams": [{"sample_rate": self.rate}]}), "")
            tools, layout = argv[5], argv[6]
            self.rate = "24000" if "24000" in argv[2] else "48000"
            self.current = (tools, layout, self.rate)
            if tools in encode_fails:
                return subprocess.CompletedProcess(argv, 1, "", "refused\nsecond line")
            Path(argv[3]).write_bytes(stream(2, fe3.DEPENDENTS[layout]))
            return subprocess.CompletedProcess(argv, 0, "encoded 2 E-AC-3 access units\n", "")
        buf = io.StringIO()
        with mock.patch.object(fe3, "_run", fake), \
                mock.patch.object(fe3.ac3space, "generate_pcm", light_pcm), \
                contextlib.redirect_stdout(buf):
            rc = fe3.check_oracles("ac3cli", "ffmpeg", "ffprobe")
        return rc, buf.getvalue()

    def test_model_matches(self):
        rc, out = self.run_oracles(lambda argv: self.current[1] == "51"
                                   and self.current[2] == "48000")
        self.assertEqual(rc, 0, out)
        self.assertIn("two dependent substreams: ffmpeg refused, syncframes walk, ffprobe not "
                      "asked", out)

    def test_ffmpeg_learning_a_gap_says_delete_it(self):
        rc, out = self.run_oracles(lambda argv: True)
        self.assertEqual(rc, 1)
        self.assertIn("fscod2 half rate: FFmpeg now DECODES this - delete the ORACLE_GAPS", out)

    def test_control_must_decode_and_probes_must_encode(self):
        rc, out = self.run_oracles(lambda argv: False, encode_fails=("tpn",))
        self.assertEqual(rc, 1)
        self.assertIn("(control: no gap): this is the control and it must decode", out)
        self.assertIn("transient pre-noise (tpn): the probe stream would not encode - refused",
                      out)


class Main(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.cli = self.tmp / "ac3cli"
        self.cli.write_text("")

    def tearDown(self):
        self._tmp.cleanup()

    def run_main(self, *extra, run_case=None, which=lambda name: f"/usr/bin/{name}"):
        argv = ["x", "--cli", str(self.cli), "--artifacts", str(self.tmp / "a"), *extra]
        buf = io.StringIO()
        code = 0
        patches = [mock.patch.object(sys, "argv", argv),
                   mock.patch.object(fe3.shutil, "which", which),
                   contextlib.redirect_stdout(buf)]
        if run_case is not None:
            patches.append(mock.patch.object(fe3, "run_case", run_case))
        with contextlib.ExitStack() as stack:
            for p in patches:
                stack.enter_context(p)
            try:
                fe3.main()
            except SystemExit as exc:
                code = exc.code
        return code, buf.getvalue()

    @staticmethod
    def verdicts(statuses):
        def run_case(cli, ffmpeg, ffprobe, case, workdir, artifacts):
            status = statuses[case.seed % len(statuses)]
            oracle, gaps = fe3.oracle_for(case)
            return fe3.Result(case, status, "d\ne" if status == "fail" else "", "encode",
                              "frmsiz ceiling" if status == "refused" else "",
                              oracle, gaps)
        return run_case

    def test_preconditions(self):
        code, _ = self.run_main(which=lambda n: None if n == "ffprobe" else n)
        self.assertIn("ffprobe not found", str(code))
        code, _ = self.run_main("--no-ffmpeg", "--check-oracles")
        self.assertIn("cannot run with --no-ffmpeg", str(code))
        self.cli.unlink()
        self.assertIn("ac3cli not found", str(self.run_main()[0]))

    def test_routes(self):
        with mock.patch.object(fe3, "check_envelope", lambda cli, jobs: 0):
            self.assertEqual(self.run_main("--check-envelope")[0], 0)
        with mock.patch.object(fe3, "check_oracles", lambda *a: 1):
            self.assertEqual(self.run_main("--check-oracles")[0], 1)

    def test_bounded_run_summary(self):
        code, out = self.run_main("--cases", "40", "--seed", "3", "--jobs", "2",
                                  run_case=self.verdicts(["ok", "no-oracle", "misprobed",
                                                          "refused"]))
        self.assertIn(code, (0, None), out)
        self.assertIn("40 cases in", out)
        self.assertIn("cells with no FFmpeg decode (framing checked, samples not):", out)

    def test_failure_and_mostly_refused(self):
        code, out = self.run_main("--cases", "3", "--seed", "3", "--no-ffmpeg",
                                  run_case=self.verdicts(["fail"]))
        self.assertEqual(code, 1)
        self.assertIn("FAIL [encode]", out)
        code, out = self.run_main("--cases", "20", "--seed", "3",
                                  run_case=self.verdicts(["refused"]))
        self.assertEqual(code, 1)
        self.assertIn("only 0 of 20 configurations encoded", out)

    def test_seconds_budget_and_replay_and_regressions(self):
        code, out = self.run_main("--seconds", "0", run_case=self.verdicts(["ok"]))
        self.assertIn("0s budget", out)
        code, out = self.run_main("--replay", "25", run_case=self.verdicts(["ok"]))
        self.assertEqual(code, 0)
        self.assertIn("oracle: header (gaps: fscod2 half rate)", out)
        code, out = self.run_main("--regressions", run_case=self.verdicts(["fail"]))
        self.assertEqual(code, 1)
        self.assertEqual(out.count("regression "), len(fe3.REGRESSION_SEEDS))
        with mock.patch.object(fe3, "REGRESSION_SEEDS", {}):
            code, out = self.run_main("--regressions")
        self.assertEqual(code, 0)
        self.assertIn("no recorded regressions yet", out)

    def test_regression_seeds_still_draw_what_they_describe(self):
        """The file's own MAINTENANCE note: a generator change silently redraws
        every pinned seed. Hold the three frmsiz-ceiling seeds to the shape
        their descriptions name."""
        for seed, rate in ((25, 16000), (24, 22050), (412, 24000)):
            case = fe3.draw_case(seed)
            self.assertEqual(case.sample_rate, rate, seed)
            self.assertTrue(fe3.over_ceiling(case.sample_rate, case.bitrate), seed)


if __name__ == "__main__":
    unittest.main()
