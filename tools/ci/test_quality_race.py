"""Unit tests for quality_race.py - the `ci` quality gate, the `trend` and
`objects` producers whose JSON feeds the published history, and the report
modes around them.

numpy is the script's measurement engine, and the CI step's system Python
does not have it, so nothing here measures audio: the scoring functions
(decode_scores, decode_scores_ours[_fixed], object_scores, align, ...) are
replaced by scripted values, subprocess/`run` is faked to write the files the
modes stat and read back, and - when numpy is absent - a stand-in module
satisfies the import. What IS tested is every decision and every row the
script assembles from those scores:

- race_ci fails (exit 1, naming the check) on each floor/ceiling breach, on a
  `search=` flag that no longer reaches the encoder (identical bytes), and on
  a search that regresses SNR past CI_SEARCH_MIN_DELTA_DB - and passes when
  all hold;
- race_trend encodes E-AC-3's "auto" landscape once, nulls LSD/HF on AC-3,
  and reports an encoder "header room" refusal as n/a instead of aborting;
- race_objects' `scene` row is the mean of its objects, skips missing leakage
  figures, and refuses stale or missing object exports;
- the helper arithmetic mirrored from C++ (rate_adaptive_fgaincod) and the
  paired-delta / formatting helpers;
- main()'s mode routing and --material validation.

The numpy signal-processing functions themselves (material synthesis,
alignment, spectrograms, band measures) are not covered.

Run: python3 -m unittest discover -s tools/ci -p 'test_*.py'
"""

import contextlib
import importlib
import io
import json
import math
import statistics
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import ClassVar
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

try:
    import numpy  # noqa: F401
    qr = importlib.import_module("quality_race")
except ImportError:
    with mock.patch.dict(sys.modules, {"numpy": mock.MagicMock(name="numpy")}):
        qr = importlib.import_module("quality_race")


class NpShim:
    """The handful of numpy reductions the row-assembly code calls, in
    stdlib terms, so those paths behave the same with or without numpy."""
    mean = staticmethod(statistics.fmean)
    std = staticmethod(lambda values, ddof=0: statistics.stdev(values) if ddof else
                       statistics.pstdev(values))
    ascontiguousarray = staticmethod(lambda x, dtype=None: x)
    float64 = float


class Arr:
    """A shape-only stand-in for an ndarray: len, shape, ndim, slicing."""

    def __init__(self, n, channels=2):
        self.n, self.channels = n, channels
        self.shape = (n, channels) if channels else (n,)
        self.ndim = 2 if channels else 1

    def __len__(self):
        return self.n

    def __getitem__(self, key):
        if isinstance(key, tuple):
            return Arr(len(range(self.n)[key[0]]), 0)
        return Arr(len(range(self.n)[key]), self.channels)

    def mean(self, axis=None):
        return Arr(self.n, 0)


class Num:
    """Absorbs the elementwise arithmetic of an SNR expression."""

    def __pow__(self, other):
        return self

    def __sub__(self, other):
        return self


class FakeRun:
    """Stands in for quality_race.run: writes whatever file each command
    would produce, so the modes can stat and read them back."""

    def __init__(self, objects=5, same_search_bytes=False):
        self.objects = objects
        self.same_search_bytes = same_search_bytes
        self.commands = []

    def __call__(self, cmd):
        cmd = [str(c) for c in cmd]
        self.commands.append(cmd)
        if cmd[0] == "ffmpeg":
            Path(cmd[-1]).write_bytes(b"RIFF")
            return
        verb = cmd[1]
        if verb == "decode":
            Path(cmd[3]).write_bytes(b"RIFF")
            if len(cmd) > 4:
                out = Path(cmd[4])
                out.mkdir(parents=True, exist_ok=True)
                for i in range(self.objects):
                    (out / f"object_{i:02}.wav").write_bytes(b"RIFF")
            return
        body = " ".join(cmd[4:])
        if self.same_search_bytes:
            body = body.replace(" search=distortion", "")
        Path(cmd[3]).write_bytes(body.encode() * 10)


class QrTestCase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.build = Path(self._tmp.name) / "build"
        self.build.mkdir()
        self.out = io.StringIO()
        stack = contextlib.ExitStack()
        self.addCleanup(stack.close)
        self.addCleanup(self._tmp.cleanup)
        stack.enter_context(mock.patch.object(qr, "BUILD", self.build))
        stack.enter_context(mock.patch.object(qr, "CLI", Path("ac3cli")))
        stack.enter_context(mock.patch.object(qr, "np", NpShim))
        stack.enter_context(contextlib.redirect_stdout(self.out))
        self.stack = stack


class Helpers(unittest.TestCase):
    def test_rate_adaptive_fgaincod_matches_the_cpp_line(self):
        # (128 - per_channel) * 7 + 45, truncated toward zero, clamped 0..7.
        self.assertEqual(qr.rate_adaptive_fgaincod(96, 2), 6)     # 605 // 90
        self.assertEqual(qr.rate_adaptive_fgaincod(192, 2), 2)    # 269 // 90
        self.assertEqual(qr.rate_adaptive_fgaincod(640, 2), 0)    # negative, clamped
        self.assertEqual(qr.rate_adaptive_fgaincod(384, 5), 4)    # 409 // 90
        self.assertEqual(qr.rate_adaptive_fgaincod(32, 2), 7)     # 9, clamped
        self.assertEqual(qr.rate_adaptive_fgaincod(64, 0), 5)     # nfchans floor of 1

    def test_fgaincod_legs_drop_pin_n_when_it_equals_the_default(self):
        legs, curve = qr.fgaincod_legs(384, 5)
        self.assertEqual(curve, 4)
        self.assertEqual([label for label, _ in legs],
                         ["auto", "pin-0x4", "search-1ax", "search-2ax"])
        legs, curve = qr.fgaincod_legs(640, 2)
        self.assertIn(("pin-0", "fgaincod=0"), legs)

    def test_paired_delta_and_formatting(self):
        with mock.patch.object(qr, "np", NpShim):
            self.assertEqual(qr.paired_delta([None, None], [1.0, 2.0]), (None, None))
            self.assertEqual(qr.paired_delta([3.0, None], [1.0, 1.0]), (2.0, None))
            mean, sem = qr.paired_delta([2.0, 4.0, 6.0], [1.0, 2.0, 3.0])
            self.assertAlmostEqual(mean, 2.0)
            self.assertAlmostEqual(sem, 1.0 / math.sqrt(3))
            with self.assertRaises(ValueError):           # pairing drift is fatal
                qr.paired_delta([1.0], [1.0, 2.0])
        self.assertEqual(qr._fmt_delta(None, None, width=3), "  -")
        self.assertEqual(qr._fmt_delta(0.5, 0.25, width=0, places=2), "+0.50+-0.25")
        self.assertEqual(qr._fmt_mos(None), "-")
        self.assertEqual(qr._fmt_mos(4.256), "4.26")
        self.assertEqual(qr._fmt_leak(None), "-")
        self.assertEqual(qr._fmt_leak(-12.34), "-12.3")

    def test_gate_and_run(self):
        with contextlib.redirect_stdout(io.StringIO()) as out:
            self.assertTrue(qr.gate("x", True, "fine"))
            self.assertFalse(qr.gate("y", False, "bad"))
        self.assertEqual(out.getvalue(), "  PASS  x: fine\n  FAIL  y: bad\n")
        def failing(cmd, **kw):
            return subprocess.CompletedProcess(cmd, 1, "", "boom")
        with mock.patch.object(qr.subprocess, "run", failing), \
                self.assertRaisesRegex(SystemExit, "command failed: a b\nboom"):
            qr.run(["a", Path("b")])

    def test_read_wav_any_refuses_unknown_formats(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "x.wav"
            fmt = struct.pack("<HHIIHH", 1, 2, 48000, 48000 * 6, 6, 24)
            path.write_bytes(b"RIFF\0\0\0\0WAVEfmt " + struct.pack("<I", 16) + fmt +
                             b"data" + struct.pack("<I", 6) + bytes(6))
            with self.assertRaisesRegex(SystemExit, "unsupported format tag 1/24"):
                qr.read_wav_any(path)

    def test_measured_kbps(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "s"
            path.write_bytes(bytes(24000))
            self.assertAlmostEqual(qr.measured_kbps(path, 2.0), 96.0)


class PerceptualScore(unittest.TestCase):
    def setUp(self):
        stack = contextlib.ExitStack()
        self.addCleanup(stack.close)
        stack.enter_context(mock.patch.object(qr, "_visqol_api", None))
        stack.enter_context(mock.patch.object(qr, "_visqol_warned", False))
        stack.enter_context(mock.patch.object(qr, "np", NpShim))
        self.out = stack.enter_context(contextlib.redirect_stdout(io.StringIO()))

    def fake_api(self, result):
        class Api:
            def create(self, mode):
                assert mode == "audio"

            def measure_from_arrays(self, o, d, sample_rate):
                if isinstance(result, Exception):
                    raise result
                self.seen = (len(o), len(d), sample_rate)
                return type("R", (), {"moslqo": result})()
        return Api

    def test_missing_package_warns_once(self):
        with mock.patch.object(qr, "VisqolApi", None):
            self.assertIsNone(qr.perceptual_score(Arr(10), Arr(10)))
            self.assertIsNone(qr.perceptual_score(Arr(10), Arr(10)))
        self.assertEqual(self.out.getvalue().count("visqol-python not installed"), 1)

    def test_scores_a_centred_window_and_rejects_nan(self):
        with mock.patch.object(qr, "VisqolApi", self.fake_api(4.2)):
            self.assertEqual(qr.perceptual_score(Arr(10 * 48000), Arr(10 * 48000)), 4.2)
            self.assertEqual(qr._visqol_api.seen, (4 * 48000, 4 * 48000, 48000))
        with mock.patch.object(qr, "VisqolApi", self.fake_api(float("nan"))), \
                mock.patch.object(qr, "_visqol_api", None):
            self.assertIsNone(qr.perceptual_score(Arr(100, 0), Arr(100, 0)))

    def test_a_scoring_failure_degrades_to_none(self):
        with mock.patch.object(qr, "VisqolApi", self.fake_api(RuntimeError("native"))):
            self.assertIsNone(qr.perceptual_score(Arr(100), Arr(100)))
        self.assertIn("visqol scoring failed (native)", self.out.getvalue())


class CiGate(QrTestCase):
    def scores(self, snr_override=None, lsd_override=None, search_delta=0.3):
        """decode_scores / decode_scores_ours stand-ins: every row sits
        comfortably inside its bars unless overridden by output-name fragment."""
        snr_override = snr_override or {}
        lsd_override = lsd_override or {}

        def pick(table, wav, default):
            for fragment, value in table.items():
                if fragment in Path(wav).name:
                    return value
            return default

        def decode_scores(original, coded, wav, strict=True, perceptual=False):
            name = Path(wav).name
            if name.startswith("ci_search_"):
                base = 30.0
                return (base + search_delta if "_distortion" in name else base), 5.0, 0.0, None
            return pick(snr_override, wav, 50.0), pick(lsd_override, wav, 1.0), 0.0, None
        return decode_scores

    def run_ci(self, fake_run=None, **score_args):
        scorer = self.scores(**score_args)
        with mock.patch.object(qr, "run", fake_run or FakeRun()), \
                mock.patch.object(qr, "decode_scores", scorer), \
                mock.patch.object(qr, "decode_scores_ours", scorer):
            try:
                qr.race_ci("orig", "src.wav", "orig51", "src51.wav", "origtr", "srctr.wav")
                return 0
            except SystemExit as exc:
                return exc.code

    def test_every_check_inside_its_bars_passes(self):
        fake = FakeRun()
        self.assertEqual(self.run_ci(fake), 0)
        text = self.out.getvalue()
        self.assertIn("all CI gate checks passed", text)
        self.assertNotIn("FAIL", text)
        # 3 materials x 7 FFmpeg-checked variants + 2 x 3 self variants + 2 AC-3 + 8 search.
        encodes = [c for c in fake.commands if c[1] in ("encode", "eac3-encode")]
        self.assertEqual(len(encodes), 21 + 6 + 2 + 8)
        self.assertTrue(any(c[-1] == "search=distortion" for c in encodes))

    def test_floor_and_ceiling_breaches_fail_by_name(self):
        code = self.run_ci(snr_override={"ci_ac3.wav": 29.9, "ci_eac3_51_cpl.wav": 9.0,
                                         "ci_eac3_stereo_ecpl.wav": 20.0},
                           lsd_override={"ci_eac3_transient_spx.wav": 3.2})
        self.assertEqual(code, 1)
        text = self.out.getvalue()
        self.assertIn("4 CI gate check(s) failed: ac3, eac3-stereo-ecpl-self, eac3-51-cpl, "
                      "eac3-transient-spx", text)
        self.assertIn("FAIL  ac3 @ 192kbps: SNR 29.90 dB (floor 30.0)", text)

    def test_ac3_51_floor(self):
        self.assertEqual(self.run_ci(snr_override={"ci_ac3_51.wav": 14.0}), 1)
        self.assertIn("failed: ac3-51", self.out.getvalue())

    def test_search_flag_that_no_longer_reaches_the_encoder_fails(self):
        code = self.run_ci(FakeRun(same_search_bytes=True))
        self.assertEqual(code, 1)
        text = self.out.getvalue()
        for label in ("ac3-stereo", "ac3-51", "eac3-stereo", "eac3-51"):
            self.assertIn(f"search-{label}-inert", text)
        self.assertIn("- identical", text)

    def test_search_that_regresses_fails_but_small_noise_does_not(self):
        self.assertEqual(self.run_ci(search_delta=-0.49), 0)
        self.out.truncate(0)
        self.assertEqual(self.run_ci(search_delta=-0.6), 1)
        self.assertIn("search-eac3-51-regressed", self.out.getvalue())


class Trend(QrTestCase):
    LEGS: ClassVar[list] = [{"name": "ac3-x", "codec": "ac3", "kbps": 448, "wav": Path("a.wav")},
                            {"name": "eac3-x", "codec": "eac3", "kbps": 64, "wav": Path("b.flac")}]

    def test_rows_cache_and_refusal(self):
        encodes, decodes = [], []

        def fake_encode(wav, kbps, codec, tools, out):
            encodes.append((codec, tools))
            if tools is None and codec == "eac3":
                return False                           # "none" cannot fit at 64k
            Path(out).write_bytes(bytes(8000))
            return True

        def fake_scores(original, coded, wav, perceptual=False):
            decodes.append(Path(coded).name)
            return 20.0, 3.0, -1.5, (4.0 if "landscape" in Path(coded).name else None)
        out_json = self.build / "sub" / "trend.json"
        with mock.patch.object(qr, "TREND_LEGS", self.LEGS), \
                mock.patch.object(qr, "materialise_fixture", lambda p: p), \
                mock.patch.object(qr, "read_wav_any", lambda p: Arr(2 * 48000)), \
                mock.patch.object(qr, "_trend_encode", fake_encode), \
                mock.patch.object(qr, "decode_scores_ours_fixed", fake_scores):
            qr.race_trend(json_out=out_json)
        rows = json.loads(out_json.read_text())["rows"]
        ac3 = [r for r in rows if r["leg"] == "ac3-x"]
        self.assertEqual([r["variant"] for r in ac3], ["landscape"])
        self.assertIsNone(ac3[0]["lsd_db"])
        self.assertIsNone(ac3[0]["hf_db"])
        self.assertAlmostEqual(ac3[0]["measured_kbps"], 32.0)
        eac3 = {r["variant"]: r for r in rows if r["leg"] == "eac3-x"}
        self.assertNotIn("none", eac3)                  # refused -> printed n/a, no row
        self.assertIn("eac3-x             | none       |     n/a", self.out.getvalue())
        self.assertEqual(eac3["landscape"]["mos_lqo"], 4.0)
        self.assertEqual(eac3["auto"], dict(eac3["landscape"], variant="auto"))
        self.assertEqual(eac3["tpn"]["lsd_db"], 3.0)
        # landscape and auto share one encode.
        self.assertEqual(encodes.count(("eac3", "auto")), 1)
        self.assertEqual(len(decodes), len(set(decodes)))

    def test_trend_encode(self):
        with mock.patch.object(qr, "run", FakeRun()) as fake:
            self.assertTrue(qr._trend_encode("w", 192, "ac3", None, self.build / "o.ac3"))
        results = iter([
            subprocess.CompletedProcess([], 0, "", ""),
            subprocess.CompletedProcess([], 1, "", f"x: {qr._HEADER_ROOM_REFUSAL}"),
            subprocess.CompletedProcess([], 1, "", "segfault"),
        ])
        seen = []

        def fake(cmd, **kw):
            seen.append([str(c) for c in cmd])
            return next(results)
        with mock.patch.object(qr.subprocess, "run", fake):
            self.assertTrue(qr._trend_encode("w", 96, "eac3", "spx", "o.ec3"))
            self.assertFalse(qr._trend_encode("w", 64, "eac3", None, "o.ec3"))
            with self.assertRaisesRegex(SystemExit, "segfault"):
                qr._trend_encode("w", 64, "eac3", "all", "o.ec3")
        self.assertEqual(seen[0][-1], "spx")
        self.assertEqual(seen[1][-1], "64")

    def test_materialise_fixture(self):
        self.assertEqual(qr.materialise_fixture("x.wav"), Path("x.wav"))
        flac = self.build / "p.flac"
        flac.write_bytes(b"fLaC")
        fake = FakeRun()
        with mock.patch.object(qr, "run", fake):
            out = qr.materialise_fixture(flac)
            self.assertEqual(out, self.build / "fixture_p.wav")
            qr.materialise_fixture(flac)                     # cached: no second decode
        self.assertEqual(len(fake.commands), 1)


class Objects(QrTestCase):
    def setUp(self):
        super().setUp()
        self.wav = self.build / "scene.wav"
        self.paths = self.build / "scene.paths"
        self.wav.write_bytes(b"RIFF")
        self.paths.write_text("0 0 0\n")
        self.stack.enter_context(mock.patch.object(qr, "OBJECTS_WAV", self.wav))
        self.stack.enter_context(mock.patch.object(qr, "OBJECTS_PATHS", self.paths))
        self.stack.enter_context(mock.patch.object(qr, "read_wav_f32", lambda p: Arr(10, 1)))

    def run_objects(self, fake_run=None, channels=5, leaks=None, moses=None):
        leaks = leaks or [-30.0, None, -20.0, -40.0, -10.0]
        moses = moses or [4.0, 3.0, 4.5, 3.5, 4.0]
        calls = iter(range(100))

        def scores(source, recovered, perceptual=False):
            i = next(calls) % 5
            return 10.0 + i, 2.0 * i, leaks[i], moses[i]
        out_json = self.build / "objects.json"
        with mock.patch.object(qr, "read_wav_any", lambda p: Arr(48000, channels)), \
                mock.patch.object(qr, "run", fake_run or FakeRun()), \
                mock.patch.object(qr, "object_scores", scores):
            qr.race_objects(json_out=out_json)
        return json.loads(out_json.read_text())["rows"]

    def test_scene_row_is_the_mean_of_its_objects(self):
        stale_dir = self.build / "objects_atmos-objects-256"
        stale_dir.mkdir()
        (stale_dir / "object_07.wav").write_bytes(b"stale")
        rows = self.run_objects()
        self.assertFalse((stale_dir / "object_07.wav").exists())
        self.assertEqual(len(rows), 2 * 6)
        scene = rows[5]
        self.assertEqual((scene["leg"], scene["variant"]), ("atmos-objects-256", "scene"))
        self.assertAlmostEqual(scene["snr_db"], 12.0)
        self.assertAlmostEqual(scene["lsd_db"], 4.0)
        self.assertAlmostEqual(scene["leak_db"], -25.0)     # the None is skipped, not zeroed
        self.assertAlmostEqual(scene["mos_lqo"], 3.8)
        self.assertEqual([r["variant"] for r in rows[:5]], qr.OBJECT_NAMES)

    def test_scene_mos_is_none_if_any_object_lacks_one_and_leak_none_if_all_do(self):
        rows = self.run_objects(leaks=[None] * 5, moses=[4.0, None, 4.0, 4.0, 4.0])
        self.assertIsNone(rows[5]["mos_lqo"])
        self.assertIsNone(rows[5]["leak_db"])
        self.assertIn("|       - |", self.out.getvalue())

    def test_refusals(self):
        with self.assertRaisesRegex(SystemExit, "object_04.wav was not written"):
            self.run_objects(fake_run=FakeRun(objects=4))
        with self.assertRaisesRegex(SystemExit, "has 6 channels, but OBJECT_NAMES lists 5"):
            self.run_objects(channels=6)
        self.paths.unlink()
        with self.assertRaisesRegex(SystemExit, "regenerate them"):
            self.run_objects()


class ReportModes(QrTestCase):
    """The table modes: scripted scores in, rows and JSON out."""

    @staticmethod
    def scorer(snr=30.0):
        return lambda original, coded, wav, strict=True, perceptual=False: (snr, 4.0, -2.0, None)

    def test_race_ac3_and_eac3_tables(self):
        fake = FakeRun()
        with mock.patch.object(qr, "run", fake), \
                mock.patch.object(qr, "decode_scores", self.scorer()):
            qr.race_ac3("o", "src.wav", 1.0)
            qr.race_eac3("o", "src.wav", 1.0, rates=(96,))
        text = self.out.getvalue()
        self.assertIn("worst gap vs ffmpeg: +0.00 dB", text)
        self.assertEqual(sum(1 for c in fake.commands if c[1:2] == ["eac3-encode"]), 7)
        self.assertTrue(any("eac3" in c and c[0] == "ffmpeg" for c in fake.commands))

    def test_race_vbr_rows(self):
        out_json = self.build / "v" / "vbr.json"
        with mock.patch.object(qr, "run", FakeRun()), \
                mock.patch.object(qr, "decode_scores", self.scorer()), \
                mock.patch.object(qr, "make_material", lambda: ("l", "r")), \
                mock.patch.object(qr, "make_material_dynamic", lambda left, right: (left, right)), \
                mock.patch.object(qr, "write_wav_f32", lambda *a: None), \
                mock.patch.object(qr, "read_wav_f32", lambda p: "dyn"):
            rows = qr.race_vbr("o", "src.wav", 1.0, json_out=out_json)
        modes = [r["mode"] for r in rows]
        self.assertEqual(modes.count("vbr"), len(qr.VBR_QUALITIES))
        self.assertEqual(modes.count("abr"), len(qr.ABR_TARGETS))
        self.assertEqual(modes.count("dynamic-abr"), 4)
        self.assertEqual(json.loads(out_json.read_text())["rows"], rows)
        self.assertTrue(all(r["cbr_kbps"] <= qr.VBR_FORMAT_MAX_KBPS
                            for r in rows if r["mode"] == "vbr"))

    def test_race_fgaincod_rows(self):
        profiles = iter([([30.0, 31.0], [4.0, 4.0], [None, None]),
                         ([30.0, 31.0], [4.0, 4.0], [None, None]),
                         ([31.0, 32.5], [3.8, 3.9], [4.0, 4.1])] * 20)
        out_json = self.build / "fg.json"
        with mock.patch.object(qr, "run", FakeRun()), \
                mock.patch.object(qr, "read_wav_f32", lambda p: "pcm"), \
                mock.patch.object(qr, "align", lambda o, d: ("o", "d", 0)), \
                mock.patch.object(qr, "window_profile", lambda o, d: next(profiles)):
            rows = qr.race_fgaincod("o", "src.wav", 1.0, rates=(192,), json_out=out_json)
        self.assertEqual([r["leg"] for r in rows],
                         ["auto", "pin-0x4", "pin-2", "search-1ax", "search-2ax"])
        self.assertEqual(rows[1]["d_snr_db"], 0.0)
        self.assertAlmostEqual(rows[2]["d_snr_db"], 1.25)
        self.assertIsNone(rows[2]["d_mos"])            # the baseline has no MOS to pair
        self.assertEqual(json.loads(out_json.read_text()), rows)

    def test_coupling_fast_mdct_and_crosscheck_tables(self):
        with mock.patch.object(qr, "encode_and_decode", lambda *a, **k: "pcm"), \
                mock.patch.object(qr, "align", lambda o, d, **k: ("o", "d", 0)), \
                mock.patch.object(qr, "band_measures", lambda o, d: (40.0, 1.5)), \
                mock.patch.object(qr, "snr_db", lambda o, d: 35.0):
            qr.race_coupling("src", "orig")
            qr.race_fast_mdct("src", "orig")
        text = self.out.getvalue()
        self.assertIn("Baseband delta positive", text)
        self.assertIn("delta = fast SNR - direct SNR", text)
        fake = FakeRun()
        with mock.patch.object(qr, "run", fake), \
                mock.patch.object(qr, "read_wav_f32", lambda p: "pcm"), \
                mock.patch.object(qr, "align", lambda o, d: (Num(), Num(), 0)), \
                mock.patch.object(qr, "np", mock.MagicMock(log10=lambda x: 3.0,
                                                           sum=lambda x: 1.0)), \
                mock.patch.object(qr, "dolby_decode", lambda coded, wav: False):
            qr.crosscheck("orig", "src")
        tokens = [c[-1] for c in fake.commands if c[1:2] == ["eac3-encode"]]
        self.assertEqual(tokens, ["nodither", "cpl+nodither", "spx+nodither", "aht+nodither",
                                  "all+nodither"])
        self.assertIn("n/a", self.out.getvalue())

    def test_dolby_decode(self):
        drp = self.build / "drp"
        with mock.patch.object(qr, "DRP", drp):
            self.assertFalse(qr.dolby_decode("a.ec3", "a.wav"))
            drp.mkdir()
            (drp / "gst-launch-1.0.exe").write_text("")
            wav = self.build / "a.wav"
            def fail(cmd, **kw):
                return subprocess.CompletedProcess(cmd, 1, "", "no element\nx")
            with mock.patch.object(qr.subprocess, "run", fail):
                self.assertFalse(qr.dolby_decode("a.ec3", wav))

            def ok(cmd, **kw):
                self.assertIn(str(drp / "gst-plugins"), kw["env"]["GST_PLUGIN_PATH"])
                wav.write_bytes(b"RIFF")
                return subprocess.CompletedProcess(cmd, 0, "", "")
            with mock.patch.object(qr.subprocess, "run", ok):
                self.assertTrue(qr.dolby_decode("a.ec3", wav))
        self.assertIn("dolby decode failed: ['no element']", self.out.getvalue())


class ScoringPlumbing(QrTestCase):
    """The decode-then-score wrappers: which decoder runs, and how strictly.
    The strict FFmpeg reader is what turns a malformed frame into a gate
    failure rather than quiet noise, so its flags are asserted exactly."""

    def setUp(self):
        super().setUp()
        self.fake = FakeRun()
        self.stack.enter_context(mock.patch.object(qr, "run", self.fake))
        self.stack.enter_context(mock.patch.object(qr, "read_wav_f32", lambda p: "pcm"))
        self.stack.enter_context(mock.patch.object(qr, "align",
                                                   lambda o, d, **k: (Num(), Num(), k)))
        self.stack.enter_context(mock.patch.object(qr, "spectral_scores",
                                                   lambda o, d: (3.0, -1.0)))
        self.stack.enter_context(mock.patch.object(qr, "perceptual_score",
                                                   lambda o, d: 4.1))
        self.stack.enter_context(mock.patch.object(
            qr, "np", mock.MagicMock(log10=lambda x: 2.0, sum=lambda x: 1.0)))

    @property
    def wav(self):
        return str(self.build / "w.wav")

    def test_decode_scores_strictness(self):
        self.assertEqual(qr.decode_scores("o", "c.ac3", self.wav), (20.0, 3.0, -1.0, None))
        strict = self.fake.commands[-1]
        self.assertEqual(strict[:4], ["ffmpeg", "-v", "error", "-y"])
        self.assertEqual(strict[4:7], ["-xerror", "-err_detect", qr.FFMPEG_ERR_DETECT])
        self.assertEqual(qr.decode_scores("o", "c.ac3", self.wav, strict=False,
                                          perceptual=True)[3], 4.1)
        self.assertNotIn("-xerror", self.fake.commands[-1])

    def test_own_decoder_paths(self):
        self.assertEqual(qr.decode_scores_ours("o", "c.ec3", self.wav, perceptual=True),
                         (20.0, 3.0, -1.0, 4.1))
        self.assertEqual(self.fake.commands[-1], ["ac3cli", "decode", "c.ec3", self.wav])
        self.assertEqual(qr.decode_scores_ours_fixed("o", "c.ec3", self.wav),
                         (20.0, 3.0, -1.0, None))
        self.assertEqual(qr.score_fixed("o", "d")[0], 20.0)

    def test_encode_and_decode_flags(self):
        qr.encode_and_decode("src.wav", "cpl", 192, couple=True, extra_flag="fast-mdct=off")
        encode, decode = self.fake.commands[-2:]
        self.assertEqual(encode[-3:], ["192", "couple", "fast-mdct=off"])
        self.assertIn("-xerror", decode)
        qr.encode_and_decode("src.wav", "plain", 96)
        self.assertEqual(self.fake.commands[-2][-1], "96")

    def test_crosscheck_with_the_reference_decoder(self):
        with mock.patch.object(qr, "dolby_decode", lambda coded, wav: True), \
                mock.patch.object(qr, "agreement_db", lambda a, b: (88.8, 0)):
            qr.crosscheck("orig", "src")
        self.assertIn("88.8", self.out.getvalue())

    def test_decode_baseline_is_a_plain_ffmpeg_decode(self):
        self.assertEqual(qr._decode_baseline("b/ffmpeg.ec3", "leg_ffmpeg"), "pcm")
        self.assertEqual(self.fake.commands[-1][-1],
                         str(self.build / "spectrogram_leg_ffmpeg.wav"))

    def test_object_scores_refuses_audio_shorter_than_the_trim(self):
        with self.assertRaisesRegex(SystemExit, "shorter than the warm-up/cool-down window"):
            qr.object_scores(Arr(3000, 0), Arr(3000, 0))


class Spectrograms(QrTestCase):
    """render_spectrograms' own refusals: it must run after race_trend, must
    find every TREND_LEGS entry in the committed baseline, and must not draw
    a DEE panel the manifest marks unverified."""

    def setUp(self):
        super().setUp()
        self.repo = Path(self._tmp.name) / "repo"
        self.baseline = self.repo / "tests" / "golden" / "external-baseline"
        (self.baseline / "leg-a").mkdir(parents=True)
        (self.baseline / "leg-a" / "ffmpeg.ec3").write_bytes(b"x")
        (self.baseline / "leg-a" / "dee.ec3").write_bytes(b"x")
        self.manifest = {"baseline_version": 2, "legs": {"leg-a": {"scores": {
            "ffmpeg": {"snr_db": 20.0}, "dee": {"status": "unverified"}}}}}
        plt = mock.MagicMock(name="pyplot")
        self.panel_counts = []

        def subplots(n, *a, **k):
            self.panel_counts.append(n)
            return mock.MagicMock(), ([mock.MagicMock() for _ in range(n)] if n > 1
                                      else mock.MagicMock())
        plt.subplots.side_effect = subplots
        matplotlib = mock.MagicMock(name="matplotlib", pyplot=plt)
        self.decoded = []
        for patch in (mock.patch.dict(sys.modules, {"matplotlib": matplotlib,
                                                    "matplotlib.pyplot": plt}),
                      mock.patch.object(qr, "REPO", self.repo),
                      mock.patch.object(qr, "TREND_LEGS",
                                        [{"name": "leg-a", "codec": "eac3", "kbps": 96,
                                          "wav": Path("a.wav")}]),
                      mock.patch.object(qr, "materialise_fixture", lambda p: p),
                      mock.patch.object(qr, "read_wav_any", lambda p: Arr(20 * 48000)),
                      mock.patch.object(qr, "read_wav_f32", lambda p: Arr(20 * 48000)),
                      mock.patch.object(qr, "align", lambda o, d, **k: (o, d, 0)),
                      mock.patch.object(qr, "_plot_spectrogram", lambda ax, mono, title: None),
                      mock.patch.object(qr, "_decode_baseline",
                                        lambda coded, tag: self.decoded.append(tag) or
                                        Arr(20 * 48000))):
            self.stack.enter_context(patch)

    def write_manifest(self):
        (self.baseline / "manifest.json").write_text(json.dumps(self.manifest))

    def test_draws_original_ours_and_verified_baselines_only(self):
        self.write_manifest()
        (self.build / "trend_leg-a_landscape.wav").write_bytes(b"RIFF")
        qr.render_spectrograms(self.build / "png")
        self.assertEqual(self.decoded, ["leg-a_ffmpeg"])
        self.assertEqual(self.panel_counts, [3])
        self.assertIn("leg-a.png", self.out.getvalue())

    def test_must_run_after_trend(self):
        self.write_manifest()
        with self.assertRaisesRegex(SystemExit, "must run after"):
            qr.render_spectrograms(self.build / "png")

    def test_leg_missing_from_the_baseline_is_fatal(self):
        self.manifest["legs"] = {}
        self.write_manifest()
        (self.build / "trend_leg-a_landscape.wav").write_bytes(b"RIFF")
        with self.assertRaisesRegex(SystemExit, "not in the committed external baseline"):
            qr.render_spectrograms(self.build / "png")


class Main(QrTestCase):
    MODES = ("race_eac3", "race_fgaincod", "race_vbr", "seam_check", "crosscheck",
             "race_coupling", "race_fast_mdct", "race_ac3", "race_ci", "race_trend",
             "render_spectrograms", "race_objects")

    def run_main(self, *argv):
        calls = []
        patches = [mock.patch.object(sys, "argv", ["quality_race.py", *argv]),
                   mock.patch.object(qr, "make_material", lambda: ([0.0] * 48000, [0.0] * 48000)),
                   mock.patch.object(qr, "make_material_51", lambda: ["51"]),
                   mock.patch.object(qr, "make_material_transient", lambda: ("t", "t")),
                   mock.patch.object(qr, "write_wav_f32", lambda *a: None),
                   mock.patch.object(qr, "read_wav_f32", lambda p: Arr(48000)),
                   mock.patch.object(qr, "read_wav_any", lambda p: Arr(96000)),
                   mock.patch.object(qr, "materialise_fixture", Path)]
        for name in self.MODES:
            def record(*a, _name=name, **k):
                calls.append((_name, a, k))
                return []
            patches.append(mock.patch.object(qr, name, record))
        with contextlib.ExitStack() as stack:
            for p in patches:
                stack.enter_context(p)
            qr.main()
        return calls

    def test_every_mode_routes(self):
        expected = {"eac3": "race_eac3", "vbr": "race_vbr", "seam": "seam_check",
                    "crosscheck": "crosscheck", "couple": "race_coupling",
                    "fast-mdct": "race_fast_mdct", "ac3": "race_ac3", "ci": "race_ci",
                    "objects": "race_objects", "eac3-51": "race_eac3",
                    "eac3-transient": "race_eac3"}
        for mode, target in expected.items():
            calls = self.run_main(mode)
            self.assertEqual(calls[0][0], target, mode)
        self.assertEqual(self.run_main()[0][0], "race_ac3")          # default mode
        self.assertEqual(self.run_main("eac3-51")[0][2]["rates"], (192, 256, 384))

    def test_json_out_and_spectrograms(self):
        calls = self.run_main("trend", "--json-out", "t.json", "--spectrogram-dir", "png")
        self.assertEqual(calls[0], ("race_trend", (), {"json_out": Path("t.json")}))
        self.assertEqual(calls[1], ("render_spectrograms", (Path("png"),), {}))
        calls = self.run_main("objects", "--json-out", "o.json")
        self.assertEqual(calls[0][2], {"json_out": Path("o.json")})
        calls = self.run_main("vbr", "--json-out", "v.json")
        self.assertEqual(calls[0][2], {"json_out": Path("v.json")})

    def test_fgaincod_arguments(self):
        calls = self.run_main("fgaincod", "--tools", "auto", "--rates", "96,192",
                              "--with-51", "--json-out", "f.json")
        stereo, surround = calls
        self.assertEqual(stereo[2]["rates"], (96, 192))
        self.assertEqual(stereo[2]["tools"], "auto")
        self.assertEqual((surround[2]["layout"], surround[2]["nfchans"], surround[2]["tools"]),
                         ("51", 5, "cpl"))
        self.assertIs(surround[2]["rows"], stereo[2].get("rows", surround[2]["rows"]))

    def test_material_selection_and_validation(self):
        calls = self.run_main("ac3", "--material", "speech")
        self.assertEqual(calls[0][1][2], 2.0)             # seconds from the fixture
        self.assertIn("material: programme_speech_stereo.flac", self.out.getvalue())
        for argv, message in ((("ac3", "--material", "jazz"), "unknown material"),
                              (("ci", "--material", "music"), "not accepted by 'ci'"),
                              (("eac3-51", "--material", "music"), "5.1-only"),
                              (("warp",), "unknown race 'warp'")):
            with self.assertRaisesRegex(SystemExit, message):
                self.run_main(*argv)


if __name__ == "__main__":
    unittest.main()
