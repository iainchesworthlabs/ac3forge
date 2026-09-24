"""Unit tests for footprint_report.py, the bare-metal footprint table.

It never fails a build, so what matters is that the numbers it publishes are
right: the GNU ld map parser must attribute .text/.rodata and .bss/COMMON to
the contributing object (both the one-line and the wrapped two-line section
form), must NOT count sections listed under "Discarded input sections" (that
once inflated .text by ~36%), and small objects are summed into one row.

arm-none-eabi-size/size are faked by patching subprocess.run.

Run: python3 -m unittest discover -s tools/checks -p 'test_*.py'
"""

import contextlib
import io
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import footprint_report as fr

MAP = """\
Archive member included to satisfy reference by file (symbol)

Discarded input sections

 .text.unused   0x00000000     0x9000 build/unused.o
 .text._ZN3ac3LongDiscardedNameEv
                0x00000000     0x9000 build/decoder.o

Memory Configuration

Linker script and memory map

 .text._ZN3ac312FrameDecoder6decodeEv
                0x00001000     0x1800 build/src/decoder.o
 .text.small    0x00002800       0x40 lib/libc.a(memcpy.o)
 .rodata.tables 0x00003000      0x900 build/src/decoder.o
 .bss.state     0x20000000     0x1000 lib/libac3.a(state.o)
 .data.init     0x20001000      0x100 build/src/decoder.o
 .text.zero     0x00004000        0x0 build/src/zero.o
 .text.dangling
 not a body line
"""

PROBE = """\
heap.peak_bytes=65536 static.frame_decoder_bytes=2048
ac3.steady_allocs_per_frame=0 heap.retained_bytes=0 junk result=PASS
"""


class Parsers(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def test_map_attribution_skips_discarded_sections(self):
        path = self.dir / "a.map"
        path.write_text(MAP)
        self.assertEqual(fr.read_map(path), {
            "decoder.o": {"text": 0x1800 + 0x900, "bss": 0},
            "memcpy.o": {"text": 0x40, "bss": 0},
            "state.o": {"text": 0, "bss": 0x1000},
        })
        self.assertEqual(fr.read_map(self.dir / "missing.map"), {})

    def test_probe_tokens(self):
        path = self.dir / "p.txt"
        path.write_text(PROBE)
        values = fr.read_probe(path)
        self.assertEqual(values["heap.peak_bytes"], "65536")
        self.assertEqual(values["result"], "PASS")
        self.assertNotIn("junk", values)
        self.assertEqual(fr.read_probe(self.dir / "nope"), {})

    def test_size_falls_back_to_host_size(self):
        calls = []

        def fake(cmd, **kw):
            calls.append(cmd[0])
            if cmd[0] == "arm-none-eabi-size":
                raise FileNotFoundError
            return subprocess.CompletedProcess(
                cmd, 0, "   text    data     bss     dec     hex filename\n"
                        "  70000     100    5000   75100   1255c x.elf\n", "")
        with mock.patch.object(fr.subprocess, "run", fake):
            self.assertEqual(fr.read_size(Path("x.elf")),
                             {"text": 70000, "data": 100, "bss": 5000, "total": 75100})
        self.assertEqual(calls, ["arm-none-eabi-size", "size"])

    def test_size_unavailable(self):
        def fake(cmd, **kw):
            if cmd[0] == "size":
                return subprocess.CompletedProcess(cmd, 0, "only a header\n", "")
            raise subprocess.CalledProcessError(1, cmd)
        with mock.patch.object(fr.subprocess, "run", fake):
            self.assertEqual(fr.read_size(Path("x.elf")), {})

    def test_human(self):
        self.assertEqual(fr.human(512), "512 B")
        self.assertEqual(fr.human(2048), "2.0 KiB")
        self.assertEqual(fr.human(3 * 1024 * 1024), "3.00 MiB")


class Main(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self._tmp.name)
        (self.dir / "p.txt").write_text(PROBE)
        (self.dir / "a.map").write_text(MAP)

    def tearDown(self):
        self._tmp.cleanup()

    def run_main(self, *args, size_out=""):
        def fake(cmd, **kw):
            return subprocess.CompletedProcess(cmd, 0, size_out, "")
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(sys, "argv", ["x", *args]), \
                mock.patch.object(fr.subprocess, "run", fake), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = fr.main()
        return rc, out.getvalue(), err.getvalue()

    def test_markdown_report(self):
        size = "text data bss dec hex filename\n1024 0 2048 3072 c00 x\n"
        rc, out, _ = self.run_main("--probe", str(self.dir / "p.txt"), "--elf", "x",
                                   "--map", str(self.dir / "a.map"), "--markdown",
                                   size_out=size)
        self.assertEqual(rc, 0)
        self.assertIn("### Static footprint", out)
        self.assertIn("| **Image total** | **3.0 KiB** (3072 bytes) |", out)
        self.assertIn("| Peak heap | 64.0 KiB |", out)
        self.assertIn("| Probe verdict | PASS |", out)
        self.assertIn("| decoder.o | 8.2 KiB text, 0 B bss |", out)
        self.assertIn("| (everything smaller, summed) | 64 B text, 0 B bss |", out)
        # state.o is exactly 4 KiB of bss: significant, listed on its own.
        self.assertIn("| state.o | 0 B text, 4.0 KiB bss |", out)

    def test_plain_text_report(self):
        rc, out, _ = self.run_main("--probe", str(self.dir / "p.txt"))
        self.assertEqual(rc, 0)
        self.assertIn("== Runtime footprint ==", out)
        self.assertIn("  Peak heap", out)

    def test_nothing_to_report_still_exits_zero(self):
        (self.dir / "empty.txt").write_text("no tokens here\n")
        rc, _out, err = self.run_main("--probe", str(self.dir / "empty.txt"))
        self.assertEqual(rc, 0)
        self.assertIn("nothing to report", err)
        rc, _, err = self.run_main()
        self.assertIn("nothing to report", err)


if __name__ == "__main__":
    unittest.main()
