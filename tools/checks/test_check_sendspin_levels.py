"""Unit tests for check_sendspin_levels.py, the Sendspin level check against a test sink.

stdlib `unittest`, as the other suites here are: this runs in ci.yml's script-lint job.
The console lines are the burst player's own format; the WAV files are made here, in
the two formats the check reads most: the test sink's 32-bit float, and 16-bit PCM.

Run: python3 -m unittest discover -s tools/checks -p 'test_*.py'
"""

import contextlib
import io
import math
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_sendspin_levels as check

CLOSING = (
    "sendspin.stream=bursts bursts=378 late=0 dropped=0 invalid=0 underruns=0 resyncs=0 "
    "silence_frames=256 skipped_frames=0 dropped_frames=0 repeated_frames=0 worst_error_us=0 "
    "burst_us=8120 heap_free=30120\r"
)


def console(rms: list[int], closing: str = CLOSING) -> str:
    lines = ["ESP-ROM:esp32s3-20210327", "sendspin: pairing token SP:0abc", closing]
    lines += [f"sendspin.rms[{i}]={value}\r" for i, value in enumerate(rms)]
    lines += ["I (9000) main_task: something after", ""]
    return "\n".join(lines)


def wav(channels: list[list[float]], tag: int = 3, bits: int = 32) -> bytes:
    """A WAV file of `channels`, each a list of samples in [-1, 1]."""
    frames = len(channels[0])
    if tag == 3:
        samples = b"".join(
            struct.pack("<f", channels[c][f]) for f in range(frames) for c in range(len(channels))
        )
    else:
        samples = b"".join(
            struct.pack("<h", round(channels[c][f] * 32767))
            for f in range(frames)
            for c in range(len(channels))
        )
    width = bits // 8
    fmt = struct.pack(
        "<HHIIHH",
        tag,
        len(channels),
        48000,
        48000 * width * len(channels),
        width * len(channels),
        bits,
    )
    body = (
        b"WAVE"
        + b"fmt "
        + struct.pack("<I", len(fmt))
        + fmt
        + b"data"
        + struct.pack("<I", len(samples))
        + samples
    )
    return b"RIFF" + struct.pack("<I", len(body)) + body


def sine(amplitude: float, frames: int = 4800) -> list[float]:
    return [amplitude * math.sin(2 * math.pi * 1000 * n / 48000) for n in range(frames)]


class BoardLevels(unittest.TestCase):
    def test_reads_the_last_closing_line_and_its_outputs(self):
        earlier = console([1, 2], CLOSING.replace("bursts=378", "bursts=5"))
        found = check.board_levels(earlier + console([353553, 176776]))
        self.assertIsNotNone(found)
        counters, rms = found
        self.assertEqual(counters["bursts"], 378)
        self.assertEqual(counters["heap_free"], 30120)
        self.assertEqual(rms, [353553, 176776])

    def test_a_pcm_stream_is_not_a_stream_of_bursts(self):
        self.assertIsNone(check.board_levels(console([1], CLOSING.replace("=bursts ", "=pcm ", 1))))

    def test_no_closing_line(self):
        self.assertIsNone(check.board_levels("ESP-ROM:esp32s3\nresult=pass\n"))


class WavLevels(unittest.TestCase):
    def test_float(self):
        levels = check.wav_levels(wav([sine(0.5), sine(0.25)]))
        self.assertEqual(len(levels), 2)
        self.assertAlmostEqual(levels[0], 353553, delta=5)
        self.assertAlmostEqual(levels[1], 176777, delta=5)

    def test_sixteen_bit(self):
        levels = check.wav_levels(wav([sine(0.5)], tag=1, bits=16))
        self.assertAlmostEqual(levels[0], 353553, delta=30)

    def test_not_a_wav(self):
        with self.assertRaises(check.Unreadable):
            check.wav_levels(b"not a wave file at all")


class Compare(unittest.TestCase):
    def setUp(self):
        self.counters = {"bursts": 378, "late": 0, "dropped": 0, "invalid": 0, "underruns": 0}

    def test_within_the_tolerance(self):
        self.assertEqual(
            check.compare(self.counters, [353600, 176700], [353553, 176777], 0.005, 30), []
        )

    def test_a_trim_is_a_mismatch(self):
        # -6 dB on the board: half the RMS.
        problems = check.compare(self.counters, [176777, 176777], [353553, 176777], 0.005, 30)
        self.assertEqual(len(problems), 1)
        self.assertIn("output 0", problems[0])
        self.assertIn("-6.02 dB", problems[0])

    def test_silence_passes_by_the_floor(self):
        self.assertEqual(check.compare(self.counters, [12], [0], 0.005, 30), [])

    def test_output_counts_differ(self):
        problems = check.compare(self.counters, [1, 2, 3], [1, 2], 0.005, 30)
        self.assertEqual(problems, ["the board has 3 outputs and the test sink's WAV 2"])

    def test_a_burst_that_did_not_play(self):
        self.counters["dropped"] = 2
        self.counters["underruns"] = 1
        problems = check.compare(self.counters, [1], [1], 0.005, 30)
        self.assertEqual(len(problems), 2)
        self.assertIn("dropped=2", problems[0])
        self.assertIn("underruns=1", problems[1])


class Main(unittest.TestCase):
    def run_main(self, console_text: str, wav_bytes: bytes) -> tuple[int, str]:
        with tempfile.TemporaryDirectory() as scratch:
            capture = Path(scratch) / "console.txt"
            capture.write_text(console_text, encoding="utf-8")
            reference = Path(scratch) / "reference.wav"
            reference.write_bytes(wav_bytes)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                status = check.main(
                    ["--console", str(capture), "--wav", str(reference), "--title", "Test"]
                )
            return status, out.getvalue()

    def test_levels_that_match_pass(self):
        status, out = self.run_main(console([353553, 176777]), wav([sine(0.5), sine(0.25)]))
        self.assertEqual(status, 0, out)
        self.assertIn("2 outputs within 0.5% of the test sink", out)

    def test_levels_that_do_not_fail_with_an_annotation(self):
        status, out = self.run_main(console([353553, 353553]), wav([sine(0.5), sine(0.25)]))
        self.assertEqual(status, 1)
        self.assertIn("::error title=Test::output 1", out)

    def test_no_stream_fails(self):
        status, out = self.run_main("ESP-ROM:esp32s3\n", wav([sine(0.5)]))
        self.assertEqual(status, 1)
        self.assertIn("no sendspin.stream=bursts line", out)

    def test_an_unreadable_wav_is_a_usage_error(self):
        status, out = self.run_main(console([1]), b"RIFF")
        self.assertEqual(status, 2)
        self.assertIn("cannot read", out)


if __name__ == "__main__":
    unittest.main()
