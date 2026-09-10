"""Unit tests for check_esp_console.py, the ESP32 console's panic-and-reset check.

stdlib `unittest`, as the other suites here are, for the reason
test_check_platform_matrix.py gives: this runs in ci.yml's script-lint job.

The captures are cut down from real ones: CI run 34449979233's streaming-player
step for a clean boot, and the same run's HTTP step for the case the check
exists for - result=pass, then a heap assert, a backtrace and a second boot.
Each test is one of the rules in the script's header.

Run: python3 -m unittest discover -s tools/checks -p 'test_*.py'
"""

import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_esp_console

# What `idf.py qemu` prints before QEMU starts: its build step, then the command.
BUILD = [
    "[1/5] cd /__w/ac3forge/ac3forge/esp-idf/ac3forge/examples/stream_player/build",
    "ac3forge_stream_player.bin binary size 0x60400 bytes.",
    "Running qemu (fg): qemu-system-xtensa -M esp32s3 -m 32M -nographic -serial mon:stdio",
    "Adding SPI flash device",
]

BOOT = [
    "ESP-ROM:esp32s3-20210327",
    "Build:Mar 27 2021",
    "rst:0x1 (POWERON),boot:0x4 (SPI_FLASH_BOOT)",
    "SPIWP:0xee",
    "I (22) boot: ESP-IDF v6.1 2nd stage bootloader",
    "I (182) main_task: Calling app_main()",
    "stream.rms[0]=107811",
    "stream.rms[1]=106647",
    "result=pass",
]

TAIL = [
    "I (822) main_task: Returned from app_main()",
    "qemu-system-xtensa: terminating on signal 15 from pid 37047 (timeout)",
    "",
]

CLEAN = BUILD + BOOT + TAIL

# The HTTP step's run from its verdict on: the player's teardown freeing its ring twice.
PANIC = [
    "",
    "assert failed: 0x4037c1bb <cached disabled>:630 "
    '(!block_is_free(block) && "block already marked as free")',
    "",
    "Backtrace: 0x4037a489:0x3fca3e50 0x4037a455:0x3fca3e70 0x40379fe1:0x3fca3e90",
    "",
    "ELF file SHA256: 1e80c01e2",
    "",
    "Rebooting...",
]

RESET = [
    "ESP-ROM:esp32s3-20210327",
    "Build:Mar 27 2021",
    "rst:0xc (RTC_SW_CPU_RST),boot:0x4 (SPI_FLASH_BOOT)",
    "SPIWP:0xee",
]

PANICKED = "panic output after boot: assert failed, Backtrace:, Rebooting"
REBOOTED = "the part booted 2 times, so it reset; a clean run boots once"


class Examine(unittest.TestCase):
    def test_one_clean_boot_passes(self) -> None:
        self.assertEqual(check_esp_console.examine(CLEAN), ([], []))

    def test_nothing_before_the_first_banner_is_read(self) -> None:
        # The rule rather than a case CI has printed: the build's output comes
        # first, and a path or a warning in it may carry a marker's words.
        build = [*BUILD, "components/newlib/src/assert.c: 'assert failed: %s'", "Rebooting.cpp"]
        self.assertEqual(check_esp_console.examine(build + BOOT + TAIL), ([], []))

    def test_a_panic_after_the_verdict_fails_and_lists_it_in_order(self) -> None:
        # The second boot runs the application again and passes again.
        capture = BUILD + BOOT + PANIC + RESET + BOOT[4:]
        problems, listing = check_esp_console.examine(capture)
        self.assertEqual(problems, [PANICKED, REBOOTED])
        self.assertEqual(
            [capture[i] for i in listing],
            [
                "ESP-ROM:esp32s3-20210327",
                "rst:0x1 (POWERON),boot:0x4 (SPI_FLASH_BOOT)",
                "result=pass",
                PANIC[1],
                PANIC[3],
                "Rebooting...",
                "ESP-ROM:esp32s3-20210327",
                "rst:0xc (RTC_SW_CPU_RST),boot:0x4 (SPI_FLASH_BOOT)",
                "result=pass",
            ],
        )

    def test_a_panic_before_the_verdict_fails_when_the_next_boot_passes(self) -> None:
        # The reset hides this one from a verdict check: the first boot never
        # prints result=pass, and the second one does.
        capture = BOOT[:-1] + PANIC + RESET + BOOT[4:]
        problems, _ = check_esp_console.examine(capture)
        self.assertEqual(problems, [PANICKED, REBOOTED])

    def test_each_marker_fails_on_its_own(self) -> None:
        lines = {
            "Guru Meditation": "Guru Meditation Error: Core  1 panic'ed (LoadProhibited).",
            "assert failed": PANIC[1],
            # Read as a regular expression this marker would miss its own line.
            "abort() was called": "abort() was called at PC 0x4200a1b2 on core 0",
            "Backtrace:": PANIC[3],
            "Rebooting": "Rebooting...",
        }
        self.assertEqual(tuple(lines), check_esp_console.MARKERS)
        for marker, line in lines.items():
            with self.subTest(marker=marker):
                problems, listing = check_esp_console.examine([*BOOT, line, *TAIL])
                self.assertEqual(problems, [f"panic output after boot: {marker}"])
                self.assertEqual(listing, [len(BOOT) - 1, len(BOOT)])

    def test_a_reset_with_no_panic_output_fails(self) -> None:
        # A reset nothing announced - a watchdog's, or esp_restart() - is one too.
        problems, _ = check_esp_console.examine(BOOT + RESET + TAIL)
        self.assertEqual(problems, [REBOOTED])

    def test_a_capture_with_no_boot_banner_fails(self) -> None:
        problems, listing = check_esp_console.examine(BUILD)
        self.assertEqual(len(problems), 1)
        self.assertIn("no boot banner", problems[0])
        self.assertEqual(listing, [])


class ReadCapture(unittest.TestCase):
    def test_crlf_and_bytes_that_are_not_utf8(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "console.txt"
            path.write_bytes(
                b"ESP-ROM:esp32s3-20210327\r\nresult=pass\r\n\xff\xfe\r\nRebooting...\r\n"
            )
            lines = check_esp_console.read_capture(path)
        self.assertEqual(lines[:2], ["ESP-ROM:esp32s3-20210327", "result=pass"])
        self.assertEqual(
            check_esp_console.examine(lines), (["panic output after boot: Rebooting"], [1, 3])
        )


class Main(unittest.TestCase):
    """main() prints ::error:: lines for most captures here. They are captured
    rather than left on stdout, where the script-lint job would read them as
    annotations of its own."""

    def run_main(self, lines: list[str], *options: str) -> tuple[int, list[str]]:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "console.txt"
            path.write_text("\n".join(lines) + "\n", encoding="utf-8")
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = check_esp_console.main([*options, str(path)])
        return code, out.getvalue().splitlines()

    def test_a_clean_capture_exits_zero(self) -> None:
        code, out = self.run_main(CLEAN)
        self.assertEqual(code, 0)
        self.assertEqual(len(out), 1)
        self.assertTrue(out[0].endswith("console.txt: one boot, no panic output"))

    def test_a_finding_is_an_annotation_per_rule_and_a_listing(self) -> None:
        code, out = self.run_main(BOOT + PANIC + RESET, "--title", "ESP32-S3 HTTP source")
        self.assertEqual(code, 1)
        for line, problem in zip(out[:2], [PANICKED, REBOOTED], strict=True):
            self.assertTrue(line.startswith("::error title=ESP32-S3 HTTP source::"))
            self.assertTrue(line.endswith(f"console.txt: {problem}"))
        self.assertIn("   9: result=pass", out)
        self.assertIn("  17: Rebooting...", out)

    def test_a_title_is_escaped_for_the_runner(self) -> None:
        self.assertEqual(
            check_esp_console.annotation("a, b: c", "100%\nnext"),
            "::error title=a%2C b%3A c::100%25%0Anext",
        )

    def test_a_crash_loop_is_listed_in_part(self) -> None:
        code, out = self.run_main(BOOT + (PANIC + RESET + BOOT[4:]) * 20)
        self.assertEqual(code, 1)
        listing = [line for line in out if not line.startswith("::error")]
        self.assertEqual(len(listing), check_esp_console.MAX_LISTED + 1)
        self.assertRegex(listing[-1], r"^  \.\.\. and \d+ more$")

    def test_a_capture_that_cannot_be_read_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = check_esp_console.main([str(Path(tmp) / "absent.txt")])
        self.assertEqual(code, 1)
        self.assertTrue(out.getvalue().startswith("::error title=ESP32 console::cannot read"))


if __name__ == "__main__":
    unittest.main()
