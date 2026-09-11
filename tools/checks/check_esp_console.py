#!/usr/bin/env python3
"""Fail when an ESP32 console capture shows a panic or a reset.

The ESP32 legs CI runs under QEMU pass on `result=pass` in what the
application printed: tools/checks/run_esp32s3_probe.sh,
tools/checks/run_esp32c3_probe.sh and the stream_player steps in
.github/workflows/_build.yml. QEMU runs on to a timeout either way, and the
verdict is not the end of the run: the probe still has its closing lines to
print and app_main to return from, and the HTTP step plays the stream a
second time. A panic prints its report and resets the part, and the next
boot runs the application again - so a check that only asks whether
result=pass appeared passes a panic after the verdict, and a panic before it
too whenever the boot after the reset gets through. The first of those
happened on 2026-09-10: the HTTP step's player freed its ring twice after its
verdict (ESP-IDF v6.1's vStreamBufferDeleteWithCaps), and result=pass was
followed by the heap's assert, a backtrace, "Rebooting..." and a second boot.

So from the first boot banner to the end, the capture must show

  - no panic output: none of MARKERS below, anywhere; and
  - one boot: one `ESP-ROM:` banner, which the ROM prints first on every
    boot. A second one means the part reset, whatever the cause.

Nothing before the first banner is read: `idf.py qemu` builds the project
before it starts QEMU, and the build's output lands in the same capture.

Each rule broken is one GitHub ::error:: annotation, followed by the lines
that broke it and the result= lines among them, so the order of the verdict
and the panic can be read off. Exit status 0 for a clean capture, 1 for a
finding or for a capture with no boot banner in it, 2 for a usage error.

  python3 tools/checks/check_esp_console.py --title "ESP32-C3 probe" console.txt

Standard library only, like the other scripts here: it runs in the
espressif/idf container, and its tests (test_check_esp_console.py) under the
script-lint job's python3.
"""

import argparse
import sys
from pathlib import Path

# The first line the ROM prints on every boot, whatever started it:
# "ESP-ROM:esp32s3-20210327" on the S3, "ESP-ROM:esp32c3-api1-20210207" on the C3.
BANNER = "ESP-ROM:"

# What ESP-IDF v6.1 prints when the application dies. Fixed strings, looked for
# anywhere in a line: as a regular expression "abort() was called" would be
# "abort" and an empty group, and would not match the line it names.
MARKERS = (
    "Guru Meditation",  # a CPU exception or an interrupt watchdog
    "assert failed",  # assert(), with the expression that failed
    "abort() was called",  # abort(), which a failed operator new also ends in
    "Backtrace:",  # the call chain in an Xtensa panic report
    "Rebooting",  # the panic handler's last line before it resets the part
)

# The application's verdict, listed beside a finding to show which came first.
VERDICT = "result="

# The most lines a finding lists. A part that panics on every boot fills a
# 900-second capture with the same few lines over and over.
MAX_LISTED = 40


def read_capture(path: Path) -> list[str]:
    """The capture's lines, without the CR ESP-IDF's console ends each one with.
    Decoded leniently: a serial line can carry any byte, and one that is not
    UTF-8 is no reason to stop reading."""
    text = path.read_bytes().decode("utf-8", errors="replace")
    return [line.rstrip("\r") for line in text.split("\n")]


def examine(lines: list[str]) -> tuple[list[str], list[int]]:
    """The rules `lines` breaks, one sentence each, and the indices of the lines
    to list for them: the ones that broke a rule, and every verdict. Both empty
    for one clean boot."""
    first = next((i for i, line in enumerate(lines) if BANNER in line), None)
    if first is None:
        return [f"no boot banner ({BANNER}) in the capture, so no boot to check"], []
    booted = range(first, len(lines))
    boots = [i for i in booted if BANNER in lines[i]]
    panics = [i for i in booted if any(marker in lines[i] for marker in MARKERS)]
    problems: list[str] = []
    shown: set[int] = set()
    if panics:
        found = [marker for marker in MARKERS if any(marker in lines[i] for i in panics)]
        problems.append(f"panic output after boot: {', '.join(found)}")
        shown.update(panics)
    if len(boots) > 1:
        problems.append(f"the part booted {len(boots)} times, so it reset; a clean run boots once")
        shown.update(boots)
        # The ROM's reset reason, a line or two below each banner.
        shown.update(i for i in booted if lines[i].startswith("rst:"))
    if shown:
        shown.update(i for i in booted if VERDICT in lines[i])
    return problems, sorted(shown)


def annotation(title: str, message: str) -> str:
    """A GitHub Actions ::error:: command, escaped the way the runner reads one."""

    def escape(text: str) -> str:
        return text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")

    title = escape(title).replace(":", "%3A").replace(",", "%2C")
    return f"::error title={title}::{escape(message)}"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Fail when an ESP32 console capture shows a panic or a reset."
    )
    parser.add_argument("capture", type=Path, help="what the part printed, as captured")
    parser.add_argument(
        "--title", default="ESP32 console", help="the title of the ::error:: annotations"
    )
    args = parser.parse_args(argv)

    try:
        lines = read_capture(args.capture)
    except OSError as error:
        print(annotation(args.title, f"cannot read the console capture: {error}"))
        return 1
    problems, listing = examine(lines)
    if not problems:
        print(f"{args.capture}: one boot, no panic output")
        return 0
    for problem in problems:
        print(annotation(args.title, f"{args.capture}: {problem}"))
    width = len(str(len(lines)))
    for index in listing[:MAX_LISTED]:
        print(f"  {index + 1:>{width}}: {lines[index]}")
    if len(listing) > MAX_LISTED:
        print(f"  ... and {len(listing) - MAX_LISTED} more")
    return 1


if __name__ == "__main__":
    sys.exit(main())
