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

import json
import math
import struct
import subprocess
import sys
import tempfile
import unittest
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


if __name__ == "__main__":
    unittest.main()
