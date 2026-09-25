"""Tests for resolve_history_conflict.py.

The unit tests cover the two rules the script decides by; the last test drives
a real rebase conflict through git, which is the shape the trend publishers hit
on main (see the script's own header).
"""

from __future__ import annotations

import contextlib
import io
import json
import os
import shutil
import subprocess
import tempfile
import unittest
import unittest.mock as mock
from pathlib import Path

import resolve_history_conflict as resolver
from append_quality_history import RECENT_WINDOW_COMMITS, write_recent_window

HERE = Path(__file__).resolve().parent


def row(commit: str, leg: str = "linux-gcc") -> str:
    """One history record, in the shape the append producers write."""
    return json.dumps({"commit": commit, "branch": "main", "leg": leg, "snr_db": 67.8})


class UnionRule(unittest.TestCase):
    def test_keeps_both_sides_records(self) -> None:
        base = [row("a")]
        merged, added = resolver.union(base, [*base, row("b")], [*base, row("c")])
        self.assertEqual(merged, [row("a"), row("b"), row("c")])
        self.assertEqual(added, [row("c")])

    def test_side_already_on_the_branch_comes_first(self) -> None:
        merged, _ = resolver.union([], [row("b")], [row("c")])
        self.assertEqual(merged, [row("b"), row("c")])

    def test_a_record_on_both_sides_is_kept_once(self) -> None:
        merged, added = resolver.union([], [row("b")], [row("b"), row("c")])
        self.assertEqual(merged, [row("b"), row("c")])
        self.assertEqual(added, [row("c")])

    def test_a_record_the_other_side_removed_stays_removed(self) -> None:
        merged, added = resolver.union([row("a")], [], [row("a"), row("c")])
        self.assertEqual(merged, [row("c")])
        self.assertEqual(added, [row("c")])


class AddedRecordsAreChecked(unittest.TestCase):
    def test_json_that_will_not_parse_is_refused(self) -> None:
        with self.assertRaises(resolver.Unresolvable):
            resolver.checked(["not json"], path="main.jsonl")

    def test_a_record_without_a_commit_is_refused(self) -> None:
        with self.assertRaises(resolver.Unresolvable):
            resolver.checked([json.dumps({"branch": "main"})], path="main.jsonl")

    def test_a_record_with_a_commit_passes(self) -> None:
        self.assertEqual(resolver.checked([row("a")], path="main.jsonl"), [row("a")])


@unittest.skipIf(shutil.which("git") is None, "git is not on PATH")
class ARealRebaseConflict(unittest.TestCase):
    """Two runs of one publisher, each adding its own commit's records."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory(ignore_cleanup_errors=True)
        self.repo = Path(self.tmp.name) / "history"
        self.repo.mkdir()
        self.git("init", "-b", "quality-history")
        self.git("config", "user.name", "test")
        self.git("config", "user.email", "test@example.com")
        # More commits than the sidecar window, so the window is written at all.
        self.history = self.repo / "main.jsonl"
        self.base_rows = [row(f"{number:040x}") for number in range(RECENT_WINDOW_COMMITS)]
        self.write(self.base_rows)
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "base")
        self.git("branch", "already-on-the-branch")

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def git(self, *args: str) -> str:
        result = subprocess.run(["git", *args], cwd=self.repo, check=False,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if result.returncode != 0:
            self.fail(f"git {' '.join(args)} failed:\n{result.stdout}")
        return result.stdout

    def write(self, rows: list[str]) -> None:
        self.history.write_text("\n".join(rows) + "\n", encoding="utf-8")
        write_recent_window(self.history)

    def append_as_a_run(self, commit: str, message: str) -> None:
        """One publisher run: add this commit's record and the window with it."""
        rows = [line for line in self.history.read_text(encoding="utf-8").splitlines() if line]
        self.write([*rows, row(commit)])
        self.git("add", "-A")
        self.git("commit", "-q", "-m", message)

    def resolve(self) -> subprocess.CompletedProcess[str]:
        env = dict(os.environ, PYTHONPATH=str(HERE))
        return subprocess.run(
            ["python3" if os.name != "nt" else "python",
             str(HERE / "resolve_history_conflict.py"), "--history-dir", "."],
            cwd=self.repo, check=False, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, env=env,
        )

    def test_both_runs_records_survive_and_the_window_is_rewritten(self) -> None:
        self.git("checkout", "-q", "already-on-the-branch")
        self.append_as_a_run("a" * 40, "the run that pushed first")
        self.git("checkout", "-q", "quality-history")
        self.append_as_a_run("b" * 40, "the run replaying onto it")

        rebase = subprocess.run(["git", "rebase", "already-on-the-branch"], cwd=self.repo,
                                check=False, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True)
        self.assertNotEqual(rebase.returncode, 0, "expected the rebase to conflict")
        self.assertIn("main.jsonl", rebase.stdout)

        resolved = self.resolve()
        self.assertEqual(resolved.returncode, 0, resolved.stdout)
        self.git("-c", "core.editor=true", "rebase", "--continue")

        rows = [line for line in self.history.read_text(encoding="utf-8").splitlines() if line]
        self.assertEqual(rows, [*self.base_rows, row("a" * 40), row("b" * 40)])
        self.assertEqual(self.git("status", "--porcelain").strip(), "")
        self.assertFalse((self.repo / ".git" / "rebase-merge").exists())

        # The window is what a single run writing that file would have left.
        expected = Path(self.tmp.name) / "expected.jsonl"
        expected.write_text("\n".join(rows) + "\n", encoding="utf-8")
        write_recent_window(expected)
        self.assertEqual((self.repo / "main.recent.jsonl").read_text(encoding="utf-8"),
                         expected.with_suffix(".recent.jsonl").read_text(encoding="utf-8"))

    def test_a_file_each_run_overwrites_takes_the_replayed_run(self) -> None:
        image = self.repo / "spectrograms" / "latest.png"
        image.parent.mkdir()
        image.write_bytes(b"base")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "an image both runs overwrite")
        self.git("branch", "-f", "already-on-the-branch")

        self.git("checkout", "-q", "already-on-the-branch")
        image.write_bytes(b"the run that pushed first")
        self.append_as_a_run("a" * 40, "the run that pushed first")
        self.git("checkout", "-q", "quality-history")
        image.write_bytes(b"the run replaying onto it")
        self.append_as_a_run("b" * 40, "the run replaying onto it")

        subprocess.run(["git", "rebase", "already-on-the-branch"], cwd=self.repo, check=False,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        resolved = self.resolve()
        self.assertEqual(resolved.returncode, 0, resolved.stdout)
        self.git("-c", "core.editor=true", "rebase", "--continue")
        self.assertEqual(image.read_bytes(), b"the run replaying onto it")

    def test_a_rebase_that_stopped_for_another_reason_is_refused(self) -> None:
        resolved = self.resolve()
        self.assertEqual(resolved.returncode, 1)
        self.assertIn("no unmerged path", resolved.stdout)


class FakeGit:
    """In-process stand-in for the three git calls the resolver makes, so
    main() and every refusal path run without a real rebase."""

    def __init__(self, unmerged, stages, fail=None):
        self.unmerged = unmerged
        self.stages = stages          # {(number, path): bytes}
        self.fail = fail or set()
        self.calls = []

    def __call__(self, cmd, cwd=None, check=False, stdout=None, stderr=None):
        self.calls.append(cmd[1:])
        verb = cmd[1]
        if verb in self.fail:
            return subprocess.CompletedProcess(cmd, 128, b"", b"fatal: boom")
        if verb == "diff":
            return subprocess.CompletedProcess(cmd, 0, "\0".join(self.unmerged).encode(), b"")
        if verb == "show":
            number, path = cmd[2][1:].split(":", 1)
            content = self.stages.get((int(number), path))
            if content is None:
                return subprocess.CompletedProcess(cmd, 128, b"", b"")
            return subprocess.CompletedProcess(cmd, 0, content, b"")
        if verb == "add":
            return subprocess.CompletedProcess(cmd, 0, b"", b"")
        raise AssertionError(cmd)


class MainInProcess(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def run_main(self, fake):
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(resolver.subprocess, "run", fake), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = resolver.main(["--history-dir", str(self.dir)])
        return rc, out.getvalue(), err.getvalue()

    def test_union_other_and_orphan_sidecar(self):
        base, ours, theirs = row("a" * 40), row("b" * 40), row("c" * 40)
        fake = FakeGit(
            ["main.jsonl", "badge.json", "memory-main.recent.jsonl"],
            {(1, "main.jsonl"): (base + "\n").encode(),
             (2, "main.jsonl"): (base + "\n" + ours + "\n").encode(),
             (3, "main.jsonl"): (base + "\n" + theirs + "\n\n").encode(),
             (2, "badge.json"): b"old", (3, "badge.json"): b"new"})
        rc, out, err = self.run_main(fake)
        self.assertEqual(rc, 0, err)
        self.assertEqual((self.dir / "main.jsonl").read_text().splitlines(), [base, ours, theirs])
        self.assertEqual((self.dir / "badge.json").read_bytes(), b"new")
        self.assertIn("added 1 from this run", out)
        self.assertIn("Resolved 3 conflicted path(s)", out)
        self.assertEqual(fake.calls[-1], ["add", "-A", "--", "."])

    def test_other_file_falls_back_to_branch_side(self):
        fake = FakeGit(["deleted-by-run.json"], {(2, "deleted-by-run.json"): b"kept"})
        rc, _, _ = self.run_main(fake)
        self.assertEqual(rc, 0)
        self.assertEqual((self.dir / "deleted-by-run.json").read_bytes(), b"kept")

    def test_other_file_with_no_side_is_refused(self):
        rc, _, err = self.run_main(FakeGit(["x.json"], {}))
        self.assertEqual(rc, 1)
        self.assertIn("x.json has no side to take", err)

    def test_jsonl_missing_a_side_is_refused(self):
        rc, _, err = self.run_main(FakeGit(["main.jsonl"], {(2, "main.jsonl"): b""}))
        self.assertEqual(rc, 1)
        self.assertIn("missing from one side", err)

    def test_malformed_added_line_is_refused_and_nothing_written(self):
        fake = FakeGit(["main.jsonl"], {(2, "main.jsonl"): b"", (3, "main.jsonl"): b"{not json"})
        rc, _, err = self.run_main(fake)
        self.assertEqual(rc, 1)
        self.assertIn("is not JSON", err)
        self.assertFalse((self.dir / "main.jsonl").exists())

    def test_git_failure_is_reported(self):
        rc, _, err = self.run_main(FakeGit([], {}, fail={"diff"}))
        self.assertEqual(rc, 1)
        self.assertIn("git diff --name-only --diff-filter=U -z failed: fatal: boom", err)

    def test_nothing_unmerged_is_refused(self):
        rc, _, err = self.run_main(FakeGit([], {}))
        self.assertEqual(rc, 1)
        self.assertIn("no unmerged path", err)


if __name__ == "__main__":
    unittest.main()
