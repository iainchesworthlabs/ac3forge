"""Unit tests for baseline.py and export_diff.py: the records a stage is proved against.

stdlib `unittest`. What needs a built tree (the CLI corpus, the exports of a DLL) is exercised by
the stage that has one; here are the parts that do not: the public-header record and the check that
a move plan keeps every header, the comparison of two records, and the export comparison that
allows a library to split into several.
"""

import contextlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import baseline
import export_diff


def git(root: Path, *args: str) -> str:
    return subprocess.run(
        ["git", "-C", str(root), *args], capture_output=True, text=True, check=True
    ).stdout


class Headers(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)
        for rel, text in {
            "src/a/include/a/x.hpp": "x\n",
            "src/a/include/a/version.hpp.in": "v\n",
            "src/a/src/private.hpp": "p\n",
            "src/a/src/impl.cpp": "i\n",
            "tests/t.hpp": "t\n",
        }.items():
            path = self.root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        git(self.root, "init", "-q")
        git(self.root, "config", "user.name", "t")
        git(self.root, "config", "user.email", "t@example.invalid")
        git(self.root, "add", "-A")
        git(self.root, "commit", "-q", "-m", "fixture")

    def move_header(self) -> None:
        (self.root / "src/a/include/iclforge/a").mkdir(parents=True)
        git(self.root, "mv", "src/a/include/a/x.hpp", "src/a/include/iclforge/a/x.hpp")

    def plan(self, moves: dict[str, str]) -> Path:
        path = self.root / "plan.json"
        path.write_text(json.dumps({"moves": moves}), encoding="utf-8")
        return path

    def baseline_dir(self) -> Path:
        out = self.root / "baseline"
        out.mkdir()
        record = {"headers": baseline.public_headers(self.root)}
        (out / "headers.json").write_text(json.dumps(record), encoding="utf-8")
        return out

    def check(self, plan: dict[str, str], directory: Path, pure: bool = False) -> tuple[int, str]:
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            code = baseline.check_moves(self.plan(plan), directory, self.root, pure)
        return code, buffer.getvalue()

    def test_only_headers_under_include_are_public_headers(self) -> None:
        self.assertEqual(
            sorted(baseline.public_headers(self.root)),
            ["src/a/include/a/version.hpp.in", "src/a/include/a/x.hpp"],
        )

    def test_headers_are_recorded_from_git_without_a_build(self) -> None:
        made = baseline.record(None, "none", ("headers",), self.root, root=self.root)
        self.assertEqual(
            sorted(made["headers"]["headers"]),
            ["src/a/include/a/version.hpp.in", "src/a/include/a/x.hpp"],
        )
        self.assertEqual(
            made["headers"]["measured_at"], git(self.root, "rev-parse", "HEAD").strip()
        )
        self.assertEqual(made["headers"]["label"], "")

    def test_every_other_kind_needs_a_build(self) -> None:
        for kinds in (("cli",), ("headers", "symbols")):
            with self.subTest(kinds=kinds), self.assertRaises(SystemExit):
                baseline.record(None, "none", kinds, self.root, root=self.root)
        with self.assertRaises(SystemExit):
            baseline.record(None, "none", ("headers",), self.root)

    def test_a_pure_move_keeps_every_header_with_its_bytes(self) -> None:
        directory = self.baseline_dir()
        self.move_header()
        plan = {"src/a/include/a/x.hpp": "src/a/include/iclforge/a/x.hpp"}
        code, out = self.check(plan, directory, pure=True)
        self.assertEqual(code, 0, out)

    def test_a_header_the_plan_sends_nowhere_is_missing(self) -> None:
        directory = self.baseline_dir()
        self.move_header()
        code, out = self.check({}, directory)
        self.assertEqual(code, 1)
        self.assertIn("missing: src/a/include/a/x.hpp", out)

    def test_an_edited_header_fails_only_a_pure_check(self) -> None:
        directory = self.baseline_dir()
        self.move_header()
        (self.root / "src/a/include/iclforge/a/x.hpp").write_text("edited\n", encoding="utf-8")
        git(self.root, "add", "-A")
        plan = {"src/a/include/a/x.hpp": "src/a/include/iclforge/a/x.hpp"}
        self.assertEqual(self.check(plan, directory, pure=False)[0], 0)
        code, out = self.check(plan, directory, pure=True)
        self.assertEqual(code, 1)
        self.assertIn("changed: src/a/include/a/x.hpp", out)


class Records(unittest.TestCase):
    def test_measured_at_and_tools_are_not_compared(self) -> None:
        a = {"kind": "cli", "measured_at": "abc", "commands": {"x": {"rc": 0}}}
        b = {"kind": "cli", "measured_at": "def", "commands": {"x": {"rc": 0}}}
        self.assertEqual(baseline.differences(a, b), [])

    def test_a_changed_key_a_missing_key_and_a_changed_list_are_all_reported(self) -> None:
        a = {"commands": {"x": {"rc": 0}, "y": {"rc": 0}}, "names": ["p", "q"]}
        b = {"commands": {"x": {"rc": 1}, "z": {"rc": 0}}, "names": ["p", "r"]}
        found = baseline.differences(a, b)
        self.assertIn("~ /commands/x/rc: 0 -> 1", found)
        self.assertIn("- /commands/y", found)
        self.assertIn("+ /commands/z", found)
        self.assertTrue(any(line.startswith("~ /names: 1 removed, 1 added") for line in found))

    def test_file_names_carry_the_label_except_the_headers(self) -> None:
        self.assertEqual(baseline.file_name("headers", "msvc"), "headers.json")
        self.assertEqual(baseline.file_name("cli", "msvc"), "cli-msvc.json")

    def test_an_unknown_kind_is_refused(self) -> None:
        with self.assertRaises(SystemExit):
            baseline.parse_kinds("headers,nonsense")


class Exports(unittest.TestCase):
    def compare(
        self, old: dict, new: dict, mapping: str, kinds: set[str], copies: bool = False
    ) -> int:
        with contextlib.redirect_stdout(io.StringIO()):
            return export_diff.compare(
                {"libraries": old}, {"libraries": new}, mapping, kinds, 5, copies
            )

    def test_the_same_libraries_with_the_same_names_agree(self) -> None:
        libs = {"a.dll": ["ac3::f()", "ac3::g()"]}
        self.assertEqual(self.compare(libs, libs, "identity", set()), 0)

    def test_a_name_gone_or_added_is_a_difference(self) -> None:
        old = {"a.dll": ["ac3::f()"]}
        self.assertEqual(
            self.compare(old, {"a.dll": ["ac3::f()", "ac3::h()"]}, "identity", set()), 1
        )
        self.assertEqual(self.compare(old, {"a.dll": []}, "identity", set()), 1)

    def test_the_cuts_rename_the_types_they_moved(self) -> None:
        old = {"a.dll": ["ac3::f(ac3::eac3::chanmap::Layout const&, ac3::BlockSink)"]}
        new = {"a.dll": ["ac3::f(ac3::base::Layout const&, ac3::render::BlockSink)"]}
        self.assertEqual(self.compare(old, new, "identity", set()), 1)
        self.assertEqual(self.compare(old, new, "identity", {"cuts"}), 0)

    def test_a_split_library_is_the_union_of_its_parts(self) -> None:
        old = {"ac3forge.dll": ["ac3::f()", "ac3::g()"], "mp4.dll": ["mp4::m()"]}
        new = {
            "iclforge_ac3.dll": ["ac3::f()"],
            "iclforge_base.dll": ["ac3::g()"],
            "iclforge_mp4.dll": ["mp4::m()"],
        }
        self.assertEqual(self.compare(old, new, "l2", set()), 0)
        new["iclforge_dsp.dll"] = ["ac3::extra()"]
        self.assertEqual(self.compare(old, new, "l2", set()), 1)

    def test_the_names_rewrite_moves_the_namespace_root(self) -> None:
        old = {"a.dll": ["ac3::f()", "mp4::m()"]}
        new = {"a.dll": ["iclforge::f()", "iclforge::mp4::m()"]}
        self.assertEqual(self.compare(old, new, "identity", {"names"}), 0)

    def test_the_idents_rewrite_moves_the_brand_in_a_name(self) -> None:
        old = {"c.dll": ["ac3forge_encoder_create", "ac3forge_c::hidden()", "AC3FORGE_x"]}
        new = {"c.dll": ["iclforge_encoder_create", "iclforge_c::hidden()", "ICLFORGE_x"]}
        self.assertEqual(self.compare(old, new, "identity", set()), 1)
        self.assertEqual(self.compare(old, new, "identity", {"idents"}), 0)

    def test_names_and_idents_combine_and_a_real_change_still_shows(self) -> None:
        old = {"a.dll": ["ac3::f()", "ac3forge_g"]}
        new = {"a.dll": ["iclforge::f()", "iclforge_g"]}
        self.assertEqual(self.compare(old, new, "identity", {"names", "idents"}), 0)
        new["a.dll"].append("iclforge_h")
        self.assertEqual(self.compare(old, new, "identity", {"names", "idents"}), 1)

    def test_a_library_that_stops_re_exporting_a_copy_passes_only_with_copies(self) -> None:
        # admbridge.dll links the codec statically and carried 278 of its names; a change to what
        # its headers include changed which members it pulls in.
        old = {"core.dll": ["ac3::f()", "ac3::g()"], "bridge.dll": ["ac3::f()", "ac3::own()"]}
        new = {"core.dll": ["ac3::f()", "ac3::g()"], "bridge.dll": ["ac3::own()"]}
        self.assertEqual(self.compare(old, new, "identity", set()), 1)
        self.assertEqual(self.compare(old, new, "identity", set(), copies=True), 0)

    def test_copies_do_not_excuse_a_name_no_library_exports_any_more(self) -> None:
        old = {"core.dll": ["ac3::f()", "ac3::g()"], "bridge.dll": ["ac3::f()"]}
        new = {"core.dll": ["ac3::f()"], "bridge.dll": []}
        self.assertEqual(self.compare(old, new, "identity", set(), copies=True), 1)

    def test_copies_do_not_excuse_a_name_a_library_gained(self) -> None:
        old = {"core.dll": ["ac3::f()"], "bridge.dll": ["ac3::f()"]}
        new = {"core.dll": ["ac3::f()"], "bridge.dll": ["ac3::f()", "ac3::new()"]}
        self.assertEqual(self.compare(old, new, "identity", set(), copies=True), 1)

    def test_a_name_that_moves_between_libraries_is_still_a_difference(self) -> None:
        old = {"a.dll": ["ac3::f()"], "b.dll": []}
        new = {"a.dll": [], "b.dll": ["ac3::f()"]}
        self.assertEqual(self.compare(old, new, "identity", set()), 1)
        self.assertEqual(self.compare(old, new, "identity", set(), copies=True), 1)


if __name__ == "__main__":
    unittest.main()
