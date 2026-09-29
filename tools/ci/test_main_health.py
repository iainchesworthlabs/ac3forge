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

    def test_the_nightly_issue_says_it_is_the_nightly_run_and_what_it_was_judged_against(self):
        body = mh.render_issue_body(
            ctx(event="schedule"), [job("Linux GCC")], self.suspects(), 1, baseline="f" * 40
        )
        self.assertIn("nightly run of `main` failed", body)
        self.assertIn("last commit the nightly run verified", body)
        self.assertIn("ffffffff", body)
        self.assertNotIn("Post-merge verification of `main` failed", body)

    def test_a_nightly_with_no_verified_commit_says_so(self):
        body = mh.render_issue_body(
            ctx(event="schedule"), [job("Linux GCC")], self.suspects(), 1, baseline=None
        )
        self.assertIn("nightly run has not verified a commit yet", body)

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


class ExcerptCause(unittest.TestCase):
    """A compiler's diagnostic sits above the build tool's own lines, not just before them."""

    def wheel_log(self) -> str:
        progress = [f"[{n}/227] Building CXX object x{n}.o" for n in range(20)]
        return "\n".join(
            ["setup"] * 40
            + ["FAILED: [code=1] src/ac4dec/decoder.cpp.o", "g++ -c decoder.cpp " + "-DX " * 300]
            + ["/opt/gcc/stl_vector.h:388:9: error: " + "a" * 3000]
            + ["cc1plus: all warnings being treated as errors"]
            + progress
            + ["ninja: build stopped: subcommand failed.", "*** CMake build failed"]
            + ["##[error]cibuildwheel: Command failed with code 1."]
            + ["##[error]Process completed with exit code 1."]
        )

    def test_the_failed_command_and_its_error_are_shown_ahead_of_the_tail(self):
        got = mh.excerpt(self.wheel_log())
        self.assertIn("FAILED: [code=1] src/ac4dec/decoder.cpp.o", got)
        self.assertIn("stl_vector.h:388:9: error:", got)
        self.assertIn("ninja: build stopped", got)
        self.assertIn("exit code 1", got)
        self.assertLess(got.index("FAILED: [code=1]"), got.index("ninja: build stopped"))

    def test_a_long_line_is_clipped_and_the_whole_is_bounded(self):
        got = mh.excerpt(self.wheel_log())
        self.assertLessEqual(max(len(ln) for ln in got.splitlines()), mh.EXCERPT_LINE_CHARS + 3)
        self.assertLessEqual(len(got), mh.EXCERPT_CHARS)

    def test_the_last_failed_command_wins_over_an_earlier_expected_error(self):
        log = "\n".join(
            ["refused (encode): error: expected refusal"]
            + ["noise"] * 60
            + ["FAILED: real.o", "a.cpp:1:1: error: the real one"]
            + ["noise"] * 30
            + ["##[error]Process completed with exit code 1."]
        )
        got = mh.excerpt(log)
        self.assertIn("the real one", got)
        self.assertNotIn("expected refusal", got)

    def test_timestamps_are_dropped(self):
        stamped = "2026-09-29T16:44:26.3061560Z ##[error]Process completed with exit code 1."
        self.assertEqual(mh.excerpt(stamped), "##[error]Process completed with exit code 1.")


class EvidenceDiagnostics(unittest.TestCase):
    def test_a_log_with_colour_codes_is_read_with_the_flag_a_newer_gh_asks_for(self):
        asks = mh.CommandError(
            ["gh", "api", "x"],
            1,
            "the response contains terminal escape sequences; "
            "pass --allow-escape-sequences to output it anyway\n",
        )
        log = ("gh", "api", "repos/o/r/actions/jobs/1/logs")
        sh = FakeSh(
            {
                ("gh", "api", "repos/o/r/check-runs/1/annotations"): "exit code 1.",
                log: asks,
                (*log, "--allow-escape-sequences"): (
                    "\x1b[31mFAILED: real.o\x1b[0m\n##[error]Process completed with exit code 1."
                ),
            }
        )
        failed = job("Build & Test / Linux GCC")
        mh.gather_evidence(ctx(), sh, [failed], mh.load_flakes())
        self.assertIn("FAILED: real.o", failed.evidence)
        self.assertNotIn("\x1b", failed.evidence)
        self.assertNotIn("could not read", failed.evidence)

    def test_an_unreadable_log_is_said_in_the_evidence_and_in_the_run_log(self):
        refused = mh.CommandError(
            ["gh", "api", "x"], 1, "gh: Resource not accessible by integration (HTTP 403)\n"
        )
        sh = FakeSh(
            {
                ("gh", "api", "repos/o/r/check-runs/1/annotations"): "exit code 1.",
                ("gh", "api", "repos/o/r/actions/jobs/1/logs"): refused,
            }
        )
        failed = job("Build & Test / Linux GCC")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            mh.gather_evidence(ctx(), sh, [failed], mh.load_flakes())
        self.assertIn("exit code 1.", failed.evidence)
        self.assertIn("could not read the log of this job", failed.evidence)
        self.assertIn("Resource not accessible", failed.evidence)
        self.assertIn(
            "::warning title=main-health::could not read the log of Build & Test / Linux GCC",
            out.getvalue(),
        )


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


class NightlyFlow(unittest.TestCase):
    """The scheduled run keeps its own baseline and its own issue."""

    JOB = json.dumps(
        {
            "id": 1,
            "name": "Build & Test / Build (Linux) / Linux LLVM ASan+UBSan",
            "html_url": f"{RUN}/job/1",
            "steps": ["Test"],
        }
    )
    ISSUES = ("gh", "issue", "list", "-R", "o/r", "--label")

    def table(self, extra=None):
        table = {
            ("gh", "api", "repos/o/r/actions/runs/77/jobs"): self.JOB,
            ("gh", "api", "repos/o/r/check-runs/1/annotations"): "",
            ("gh", "api", "repos/o/r/actions/jobs/1/logs"): "##[error]heap-use-after-free\n",
            ("git", "log"): "c" * 40 + "\tMerge pull request #12 from o/feature/a\n",
            ("git", "rev-parse", "--verify", "--quiet", "refs/remotes/origin/verified"): "e" * 40,
            (
                "git",
                "rev-parse",
                "--verify",
                "--quiet",
                "refs/remotes/origin/verified-nightly",
            ): "f" * 40,
            (*self.ISSUES, "main-red"): "[]",
            (*self.ISSUES, "main-red-nightly"): "[]",
        }
        table.update(extra or {})
        return table

    def test_a_green_nightly_moves_both_refs(self):
        sh = FakeSh(self.table())
        self.assertEqual(mh.health(ctx(conclusion="success", event="schedule"), sh), "advance")
        targets = sorted(c[0][-1] for c in sh.ran("git", "push"))
        self.assertEqual(
            targets, [f"{'a' * 40}:refs/heads/verified", f"{'a' * 40}:refs/heads/verified-nightly"]
        )

    def test_a_green_nightly_closes_both_issues(self):
        table = self.table(
            {
                (*self.ISSUES, "main-red"): '[{"number": 9}]',
                (*self.ISSUES, "main-red-nightly"): '[{"number": 10}]',
            }
        )
        sh = FakeSh(table)
        mh.health(ctx(conclusion="success", event="schedule"), sh)
        self.assertTrue(sh.ran("gh", "issue", "close", "9"))
        self.assertTrue(sh.ran("gh", "issue", "close", "10"))

    def test_a_green_run_after_a_merge_leaves_the_nightly_alone(self):
        table = self.table(
            {
                (*self.ISSUES, "main-red"): '[{"number": 9}]',
                (*self.ISSUES, "main-red-nightly"): '[{"number": 10}]',
            }
        )
        sh = FakeSh(table)
        mh.health(ctx(conclusion="success", event="push"), sh)
        self.assertEqual(
            [c[0][-1] for c in sh.ran("git", "push")], [f"{'a' * 40}:refs/heads/verified"]
        )
        self.assertTrue(sh.ran("gh", "issue", "close", "9"))
        self.assertEqual(sh.ran("gh", "issue", "close", "10"), [])
        self.assertEqual(sh.ran(*self.ISSUES, "main-red-nightly"), [])

    def test_a_red_nightly_is_judged_against_the_last_green_nightly(self):
        sh = FakeSh(self.table())
        self.assertEqual(mh.health(ctx(event="schedule"), sh), "report")
        log = sh.ran("git", "log")[0][0]
        self.assertIn(f"{'f' * 40}..{'a' * 40}", log)
        self.assertNotIn(f"{'e' * 40}..", " ".join(log))

    def test_a_red_nightly_opens_its_own_issue_and_names_no_pull_request(self):
        sh = FakeSh(self.table())
        mh.health(ctx(event="schedule"), sh)
        cmd, body = sh.ran("gh", "issue", "create")[0]
        self.assertIn("main-red-nightly", cmd)
        title = cmd[cmd.index("--title") + 1]
        self.assertTrue(title.startswith("the nightly run is red at "), title)
        self.assertIn("ASan+UBSan", title)
        self.assertIn("nightly run of `main` failed", body)
        self.assertEqual(sh.ran("gh", "pr", "comment"), [])
        self.assertTrue(sh.ran("gh", "label", "create", "main-red-nightly"))

    def test_a_red_nightly_updates_the_open_nightly_issue(self):
        sh = FakeSh(self.table({(*self.ISSUES, "main-red-nightly"): '[{"number": 10}]'}))
        mh.health(ctx(event="schedule"), sh)
        self.assertEqual(sh.ran("gh", "issue", "create"), [])
        self.assertTrue(sh.ran("gh", "issue", "comment", "10"))

    def test_a_red_run_after_a_merge_still_files_under_main_red(self):
        sh = FakeSh(self.table())
        mh.health(ctx(event="push"), sh)
        cmd, _ = sh.ran("gh", "issue", "create")[0]
        self.assertIn("main-red", cmd)
        self.assertNotIn("main-red-nightly", cmd)
        self.assertTrue(sh.ran("gh", "pr", "comment", "12"))
        self.assertIn(f"{'e' * 40}..{'a' * 40}", sh.ran("git", "log")[0][0])


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
        self.assertEqual(args.event, "")

    def test_the_event_flag_marks_the_nightly_run(self):
        args = mh.parse_args(
            [
                *("--repo", "o/r", "--run-id", "5", "--head-sha", "b" * 40),
                *("--conclusion", "success", "--run-url", RUN, "--event", "schedule"),
            ]
        )
        self.assertEqual(args.event, "schedule")
        self.assertTrue(mh.nightly(ctx(event=args.event)))
        self.assertFalse(mh.nightly(ctx(event="push")))
        self.assertFalse(mh.nightly(ctx()))

    def test_marker_is_stable(self):
        self.assertTrue(re.fullmatch(r"<!-- main-health run:77 -->", mh.run_marker(77)))


class Workflow(unittest.TestCase):
    """main-health.yml only runs from main, so what it passes to the script is pinned here."""

    TEXT = (Path(__file__).resolve().parents[2] / ".github/workflows/main-health.yml").read_text(
        encoding="utf-8"
    )

    def test_it_tells_the_script_which_event_ended_the_run(self):
        self.assertIn("EVENT: ${{ github.event.workflow_run.event }}", self.TEXT)
        self.assertIn('--event "$EVENT"', self.TEXT)

    def test_it_takes_every_value_from_the_run_through_env(self):
        # The script's own arguments carry `$NAME`, never a `${{ }}` expansion.
        run = self.TEXT.split("        run: |", maxsplit=1)[1]
        self.assertNotIn("${{", run)


if __name__ == "__main__":
    unittest.main()
