"""Unit tests for n1b_reflow: which lines the namespace pass pushed past the column limit.

stdlib `unittest`; each case is a `git diff -U0` text and the lines it names. The clang-format half
runs only where the executable exists.
"""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import n1b_reflow

LONG = "x" * 101
SHORT = "x" * 100


def hunk(first: int, old: list[str], new: list[str], path: str = "a/b.cpp") -> str:
    return (
        f"--- a/{path}\n+++ b/{path}\n@@ -{first},{len(old)} +{first},{len(new)} @@\n"
        + "".join(f"-{s}\n" for s in old)
        + "".join(f"+{s}\n" for s in new)
    )


class PushedOver(unittest.TestCase):
    def test_a_line_that_grew_past_the_limit(self) -> None:
        self.assertEqual(n1b_reflow.pushed_over(hunk(7, [SHORT], [LONG]), 100), {"a/b.cpp": [7]})

    def test_a_line_that_was_long_before_is_not_the_passs(self) -> None:
        self.assertEqual(n1b_reflow.pushed_over(hunk(7, [LONG], [LONG + "y"]), 100), {})

    def test_a_line_within_the_limit_is_left_alone(self) -> None:
        self.assertEqual(n1b_reflow.pushed_over(hunk(7, ["a"], [SHORT]), 100), {})

    def test_lines_pair_by_position_in_a_hunk_and_an_added_line_has_no_old_one(self) -> None:
        diff = hunk(10, [SHORT, LONG], [LONG, LONG, LONG])
        self.assertEqual(n1b_reflow.pushed_over(diff, 100), {"a/b.cpp": [10, 12]})

    def test_the_last_hunk_of_a_file_belongs_to_that_file(self) -> None:
        diff = hunk(3, [SHORT], [LONG], "a/one.cpp") + hunk(5, [SHORT], [LONG], "a/two.cpp")
        self.assertEqual(n1b_reflow.pushed_over(diff, 100), {"a/one.cpp": [3], "a/two.cpp": [5]})

    def test_a_carriage_return_is_not_a_column(self) -> None:
        self.assertEqual(n1b_reflow.pushed_over(hunk(1, [SHORT], [SHORT + "\r"]), 100), {})


class Ranges(unittest.TestCase):
    def test_neighbours_join(self) -> None:
        self.assertEqual(n1b_reflow.ranges([9, 3, 4, 5, 3]), [(3, 5), (9, 9)])


class NewLongLines(unittest.TestCase):
    def test_a_long_line_the_file_had_is_not_new(self) -> None:
        before = f"{LONG}\nshort\n".encode()
        after = f"short\n{LONG}\n{LONG}y\n".encode()
        self.assertEqual(n1b_reflow.new_long_lines(before, after, 100), [3])


@unittest.skipUnless(Path(n1b_reflow.DEFAULT_CLANG_FORMAT).exists(), "clang-format is absent")
class WithClangFormat(unittest.TestCase):
    CALL = "int call() { return some_function(first_argument, second_argument, third_argument); }"

    def test_only_the_line_asked_about_is_wrapped(self) -> None:
        keep = "int a = 1;   // spaced as a person wrote it\n"
        old = keep + self.CALL.replace("third_argument", "third") + "\n"
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / ".clang-format").write_text("BasedOnStyle: Google\nColumnLimit: 80\n")
            git = ["git", "-C", tmp]
            subprocess.run([*git, "init", "-q"], check=True)
            subprocess.run([*git, "config", "user.email", "t@example.com"], check=True)
            subprocess.run([*git, "config", "user.name", "t"], check=True)
            (root / "x.cpp").write_text(old, newline="\n")
            subprocess.run([*git, "add", "."], check=True)
            subprocess.run([*git, "commit", "-q", "-m", "one"], check=True)
            (root / "x.cpp").write_text(old.replace("third", "third_argument"), newline="\n")
            script = str(Path(n1b_reflow.__file__))
            args = [sys.executable, script, "--root", tmp, "--limit", "80"]
            done = subprocess.run(args, capture_output=True, text=True, check=True)
            text = (root / "x.cpp").read_text()
        self.assertIn("1 lines over 80 columns in 1 files; reflowed 1 files", done.stdout)
        self.assertTrue(text.startswith(keep))
        self.assertTrue(all(len(line) <= 80 for line in text.splitlines()))


if __name__ == "__main__":
    unittest.main()
