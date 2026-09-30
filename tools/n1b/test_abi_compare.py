"""Unit tests for abi_compare.py: the old exported-symbol allowlists against the ones S2 writes.

stdlib `unittest`, on small allowlist directories written to a temporary place: a library that only
changed its file name is no change, the six libraries that replace libac3forge.so together export
what it did, and a name lost, gained or exported by two of the six is listed.
"""

import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import abi_compare

SIX = [
    "libiclforge_ac3.so",
    "libiclforge_base.so",
    "libiclforge_dsp.so",
    "libiclforge_objects.so",
    "libiclforge_render.so",
    "libiclforge_iec61937.so",
]


class Compare(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.old = Path(self._tmp.name) / "old"
        self.new = Path(self._tmp.name) / "new"
        self.old.mkdir()
        self.new.mkdir()

    @staticmethod
    def write(directory: Path, library: str, names: list[str]) -> None:
        text = "".join(f"{name}\n" for name in names)
        (directory / f"{library}.txt").write_text(text, encoding="utf-8")

    def run_compare(self) -> tuple[int, str]:
        out = io.StringIO()
        return abi_compare.compare(self.old, self.new, out), out.getvalue()

    def test_a_library_that_only_changed_its_file_name_is_no_change(self) -> None:
        self.write(self.old, "libmp4.so", ["mp4::write()", "mp4::Muxer::finish()"])
        self.write(self.new, "libiclforge_mp4.so", ["mp4::write()", "mp4::Muxer::finish()"])
        changed, text = self.run_compare()
        self.assertEqual(changed, 0)
        self.assertIn("libmp4.so (2) <- 1 library (2: mp4 2): -0 +0", text)

    def test_the_six_libraries_that_replace_libac3forge_export_what_it_did(self) -> None:
        names = [f"ac3::f{i}()" for i in range(6)]
        self.write(self.old, "libac3forge.so", names)
        for library, name in zip(SIX, names, strict=True):
            self.write(self.new, library, [name])
        changed, text = self.run_compare()
        self.assertEqual(changed, 0)
        self.assertIn("libac3forge.so (6) <- 6 libraries (6:", text)

    def test_a_name_lost_and_one_gained_are_listed_and_counted(self) -> None:
        self.write(self.old, "libac3forge.so", ["ac3::a()", "ac3::b()"])
        self.write(self.new, "libiclforge_ac3.so", ["ac3::a()"])
        self.write(self.new, "libiclforge_base.so", ["ac3::internal::cpu::has_avx2()"])
        changed, text = self.run_compare()
        self.assertEqual(changed, 1)
        self.assertIn("only in old: ac3::b()", text)
        self.assertIn("only in new: ac3::internal::cpu::has_avx2()", text)

    def test_a_name_that_two_of_the_six_export_is_reported(self) -> None:
        self.write(self.old, "libac3forge.so", ["ac3::a()"])
        self.write(self.new, "libiclforge_ac3.so", ["ac3::a()"])
        self.write(self.new, "libiclforge_dsp.so", ["ac3::a()"])
        _, text = self.run_compare()
        self.assertIn("exported by more than one of them: 1", text)

    def test_a_library_missing_from_the_new_directory_lost_all_its_names(self) -> None:
        self.write(self.old, "libac4.so", ["ac4::a()", "ac4::b()"])
        changed, text = self.run_compare()
        self.assertEqual(changed, 1)
        self.assertIn("libac4.so (2) <- 1 library (0: ac4 0): -2 +0", text)

    def run_identity(self, kinds: set[str]) -> tuple[int, str]:
        out = io.StringIO()
        mapping = abi_compare.identity(self.old)
        return abi_compare.compare(self.old, self.new, out, mapping, kinds), out.getvalue()

    def test_a_namespace_rename_is_no_change_once_the_old_names_are_rewritten(self) -> None:
        self.write(self.old, "libiclforge_mp4.so", ["mp4::write()", "ac3::a(mp4::X)"])
        renamed = ["iclforge::mp4::write()", "iclforge::a(iclforge::mp4::X)"]
        self.write(self.new, "libiclforge_mp4.so", renamed)
        changed, text = self.run_identity({"names"})
        self.assertEqual(changed, 0)
        self.assertIn("libiclforge_mp4.so (2) <- 1 library (2: mp4 2): -0 +0", text)

    def test_the_same_rename_is_listed_without_the_rewrite(self) -> None:
        self.write(self.old, "libiclforge_mp4.so", ["mp4::write()"])
        self.write(self.new, "libiclforge_mp4.so", ["iclforge::mp4::write()"])
        changed, text = self.run_identity(set())
        self.assertEqual(changed, 1)
        self.assertIn("only in old: mp4::write()", text)
        self.assertIn("only in new: iclforge::mp4::write()", text)


if __name__ == "__main__":
    unittest.main()
