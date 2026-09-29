"""Unit tests for precheck.py: the local run of the gate's static checks."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import precheck


class BranchName(unittest.TestCase):
    def test_accepts_the_shapes_the_gate_accepts(self):
        for name in (
            "feature/eac3-decoder",
            "bugfix/x1",
            "hotfix/a-b-c",
            "docs/roadmap",
            "chore/ci",
        ):
            with self.subTest(name=name):
                self.assertTrue(precheck.branch_name_ok(name))

    def test_main_and_dependabot_are_allowed(self):
        self.assertTrue(precheck.branch_name_ok("main"))
        self.assertTrue(precheck.branch_name_ok("dependabot/pip/requirements/x"))

    def test_rejects_what_the_gate_rejects(self):
        for name in (
            "claude/agentic-ci-system-b39317",
            "feature/Under_score",
            "feature/trailing-",
            "topic",
        ):
            with self.subTest(name=name):
                self.assertFalse(precheck.branch_name_ok(name))


class Summary(unittest.TestCase):
    def test_a_failure_makes_the_run_fail_and_a_skip_does_not(self):
        ok = [precheck.Result("a", "pass", ""), precheck.Result("b", "skip", "no pwsh")]
        bad = [*ok, precheck.Result("c", "fail", "boom")]
        self.assertEqual(precheck.exit_code(ok), 0)
        self.assertEqual(precheck.exit_code(bad), 1)

    def test_the_table_names_every_check(self):
        text = precheck.render(
            [precheck.Result("ruff", "pass", ""), precheck.Result("macros", "skip", "no pwsh")]
        )
        self.assertIn("ruff", text)
        self.assertIn("macros", text)
        self.assertIn("no pwsh", text)


class Plan(unittest.TestCase):
    def test_plan_line_reads_the_diff(self):
        line = precheck.plan_line(["src/ac4dec/x.cpp"])
        self.assertIn("build=true", line)
        self.assertIn("gui=false", line)

    def test_plan_line_for_docs(self):
        self.assertIn("docs_only=true", precheck.plan_line(["docs/a.md"]))


if __name__ == "__main__":
    unittest.main()
