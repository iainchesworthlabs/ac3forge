"""Unit tests for fuzz_ac4_encoder_space.py, the AC-4 encoder's input-space fuzzer.

ac3cli and ffprobe are never run. What is tested is the harness logic that decides
whether a defect is reported:

- the CRC and the sync frame walk, which read nothing of the encoder's, catching each
  framing defect they name;
- the trace comparison naming the first record that differs;
- draw_case purity, the configurations it draws, and the lengths a measurement needs;
- run_case()'s verdicts: a refusal only with the encoder's own message, an out-of-range
  rate or a frame rate 44.1 kHz does not have that encodes is a failure, a stream whose
  frames do not cover the input at its lag is a failure, and a stream whose traces differ
  is a failure.

Run: python3 -m unittest discover -s tools/ci -p 'test_*.py'
"""

import math
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
        self.assertEqual({c.channels for c in cases}, set(fa4.CHANNELS))
        # Seven and eight channels always name a 7.X pair, and nothing else does; 5.X and 7.X
        # never draw a rate in range below their least.
        for case in cases:
            pairs = [p for o in case.options for p in fa4.SEVEN_X if p in o]
            self.assertEqual(len(pairs), 1 if case.channels > 6 else 0)
            if case.channels > 2 and case.in_range:
                self.assertGreaterEqual(case.bitrate, fa4.MULTICHANNEL_LOWEST_KBPS)
        self.assertTrue(any("coding-configs" in o for c in cases for o in c.options))
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
            frame = fa4.FRAME
            if case.sample_rate == 48000:
                frame = math.ceil(fa4.FRAME_RATES[case.frame_rate_index][1])
            self.assertGreaterEqual(samples, 2 * frame)
            if case.measures:
                self.assertGreaterEqual(samples, 0.6 * case.sample_rate)
        # Every frame rate at 48 kHz, and now and then one 44.1 kHz does not have; the rate
        # modes, the I-frame options, and each metadata option.
        at_48k = {c.frame_rate_index for c in cases if c.sample_rate == 48000}
        self.assertEqual(at_48k, set(fa4.FRAME_RATES))
        self.assertTrue(any(not c.frame_rate_valid for c in cases))
        keys = {o.split("=", 1)[0] for o in options}
        self.assertTrue(
            {"frame-rate", "rate-mode", "iframe-interval", "iframes", "fragment", "dialnorm",
             "loudness", "drc", *fa4.DRC_MODES, "lorocmixlev", "lorosurmixlev", "ltrtcmixlev",
             "ltrtsurmixlev", "lfemix", "dmixmod", "loro-correction", "ltrt-correction",
             "dialogue-channels", "dialogue-method", "dialogue-max-gain"} <= keys
        )
        self.assertTrue({"rate-mode=average", "rate-mode=variable", "dialogue-method=mid",
                         "dialogue-method=cross"} <= options)
        self.assertTrue(any("drc-gains-" in o for o in options))
        self.assertTrue(any(c.stem for c in cases))
        # The cross-channel method only with a stem, and the downmix only in 5.X and 7.X.
        for case in cases:
            if "dialogue-method=cross" in case.options:
                self.assertTrue(case.stem)
            if any(o.startswith(("lorocmixlev=", "dmixmod=", "lfemix=")) for o in case.options):
                self.assertGreaterEqual(case.channels, 5)


def completed(returncode=0, stdout="", stderr=""):
    return subprocess.CompletedProcess([], returncode, stdout, stderr)


class RunCase(unittest.TestCase):
    def case(self, bitrate=192):
        case = fa4.draw_case(7)
        case.bitrate = bitrate
        case.options = []
        case.mp4 = False
        case.sample_rate = 48000
        case.frame_rate_index = 13
        case.stem = False
        return case

    def test_a_frame_rate_441_khz_lacks_refused_with_its_message(self):
        case = self.case()
        case.sample_rate = 44100
        case.frame_rate_index = 2
        refusal = completed(1, stderr=fa4.REFUSALS["frame rate at 44.1 kHz"])
        with (
            tempfile.TemporaryDirectory() as tmp,
            mock.patch.object(fa4, "_run", return_value=refusal),
        ):
            result = fa4.run_case("ac3cli", None, case, tmp)
        self.assertEqual(result.status, "refused")
        encoded = completed(0, stdout="encoded 3 AC-4 frames")
        with (
            tempfile.TemporaryDirectory() as tmp,
            mock.patch.object(fa4, "_run", return_value=encoded),
        ):
            result = fa4.run_case("ac3cli", None, case, tmp)
        self.assertEqual(result.status, "fail")

    def test_a_rate_too_low_for_the_least_frame_refused_if_frames_of_the_cap_encode(self):
        # 16 kbps at 120 fps is frames of 16 2/3 bytes: refused, and at FRAME_BYTES_CAP bytes a
        # frame, 384 kbps, encoded.
        case = self.case(16)
        case.frame_rate_index = 12

        def fake(accept_higher):
            def run(argv):
                kbps = int(str(argv[4]))
                if kbps == 16 or not accept_higher:
                    return completed(1, stderr=fa4.REFUSALS["rate out of range"])
                return completed(0, stdout="encoded 3 AC-4 frames")
            return run

        for accept_higher, status in ((True, "refused"), (False, "fail")):
            with (
                tempfile.TemporaryDirectory() as tmp,
                mock.patch.object(fa4, "_run", side_effect=fake(accept_higher)),
            ):
                result = fa4.run_case("ac3cli", None, case, tmp)
            self.assertEqual(result.status, status)
        # A refusal of frames the cap holds is a failure outright, with no second encode.
        case.bitrate = 400
        refusal = completed(1, stderr=fa4.REFUSALS["rate out of range"])
        with (
            tempfile.TemporaryDirectory() as tmp,
            mock.patch.object(fa4, "_run", return_value=refusal) as run,
        ):
            result = fa4.run_case("ac3cli", None, case, tmp)
        self.assertEqual((result.status, run.call_count), ("fail", 1))
        self.assertIn("exit 1", result.detail)

    def test_frames_that_do_not_cover_the_input_fail(self):
        # At 25 fps, 1 920 samples a frame: two frames short of the input and its lag.
        case = self.case()
        case.frame_rate_index = 2
        frames = (case.blocks * fa4.BLOCK + 4000) // 1920 - 1
        stdout = f"encoded {frames} AC-4 frames\n lags the input by 4000 samples"
        with (
            tempfile.TemporaryDirectory() as tmp,
            mock.patch.object(fa4, "_run", return_value=completed(0, stdout=stdout)),
        ):
            result = fa4.run_case("ac3cli", None, case, tmp)
        self.assertEqual((result.status, result.stage), ("fail", "encode"))
        self.assertIn("decode to", result.detail)

    def test_an_out_of_range_rate_refused_with_its_message(self):
        with (
            tempfile.TemporaryDirectory() as tmp,
            mock.patch.object(
                fa4, "_run", return_value=completed(1, stderr=fa4.REFUSALS["rate out of range"])
            ),
        ):
            result = fa4.run_case("ac3cli", None, self.case(4), tmp)
        self.assertEqual(result.status, "refused")

    def test_a_case_wrong_on_both_counts_takes_either_refusal(self):
        # An out-of-range rate at 44.1 kHz with another frame rate: ac3cli names the frame rate
        # first (CI, 2026-09-25, case 5756050987806798014).
        case = self.case(4)
        case.sample_rate = 44100
        case.frame_rate_index = 2
        for why in ("frame rate at 44.1 kHz", "rate out of range"):
            with (
                tempfile.TemporaryDirectory() as tmp,
                mock.patch.object(fa4, "_run", return_value=completed(1, stderr=fa4.REFUSALS[why])),
            ):
                result = fa4.run_case("ac3cli", None, case, tmp)
            self.assertEqual((result.status, result.detail), ("refused", why))
        with (
            tempfile.TemporaryDirectory() as tmp,
            mock.patch.object(fa4, "_run", return_value=completed(134, stderr="abort")),
        ):
            result = fa4.run_case("ac3cli", None, case, tmp)
        self.assertEqual(result.status, "fail")
        self.assertIn("rate out of range and a frame rate at 44.1 kHz", result.detail)

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
                return completed(
                    0,
                    stdout=f"encoded {frames} AC-4 frames\n lags the input by {fa4.LAG} samples",
                )
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
