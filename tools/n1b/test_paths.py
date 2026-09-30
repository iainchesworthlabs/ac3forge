"""Unit tests for n1b_paths.py: the paths that pages, comments, strings and scripts name.

stdlib `unittest`. A small git repository holds one file of each kind the pass treats differently
(a page, a C++ source with a path in a comment and in a string, a workflow, the history that must
keep the old layout, a byte-exact fixture, a binary file), the files have already moved the way a
plan says, and the pass is run over it.
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

import n1b_paths

OLD_A = "src/forge/src/core/mdct.cpp"
NEW_A = "src/ac3/src/core/mdct.cpp"
OLD_B = "src/forge/src/dsp/fft.cpp"
NEW_B = "src/dsp/src/fft.cpp"
MOVES = {
    OLD_A: NEW_A,
    OLD_B: NEW_B,
    "src/forge/src/core/bitalloc.cpp": "src/ac3/src/core/bitalloc.cpp",
    "src/forge/src/core/exponents.cpp": "src/ac3/src/core/exponents.cpp",
    "src/forge/src/oba/scene.cpp": "src/objects/src/scene.cpp",
    "src/forge/src/oba/motion.cpp": "src/objects/src/motion.cpp",
    "src/forge/src/oba/joc.cpp": "src/ac3/src/oba/joc.cpp",
    "src/forge/src/oba/atmos.cpp": "src/ac3/src/oba/atmos.cpp",
}
FILES = {
    NEW_A: "// mdct\n",
    NEW_B: "// fft\n",
    "src/ac3/src/core/bitalloc.cpp": "// bitalloc\n",
    "src/ac3/src/core/exponents.cpp": "// exponents\n",
    "src/objects/src/scene.cpp": "// scene\n",
    "src/objects/src/motion.cpp": "// motion\n",
    "src/ac3/src/oba/joc.cpp": "// joc\n",
    "src/ac3/src/oba/atmos.cpp": "// atmos\n",
    "src/ac3/src/use.cpp": (
        f"// see {OLD_A}: the transform, and {OLD_B}.\n"
        f'const char* kSelf = "{OLD_A}";\n'
        "// other/" + OLD_A + " is a different file, and src/forge/src/core/mdct.cpp.bak too.\n"
    ),
    "docs/library/page.md": f"The transform is in `{OLD_A}`; see [it](../../{OLD_A}).\n",
    ".github/workflows/ci.yml": f"      - '{OLD_B}'\n      - 'src/forge/src/core/**'\n",
    "docs/oba.md": "Objects: `src/forge/src/oba`\n",
    "tools/generators/gen.py": (
        'OUT = REPO / "src" / "forge" / "src" / "dsp" / "fft.cpp"\n'
        'HERE = REPO / "src" / "forge" / "src" / "oba"\n'
        'KEPT = REPO / "src" / "ac3" / "src"\n'
    ),
    "docs/notes.md": 'not python: REPO / "src" / "forge" / "src" / "dsp" / "fft.cpp"\n',
    "docs/abi.md": (
        "Allowlists: `tools/ci/abi-allowlist/libac4enc.so.txt` and\n"
        "tools/ci/abi-allowlist/libac3forge.so.txt. The libac4.so.txt name alone is no path.\n"
    ),
    "CHANGELOG.md": f"- moved {OLD_A}\n",
    "planning/layout.md": f"the study read {OLD_A}\n",
    "tools/n1b/notes.py": f'OLD = "{OLD_A}"\n',
    "tests/golden/expected.txt": f"{OLD_A}\n",
    "docs/logo.png": "\x89PNG\r\n\x1a\n\0" + OLD_A,
}


def git(root: Path, *args: str) -> str:
    return subprocess.run(
        ["git", "-C", str(root), *args], capture_output=True, text=True, check=True
    ).stdout


class Paths(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name) / "repo"
        self.root.mkdir()
        self.addCleanup(self._tmp.cleanup)
        for rel, text in FILES.items():
            path = self.root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(text.encode("latin-1" if rel.endswith(".png") else "utf-8"))
        git(self.root, "init", "-q")
        git(self.root, "config", "user.name", "t")
        git(self.root, "config", "user.email", "t@example.invalid")
        git(self.root, "config", "core.autocrlf", "false")
        git(self.root, "add", "-A")
        git(self.root, "commit", "-q", "-m", "fixture")
        self.plan = Path(self._tmp.name) / "plan.json"
        self.plan.write_text(json.dumps({"moves": MOVES}), encoding="utf-8")

    def run_pass(self, dry_run: bool = False) -> tuple[int, dict[str, list[str]]]:
        with contextlib.redirect_stdout(io.StringIO()):
            return n1b_paths.run(self.root, self.plan, dry_run)

    def text(self, rel: str) -> str:
        return (self.root / rel).read_text(encoding="utf-8")

    def test_a_comment_a_string_a_page_and_a_workflow_follow_the_move(self) -> None:
        self.run_pass()
        use = self.text("src/ac3/src/use.cpp")
        self.assertIn(f"// see {NEW_A}: the transform, and {NEW_B}.", use)
        self.assertIn(f'const char* kSelf = "{NEW_A}";', use)
        self.assertIn(NEW_A, self.text("docs/library/page.md"))
        self.assertIn(f"'{NEW_B}'", self.text(".github/workflows/ci.yml"))

    def test_a_relative_link_keeps_its_prefix_and_gets_the_new_path(self) -> None:
        self.run_pass()
        self.assertIn(f"(../../{NEW_A})", self.text("docs/library/page.md"))

    def test_a_path_inside_another_path_is_left_alone(self) -> None:
        self.run_pass()
        self.assertIn("other/" + OLD_A, self.text("src/ac3/src/use.cpp"))

    def test_the_full_stop_that_ends_a_sentence_does_not_stop_the_file_rule(self) -> None:
        # `.../dsp/fft.cpp.` is the moved file and a full stop, not a file with another suffix; had
        # the file rule missed it, the directory rule for src/forge/src would have made
        # `src/ac3/src/dsp/fft.cpp.`, a path that is nowhere.
        self.run_pass()
        self.assertIn(f"and {NEW_B}.\n", self.text("src/ac3/src/use.cpp"))

    def test_a_path_that_is_no_file_of_the_tree_is_not_carried_to_a_place_it_does_not_exist(
        self,
    ) -> None:
        # mdct.cpp.bak was never tracked, so `src/ac3/src/core/mdct.cpp.bak` would be a path that
        # is nowhere: the directory rule is applied only where the result exists.
        self.run_pass()
        self.assertIn("src/forge/src/core/mdct.cpp.bak too", self.text("src/ac3/src/use.cpp"))

    def test_the_history_the_scripts_and_the_byte_exact_files_are_not_touched(self) -> None:
        self.run_pass()
        for kept in (
            "CHANGELOG.md",
            "planning/layout.md",
            "tools/n1b/notes.py",
            "tests/golden/expected.txt",
        ):
            self.assertIn(OLD_A, self.text(kept), kept)

    def test_a_binary_file_is_skipped_even_when_it_holds_a_path(self) -> None:
        self.run_pass()
        self.assertIn(OLD_A.encode(), (self.root / "docs/logo.png").read_bytes())

    def test_a_directory_whose_files_split_is_left_and_reported(self) -> None:
        changed, hits = self.run_pass()
        self.assertGreater(changed, 0)
        self.assertEqual(self.text("docs/oba.md"), "Objects: `src/forge/src/oba`\n")
        self.assertIn("src/forge/src/oba", hits)
        self.assertIn("docs/oba.md", hits["src/forge/src/oba"])

    def test_a_path_a_python_file_builds_from_its_components_follows_the_move(self) -> None:
        self.run_pass()
        gen = self.text("tools/generators/gen.py")
        self.assertIn('OUT = REPO / "src" / "dsp" / "src" / "fft.cpp"\n', gen)
        self.assertIn('KEPT = REPO / "src" / "ac3" / "src"\n', gen)
        # the same words in a page are not a path chain
        self.assertIn('"src" / "forge" / "src" / "dsp"', self.text("docs/notes.md"))

    def test_a_file_the_hand_written_part_renames_is_followed_where_a_page_names_it(self) -> None:
        self.run_pass()
        text = self.text("docs/abi.md")
        self.assertIn("`tools/ci/abi-allowlist/libiclforge_ac4enc.so.txt` and\n", text)
        self.assertIn("tools/ci/abi-allowlist/libiclforge_ac3.so.txt. The", text)
        self.assertIn("The libac4.so.txt name alone", text)

    def test_a_chain_that_names_a_split_directory_is_left_and_reported(self) -> None:
        _, hits = self.run_pass()
        gen = self.text("tools/generators/gen.py")
        self.assertIn('HERE = REPO / "src" / "forge" / "src" / "oba"\n', gen)
        self.assertIn("tools/generators/gen.py", hits["src/forge/src/oba"])

    def test_a_directory_most_of_whose_files_stayed_together_follows(self) -> None:
        self.run_pass()
        self.assertIn("'src/ac3/src/core/**'", self.text(".github/workflows/ci.yml"))

    def test_a_dry_run_writes_nothing_and_a_second_run_changes_nothing(self) -> None:
        changed, _ = self.run_pass(dry_run=True)
        self.assertGreater(changed, 0)
        self.assertIn(OLD_A, self.text("src/ac3/src/use.cpp"))
        self.run_pass()
        self.assertEqual(self.run_pass()[0], 0)


if __name__ == "__main__":
    unittest.main()
