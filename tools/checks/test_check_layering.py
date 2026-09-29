"""Unit tests for check_layering.py, the dependency check over the libraries under src/.

stdlib `unittest`, for the reason the script is stdlib-only: this runs in _static.yml's static job.

Each test builds a small temporary tree (there is no git repository in it, so the script lists
src/ by walking it) with a table beside it, and runs the check over it, so the cases are the rules
the script's header states: an allowed include passes; a forbidden one fails with the file and the
line; a private header is found by the tail of its path and a quoted include by its own directory;
a system header is not the project's; a library the table lacks, a table that has a cycle and a
library nothing is filed under fail; a known debt is reported and does not fail, and a debt that
is no longer there does; and the transitional split and rename place a file under a library.
"""

import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_layering


def _write(root: Path, relative: str, text: str) -> Path:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


class LayeringCheck(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)
        _write(self.root, "src/base/include/x/base/bits.hpp", "#pragma once\n")
        _write(
            self.root,
            "src/codec/src/decode.cpp",
            '#include <vector>\n#include "x/base/bits.hpp"\nint main() { return 0; }\n',
        )

    def table(self, libraries: dict, layout: dict | None = None) -> Path:
        body: dict = {"libraries": libraries}
        if layout is not None:
            body["layout"] = layout
        return _write(self.root, "layering.json", json.dumps(body))

    def run_check(self, libraries: dict, layout: dict | None = None, debt: str | None = None):
        argv = ["--root", str(self.root), "--table", str(self.table(libraries, layout))]
        argv += ["--debt", str(self.root / (debt or "no-debt"))]
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            code = check_layering.main(argv)
        return code, buffer.getvalue()

    def test_allowed_include_passes(self) -> None:
        code, out = self.run_check({"base": [], "codec": ["base"]})
        self.assertEqual(code, 0, out)
        self.assertIn("1 include edges", out)

    def test_forbidden_include_fails_and_names_file_and_line(self) -> None:
        code, out = self.run_check({"base": [], "codec": []})
        self.assertEqual(code, 1)
        self.assertIn("file=src/codec/src/decode.cpp,line=2", out)
        self.assertIn("codec may not include base", out)

    def test_system_and_unresolved_includes_are_not_edges(self) -> None:
        _write(self.root, "src/codec/src/other.cpp", '#include <string>\n#include "fmt/core.h"\n')
        code, out = self.run_check({"base": [], "codec": ["base"]})
        self.assertEqual(code, 0, out)
        self.assertIn("1 include edges", out)

    def test_private_header_is_found_by_the_tail_of_its_path(self) -> None:
        _write(self.root, "src/base/src/detail/tables.hpp", "#pragma once\n")
        _write(self.root, "src/codec/src/use.cpp", '#include "detail/tables.hpp"\n')
        code, out = self.run_check({"base": [], "codec": []})
        self.assertEqual(code, 1)
        self.assertIn("src/codec/src/use.cpp", out)

    def test_quoted_include_beside_the_file_stays_in_its_library(self) -> None:
        _write(self.root, "src/codec/src/helper.hpp", "#pragma once\n")
        _write(self.root, "src/codec/src/use.cpp", '#include "helper.hpp"\n')
        code, out = self.run_check({"base": [], "codec": ["base"]})
        self.assertEqual(code, 0, out)

    def test_a_library_the_table_lacks_fails(self) -> None:
        code, out = self.run_check({"base": []})
        self.assertEqual(code, 1)
        self.assertIn("holds files of codec, which the table lacks", out)

    def test_a_library_with_no_files_fails(self) -> None:
        code, out = self.run_check({"base": [], "codec": ["base"], "ghost": []})
        self.assertEqual(code, 1)
        self.assertIn("nothing under src/ is filed under it", out)

    def test_a_cycle_in_the_table_fails(self) -> None:
        code, out = self.run_check({"base": ["codec"], "codec": ["base"]})
        self.assertEqual(code, 1)
        self.assertIn("cycle in the table: base -> codec", out)

    def test_a_row_naming_an_unknown_library_fails(self) -> None:
        code, out = self.run_check({"base": [], "codec": ["base", "nowhere"]})
        self.assertEqual(code, 1)
        self.assertIn("nowhere, which is not a library of the table", out)

    def test_known_debt_is_reported_and_does_not_fail(self) -> None:
        _write(
            self.root,
            "debt/c1.txt",
            "# a cut\n\nsrc/codec/src/decode.cpp x/base/bits.hpp  # codec -> base\n",
        )
        code, out = self.run_check({"base": [], "codec": []}, debt="debt")
        self.assertEqual(code, 0, out)
        self.assertIn("1 known debts", out)

    def test_debt_that_is_no_longer_forbidden_fails(self) -> None:
        _write(self.root, "debt/c1.txt", "src/codec/src/decode.cpp x/base/bits.hpp\n")
        code, out = self.run_check({"base": [], "codec": ["base"]}, debt="debt")
        self.assertEqual(code, 1)
        self.assertIn("c1.txt lists src/codec/src/decode.cpp including x/base/bits.hpp", out)

    def test_debt_for_a_file_that_is_gone_fails(self) -> None:
        _write(self.root, "debt/c2.txt", "src/codec/src/gone.cpp x/base/bits.hpp\n")
        code, out = self.run_check({"base": [], "codec": []}, debt="debt")
        self.assertEqual(code, 1)
        self.assertIn("c2.txt", out)

    def test_a_new_forbidden_include_is_not_covered_by_a_debt_for_another(self) -> None:
        _write(self.root, "debt/c1.txt", "src/codec/src/decode.cpp x/base/bits.hpp\n")
        _write(self.root, "src/codec/src/new.cpp", '#include "x/base/bits.hpp"\n')
        code, out = self.run_check({"base": [], "codec": []}, debt="debt")
        self.assertEqual(code, 1)
        self.assertIn("src/codec/src/new.cpp", out)

    def test_malformed_debt_line_is_an_error(self) -> None:
        _write(self.root, "debt/c1.txt", "only-one-field\n")
        with self.assertRaises(ValueError):
            self.run_check({"base": [], "codec": []}, debt="debt")

    def test_split_and_rename_file_a_path_under_a_library(self) -> None:
        _write(self.root, "src/big/include/x/big/parts.hpp", "#pragma once\n")
        _write(self.root, "src/big/include/x/big/core.hpp", '#include "x/big/parts.hpp"\n')
        _write(self.root, "src/old/include/x/old/o.hpp", '#include "x/big/core.hpp"\n')
        layout = {
            "rename": {"old": "new"},
            "split": [["^src/big/include/x/big/core\\.hpp$", "core"], ["^src/big/", "rest"]],
        }
        libraries = {"base": [], "codec": ["base"], "core": ["rest"], "rest": [], "new": ["core"]}
        code, out = self.run_check(libraries, layout)
        self.assertEqual(code, 0, out)
        forbidden = {**libraries, "core": []}
        code, out = self.run_check(forbidden, layout)
        self.assertEqual(code, 1)
        self.assertIn("core may not include rest", out)

    def test_edges_flag_prints_the_edges_and_stops(self) -> None:
        argv = ["--root", str(self.root), "--table", str(self.table({"base": [], "codec": []}))]
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            code = check_layering.main([*argv, "--edges"])
        self.assertEqual(code, 0)
        self.assertEqual(buffer.getvalue().strip(), "codec -> base: 1")

    def test_an_empty_src_tree_fails_rather_than_passing_vacuously(self) -> None:
        with tempfile.TemporaryDirectory() as empty:
            table = _write(Path(empty), "layering.json", json.dumps({"libraries": {"base": []}}))
            buffer = io.StringIO()
            with redirect_stdout(buffer):
                code = check_layering.main(["--root", empty, "--table", str(table)])
            self.assertEqual(code, 1)
            self.assertIn("nothing was checked", buffer.getvalue())


class RealTable(unittest.TestCase):
    """The committed table is a table the check accepts, whatever the tree holds."""

    def test_table_is_loadable_and_acyclic(self) -> None:
        table = check_layering.load_table(check_layering.DEFAULT_TABLE)
        self.assertFalse(check_layering.cycles(table.libraries))
        for library, uses in table.libraries.items():
            self.assertNotIn(library, uses)
            for used in uses:
                self.assertIn(used, table.libraries, f"{library} uses {used}")


if __name__ == "__main__":
    unittest.main()
