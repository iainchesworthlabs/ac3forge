"""Unit tests for write_measurement_badges.py, the README's measurement badges.

stdlib `unittest`, not pytest, for the same reason the scripts under test are
stdlib-only: this runs in ci.yml's script-lint job, which installs ruff,
shellcheck and actionlint and nothing else, and a test that needs a new pinned
dependency to run is a test that will not be run.

Three regressions this file exists to hold down.

A badge that disagreed with the page it links to. Per-channel SNR floors (PR
#503) taught docs/performance-quality.md's Decode accuracy card to pick a check
by MARGIN and report the channel that owns it; accuracy_badge() was not taught
the same thing and kept picking by the old scalar `worst_db - threshold_db` and
printing `worst_db`. On the same commit and the same CI leg that made the
README badge read "18.3 dB SNR" - a dither-dominated surround - while the card
one click away read "58.1 dB SNR". Both numbers were correct; they answered
different questions. test_badge_agrees_with_the_page_it_links_to is that
scenario, and the Node tests at the end hold the two site scripts to the
badges.

A badge whose colour could not see a per-channel breach. `worst_db >=
threshold_db` is a scalar test, so the badge could stay green on a build the
gold-reference gate itself fails.
test_per_channel_breach_is_amber_where_the_scalar_test_stayed_green pins it.

A figure with no codec. "2.0 dB SNR" and "2009 KB/frame" read as the project's
headline numbers and were the tightest E-AC-3 check and the AC-4 5.1 encoder's
allocator traffic. Every figure is now one codec's worst case, with its
workload named, and the tests below hold each codec to its own rows.
WorkloadTableTest holds the table that says which codec a workload belongs to:
a workload added to tests/performance/ without a row in it fails there, in the
pull request that adds it, and not later on main where the badge job would
refuse it.

Run: python3 -m unittest discover -s tools/ci -p 'test_*.py'
"""

import contextlib
import io
import json
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import unittest.mock as mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import write_measurement_badges as badges

REPO = Path(__file__).resolve().parents[2]
LABELS = ["L", "R", "C", "LFE", "Ls", "Rs"]
AC4_BUDGET = 2048 / 48.0
# What the site's scripts print for "8x" and for a range. By code point, as ruff
# rejects them as ambiguous characters in a string.
MULTIPLICATION_SIGN = chr(0xD7)
EN_DASH = chr(0x2013)


def record(channels_db, thresholds_db, leg="macos-llvm", codec="ac3", bitrate_kbps=448,
           check="ext_ac3_51_448_dee"):
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
        "codec": codec,
        "check": check,
        "bitrate_kbps": bitrate_kbps,
        "channel_labels": LABELS[:len(channels_db)],
        "channels_db": list(channels_db),
        "thresholds_db": list(thresholds_db),
        "worst_db": min(channels_db),
        "threshold_db": min(thresholds_db),
        "tightest_channel": tightest,
        "tightest_headroom_db": headrooms[tightest],
    }


def eac3_transient(snr=2.03, floor=1.0):
    """The tightest E-AC-3 check on main at the time of writing."""
    return record([snr + 0.24, snr], [floor, floor], leg="linux-gcc", codec="eac3",
                  bitrate_kbps=128, check="ext_eac3_transient_stereo_128_dee_source")


def perf(config, ms, budget=32.0, leg="linux-gcc"):
    return {"config": config, "ms_per_frame": ms, "real_time_budget_ms_per_frame": budget,
            "leg": leg}


def mem(config, bytes_per_frame, growth=0, frames=200, allocs=10.0):
    return {"config": config, "bytes_per_frame": bytes_per_frame, "steady_live_growth": growth,
            "frames": frames, "allocs_per_frame": allocs, "leg": "linux-gcc"}


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
        self.assertEqual(got["message"],
                         "AC-3 5.1 448 kbps: 58.1 dB | E-AC-3 no data | AC-4 no data")
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
        self.assertTrue(got["message"].startswith("AC-3 5.1 448 kbps: 48.0 dB"))

    def test_records_without_per_channel_floors_use_the_old_computation(self):
        """Pre-PR-503 records carry only worst_db and a scalar threshold_db.

        They are not dropped from the comparison - a history file spans both
        formats, and dropping the older half would silently narrow what the
        badge summarises. Nothing names their workload, so the figure stands
        alone after the codec.
        """
        legacy = [
            {"leg": "linux-gcc", "codec": "ac3", "worst_db": 22.7, "threshold_db": 16.0},
            {"leg": "macos-llvm", "codec": "ac3", "worst_db": 58.4, "threshold_db": 57.0},
        ]
        got = badges.accuracy_badge(legacy)
        self.assertEqual(got["message"], "AC-3 58.4 dB | E-AC-3 no data | AC-4 no data")
        self.assertEqual(got["color"], badges.GREEN)

    def test_legacy_and_per_channel_records_compare_on_one_scale(self):
        """A mixed history still picks the tightest check.

        The legacy record has 0.5 dB of margin against its scalar gate; the
        per-channel one has 1.1 dB. Margin is margin, whichever way it was
        recorded, so the legacy check wins.
        """
        legacy = {"leg": "linux-gcc", "codec": "ac3", "worst_db": 16.5, "threshold_db": 16.0}
        modern = record([58.1, 60.0, 62.0, 85.0, 25.0, 25.0],
                        [57.0, 47.0, 49.0, 81.0, 17.0, 17.0])
        got = badges.accuracy_badge([modern, legacy])
        self.assertTrue(got["message"].startswith("AC-3 16.5 dB |"))

    def test_out_of_range_tightest_channel_does_not_raise(self):
        """Python indexes differently than the card's JavaScript does.

        An index past the end is `undefined` in JS but an IndexError here, and
        a negative one wraps silently instead of reading as missing - so a
        malformed record must not take the badge writer down mid-commit.
        """
        for bad_index in (99, -1, None, "C", True):
            with self.subTest(tightest_channel=bad_index):
                rec = record([58.1, 60.0, 62.0, 85.0, 25.0, 25.0],
                             [57.0, 47.0, 49.0, 81.0, 17.0, 17.0])
                rec["tightest_channel"] = bad_index
                got = badges.accuracy_badge([rec])
                self.assertIn(": 58.1 dB", got["message"])

    def test_no_measurements_is_grey_rather_than_a_number(self):
        """No data is its own state - a badge must not invent one."""
        got = badges.accuracy_badge([])
        self.assertEqual(got["color"], badges.GREY)
        self.assertEqual(got["message"], "no data")

    def test_rows_missing_the_scored_fields_are_ignored(self):
        """A performance row sharing the commit must not be read as a check."""
        got = badges.accuracy_badge([{"leg": "linux-gcc", "ms_per_frame": 0.31}])
        self.assertEqual(got["message"], "no data")

    def test_each_codec_reports_its_own_tightest_check(self):
        """One codec's margin says nothing about another's.

        The AC-3 check has 1.1 dB of margin and the E-AC-3 one 1.03 dB, and the
        looser E-AC-3 row must not displace the tighter one: each codec is
        compared with its own rows and no other's. The old badge, with one
        figure, could only ever show whichever codec happened to be tightest.
        """
        rows = [
            record([58.1, 63.8, 58.1, 82.2, 22.8, 22.7], [56.0, 62.0, 57.0, 81.0, 21.0, 21.0]),
            eac3_transient(),
            record([66.1, 66.2], [65.0, 65.0], codec="eac3", bitrate_kbps=256, check="eac3"),
        ]
        got = badges.accuracy_badge(rows)
        self.assertEqual(
            got["message"],
            "AC-3 5.1 448 kbps: 58.1 dB | E-AC-3 stereo 128 kbps transient: 2.0 dB | AC-4 no data")

    def test_a_breach_in_one_codec_colours_the_whole_badge_amber(self):
        breached = eac3_transient(snr=0.8, floor=1.0)
        healthy = record([58.1, 63.8], [56.0, 62.0])
        self.assertEqual(badges.accuracy_badge([healthy, breached])["color"], badges.AMBER)
        self.assertEqual(badges.accuracy_badge([healthy, eac3_transient()])["color"],
                         badges.GREEN)

    def test_ac4_is_shown_and_takes_no_part_in_the_colour(self):
        """AC-4's decode quality has no floors, so it is shown and not judged.

        Its lowest minimum SNR comes from ac4-quality-*.jsonl. The rows have no
        thresholds, so with nothing else in the badge the colour is blue, and
        beside gated codecs it leaves their colour alone.
        """
        ac4 = [{"leg": "ac4-20-music-192", "min_snr_db": 34.9, "mos_lqo": 4.72},
               {"leg": "ac4-ims-music-64-2997", "min_snr_db": 16.67, "mos_lqo": 4.64},
               {"leg": "ac4-20-speech-128", "min_snr_db": 38.1, "mos_lqo": 4.39}]
        alone = badges.accuracy_badge([], ac4)
        self.assertEqual(
            alone["message"],
            "AC-3 no data | E-AC-3 no data | AC-4 Dolby streams: 16.7 dB min, MOS-LQO 4.39")
        self.assertEqual(alone["color"], badges.INFO)

        beside = badges.accuracy_badge([record([58.1, 63.8], [56.0, 62.0])], ac4)
        self.assertEqual(beside["color"], badges.GREEN)
        self.assertTrue(beside["message"].endswith("AC-4 Dolby streams: 16.7 dB min, MOS-LQO 4.39"))

        # A stream with only one of the two scores shows that one.
        only_mos = badges.accuracy_badge([], [{"leg": "x", "mos_lqo": 4.5}])
        self.assertTrue(only_mos["message"].endswith("AC-4 Dolby streams: MOS-LQO 4.50"))

        summary = badges.ac4_quality(ac4)
        self.assertEqual((summary["snr_leg"], summary["mos"], summary["mos_leg"],
                          summary["mos_best"]),
                         ("ac4-ims-music-64-2997", 4.39, "ac4-20-speech-128", 4.72))
        self.assertIsNone(badges.ac4_quality([{"leg": "x"}]))

    def test_an_unlisted_codec_label_is_refused(self):
        """A codec this page does not name must not be filed under another."""
        rec = record([58.1, 63.8], [56.0, 62.0], codec="ac4")
        with self.assertRaises(badges.UnknownWorkload):
            badges.accuracy_badge([rec])
        del rec["codec"]
        with self.assertRaises(badges.UnknownWorkload):
            badges.accuracy_badge([rec])

    def test_the_workload_is_named_by_layout_rate_and_signal(self):
        """"E-AC-3 stereo 128 kbps transient", as the user reads it, not `_dee_source`."""
        self.assertEqual(badges.check_words(eac3_transient()), "stereo 128 kbps transient")
        five_one = record([58.1] * 6, [56.0] * 6)
        self.assertEqual(badges.check_words(five_one), "5.1 448 kbps")
        self.assertEqual(badges.check_words({"check": "x"}), "")
        self.assertEqual(badges.layout_of({"channels_db": [1.0] * 8}), "7.1")
        self.assertEqual(badges.layout_of({"channels_db": [1.0] * 3}), "3 channels")


class SpeedBadgeTest(unittest.TestCase):
    def test_speed_is_the_slowest_workload_of_each_codec(self):
        """Each codec is held to its own rows, and the words say which one won."""
        rows = [
            perf("plain_51", 4.0), perf("ac3_51_decode", 0.08),
            perf("atmos_4obj", 8.0), perf("eac3_51_auto", 0.42), perf("eac3_51_decode", 0.08),
            perf("ac4_51_encode", 3.83, AC4_BUDGET), perf("ac4_stereo_encode", 2.2, AC4_BUDGET),
            perf("ac4_51_decode", 0.43, AC4_BUDGET),
        ]
        got = badges.speed_badge(rows)
        self.assertEqual(
            got["message"],
            "AC-3 5.1 encode: 8x | E-AC-3 Atmos 4-object encode: 4x | AC-4 5.1 encode: 11x")
        self.assertEqual(got["color"], badges.GREEN)

    def test_speed_ranks_workloads_by_their_multiple_and_not_by_ms_per_frame(self):
        """The tile on the home page used to take the row with the LARGEST ms/frame.

        An AC-4 frame is 2 048 samples, 42.67 ms: at 4 ms/frame it has more
        headroom (10.7x) than an AC-3 row at 3.2 ms against 32 ms (10x). Within
        one codec the frame budgets can differ too, so the slower row is the one
        with the smaller multiple, and that is not always the larger ms/frame.
        """
        rows = [perf("ac4_51_encode", 4.0, AC4_BUDGET),   # 10.7x
                perf("ac4_stereo_encode", 3.0, 24.0)]     # 8x, though it takes less time
        self.assertEqual(badges.slowest_per_codec(rows)["AC-4"]["config"], "ac4_stereo_encode")

    def test_a_decode_workload_that_is_slower_wins_and_is_named_as_one(self):
        """The badge does not say "encode" unless the slowest row is one."""
        rows = [perf("plain_51", 4.0), perf("ac3_51_decode", 32.0)]   # 8x and 1x
        got = badges.speed_badge(rows)
        self.assertTrue(got["message"].startswith("AC-3 5.1 decode: 1x |"))

    def test_one_codec_below_real_time_makes_the_badge_amber(self):
        """Worst case, not average: one workload below real time goes amber
        even when every other one is far above it, and 0.5x is not shown as 1x."""
        b = badges.speed_badge([perf("plain_51", 64.0), perf("ac4_51_encode", 1.0, AC4_BUDGET)])
        self.assertEqual(b["color"], badges.AMBER)
        self.assertTrue(b["message"].startswith("AC-3 5.1 encode: 0.50x |"))
        self.assertEqual(badges.format_multiple(0.96), "0.96")
        self.assertEqual(badges.format_multiple(1.0), "1")

    def test_a_row_without_a_budget_is_held_to_the_ac3_frame(self):
        b = badges.speed_badge([{"config": "plain_51", "ms_per_frame": 2.0}])
        self.assertTrue(b["message"].startswith("AC-3 5.1 encode: 16x |"))
        self.assertEqual(b["color"], badges.GREEN)

    def test_untimed_rows_give_no_data_and_are_not_classified(self):
        """Nothing to summarise, so nothing to file: a row with no time is skipped
        before its config is looked up."""
        b = badges.speed_badge([{"config": "plain_51", "ms_per_frame": 0},
                                {"config": "not_a_workload", "ms_per_frame": "n/a"},
                                {"ms_per_frame": True}])
        self.assertEqual((b["message"], b["color"]), (badges.NO_DATA, badges.GREY))

    def test_the_first_row_wins_a_tie(self):
        rows = [perf("plain_51", 4.0, leg="linux-gcc"),
                perf("plain_51_fast_mdct", 4.0, leg="linux-gcc-arm64")]
        self.assertEqual(badges.slowest_per_codec(rows)["AC-3"]["config"], "plain_51")

    def test_a_workload_the_table_does_not_list_is_refused(self):
        with self.assertRaises(badges.UnknownWorkload) as raised:
            badges.speed_badge([perf("plain_51", 4.0), perf("truehd_51_encode", 4.0)])
        self.assertIn("truehd_51_encode", str(raised.exception))
        self.assertIn("WORKLOADS", str(raised.exception))


class MemoryBadgeTest(unittest.TestCase):
    def test_allocation_is_the_heaviest_workload_of_each_codec(self):
        rows = [mem("ac3_51_encode", 20110.6), mem("ac3_51_decode", 43256),
                mem("atmos_4obj_decode", 82304), mem("eac3_51_encode", 34581.9),
                mem("ac4_stereo_encode", 832096), mem("ac4_51_encode", 2057000.0)]
        b = badges.memory_badge(rows)
        self.assertEqual(
            b["message"],
            "AC-3 5.1 decode: 42 KB | E-AC-3 Atmos 4-object decode: 80 KB | "
            "AC-4 5.1 encode: 2009 KB")
        self.assertEqual(b["label"], "allocation per frame (heaviest workload)")

    def test_allocation_has_no_gate_so_it_is_not_coloured_by_one(self):
        """The old badge showed a churn figure in the colour of a retention one.

        A reader saw orange beside "2009 KB/frame" and had no way to tell that
        it meant something else held memory. Allocation is blue whatever is
        retained, and the retention has a badge of its own.
        """
        rows = [mem("ac3_51_encode", 20110.6, growth=0),
                mem("ecpl_51_encode", 28866.3, growth=14592)]
        self.assertEqual(badges.memory_badge(rows)["color"], badges.INFO)
        self.assertEqual(badges.growth_badge(rows)["color"], badges.AMBER)
        self.assertEqual(badges.memory_badge([{}])["color"], badges.GREY)

    def test_live_growth_names_the_workload_that_retains_memory(self):
        rows = [mem("ac3_51_encode", 1, growth=2048), mem("eac3_51_encode", 1, growth=6976),
                mem("ecpl_51_encode", 1, growth=14592), mem("ac4_51_encode", 1, growth=11808),
                mem("ac4_51_decode", 1, growth=2816)]
        b = badges.growth_badge(rows)
        self.assertEqual(
            b["message"],
            "AC-3 5.1 encode: 2.0 KiB | E-AC-3 5.1 enhanced-coupling encode: 14.3 KiB | "
            "AC-4 5.1 encode: 11.5 KiB")
        self.assertEqual(b["color"], badges.AMBER)

    def test_live_growth_is_amber_from_the_warning_line_and_not_only_above_it(self):
        """append_memory_history.py's check_leak warns at >= 4096, so 4096 is amber."""
        self.assertEqual(badges.growth_badge([mem("ac3_51_encode", 1, growth=4095)])["color"],
                         badges.GREEN)
        self.assertEqual(badges.growth_badge([mem("ac3_51_encode", 1, growth=4096)])["color"],
                         badges.AMBER)

    def test_ties_round_up_as_the_site_scripts_round_them(self):
        """14592 B is exactly 14.25 KiB: format() would print 14.2, Math.round 14.3."""
        b = badges.growth_badge([mem("ecpl_51_encode", 1, growth=14592)])
        self.assertIn("14.3 KiB", b["message"])
        self.assertEqual((badges.round0(2.5), badges.round1(14.25)), (3, 14.3))

    def test_missing_fields_are_no_data_for_that_codec(self):
        b = badges.growth_badge([mem("ac3_51_encode", 1, growth=None),
                                 mem("ac4_51_encode", 1, growth=100)])
        self.assertEqual(b["message"], "AC-3 no data | E-AC-3 no data | AC-4 5.1 encode: 0.1 KiB")
        self.assertEqual(badges.growth_badge([])["message"], badges.NO_DATA)

    def test_a_workload_the_table_does_not_list_is_refused(self):
        rows = [mem("ac3_51_encode", 1), mem("iamf_51_encode", 1)]
        for build in (badges.memory_badge, badges.growth_badge):
            with self.subTest(build=build.__name__), self.assertRaises(badges.UnknownWorkload):
                build(rows)


# The naming convention WORKLOADS documents: what a config's first word says about its codec.
PREFIX_CODEC = {"ac3": "AC-3", "plain": "AC-3", "eac3": "E-AC-3", "ecpl": "E-AC-3",
                "atmos": "E-AC-3", "ac4": "AC-4"}


class WorkloadTableTest(unittest.TestCase):
    """The table that says which codec a `config` belongs to, and how to say it."""

    def test_names_follow_the_prefix_convention(self):
        """ac3_* and plain_* are AC-3, eac3_*, ecpl_* and atmos_* E-AC-3, ac4_* AC-4."""
        for config, (codec, _words) in badges.WORKLOADS.items():
            with self.subTest(config=config):
                self.assertEqual(codec, PREFIX_CODEC[config.split("_")[0]])
        self.assertEqual(set(PREFIX_CODEC.values()), set(badges.CODECS))

    def test_every_description_says_which_direction_it_runs(self):
        """Encode and decode differ by an order of magnitude in time and allocation."""
        for config, (_codec, words) in badges.WORKLOADS.items():
            with self.subTest(config=config):
                direction = re.search(r"\b(encode|decode)\b", words)
                self.assertIsNotNone(direction, words)
                if config.endswith(("_encode", "_decode")):
                    self.assertEqual(direction.group(1), config.rsplit("_", 1)[1])

    def test_every_benchmarked_workload_is_classified(self):
        """A workload added to the benches without a row in the table fails HERE.

        The badge job refuses an unlisted config too, but it runs on main after
        the merge and takes that commit's trend data with it. The names are the
        `wanted("...")` arguments of bench_encoder.cpp, which CI never passes
        --only to, and every "<name>_encode" and "<name>_decode" literal of
        both benches.
        """
        found = set()
        for source in ("bench_encoder.cpp", "bench_memory.cpp"):
            text = (REPO / "tests" / "performance" / source).read_text(encoding="utf-8")
            found.update(re.findall(r'wanted\("([a-z0-9_]+)"\)', text))
            found.update(name for name in re.findall(r'"([a-z0-9]+(?:_[a-z0-9]+)+)"', text)
                         if name.endswith(("_encode", "_decode")))
        self.assertGreaterEqual(len(found), 14, "the scan found too few workloads to be reading "
                                "the benches: " + ", ".join(sorted(found)))
        unclassified = sorted(found - set(badges.WORKLOADS))
        self.assertEqual(unclassified, [],
                         "add these to WORKLOADS in tools/ci/write_measurement_badges.py, "
                         "docs/javascripts/hero-stats.js and docs/performance-quality.md")

    def test_every_workload_in_the_recorded_series_is_classified(self):
        """The names on quality-history as of 2026-09-30, including ones since retired."""
        recorded = {
            "plain_51", "plain_51_fast_mdct", "eac3_51_auto", "eac3_stereo_auto", "atmos_4obj",
            "atmos_4obj_fast_mdct", "atmos_4obj_qmf_fast_mdct", "ac3_51_decode",
            "eac3_51_decode", "atmos_4obj_decode", "ac4_stereo_encode", "ac4_51_encode",
            "ac4_stereo_decode", "ac4_51_decode", "ac3_51_encode", "eac3_51_encode",
            "ecpl_51_encode", "atmos_4obj_encode",
        }
        self.assertEqual(sorted(recorded - set(badges.WORKLOADS)), [])


class SiteScriptsTest(unittest.TestCase):
    """The home strip and the page's cards apply the badges' rules in JavaScript.

    There is no browser test, so what can be held is checked here: the table and
    the summary text are the same in both scripts and in the badge writer, and,
    where Node is installed, the scripts render the figures the badges carry.
    """

    HERO = REPO / "docs" / "javascripts" / "hero-stats.js"
    PAGE = REPO / "docs" / "performance-quality.md"

    @staticmethod
    def summary_block(path):
        text = path.read_text(encoding="utf-8").replace("\r\n", "\n")
        match = re.search(r"^ *// >>> measurement summary\n.*?^ *// <<< measurement summary$",
                          text, re.DOTALL | re.MULTILINE)
        if match is None:
            raise AssertionError(f"{path} has no measurement summary block")
        return match.group(0)

    @staticmethod
    def js_table(block, name):
        match = re.search(rf"const {name} = (\{{.*?\}});", block, re.DOTALL)
        if match is None:
            raise AssertionError(f"no {name} in the measurement summary block")
        # Quoted keys and no trailing commas is JSON except for the commas.
        return json.loads(re.sub(r",(\s*\})", r"\1", match.group(1)))

    def test_both_scripts_carry_the_same_summary_text(self):
        self.assertEqual(self.summary_block(self.HERO), self.summary_block(self.PAGE),
                         "docs/javascripts/hero-stats.js and docs/performance-quality.md "
                         "must carry the same text between their measurement summary markers")

    def test_the_tables_match_the_badge_writer_in_every_copy(self):
        for path in (self.HERO, self.PAGE):
            block = self.summary_block(path)
            with self.subTest(path=path.name):
                workloads = {k: tuple(v) for k, v in self.js_table(block, "WORKLOADS").items()}
                self.assertEqual(workloads, badges.WORKLOADS)
                self.assertEqual(self.js_table(block, "QUALITY_CODECS"), badges.QUALITY_CODECS)
                self.assertEqual(re.search(r"const CODECS = (\[.*?\]);", block).group(1),
                                 json.dumps(list(badges.CODECS)))
                self.assertEqual(self.js_table(block, "LAYOUTS"),
                                 {str(k): v for k, v in badges.LAYOUTS.items()})
                kib = badges.MEMORY_RETENTION_WARN_BYTES // 1024
                self.assertIn(f"const MEMORY_RETENTION_WARN_BYTES = {kib} * 1024;", block)


@unittest.skipUnless(shutil.which("node"), "node is not installed")
class SiteScriptsRenderTest(unittest.TestCase):
    """Runs the two scripts under Node (tools/ci/render_measurement_tiles.js)."""

    HARNESS = REPO / "tools" / "ci" / "render_measurement_tiles.js"

    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = Path(self._tmp.name)

    def write(self, stem, rows, commit="c0ffee"):
        stamped = [{"commit": commit, "commit_date": "2026-09-30T00:00:00Z", **r} for r in rows]
        (self.dir / f"{stem}.jsonl").write_text("".join(json.dumps(r) + "\n" for r in stamped))

    def fixture(self):
        self.write("performance-main", [
            perf("plain_51", 4.0), perf("ac3_51_decode", 0.08),
            perf("atmos_4obj", 8.0), perf("eac3_51_decode", 0.08),
            perf("ac4_51_encode", 3.83, AC4_BUDGET), perf("ac4_51_decode", 0.43, AC4_BUDGET)])
        self.write("main", [
            record([58.1, 63.8, 58.1, 82.2, 22.8, 22.7], [56.0, 62.0, 57.0, 81.0, 21.0, 21.0]),
            eac3_transient()])
        self.write("ac4-quality-main", [
            {"leg": "ac4-20-music-192", "min_snr_db": 34.9, "mos_lqo": 4.72},
            {"leg": "ac4-ims-music-64-2997", "min_snr_db": 16.67, "mos_lqo": 4.39}])
        self.write("memory-main", [
            mem("ac3_51_encode", 20110.6, growth=2048), mem("ac3_51_decode", 43256),
            mem("ecpl_51_encode", 28866.3, growth=14592), mem("atmos_4obj_decode", 82304),
            mem("ac4_51_encode", 2057000.0, growth=11808, allocs=1401.4)])
        self.write("external-comparison-main", [
            {"codec": "eac3", "leg": "eac3-51-256", "variant": "spx", "mos_lqo": 2.38},
            {"codec": "eac3", "leg": "eac3-music-stereo-96", "variant": "none", "mos_lqo": 4.65},
            {"codec": "ac3", "leg": "ac3-music-stereo-192", "variant": "landscape",
             "mos_lqo": 4.71}])

    def render(self, *flags):
        # UTF-8 explicitly: the harness prints a multiplication sign and en dashes, and a
        # Windows locale would read them as cp1252.
        done = subprocess.run(["node", str(self.HARNESS), str(self.dir), "--json", *flags],
                              capture_output=True, encoding="utf-8", check=False, timeout=60)
        self.assertEqual(done.returncode, 0, done.stderr)
        return {card["title"]: card for card in json.loads(done.stdout)["cards"]}

    def badge_messages(self):
        buf = io.StringIO()
        argv = ["x", "--history-dir", str(self.dir), "--branch", "main"]
        with mock.patch.object(sys, "argv", argv), contextlib.redirect_stdout(buf):
            self.assertEqual(badges.main(), 0)
        return {name: json.loads((self.dir / "badges" / f"{name}.json").read_text())
                for name in ("speed", "accuracy", "memory", "growth")}

    @staticmethod
    def as_badge(figure):
        """A card's figure with a multiplication sign as the badge writes it, with an x."""
        return figure.replace(MULTIPLICATION_SIGN, "x")

    def figures(self, card):
        return {line["codec"]: self.as_badge(line["figure"]) for line in card["lines"]}

    def test_the_cards_and_the_tiles_show_the_figures_on_the_badges(self):
        """A reader who clicks a badge through to the page must land on its numbers."""
        self.fixture()
        made = self.badge_messages()
        for flags in ((), ("--page",)):
            cards = self.render(*flags)
            with self.subTest(surface="page" if flags else "strip"):
                self.assertEqual(self.figures(cards["Speed"]),
                                 {"AC-3": "8x", "E-AC-3": "4x", "AC-4": "11x"})
                self.assertEqual(self.figures(cards["Decode accuracy"]),
                                 {"AC-3": "58.1 dB", "E-AC-3": "2.0 dB", "AC-4": "16.7 dB"})
                self.assertEqual(self.figures(cards["Allocation per frame"]),
                                 {"AC-3": "42 KB", "E-AC-3": "80 KB", "AC-4": "2009 KB"})
                self.assertEqual(self.figures(cards["Live growth"]),
                                 {"AC-3": "2.0 KiB", "E-AC-3": "14.3 KiB", "AC-4": "11.5 KiB"})
                for card, badge in (("Speed", "speed"), ("Decode accuracy", "accuracy"),
                                    ("Allocation per frame", "memory"), ("Live growth", "growth")):
                    for line in cards[card]["lines"]:
                        codec_part = next(p for p in made[badge]["message"].split(" | ")
                                          if p.startswith(line["codec"] + " "))
                        self.assertIn(self.as_badge(line["figure"]), codec_part)
                # The colours are the badges' colours.
                self.assertEqual(cards["Speed"]["chip"], "ok")
                self.assertEqual(cards["Decode accuracy"]["chip"], "ok")
                self.assertEqual(cards["Live growth"]["chip"], "watch")
                self.assertIsNone(cards["Allocation per frame"]["chip"])
        self.assertEqual((made["speed"]["color"], made["accuracy"]["color"],
                          made["memory"]["color"], made["growth"]["color"]),
                         (badges.GREEN, badges.GREEN, badges.INFO, badges.AMBER))

    def test_each_line_names_its_workload(self):
        self.fixture()
        cards = self.render()
        whats = {line["codec"]: line["what"] for line in cards["Speed"]["lines"]}
        self.assertIn("5.1 encode", whats["AC-3"])
        self.assertIn("Atmos 4-object encode", whats["E-AC-3"])
        self.assertIn("42.7 ms frame", whats["AC-4"])
        accuracy = {line["codec"]: line["what"] for line in cards["Decode accuracy"]["lines"]}
        self.assertIn("stereo 128 kbps transient, right channel", accuracy["E-AC-3"])
        self.assertIn("Floor 1.0 dB", accuracy["E-AC-3"])
        self.assertIn("source WAV", accuracy["E-AC-3"])
        self.assertIn("FFmpeg's decode", accuracy["AC-3"])
        self.assertIn("No floor yet", accuracy["AC-4"])
        self.assertIn(f"MOS-LQO 4.39{EN_DASH}4.72", accuracy["AC-4"])
        growth = {line["codec"]: line for line in cards["Live growth"]["lines"]}
        self.assertTrue(growth["E-AC-3"]["watch"])
        self.assertFalse(growth["AC-3"]["watch"])
        self.assertIn("allocator traffic, not memory in use", cards["Allocation per frame"]["note"])
        listening = {line["codec"]: line for line in cards["Listening quality"]["lines"]}
        self.assertEqual(listening["E-AC-3"]["figure"], f"2.38{EN_DASH}4.65 MOS-LQO")
        self.assertIn("No series yet", listening["AC-4"]["what"])

    def test_an_unclassified_workload_is_said_in_its_tile_and_the_others_still_render(self):
        """The site does not fail a build over data, and does not stay on "Loading"."""
        self.fixture()
        self.write("memory-main", [mem("ac3_51_encode", 1), mem("iamf_51_encode", 1)])
        cards = self.render()
        self.assertIn("iamf_51_encode", cards["Allocation per frame"]["note"])
        self.assertIn("is not classified", cards["Live growth"]["note"])
        self.assertEqual(self.figures(cards["Speed"])["AC-3"], "8x")

    def test_a_codec_with_no_rows_says_so_on_its_own_line(self):
        self.fixture()
        self.write("performance-main", [perf("plain_51", 4.0)])
        cards = self.render()
        speed = {line["codec"]: line for line in cards["Speed"]["lines"]}
        self.assertEqual(speed["AC-4"]["figure"], "—")
        self.assertEqual(speed["AC-4"]["what"], "No measurement recorded.")


class HistoryAndMainTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.dir = Path(self._tmp.name)

    def run_main(self, branch="dev"):
        argv = ["x", "--history-dir", str(self.dir), "--branch", branch]
        buf = io.StringIO()
        with mock.patch.object(sys, "argv", argv), contextlib.redirect_stdout(buf):
            code = badges.main()
        return code, buf.getvalue()

    def test_newest_commit_rows_takes_the_last_sha(self):
        path = self.dir / "h.jsonl"
        self.assertEqual(badges.newest_commit_rows(path), [])
        path.write_text("\n\n{broken\n")
        self.assertEqual(badges.newest_commit_rows(path), [])
        path.write_text("\n".join(json.dumps(r) for r in [
            {"commit": "a", "v": 1}, {"commit": "b", "v": 2}, {"commit": "b", "v": 3}]) + "\n")
        self.assertEqual([r["v"] for r in badges.newest_commit_rows(path)], [2, 3])

    def test_main_writes_four_badges(self):
        (self.dir / "performance-dev.jsonl").write_text(json.dumps(
            {"commit": "x", "config": "plain_51", "ms_per_frame": 4.0,
             "real_time_budget_ms_per_frame": 32.0}) + "\n")
        code, out = self.run_main()
        self.assertEqual(code, 0)
        speed = json.loads((self.dir / "badges" / "speed.json").read_text())
        self.assertEqual(speed["message"], "AC-3 5.1 encode: 8x | E-AC-3 no data | AC-4 no data")
        self.assertEqual(speed["schemaVersion"], 1)
        for name in ("accuracy", "memory", "growth"):
            data = json.loads((self.dir / "badges" / f"{name}.json").read_text())
            self.assertEqual(data["message"], badges.NO_DATA)
            self.assertEqual(data["color"], badges.GREY)
        self.assertIn(f"{badges.LABELS['speed']} = AC-3 5.1 encode: 8x", out)

    def test_main_reads_ac4_quality_from_its_own_file(self):
        (self.dir / "ac4-quality-dev.jsonl").write_text(json.dumps(
            {"commit": "y", "leg": "ac4-20-music-192", "min_snr_db": 34.85, "mos_lqo": 4.7}) + "\n")
        code, _ = self.run_main()
        self.assertEqual(code, 0)
        data = json.loads((self.dir / "badges" / "accuracy.json").read_text())
        self.assertEqual(
            data["message"],
            "AC-3 no data | E-AC-3 no data | AC-4 Dolby streams: 34.9 dB min, MOS-LQO 4.70")

    def test_main_fails_on_an_unlisted_workload_and_writes_nothing(self):
        """The forcing function on main: a new workload has to be classified.

        Non-zero, so the workflow step fails, and no half-written badge set is
        left in the checkout for the commit step to pick up.
        """
        rows = [{"commit": "x", "config": "plain_51", "ms_per_frame": 4.0},
                {"commit": "x", "config": "truehd_51_encode", "ms_per_frame": 9.0}]
        (self.dir / "performance-dev.jsonl").write_text(
            "".join(json.dumps(r) + "\n" for r in rows))
        code, out = self.run_main()
        self.assertEqual(code, 1)
        self.assertIn("::error", out)
        self.assertIn("truehd_51_encode", out)
        self.assertEqual(sorted((self.dir / "badges").iterdir()), [])


if __name__ == "__main__":
    unittest.main()
