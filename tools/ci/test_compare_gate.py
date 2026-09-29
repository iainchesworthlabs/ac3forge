"""Unit tests for compare_gate.py: what a hard regression does to a merge queue entry."""

from __future__ import annotations

import contextlib
import io
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compare_gate as cg


class FakeGh:
    """A `gh` that answers the label request from a list of outcomes, one per attempt."""

    def __init__(self, *outcomes):
        self.outcomes = list(outcomes)
        self.calls = []

    def __call__(self, cmd):
        self.calls.append(tuple(cmd))
        outcome = self.outcomes.pop(0) if len(self.outcomes) > 1 else self.outcomes[0]
        if isinstance(outcome, Exception):
            raise outcome
        return outcome


def refused(message="gh: HTTP 502"):
    return cg.CommandError(["gh", "api"], 1, message + "\n")


def run(kind, verdict, pr, gh, **retry):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        code = cg.gate(cg.KINDS[kind], verdict, "o/r", pr, gh, **retry)
    return code, out.getvalue()


class NoVerdict(unittest.TestCase):
    def test_anything_but_true_passes_without_asking_for_labels(self):
        for verdict in ("", "false", "none"):
            with self.subTest(verdict=verdict):
                gh = FakeGh("")
                code, out = run("performance", verdict, "42", gh)
                self.assertEqual(code, 0)
                self.assertIn("No hard performance regression to gate on.", out)
                self.assertEqual(gh.calls, [])


class HardRegression(unittest.TestCase):
    def test_the_kinds_own_label_lets_it_through_with_a_warning(self):
        for kind, label in (
            ("performance", "perf-regression-approved"),
            ("memory", "memory-regression-approved"),
        ):
            with self.subTest(kind=kind):
                code, out = run(kind, "true", "42", FakeGh(f"bug\n{label}\n"))
                self.assertEqual(code, 0)
                self.assertIn("::warning title=Hard", out)
                self.assertIn(f"'{label}'", out)
                self.assertNotIn("::error", out)

    def test_no_label_fails_and_says_how_to_approve(self):
        code, out = run("performance", "true", "42", FakeGh("bug\ndocs\n"))
        self.assertEqual(code, 1)
        self.assertIn("::error title=Hard performance regression::", out)
        self.assertIn("'perf-regression-approved'", out)
        self.assertIn("pull request 42", out)
        self.assertIn("merge queue", out)
        self.assertIn("'Performance vs base' job summary", out)

    def test_the_other_kinds_label_does_not_count(self):
        code, _ = run("memory", "true", "42", FakeGh("perf-regression-approved\n"))
        self.assertEqual(code, 1)
        code, _ = run("performance", "true", "42", FakeGh("memory-regression-approved\n"))
        self.assertEqual(code, 1)

    def test_a_label_that_only_contains_the_name_does_not_count(self):
        code, _ = run("performance", "true", "42", FakeGh("perf-regression-approved-later\n"))
        self.assertEqual(code, 1)

    def test_the_labels_are_asked_of_the_named_pull_request(self):
        gh = FakeGh("perf-regression-approved\n")
        run("performance", "true", "42", gh)
        self.assertEqual(len(gh.calls), 1)
        self.assertIn("repos/o/r/issues/42/labels", gh.calls[0])

    def test_no_pull_request_means_nothing_can_approve_it(self):
        gh = FakeGh("perf-regression-approved\n")
        code, out = run("performance", "true", "", gh)
        self.assertEqual(code, 1)
        self.assertEqual(gh.calls, [])
        self.assertIn("No pull request was named", out)


class Retries(unittest.TestCase):
    def test_a_failed_read_is_tried_again(self):
        gh = FakeGh(refused(), "perf-regression-approved\n")
        naps = []
        code, _ = run("performance", "true", "42", gh, sleep=naps.append)
        self.assertEqual(code, 0)
        self.assertEqual(len(gh.calls), 2)
        self.assertEqual(naps, [cg.PAUSE_SECONDS])

    def test_three_failures_fail_the_entry_and_say_why(self):
        gh = FakeGh(refused("gh: HTTP 502 Bad Gateway"))
        naps = []
        code, out = run("performance", "true", "42", gh, sleep=naps.append)
        self.assertEqual(code, 1)
        self.assertEqual(len(gh.calls), cg.ATTEMPTS)
        self.assertEqual(len(naps), cg.ATTEMPTS - 1)
        self.assertIn("could not be read (gh: HTTP 502 Bad Gateway)", out)
        self.assertIn("queue the pull request again", out)


class Main(unittest.TestCase):
    def test_the_arguments_reach_the_gate(self):
        gh = FakeGh("memory-regression-approved\n")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = cg.main(
                ["--kind", "memory", "--verdict", "true", "--repo", "o/r", "--pr", "7"], gh
            )
        self.assertEqual(code, 0)
        self.assertIn("repos/o/r/issues/7/labels", gh.calls[0])

    def test_an_empty_verdict_is_a_pass(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = cg.main(["--kind", "performance", "--verdict", "", "--pr", ""], FakeGh(""))
        self.assertEqual(code, 0)


if __name__ == "__main__":
    unittest.main()
