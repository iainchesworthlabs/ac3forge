"""Unit tests for ac3ns_fix.py: the compiler loop of stage S6.

stdlib `unittest`. The diagnostics are lines written out here in the forms Clang (as clang-cl and as
clang++), GCC and MSVC print them; the sources are small files written to a temporary tree. What
must be left alone is as much of the test as what must be written: an error that follows from an
earlier one, a namespace alias, a name some other library declares, a file that is not the tree's.
"""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ac3ns_core as core
import ac3ns_fix as fix

TABLE = core.Table.from_json(
    {
        "namespaces": {
            "<root>": {
                "owner": "shared",
                "names": ["FrameEncoder", "Acmod"],
                "others": {"src/base": ["BitReader"], "src/mp4": ["mp4"]},
            },
            "meta": {"owner": "exclusive", "names": ["QcPreset"]},
            "eac3": {"owner": "exclusive", "names": ["AccessUnitEncoder"]},
            "eac3::chanmap": {"owner": "exclusive", "names": ["Location"]},
            "emdf": {
                "owner": "shared",
                "names": ["FrameLayout", "walk_frame"],
                "others": {"src/objects": ["Payload"]},
            },
            "oba": {
                "owner": "shared",
                "names": ["AtmosEncoder"],
                "others": {"src/objects": ["Position", "bed_labels"]},
            },
            "oba::joc": {
                "owner": "shared",
                "names": ["reconstruct"],
                "others": {"src/objects": ["Domain"]},
            },
            "render": {
                "owner": "shared",
                "names": ["serve"],
                "others": {"src/render": ["OutputLayout"]},
            },
            "internal": {
                "owner": "shared",
                "names": ["pow43", "store_norm"],
                "others": {"src/arithmetic": ["Fixed32", "arch"]},
            },
        },
    }
)


def diag(kind: str, name: str, scope: str = "", line: int = 1, col: int | None = 1) -> fix.Diag:
    return fix.Diag("x.cpp", line, col, kind, name, scope)


class Parsing(unittest.TestCase):
    def test_clang_in_the_msvc_format(self) -> None:
        log = (
            "D:\\w\\src\\a.cpp(12,34): error: use of undeclared identifier 'Fixed32'\n"
            "D:\\w\\src\\a.cpp(13,5): error: unknown type name 'Fixed32'\n"
            "D:\\w\\src\\a.cpp(14,9): error: no member named 'arch' in namespace "
            "'iclforge::ac3::internal'\n"
            "D:\\w\\src\\a.cpp(15,9): error: no type named 'Fixed32' in namespace "
            "'iclforge::ac3::internal'\n"
            "D:\\w\\src\\a.cpp(16,9): error: no template named 'FftTables' in namespace "
            "'iclforge::ac3::internal'\n"
            "D:\\w\\src\\a.cpp(17,9): error: no template named 'Box'\n"
        )
        got = fix.parse_log(log)
        self.assertEqual(
            [(d.line, d.col, d.kind, d.name, d.scope) for d in got],
            [
                (12, 34, "undeclared", "Fixed32", ""),
                (13, 5, "undeclared", "Fixed32", ""),
                (14, 9, "member", "arch", "iclforge::ac3::internal"),
                (15, 9, "member", "Fixed32", "iclforge::ac3::internal"),
                (16, 9, "member", "FftTables", "iclforge::ac3::internal"),
                (17, 9, "undeclared", "Box", ""),
            ],
        )
        self.assertEqual(got[0].file, "D:\\w\\src\\a.cpp")

    def test_clang_in_the_gnu_format(self) -> None:
        log = "/home/dev/src/a.cpp:12:34: error: use of undeclared identifier 'Acmod'\n"
        (d,) = fix.parse_log(log)
        self.assertEqual((d.file, d.line, d.col, d.name), ("/home/dev/src/a.cpp", 12, 34, "Acmod"))

    def test_gcc(self) -> None:
        log = (
            "/r/a.cpp:3:5: error: 'Acmod' was not declared in this scope\n"
            "/r/a.cpp:4:5: error: 'Acmod' does not name a type\n"
            "/r/a.cpp:5:5: error: 'meta' has not been declared\n"
            "/r/a.cpp:6:5: error: 'AtmosEncoder' is not a member of 'iclforge::oba'\n"
            "/r/a.cpp:7:5: error: 'Fixed32' in namespace 'iclforge::ac3::internal' does not name a "
            "type\n"
            "/r/a.cpp:8:5: error: \u2018Acmod\u2019 was not declared in this scope\n"
        )
        got = [(d.line, d.kind, d.name, d.scope) for d in fix.parse_log(log)]
        self.assertEqual(
            got,
            [
                (3, "undeclared", "Acmod", ""),
                (4, "undeclared", "Acmod", ""),
                (5, "undeclared", "meta", ""),
                (6, "member", "AtmosEncoder", "iclforge::oba"),
                (7, "member", "Fixed32", "iclforge::ac3::internal"),
                (8, "undeclared", "Acmod", ""),
            ],
        )

    def test_msvc_with_and_without_a_column(self) -> None:
        log = (
            "a.cpp(12,5): error C2065: 'Acmod': undeclared identifier\n"
            "a.cpp(13): error C2039: 'AtmosEncoder': is not a member of 'iclforge::oba'\n"
            "a.cpp(14): error C2653: 'meta': is not a class or namespace name\n"
            "a.cpp(15): error C3083: 'oba': the symbol to the left of a '::' must be a type\n"
        )
        got = [(d.line, d.col, d.kind, d.name, d.scope) for d in fix.parse_log(log)]
        self.assertEqual(
            got,
            [
                (12, 5, "undeclared", "Acmod", ""),
                (13, None, "member", "AtmosEncoder", "iclforge::oba"),
                (14, None, "undeclared", "meta", ""),
                (15, None, "undeclared", "oba", ""),
            ],
        )

    def test_warnings_notes_and_other_errors_are_not_diagnostics_it_acts_on(self) -> None:
        log = (
            "a.cpp(1,1): warning: use of undeclared identifier 'X'\n"
            "a.cpp(2,1): note: declared here\n"
            "a.cpp(3,1): error: expected ';' at end of declaration\n"
            "[12/90] Building CXX object a.obj\n"
        )
        self.assertEqual(fix.parse_log(log), [])

    def test_the_same_diagnostic_from_two_units_is_one(self) -> None:
        line = "a.hpp(3,5): error: use of undeclared identifier 'Acmod'\n"
        self.assertEqual(len(fix.parse_log(line * 3)), 1)


class Tree(unittest.TestCase):
    """Sources in a temporary tree and a loop over it."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)

    def write(self, rel: str, text: str) -> None:
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(text.encode("utf-8"))

    def propose(self, rel: str, d: fix.Diag) -> fix.Edit | fix.Left | None:
        d = fix.Diag(str(self.root / rel), d.line, d.col, d.kind, d.name, d.scope)
        return fix.Loop(self.root, TABLE).propose(d)

    def col(self, rel: str, line: int, needle: str, nth: int = 1) -> int:
        row = (self.root / rel).read_text(encoding="utf-8").split("\n")[line - 1]
        at = -1
        for _ in range(nth):
            at = row.index(needle, at + 1)
        return len(row[:at].encode("utf-8")) + 1

    def edit(self, rel: str, line: int, needle: str, d: fix.Diag, nth: int = 1) -> fix.Edit:
        d = fix.Diag(d.file, line, self.col(rel, line, needle, nth), d.kind, d.name, d.scope)
        r = self.propose(rel, d)
        self.assertIsInstance(r, fix.Edit, r)
        assert isinstance(r, fix.Edit)
        return r

    def applied(self, rel: str, e: fix.Edit) -> str:
        text = (self.root / rel).read_bytes().decode("utf-8")
        return text[: e.offset] + e.text + text[e.offset :]


class Undeclared(Tree):
    def test_a_name_of_the_root_used_inside_another_library_is_found_through_iclforge(self) -> None:
        self.write("a.cpp", "namespace iclforge::mp4 {\nvoid f() { FrameEncoder e; }\n}\n")
        e = self.edit("a.cpp", 2, "FrameEncoder", diag("undeclared", "FrameEncoder"))
        self.assertEqual(
            self.applied("a.cpp", e).split("\n")[1], "void f() { ac3::FrameEncoder e; }"
        )

    def test_the_head_of_an_exclusive_namespace(self) -> None:
        self.write("a.cpp", "namespace iclforge::hearth {\nauto q = meta::QcPreset{};\n}\n")
        e = self.edit("a.cpp", 2, "meta", diag("undeclared", "meta"))
        self.assertEqual(e.text, "ac3::")

    def test_the_head_of_a_shared_namespace_is_decided_by_the_name_that_follows(self) -> None:
        self.write("a.cpp", "namespace iclforge::signing {\nauto a = emdf::walk_frame(f);\n}\n")
        e = self.edit("a.cpp", 2, "emdf", diag("undeclared", "emdf"))
        self.assertEqual(e.text, "ac3::")
        self.write("b.cpp", "namespace iclforge::signing {\nauto a = emdf::Payload{};\n}\n")
        self.assertIsInstance(
            self.propose("b.cpp", diag("undeclared", "emdf", col=10, line=2)), fix.Left
        )

    def test_a_name_used_unqualified_inside_the_namespace_it_was_declared_in(self) -> None:
        # objects' own code, in iclforge::oba, naming the library's AtmosEncoder unqualified
        self.write("a.cpp", "namespace iclforge::oba {\nAtmosEncoder make();\n}\n")
        e = self.edit("a.cpp", 2, "AtmosEncoder", diag("undeclared", "AtmosEncoder"))
        self.assertEqual(e.text, "ac3::oba::")

    def test_inside_the_library_another_librarys_name_in_a_shared_namespace_is_anchored(
        self,
    ) -> None:
        self.write("a.cpp", "namespace iclforge::ac3::internal {\nFixed32 x;\n}\n")
        e = self.edit("a.cpp", 2, "Fixed32", diag("undeclared", "Fixed32"))
        self.assertEqual(e.text, "iclforge::internal::")
        self.assertEqual(e.why, "anchor")

    def test_inside_the_library_in_a_nested_namespace_the_enclosing_one_is_named(self) -> None:
        self.write(
            "a.cpp", "namespace iclforge::ac3::internal {\nnamespace avx2 {\nFixed32 x;\n}\n}\n"
        )
        e = self.edit("a.cpp", 3, "Fixed32", diag("undeclared", "Fixed32"))
        self.assertEqual(e.text, "iclforge::internal::")

    def test_inside_the_library_a_name_no_other_library_declares_is_left(self) -> None:
        self.write("a.cpp", "namespace iclforge::ac3::internal {\nMystery x;\n}\n")
        r = self.propose("a.cpp", diag("undeclared", "Mystery", line=2, col=1))
        self.assertIsInstance(r, fix.Left)

    def test_a_using_directive_gets_its_partner(self) -> None:
        self.write(
            "a.cpp",
            "using namespace iclforge;\nvoid f() {\n"
            "    using namespace iclforge;\n    FrameEncoder e;\n}\n",
        )
        e = self.edit("a.cpp", 4, "FrameEncoder", diag("undeclared", "FrameEncoder"))
        self.assertEqual(
            self.applied("a.cpp", e),
            "using namespace iclforge;\nvoid f() {\n    using namespace iclforge;\n"
            "    using namespace iclforge::ac3;\n    FrameEncoder e;\n}\n",
        )

    def test_a_using_directive_of_a_shared_namespace_gets_its_partner(self) -> None:
        self.write(
            "a.cpp", "using namespace iclforge::internal;\nvoid f() { auto x = pow43(2); }\n"
        )
        e = self.edit("a.cpp", 2, "pow43", diag("undeclared", "pow43"))
        self.assertEqual(e.text, "\nusing namespace iclforge::ac3::internal;")

    def test_a_crlf_file_gets_a_crlf_line(self) -> None:
        self.write("a.cpp", "using namespace iclforge;\r\nvoid f() { FrameEncoder e; }\r\n")
        e = self.edit("a.cpp", 2, "FrameEncoder", diag("undeclared", "FrameEncoder"))
        self.assertEqual(
            self.applied("a.cpp", e),
            "using namespace iclforge;\r\nusing namespace iclforge::ac3;\r\n"
            "void f() { FrameEncoder e; }\r\n",
        )

    def test_a_directive_whose_scope_has_ended_does_not_reach(self) -> None:
        self.write(
            "a.cpp",
            "void f() {\n    using namespace iclforge;\n}\nvoid g() { FrameEncoder e; }\n",
        )
        r = self.propose("a.cpp", diag("undeclared", "FrameEncoder", line=4, col=12))
        self.assertIsInstance(r, fix.Left)

    def test_a_qualified_name_the_compiler_took_for_undeclared_is_left(self) -> None:
        self.write("a.cpp", "namespace iclforge::mp4 {\nauto x = foo::FrameEncoder{};\n}\n")
        r = self.propose("a.cpp", diag("undeclared", "FrameEncoder", line=2, col=15))
        self.assertIsInstance(r, fix.Left)

    def test_a_namespace_alias_is_left_for_a_person(self) -> None:
        self.write(
            "a.cpp",
            "namespace bn = iclforge::internal;\n"
            "namespace iclforge::test {\nauto x = bn::pow43(2);\n}\n",
        )
        r = self.propose("a.cpp", diag("undeclared", "bn", line=3, col=10))
        self.assertIsInstance(r, fix.Left)

    def test_outside_the_tree_is_not_ours(self) -> None:
        d = fix.Diag("C:/Program Files/MSVC/include/optional", 290, 11, "undeclared", "X")
        self.assertIsNone(fix.Loop(self.root, TABLE).propose(d))

    def test_the_name_is_found_on_its_line_when_the_compiler_prints_no_column(self) -> None:
        self.write("a.cpp", "namespace iclforge::mp4 {\nvoid f() { FrameEncoder e; }\n}\n")
        d = fix.Diag(str(self.root / "a.cpp"), 2, None, "undeclared", "FrameEncoder")
        r = fix.Loop(self.root, TABLE).propose(d)
        self.assertIsInstance(r, fix.Edit)

    def test_a_name_that_is_twice_on_a_line_with_no_column_is_left(self) -> None:
        self.write(
            "a.cpp", "namespace iclforge::mp4 {\nvoid f() { FrameEncoder a; FrameEncoder b; }\n}\n"
        )
        d = fix.Diag(str(self.root / "a.cpp"), 2, None, "undeclared", "FrameEncoder")
        self.assertIsInstance(fix.Loop(self.root, TABLE).propose(d), fix.Left)

    def test_a_column_counts_bytes(self) -> None:
        self.write("a.cpp", "namespace iclforge::mp4 {\n// \u00d7\u00d7 x\nFrameEncoder e;\n}\n")
        e = self.edit("a.cpp", 3, "FrameEncoder", diag("undeclared", "FrameEncoder"))
        self.assertEqual(self.applied("a.cpp", e).split("\n")[2], "ac3::FrameEncoder e;")


class Member(Tree):
    SCOPE = "iclforge::oba"

    def test_a_partial_chain_gets_the_new_namespace_in_front(self) -> None:
        self.write("a.cpp", "namespace iclforge::mp4 {\nauto e = oba::AtmosEncoder{};\n}\n")
        e = self.edit("a.cpp", 2, "AtmosEncoder", diag("member", "AtmosEncoder", self.SCOPE))
        self.assertEqual(
            self.applied("a.cpp", e).split("\n")[1], "auto e = ac3::oba::AtmosEncoder{};"
        )

    def test_a_fully_qualified_name_gets_it_after_the_root(self) -> None:
        self.write("a.cpp", "void f() { iclforge::oba::AtmosEncoder e; }\n")
        e = self.edit("a.cpp", 1, "AtmosEncoder", diag("member", "AtmosEncoder", self.SCOPE))
        self.assertEqual(
            self.applied("a.cpp", e), "void f() { iclforge::ac3::oba::AtmosEncoder e; }\n"
        )

    def test_a_global_qualification_keeps_its_colons(self) -> None:
        self.write("a.cpp", "void f() { ::iclforge::oba::AtmosEncoder e; }\n")
        e = self.edit("a.cpp", 1, "AtmosEncoder", diag("member", "AtmosEncoder", self.SCOPE))
        self.assertEqual(
            self.applied("a.cpp", e), "void f() { ::iclforge::ac3::oba::AtmosEncoder e; }\n"
        )

    def test_a_chain_that_covers_the_end_of_the_path_names_the_rest(self) -> None:
        # inside iclforge::oba, `joc::reconstruct` is iclforge::oba::joc::reconstruct
        table = core.Table.from_json(
            {
                "namespaces": {
                    "<root>": {"owner": "shared", "names": [], "others": {"x": ["oba"]}},
                    "oba": {
                        "owner": "shared",
                        "names": [],
                        "others": {"src/objects": ["Position"]},
                    },
                    "oba::joc": {
                        "owner": "shared",
                        "names": ["reconstruct"],
                        "others": {"o": ["Domain"]},
                    },
                }
            }
        )
        self.write("a.cpp", "namespace iclforge::oba {\nauto r = joc::reconstruct(x);\n}\n")
        d = fix.Diag(
            str(self.root / "a.cpp"),
            2,
            self.col("a.cpp", 2, "reconstruct"),
            "member",
            "reconstruct",
            "iclforge::oba::joc",
        )
        r = fix.Loop(self.root, table).propose(d)
        assert isinstance(r, fix.Edit)
        self.assertEqual(r.text, "ac3::oba::")

    def test_inside_the_library_the_other_half_of_a_shared_namespace_is_anchored(self) -> None:
        self.write(
            "a.cpp", "namespace iclforge::ac3 {\nauto a = internal::arch::i32x4::broadcast(3);\n}\n"
        )
        e = self.edit("a.cpp", 2, "arch", diag("member", "arch", "iclforge::ac3::internal"))
        self.assertEqual(
            self.applied("a.cpp", e).split("\n")[1],
            "auto a = iclforge::internal::arch::i32x4::broadcast(3);",
        )

    def test_the_anchor_names_the_part_of_the_path_the_chain_does_not(self) -> None:
        # inside iclforge::ac3::oba, `joc::Domain` finds the library's joc, which lacks Domain: the
        # objects library's is iclforge::oba::joc::Domain, and the chain names only `joc`
        self.write(
            "a.cpp", "namespace iclforge::ac3::oba {\njoc::Domain d = joc::Domain::kQmf;\n}\n"
        )
        scope = "iclforge::ac3::oba::joc"
        e = self.edit("a.cpp", 2, "Domain", diag("member", "Domain", scope))
        self.assertEqual(e.text, "iclforge::oba::")
        e = self.edit("a.cpp", 2, "Domain", diag("member", "Domain", scope), 2)
        self.assertEqual(
            self.applied("a.cpp", e).split("\n")[1],
            "joc::Domain d = iclforge::oba::joc::Domain::kQmf;",
        )

    def test_a_chain_that_names_the_librarys_copy_on_purpose_is_left(self) -> None:
        self.write("a.cpp", "namespace iclforge::mp4 {\nac3::internal::Fixed32 x;\n}\n")
        d = diag("member", "Fixed32", "iclforge::ac3::internal")
        d = fix.Diag(d.file, 2, self.col("a.cpp", 2, "Fixed32"), d.kind, d.name, d.scope)
        self.assertIsInstance(self.propose("a.cpp", d), fix.Left)

    def test_inside_the_library_a_type_of_the_other_half(self) -> None:
        self.write("a.cpp", "namespace iclforge::ac3 {\nstd::span<const internal::Fixed32> s;\n}\n")
        e = self.edit("a.cpp", 2, "Fixed32", diag("member", "Fixed32", "iclforge::ac3::internal"))
        self.assertEqual(e.text, "iclforge::")
        self.assertEqual(
            self.applied("a.cpp", e).split("\n")[1],
            "std::span<const iclforge::internal::Fixed32> s;",
        )

    def test_a_name_no_other_library_declares_is_left(self) -> None:
        self.write("a.cpp", "namespace iclforge::ac3 {\nauto a = render::Mystery{};\n}\n")
        r = self.propose(
            "a.cpp", diag("member", "Mystery", "iclforge::ac3::render", line=2, col=19)
        )
        self.assertIsInstance(r, fix.Left)

    def test_a_chain_that_starts_at_an_alias_is_left(self) -> None:
        self.write("a.cpp", "namespace bn = iclforge::internal;\nauto x = bn::pow43(2);\n")
        r = self.propose("a.cpp", diag("member", "pow43", "iclforge::internal", line=2, col=13))
        self.assertIsInstance(r, fix.Left)

    def test_a_class_scope_or_a_template_instance_is_what_follows_an_earlier_error(self) -> None:
        self.write("a.cpp", "void f() { t.fft; }\n")
        d = diag(
            "member",
            "fft",
            "iclforge::ac3::(anonymous namespace)::FastMdctTables<512>",
            line=1,
            col=14,
        )
        self.assertIsNone(self.propose("a.cpp", d))

    def test_a_member_another_librarys_namespace_lacks_is_listed_not_written(self) -> None:
        self.write("a.cpp", "void f() { mp4::nothing(); }\n")
        col = self.col("a.cpp", 1, "nothing")
        r = self.propose("a.cpp", diag("member", "nothing", "iclforge::mp4", line=1, col=col))
        self.assertIsInstance(r, fix.Left)


class UsingNamespace(Tree):
    def col(self, rel: str, line: int, needle: str) -> int:
        row = (self.root / rel).read_text(encoding="utf-8").split("\n")[line - 1]
        return row.index(needle) + 1

    def test_a_directive_whose_namespace_the_unit_no_longer_sees_is_the_librarys_now(self) -> None:
        self.write("a.cpp", "void f() {\n    using namespace iclforge::internal;\n}\n")
        d = fix.Diag(str(self.root / "a.cpp"), 2, 31, "usingns", "")
        r = fix.Loop(self.root, TABLE).propose(d)
        assert isinstance(r, fix.Edit)
        text = (self.root / "a.cpp").read_text(encoding="utf-8")
        self.assertEqual(
            (text[: r.offset] + r.text + text[r.offset :]).split("\n")[1],
            "    using namespace iclforge::ac3::internal;",
        )

    def test_a_directive_of_a_namespace_the_library_does_not_declare_into_is_left(self) -> None:
        self.write("a.cpp", "using namespace iclforge::mp4;\n")
        d = fix.Diag(str(self.root / "a.cpp"), 1, 26, "usingns", "")
        self.assertIsInstance(fix.Loop(self.root, TABLE).propose(d), fix.Left)

    def test_the_message_is_read_without_a_name(self) -> None:
        (d,) = fix.parse_log("a.cpp(338,31): error: expected namespace name\n")
        self.assertEqual((d.kind, d.name, d.line, d.col), ("usingns", "", 338, 31))

    def test_the_directive_added_beside_it_is_dropped(self) -> None:
        self.write(
            "a.cpp",
            "void f() {\n    using namespace iclforge::internal;\n    auto x = pow43(2);\n}\n",
        )
        path = str(self.root / "a.cpp")
        diags = [
            fix.Diag(path, 2, 31, "usingns", ""),
            fix.Diag(path, 3, self.col("a.cpp", 3, "pow43"), "undeclared", "pow43"),
        ]
        edits, left = fix.plan(fix.Loop(self.root, TABLE), diags)
        self.assertEqual([e.why for e in edits], ["usingns:internal"])
        self.assertEqual(left, [])


class Planning(Tree):
    def test_two_spellings_of_one_file_are_one_file(self) -> None:
        self.write("common/h.hpp", "namespace iclforge::mp4 {\nFrameEncoder e;\n}\n")
        (self.root / "cli").mkdir()
        loop = fix.Loop(self.root, TABLE)
        direct = str(self.root / "common" / "h.hpp")
        around = str(self.root / "cli" / ".." / "common" / "h.hpp")
        diags = [
            fix.Diag(direct, 2, 1, "undeclared", "FrameEncoder"),
            fix.Diag(around, 2, 1, "undeclared", "FrameEncoder"),
        ]
        edits, left = fix.plan(loop, diags)
        self.assertEqual(len(edits), 1)
        self.assertEqual(left, [])

    def test_apply_writes_every_edit_and_keeps_the_places_of_the_earlier_ones(self) -> None:
        self.write("a.cpp", "namespace iclforge::mp4 {\nFrameEncoder a; FrameEncoder b;\n}\n")
        loop = fix.Loop(self.root, TABLE)
        path = str(self.root / "a.cpp")
        diags = [
            fix.Diag(path, 2, 1, "undeclared", "FrameEncoder"),
            fix.Diag(path, 2, 17, "undeclared", "FrameEncoder"),
        ]
        edits, _ = fix.plan(loop, diags)
        files, skipped = fix.apply(self.root, edits)
        self.assertEqual((files, skipped), (1, []))
        self.assertEqual(
            (self.root / "a.cpp").read_text(encoding="utf-8"),
            "namespace iclforge::mp4 {\nac3::FrameEncoder a; ac3::FrameEncoder b;\n}\n",
        )

    def test_an_edit_whose_place_has_changed_is_skipped_and_named(self) -> None:
        self.write("a.cpp", "namespace iclforge::mp4 {\n  FrameEncoder a;\n}\n")
        loop = fix.Loop(self.root, TABLE)
        edits, _ = fix.plan(
            loop, [fix.Diag(str(self.root / "a.cpp"), 2, 3, "undeclared", "FrameEncoder")]
        )
        self.write(
            "a.cpp", "namespace iclforge::mp4 {\n    FrameEncoder a;\n}\n"
        )  # the file moved on
        _, skipped = fix.apply(self.root, edits)
        self.assertEqual(len(skipped), 1)
        self.assertEqual(
            (self.root / "a.cpp").read_text(encoding="utf-8"),
            "namespace iclforge::mp4 {\n    FrameEncoder a;\n}\n",
        )

    def test_a_second_pass_over_a_fixed_tree_finds_nothing_to_do(self) -> None:
        self.write("a.cpp", "namespace iclforge::mp4 {\nac3::FrameEncoder a;\n}\n")
        self.assertEqual(fix.plan(fix.Loop(self.root, TABLE), [])[0], [])


class Records(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name) / "repo"
        self.root.mkdir()
        self.git("init", "-q")
        self.git("config", "user.name", "t")
        self.git("config", "user.email", "t@example.org")
        self.git("config", "core.autocrlf", "false")

    def git(self, *args: str) -> str:
        return subprocess.run(
            ["git", "-C", str(self.root), *args], capture_output=True, text=True, check=True
        ).stdout

    def write(self, rel: str, text: str) -> None:
        (self.root / rel).write_bytes(text.encode("utf-8"))

    def commit(self) -> str:
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "x")
        return self.git("rev-parse", "HEAD").strip()

    def test_a_record_made_on_one_tree_makes_the_same_edit_on_another(self) -> None:
        crlf = "one\r\nFrameEncoder a;\r\nthree\r\nFrameEncoder b;\r\n"
        lf = "x\nusing namespace iclforge;\ny\n"
        self.write("a.cpp", crlf)
        self.write("b.cpp", lf)
        base = self.commit()
        self.write("a.cpp", crlf.replace("FrameEncoder", "ac3::FrameEncoder"))
        self.write("b.cpp", "x\nusing namespace iclforge;\nusing namespace iclforge::ac3;\ny\n")
        hunks = fix.record(self.root, base)
        self.assertEqual(sorted({h["file"] for h in hunks}), ["a.cpp", "b.cpp"])
        done = {
            "a.cpp": (self.root / "a.cpp").read_bytes(),
            "b.cpp": (self.root / "b.cpp").read_bytes(),
        }
        self.git("checkout", "-q", "--", ".")
        self.assertEqual(fix.replay(self.root, hunks), [])
        for rel, data in done.items():
            self.assertEqual((self.root / rel).read_bytes(), data, rel)

    def test_a_line_that_is_not_the_one_the_record_names_stops_the_run_and_writes_nothing(
        self,
    ) -> None:
        self.write("a.cpp", "one\nFrameEncoder a;\n")
        self.write("b.cpp", "uno\nFrameEncoder b;\n")
        base = self.commit()
        self.write("a.cpp", "one\nac3::FrameEncoder a;\n")
        self.write("b.cpp", "uno\nac3::FrameEncoder b;\n")
        hunks = fix.record(self.root, base)
        self.git("checkout", "-q", "--", ".")
        self.write("b.cpp", "uno\nSomethingElse b;\n")
        problems = fix.replay(self.root, hunks)
        self.assertEqual(len(problems), 1)
        self.assertIn("b.cpp:2", problems[0])
        self.assertEqual(
            (self.root / "a.cpp").read_text(encoding="utf-8"), "one\nFrameEncoder a;\n"
        )

    def test_a_second_replay_changes_nothing_and_says_so(self) -> None:
        self.write("a.cpp", "one\nFrameEncoder a;\nthree\nFrameEncoder b;\n")
        base = self.commit()
        self.write("a.cpp", "one\nac3::FrameEncoder a;\nthree\nac3::FrameEncoder b;\n")
        hunks = fix.record(self.root, base)
        self.git("checkout", "-q", "--", ".")
        self.assertEqual(fix.replay(self.root, hunks), [])
        done = (self.root / "a.cpp").read_bytes()
        self.assertTrue(fix.has_the_record(self.root, hunks))
        self.assertEqual(fix.replay(self.root, hunks), [])
        self.assertEqual((self.root / "a.cpp").read_bytes(), done)

    def test_the_record_is_found_in_a_file_whose_hunks_changed_its_length(self) -> None:
        self.write("a.cpp", "one\ntwo\nthree\nfour\n")
        base = self.commit()
        self.write("a.cpp", "one\nuno\ndos\nthree\nfour\nfive\n")
        hunks = fix.record(self.root, base)
        self.assertTrue(fix.has_the_record(self.root, hunks))
        self.git("checkout", "-q", "--", ".")
        self.assertFalse(fix.has_the_record(self.root, hunks))

    def test_a_tree_that_has_part_of_the_record_is_a_problem_and_nothing_is_written(self) -> None:
        self.write("a.cpp", "one\nFrameEncoder a;\n")
        self.write("b.cpp", "uno\nFrameEncoder b;\n")
        base = self.commit()
        self.write("a.cpp", "one\nac3::FrameEncoder a;\n")
        self.write("b.cpp", "uno\nac3::FrameEncoder b;\n")
        hunks = fix.record(self.root, base)
        self.git("checkout", "-q", "--", "b.cpp")
        problems = fix.replay(self.root, hunks)
        self.assertEqual(len(problems), 1)
        self.assertIn("have the record already and the rest not", problems[0])
        self.assertEqual(
            (self.root / "b.cpp").read_text(encoding="utf-8"), "uno\nFrameEncoder b;\n"
        )


if __name__ == "__main__":
    unittest.main()
