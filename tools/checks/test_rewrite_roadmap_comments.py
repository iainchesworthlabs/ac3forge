"""Unit tests for rewrite_roadmap_comments.py, the one-shot rewriter that
turns legacy "ROADMAP XXn" comment references into plain English.

What it must get right: known ids map to their names (a lettered sub-item
falls back to its base id, an unknown id says so rather than vanishing),
possessives and "phase" tails are kept, ROADMAP.md file references are NOT
treated as ids, the dry run changes nothing on disk, --write rewrites only
files that mention the roadmap, and skipped files/dirs/suffixes are left
alone. The tree is a synthetic one under a patched ROOT.

Run: python3 -m unittest discover -s tools/checks -p 'test_*.py'
"""

import contextlib
import io
import sys
import tempfile
import unittest
import unittest.mock as mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import rewrite_roadmap_comments as rrc


class Rewrite(unittest.TestCase):
    def test_name_for_id(self):
        self.assertEqual(rrc.name_for_id("PF5"), "SIMD kernels")
        self.assertEqual(rrc.name_for_id("VX18a"), "WASM Playwright coverage")
        self.assertEqual(rrc.name_for_id("PF7b"), "minimum-footprint decoder profile")
        self.assertEqual(rrc.name_for_id("ZZ9"), "legacy item ZZ9")

    def test_bare_ids_keep_their_tails(self):
        self.assertEqual(rrc.replace_bare_roadmap_ids("see roadmap VX11's notes"),
                         "see cross-platform bitstream reproducibility's notes")
        self.assertEqual(rrc.replace_bare_roadmap_ids("ROADMAP IM1 phase 2 work"),
                         "IAB reader phase 2 work")
        self.assertEqual(rrc.replace_bare_roadmap_ids("(roadmap PF6) done"),
                         "(bare-metal probe harness) done")
        self.assertEqual(rrc.replace_bare_roadmap_ids("see ROADMAP.md PF5"),
                         "see ROADMAP.md PF5")

    def test_rewrite_applies_phrase_table_first(self):
        self.assertEqual(rrc.rewrite("// roadmap item F1 wrapper, see ROADMAP.md"),
                         "// C API wrapper")
        self.assertEqual(rrc.rewrite("# ROADMAP PF5 phase 4c kernel"),
                         "# batched MDCT (four blocks) kernel")

    def test_slash_pair_names_both_ids(self):
        """Regression: the dedicated "roadmap X/Y" substitution used to run
        after the case-insensitive bare-id pass, which had already consumed
        "roadmap DC1", leaving "decoder output stage/DC2"."""
        self.assertEqual(rrc.replace_bare_roadmap_ids("roadmap DC1/DC2"),
                         "decoder output stage/decoder concealment")

    def test_parenthesised_see_roadmap_is_removed_whole(self):
        """Regression: in REPLACEMENTS, "see ROADMAP.md)" -> ")" used to run
        before the " (see ROADMAP.md)" -> "" rule, so the latter was
        unreachable and the text was left with an empty "()"."""
        self.assertEqual(rrc.rewrite("// C API wrapper (see ROADMAP.md)"),
                         "// C API wrapper")


class Main(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.files = {
            "src/a.cpp": "// roadmap PF5 kernel\n",
            "src/plain.cpp": "// nothing to see\n",
            "src/mention.cpp": "// ROADMAP.md only\n",
            "tools/build/b.py": "# roadmap PF5\n",            # skipped dir part
            "tools/rewrite_roadmap_comments.py": "# roadmap PF5\n",  # skipped file
            "tests/golden/x.json": '"roadmap PF5"\n',         # suffix not scanned
            "tests/c.bin": "roadmap PF5\n",
            "CMakeLists.txt": "# roadmap item F1\n",
        }
        for rel, text in self.files.items():
            path = self.root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)

    def tearDown(self):
        self._tmp.cleanup()

    def run_main(self, *args):
        buf = io.StringIO()
        with mock.patch.object(rrc, "ROOT", self.root), \
                mock.patch.object(sys, "argv", ["x", *args]), contextlib.redirect_stdout(buf):
            rc = rrc.main()
        return rc, buf.getvalue()

    def test_dry_run_reports_without_writing(self):
        rc, out = self.run_main()
        self.assertEqual(rc, 0)
        self.assertIn("Would change 2 files", out)
        self.assertIn("src/a.cpp", out)
        self.assertIn("CMakeLists.txt", out)
        self.assertEqual((self.root / "src/a.cpp").read_text(), self.files["src/a.cpp"])

    def test_write_rewrites_only_matching_files(self):
        _rc, out = self.run_main("--write")
        self.assertIn("Wrote 2 files", out)
        self.assertEqual((self.root / "src/a.cpp").read_text(), "// SIMD kernels kernel\n")
        self.assertEqual((self.root / "CMakeLists.txt").read_text(), "# C API\n")
        for untouched in ("tools/build/b.py", "tools/rewrite_roadmap_comments.py",
                          "tests/golden/x.json", "tests/c.bin", "src/mention.cpp"):
            self.assertEqual((self.root / untouched).read_text(), self.files[untouched])


if __name__ == "__main__":
    unittest.main()
