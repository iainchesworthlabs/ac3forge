"""Unit tests for main_health.py: what happens after a post-merge run completes."""

from __future__ import annotations

import contextlib
import io
import json
import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import main_health as mh

RUN = "https://github.com/o/r/actions/runs/77"


class FakeSh:
    """A command runner that answers from a table and records every call.

    Keys are command prefixes; the longest matching prefix wins. A value that is
    an Exception is raised, anything else is returned as the command's stdout.
    """

    def __init__(self, table: dict[tuple[str, ...], object] | None = None):
        self.table = table or {}
        self.calls: list[tuple[tuple[str, ...], str | None]] = []

    def __call__(self, cmd, stdin=None):
        cmd = tuple(cmd)
        self.calls.append((cmd, stdin))
        best = None
        for prefix in self.table:
            if cmd[: len(prefix)] == prefix and (best is None or len(prefix) > len(best)):
                best = prefix
        if best is None:
            return ""
        value = self.table[best]
        if isinstance(value, Exception):
            raise value
        return value

    def ran(self, *prefix: str) -> list[tuple[tuple[str, ...], str | None]]:
        return [c for c in self.calls if c[0][: len(prefix)] == prefix]


def ctx(**over) -> mh.Context:
    base = {
        "repo": "o/r",
        "run_id": 77,
        "head_sha": "a" * 40,
        "conclusion": "failure",
        "attempt": 1,
        "run_url": RUN,
        "dry_run": False,
    }
    base.update(over)
    return mh.Context(**base)


def job(name: str, flake: str | None = None, steps=("Build",)) -> mh.FailedJob:
    found = None
    if flake:
        found = next(f for f in mh.load_flakes() if f.id == flake)
    return mh.FailedJob(id=1, name=name, url=f"{RUN}/job/1", steps=list(steps), flake=found)


class Flakes(unittest.TestCase):
    def setUp(self):
        self.flakes = mh.load_flakes()

    def match(self, job_name: str, evidence: str):
        got = mh.match_flake(job_name, evidence, self.flakes)
        return got.id if got else None

    def test_the_recorded_signatures_match(self):
        cases = {
            "runner-lost": (
                "Build (Linux) / Linux GCC",
                "The runner has received a shutdown signal.",
            ),
            "apt-mirror": (
                "Build (Linux) / Linux GCC",
                "File has unexpected size (817310 != 817362).",
            ),
            "launchpad-503": (
                "Linux AppImage (ac3gui)",
                "lazr.restfulclient.errors.ServerError: HTTP Error 503: Service Unavailable",
            ),
            "android-sdk-zip": ("Android (Shield)", "Error on ZipFile unknown archive."),
            "qemu-restart-segfault": (
                "Minimum-footprint codec (ESP32-S3, QEMU)",
                "QEMU exited with status -11: Adding SPI flash device",
            ),
            "hdiutil-busy": (
                "Build (macOS) / macOS LLVM",
                "hdiutil: create failed - Resource busy",
            ),
        }
        for want, (name, evidence) in cases.items():
            with self.subTest(flake=want):
                self.assertEqual(self.match(name, evidence), want)

    def test_a_job_scoped_signature_ignores_other_jobs(self):
        self.assertIsNone(self.match("Linux GCC", "Error on ZipFile unknown archive"))
        self.assertIsNone(self.match("Windows MSVC", "hdiutil: create failed - Resource busy"))

    def test_a_real_failure_is_not_a_flake(self):
        self.assertIsNone(self.match("Linux GCC", "error: 'foo' was not declared in this scope"))
        self.assertIsNone(self.match("Linux GCC", "1 test case failed: ac4 decode roundtrip"))

    def test_signatures_are_valid_and_unique(self):
        ids = [f.id for f in self.flakes]
        self.assertEqual(len(ids), len(set(ids)))
        self.assertGreaterEqual(len(ids), 6)


class Decide(unittest.TestCase):
    def test_success_advances(self):
        self.assertEqual(mh.decide("success", 1, []), "advance")

    def test_a_superseded_run_is_ignored(self):
        for conclusion in ("cancelled", "skipped", "stale"):
            with self.subTest(conclusion=conclusion):
                self.assertEqual(mh.decide(conclusion, 1, []), "ignore")

    def test_every_failure_a_known_flake_reruns_once(self):
        jobs = [job("Linux GCC", "runner-lost"), job("macOS LLVM", "hdiutil-busy")]
        self.assertEqual(mh.decide("failure", 1, jobs), "rerun")

    def test_one_real_failure_among_flakes_reports(self):
        jobs = [job("Linux GCC", "runner-lost"), job("Windows MSVC")]
        self.assertEqual(mh.decide("failure", 1, jobs), "report")

    def test_the_second_attempt_never_reruns(self):
        self.assertEqual(mh.decide("failure", 2, [job("Linux GCC", "runner-lost")]), "report")

    def test_a_failure_with_no_failed_job_reports(self):
        self.assertEqual(mh.decide("failure", 1, []), "report")

    def test_a_timeout_reports(self):
        self.assertEqual(mh.decide("timed_out", 1, [job("Linux GCC")]), "report")


class MergeSubjects(unittest.TestCase):
    def test_pull_request_number_and_branch(self):
        pr, branch = mh.parse_merge_subject(
            "Merge pull request #1096 from iainchesworthlabs/feature/x"
        )
        self.assertEqual((pr, branch), (1096, "iainchesworthlabs/feature/x"))

    def test_a_direct_commit_has_no_pull_request(self):
        self.assertEqual(mh.parse_merge_subject("fix(ci): something"), (None, ""))


class Suspects(unittest.TestCase):
    LOG = (
        "c" * 40
        + "\tMerge pull request #12 from o/feature/a\n"
        + "b" * 40
        + "\tMerge pull request #11 from o/bugfix/b\n"
        + "d" * 40
        + "\tdirect fix\n"
    )

    def test_range_since_verified(self):
        sh = FakeSh({("git", "log"): self.LOG})
        found, total = mh.suspects(sh, "e" * 40, "a" * 40)
        self.assertEqual([s.pr for s in found], [12, 11, None])
        self.assertEqual(total, 3)
        self.assertIn(f"{'e' * 40}..{'a' * 40}", sh.calls[0][0])

    def test_no_baseline_looks_back_a_fixed_distance(self):
        sh = FakeSh({("git", "log"): self.LOG})
        mh.suspects(sh, None, "a" * 40)
        self.assertIn("-n", sh.calls[0][0])
        self.assertNotIn("..", " ".join(sh.calls[0][0]))

    def test_long_ranges_are_capped_and_counted(self):
        log = "".join(f"{i:040x}\tMerge pull request #{i} from o/f{i}\n" for i in range(1, 30))
        found, total = mh.suspects(FakeSh({("git", "log"): log}), "e" * 40, "a" * 40)
        self.assertEqual(len(found), mh.MAX_SUSPECTS)
        self.assertEqual(total, 29)


class Rendering(unittest.TestCase):
    def suspects(self):
        return [mh.Suspect("c" * 40, 12, "Merge pull request #12 from o/feature/a", "o/feature/a")]

    def test_issue_body_names_the_run_the_jobs_and_the_suspects(self):
        body = mh.render_issue_body(
            ctx(),
            [job("Windows MSVC", steps=("Build", "Test"))],
            self.suspects(),
            1,
            baseline="e" * 40,
        )
        self.assertIn(RUN, body)
        self.assertIn("Windows MSVC", body)
        self.assertIn("#12", body)
        self.assertIn("Build", body)

    def test_a_single_suspect_gets_the_revert_command(self):
        body = mh.render_issue_body(
            ctx(), [job("Linux GCC")], self.suspects(), 1, baseline="e" * 40
        )
        self.assertIn(f"git revert -m 1 {'c' * 40}", body)

    def test_several_suspects_get_the_bisect_recipe_instead(self):
        many = self.suspects() * 2
        body = mh.render_issue_body(ctx(), [job("Linux GCC")], many, 2, baseline="e" * 40)
        self.assertIn("git bisect start", body)
        self.assertNotIn("git revert", body)

    def test_a_preset_leg_gets_its_local_reproduction(self):
        body = mh.render_issue_body(
            ctx(), [job("Build (Linux) / Linux LLVM TSan")], self.suspects(), 1, baseline=None
        )
        self.assertIn("ctest --preset test-linux-llvm-tsan", body)

    def test_a_missing_baseline_is_said_plainly(self):
        body = mh.render_issue_body(ctx(), [job("Linux GCC")], self.suspects(), 1, baseline=None)
        self.assertIn("no verified commit", body.lower())

    def test_excerpt_prefers_error_lines_and_is_bounded(self):
        tail = "\n".join(
            ["noise"] * 200 + ["##[error]Process completed with exit code 2."] + ["more"] * 5
        )
        excerpt = mh.excerpt(tail)
        self.assertIn("exit code 2", excerpt)
        self.assertLessEqual(len(excerpt), mh.EXCERPT_CHARS + 200)


class ExcerptContext(unittest.TestCase):
    def test_the_lines_before_the_last_marker_are_the_explanation(self):
        log = "\n".join(
            ["setup"] * 30
            + ["refused (encode): error: expected refusal"]
            + ["c1.cpp:1: error: no such member 'x'", "ninja: build stopped"]
            + ["##[error]Process completed with exit code 1."]
        )
        got = mh.excerpt(log)
        self.assertIn("no such member 'x'", got)
        self.assertIn("exit code 1", got)
        self.assertLessEqual(len(got.splitlines()), 13)

    def test_without_a_marker_the_newest_error_lines_are_used(self):
        log = "\n".join([f"error: old {i}" for i in range(40)] + ["fine"])
        got = mh.excerpt(log)
        self.assertIn("error: old 39", got)
        self.assertNotIn("error: old 0\n", got)

    def test_without_either_the_tail(self):
        got = mh.excerpt("\n".join(f"line {i}" for i in range(100)))
        self.assertTrue(got.endswith("line 99"))


class Flow(unittest.TestCase):
    JOBS = json.dumps(
        {
            "id": 1,
            "name": "Build & Test / Windows MSVC",
            "html_url": f"{RUN}/job/1",
            "steps": ["Build"],
        }
    )

    def table(self, extra=None):
        table = {
            ("gh", "api", "repos/o/r/actions/runs/77/jobs"): self.JOBS,
            ("gh", "api", "repos/o/r/check-runs/1/annotations"): "compiler exploded",
            ("gh", "api", "repos/o/r/actions/jobs/1/logs"): "##[error]C2664 cannot convert\n",
            ("git", "log"): "c" * 40 + "\tMerge pull request #12 from o/feature/a\n",
            ("git", "rev-parse", "--verify", "--quiet", "refs/remotes/origin/verified"): "e" * 40
            + "\n",
            ("gh", "issue", "list"): "[]",
        }
        table.update(extra or {})
        return table

    def test_success_pushes_verified_forward(self):
        sh = FakeSh(self.table({("gh", "issue", "list"): "[]"}))
        action = mh.health(ctx(conclusion="success"), sh)
        self.assertEqual(action, "advance")
        pushes = sh.ran("git", "push")
        self.assertEqual(len(pushes), 1)
        self.assertIn(f"{'a' * 40}:refs/heads/verified", pushes[0][0])
        self.assertNotIn("--force", pushes[0][0])

    def test_success_does_not_move_verified_backwards(self):
        table = self.table({("git", "merge-base", "--is-ancestor"): mh.CommandError(("x",), 1, "")})
        sh = FakeSh(table)
        mh.health(ctx(conclusion="success"), sh)
        self.assertEqual(sh.ran("git", "push"), [])

    def test_success_closes_an_open_red_issue(self):
        sh = FakeSh(self.table({("gh", "issue", "list"): '[{"number": 9}]'}))
        mh.health(ctx(conclusion="success"), sh)
        self.assertTrue(sh.ran("gh", "issue", "close", "9"))

    def test_cancelled_touches_nothing(self):
        sh = FakeSh(self.table())
        self.assertEqual(mh.health(ctx(conclusion="cancelled"), sh), "ignore")
        self.assertEqual(sh.ran("git", "push"), [])
        self.assertEqual(sh.ran("gh", "issue"), [])

    def test_a_flake_reruns_the_failed_jobs_and_files_nothing(self):
        table = self.table(
            {
                (
                    "gh",
                    "api",
                    "repos/o/r/check-runs/1/annotations",
                ): "The runner has received a shutdown signal."
            }
        )
        sh = FakeSh(table)
        self.assertEqual(mh.health(ctx(), sh), "rerun")
        self.assertTrue(sh.ran("gh", "run", "rerun", "77", "--failed"))
        self.assertEqual(sh.ran("gh", "issue", "create"), [])

    def test_a_real_failure_files_one_issue_and_comments_on_the_suspect(self):
        sh = FakeSh(self.table())
        self.assertEqual(mh.health(ctx(), sh), "report")
        creates = sh.ran("gh", "issue", "create")
        self.assertEqual(len(creates), 1)
        self.assertIn("main-red", creates[0][0])
        self.assertIn("Windows MSVC", creates[0][1])
        self.assertTrue(sh.ran("gh", "pr", "comment", "12"))
        self.assertEqual(sh.ran("gh", "run", "rerun"), [])

    def test_a_still_red_main_updates_the_open_issue_instead_of_opening_another(self):
        sh = FakeSh(self.table({("gh", "issue", "list"): '[{"number": 9}]'}))
        mh.health(ctx(), sh)
        self.assertEqual(sh.ran("gh", "issue", "create"), [])
        self.assertTrue(sh.ran("gh", "issue", "comment", "9"))

    def test_a_pull_request_already_told_about_this_run_is_not_told_twice(self):
        marker = "<!-- main-health run:77 -->"
        table = self.table({("gh", "api", "repos/o/r/issues/12/comments"): marker})
        sh = FakeSh(table)
        mh.health(ctx(), sh)
        self.assertEqual(sh.ran("gh", "pr", "comment", "12"), [])

    def test_dry_run_changes_nothing(self):
        sh = FakeSh(self.table())
        with contextlib.redirect_stdout(io.StringIO()):
            mh.health(ctx(dry_run=True), sh)
        for prefix in (
            ("git", "push"),
            ("gh", "issue", "create"),
            ("gh", "pr", "comment"),
            ("gh", "run", "rerun"),
        ):
            self.assertEqual(sh.ran(*prefix), [], prefix)

    def test_the_body_reaches_gh_on_stdin_not_on_the_command_line(self):
        sh = FakeSh(self.table())
        mh.health(ctx(), sh)
        cmd, stdin = sh.ran("gh", "issue", "create")[0]
        self.assertIn("--body-file", cmd)
        self.assertIn("-", cmd)
        self.assertTrue(stdin)
        self.assertFalse(any("Failed jobs" in part for part in cmd))


class Cli(unittest.TestCase):
    def test_attempt_and_flags_parse(self):
        args = mh.parse_args(
            [
                "--repo",
                "o/r",
                "--run-id",
                "5",
                "--head-sha",
                "b" * 40,
                "--conclusion",
                "failure",
                "--attempt",
                "2",
                "--run-url",
                RUN,
                "--dry-run",
            ]
        )
        self.assertEqual((args.run_id, args.attempt, args.dry_run), (5, 2, True))

    def test_marker_is_stable(self):
        self.assertTrue(re.fullmatch(r"<!-- main-health run:77 -->", mh.run_marker(77)))


if __name__ == "__main__":
    unittest.main()
