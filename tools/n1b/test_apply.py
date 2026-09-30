"""Unit tests for n1b_apply.py: the moves of stage S2 and the include spellings that follow them.

stdlib `unittest`. Each case builds a small git repository holding a few files of every kind the
script treats differently (a library of the split, a library that is not split, tests of
several libraries in one directory, a generator that emits an include, a page that quotes the old
layout), runs the script over it, and looks at where the files went and what their includes say.
The miniature is written here, not copied from the tree, so the tests keep passing once the real
tree has been moved.
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

import n1b_apply
from include_graph import build_index
from n1b_lib import Repo

FILES = {
    "src/forge/include/ac3/core/bitwriter.hpp": "#pragma once\n",
    "src/forge/include/ac3/core/crc16.hpp": "#pragma once\n",
    "src/forge/include/ac3/dsp/qmf.hpp": "#pragma once\n",
    "src/mp4/include/mp4/mp4.hpp": "#pragma once\n",
    "tests/core/helper.hpp": "#pragma once\n",
    "tests/core/test_bitwriter.cpp": '#include "ac3/core/bitwriter.hpp"\n#include "helper.hpp"\n',
    "tests/core/test_crc16.cpp": '#include "ac3/core/crc16.hpp"\n#include "helper.hpp"\n',
    "tests/containers/test_mp4_reader.cpp": '#include "mp4/mp4.hpp"\n',
    "tests/containers/test_no_include.cpp": "int x;\n",
    "tests/dsp/test_qmf.cpp": '#include "ac3/dsp/qmf.hpp"\n',
    "rust/ac3forge/src/lib.rs": "// rust\n",
    "esp-idf/ac3forge/include/ac3forge/player.hpp": "#pragma once\n",
    "esp-idf/ac3forge/include/ac3forge/interleave.hpp": '#include "ac3forge/slot_conversion.hpp"\n',
    "esp-idf/ac3forge/conversion/bits/ac3forge/slot_conversion.hpp": "#pragma once\n",
    "src/sendspin/include/iclforge/sendspin/ac3forge_player.hpp": "#pragma once\n",
    "apps/hearth/testsink/sink.cpp": '#include "iclforge/sendspin/ac3forge_player.hpp"\n',
    "packaging/winget/manifests/i/iainchesworthlabs/ac3forge/0.10.0-beta.1/x.yaml": "x\n",
    "tools/generators/gen.py": 'TEMPLATE = """\n#include "ac3/core/crc16.hpp"\n"""\n',
    "tools/packaging/pack.py": "lines.append('#include \"ac3/core/crc16.hpp\"')\n",
    "tools/n1b/notes.py": 'OLD = """\n#include "ac3/core/crc16.hpp"\n"""\n',
    "planning/layout.md": '```cpp\n#include "ac3/core/crc16.hpp"\n```\n',
    "planning/other-plan.md": 'Written when it was `#include "ac3/core/crc16.hpp"`.\n',
    "docs/library/page.md": "```cpp\n#include <ac3/core/crc16.hpp>\n```\n",
}


def git(root: Path, *args: str) -> str:
    return subprocess.run(
        ["git", "-C", str(root), *args], capture_output=True, text=True, check=True
    ).stdout


class Fixture(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)
        for rel, text in FILES.items():
            path = self.root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8", newline="\n")
        git(self.root, "init", "-q")
        git(self.root, "config", "user.name", "t")
        git(self.root, "config", "user.email", "t@example.invalid")
        git(self.root, "config", "core.autocrlf", "false")
        git(self.root, "add", "-A")
        git(self.root, "commit", "-q", "-m", "fixture")

    def run_script(self, scope: str, phase: str) -> dict:
        with contextlib.redirect_stdout(io.StringIO()):
            return n1b_apply.run(self.root, n1b_apply.parse_scope(scope), phase)

    def text(self, rel: str) -> str:
        return (self.root / rel).read_text(encoding="utf-8")


class Scope(unittest.TestCase):
    def test_a_list_and_all(self) -> None:
        self.assertEqual(n1b_apply.parse_scope("src,tests"), {"src", "tests"})
        self.assertEqual(n1b_apply.parse_scope("all"), {"src", "tests", "packages"})
        self.assertEqual(n1b_apply.parse_scope(n1b_apply.DEFAULT_SCOPE), {"src", "tests"})

    def test_an_unknown_scope_or_an_empty_one_is_refused(self) -> None:
        for bad in ("docs", "src,docs", ""):
            with self.subTest(bad=bad), self.assertRaises(SystemExit):
                n1b_apply.parse_scope(bad)


class Moves(Fixture):
    def moves(self, scope: str) -> dict[str, str]:
        return self.run_script(scope, "plan")["moves"]

    def test_the_scope_decides_what_moves(self) -> None:
        src, tests, packages = self.moves("src"), self.moves("tests"), self.moves("packages")
        self.assertIn("src/forge/include/ac3/core/bitwriter.hpp", src)
        self.assertFalse([old for old in src if old.startswith("tests/")])
        self.assertTrue(tests)
        self.assertFalse([old for old in tests if not old.startswith("tests/")])
        self.assertEqual(
            packages,
            {
                "rust/ac3forge/src/lib.rs": "rust/iclforge/src/lib.rs",
                "esp-idf/ac3forge/include/ac3forge/player.hpp": (
                    "esp-idf/iclforge/include/iclforge/player.hpp"
                ),
                "esp-idf/ac3forge/include/ac3forge/interleave.hpp": (
                    "esp-idf/iclforge/include/iclforge/interleave.hpp"
                ),
                "esp-idf/ac3forge/conversion/bits/ac3forge/slot_conversion.hpp": (
                    "esp-idf/iclforge/conversion/bits/iclforge/slot_conversion.hpp"
                ),
                "src/sendspin/include/iclforge/sendspin/ac3forge_player.hpp": (
                    "src/sendspin/include/iclforge/sendspin/iclforge_player.hpp"
                ),
            },
        )

    def test_a_test_goes_with_the_library_whose_files_it_includes_most(self) -> None:
        tests = self.moves("tests")
        self.assertEqual(tests["tests/core/test_bitwriter.cpp"], "tests/base/test_bitwriter.cpp")
        self.assertEqual(
            tests["tests/containers/test_mp4_reader.cpp"], "tests/mp4/test_mp4_reader.cpp"
        )

    def test_a_test_of_the_codec_proper_stays_in_its_directory_under_ac3(self) -> None:
        tests = self.moves("tests")
        self.assertEqual(tests["tests/core/test_crc16.cpp"], "tests/ac3/core/test_crc16.cpp")
        self.assertEqual(tests["tests/core/helper.hpp"], "tests/ac3/core/helper.hpp")

    def test_a_test_directory_named_for_its_library_does_not_move(self) -> None:
        self.assertNotIn("tests/dsp/test_qmf.cpp", self.moves("tests"))

    def test_a_container_test_that_includes_nothing_stays_where_it_is(self) -> None:
        self.assertNotIn("tests/containers/test_no_include.cpp", self.moves("tests"))

    def test_the_libraries_a_test_is_counted_for_are_the_mixed_directories_only(self) -> None:
        repo = Repo(str(self.root))
        index, _ = build_index(repo)
        libs = n1b_apply.test_libraries(repo, index)
        self.assertEqual(libs["tests/core/test_bitwriter.cpp"], "base")
        self.assertEqual(libs["tests/containers/test_mp4_reader.cpp"], "mp4")
        self.assertNotIn("tests/core/test_crc16.cpp", libs)  # ac3 is the directory's own
        self.assertNotIn("tests/dsp/test_qmf.cpp", libs)


class Includes(Fixture):
    def test_a_helper_left_behind_is_named_by_a_relative_path(self) -> None:
        self.run_script("src,tests", "all")
        self.assertEqual(
            self.text("tests/base/test_bitwriter.cpp"),
            '#include "iclforge/base/bitwriter.hpp"\n#include "../ac3/core/helper.hpp"\n',
        )

    def test_a_helper_that_moved_with_its_test_keeps_its_spelling(self) -> None:
        self.run_script("src,tests", "all")
        self.assertEqual(
            self.text("tests/ac3/core/test_crc16.cpp"),
            '#include "iclforge/ac3/core/crc16.hpp"\n#include "helper.hpp"\n',
        )

    def test_a_generator_and_a_page_follow_but_the_history_does_not(self) -> None:
        self.run_script("src,tests", "all")
        for followed in ("tools/generators/gen.py", "tools/packaging/pack.py"):
            self.assertIn("iclforge/ac3/core/crc16.hpp", self.text(followed), followed)
        self.assertIn("iclforge/ac3/core/crc16.hpp", self.text("docs/library/page.md"))
        for kept in ("tools/n1b/notes.py", "planning/layout.md", "planning/other-plan.md"):
            self.assertIn('"ac3/core/crc16.hpp"', self.text(kept), kept)

    def test_an_include_named_inside_a_string_or_a_code_span_is_rewritten(self) -> None:
        self.run_script("src,tests", "all")
        self.assertEqual(
            self.text("tools/packaging/pack.py"),
            "lines.append('#include \"iclforge/ac3/core/crc16.hpp\"')\n",
        )

    def test_the_moves_are_git_renames_and_the_edits_wait_in_the_working_tree(self) -> None:
        self.run_script("src,tests", "all")
        staged = git(self.root, "diff", "--cached", "--name-status", "-M").splitlines()
        self.assertTrue(staged and all(line.startswith("R100") for line in staged), staged)
        waiting = git(self.root, "diff", "--name-only").splitlines()
        self.assertIn("tests/base/test_bitwriter.cpp", waiting)

    def test_a_second_run_on_the_finished_tree_plans_nothing(self) -> None:
        self.run_script("src,tests", "all")
        git(self.root, "add", "-A")
        git(self.root, "commit", "-q", "-m", "moved")
        plan = self.run_script("src,tests", "plan")
        self.assertEqual(plan["moves"], {})
        self.assertEqual(plan["edits"], {})

    def test_the_package_moves_carry_their_includes_and_leave_the_released_manifests(self) -> None:
        self.run_script("packages", "all")
        self.assertEqual(
            self.text("apps/hearth/testsink/sink.cpp"),
            '#include "iclforge/sendspin/iclforge_player.hpp"\n',
        )
        self.assertEqual(
            self.text("esp-idf/iclforge/include/iclforge/interleave.hpp"),
            '#include "iclforge/slot_conversion.hpp"\n',
        )
        released = "packaging/winget/manifests/i/iainchesworthlabs/ac3forge/0.10.0-beta.1/x.yaml"
        self.assertTrue((self.root / released).exists())

    def test_a_second_package_run_on_the_finished_tree_plans_nothing(self) -> None:
        self.run_script("packages", "all")
        git(self.root, "add", "-A")
        git(self.root, "commit", "-q", "-m", "moved")
        plan = self.run_script("packages", "plan")
        self.assertEqual(plan["moves"], {})
        self.assertEqual(plan["edits"], {})

    def test_the_plan_file_carries_the_moves_and_the_spelling_map(self) -> None:
        out = self.root.parent / (self.root.name + "-plan.json")
        self.addCleanup(lambda: out.unlink(missing_ok=True))
        with contextlib.redirect_stdout(io.StringIO()):
            n1b_apply.run(self.root, {"src", "tests"}, "plan", str(out))
        plan = json.loads(out.read_text(encoding="utf-8"))
        self.assertEqual(plan["scope"], ["src", "tests"])
        self.assertEqual(plan["spellings"]["ac3/core/crc16.hpp"], "iclforge/ac3/core/crc16.hpp")
        self.assertIn("tests/core/test_bitwriter.cpp", plan["moves"])


if __name__ == "__main__":
    unittest.main()
