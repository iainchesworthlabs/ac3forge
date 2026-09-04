"""Unit tests for write_measurement_badges.py, the README's measurement badges.

stdlib `unittest`, not pytest, for the same reason the scripts under test are
stdlib-only: this runs in ci.yml's script-lint job, which installs ruff,
shellcheck and actionlint and nothing else, and a test that needs a new pinned
dependency to run is a test that will not be run.

The regression this file exists to hold down is a badge that disagreed with the
page it links to. Per-channel SNR floors (PR #503) taught
docs/performance-quality.md's Decode accuracy card to pick a check by MARGIN
and report the channel that owns it; accuracy_badge() was not taught the same
thing and kept picking by the old scalar `worst_db - threshold_db` and printing
`worst_db`. On the same commit and the same CI leg that made the README badge
read "18.3 dB SNR" - a dither-dominated surround - while the card one click
away read "58.1 dB SNR". Both numbers were correct; they answered different
questions. test_badge_agrees_with_the_page_it_links_to is that scenario.

The second defect was the colour: `worst_db >= threshold_db` is a scalar test
that cannot see a per-channel breach, so the badge could stay green on a build
the gold-reference gate itself fails.
test_per_channel_breach_is_amber_where_the_scalar_test_stayed_green pins it.

Run: python3 -m unittest discover -s tools/ci -p 'test_*.py'
"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import write_measurement_badges as badges

LABELS = ["L", "R", "C", "LFE", "Ls", "Rs"]


def record(channels_db, thresholds_db, leg="macos-llvm"):
    """A quality record shaped the way compare_wav.py's --json-out writes one.

    worst_db/threshold_db are derived rather than passed so a test cannot
    accidentally describe a record that append_quality_history.py would never
    write: they are the minimum channel and the minimum floor, which is exactly
    what made the scalar comparison blind to a high-floor channel breaching.
    """
    headrooms = [db - floor for db, floor in zip(channels_db, thresholds_db, strict=True)]
    tightest = min(range(len(headrooms)), key=lambda i: headrooms[i])
    return {
        "leg": leg,
        "channel_labels": LABELS[:len(channels_db)],
        "channels_db": list(channels_db),
        "thresholds_db": list(thresholds_db),
        "worst_db": min(channels_db),
        "threshold_db": min(thresholds_db),
        "tightest_channel": tightest,
        "tightest_headroom_db": headrooms[tightest],
    }


class AccuracyBadgeTest(unittest.TestCase):

    def test_badge_agrees_with_the_page_it_links_to(self):
        """The tightest margin, not the lowest number - the 18.3-vs-58.1 case.

        Two checks from one commit. The first has the lowest SNR anywhere in
        the set (Ls at 18.3 dB) but 1.3 dB of margin; the second's front-left
        sits at 58.1 dB with only 1.1 dB over its own much higher floor. The
        second is the one closer to failing, so it is the one both the badge
        and the card report.
        """
        lowest_snr = record([60.0, 60.0, 60.0, 85.0, 18.3, 19.0],
                            [42.0, 47.0, 49.0, 81.0, 17.0, 17.0])
        tightest_margin = record([58.1, 60.0, 62.0, 85.0, 25.0, 25.0],
                                 [57.0, 47.0, 49.0, 81.0, 17.0, 17.0])

        # The old computation, spelled out so the regression is unmistakable:
        # it picks the other record, and reports its worst channel.
        stale = min([lowest_snr, tightest_margin],
                    key=lambda r: r["worst_db"] - r["threshold_db"])
        self.assertEqual(f"{stale['worst_db']:.1f} dB SNR", "18.3 dB SNR")

        got = badges.accuracy_badge([lowest_snr, tightest_margin])
        self.assertEqual(got["message"], "58.1 dB SNR")
        self.assertEqual(got["color"], badges.GREEN)

    def test_per_channel_breach_is_amber_where_the_scalar_test_stayed_green(self):
        """A centre channel under its own floor, with the surrounds still clear.

        C at 48.0 dB is 1 dB below its 49 dB floor - the gold-reference gate
        fails this build. worst_db is Ls at 18.0, which clears the scalar 17.0
        floor, so the old colour rule called it green.
        """
        breached = record([60.0, 60.0, 48.0, 85.0, 18.0, 19.0],
                          [42.0, 47.0, 49.0, 81.0, 17.0, 17.0])
        self.assertGreaterEqual(breached["worst_db"], breached["threshold_db"])

        got = badges.accuracy_badge([breached])
        self.assertEqual(got["color"], badges.AMBER)
        self.assertEqual(got["message"], "48.0 dB SNR")

    def test_records_without_per_channel_floors_use_the_old_computation(self):
        """Pre-PR-503 records carry only worst_db and a scalar threshold_db.

        They are not dropped from the comparison - a history file spans both
        formats, and dropping the older half would silently narrow what the
        badge summarises.
        """
        legacy = [
            {"leg": "linux-gcc", "worst_db": 22.7, "threshold_db": 16.0},
            {"leg": "macos-llvm", "worst_db": 58.4, "threshold_db": 57.0},
        ]
        got = badges.accuracy_badge(legacy)
        self.assertEqual(got["message"], "58.4 dB SNR")
        self.assertEqual(got["color"], badges.GREEN)

    def test_legacy_and_per_channel_records_compare_on_one_scale(self):
        """A mixed history still picks the genuinely tightest check.

        The legacy record has 0.5 dB of margin against its scalar gate; the
        per-channel one has 1.1 dB. Margin is margin, whichever way it was
        recorded, so the legacy check wins.
        """
        legacy = {"leg": "linux-gcc", "worst_db": 16.5, "threshold_db": 16.0}
        modern = record([58.1, 60.0, 62.0, 85.0, 25.0, 25.0],
                        [57.0, 47.0, 49.0, 81.0, 17.0, 17.0])
        got = badges.accuracy_badge([modern, legacy])
        self.assertEqual(got["message"], "16.5 dB SNR")

    def test_out_of_range_tightest_channel_does_not_raise(self):
        """Python indexes differently than the card's JavaScript does.

        An index past the end is `undefined` in JS but an IndexError here, and
        a negative one wraps silently instead of reading as missing - so a
        malformed record must not take the badge writer down mid-commit.
        """
        for bad_index in (99, -1, None, "C"):
            with self.subTest(tightest_channel=bad_index):
                rec = record([58.1, 60.0, 62.0, 85.0, 25.0, 25.0],
                             [57.0, 47.0, 49.0, 81.0, 17.0, 17.0])
                rec["tightest_channel"] = bad_index
                got = badges.accuracy_badge([rec])
                self.assertEqual(got["message"], "58.1 dB SNR")

    def test_no_measurements_is_grey_rather_than_a_number(self):
        """No data is its own state - a badge must not invent one."""
        got = badges.accuracy_badge([])
        self.assertEqual(got["color"], badges.GREY)
        self.assertEqual(got["message"], "no data")

    def test_rows_missing_the_scored_fields_are_ignored(self):
        """A performance row sharing the commit must not be read as a check."""
        got = badges.accuracy_badge([{"leg": "linux-gcc", "ms_per_frame": 0.31}])
        self.assertEqual(got["message"], "no data")


if __name__ == "__main__":
    unittest.main()
