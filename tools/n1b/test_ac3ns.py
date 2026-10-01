"""Unit tests for n1b_ac3ns.py and ac3ns_census.py: the AC-3 library's names under `iclforge::ac3`.

stdlib `unittest`. The cases are text written out here in the forms the tree has them. What must not
change is as much of the test as what must: the other libraries' half of a shared namespace, the
ESPHome component's own `esphome::iclforge`, a name that already carries the new namespace, and the
blocks a file outside the library opens for its own sake.
"""

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ac3ns_census as census
import ac3ns_core as scan
import n1b_ac3ns as ns

TABLE = scan.Table.from_json(
    {
        "namespaces": {
            "<root>": {
                "owner": "shared",
                "names": ["FrameEncoder", "Acmod", "version_details", "kMaxChannels"],
                "others": {"src/base": ["BitReader"], "esp-idf": ["Player"]},
            },
            "meta": {"owner": "exclusive", "names": ["QcPreset"]},
            "io": {"owner": "exclusive", "names": ["read_wav"]},
            "eac3": {"owner": "exclusive", "names": ["AccessUnitEncoder"]},
            "eac3::chanmap": {"owner": "exclusive", "names": ["Location"]},
            "oba": {
                "owner": "shared",
                "names": ["AtmosEncoder"],
                "others": {"src/objects": ["Position", "ObjectScene"]},
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
                "names": ["pow43"],
                "others": {"src/arithmetic": ["Fixed32"]},
            },
            "internal::avx2": {"owner": "exclusive", "names": ["avx2_probe_matches_expected"]},
        },
        "followers": {
            "tests/ac3/core/avx2/absent/avx2_tier.cpp": ["internal::avx2"],
        },
    }
)


def q(text: str) -> str:
    return ns.qualify(text, TABLE)[0]


class Moves(unittest.TestCase):
    def test_a_name_of_the_root_the_library_declares(self) -> None:
        self.assertTrue(TABLE.moves(["FrameEncoder"]))
        self.assertTrue(TABLE.moves(["FrameEncoder", "create"]))

    def test_a_name_of_the_root_another_library_declares(self) -> None:
        self.assertFalse(TABLE.moves(["BitReader"]))
        self.assertFalse(TABLE.moves(["Player"]))

    def test_an_exclusive_namespace_takes_everything_below_it(self) -> None:
        self.assertTrue(TABLE.moves(["meta"]))
        self.assertTrue(TABLE.moves(["meta", "anything_at_all"]))
        self.assertTrue(TABLE.moves(["eac3", "chanmap", "Location"]))

    def test_a_shared_namespace_is_decided_by_the_name(self) -> None:
        self.assertTrue(TABLE.moves(["oba", "AtmosEncoder"]))
        self.assertFalse(TABLE.moves(["oba", "Position"]))
        self.assertFalse(TABLE.moves(["oba"]))
        self.assertTrue(TABLE.moves(["oba", "joc", "reconstruct"]))
        self.assertFalse(TABLE.moves(["oba", "joc", "Domain"]))

    def test_a_namespace_the_table_does_not_list_is_left_alone(self) -> None:
        self.assertFalse(TABLE.moves(["mp4", "Writer"]))
        self.assertFalse(TABLE.moves(["internal", "arch", "simd"]))
        self.assertFalse(TABLE.moves(["ac3", "FrameEncoder"]))  # already moved


class Qualify(unittest.TestCase):
    def test_the_forms_a_use_takes(self) -> None:
        self.assertEqual(
            q(
                "iclforge::FrameEncoder encoder{config};\n"
                "auto w = iclforge::io::read_wav(path);\n"
                "using iclforge::meta::QcPreset;\n"
                "namespace cm = iclforge::eac3::chanmap;\n"
                "using namespace iclforge::eac3;\n"
            ),
            "iclforge::ac3::FrameEncoder encoder{config};\n"
            "auto w = iclforge::ac3::io::read_wav(path);\n"
            "using iclforge::ac3::meta::QcPreset;\n"
            "namespace cm = iclforge::ac3::eac3::chanmap;\n"
            "using namespace iclforge::ac3::eac3;\n",
        )

    def test_a_global_qualification_keeps_its_leading_colons(self) -> None:
        self.assertEqual(q("::iclforge::FrameEncoder e;"), "::iclforge::ac3::FrameEncoder e;")

    def test_a_name_inside_a_template_argument_or_after_a_scope(self) -> None:
        self.assertEqual(
            q("std::vector<iclforge::Acmod> v; f(iclforge::Acmod::k20);"),
            "std::vector<iclforge::ac3::Acmod> v; f(iclforge::ac3::Acmod::k20);",
        )

    def test_the_other_half_of_a_shared_namespace_stays(self) -> None:
        self.assertEqual(
            q("iclforge::oba::Position p; iclforge::oba::AtmosEncoder e;"),
            "iclforge::oba::Position p; iclforge::ac3::oba::AtmosEncoder e;",
        )
        self.assertEqual(
            q("iclforge::render::OutputLayout a; iclforge::render::serve(x);"),
            "iclforge::render::OutputLayout a; iclforge::ac3::render::serve(x);",
        )
        self.assertEqual(
            q("iclforge::internal::Fixed32 a; iclforge::internal::pow43(x);"),
            "iclforge::internal::Fixed32 a; iclforge::ac3::internal::pow43(x);",
        )

    def test_other_libraries_and_the_root_names_that_are_theirs_stay(self) -> None:
        text = "iclforge::mp4::Writer w; iclforge::BitReader r; iclforge::sendspin::player::Role x;"
        self.assertEqual(q(text), text)

    def test_the_components_own_namespace_inside_esphome_stays(self) -> None:
        text = "esphome::iclforge::FrameEncoder x; foo::iclforge::meta::QcPreset y;"
        self.assertEqual(q(text), text)

    def test_a_name_that_only_ends_in_the_word_stays(self) -> None:
        text = "my_iclforge::FrameEncoder x;"
        self.assertEqual(q(text), text)

    def test_a_second_run_changes_nothing(self) -> None:
        once = q(
            "iclforge::FrameEncoder e; iclforge::meta::QcPreset p; iclforge::oba::AtmosEncoder a;"
        )
        self.assertEqual(q(once), once)

    def test_the_nested_declaration_head_of_an_exclusive_namespace(self) -> None:
        # a file outside the library that opens one of its namespaces moves with it
        self.assertEqual(
            q("namespace iclforge::io::detail {\n}  // namespace iclforge::io::detail\n"),
            "namespace iclforge::ac3::io::detail {\n}  // namespace iclforge::ac3::io::detail\n",
        )

    def test_the_context_of_each_change_is_reported(self) -> None:
        text = (
            "// see iclforge::FrameEncoder\n"
            'auto m = "iclforge::meta::QcPreset failed";\n'
            "iclforge::Acmod a;\n"
            "/* iclforge::io::read_wav */ int x;\n"
        )
        _, changes = ns.qualify(text, TABLE)
        self.assertEqual(
            [(c.line, c.context) for c in changes],
            [(1, "comment"), (2, "string"), (3, "code"), (4, "comment")],
        )

    def test_a_crlf_file_keeps_its_line_endings(self) -> None:
        self.assertEqual(
            q("iclforge::Acmod a;\r\nint b;\r\n"), "iclforge::ac3::Acmod a;\r\nint b;\r\n"
        )


class Declare(unittest.TestCase):
    def d(self, text: str, path: str = "src/ac3/src/meta/x.cpp") -> str:
        return ns.declare(text, TABLE, path, path.startswith(ns.LIBRARY_PREFIX))[0]

    def test_the_heads_and_closing_comments_of_the_librarys_own_files(self) -> None:
        self.assertEqual(
            self.d(
                "namespace iclforge {\n"
                "}  // namespace iclforge\n"
                "namespace iclforge::meta {\n"
                "namespace detail {\n"
                "}  // namespace detail\n"
                "}  // namespace iclforge::meta\n"
                "namespace iclforge::oba::joc {\n"
                "}  // namespace iclforge::oba::joc\n"
            ),
            "namespace iclforge::ac3 {\n"
            "}  // namespace iclforge::ac3\n"
            "namespace iclforge::ac3::meta {\n"
            "namespace detail {\n"
            "}  // namespace detail\n"
            "}  // namespace iclforge::ac3::meta\n"
            "namespace iclforge::ac3::oba::joc {\n"
            "}  // namespace iclforge::ac3::oba::joc\n",
        )

    def test_a_second_run_changes_nothing(self) -> None:
        once = self.d("namespace iclforge::io {\n}  // namespace iclforge::io\n")
        self.assertEqual(self.d(once), once)

    def test_an_anonymous_namespace_and_other_namespaces_stay(self) -> None:
        text = "namespace {\n}  // namespace\nnamespace std {\n}  // namespace std\n"
        self.assertEqual(self.d(text), text)

    def test_the_indentation_and_an_inline_namespace_are_kept(self) -> None:
        self.assertEqual(
            self.d("  inline namespace iclforge::v1 {\n  }  // namespace iclforge::v1\n"),
            "  inline namespace iclforge::ac3::v1 {\n  }  // namespace iclforge::ac3::v1\n",
        )

    def test_a_file_outside_the_library_moves_only_the_paths_the_table_lists(self) -> None:
        path = "tests/ac3/core/avx2/absent/avx2_tier.cpp"
        text = (
            "namespace iclforge::internal::avx2 {\n"
            "bool avx2_probe_matches_expected() noexcept;\n"
            "}  // namespace iclforge::internal::avx2\n"
            "namespace iclforge::test::avx2 {\n"
            "}  // namespace iclforge::test::avx2\n"
        )
        self.assertEqual(
            self.d(text, path),
            "namespace iclforge::ac3::internal::avx2 {\n"
            "bool avx2_probe_matches_expected() noexcept;\n"
            "}  // namespace iclforge::ac3::internal::avx2\n"
            "namespace iclforge::test::avx2 {\n"
            "}  // namespace iclforge::test::avx2\n",
        )

    def test_a_file_outside_the_library_that_is_no_follower_is_left_alone(self) -> None:
        text = "namespace iclforge::io {\n}  // namespace iclforge::io\n"
        self.assertEqual(self.d(text, "tests/ac3/io/test_x.cpp"), text)

    def test_crlf_is_kept(self) -> None:
        self.assertEqual(
            self.d("namespace iclforge::io {\r\n}  // namespace iclforge::io\r\n"),
            "namespace iclforge::ac3::io {\r\n}  // namespace iclforge::ac3::io\r\n",
        )


class Reading(unittest.TestCase):
    def test_the_history_the_rust_crate_and_cmake_are_not_read(self) -> None:
        self.assertFalse(ns.reads_uses("CHANGELOG.md"))
        self.assertFalse(ns.reads_uses("planning/ac4.md"))
        self.assertFalse(ns.reads_uses("tools/n1b/README.md"))
        self.assertFalse(ns.reads_uses("rust/iclforge/src/lib.rs"))
        self.assertFalse(ns.reads_uses("src/ac3/CMakeLists.txt"))
        self.assertFalse(ns.reads_uses("cmake/IclforgeLibrary.cmake"))
        self.assertFalse(ns.reads_uses("tests/golden/bitstream-hashes.json"))

    def test_the_code_and_the_pages_are_read(self) -> None:
        for path in (
            "apps/cli/main.cpp",
            "src/ac3/include/iclforge/ac3/version.hpp.in",
            "docs/library/ac3.md",
        ):
            self.assertTrue(ns.reads_uses(path), path)


class Lexing(unittest.TestCase):
    def test_comments_strings_and_literals_are_blanked_with_offsets_kept(self) -> None:
        text = (
            "int a; // namespace x {\n"
            "const char* s = \"namespace y {\"; /* namespace z { */ char c = '{';\n"
        )
        m = scan.mask(text)
        self.assertEqual(len(m), len(text))
        self.assertNotIn("namespace", m)
        self.assertNotIn("{", m)
        self.assertEqual(m.count("\n"), text.count("\n"))

    def test_a_raw_string_and_a_digit_separator(self) -> None:
        text = "auto r = R\"x(namespace q {)x\"; int n = 1'000'000; namespace real {\n"
        m = scan.mask(text)
        self.assertIn("namespace real {", m)
        self.assertNotIn("namespace q", m)

    def test_a_line_comment_that_continues(self) -> None:
        m = scan.mask("// one \\\n namespace hidden {\nint x;\n")
        self.assertNotIn("namespace", m)


class Blocks(unittest.TestCase):
    def paths(self, text: str) -> list[tuple[str, ...]]:
        return [b.path for b in scan.blocks(text) if b.named]

    def test_the_absolute_path_of_each_block(self) -> None:
        text = (
            "namespace iclforge { namespace meta {\n"
            "namespace detail {}\n"
            "}}\n"
            "namespace iclforge::io::detail {}\n"
            "namespace {}\n"
            "namespace al = iclforge::io;\n"
            "using namespace iclforge::io;\n"
        )
        self.assertEqual(
            self.paths(text),
            [
                ("iclforge",),
                ("iclforge", "meta"),
                ("iclforge", "meta", "detail"),
                ("iclforge", "io", "detail"),
                (),
            ],
        )

    def test_the_namespace_at_an_offset(self) -> None:
        text = "namespace iclforge::meta {\nstruct S { int f() { return 1; } };\n}\nint after;\n"
        bs = scan.blocks(text)
        self.assertEqual(scan.enclosing_path(bs, text.index("return")), ("iclforge", "meta"))
        self.assertEqual(scan.enclosing_path(bs, text.index("after")), ())


# --- the census ---------------------------------------------------------------------------------

BASE = "D:\\work\\tree\\"


def node(kind: str, name: str | None = None, **kw: object) -> dict:
    d: dict = {"kind": kind}
    if name is not None:
        d["name"] = name
    d.update(kw)
    return d


def at(file: str | None, line: int) -> dict:
    d: dict = {"line": line, "col": 1}
    if file:
        d["file"] = file
    return d


class Walking(unittest.TestCase):
    def dump(self) -> dict:
        f1 = BASE + "src\\ac3\\include\\iclforge/ac3/meta/qc.hpp"
        f2 = BASE + "src\\objects\\include\\iclforge/objects/oamd.hpp"
        return node(
            "NamespaceDecl",
            "iclforge",
            loc=at(f1, 3),
            inner=[
                node("NamespaceDecl", "meta", loc=at(None, 4), inner=[
                    node("CXXRecordDecl", "QcPreset", loc=at(None, 6), completeDefinition=True),
                    node("CXXRecordDecl", None, loc=at(None, 7), isImplicit=True),
                    node("EnumDecl", "Unscoped", loc=at(None, 9), inner=[
                        node("EnumConstantDecl", "kA", loc=at(None, 10)),
                    ]),
                    node("EnumDecl", "Scoped", loc=at(None, 12), scopedEnumTag="class", inner=[
                        node("EnumConstantDecl", "kB", loc=at(None, 13)),
                    ]),
                    node("UsingDecl", "iclforge::base::Location", loc=at(None, 15)),
                    node("FunctionDecl", "operator<<", loc=at(None, 16)),
                    node("UsingDirectiveDecl", loc=at(None, 17)),
                ]),
                node("NamespaceDecl", "oba", loc=at(f2, 8), inner=[
                    node("VarDecl", "kLimit", loc=at(None, 9)),
                ]),
            ],
        )  # fmt: skip

    def test_declarations_are_attributed_to_the_file_the_compiler_last_named(self) -> None:
        w = census.Walker()
        w.node(self.dump(), ())
        got = {(p, n): (k, f.rsplit("/", 1)[-1], line) for p, n, k, f, line in w.decls}
        self.assertEqual(got[(("meta",), "QcPreset")], ("CXXRecordDecl", "qc.hpp", 6))
        self.assertEqual(got[(("oba",), "kLimit")], ("VarDecl", "oamd.hpp", 9))

    def test_the_enumerators_of_an_unscoped_enumeration_only(self) -> None:
        w = census.Walker()
        w.node(self.dump(), ())
        names = {n for _, n, _, _, _ in w.decls}
        self.assertIn("kA", names)
        self.assertNotIn("kB", names)

    def test_a_using_declaration_binds_its_last_component(self) -> None:
        w = census.Walker()
        w.node(self.dump(), ())
        self.assertIn((("meta",), "Location"), {(p, n) for p, n, *_ in w.decls})

    def test_implicit_declarations_and_using_directives_are_skipped(self) -> None:
        w = census.Walker()
        w.node(self.dump(), ())
        kinds = {k for _, _, k, _, _ in w.decls}
        self.assertNotIn("UsingDirectiveDecl", kinds)
        self.assertEqual(sum(1 for _, n, *_ in w.decls if n is None), 0)

    def test_declarations_are_filed_by_path_relative_to_the_tree(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            dump = Path(d) / "ast.json"
            glob = node(
                "FunctionDecl", "iclforge_encoder_create", loc=at(BASE + "src\\capi\\x.h", 1)
            )
            dump.write_text(json.dumps(self.dump()) + "\n" + json.dumps(glob), encoding="utf-8")
            decls = census.ast_declarations(dump, Path("D:/work/tree"))
        files = {f for _, _, _, f, _ in decls}
        self.assertEqual(
            files,
            {
                "src/ac3/include/iclforge/ac3/meta/qc.hpp",
                "src/objects/include/iclforge/objects/oamd.hpp",
            },
        )
        # the global declaration the filter matched by its name is not in the root namespace
        self.assertNotIn("iclforge_encoder_create", {n for _, n, *_ in decls})


class FakeRepo:
    def __init__(self, files: dict[str, str]) -> None:
        self.files = sorted(files)
        self._t = files

    def ext(self, rel: str) -> str:
        return "." + rel.rsplit(".", 1)[-1]

    def read(self, rel: str) -> str:
        return self._t[rel]


class Table_(unittest.TestCase):
    def table(self) -> dict:
        files = {
            "src/ac3/src/io/wav.cpp": "namespace iclforge::io {\nvoid read_wav();\n}\n",
            "src/ac3/src/oba/joc.cpp": "namespace iclforge::oba {\nvoid encode_frame();\n}\n",
            "src/objects/src/scene.cpp": "namespace iclforge::oba {\nstruct Position {};\n}\n",
            "tests/ac3/io/test_wav.cpp": "namespace iclforge::test {\nstruct Helper {};\n}\n",
            "tests/ac3/core/avx2.cpp": "namespace iclforge::internal::avx2 {\nbool probe();\n}\n",
            "tests/ac3/core/other.cpp": "namespace iclforge::io::detail {\n// nothing\n}\n",
        }
        decls = [
            ((), "FrameEncoder", "CXXRecordDecl", "src/ac3/include/iclforge/ac3/encoder.hpp", 1),
            (("io",), "read_wav", "FunctionDecl", "src/ac3/include/iclforge/ac3/io/wav.hpp", 1),
            (
                ("oba",),
                "AtmosEncoder",
                "CXXRecordDecl",
                "src/ac3/include/iclforge/ac3/oba/atmos.hpp",
                1,
            ),
            (
                ("oba",),
                "Position",
                "CXXRecordDecl",
                "src/objects/include/iclforge/objects/placement.hpp",
                1,
            ),
            (
                ("internal", "avx2"),
                "probe",
                "FunctionDecl",
                "src/ac3/src/internal/avx2/probe.hpp",
                1,
            ),
            (("io", "detail"), "helper", "FunctionDecl", "src/ac3/src/io/detail.hpp", 1),
        ]
        return census.build_table(FakeRepo(files), decls, "abc123")

    def test_an_exclusive_namespace_and_a_shared_one(self) -> None:
        rows = self.table()["namespaces"]
        self.assertEqual(rows["io"]["owner"], "exclusive")
        self.assertEqual(rows["oba"]["owner"], "shared")
        self.assertEqual(rows["oba"]["names"], ["AtmosEncoder"])
        self.assertEqual(rows["oba"]["others"], {"src/objects": ["Position"]})

    def test_the_root_the_library_declares_into_is_listed(self) -> None:
        self.assertIn("<root>", self.table()["namespaces"])
        self.assertEqual(self.table()["namespaces"]["<root>"]["names"], ["FrameEncoder"])

    def test_a_file_that_defines_a_name_the_library_declares_follows_it(self) -> None:
        t = self.table()
        # the stand-in declares `probe`, the library's own name, so its block follows
        self.assertEqual(t["followers"], {"tests/ac3/core/avx2.cpp": ["internal::avx2"]})
        self.assertEqual(t["namespaces"]["internal::avx2"]["owner"], "exclusive")

    def test_a_file_that_declares_a_name_of_its_own_in_the_namespace_makes_it_shared(self) -> None:
        files = {
            "src/ac3/src/io/wav.cpp": "namespace iclforge::io {\nvoid read_wav();\n}\n",
            "tests/ac3/io/helper.cpp": "namespace iclforge::io {\nstruct Helper {};\n}\n",
        }
        decls = [
            (("io",), "read_wav", "FunctionDecl", "src/ac3/include/iclforge/ac3/io/wav.hpp", 1)
        ]
        row = census.build_table(FakeRepo(files), decls, "abc")["namespaces"]["io"]
        self.assertEqual(row["owner"], "shared")
        self.assertEqual(row["others"], {"tests": ["Helper"]})

    def test_a_file_that_opens_an_exclusive_namespace_for_its_own_sake_is_listed(self) -> None:
        t = self.table()
        self.assertEqual(t["outside_openers"].get("io::detail"), ["tests/ac3/core/other.cpp"])

    def test_the_tests_own_namespaces_are_not_the_librarys(self) -> None:
        self.assertNotIn("test", self.table()["namespaces"])

    def test_the_table_the_pass_reads_is_the_one_the_census_writes(self) -> None:
        t = scan.Table.from_json(self.table())
        self.assertTrue(t.moves(["io", "read_wav"]))
        self.assertTrue(t.moves(["oba", "AtmosEncoder"]))
        self.assertFalse(t.moves(["oba", "Position"]))
        self.assertTrue(t.moves(["FrameEncoder"]))


class CommittedTable(unittest.TestCase):
    """The data the stage runs on, held to the shape the pass needs."""

    def test_the_committed_table_loads_and_names_the_librarys_namespaces(self) -> None:
        path = Path(__file__).resolve().parent / "ac3ns_symbols.json"
        if not path.exists():
            self.skipTest("no table committed yet")
        t = scan.Table.load(path)
        self.assertTrue(t.moves(["FrameEncoder"]))
        self.assertTrue(t.moves(["meta", "QcPreset"]))
        self.assertFalse(t.moves(["BitReader"]))
        self.assertFalse(t.moves(["oba", "Position"]))
        self.assertFalse(t.moves(["mp4", "Writer"]))
        data = json.loads(path.read_text(encoding="utf-8"))
        self.assertEqual(
            data["outside_openers"], {}, "a file outside the library opens an exclusive namespace"
        )


if __name__ == "__main__":
    unittest.main()
