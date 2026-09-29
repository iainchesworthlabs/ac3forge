"""Unit tests for the re-layout's two text rewrites: names (n1b_names), build files (n1b_cmake).

stdlib `unittest`; each case is a line of C++ or CMake and what it becomes. What they pin is the
contract the migration relies on: the family root replaces `ac3` wherever it is a namespace or a
qualifier, and nowhere it is part of another name (`eac3::`, `foo::ac3::`); an include spelling is
left to n1b_apply; and a build file's target names, output names and moved paths follow the move
map without touching a path that only ends the same way.
"""

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
            "src/forge/src/mix/y.cpp": "src/render/src/y.cpp",
            "src/forge/src/mix/z.cpp": "src/render/src/z.cpp",
        }
        rules, split = n1b_cmake.dir_rules(moves)
        as_dict = dict(rules)
        self.assertEqual(as_dict["src/forge/src/core"], "src/ac3/src/core")
        # a directory whose files went two ways is reported for a person, and not rewritten
        # when 40% or more of them left
        self.assertIn("src/forge/src/mix", split)
        self.assertNotIn("src/forge/src/mix", as_dict)


if __name__ == "__main__":
    unittest.main()
