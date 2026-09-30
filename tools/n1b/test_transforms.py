"""Unit tests for the re-layout's two text rewrites: names (n1b_names), build files (n1b_cmake).

stdlib `unittest`; each case is a line of C++ or CMake and what it becomes. What they pin is the
contract the migration relies on: the family root replaces `ac3` wherever it is a namespace or a
qualifier, and nowhere it is part of another name (`eac3::`, `foo::ac3::`); an include spelling is
left to n1b_apply; and a build file's target names, output names and moved paths follow the move
map without touching a path that only ends the same way.
"""

import json
import sys
import unittest
from pathlib import Path
from typing import ClassVar

sys.path.insert(0, str(Path(__file__).resolve().parent))

import n1b_cmake
import n1b_names


class Names(unittest.TestCase):
    def check(self, before: str, after: str) -> None:
        self.assertEqual(n1b_names.transform(before), after)

    def test_a_namespace_and_its_closing_comment(self) -> None:
        self.check(
            "namespace ac3 {\n}  // namespace ac3\n",
            "namespace iclforge {\n}  // namespace iclforge\n",
        )
        self.check(
            "namespace ac3::io {\n} // namespace ac3::io",
            "namespace iclforge::io {\n} // namespace iclforge::io",
        )

    def test_a_qualifier_and_a_global_qualifier(self) -> None:
        self.check("ac3::render::OutputLayout room;", "iclforge::render::OutputLayout room;")
        self.check("::ac3::x y;", "::iclforge::x y;")

    def test_a_name_that_only_contains_ac3_is_left_alone(self) -> None:
        self.check("eac3::chanmap::Location l;", "eac3::chanmap::Location l;")
        self.check("foo::ac3::bar b;", "foo::ac3::bar b;")
        self.check('#include "ac3/core/x.hpp"', '#include "ac3/core/x.hpp"')

    def test_the_libraries_that_were_top_level_nest_under_the_family(self) -> None:
        self.check(
            "mp4::AudioTrack t; using namespace mp4;",
            "iclforge::mp4::AudioTrack t; using namespace iclforge::mp4;",
        )
        self.check("namespace mp4 {\n}", "namespace iclforge::mp4 {\n}")
        self.check("ac4::detail::x y;", "iclforge::ac4::detail::x y;")

    def test_a_namespace_alias_keeps_its_own_name(self) -> None:
        self.check("namespace mp4 = other;", "namespace mp4 = other;")

    def test_libadms_namespace_is_named_from_the_global_namespace(self) -> None:
        self.check(
            "std::shared_ptr<adm::Document> d = adm::parseXml(s);",
            "std::shared_ptr<::adm::Document> d = ::adm::parseXml(s);",
        )
        self.check(
            "::adm::TypeDefinition t; ac3adm::Model m;",
            "::adm::TypeDefinition t; iclforge::adm::Model m;",
        )
        self.check(
            "ac3::admbridge::build(); my_adm::x;", "iclforge::admbridge::build(); my_adm::x;"
        )

    def test_ac3iab_and_ac3adm_become_iab_and_adm(self) -> None:
        self.check(
            "ac3iab::Model m; ac3adm::Doc d;", "iclforge::iab::Model m; iclforge::adm::Doc d;"
        )

    def test_a_library_alias_named_in_a_comment_follows_the_cmake_name(self) -> None:
        self.check(
            "// links ac3::forge_static and ac3::audio",
            "// links iclforge::ac3_static and iclforge::audio",
        )


class CMake(unittest.TestCase):
    MOVES: ClassVar[dict[str, str]] = {
        "src/forge/CMakeLists.txt": "src/ac3/CMakeLists.txt",
        "src/forge/src/core/fft.cpp": "src/dsp/src/fft.cpp",
    }
    DIRS: ClassVar[list[tuple[str, str]]] = [
        ("src/forge/src/core", "src/ac3/src/core"),
        ("src/forge/include/ac3/render", "src/render/include/iclforge/render"),
    ]

    def check(self, before: str, after: str) -> None:
        self.assertEqual(n1b_cmake.transform(before, self.MOVES, self.DIRS), after)

    def test_aliases_follow_the_library_names(self) -> None:
        self.check(
            "ac3::forge_static ac3::forge mp4::mp4_static ac4::decoder ac4::core ac3::audio",
            "iclforge::ac3_static iclforge::ac3 iclforge::mp4_static iclforge::ac4dec "
            "iclforge::ac4core iclforge::audio",
        )

    def test_raw_target_names(self) -> None:
        self.check(
            "forge_objects forge_c_shared mp4_objects ac3iab_static ac3audio ac4core",
            "iclforge_ac3_objects iclforge_capi_shared iclforge_mp4_objects iclforge_iab_static "
            "iclforge_audio iclforge_ac4core",
        )

    def test_a_directory_named_ac4core_in_a_quote_or_a_pattern_keeps_its_name(self) -> None:
        text = (
            'OUT = REPO / "src" / "ac4core" / "src"\n'
            "--filter 'src/(ac4|ac4core|ac4dec)/.*'\n"
        )
        self.check(text, text)
        self.check(
            "target_link_libraries(x PRIVATE ac4core)",
            "target_link_libraries(x PRIVATE iclforge_ac4core)",
        )

    def test_a_raw_name_inside_a_path_or_after_a_scope_is_not_a_target(self) -> None:
        self.check(
            "a/forge_objects.txt ac3::forge_objects_x", "a/forge_objects.txt ac3::forge_objects_x"
        )

    def test_output_names(self) -> None:
        self.check(
            'OUTPUT_NAME "ac3forge"\nOUTPUT_NAME "mp4"\nOUTPUT_NAME "ac3forge_static"',
            'OUTPUT_NAME "iclforge_ac3"\nOUTPUT_NAME "iclforge_mp4"\n'
            'OUTPUT_NAME "iclforge_ac3_static"',
        )
        self.check('OUTPUT_NAME "somethingelse"', 'OUTPUT_NAME "somethingelse"')

    def test_the_library_names_a_pkg_config_file_is_made_from_are_output_names(self) -> None:
        self.check(
            "ac3forge_pkgconfig_libname(_v forge_shared ac3forge ac3forge_static\n"
            '    "${_targets}")\n'
            "ac3forge_pkgconfig_libname(_w mp4_shared mp4 mp4_static "
            '"${_targets}")\n'
            "ac3forge_pkgconfig_libname(_x forge_c_shared ac3forge_c ac3forge_c_static\n"
            '    "${_targets}")\n',
            "ac3forge_pkgconfig_libname(_v iclforge_ac3_shared iclforge_ac3 iclforge_ac3_static\n"
            '    "${_targets}")\n'
            "ac3forge_pkgconfig_libname(_w iclforge_mp4_shared iclforge_mp4 iclforge_mp4_static "
            '"${_targets}")\n'
            "ac3forge_pkgconfig_libname(_x iclforge_capi_shared iclforge_c iclforge_c_static\n"
            '    "${_targets}")\n',
        )
        self.check(
            "LIBNAME ac3adm)\nLIBNAME admbridge\nLIBNAME ac4core_static)",
            "LIBNAME iclforge_adm)\nLIBNAME iclforge_admbridge\nLIBNAME iclforge_ac4core_static)",
        )

    def test_a_libname_that_is_a_variable_or_a_word_of_a_comment_is_left_alone(self) -> None:
        text = 'LIBNAME "${_pc_libname}"\n# its LIBNAME ends in _static\n'
        self.check(text, text)
        self.check("LIBNAME iclforge_adm)", "LIBNAME iclforge_adm)")

    def test_a_moved_file_is_rewritten_where_the_whole_path_appears(self) -> None:
        self.check(
            'src/forge/CMakeLists.txt "${CMAKE_SOURCE_DIR}/src/forge/src/core/fft.cpp"',
            'src/ac3/CMakeLists.txt "${CMAKE_SOURCE_DIR}/src/dsp/src/fft.cpp"',
        )
        self.check("other/src/forge/CMakeLists.txt", "other/src/forge/CMakeLists.txt")

    def test_a_directory_follows_the_directory_rule_but_not_a_longer_name(self) -> None:
        self.check(
            "PRIVATE src/forge/src/core src/forge/src/core_extra",
            "PRIVATE src/ac3/src/core src/forge/src/core_extra",
        )

    def test_directory_rules_are_derived_from_the_file_moves(self) -> None:
        moves = {
            "src/forge/src/core/a.cpp": "src/ac3/src/core/a.cpp",
            "src/forge/src/core/b.cpp": "src/ac3/src/core/b.cpp",
            "src/forge/src/mix/x.cpp": "src/ac3/src/mix/x.cpp",
            "src/forge/src/mix/w.cpp": "src/ac3/src/mix/w.cpp",
            "src/forge/src/mix/y.cpp": "src/render/src/y.cpp",
            "src/forge/src/mix/z.cpp": "src/render/src/z.cpp",
            "src/forge/src/spatial/p.cpp": "src/render/src/p.cpp",
            "src/forge/src/spatial/q.cpp": "src/render/src/q.cpp",
        }
        rules, split = n1b_cmake.dir_rules(moves)
        as_dict = dict(rules)
        self.assertEqual(as_dict["src/forge/src/core"], "src/ac3/src/core")
        # a directory whose files went two ways is reported for a person, and not rewritten
        # when 40% or more of them left
        self.assertIn("src/forge/src/mix", split)
        self.assertNotIn("src/forge/src/mix", as_dict)
        # a directory that went wholesale to a directory of another name has a rule too
        self.assertEqual(as_dict["src/forge/src/spatial"], "src/render/src")
        self.assertNotIn("src/forge/src/spatial", split)

    def test_a_directory_rule_is_applied_only_where_the_result_exists(self) -> None:
        moves = {"src/forge/src/core/a.cpp": "src/ac3/src/core/a.cpp"}
        rules, _split = n1b_cmake.dir_rules(moves)
        known = n1b_cmake.path_index(["src/ac3/src/core/a.cpp", "src/ac4core/src/dsp/qmf.cpp"])
        text = (
            "see src/forge/src/core/a.cpp and src/forge/src/core/ghost.cpp and src/forge/src/core."
        )
        out = n1b_cmake.rewrite_paths(text, moves, rules, None, known)
        self.assertEqual(
            out,
            "see src/ac3/src/core/a.cpp and src/forge/src/core/ghost.cpp and src/ac3/src/core.",
        )

    def test_a_longer_path_through_a_held_directory_keeps_its_own_rule(self) -> None:
        moves = {
            "src/forge/src/internal/profiling/on/p.hpp": "src/base/variants/profiling-on/p.hpp"
        }
        rules = [
            ("src/forge/src/internal/profiling/on", "src/base/variants/profiling-on"),
            ("src/forge/src", "src/ac3/src"),
        ]
        known = n1b_cmake.path_index([*moves.values(), "src/ac3/src/x.cpp"])
        text = (
            "dirs src/forge/src/internal and src/forge/src/internal/profiling/on and src/forge/src"
        )
        out = n1b_cmake.rewrite_paths(text, moves, rules, ["src/forge/src/internal"], known)
        self.assertEqual(
            out,
            "dirs src/forge/src/internal and src/base/variants/profiling-on and src/ac3/src",
        )

    def test_a_link_into_the_repository_on_github_follows_the_path(self) -> None:
        moves = {"src/forge/src/core/a.cpp": "src/ac3/src/core/a.cpp"}
        url = "https://github.com/x/y/blob/main/"
        out = n1b_cmake.rewrite_paths(f"[a]({url}src/forge/src/core/a.cpp)", moves, [])
        self.assertEqual(out, f"[a]({url}src/ac3/src/core/a.cpp)")

    def test_a_held_directory_is_not_carried_along_by_the_rule_of_its_parent(self) -> None:
        moves = {"src/forge/src/core/a.cpp": "src/ac3/src/core/a.cpp"}
        rules = [("src/forge/src", "src/ac3/src")]
        text = "the oba directory src/forge/src/oba and the core one src/forge/src/core"
        out = n1b_cmake.rewrite_paths(text, moves, rules, ["src/forge/src/oba"])
        self.assertEqual(
            out, "the oba directory src/forge/src/oba and the core one src/ac3/src/core"
        )


class LayeringTable(unittest.TestCase):
    TABLE = (
        '{\n  "_comment": "The table. libraries: who may include whom. layout: how a path under '
        "src/ is filed while the tree is re-laid out (planning/layout.md): `split` gives the "
        'libraries a directory holds. Both go when the re-layout is done.",\n'
        '  "libraries": {\n    "base": [],\n    "codec": ["base"]\n  },\n'
        '  "layout": {\n    "rename": {"ac3adm": "adm"},\n'
        '    "split": [["^src/forge/", "ac3"]]\n  }\n}\n'
    )

    def test_the_layout_section_and_its_sentence_go(self) -> None:
        out = n1b_cmake.retire_layout_section(self.TABLE)
        data = json.loads(out)
        self.assertEqual(list(data), ["_comment", "libraries"])
        self.assertEqual(data["libraries"], {"base": [], "codec": ["base"]})
        self.assertNotIn("layout:", data["_comment"])
        self.assertTrue(data["_comment"].startswith("The table. libraries: who may include whom."))

    def test_it_keeps_the_files_line_endings_and_a_second_run_changes_nothing(self) -> None:
        crlf = self.TABLE.replace("\n", "\r\n")
        out = n1b_cmake.retire_layout_section(crlf)
        self.assertNotIn('"layout"', out)
        self.assertNotIn("\n", out.replace("\r\n", ""))
        self.assertEqual(n1b_cmake.retire_layout_section(out), out)

    def test_a_table_without_the_section_is_left_alone(self) -> None:
        plain = '{\n  "_comment": "The table.",\n  "libraries": {"base": []}\n}\n'
        self.assertEqual(n1b_cmake.retire_layout_section(plain), plain)


class RelativeToTheBuildFile(unittest.TestCase):
    """tests/CMakeLists.txt names its files relative to tests/."""

    MOVES: ClassVar[dict[str, str]] = {
        "tests/core/test_crc16.cpp": "tests/ac3/core/test_crc16.cpp",
        "tests/core/test_fft.cpp": "tests/dsp/test_fft.cpp",
        "tests/core/avx2/absent/avx2_tier.cpp": "tests/ac3/core/avx2/absent/avx2_tier.cpp",
        "tests/core/avx2/absent/avx2_tier.hpp": "tests/ac3/core/avx2/absent/avx2_tier.hpp",
        "tests/ac3iab/test_ac3iab.cpp": "tests/iab/test_ac3iab.cpp",
        "src/forge/src/core/fft.cpp": "src/dsp/src/fft.cpp",
    }

    def rules(self):
        return n1b_cmake.relative_rules(self.MOVES, "tests")

    def check(self, before: str, after: str) -> None:
        text = n1b_cmake.transform(before, self.MOVES, None, self.rules())
        self.assertEqual(text, after)
        # a second run finds nothing more to do
        self.assertEqual(n1b_cmake.transform(text, self.MOVES, None, self.rules()), text)

    def test_only_moves_inside_the_directory_count(self) -> None:
        files, dirs = self.rules()
        self.assertEqual(files["core/test_crc16.cpp"], "ac3/core/test_crc16.cpp")
        self.assertEqual(files["ac3iab/test_ac3iab.cpp"], "iab/test_ac3iab.cpp")
        self.assertNotIn("src/forge/src/core/fft.cpp", files)
        self.assertIn(("core/avx2/absent", "ac3/core/avx2/absent"), dirs)

    def test_a_directory_of_one_component_is_not_a_rule(self) -> None:
        _files, dirs = self.rules()
        self.assertNotIn("core", [old for old, _new in dirs])

    def test_a_file_named_beside_the_build_file_follows_its_move(self) -> None:
        self.check(
            "add_executable(ac3tests\n    core/test_crc16.cpp core/test_fft.cpp\n"
            "    ac3iab/test_ac3iab.cpp)",
            "add_executable(ac3tests\n    ac3/core/test_crc16.cpp dsp/test_fft.cpp\n"
            "    iab/test_ac3iab.cpp)",
        )

    def test_a_directory_after_the_current_source_dir_follows_its_rule(self) -> None:
        self.check(
            'include_directories("${CMAKE_CURRENT_SOURCE_DIR}/core/avx2/absent")',
            'include_directories("${CMAKE_CURRENT_SOURCE_DIR}/ac3/core/avx2/absent")',
        )

    def test_a_path_that_only_ends_the_same_way_is_left_alone(self) -> None:
        self.check(
            "src/forge/src/core/test_crc16.cpp other/core/test_crc16.cpp x-core/test_crc16.cpp",
            "src/forge/src/core/test_crc16.cpp other/core/test_crc16.cpp x-core/test_crc16.cpp",
        )

    def test_the_word_core_in_a_comment_is_not_a_path(self) -> None:
        self.check("# the core tests and the core library", "# the core tests and the core library")


if __name__ == "__main__":
    unittest.main()
