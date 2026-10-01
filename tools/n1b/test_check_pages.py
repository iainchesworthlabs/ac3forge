"""Unit tests for check_pages.py: the headers and targets the pages name, held to a tree."""

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_pages as C
from n1b_lib import DEFAULT_ROOT

FILES = {
    "tools/checks/layering.json": json.dumps(
        {"libraries": {"base": [], "dsp": ["base"], "capi": ["base"]}}
    ),
    "src/base/include/iclforge/base/layout.hpp": "// a header\n",
    "src/dsp/include/iclforge/dsp/fft.hpp": "// a header\n",
    "src/dsp/variants/arch-x/iclforge/dsp/detail/kernel.hpp": "// a variant\n",
    "src/capi/include/iclforge_c/iclforge.h": "/* the C API */\n",
    "src/dsp/CMakeLists.txt": "add_library(iclforge::dsp_static ALIAS iclforge_dsp_static)\n",
    "src/base/CMakeLists.txt": "iclforge_add_library(base)\n",
    "src/capi/CMakeLists.txt": "add_library(iclforge::c ALIAS iclforge_capi)\n",
    "docs/library/index.md": "`iclforge::base`, `iclforge::dsp`, `iclforge::c`\n",
}


class Check(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        for rel, text in FILES.items():
            path = self.root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")

    def commit(self) -> list[str]:
        subprocess.run(["git", "-C", str(self.root), "init", "-q"], check=True)
        subprocess.run(["git", "-C", str(self.root), "add", "-A"], check=True)
        return C.check(self.root)

    def page(self, text: str, rel: str = "docs/library/page.md") -> None:
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def test_a_tree_whose_pages_are_right_has_no_problem(self) -> None:
        self.page(
            "`iclforge/base/layout.hpp`, `iclforge/dsp/detail/kernel.hpp`, "
            "`iclforge_c/iclforge.h`, `iclforge/dsp/export.hpp`, `iclforge::dsp_static`, "
            "`iclforge::c` and `iclforge::mlp`\n"
        )
        self.assertEqual(self.commit(), [])

    def test_a_header_that_is_not_there_is_named(self) -> None:
        self.page("see `iclforge/base/missing.hpp` and `ac3/core/layout.hpp`\n")
        self.assertEqual(
            self.commit(), ["docs/library/page.md:1: no such header: iclforge/base/missing.hpp"]
        )

    def test_a_target_that_no_cmake_file_makes_is_named(self) -> None:
        self.page(
            "`iclforge::dsp_shared` and `iclforge::dsp_static` and `iclforge::split_frames`\n"
        )
        self.assertEqual(
            self.commit(), ["docs/library/page.md:1: no CMake target makes iclforge::dsp_shared"]
        )

    def test_a_library_the_index_does_not_name_is_reported(self) -> None:
        (self.root / "docs/library/index.md").write_text("`iclforge::base`\n", encoding="utf-8")
        self.assertEqual(
            self.commit(),
            [
                "docs/library/index.md: the library capi is not named (iclforge::c)",
                "docs/library/index.md: the library dsp is not named (iclforge::dsp)",
            ],
        )

    def test_the_history_is_not_read(self) -> None:
        self.page("`iclforge/base/missing.hpp`\n", "planning/old.md")
        self.assertEqual(self.commit(), [])

    def test_this_tree_is_clean(self) -> None:
        self.assertEqual(C.check(Path(DEFAULT_ROOT)), [])


if __name__ == "__main__":
    unittest.main()
