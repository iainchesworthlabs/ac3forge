"""Unit tests for how the merge queue's comparisons are wired through the workflows.

stdlib only, like the other suites in tools/ci: these read the workflow files as text. They
pin the seams a change to one file could break without any other file noticing: the gate
calls _compare.yml for a queue entry that changes src/, `CI Status` reads its result, the
gate jobs read labels through compare_gate.py, and the comparisons are no longer in
_ci-core.yml, whose outputs ci.yml's aggregator would otherwise still read.
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
    match = re.search(
        rf"^  {re.escape(name)}:\n(.*?)(?=^  [A-Za-z0-9_-]+:\n|\Z)", text(workflow), re.S | re.M
    )
    if not match:
        raise AssertionError(f"no job {name!r} in {workflow}")
    return match.group(1)


class TheGateCallsTheComparisons(unittest.TestCase):
    def test_the_compare_job_calls_the_reusable_workflow_only_when_the_plan_asks(self):
        compare = job("pr-gate.yml", "compare")
        self.assertIn("uses: ./.github/workflows/_compare.yml", compare)
        self.assertIn("needs.plan.outputs.compare == 'true'", compare)
        # It reads pull request labels, and nothing more than that.
        self.assertRegex(compare, r"permissions:\n\s+contents: read\n\s+pull-requests: read")

    def test_it_is_handed_the_base_the_pull_request_and_a_runner(self):
        compare = job("pr-gate.yml", "compare")
        self.assertIn("base_sha: ${{ needs.plan.outputs.compare_base }}", compare)
        self.assertIn("pr_number: ${{ needs.plan.outputs.compare_pr }}", compare)
        self.assertIn("vars.GATE_COMPARE_RUNNER_JSON || vars.GATE_RUNNER_JSON", compare)

    def test_the_plan_names_the_pull_request_from_the_merge_groups_ref(self):
        gate = text("pr-gate.yml")
        self.assertIn("MG_REF: ${{ github.ref_name }}", gate)
        self.assertIn(r"^gh-readonly-queue/[^/]+/pr-([0-9]+)-", gate)
        for output in ("compare", "compare_base", "compare_pr"):
            self.assertIn(f"{output}: ${{{{ steps.plan.outputs.{output} }}}}", gate)

    def test_only_a_queue_entry_or_a_request_runs_them(self):
        plan = job("pr-gate.yml", "plan")
        # A queue entry: what the planner said about src/, and the merge group's base.
        entry = "if [ \"$(sed -n 's/^compare=//p' plan.out)\" = true ]; then compare=true; fi"
        self.assertIn(entry, plan)
        self.assertLess(plan.index(entry), plan.index('compare_base="$MG_BASE"'))
        # Anything else: only when asked for.
        self.assertIn('if [ "$WANT_COMPARE" = true ]; then compare=true; fi', plan)
        # And never for a change that does not build.
        self.assertIn('if [ "$build" != true ]; then compare=false; fi', plan)

    def test_ci_status_reads_the_result_and_requires_it_when_planned(self):
        status = job("pr-gate.yml", "ci-status")
        self.assertRegex(status, r"needs: \[[^\]]*\bcompare\b[^\]]*\]")
        self.assertIn("R_COMPARE: ${{ needs.compare.result }}", status)
        self.assertIn("WANT_COMPARE: ${{ needs.plan.outputs.compare }}", status)
        self.assertIn('check compare "$R_COMPARE"', status)


class TheReusableWorkflow(unittest.TestCase):
    def test_it_is_a_workflow_call_with_the_four_jobs(self):
        compare = text("_compare.yml")
        self.assertRegex(compare, r"(?m)^on:\n  workflow_call:")
        for name in ("performance-compare", "performance-gate", "memory-compare", "memory-gate"):
            with self.subTest(job=name):
                self.assertIsNotNone(job("_compare.yml", name))

    def test_the_measuring_jobs_do_not_block_and_do_not_wait_for_a_pull_request_event(self):
        for name in ("performance-compare", "memory-compare"):
            with self.subTest(job=name):
                body = job("_compare.yml", name)
                self.assertIn("continue-on-error: true", body)
                self.assertIn("timeout-minutes:", body)
                self.assertNotIn("github.event_name", body)
                self.assertNotIn("github.event.pull_request", body)
                self.assertIn("inputs.base_sha", body)

    def test_the_gates_decide_through_the_script_and_read_labels_of_the_named_pull_request(self):
        for name, verdict, kind in (
            ("performance-gate", "performance-compare", "performance"),
            ("memory-gate", "memory-compare", "memory"),
        ):
            with self.subTest(job=name):
                body = job("_compare.yml", name)
                self.assertIn("if: always()", body)
                self.assertIn(f"needs.{verdict}.outputs.hard_regression", body)
                self.assertIn(f"tools/ci/compare_gate.py --kind {kind}", body)
                self.assertIn("PR_NUMBER: ${{ inputs.pr_number }}", body)
                self.assertRegex(body, r"permissions:\n\s+contents: read\n\s+pull-requests: read")
                self.assertNotIn("github.event.pull_request", body)

    def test_the_script_the_gates_call_exists(self):
        self.assertTrue((Path(__file__).resolve().parent / "compare_gate.py").is_file())


class TheOldCopiesAreGone(unittest.TestCase):
    """ci.yml does not run on pull requests, so the jobs in _ci-core.yml never ran."""

    def test_ci_core_no_longer_has_the_jobs_or_their_outputs(self):
        core = text("_ci-core.yml")
        for name in ("performance-compare", "memory-compare", "performance-gate", "memory-gate"):
            with self.subTest(job=name):
                self.assertNotIn(f"\n  {name}:", core)
        self.assertNotIn("performance_gate_result", core)
        self.assertNotIn("memory_gate_result", core)

    def test_the_aggregator_in_ci_yml_does_not_read_them(self):
        ci = text("ci.yml")
        self.assertNotIn("performance_gate_result", ci)
        self.assertNotIn("memory_gate_result", ci)
        self.assertNotIn("R_PERF_GATE", ci)
        self.assertNotIn("R_MEM_GATE", ci)


if __name__ == "__main__":
    unittest.main()
