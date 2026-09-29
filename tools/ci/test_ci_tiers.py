"""Unit tests for how ci.yml's two tiers are wired through the workflows.

stdlib only, like the other suites in tools/ci: the static job that runs them installs
nothing beyond its linters, so these read the workflow files as text. They pin the
placement of jobs in the run after a merge and in the nightly run (docs/ci-agentic.md,
"The tiers"), so moving a job between them is a change that shows in a diff of this
file too.
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

WORKFLOWS = Path(__file__).resolve().parents[2] / ".github" / "workflows"


def text(name: str) -> str:
    return (WORKFLOWS / name).read_text(encoding="utf-8")


def job(workflow: str, name: str) -> str:
    """The source of one top-level job: from `  name:` to the next job or the end."""
    body = text(workflow)
    match = re.search(
        rf"^  {re.escape(name)}:\n(.*?)(?=^  [A-Za-z0-9_-]+:\n|\Z)", body, re.S | re.M
    )
    if not match:
        raise AssertionError(f"no job {name!r} in {workflow}")
    return match.group(1)


def condition(workflow: str, name: str) -> str:
    """The job-level `if:` of a job, joined onto one line; empty when it has none."""
    lines = job(workflow, name).splitlines()
    for i, line in enumerate(lines):
        if line.startswith("    if:"):
            out = [line.removeprefix("    if:").strip()]
            for more in lines[i + 1 :]:
                if more.startswith("      ") and not more.startswith("    #"):
                    out.append(more.strip())
                else:
                    break
            return " ".join(out).lstrip(">-").strip()
    return ""


class Triggers(unittest.TestCase):
    def test_ci_runs_after_a_merge_and_nightly_and_on_request(self):
        ci = text("ci.yml")
        self.assertRegex(ci, r"(?m)^  push:\n    branches: \[ main \]")
        # 13:17 UTC starts the run at about 19:47 UTC once GitHub's delay is added (see ci.yml).
        self.assertRegex(ci, r"(?m)^  schedule:\n    - cron: '17 13 \* \* \*'")
        self.assertIn("workflow_dispatch:", ci)

    def test_the_label_workflow_reacts_only_to_ci_deep_on_a_pull_request_from_this_repository(self):
        label = text("deep-on-label.yml")
        self.assertRegex(label, r"types: \[ labeled \]")
        self.assertIn("github.event.label.name == 'ci:deep'", label)
        self.assertIn("github.event.pull_request.head.repo.full_name == github.repository", label)
        self.assertNotIn("pull_request_target", label)
        self.assertIn("-f tier=all", label)


class TierIsPassedDown(unittest.TestCase):
    def test_the_changes_job_decides_the_tier(self):
        changes = job("ci.yml", "changes")
        self.assertIn("tier: ${{ steps.detect.outputs.tier }}", changes)
        self.assertIn("tier=t2", changes)
        self.assertIn('echo "tier=$tier" >> "$GITHUB_OUTPUT"', changes)

    def test_the_builds_and_the_core_checks_get_it(self):
        ci = text("ci.yml")
        self.assertEqual(ci.count("tier: ${{ needs.changes.outputs.tier }}"), 2)
        self.assertIn("_build.yml", job("ci.yml", "build-and-test"))
        self.assertIn("tier: ${{ needs.changes.outputs.tier }}", job("ci.yml", "build-and-test"))
        self.assertIn("tier: ${{ needs.changes.outputs.tier }}", job("ci.yml", "core"))

    def test_the_reusable_workflows_default_to_everything(self):
        for workflow in ("_build.yml", "_ci-core.yml"):
            with self.subTest(workflow=workflow):
                self.assertRegex(
                    text(workflow), r"(?m)^      tier:\n(?:        .*\n)*?        default: all\n"
                )


class WhatRunsAfterAMerge(unittest.TestCase):
    NIGHTLY_ONLY_CORE = ("coverage", "abi-gate", "ffmpeg-validate")
    STAYS_CORE = ("adm-validate", "hearth-validate")

    def test_the_slow_core_jobs_wait_for_the_nightly_run(self):
        for name in self.NIGHTLY_ONLY_CORE:
            with self.subTest(job=name):
                self.assertIn("inputs.tier != 't2'", condition("_ci-core.yml", name))

    def test_the_other_core_jobs_stay(self):
        for name in self.STAYS_CORE:
            with self.subTest(job=name):
                self.assertNotIn("tier", condition("_ci-core.yml", name))

    def test_the_trend_publishers_that_read_ffmpeg_validate_run_nightly_too(self):
        for name in ("persist-external-comparison-trend", "persist-object-quality-trend"):
            with self.subTest(job=name):
                cond = condition("_ci-core.yml", name)
                self.assertIn("github.event_name == 'schedule'", cond)
                self.assertIn("needs: ffmpeg-validate", job("_ci-core.yml", name))

    def test_the_appimage_is_nightly_only(self):
        self.assertIn("inputs.tier != 't2'", condition("_build.yml", "linux-appimage"))

    def test_the_quality_trend_is_recorded_by_both_tiers_and_tolerates_a_skipped_platform(self):
        self.assertIn(
            "(github.event_name == 'push' || github.event_name == 'schedule')",
            text("ci.yml"),
        )
        trend = condition("_build.yml", "quality-trend")
        for platform in ("windows", "linux", "macos"):
            self.assertIn(f"needs.build-{platform}.result", trend)
        self.assertIn('"skipped"', trend)
        # ...but not with nothing built at all, which leaves no artifact to record.
        self.assertIn("== 'success'", trend)

    def test_the_lane_range_is_what_merged_since_verified_and_satellites_are_direct(self):
        changes = job("ci.yml", "changes")
        self.assertIn("git/ref/heads/verified", changes)
        self.assertIn("compare/$verified...$SHA", changes)
        self.assertIn("--satellites-direct", changes)
        self.assertIn('[ "$tier" = t2 ]', changes)

    def test_a_failed_lookup_is_told_by_its_exit_status_not_by_what_it_printed(self):
        # gh prints a failed request's JSON body on stdout, so a lookup that only
        # checked for output took the 404 body of a missing `verified` ref for a
        # commit id and logged it as one.
        changes = job("ci.yml", "changes")
        self.assertIn('if verified="$(gh api "repos/$REPO/git/ref/heads/verified"', changes)
        self.assertIn('[ "${#verified}" -eq 40 ]', changes)
        self.assertIn('if files="$(gh api --paginate "repos/$REPO/compare/$verified', changes)
        self.assertNotIn("|| true)", changes)

    def test_the_nightly_run_and_a_dispatch_force_every_lane(self):
        changes = job("ci.yml", "changes")
        self.assertIn('[ "$EVENT_NAME" = "schedule" ]', changes)
        self.assertIn('[ "$EVENT_NAME" = "workflow_dispatch" ]', changes)
        # The run after a merge no longer forces them.
        forcing = re.search(r"if \[ \"\$EVENT_NAME\" = \"merge_group\" \].*?then", changes, re.S)
        self.assertIsNotNone(forcing)
        self.assertNotIn("refs/heads/main", forcing.group(0))


if __name__ == "__main__":
    unittest.main()
