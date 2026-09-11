"""Unit tests for check_stream_set.py, which holds the ESP32 player's plays of
its stream set to the set's manifest.

stdlib `unittest`, as the other suites here are: this runs in ci.yml's script-
lint job with the system python3. The console lines are the shapes the
streaming example prints (examples/stream_player/main/stream_player.cpp and
its http source), cut down to what the checker reads.

Run: python3 -m unittest discover -s tools/checks -p 'test_*.py'
"""

import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_stream_set

SLOTS = ["L", "C", "R", "Ls", "Rs", "Lrs", "Rrs", "Vhl", "Vhr", "Lts", "Rts", "LFE"]

# A 5.1 stream onto 7.1.4: six slots at their levels, six silent.
LEVELS_51 = [350000, 351000, 352000, 353000, 354000, 0, 0, 0, 0, 0, 0, 349000]
ENTRY_51 = {
    "file": "layout-51.ec3",
    "channels": ["L", "C", "R", "Ls", "Rs", "LFE"],
    "units": 32,
    "levels_714": LEVELS_51,
    "psram": False,
    "refused": None,
}
ENTRY_TPN = {
    "file": "51-tpn.ec3",
    "channels": ["L", "C", "R", "Ls", "Rs", "LFE"],
    "units": 16,
    "levels_714": LEVELS_51,
    "psram": False,
    "refused": None,
}
ENTRY_44K = {
    "file": "ac3-51-44k.ac3",
    "channels": ["L", "C", "R", "Ls", "Rs", "LFE"],
    "units": None,
    "levels_714": None,
    "psram": False,
    "refused": "sample rate",
}
ENTRY_AHT = {
    "file": "714-aht.ec3",
    "channels": [],
    "units": 16,
    "levels_714": None,
    "psram": True,
    "refused": None,
}
MANIFEST = {
    "layout": "7.1.4",
    "slots": SLOTS,
    "streams": [ENTRY_51, ENTRY_TPN, ENTRY_44K, ENTRY_AHT],
}


def play_lines(name, levels, units=32, held=0, resync=0, result="pass"):
    return (
        [
            f"source: http http://10.0.2.2:8000/{name}, 24576 bytes",
            "player: layout 7.1.4, 12 slots, as coded, the bed placed",
            (
                "stream: E-AC-3 acmod=7 channels=6 substreams=1 dialnorm=-31 objects=no, "
                "onto 7.1.4 (12 slots)"
            ),
        ]
        + [f"stream.rms[{i}]={v}" for i, v in enumerate(levels)]
        + [
            (
                f"stream.units={units} stream.held={held} stream.resync_bytes={resync} "
                "stream.sink=capture-tdm"
            ),
            f"result={result}",
        ]
    )


def refused_lines(name, hz=44100):
    return [
        f"source: http http://10.0.2.2:8000/{name}, 29256 bytes",
        f"error: sample rate failed ({hz})",
        "stream.units=0 stream.held=0 stream.resync_bytes=0 stream.sink=capture-tdm",
        "result=fail",
    ]


def console(*groups):
    return "\n".join(line for group in groups for line in group) + "\n"


class ParsingTest(unittest.TestCase):
    def test_each_play_from_its_source_line_to_its_verdict(self):
        found = check_stream_set.plays(
            console(
                ["ESP-ROM:esp32s3-20210327"],
                play_lines("layout-51.ec3", LEVELS_51),
                refused_lines("ac3-51-44k.ac3"),
            )
        )
        self.assertEqual(
            [p["location"] for p in found],
            ["http://10.0.2.2:8000/layout-51.ec3", "http://10.0.2.2:8000/ac3-51-44k.ac3"],
        )
        self.assertEqual(found[0]["rms"], dict(enumerate(LEVELS_51)))
        self.assertEqual((found[0]["units"], found[0]["held"], found[0]["resync"]), (32, 0, 0))
        self.assertEqual(found[0]["result"], "pass")
        self.assertEqual(found[1]["error"], "sample rate failed (44100)")
        self.assertEqual(found[1]["result"], "fail")

    def test_a_play_cut_short_by_a_panic_has_no_verdict(self):
        lines = [
            *play_lines("714-aht.ec3", [])[:3],
            "abort() was called at PC 0x4207bf42 on core 1",
            "Rebooting...",
            "ESP-ROM:esp32s3-20210327",
        ]
        (only,) = check_stream_set.plays(console(lines))
        self.assertTrue(only["panic"])
        self.assertIsNone(only["result"])

    def test_the_name_of_a_location(self):
        self.assertEqual(
            check_stream_set.name_of("http://10.0.2.2:8000/714-walk.ec3?from=the-page"),
            "714-walk.ec3",
        )


class PlayTest(unittest.TestCase):
    def check(self, entry, lines):
        (play,) = check_stream_set.plays(console(lines))
        return check_stream_set.check_play(play, entry, SLOTS)

    def test_a_play_at_the_hosts_levels_passes(self):
        self.assertEqual(self.check(ENTRY_51, play_lines("layout-51.ec3", LEVELS_51)), [])

    def test_within_one_percent_and_twenty(self):
        near = [v + v // 100 + 20 if v else 0 for v in LEVELS_51]
        self.assertEqual(self.check(ENTRY_51, play_lines("layout-51.ec3", near)), [])
        far = list(near)
        far[1] += 1
        (problem,) = self.check(ENTRY_51, play_lines("layout-51.ec3", far))
        self.assertIn("slot 1 (C)", problem)

    def test_a_silent_slot_must_be_exactly_silent(self):
        upmixed = list(LEVELS_51)
        upmixed[7] = 1
        (problem,) = self.check(ENTRY_51, play_lines("layout-51.ec3", upmixed))
        self.assertIn("slot 7 (Vhl) at 1", problem)
        self.assertIn("must be silent", problem)

    def test_the_access_units_played_and_held_together(self):
        self.assertEqual(
            self.check(ENTRY_TPN, play_lines("51-tpn.ec3", LEVELS_51, units=15, held=1)), []
        )
        (problem,) = self.check(ENTRY_TPN, play_lines("51-tpn.ec3", LEVELS_51, units=15, held=0))
        self.assertIn("15 access units decoded, the stream has 16", problem)

    def test_two_programmes_played_as_one_is_caught_by_the_count(self):
        (problem,) = self.check(ENTRY_51, play_lines("layout-51.ec3", LEVELS_51, units=64))
        self.assertIn("64 access units", problem)

    def test_bytes_skipped_for_sync(self):
        (problem,) = self.check(ENTRY_51, play_lines("layout-51.ec3", LEVELS_51, resync=12))
        self.assertIn("12 bytes skipped", problem)

    def test_a_failed_play(self):
        self.assertEqual(
            self.check(ENTRY_51, play_lines("layout-51.ec3", LEVELS_51, result="fail")),
            ["result=fail"],
        )

    def test_too_few_slots(self):
        (problem,) = self.check(ENTRY_51, play_lines("layout-51.ec3", LEVELS_51[:2]))
        self.assertIn("2 slot levels printed, the layout has 12", problem)

    def test_a_stream_the_player_must_refuse(self):
        self.assertEqual(self.check(ENTRY_44K, refused_lines("ac3-51-44k.ac3")), [])
        (played,) = self.check(ENTRY_44K, play_lines("ac3-51-44k.ac3", LEVELS_51))
        self.assertIn("should have refused it (sample rate)", played)
        other = refused_lines("ac3-51-44k.ac3")
        other[1] = "error: decode failed (2)"
        (wrong,) = self.check(ENTRY_44K, other)
        self.assertIn("expected 'sample rate'", wrong)


# GET /status after a 5.1 stream finished on 7.1.4, the fields the checker reads.
STATUS_51 = {
    "state": "finished",
    "why": "end of stream",
    "stream": {
        "layout": "7.1.4",
        "render": "channels",
        "coded": "L,C,R,Ls,Rs,LFE",
        "silent": "Lrs,Rrs,Vhl,Vhr,Lts,Rts",
    },
}


class StatusTest(unittest.TestCase):
    def test_silent_comes_from_the_channels_not_the_levels(self):
        # A channel in the stream that happens to be quiet still reaches its slot.
        quiet_centre = dict(ENTRY_51, levels_714=[350000, 0, *LEVELS_51[2:]])
        self.assertEqual(
            check_stream_set.expected_silent(quiet_centre, SLOTS), "Lrs,Rrs,Vhl,Vhr,Lts,Rts"
        )
        dual_mono = dict(ENTRY_51, channels=["Ch1", "Ch2"])
        self.assertEqual(
            check_stream_set.expected_silent(dual_mono, SLOTS),
            "C,Ls,Rs,Lrs,Rrs,Vhl,Vhr,Lts,Rts,LFE",
        )
        self.assertIsNone(check_stream_set.expected_silent(ENTRY_44K, SLOTS))

    def test_what_status_says_about_a_play(self):
        self.assertEqual(check_stream_set.check_status(STATUS_51, ENTRY_51, SLOTS, "7.1.4"), [])
        wrong = json.loads(json.dumps(STATUS_51))
        wrong["stream"].update(layout="5.1", render="objects", coded="L,R", silent="")
        problems = check_stream_set.check_status(wrong, ENTRY_51, SLOTS, "7.1.4")
        self.assertEqual(len(problems), 4)

    def test_a_firmware_without_the_fields_is_not_wrong(self):
        self.assertEqual(
            check_stream_set.check_status(
                {"state": "finished", "stream": {"slots": 12}}, ENTRY_51, SLOTS, "7.1.4"
            ),
            [],
        )

    def test_a_refusal_in_status(self):
        self.assertEqual(
            check_stream_set.check_status(
                {"state": "failed", "why": "sample rate"}, ENTRY_44K, SLOTS, "7.1.4"
            ),
            [],
        )
        (problem,) = check_stream_set.check_status(
            {"state": "finished", "why": "end of stream"}, ENTRY_44K, SLOTS, "7.1.4"
        )
        self.assertIn("expected failed ('sample rate')", problem)


class CommandTest(unittest.TestCase):
    def run_main(self, text, *extra):
        with tempfile.TemporaryDirectory() as tmp:
            manifest = Path(tmp) / "streams.json"
            manifest.write_text(json.dumps(MANIFEST), encoding="utf-8")
            capture = Path(tmp) / "console.txt"
            capture.write_text(text, encoding="utf-8")
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = check_stream_set.main(["check", str(manifest), str(capture), *extra])
            return code, out.getvalue()

    def test_check_passes_every_play_it_finds(self):
        code, out = self.run_main(
            console(
                play_lines("layout-51.ec3", LEVELS_51),
                play_lines("51-tpn.ec3", LEVELS_51, units=15, held=1),
                refused_lines("ac3-51-44k.ac3"),
            )
        )
        self.assertEqual(code, 0, out)

    def test_all_fails_a_stream_never_played_but_not_a_psram_one(self):
        code, out = self.run_main(console(play_lines("layout-51.ec3", LEVELS_51)), "--all")
        self.assertEqual(code, 1)
        self.assertIn("51-tpn.ec3: never played", out)
        self.assertIn("ac3-51-44k.ac3: never played", out)
        self.assertNotIn("714-aht.ec3", out)
        self.assertIn("::error title=ESP32-S3 stream set::", out)


if __name__ == "__main__":
    unittest.main()
