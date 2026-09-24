"""Unit tests for check_matrix_coverage.py, the "does the FFmpeg-oracle matrix
touch every CLI option at all" gate.

What it must catch: a command, layout, Annex E tool, Atmos mode or VBR/ABR
rate control the CLI exposes that run_codec_matrix.sh never exercises - and in
particular the 2026-08-17 false pass, where the `auto` tool set read as covered
only because `dialnorm=auto` appeared elsewhere in the script. Tokens that
appear only in `#` comments must not count either.

ac3cli is never run: check_matrix_coverage.run is patched with a fake that
answers the three introspection probes (usage table, unknown layout, unknown
tool set) the way the real binary does.

Run: python3 -m unittest discover -s tools/checks -p 'test_*.py'
"""

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_matrix_coverage as cmc

USAGE = """\
usage:
  ac3cli encode <in.wav> <out.ac3> [kbps]
  ac3cli sine <out> <s> <kbps> <hz> <pct> [layout]
  ac3cli eac3-sine <out> <s> <kbps> <hz> <pct> [layout]
  ac3cli eac3-encode <in.wav> <out> <kbps> [tools] [layout] [vbr]
  ac3cli play <in>
  ac3cli --version
vbr    off | q:0..1[,min:kbps][,max:kbps] | avg:kbps[,win:frames]
"""


class FakeCli:
    def __init__(self, usage=USAGE, layouts="mono | stereo | 51", eac3_layouts="stereo | 71",
                 tools="none | cpl | auto | numblkscod:N (cpl:N pins a band)"):
        self.usage, self.layouts, self.eac3_layouts, self.tools = (
            usage, layouts, eac3_layouts, tools)

    def __call__(self, cli, *args):
        if not args:
            return 1, self.usage, ""
        if args[0] == "sine":
            return 1, "", f"error: unknown layout '__coverage_probe__' ({self.layouts})\n"
        if args[0] == "eac3-sine":
            return 1, "", f"error: unknown layout '__coverage_probe__' ({self.eac3_layouts})\n"
        if args[0] == "eac3-encode":
            return 1, "", f"error: unknown tool set '__coverage_probe__' ({self.tools})"
        return 0, "", ""


GOOD_MATRIX = """\
#!/bin/bash
run encode in.wav out.ac3 192
for layout in mono stereo 51 71; do
  run sine out.ac3 1 192 1000 50 "$layout"
  run eac3-sine out.ec3 1 192 1000 50 "$layout"
done
for tools in none cpl:4 \\
    "auto+numblkscod:1"; do
  run_tolerate_eac3_tool_unsupported eac3-encode in.wav out.ec3 256 "$tools" 51
done
run eac3-encode in.wav out.ec3 256 none 51 q:0.5
run eac3-encode in.wav out.ec3 256 none 51 avg:192
run atmos objects in.wav out.ec3
run atmos bed51 in.wav out.ec3
"""


class Parsing(unittest.TestCase):
    def test_probes_parse_the_cli_answers(self):
        with mock.patch.object(cmc, "run", FakeCli()), tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(cmc.usage_commands("cli"),
                             {"encode", "sine", "eac3-sine", "eac3-encode", "play", "--version"})
            self.assertTrue(cmc.vbr_supported("cli"))
            self.assertTrue(cmc.abr_supported("cli"))
            self.assertEqual(cmc.layout_names("cli", Path(tmp), "sine"), {"mono", "stereo", "51"})
            self.assertEqual(cmc.tool_names("cli", Path(tmp)),
                             {"none", "cpl", "auto", "numblkscod"})

    def test_unparseable_probes_are_fatal(self):
        """An introspection answer the parser no longer understands must stop
        the check, not silently yield an empty canonical set (a pass)."""
        with tempfile.TemporaryDirectory() as tmp, \
                mock.patch.object(cmc, "run", lambda *a: (1, "garbage", "garbage")):
            with self.assertRaisesRegex(SystemExit, "could not parse any commands"):
                cmc.usage_commands("cli")
            with self.assertRaisesRegex(SystemExit, "layout list"):
                cmc.layout_names("cli", Path(tmp), "sine")
            with self.assertRaisesRegex(SystemExit, "tool list"):
                cmc.tool_names("cli", Path(tmp))
            self.assertFalse(cmc.vbr_supported("cli"))
            self.assertFalse(cmc.abr_supported("cli"))

    def test_comments_are_not_coverage(self):
        text = cmc.strip_comments("run encode x # also sine\n# 71 only in prose\nfoo#bar\n")
        self.assertTrue(cmc.covered(text, "encode"))
        self.assertFalse(cmc.covered(text, "sine"))
        self.assertFalse(cmc.covered(text, "71"))
        self.assertIn("foo#bar", text)   # '#' mid-word is not a comment

    def test_tool_tokens_come_only_from_where_tools_are_used(self):
        """The dialnorm=auto false pass: a value elsewhere is not a tool leg."""
        text = ("run eac3-encode a.wav b.ec3 256 none 51 dialnorm=auto\n"
                "for tools in cpl 'spx:3+aht' \"$extra\"; do :; done\n")
        self.assertEqual(cmc.matrix_tool_tokens(text), {"none", "cpl", "spx", "aht"})
        self.assertNotIn("auto", cmc.matrix_tool_tokens(text))

    def test_commands_invoked(self):
        self.assertEqual(cmc.commands_invoked(GOOD_MATRIX),
                         {"encode", "sine", "eac3-sine", "eac3-encode", "atmos"})


class Main(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.cli = self.tmp / "ac3cli"
        self.cli.write_text("")
        self.matrix = self.tmp / "matrix.sh"

    def tearDown(self):
        self._tmp.cleanup()

    def run_main(self, matrix_text, fake=None, cli=None):
        self.matrix.write_text(matrix_text)
        argv = ["x", "--cli", str(cli or self.cli), "--matrix", str(self.matrix)]
        buf = io.StringIO()
        code = 0
        with mock.patch.object(sys, "argv", argv), mock.patch.object(cmc, "FAILURES", []), \
                mock.patch.object(cmc, "run", fake or FakeCli()), contextlib.redirect_stdout(buf):
            try:
                cmc.main()
            except SystemExit as exc:
                code = exc.code
        return code, buf.getvalue()

    def test_complete_matrix_passes(self):
        code, out = self.run_main(GOOD_MATRIX)
        self.assertIn(code, (0, None), out)
        self.assertIn("matrix covers every option", out)
        self.assertNotIn("FAIL", out)

    def test_every_kind_of_gap_fails(self):
        gappy = (GOOD_MATRIX.replace("run encode in.wav out.ac3 192\n", "")
                 .replace("mono stereo 51 71", "mono stereo 51")
                 .replace('"auto+numblkscod:1"', "numblkscod:1")
                 .replace("q:0.5", "").replace("avg:192", "")
                 .replace("run atmos bed51 in.wav out.ec3\n", ""))
        code, out = self.run_main(gappy + "echo dialnorm=auto\n# for tools in auto; do\n")
        self.assertEqual(code, 1)
        for gap in ("commands (encode)", "eac3-layouts (71)", "annex-e-tools (auto)",
                    "atmos-modes (bed51)", "eac3-vbr (q:<quality>)", "eac3-abr (avg:<kbps>)"):
            self.assertIn(gap, out)
        self.assertIn("6 coverage gap(s)", out)

    def test_older_builds_skip_vbr_and_abr(self):
        no_abr = USAGE.replace("vbr    off", "vbrx   off")
        _, out = self.run_main(GOOD_MATRIX, FakeCli(usage=no_abr))
        self.assertIn("SKIP  eac3-abr", out)
        no_vbr = no_abr.replace(" [vbr]", "")
        _code, out = self.run_main(GOOD_MATRIX, FakeCli(usage=no_vbr))
        self.assertIn("SKIP  eac3-vbr", out)

    def test_matrix_with_no_tool_sets_is_fatal(self):
        with self.assertRaisesRegex(SystemExit, "found no tool sets"):
            self.matrix.write_text("run encode a b\n")
            argv = ["x", "--cli", str(self.cli), "--matrix", str(self.matrix)]
            with mock.patch.object(sys, "argv", argv), mock.patch.object(cmc, "FAILURES", []), \
                    mock.patch.object(cmc, "run", FakeCli()), \
                    contextlib.redirect_stdout(io.StringIO()):
                cmc.main()

    def test_missing_cli_is_fatal(self):
        with self.assertRaisesRegex(SystemExit, "ac3cli not found"):
            argv = ["x", "--cli", str(self.tmp / "nope"), "--matrix", str(self.matrix)]
            with mock.patch.object(sys, "argv", argv):
                cmc.main()

    def test_real_matrix_script_has_tool_legs(self):
        """The committed matrix must still be readable by matrix_tool_tokens -
        otherwise main() would refuse to run in CI."""
        real = cmc.REPO / "tools" / "ci" / "run_codec_matrix.sh"
        self.assertTrue(cmc.matrix_tool_tokens(real.read_text()))


if __name__ == "__main__":
    unittest.main()
