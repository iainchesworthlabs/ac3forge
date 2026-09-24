"""Unit tests for ac4_syntax_differential.py, the harness that compares the
C++ and Python AC-4 syntax transcriptions on streams no encoder writes.

The C++ trace tool is never built or run. What is tested is the harness:
sync-frame framing both ways (including the 0xFFFF extended size and the
0xAC41 CRC word), the synthetic-frame generator producing tables of contents
the reference parser can lay out, mutations that keep every substream size,
the C++ trace parser, and above all compare_case's verdicts - DIVERGE, END,
STOP, KIND and TOC must each be reported for the disagreement it names, and
identical traces must produce no finding. main() is driven end to end with a
fake trace tool that writes the Python side's own trace in the C++ format
(exit 0), or a refusal (exit 1).

Run: python3 -m unittest discover -s tools/checks -p 'test_*.py'
"""

import concurrent.futures
import contextlib
import io
import random
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ac4_syntax_differential as d


def write_cpp_trace(path, frames):
    """Serialise python_trace() output in ac4_syntax_trace.cpp's TSV format."""
    lines = []
    for fi, frame in frames.items():
        if isinstance(frame, tuple):
            lines.append(f"F\t{fi}\t{frame[1]}")
            continue
        lines.append("\t".join(["T", str(fi), *frame["layout"]]))
        for idx, (kind, recs, err) in frame["subs"].items():
            for off, width, value, name in recs:
                lines.append(f"R\t{fi}\t{idx}\t{off}\t{width}\t{value}\t{name}")
            status = "ok" if err is None else "FAIL"
            lines.append(f"S\t{fi}\t{idx}\t{kind}\t{status}\t{err or ''}")
    path.write_text("\n".join(lines) + "\n")


class Framing(unittest.TestCase):
    def test_sync_frame_round_trip_including_extended_size(self):
        small, big = b"\x01" * 10, b"\x02" * 0x10000
        data = d.sync_frame(small) + d.sync_frame(big)
        self.assertEqual(data[:4], b"\xac\x40\x00\x0a")
        self.assertEqual(data[14:18], b"\xac\x40\xff\xff")
        self.assertEqual(list(d.sync_frames(data)), [small, big])

    def test_sync_frames_stops_on_lost_sync_or_truncation(self):
        crc_frame = b"\xac\x41\x00\x02ab\x00\x00"       # 0xAC41 carries a CRC word
        self.assertEqual(list(d.sync_frames(crc_frame + b"\x00\x00\x00\x00")), [b"ab"])
        self.assertEqual(list(d.sync_frames(b"\xac\x40\x00\x09abc")), [])
        self.assertEqual(list(d.sync_frames(b"\xac\x40\xff\xff\x00")), [])
        self.assertEqual(list(d.sync_frames(b"\xac\x41\x00\x02ab")), [])   # CRC missing

    def test_bits_writer(self):
        w = d.Bits()
        w.put(0b101, 3)
        w.put_variable_bits(2, 2)
        self.assertEqual(w.to_bytes(), bytes([0b10110000]))
        with self.assertRaises(ValueError):
            d.Bits().put_variable_bits(4, 2)

    def test_randomise_from_keeps_prefix(self):
        rng = random.Random(1)
        out = d.randomise_from(rng, b"\xff\xff\xff", 12)
        self.assertEqual(out[0], 0xFF)
        self.assertEqual(out[1] & 0xF0, 0xF0)


class Generators(unittest.TestCase):
    def test_synthetic_frames_lay_out_and_mutate_size_preserving(self):
        rng = random.Random(7)
        for _ in range(20):
            raw = d.synthetic(rng)
            layout = d.python_layout(raw)
            spans = d.substream_spans(raw)
            self.assertEqual(layout, [f"{o}:{s}" for o, s in spans])
            self.assertEqual(sum(s for _, s in spans) + spans[0][0], len(raw))
            mutated, how = d.mutate(rng, raw)
            self.assertIn(how, ("tail", "flips", "mode"))
            self.assertEqual(len(mutated), len(raw))
            self.assertEqual(d.python_layout(mutated), layout)

    def test_mutate_with_no_nonempty_substream(self):
        with mock.patch.object(d, "substream_spans", lambda raw: [(4, 0)]):
            self.assertEqual(d.mutate(random.Random(0), b"x"), (None, None))

    def test_generate_writes_cases_and_index(self):
        rng = random.Random(11)
        with tempfile.TemporaryDirectory() as tmp_name:
            tmp = Path(tmp_name)
            source = tmp / "src.ac4"
            source.write_bytes(b"".join(d.sync_frame(d.synthetic(rng)) for _ in range(3)))
            empty = tmp / "empty.ac4"
            empty.write_bytes(b"")
            count = d.generate([source, empty], tmp / "cases", 8, 3, seed=5)
            files = sorted((tmp / "cases").glob("*.ac4"))
            index = (tmp / "cases" / "index.tsv").read_text().splitlines()
            self.assertEqual(count, len(files))
            self.assertEqual(len(index), count)
            self.assertEqual(sum(1 for f in files if f.name.startswith("s")), 3)
            self.assertTrue(any("\tsynthetic\t" in line for line in index))

    @unittest.expectedFailure
    def test_generate_skips_a_stream_that_is_not_ac4(self):
        """SUSPECTED BUG (ac4_syntax_differential.py:241): generate()'s
        `if not frames: continue  # a file with no sync frame at all` never
        sees a non-AC-4 file, because ac4_parse.iter_sync_frames raises
        ValueError("lost sync ...") rather than yielding nothing, and that
        call is outside the try. One stray *.ac4 under --streams aborts the
        whole run."""
        with tempfile.TemporaryDirectory() as tmp:
            junk = Path(tmp) / "junk.ac4"
            junk.write_bytes(b"not ac4 at all")
            self.assertEqual(d.generate([junk], Path(tmp) / "cases", 2, 0, seed=1), 0)


def frame(layout, subs):
    return {"layout": layout, "subs": subs}


REC = [(0, 15, 100, "audio_size_value"), (15, 1, 0, "b_more_bits"), (16, 2, 3, "mode")]


class CompareCase(unittest.TestCase):
    def verdicts(self, py, cpp_frames):
        with tempfile.TemporaryDirectory() as tmp:
            case = Path(tmp) / "case.ac4"
            write_cpp_trace(Path(tmp) / "case.cpp.tsv", cpp_frames)
            with mock.patch.object(d, "python_trace", lambda path, last: py):
                return [f[0] for f in d.compare_case((case, Path(tmp), True))]

    def test_identical_traces_have_no_findings(self):
        f = {0: frame(["4:10"], {0: ("audio", REC, None), 1: ("unreferenced", [], None)})}
        self.assertEqual(self.verdicts(f, f), [])

    def test_diverge(self):
        other = [REC[0], (15, 1, 1, "b_more_bits"), REC[2]]
        self.assertEqual(self.verdicts({0: frame(["4:10"], {0: ("audio", REC, None)})},
                                       {0: frame(["4:10"], {0: ("audio", other, None)})}),
                         ["DIVERGE"])

    def test_end_and_stop(self):
        short = REC[:2]
        self.assertEqual(self.verdicts({0: frame(["4:10"], {0: ("audio", REC, None)})},
                                       {0: frame(["4:10"], {0: ("audio", short, None)})}),
                         ["END"])
        self.assertEqual(self.verdicts({0: frame(["4:10"], {0: ("audio", short, "FAIL: x")})},
                                       {0: frame(["4:10"], {0: ("audio", REC, None)})}),
                         ["STOP"])
        self.assertEqual(self.verdicts({0: frame(["4:10"], {0: ("audio", REC, None)})},
                                       {0: frame(["4:10"], {0: ("audio", REC, "FAIL: y")})}),
                         ["STOP"])

    def test_kind(self):
        self.assertEqual(self.verdicts({0: frame(["4:10"], {0: ("audio", REC, None)})},
                                       {0: frame(["4:10"], {0: ("presentation", REC, None)})}),
                         ["KIND"])

    def test_toc_reported_once_and_later_frames_still_compared(self):
        py = {0: ("TOC", "ValueError()"), 1: frame(["4:10"], {0: ("audio", REC, None)}),
              2: frame(["4:10"], {0: ("audio", REC, None)})}
        cpp = {0: frame(["4:10"], {}), 1: frame(["4:11"], {}),
               2: frame(["4:10"], {0: ("audio", [*REC[:1], (15, 1, 1, "x")], None)})}
        self.assertEqual(self.verdicts(py, cpp), ["TOC", "DIVERGE"])

    def test_both_sides_refusing_is_no_finding(self):
        self.assertEqual(self.verdicts({0: ("TOC", "e")}, {0: ("TOC", "e")}), [])

    def test_layout_mismatch_is_toc(self):
        self.assertEqual(self.verdicts({0: frame(["4:10"], {})}, {0: frame(["4:12"], {})}),
                         ["TOC"])


class Main(unittest.TestCase):
    def run_main(self, writer, *extra):
        with tempfile.TemporaryDirectory() as tmp_name:
            tmp = Path(tmp_name)
            inputs = tmp / "corpus"
            inputs.mkdir()
            rng = random.Random(3)
            for i in range(3):
                (inputs / f"c{i}").write_bytes(d.sync_frame(d.synthetic(rng)))

            def fake_run(cmd, check):
                cpp_dir = Path(cmd[-4])
                self.assertNotIn("--last-frame", cmd)   # --inputs compares every frame
                for case in map(Path, cmd[-3:]):
                    writer(cpp_dir / f"{case.stem}.cpp.tsv", case)
                return subprocess.CompletedProcess(cmd, 0)
            argv = ["x", "--trace-tool", "tool", "--work-dir", str(tmp / "w"),
                    "--inputs", str(inputs), *extra]
            buf = io.StringIO()
            with mock.patch.object(sys, "argv", argv), \
                    mock.patch.object(d.subprocess, "run", fake_run), \
                    mock.patch.object(d, "ProcessPoolExecutor",
                                      concurrent.futures.ThreadPoolExecutor), \
                    contextlib.redirect_stdout(buf):
                rc = d.main()
            report = (tmp / "w" / "report.tsv").read_text()
        return rc, buf.getvalue(), report

    def test_agreeing_transcriptions_pass(self):
        rc, out, report = self.run_main(
            lambda path, case: write_cpp_trace(path, d.python_trace(case, False)))
        self.assertEqual(rc, 0, out)
        self.assertIn("3 inputs copied", out)
        self.assertIn("3 streams, 0 findings", out)
        self.assertEqual(report, "")

    def test_cpp_refusing_every_frame_fails(self):
        rc, out, report = self.run_main(lambda path, case: path.write_text("F\t0\tbad toc\n"))
        self.assertEqual(rc, 1)
        self.assertIn("TOC     one side reads the table of contents", out)
        self.assertEqual(report.count("TOC\t"), 3)


if __name__ == "__main__":
    unittest.main()
