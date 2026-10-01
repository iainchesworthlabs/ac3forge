"""Unit tests for check_namespaces.py, the check that a public header declares into its library's
namespace and the root namespace holds no declaration of its own.

stdlib `unittest`, for the reason the script is stdlib-only: this runs in _static.yml's static job.

Each test builds a small temporary tree (no git repository in it, so the script walks src/) with a
table beside it, and runs the check over it, so the cases are the rules the script's header states:
a header in its library's namespace passes; one in another's fails with its name; a block that
declares into `iclforge` itself fails and one that only holds namespaces does not; comments and
strings do not count; a library the table lacks, and a row for a library with no headers, fail; a
known debt is reported and does not fail, and a debt that no longer breaks the rule does.
"""

import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_namespaces as cn


def write(root: Path, rel: str, text: str) -> None:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def header(lib: str, name: str = "x.hpp") -> str:
    return f"src/{lib}/include/iclforge/{lib}/{name}"


class Namespaces(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)

    def run_check(self, libraries: dict, debts: list | None = None) -> tuple[int, str]:
        table = self.root / "namespaces.json"
        table.write_text(
            json.dumps({"libraries": libraries, "debts": debts or []}), encoding="utf-8"
        )
        out = io.StringIO()
        with redirect_stdout(out):
            code = cn.main(["--root", str(self.root), "--table", str(table)])
        return code, out.getvalue()

    def test_a_header_in_its_librarys_namespace_passes(self) -> None:
        write(self.root, header("ac3"), "namespace iclforge::ac3 {\nstruct A {};\n}\n")
        write(self.root, header("ac3", "io/y.hpp"), "namespace iclforge::ac3::io {\nint f();\n}\n")
        code, out = self.run_check({"ac3": ["ac3"]})
        self.assertEqual(code, 0, out)
        self.assertIn("2 public headers in 1 libraries: 0 failures", out)

    def test_a_header_in_another_librarys_namespace_fails_and_names_it(self) -> None:
        write(self.root, header("ac3"), "namespace iclforge::mp4 {\nstruct A {};\n}\n")
        code, out = self.run_check({"ac3": ["ac3"]})
        self.assertEqual(code, 1)
        self.assertIn("src/ac3/include/iclforge/ac3/x.hpp: opens iclforge::mp4", out)

    def test_a_library_may_have_several_namespaces_when_the_table_says_so(self) -> None:
        write(
            self.root,
            header("objects"),
            "namespace iclforge::oba {\n}\nnamespace iclforge::emdf {\n}\n",
        )
        code, out = self.run_check({"objects": ["oba", "emdf"]})
        self.assertEqual(code, 0, out)

    def test_a_declaration_in_the_root_fails(self) -> None:
        write(self.root, header("ac3"), "namespace iclforge {\nstruct A {};\n}\n")
        code, out = self.run_check({"ac3": ["ac3"]})
        self.assertEqual(code, 1)
        self.assertIn("declares into the root namespace iclforge itself", out)

    def test_a_root_block_that_only_holds_namespaces_passes(self) -> None:
        text = (
            "#pragma once\n#include <vector>\n"
            "namespace iclforge {\n"
            "#define X 1\n"
            "namespace ac4 {\nstruct A {};\n}  // namespace ac4\n"
            "}  // namespace iclforge\n"
        )
        write(self.root, header("ac4"), text)
        code, out = self.run_check({"ac4": ["ac4"]})
        self.assertEqual(code, 0, out)

    def test_a_declaration_after_a_nested_namespace_still_fails(self) -> None:
        text = "namespace iclforge {\nnamespace ac4 {\n}\nint stray();\n}\n"
        write(self.root, header("ac4"), text)
        code, _ = self.run_check({"ac4": ["ac4"]})
        self.assertEqual(code, 1)

    def test_a_namespace_nested_two_deep_is_not_a_root_declaration(self) -> None:
        text = "namespace iclforge {\nnamespace ac4 {\nnamespace detail {\nint f();\n}\n}\n}\n"
        write(self.root, header("ac4"), text)
        code, out = self.run_check({"ac4": ["ac4"]})
        self.assertEqual(code, 0, out)

    def test_comments_and_strings_do_not_count(self) -> None:
        text = (
            '// namespace iclforge { int x; }\nconst char* s = "namespace iclforge::mp4 {";\n'
            "/* namespace iclforge::render { */\nnamespace iclforge::ac3 {\n}\n"
        )
        write(self.root, header("ac3"), text)
        code, out = self.run_check({"ac3": ["ac3"]})
        self.assertEqual(code, 0, out)

    def test_a_template_of_a_header_is_read_too(self) -> None:
        write(self.root, header("ac3", "version.hpp.in"), "namespace iclforge {\nint v = @V@;\n}\n")
        code, _ = self.run_check({"ac3": ["ac3"]})
        self.assertEqual(code, 1)

    def test_a_private_header_and_a_source_are_not_public(self) -> None:
        write(self.root, "src/ac3/src/x.hpp", "namespace iclforge {\nint f();\n}\n")
        write(self.root, "src/ac3/src/x.cpp", "namespace iclforge {\nint f() { return 1; }\n}\n")
        write(self.root, header("ac3"), "namespace iclforge::ac3 {\n}\n")
        code, out = self.run_check({"ac3": ["ac3"]})
        self.assertEqual(code, 0, out)

    def test_a_library_the_table_lacks_fails(self) -> None:
        write(self.root, header("ac3"), "namespace iclforge::ac3 {\n}\n")
        write(self.root, header("mp4"), "namespace iclforge::mp4 {\n}\n")
        code, out = self.run_check({"ac3": ["ac3"]})
        self.assertEqual(code, 1)
        self.assertIn("mp4: public headers, and no row of the table", out)

    def test_a_row_for_a_library_without_headers_fails(self) -> None:
        write(self.root, header("ac3"), "namespace iclforge::ac3 {\n}\n")
        code, out = self.run_check({"ac3": ["ac3"], "ghost": ["ghost"]})
        self.assertEqual(code, 1)
        self.assertIn("ghost: a row of the table for a library with no public headers", out)

    def test_a_debt_is_reported_and_does_not_fail(self) -> None:
        write(
            self.root,
            header("base", "bitreader.hpp"),
            "namespace iclforge {\nclass BitReader {};\n}\n",
        )
        debts = [{"file": header("base", "bitreader.hpp"), "reason": "BitReader is the root's"}]
        code, out = self.run_check({"base": ["base"]}, debts)
        self.assertEqual(code, 0, out)
        self.assertIn(
            "known debt: src/base/include/iclforge/base/bitreader.hpp: BitReader is the root's", out
        )
        self.assertIn("0 failures, 1 known debts", out)

    def test_a_debt_that_no_longer_breaks_the_rule_fails(self) -> None:
        write(
            self.root,
            header("base", "bitreader.hpp"),
            "namespace iclforge::base {\nclass BitReader {};\n}\n",
        )
        debts = [{"file": header("base", "bitreader.hpp"), "reason": "BitReader is the root's"}]
        code, out = self.run_check({"base": ["base"]}, debts)
        self.assertEqual(code, 1)
        self.assertIn("no longer breaks the rule: delete its row", out)

    def test_a_debt_for_a_file_that_is_gone_fails(self) -> None:
        write(self.root, header("base"), "namespace iclforge::base {\n}\n")
        debts = [{"file": header("base", "gone.hpp"), "reason": "was the root's"}]
        code, out = self.run_check({"base": ["base"]}, debts)
        self.assertEqual(code, 1)
        self.assertIn("is not a public header of the tree", out)

    def test_the_census_lists_what_each_library_opens_and_what_declares_into_the_root(self) -> None:
        write(self.root, header("ac3"), "namespace iclforge {\nint f();\n}\n")
        write(self.root, header("ac3", "io.hpp"), "namespace iclforge::mp4 {\n}\n")
        out = cn.census(self.root, cn.tracked(self.root))
        self.assertIn("ac3: 2 headers open mp4 (1)", out)
        self.assertIn("declares into the root: src/ac3/include/iclforge/ac3/x.hpp", out)


class CommittedTable(unittest.TestCase):
    """The data the check runs on, held to the tree it is committed in."""

    def test_the_committed_table_passes_on_this_tree(self) -> None:
        root = Path(__file__).resolve().parents[2]
        out = io.StringIO()
        with redirect_stdout(out):
            code = cn.main(["--root", str(root)])
        self.assertEqual(code, 0, out.getvalue())


if __name__ == "__main__":
    unittest.main()
