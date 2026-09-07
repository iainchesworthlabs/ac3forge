"""Unit tests for append_memory_history.py's churn gate.

stdlib `unittest`, not pytest, for the same reason the scripts under test are
stdlib-only: this runs in ci.yml's script-lint job, which installs ruff,
shellcheck and actionlint and nothing else, and a test that needs a new pinned
dependency to run is a test that will not be run.

The regression this file exists to hold down is a gate with no opinion. The
series this script keeps is PER BRANCH - memory-<branch>.jsonl - so a branch
rename, a release branch, or a gitflow-to-trunk switch starts one from empty.
The trailing mean is then computed over a file that holds nothing, the old
`baseline is None: continue` returned quietly, and the record was written with
nothing having judged it. Not a warning, not a skip anyone could see in the log:
a pass.

The near-miss that motivates it is on the record. E-AC-3 encode churn stepped
67 -> 199 allocs/frame across acc4f6e0's trunk switch (PR #352's per-channel
exponent-run planner). memory-main.jsonl happened to already hold exactly ONE
record for that series, so the window was thin rather than absent and the hard
tier did fire. Had the switch landed one commit earlier the file would have been
empty, and a 197% step would have been recorded as the series' own first data
point - the baseline every later run was then measured against.

test_step_across_a_branch_switch_is_gated is that scenario with the file empty.
test_series_with_no_history_anywhere_is_not_silent covers the genuinely new
workload, which must stay a warning rather than a failure - otherwise no
workload could ever be added - but must not be invisible.

Run: python3 -m unittest discover -s tools/ci -p 'test_*.py'
"""

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import append_memory_history as amh


def record(config="eac3_51_encode", allocs=67.0, byts=28792.0, leg="linux-gcc"):
    return {"leg": leg, "config": config,
            "allocs_per_frame": allocs, "bytes_per_frame": byts,
            "steady_live_growth": 0}


def write_series(path: Path, values, config="eac3_51_encode", leg="linux-gcc",
                 start_day=1):
    """One JSONL file, one record per value, commit dates ascending so
    baseline_for() can order samples drawn from more than one file."""
    with path.open("w") as f:
        for i, value in enumerate(values):
            f.write(json.dumps({
                "commit": f"{i:040x}",
                "commit_date": f"2026-08-{start_day + i:02d}T12:00:00+10:00",
                "branch": path.stem.removeprefix("memory-"),
                "leg": leg, "config": config,
                "allocs_per_frame": value, "bytes_per_frame": value * 430.0,
                "steady_live_growth": 0,
            }) + "\n")


class ChurnGateTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.history = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)

    def findings(self, rec, branch="main"):
        return list(amh.check_churn(rec, self.history, branch))

    def test_step_across_a_branch_switch_is_gated(self):
        """The regression. develop carries the established series; main's file
        does not exist yet because the trunk switch just happened. A 197% step
        on the first main record must still reach the hard tier."""
        write_series(self.history / "memory-develop.jsonl", [67.0] * 12)
        self.assertFalse((self.history / "memory-main.jsonl").exists())

        findings = self.findings(record(allocs=199.11, byts=53845.7))
        hard = [msg for is_hard, msg in findings if is_hard]

        self.assertTrue(hard, "a 197% step on a freshly-started series was not "
                              "gated; check_churn returned no hard finding")
        self.assertTrue(any("allocs/frame" in msg for msg in hard))

    def test_thin_branch_file_widens_rather_than_gating_on_one_point(self):
        """One record on this branch is a point, not a trend. The baseline
        widens to the sibling series instead of calling a single sample a
        trailing 10-run mean."""
        write_series(self.history / "memory-develop.jsonl", [67.0] * 12)
        write_series(self.history / "memory-main.jsonl", [67.0], start_day=20)

        findings = self.findings(record(allocs=199.11, byts=53845.7))
        self.assertTrue([m for h, m in findings if h])
        self.assertTrue(any("sibling branch files" in m for _, m in findings),
                        "a thin branch file should say the baseline was widened")

    def test_established_branch_series_does_not_widen(self):
        """A healthy branch file is judged on its own history. Nothing is
        drawn from a sibling, so a divergent sibling cannot move the verdict."""
        write_series(self.history / "memory-main.jsonl", [67.0] * 12)
        write_series(self.history / "memory-develop.jsonl", [999.0] * 12,
                     start_day=20)

        findings = self.findings(record(allocs=67.0, byts=28792.0))
        self.assertEqual([], [m for h, m in findings if h])
        self.assertFalse(any("sibling branch files" in m for _, m in findings))

    def test_series_with_no_history_anywhere_is_not_silent(self):
        """A workload being added has no baseline and cannot have one. That is
        a warning naming it as ungated - never a hard failure, or no new
        workload could be added, and never silence."""
        findings = self.findings(record(config="ecpl_51_encode", allocs=140.0))

        self.assertEqual([], [m for h, m in findings if h])
        self.assertTrue(findings, "an ungated series produced no annotation")
        self.assertTrue(any("NOTHING gated this value" in m for _, m in findings))

    def test_steady_series_within_threshold_stays_quiet(self):
        """The ordinary case: no annotations at all when nothing moved."""
        write_series(self.history / "memory-main.jsonl", [67.0] * 12)
        self.assertEqual([], self.findings(record(allocs=67.0, byts=28792.0)))


if __name__ == "__main__":
    unittest.main()
