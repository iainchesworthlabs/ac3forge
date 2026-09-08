"""Unit tests for compare_memory.py, the pre-merge heap-churn gate.

stdlib `unittest`, not pytest, for the same reason the scripts under test are
stdlib-only: this runs in ci.yml's script-lint job, which installs ruff,
shellcheck and actionlint and nothing else, and a test that needs a new pinned
dependency to run is a test that will not be run.

Two properties are worth holding down here, and they are different kinds of
claim.

THE GATES MUST AGREE. compare_memory.py runs before a merge and
append_memory_history.py runs after it, over the same metrics with the same
thresholds, so a change that the PR-time gate waves through and the post-merge
gate then fails on main puts CI back exactly where this work started - red on
an already-merged commit. AgreementTests runs the same numbers through both and
asserts the same tier comes out. The thresholds are imported rather than
restated, so nothing here can drift by editing one constant; what these tests
cover is the LADDER around them, which is written out twice (check_churn's
generator and classify_churn's returns) and could.

THE LEAK CHECK IS ATTRIBUTED, NOT ABSOLUTE. append_memory_history.py judges
steady_live_growth against fixed thresholds with no baseline, which is right
for a series on main. Copied unchanged into a per-PR job it would annotate
every pull request for bytes no pull request retained: measured on this tree,
eac3_51_encode holds 4,963 bytes and atmos_4obj_encode 5,322 across their
steady state, both already past the 4 KiB warn line. LeakAttributionTests pins
those two real numbers as the case that must stay quiet, alongside the
crossings and growths that must not.

Run: python3 -m unittest discover -s tools/ci -p 'test_*.py'
"""

import contextlib
import io
import json
import sys
import tempfile
import unittest
import unittest.mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import append_memory_history as amh
import compare_memory as cm

ALLOCS_FLOOR = amh.ALLOCS_ABSOLUTE_FLOOR
BYTES_FLOOR = amh.BYTES_ABSOLUTE_FLOOR


def record(allocs=67.0, byts=28792.0, live=0, config="eac3_51_encode", leg="linux-gcc"):
    """One ac3membench result as both scripts see it after their shared
    load_leg_results."""
    return {"leg": leg, "config": config, "allocs_per_frame": allocs,
            "bytes_per_frame": byts, "steady_live_growth": live}


def side(*records):
    """A {(leg, config): record} side, the shape load_side returns."""
    return {(r["leg"], r["config"]): r for r in records}


def write_side(root: Path, *records, leg="linux-gcc"):
    """A real results tree - one memory-<leg> subdirectory holding
    ac3membench's own JSON - so load_side is exercised over the layout CI
    actually produces rather than over a hand-built dict."""
    leg_dir = root / f"memory-{leg}"
    leg_dir.mkdir(parents=True, exist_ok=True)
    payload = {"peak_rss_bytes": 9564160, "results": [
        {"name": r["config"], "frames": 200, "setup_allocs": 2, "setup_bytes": 38224,
         "first_allocs": 255, "first_bytes": 205115,
         "allocs_per_frame": r["allocs_per_frame"], "bytes_per_frame": r["bytes_per_frame"],
         "steady_live_growth": r["steady_live_growth"], "peak_live_delta": 7687}
        for r in records]}
    (leg_dir / "membench.json").write_text(json.dumps(payload))


class ChurnTierTests(unittest.TestCase):
    def tier(self, base, head, floor=ALLOCS_FLOOR):
        return cm.classify_churn(base, head, floor)[0]

    def test_pr352_step_reaches_the_hard_tier(self):
        """The measurement this whole job exists for. E-AC-3 encode churn
        stepped 67 -> 199 allocs/frame at PR #352 and only a post-merge push to
        main ever said so."""
        self.assertEqual(cm.HARD, self.tier(67.0, 199.1))

    def test_atmos_step_reaches_the_hard_tier(self):
        """The same merge's second finding, 106 -> 219."""
        self.assertEqual(cm.HARD, self.tier(106.0, 219.1))

    def test_exactly_at_the_hard_threshold_is_hard(self):
        """The boundary is inclusive, matching append_memory_history's `>=`.
        An exact doubling is the hard tier, not the soft one."""
        self.assertEqual(cm.HARD, self.tier(100.0, 200.0))

    def test_soft_tier_is_a_regression_row_not_a_hard_one(self):
        self.assertEqual("regression", self.tier(100.0, 125.0))

    def test_below_the_soft_tier_is_reported_but_not_a_regression(self):
        """A 5% climb is a code change - these counts do not wobble - so it
        gets a row saying `higher`, and no annotation."""
        self.assertEqual("higher", self.tier(100.0, 105.0))

    def test_identical_counts_are_unchanged(self):
        self.assertEqual("unchanged", self.tier(100.0, 100.0))

    def test_an_improvement_is_reported_as_lower(self):
        self.assertEqual("lower", self.tier(100.0, 60.0))

    def test_near_zero_baseline_is_judged_by_the_floor_not_a_ratio(self):
        """Under the floor a ratio says nothing useful: 2 -> 4 allocations is a
        doubling by arithmetic and a rounding error in fact. Both sides under
        the floor stay quiet; climbing past it is a soft finding."""
        self.assertEqual("higher", self.tier(2.0, 4.0))
        self.assertEqual("off a near-zero baseline", self.tier(2.0, 9.0))

    def test_near_zero_baseline_never_reaches_the_hard_tier(self):
        """The floor branch has no hard tier at all - by design, and mirrored
        from append_memory_history.check_churn, which `continue`s there."""
        self.assertNotEqual(cm.HARD, self.tier(1.0, 500.0))

    def test_the_floor_decides_which_ladder_a_metric_is_judged_by(self):
        """Identical numbers, opposite verdicts, entirely because the floors
        differ - 4,096 for bytes/frame against 5 for allocs/frame. 3,000 is
        under the bytes floor, so no ratio is computed and 3,000 -> 4,000 is a
        quiet row; it is far over the allocs floor, where the same pair is a
        33% climb and a soft regression."""
        self.assertEqual("higher", self.tier(3000.0, 4000.0, floor=BYTES_FLOOR))
        self.assertEqual("regression", self.tier(3000.0, 4000.0, floor=ALLOCS_FLOOR))


class LeakAttributionTests(unittest.TestCase):
    def test_preexisting_growth_unchanged_by_this_branch_is_quiet(self):
        """The two real numbers on this tree, both already over the 4 KiB warn
        line. A branch that does not touch them must not be annotated for
        them, or the gate is red on every PR and nobody reads it."""
        self.assertEqual((None, None), cm.classify_leak(4963, 4963))
        self.assertEqual((None, None), cm.classify_leak(5322, 5322))

    def test_crossing_the_warn_line_is_a_soft_finding(self):
        hard, note = cm.classify_leak(0, 9000)
        self.assertIs(False, hard)
        self.assertIn("9,000 bytes", note)

    def test_crossing_the_hard_line_is_a_hard_finding(self):
        hard, _ = cm.classify_leak(1024, amh.LIVE_GROWTH_HARD_BYTES)
        self.assertIs(True, hard)

    def test_growth_past_the_hard_threshold_counts_even_without_crossing(self):
        """A workload already over 1 MiB cannot cross it again. Growing by
        more than a mebibyte on top is the same leak and earns the same
        tier."""
        base = 2 * amh.LIVE_GROWTH_HARD_BYTES
        hard, _ = cm.classify_leak(base, base + amh.LIVE_GROWTH_HARD_BYTES)
        self.assertIs(True, hard)

    def test_already_over_the_hard_line_and_flat_is_quiet(self):
        """Attribution at the hard tier too: this branch did not do it."""
        base = 4 * amh.LIVE_GROWTH_HARD_BYTES
        self.assertEqual((None, None), cm.classify_leak(base, base))

    def test_growth_past_the_warn_threshold_counts_without_crossing(self):
        """5,322 -> 20,000 never crosses 4 KiB from below, and is still
        15 KiB of retained bytes this branch added."""
        hard, note = cm.classify_leak(5322, 20000)
        self.assertIs(False, hard)
        self.assertIn("20,000 bytes", note)

    def test_a_branch_that_fixes_a_leak_is_quiet(self):
        self.assertEqual((None, None), cm.classify_leak(9000, 0))


class AgreementTests(unittest.TestCase):
    """The pre-merge and post-merge gates must reach the same verdict.

    append_memory_history.check_churn compares a value against a trailing mean;
    compare_memory.classify_churn compares it against the merge base. Feed the
    post-merge gate a flat history at the base value and the two are being
    asked the identical question, so the tiers they return must match.
    """

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.history = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def post_merge_is_hard(self, base, head):
        """append_memory_history's verdict on `head`, against a flat series at
        `base`. Twelve records so the trailing window is this branch's own and
        baseline_for does not widen to siblings."""
        path = self.history / "memory-main.jsonl"
        with path.open("w") as f:
            for i in range(12):
                f.write(json.dumps({
                    "commit": f"{i:040x}",
                    "commit_date": f"2026-08-{i + 1:02d}T12:00:00+10:00",
                    "branch": "main", "leg": "linux-gcc", "config": "eac3_51_encode",
                    "allocs_per_frame": base, "bytes_per_frame": 28792.0,
                    "steady_live_growth": 0}) + "\n")
        findings = list(amh.check_churn(record(allocs=head), self.history, "main"))
        return any(hard for hard, _ in findings), findings

    def assert_agree(self, base, head):
        post_hard, findings = self.post_merge_is_hard(base, head)
        pre_tier = cm.classify_churn(base, head, ALLOCS_FLOOR)[0]
        pre_hard = pre_tier == cm.HARD
        self.assertEqual(post_hard, pre_hard,
                         f"{base} -> {head}: pre-merge said {pre_tier!r}, post-merge "
                         f"hard={post_hard} ({[m for _, m in findings]})")
        return pre_tier

    def test_the_pr352_step_is_hard_on_both_sides(self):
        self.assertEqual(cm.HARD, self.assert_agree(67.0, 199.1))

    def test_a_soft_step_is_soft_on_both_sides(self):
        """Both must decline to fail. The pre-merge row says `regression` and
        the post-merge run emits a warning; neither blocks."""
        tier = self.assert_agree(67.0, 85.0)
        self.assertEqual("regression", tier)

    def test_a_step_under_the_soft_tier_is_quiet_on_both_sides(self):
        self.assertEqual("higher", self.assert_agree(67.0, 79.0))

    def test_a_flat_series_is_quiet_on_both_sides(self):
        self.assertEqual("unchanged", self.assert_agree(67.0, 67.0))

    def test_an_improvement_is_quiet_on_both_sides(self):
        self.assertEqual("lower", self.assert_agree(67.0, 30.0))


class TableTests(unittest.TestCase):
    def test_a_workload_added_by_this_pr_is_a_row_not_an_annotation(self):
        """A new workload has no base to compare against, and must never fail
        a PR for having been added."""
        lines, notes = cm.churn_table(
            side(), side(record(config="ecpl_51_encode", allocs=140.0)),
            "allocs_per_frame", "allocs/frame", ALLOCS_FLOOR)
        self.assertEqual([], notes)
        self.assertTrue(any("added by this PR" in line for line in lines))

    def test_a_workload_removed_by_this_pr_is_a_row_not_an_annotation(self):
        lines, notes = cm.churn_table(
            side(record()), side(), "allocs_per_frame", "allocs/frame", ALLOCS_FLOOR)
        self.assertEqual([], notes)
        self.assertTrue(any("removed by this PR" in line for line in lines))

    def test_hard_rows_are_annotated_with_the_hard_tier_marker(self):
        """The marker `(hard tier)` is the string main() counts to decide the
        job's verdict, so it has to be in the annotation, not only the row."""
        _, notes = cm.churn_table(
            side(record(allocs=67.0)), side(record(allocs=199.1)),
            "allocs_per_frame", "allocs/frame", ALLOCS_FLOOR)
        self.assertEqual(1, len(notes))
        self.assertIn("(hard tier)", notes[0])

    def test_a_flat_tree_renders_one_line_instead_of_six_zero_rows(self):
        lines, notes = cm.leak_table(side(record()), side(record()))
        self.assertEqual([], notes)
        self.assertTrue(any("No workload retained bytes" in line for line in lines))


class EndToEndTests(unittest.TestCase):
    """main() over real results trees, so load_side's use of
    append_memory_history.load_leg_results is exercised over the directory
    layout CI produces."""

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)
        self.output = self.root / "github_output.txt"
        self.summary = self.root / "summary.md"

    def run_main(self, base_records, head_records):
        write_side(self.root / "base", *base_records)
        write_side(self.root / "head", *head_records)
        argv = ["compare_memory.py",
                "--base-dir", str(self.root / "base"),
                "--head-dir", str(self.root / "head"),
                "--summary-out", str(self.summary)]
        with unittest.mock.patch.object(sys, "argv", argv), \
                unittest.mock.patch.dict("os.environ", {"GITHUB_OUTPUT": str(self.output)}), \
                contextlib.redirect_stdout(io.StringIO()):
            code = cm.main()
        return code, self.output.read_text(), self.summary.read_text()

    def test_a_hard_regression_reports_true_and_still_exits_zero(self):
        """The exit code is deliberately 0 - the verdict travels as an output
        because the job running this is continue-on-error and would discard a
        non-zero exit."""
        code, output, summary = self.run_main([record(allocs=67.0)], [record(allocs=199.1)])
        self.assertEqual(0, code)
        self.assertIn("hard_regression=true", output)
        self.assertIn("eac3_51_encode", summary)

    def test_a_clean_comparison_reports_false(self):
        code, output, _ = self.run_main([record()], [record()])
        self.assertEqual(0, code)
        self.assertIn("hard_regression=false", output)

    def test_a_missing_base_side_is_a_skip_reporting_false(self):
        """A PR whose merge base predates ac3membench, or a runner that could
        not build one side. Explicitly false, so the gate job is never left
        interpreting an unset output."""
        write_side(self.root / "head", record())
        argv = ["compare_memory.py",
                "--base-dir", str(self.root / "absent"),
                "--head-dir", str(self.root / "head"),
                "--summary-out", str(self.summary)]
        with unittest.mock.patch.object(sys, "argv", argv), \
                unittest.mock.patch.dict("os.environ", {"GITHUB_OUTPUT": str(self.output)}), \
                contextlib.redirect_stdout(io.StringIO()):
            code = cm.main()
        self.assertEqual(0, code)
        self.assertIn("hard_regression=false", self.output.read_text())

    def test_a_leak_this_branch_introduced_reports_true(self):
        code, output, _ = self.run_main(
            [record(live=0)], [record(live=amh.LIVE_GROWTH_HARD_BYTES)])
        self.assertEqual(0, code)
        self.assertIn("hard_regression=true", output)

    def test_a_leak_the_merge_base_already_had_reports_false(self):
        base = 4 * amh.LIVE_GROWTH_HARD_BYTES
        code, output, _ = self.run_main([record(live=base)], [record(live=base)])
        self.assertEqual(0, code)
        self.assertIn("hard_regression=false", output)


if __name__ == "__main__":
    unittest.main()
