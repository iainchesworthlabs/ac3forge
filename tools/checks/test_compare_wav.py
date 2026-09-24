"""Unit tests for compare_wav.py, the gold-reference gate's comparator.

stdlib `unittest`, not pytest, for the same reason compare_wav.py itself is
stdlib-only: this runs in ci.yml's script-lint job, which installs ruff,
shellcheck and actionlint and nothing else, and a test that needs a new pinned
dependency to run is a test that will not be run.

The regression this file exists to hold down is the one that motivated
per-channel floors: `tools/checks/verify_gold_reference.sh` used to apply ONE
SNR floor to all six channels of a 5.1 fixture, with that floor set low enough
for the dither-dominated surrounds to pass. On ext_ac3_51_448_dee that left
the centre channel measuring 58.11 dB against a 22 dB floor - 36 dB of room to
collapse in without failing anything - which is exactly the class of defect
(a per-channel syntax misread) the external-baseline fixtures were added to
catch. test_per_channel_floor_catches_what_scalar_misses is that scenario.

Run: python3 -m unittest discover -s tools/checks -p 'test_*.py'
"""

import contextlib
import io
import json
import math
import struct
import subprocess
import sys
import tempfile
import unittest
import unittest.mock as mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import compare_wav

SCRIPT = Path(__file__).resolve().parent / "compare_wav.py"
RATE = 48000
FRAMES = 8192


def write_wav_f32(path: Path, channels: list[list[float]]) -> None:
    """Float32 WAVE_FORMAT_IEEE_FLOAT, the format ac3cli's own decode writes -
    see compare_wav.py's read_channels. Float rather than PCM16 so a test can
    place a channel at an exact SNR without quantization moving it."""
    nch = len(channels)
    frames = len(channels[0])
    payload = bytearray()
    for i in range(frames):
        for c in range(nch):
            payload += struct.pack("<f", channels[c][i])
    block_align = nch * 4
    fmt = struct.pack("<HHIIHH", 3, nch, RATE, RATE * block_align, block_align, 32)
    riff = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + \
           b"data" + struct.pack("<I", len(payload)) + bytes(payload)
    path.write_bytes(b"RIFF" + struct.pack("<I", len(riff)) + riff)


def tone(frames: int = FRAMES, freq: float = 997.0, amp: float = 0.5) -> list[float]:
    return [amp * math.sin(2.0 * math.pi * freq * i / RATE) for i in range(frames)]


def degrade(signal: list[float], target_snr_db: float) -> list[float]:
    """signal + a deterministic error at exactly `target_snr_db`.

    Alternating +e/-e has power e^2 per sample and is orthogonal to nothing in
    particular, which is all this needs: the point is a known error POWER, not
    a realistic error spectrum."""
    signal_power = sum(v * v for v in signal)
    noise_power = signal_power / (10.0 ** (target_snr_db / 10.0))
    e = math.sqrt(noise_power / len(signal))
    return [v + (e if i % 2 == 0 else -e) for i, v in enumerate(signal)]


class ResolveThresholds(unittest.TestCase):
    def test_scalar_is_repeated(self):
        """No vector given -> every channel gets the scalar. This is the
        pre-per-channel behaviour every unconverted call site still relies on."""
        self.assertEqual(compare_wav.resolve_thresholds(None, 55.0, 6), [55.0] * 6)

    def test_vector_is_parsed_in_order(self):
        self.assertEqual(
            compare_wav.resolve_thresholds("51,57,52,76,16,16", 22.0, 6),
            [51.0, 57.0, 52.0, 76.0, 16.0, 16.0])

    def test_length_mismatch_is_fatal(self):
        """Not padded, not truncated: a 5.1 vector against a stereo file would
        otherwise gate two channels and silently drop four floors."""
        with self.assertRaises(SystemExit):
            compare_wav.resolve_thresholds("51,57,52,76,16,16", 22.0, 2)

    def test_non_numeric_is_fatal(self):
        with self.assertRaises(SystemExit):
            compare_wav.resolve_thresholds("51,fifty-seven,52", 22.0, 3)


class ChannelLabels(unittest.TestCase):
    def test_51_is_named(self):
        self.assertEqual(compare_wav.channel_labels(6),
                          ["L", "R", "C", "LFE", "Ls", "Rs"])

    def test_stereo_is_named(self):
        self.assertEqual(compare_wav.channel_labels(2), ["L", "R"])

    def test_unknown_layout_falls_back_to_indices(self):
        """Never guess a mapping for a layout that is not named - the same rule
        docs/quality-trend.md's channelLabel() follows."""
        self.assertEqual(compare_wav.channel_labels(4),
                          ["ch0", "ch1", "ch2", "ch3"])


class EndToEnd(unittest.TestCase):
    """Drives the script the way verify_gold_reference.sh does - argv in, exit
    code and --json-out out - rather than calling main() directly, so the
    argument wiring is covered too."""

    def run_compare(self, ref_channels, act_channels, *args):
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            ref, act, out = tmp / "ref.wav", tmp / "act.wav", tmp / "out.json"
            write_wav_f32(ref, ref_channels)
            write_wav_f32(act, act_channels)
            proc = subprocess.run(
                [sys.executable, str(SCRIPT), str(ref), str(act),
                 "--json-out", str(out), *args],
                capture_output=True, text=True, check=False)
            result = json.loads(out.read_text()) if out.exists() else None
            return proc, result

    def test_identical_files_pass_and_report_bit_exact(self):
        ref = [tone(freq=200.0 * (c + 1)) for c in range(6)]
        proc, result = self.run_compare(ref, [list(c) for c in ref],
                                         "--min-snr-db", "55")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        # json_safe_db clamps a bit-exact +inf to the 200.0 sentinel so the
        # file stays parseable by JSON.parse in the browser.
        self.assertEqual(result["worst_db"], 200.0)
        self.assertTrue(result["pass"])

    def test_per_channel_floor_catches_what_scalar_misses(self):
        """The regression this whole change exists for.

        Six channels: the surrounds sit at ~23 dB (as the real DEE fixture's
        do, dither-dominated), and the centre channel has collapsed from its
        usual ~58 dB to 30 dB - a 28 dB regression. Against the single 22 dB
        floor that collapse passes, because 30 >= 22. Against the centre
        channel's own 52 dB floor it fails, which is the point."""
        ref = [tone(freq=200.0 * (c + 1)) for c in range(6)]
        act = [list(c) for c in ref]
        act[2] = degrade(ref[2], 30.0)   # C: collapsed but still above 22
        act[4] = degrade(ref[4], 23.0)   # Ls: normal, dither-dominated
        act[5] = degrade(ref[5], 23.0)   # Rs: normal

        scalar, scalar_json = self.run_compare(ref, act, "--min-snr-db", "22")
        self.assertEqual(scalar.returncode, 0,
                          "the single-floor form is supposed to MISS this - if it "
                          "now catches it, this test no longer proves anything")
        self.assertTrue(scalar_json["pass"])

        per_ch, per_ch_json = self.run_compare(
            ref, act, "--min-snr-db-per-channel", "51,57,52,76,16,16")
        self.assertEqual(per_ch.returncode, 1)
        self.assertFalse(per_ch_json["pass"])
        self.assertIn("C", per_ch.stdout)

    def test_json_carries_the_per_channel_schema(self):
        ref = [tone(freq=200.0 * (c + 1)) for c in range(6)]
        act = [degrade(c, 40.0) for c in ref]
        _, result = self.run_compare(ref, act,
                                      "--min-snr-db-per-channel", "30,30,30,30,30,30")
        self.assertEqual(result["thresholds_db"], [30.0] * 6)
        self.assertEqual(result["channel_labels"], ["L", "R", "C", "LFE", "Ls", "Rs"])
        self.assertEqual(len(result["headroom_db"]), 6)
        for snr, floor, head in zip(result["channels_db"], result["thresholds_db"],
                                     result["headroom_db"], strict=True):
            self.assertAlmostEqual(head, snr - floor, places=6)
        self.assertIn(result["tightest_channel"], range(6))
        self.assertAlmostEqual(result["tightest_headroom_db"],
                                min(result["headroom_db"]), places=6)

    def test_threshold_db_still_belongs_to_the_worst_channel(self):
        """Backward compatibility: docs/performance-quality.md computes
        `worst_db - threshold_db`, so the scalar must stay the floor the worst
        channel was actually judged against, not an unrelated channel's."""
        ref = [tone(freq=200.0 * (c + 1)) for c in range(6)]
        act = [list(c) for c in ref]
        act[4] = degrade(ref[4], 25.0)   # Ls is the worst-scoring channel
        _, result = self.run_compare(
            ref, act, "--min-snr-db-per-channel", "51,57,52,76,16,16")
        worst_idx = result["channels_db"].index(result["worst_db"])
        self.assertEqual(worst_idx, 4)
        self.assertEqual(result["threshold_db"], 16.0)

    def test_scalar_mode_json_is_unchanged_for_old_consumers(self):
        ref = [tone(freq=200.0 * (c + 1)) for c in range(2)]
        act = [degrade(c, 40.0) for c in ref]
        _, result = self.run_compare(ref, act, "--min-snr-db", "30")
        self.assertEqual(result["threshold_db"], 30.0)
        self.assertEqual(result["thresholds_db"], [30.0, 30.0])


def write_wav_pcm16(path: Path, channels: list[list[float]], rate: int = RATE,
                    extensible: bool = False, junk: bytes = b"") -> None:
    """PCM16 (optionally WAVE_FORMAT_EXTENSIBLE, optionally preceded by an
    odd-sized chunk) - the other two shapes read_channels must accept."""
    nch = len(channels)
    frames = len(channels[0])
    payload = bytearray()
    for i in range(frames):
        for c in range(nch):
            payload += struct.pack("<h", max(-32768, min(32767, round(channels[c][i] * 32768))))
    block_align = nch * 2
    if extensible:
        fmt = struct.pack("<HHIIHH", 0xFFFE, nch, rate, rate * block_align, block_align, 16)
        fmt += struct.pack("<HHI", 22, 16, 0) + struct.pack("<H", 1) + bytes(14)
    else:
        fmt = struct.pack("<HHIIHH", 1, nch, rate, rate * block_align, block_align, 16)
    riff = b"WAVE"
    if junk:
        riff += b"LIST" + struct.pack("<I", len(junk)) + junk + (b"\0" if len(junk) & 1 else b"")
    riff += b"fmt " + struct.pack("<I", len(fmt)) + fmt + \
        b"data" + struct.pack("<I", len(payload)) + bytes(payload)
    path.write_bytes(b"RIFF" + struct.pack("<I", len(riff)) + riff)


class ReadChannels(unittest.TestCase):
    """The reader must accept every shape the two decoders emit and refuse,
    loudly, anything it cannot interpret - a reader that returned an empty or
    misscaled signal would make every SNR meaningless."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def test_pcm16_is_scaled_to_unit_range(self):
        path = self.tmp / "a.wav"
        write_wav_pcm16(path, [[0.5, -0.25, 0.0], [0.0, 0.125, -1.0]])
        chans, rate = compare_wav.read_channels(path)
        self.assertEqual(rate, RATE)
        self.assertEqual(chans, [[0.5, -0.25, 0.0], [0.0, 0.125, -1.0]])

    def test_extensible_with_odd_chunk_before_fmt(self):
        """The SubFormat GUID carries the real tag, and odd chunks are padded
        to a word boundary - misreading either loses the fmt/data chunks."""
        path = self.tmp / "b.wav"
        write_wav_pcm16(path, [[0.5, -0.5]], rate=44100, extensible=True, junk=b"abc")
        chans, rate = compare_wav.read_channels(path)
        self.assertEqual((chans, rate), ([[0.5, -0.5]], 44100))

    def test_float32_round_trips(self):
        path = self.tmp / "c.wav"
        write_wav_f32(path, [[0.25, -0.75]])
        self.assertEqual(compare_wav.read_channels(path), ([[0.25, -0.75]], RATE))

    def test_not_riff_is_fatal(self):
        path = self.tmp / "d.wav"
        path.write_bytes(b"OggS" + bytes(40))
        with self.assertRaisesRegex(SystemExit, "not a RIFF/WAVE"):
            compare_wav.read_channels(path)

    def test_missing_data_chunk_is_fatal(self):
        path = self.tmp / "e.wav"
        fmt = struct.pack("<HHIIHH", 1, 1, RATE, RATE * 2, 2, 16)
        riff = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt
        path.write_bytes(b"RIFF" + struct.pack("<I", len(riff)) + riff)
        with self.assertRaisesRegex(SystemExit, "missing fmt/data"):
            compare_wav.read_channels(path)

    def test_unsupported_format_is_fatal(self):
        """24-bit PCM is not silently read as 16-bit garbage."""
        path = self.tmp / "f.wav"
        fmt = struct.pack("<HHIIHH", 1, 1, RATE, RATE * 3, 3, 24)
        riff = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + \
            b"data" + struct.pack("<I", 6) + bytes(6)
        path.write_bytes(b"RIFF" + struct.pack("<I", len(riff)) + riff)
        with self.assertRaisesRegex(SystemExit, "unsupported format tag 1/24"):
            compare_wav.read_channels(path)


class Helpers(unittest.TestCase):
    def test_best_lag_finds_positive_and_negative_shifts(self):
        sig = [math.sin(i * 0.37) * math.cos(i * 0.011) for i in range(400)]
        delayed = [0.0] * 7 + sig          # actual lags reference by 7
        self.assertEqual(compare_wav.best_lag(sig, delayed, 16, 300), 7)
        self.assertEqual(compare_wav.best_lag(delayed, sig, 16, 300), -7)

    def test_align_trims_both_directions(self):
        self.assertEqual(compare_wav.align([1, 2, 3], [0, 1, 2, 3], 1), ([1, 2, 3], [1, 2, 3]))
        self.assertEqual(compare_wav.align([0, 0, 1, 2], [1, 2, 3], -2), ([1, 2], [1, 2]))

    def test_diff_rms_dbfs(self):
        self.assertEqual(compare_wav.diff_rms_dbfs([], []), -math.inf)
        self.assertEqual(compare_wav.diff_rms_dbfs([0.5], [0.5]), -math.inf)
        self.assertAlmostEqual(compare_wav.diff_rms_dbfs([0.1, 0.1], [0.0, 0.0]), -20.0)

    def test_snr_db_edges(self):
        self.assertEqual(compare_wav.snr_db([1.0], [1.0]), math.inf)
        self.assertEqual(compare_wav.snr_db([0.0, 0.0], [0.1, 0.1]), -math.inf)
        self.assertAlmostEqual(compare_wav.snr_db([1.0, 1.0], [1.1, 0.9]), 20.0)

    def test_json_safe_db(self):
        self.assertEqual(compare_wav.json_safe_db(math.inf), 200.0)
        self.assertEqual(compare_wav.json_safe_db(-math.inf), -200.0)
        self.assertEqual(compare_wav.json_safe_db(12.5), 12.5)


class MainInProcess(unittest.TestCase):
    """main() driven in-process (argv patched) on short signals with a small
    lag search, so every decision branch is exercised cheaply."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def run_main(self, ref, act, *args, act_rate=None):
        r, a = self.tmp / "r.wav", self.tmp / "a.wav"
        out = self.tmp / "sub" / "o.json"
        write_wav_f32(r, ref)
        if act_rate is None:
            write_wav_f32(a, act)
        else:
            write_wav_pcm16(a, act, rate=act_rate)
        argv = ["compare_wav.py", str(r), str(a), "--max-lag-samples", "4",
                "--probe-samples", "256", "--json-out", str(out), *args]
        buf = io.StringIO()
        with mock.patch.object(sys, "argv", argv), contextlib.redirect_stdout(buf):
            rc = compare_wav.main()
        return rc, buf.getvalue(), (json.loads(out.read_text()) if out.exists() else None)

    def test_rate_mismatch_fails(self):
        sig = [tone(frames=512)]
        rc, text, js = self.run_main(sig, sig, act_rate=44100)
        self.assertEqual(rc, 1)
        self.assertIn("sample rate mismatch", text)
        self.assertIsNone(js)

    def test_channel_count_mismatch_fails(self):
        sig = tone(frames=512)
        rc, text, _ = self.run_main([sig], [sig, sig])
        self.assertEqual(rc, 1)
        self.assertIn("channel count mismatch", text)

    def test_below_floor_fails_with_named_channel(self):
        ref = [tone(frames=512), tone(frames=512, freq=300.0)]
        act = [ref[0], degrade(ref[1], 10.0)]
        rc, text, js = self.run_main(ref, act, "--min-snr-db", "20")
        self.assertEqual(rc, 1)
        self.assertIn("channel 1 (R)", text)
        self.assertIn("below its own SNR floor\n", text)
        self.assertFalse(js["pass"])
        self.assertEqual(js["tightest_channel"], 1)

    def test_max_diff_dbfs_gates_even_when_snr_passes(self):
        """A quiet passage can clear the SNR ratio floor while the absolute
        difference is too loud; --max-diff-dbfs must catch that."""
        ref = [tone(frames=512, amp=0.5)]
        act = [degrade(ref[0], 30.0)]   # diff ~ -39 dBFS
        rc, text, js = self.run_main(ref, act, "--min-snr-db", "20", "--max-diff-dbfs", "-60")
        self.assertEqual(rc, 1)
        self.assertIn("difference threshold", text)
        self.assertIn("loudest channel difference", text)
        self.assertFalse(js["pass"])

    def test_max_diff_dbfs_passes_when_below(self):
        ref = [tone(frames=512, amp=0.5)]
        act = [degrade(ref[0], 30.0)]
        rc, text, js = self.run_main(ref, act, "--min-snr-db", "20", "--max-diff-dbfs", "-20",
                                     "--codec-label", "ac3", "--bitrate-kbps", "192")
        self.assertEqual(rc, 0, text)
        self.assertTrue(text.rstrip().endswith("PASS"))
        self.assertEqual((js["codec"], js["bitrate_kbps"]), ("ac3", 192))
        self.assertLess(js["worst_diff_dbfs"], -20)
        json.dumps(js, allow_nan=False)   # stays browser-parseable

    def test_bit_exact_diff_clamped_in_json(self):
        ref = [tone(frames=512)]
        rc, _, js = self.run_main(ref, ref, "--max-diff-dbfs", "-100")
        self.assertEqual(rc, 0)
        self.assertEqual(js["worst_diff_dbfs"], -200.0)
        self.assertEqual(js["headroom_db"], [200.0])


if __name__ == "__main__":
    unittest.main()
