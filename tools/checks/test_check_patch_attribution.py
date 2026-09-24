"""Unit tests for check_patch_attribution.py, the AI-attribution/identity check.

stdlib `unittest`, matching every other script in this directory. The
`--patch` cases build a synthetic `git format-patch` mbox by hand, so they
need no git repo and always run. The `--range` cases drive a real temporary
git repo (skipped if `git` is not on PATH, the same guard
test_resolve_history_conflict.py uses) - that is the only way to get a real
committer identity distinct from the author, or a real merge commit to prove
`--no-merges` actually excludes it.

Run: python3 -m unittest discover -s tools/checks -p 'test_*.py'
"""

from __future__ import annotations

import contextlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_patch_attribution as cpa

AUTHOR_EMAIL = "iain.chesworth@gmail.com"
AUTHOR = f"Iain Chesworth <{AUTHOR_EMAIL}>"


def _patch_chunk(
    subject: str, body: str = "", *, author: str = AUTHOR, sha: str = "1" * 40, seq: str = "1/1"
) -> str:
    lines = [
        f"From {sha} Mon Sep 17 00:00:00 2001",
        f"From: {author}",
        "Date: Thu, 24 Sep 2026 08:14:06 +0000",
        f"Subject: [PATCH {seq}] {subject}",
        "",
    ]
    lines.extend(body.splitlines())
    lines += [
        "---",
        " some/file.txt | 1 +",
        " 1 file changed, 1 insertion(+)",
        "",
        "diff --git a/some/file.txt b/some/file.txt",
        "index 0000000..1111111 100644",
        "--- a/some/file.txt",
        "+++ b/some/file.txt",
        "@@ -0,0 +1 @@",
        "+hello",
        "-- ",
        "2.43.0",
        "",
    ]
    return "\n".join(lines)


def _write_patch(root: Path, *chunks: str) -> Path:
    path = root / "series.patch"
    path.write_text("".join(chunks), encoding="utf-8")
    return path


class PatchModeChecks(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(ignore_cleanup_errors=True)
        self.root = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def _run(self, *extra_args: str) -> tuple[int, str]:
        argv = ["check_patch_attribution.py", *extra_args]
        buffer = io.StringIO()
        original_argv = sys.argv
        sys.argv = argv
        try:
            with contextlib.redirect_stdout(buffer):
                code = cpa.main()
        finally:
            sys.argv = original_argv
        return code, buffer.getvalue()

    def _run_patch(
        self, path: Path, expect_name: str = "Iain Chesworth", expect_email: str = AUTHOR_EMAIL
    ) -> tuple[int, str]:
        return self._run(
            "--patch", str(path), "--expect-name", expect_name, "--expect-email", expect_email
        )

    def test_clean_single_commit_patch_passes(self) -> None:
        body = "A plain, unremarkable commit body.\n"
        patch = _write_patch(self.root, _patch_chunk("feat: add a thing", body))
        code, out = self._run_patch(patch)
        self.assertEqual(code, 0, out)
        self.assertIn("1 commit(s) checked, 0 violation(s)", out)

    def test_multi_commit_patch_all_clean_counts_each(self) -> None:
        patch = _write_patch(
            self.root,
            _patch_chunk("feat: part one", "First half.\n", sha="1" * 40, seq="1/2"),
            _patch_chunk("feat: part two", "Second half.\n", sha="2" * 40, seq="2/2"),
        )
        code, out = self._run_patch(patch)
        self.assertEqual(code, 0, out)
        self.assertIn("2 commit(s) checked, 0 violation(s)", out)

    def test_coauthored_by_trailer_fails(self) -> None:
        body = "A normal summary.\n\nCo-Authored-By: Claude <noreply@anthropic.com>\n"
        patch = _write_patch(self.root, _patch_chunk("feat: add a thing", body))
        code, out = self._run_patch(patch)
        self.assertEqual(code, 1)
        self.assertIn("a Co-authored-by trailer", out)

    def test_generated_with_claude_line_fails(self) -> None:
        body = "A normal summary.\n\n\U0001f916 Generated with [Claude Code](https://claude.com/claude-code)\n"
        patch = _write_patch(self.root, _patch_chunk("feat: add a thing", body))
        code, out = self._run_patch(patch)
        self.assertEqual(code, 1)
        self.assertIn("Generated with Claude Code", out)
        self.assertIn("robot-emoji", out)

    def test_bare_claude_mention_in_body_fails(self) -> None:
        body = "The wording here was suggested by Claude during review.\n"
        patch = _write_patch(self.root, _patch_chunk("feat: add a thing", body))
        code, out = self._run_patch(patch)
        self.assertEqual(code, 1)
        self.assertIn('a mention of "Claude"', out)

    def test_anthropic_address_fails(self) -> None:
        body = "cc: someone <person@anthropic.com>\n"
        patch = _write_patch(self.root, _patch_chunk("feat: add a thing", body))
        code, out = self._run_patch(patch)
        self.assertEqual(code, 1)
        self.assertIn("an @anthropic.com address", out)

    def test_wrong_author_email_fails(self) -> None:
        author = "Iain Chesworth <wrong@example.com>"
        patch = _write_patch(self.root, _patch_chunk("feat: add a thing", "Fine.\n", author=author))
        code, out = self._run_patch(patch)
        self.assertEqual(code, 1)
        self.assertIn("author is Iain Chesworth <wrong@example.com>", out)
        self.assertIn(f"expected Iain Chesworth <{AUTHOR_EMAIL}>", out)

    def test_wrong_author_name_fails(self) -> None:
        author = f"Someone Else <{AUTHOR_EMAIL}>"
        patch = _write_patch(self.root, _patch_chunk("feat: add a thing", "Fine.\n", author=author))
        code, out = self._run_patch(patch)
        self.assertEqual(code, 1)
        self.assertIn("author is Someone Else <iain.chesworth@gmail.com>", out)

    def test_empty_patch_fails_rather_than_passing_vacuously(self) -> None:
        path = self.root / "empty.patch"
        path.write_text("not a patch\n", encoding="utf-8")
        code, out = self._run_patch(path)
        self.assertEqual(code, 1)
        self.assertIn("no commits found to check", out)

    def test_patch_and_range_are_mutually_exclusive(self) -> None:
        for argv in (
            ["check_patch_attribution.py"],
            ["check_patch_attribution.py", "--patch", "x", "--range", "main..HEAD"],
        ):
            with self.subTest(argv=argv):
                original_argv = sys.argv
                sys.argv = argv
                try:
                    with (
                        contextlib.redirect_stderr(io.StringIO()),
                        self.assertRaises(SystemExit) as ctx,
                    ):
                        cpa.main()
                    self.assertEqual(ctx.exception.code, 2)
                finally:
                    sys.argv = original_argv


@unittest.skipIf(shutil.which("git") is None, "git is not on PATH")
class RangeModeChecks(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(ignore_cleanup_errors=True)
        self.repo = Path(self._tmp.name) / "repo"
        self.repo.mkdir()
        self.addCleanup(self._tmp.cleanup)
        self.git("init", "-q", "-b", "main")
        self.git("config", "user.name", "Iain Chesworth")
        self.git("config", "user.email", "iain.chesworth@gmail.com")
        self.git("config", "commit.gpgsign", "false")
        (self.repo / "file.txt").write_text("base\n", encoding="utf-8")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "base")
        self.base_sha = self.git("rev-parse", "HEAD").strip()

    def git(self, *args: str, env: dict[str, str] | None = None) -> str:
        full_env = dict(os.environ, **env) if env else None
        result = subprocess.run(
            ["git", *args],
            cwd=self.repo,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            env=full_env,
        )
        if result.returncode != 0:
            self.fail(f"git {' '.join(args)} failed:\n{result.stdout}")
        return result.stdout

    def commit(
        self, message: str, *, path: str = "file.txt", author: str | None = None,
        committer_env: dict[str, str] | None = None,
    ) -> None:
        (self.repo / path).write_text(message, encoding="utf-8")
        self.git("add", "-A")
        args = ["commit", "-q", "-m", message]
        if author:
            args += ["--author", author]
        self.git(*args, env=committer_env)

    def _run(self, *extra_args: str) -> tuple[int, str]:
        argv = ["check_patch_attribution.py", "--root", str(self.repo), *extra_args]
        buffer = io.StringIO()
        original_argv = sys.argv
        sys.argv = argv
        try:
            with contextlib.redirect_stdout(buffer):
                code = cpa.main()
        finally:
            sys.argv = original_argv
        return code, buffer.getvalue()

    def test_clean_commit_passes_with_identity_from_git_config(self) -> None:
        self.commit("feat: add a thing\n")
        code, out = self._run("--range", f"{self.base_sha}..HEAD")
        self.assertEqual(code, 0, out)
        self.assertIn("1 commit(s) checked, 0 violation(s)", out)

    def test_author_mismatch_fails(self) -> None:
        self.commit("feat: add a thing\n", author="Someone Else <else@example.com>")
        code, out = self._run("--range", f"{self.base_sha}..HEAD")
        self.assertEqual(code, 1)
        self.assertIn("author is Someone Else <else@example.com>", out)
        self.assertNotIn("committer is", out)

    def test_committer_mismatch_fails(self) -> None:
        env = {"GIT_COMMITTER_NAME": "Someone Else", "GIT_COMMITTER_EMAIL": "else@example.com"}
        self.commit("feat: add a thing\n", committer_env=env)
        code, out = self._run("--range", f"{self.base_sha}..HEAD")
        self.assertEqual(code, 1)
        self.assertIn("committer is Someone Else <else@example.com>", out)
        self.assertNotIn("author is", out)

    def test_merge_commits_are_excluded(self) -> None:
        self.git("checkout", "-q", "-b", "feature")
        self.commit("feat: from the feature branch\n", path="feature.txt")
        self.git("checkout", "-q", "main")
        self.commit("feat: from main\n", path="main.txt")
        self.git(
            "merge", "--no-ff", "-q", "-m", "Merge branch 'feature'", "feature",
            env={"GIT_COMMITTER_NAME": "GitHub", "GIT_COMMITTER_EMAIL": "noreply@github.com"},
        )
        code, out = self._run("--range", f"{self.base_sha}..HEAD")
        self.assertEqual(code, 0, out)
        self.assertIn("2 commit(s) checked, 0 violation(s)", out)


if __name__ == "__main__":
    unittest.main()
