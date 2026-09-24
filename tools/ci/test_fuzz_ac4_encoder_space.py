"""Unit tests for fuzz_ac4_encoder_space.py, the AC-4 encoder's input-space fuzzer.

ac3cli and ffprobe are never run. What is tested is the harness logic that decides
whether a defect is reported:

- the CRC and the sync frame walk, which read nothing of the encoder's, catching each
  framing defect they name;
- the trace comparison naming the first record that differs;
- draw_case purity, the configurations it draws, and the lengths dialnorm=auto needs;
- run_case()'s verdicts: a refusal only with the encoder's own message, an out-of-range
  rate that encodes is a failure, and a stream whose traces differ is a failure.

Run: python3 -m unittest discover -s tools/ci -p 'test_*.py'
"""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import fuzz_ac4_encoder_space as fa4


def sync_frame(raw, crc=True):
    size = len(raw)
    header = size.to_bytes(2, "big") if size < 0xFFFF else b"\xff\xff" + size.to_bytes(3, "big")
    out = (b"\xac\x41" if crc else b"\xac\x40") + header + raw
    if crc:
        out += fa4.crc16(header + raw).to_bytes(2, "big")
    return out


class Crc(unittest.TestCase):
    def test_known_value(self):
        # CRC-16 with polynomial 0x8005 and initial value 0 (CRC-16/BUYPASS) of "123456789".
        self.assertEqual(fa4.crc16(b"123456789"), 0xFEE8)

    def test_a_frame_with_its_crc_reads_back_as_zero(self):
        data = b"\x01\x02\x03\x04"
        crc = fa4.crc16(data)
        self.assertEqual(fa4.crc16(data + crc.to_bytes(2, "big")), 0)


class SyncFrameWalk(unittest.TestCase):
    def test_frames_tile_the_stream(self):
        raws = [bytes(range(10)), bytes(300), b"\x55" * 70000]
        frames, why = fa4.sync_frames(b"".join(sync_frame(r) for r in raws))
        self.assertEqual(why, "")
        self.assertEqual([len(f) for f in frames], [10, 300, 70000])

    def test_frames_without_a_crc(self):
        frames, why = fa4.sync_frames(sync_frame(b"abc", crc=False) * 3)
        self.assertEqual((len(frames), why), (3, ""))

    def test_a_bad_crc_is_named(self):
        data = bytearray(sync_frame(bytes(40)))
        data[10] ^= 0x01
        frames, why = fa4.sync_frames(bytes(data))
        self.assertIsNone(frames)
        self.assertIn("crc_word", why)

    def test_a_bad_sync_word_is_named(self):
        frames, why = fa4.sync_frames(sync_frame(b"x") + b"\x0b\x77\x00\x00")
        self.assertIsNone(frames)
        self.assertIn("0x0B77", why)

    def test_a_frame_past_the_end_is_named(self):
        frames, why = fa4.sync_frames(sync_frame(bytes(20))[:-5])
        self.assertIsNone(frames)
        self.assertIn("past the end", why)


class Traces(unittest.TestCase):
    def test_the_first_difference_is_named(self):
        a = [(0, 0, 0, 1, 0), (0, 1, 0, 15, 200)]
        self.assertEqual(fa4.first_difference(a, a), "")
        b = [(0, 0, 0, 1, 0), (0, 1, 0, 15, 201)]
        self.assertIn("record 1", fa4.first_difference(a, b))
        self.assertIn("2 records against 1", fa4.first_difference(a, a[:1]))

    def test_read_trace(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "t.tsv"
            path.write_text(
                "0\t0\t0\t1\t0\tb_additional_data\n3\t1\t15\t7\t124\tdialnorm_bits\n",
                encoding="utf-8",
            )
            self.assertEqual(fa4.read_trace(path), [(0, 0, 0, 1, 0), (3, 1, 15, 7, 124)])


class DrawCase(unittest.TestCase):
    def test_a_case_is_a_function_of_its_seed(self):
        self.assertEqual(fa4.draw_case(1234), fa4.draw_case(1234))

    def test_the_space_drawn(self):
        cases = [fa4.draw_case(seed) for seed in range(2000)]
        self.assertEqual({c.channels for c in cases}, {1, 2})
        self.assertEqual({c.sample_rate for c in cases}, set(fa4.SAMPLE_RATES))
        self.assertTrue(any(not c.in_range for c in cases))
        self.assertTrue(any(c.mp4 for c in cases))
        # The codec mode the rate picks, and each forced, and the experimental tools.
        options = {o for c in cases for o in c.options}
        self.assertTrue({"codec-mode=simple", "codec-mode=aspx"} <= options)
        self.assertTrue(any(o.startswith("experimental=") for o in options))
        self.assertTrue(any(not any(o.startswith("codec-mode=") for o in c.options) for c in cases))
        for case in cases:
            samples = case.blocks * fa4.BLOCK
            self.assertGreaterEqual(samples, 2 * fa4.FRAME)
            if "dialnorm=auto" in case.options:
                self.assertGreaterEqual(samples, 0.6 * case.sample_rate)


def completed(returncode=0, stdout="", stderr=""):
    return subprocess.CompletedProcess([], returncode, stdout, stderr)


class RunCase(unittest.TestCase):
    def case(self, bitrate=192):
        case = fa4.draw_case(7)
        case.bitrate = bitrate
        case.options = []
        case.mp4 = False
        return case

    def test_an_out_of_range_rate_refused_with_its_message(self):
        with (
            tempfile.TemporaryDirectory() as tmp,
            mock.patch.object(
                fa4, "_run", return_value=completed(1, stderr=fa4.REFUSALS["rate out of range"])
            ),
        ):
            result = fa4.run_case("ac3cli", None, self.case(4), tmp)
        self.assertEqual(result.status, "refused")

    def test_an_out_of_range_rate_that_encodes_fails(self):
        with (
            tempfile.TemporaryDirectory() as tmp,
            mock.patch.object(
                fa4, "_run", return_value=completed(0, stdout="encoded 3 AC-4 frames")
            ),
        ):
            result = fa4.run_case("ac3cli", None, self.case(4000), tmp)
        self.assertEqual(result.status, "fail")

    def test_a_refusal_without_the_message_fails(self):
        with (
            tempfile.TemporaryDirectory() as tmp,
            mock.patch.object(fa4, "_run", return_value=completed(134, stderr="abort")),
        ):
            result = fa4.run_case("ac3cli", None, self.case(), tmp)
        self.assertEqual((result.status, result.stage), ("fail", "encode"))

    def test_differing_traces_fail(self):
        case = self.case()
        frames = -(-(case.blocks * fa4.BLOCK + fa4.LAG) // fa4.FRAME)

        def fake(argv):
            argv = [str(a) for a in argv]
            trace = next(a.split("=", 1)[1] for a in argv if a.startswith("syntax-trace="))
            if argv[1] == "ac4-encode":
                Path(argv[3]).write_bytes(sync_frame(bytes(16)) * frames)
                Path(trace).write_text("0\t0\t0\t1\t0\tx\n", encoding="utf-8")
                return completed(0, stdout=f"encoded {frames} AC-4 frames")
            Path(trace).write_text("0\t0\t0\t1\t1\tx\n", encoding="utf-8")
            return completed(0)

        with (
            tempfile.TemporaryDirectory() as tmp,
            mock.patch.object(fa4, "_run", side_effect=fake),
            mock.patch.object(fa4, "python_trace", return_value=([(0, 0, 0, 1, 0)], [])),
        ):
            result = fa4.run_case("ac3cli", None, case, tmp)
        self.assertEqual((result.status, result.stage), ("fail", "traces"))
        self.assertIn("the decoder's", result.detail)


if __name__ == "__main__":
    unittest.main()
