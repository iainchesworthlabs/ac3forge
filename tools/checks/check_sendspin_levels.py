#!/usr/bin/env python3
"""Hold a board's Sendspin levels to a test sink's, for one stream both played.

planning/hearth-reference-player.md, B4. A board playing a stream over
_ac3forge_player@v1 prints, when the stream ends, a line of counters and one
line per output with that output's RMS over the whole stream, scaled by a
million (esp-idf/ac3forge/src/burst_player.cpp):

  sendspin.stream=bursts bursts=378 late=0 dropped=0 invalid=0 underruns=0 ... heap_free=30120
  sendspin.rms[0]=107370
  sendspin.rms[1]=106234

A test sink that played the same stream in the same group, rendered to the same
layout (ac3hearth-testserver's --sink), wrote what it decoded to a WAV file.
This reads both and fails when

  - the capture has no closing line for a stream of bursts, or its counters
    show a burst that did not play (late, dropped or invalid) or an underrun;
  - the board and the WAV file have different numbers of outputs; or
  - an output's RMS is further from the WAV channel's than the tolerance: a
    fraction of the WAV's figure (0.5% by default, 0.04 dB), and never less
    than an absolute floor, so that a silent channel's float noise passes.

The board decodes with the library's single-precision build and the test sink
with the computer's, so the two agree closely rather than exactly.

Each finding is one GitHub ::error:: annotation. Exit status 0 when the levels
match, 1 for a finding, 2 for a usage error or a file that cannot be read.

  python3 tools/checks/check_sendspin_levels.py --console qemu-sendspin.txt \\
      --wav reference/out/bursts-1-1.wav

Standard library only, like the other scripts here. Its tests are
test_check_sendspin_levels.py.
"""

import argparse
import array
import math
import re
import struct
import sys
from pathlib import Path

STREAM_LINE = re.compile(r"sendspin\.stream=(\S+)")
RMS_LINE = re.compile(r"sendspin\.rms\[(\d+)\]=(\d+)")
FIELD = re.compile(r"(\w+)=(-?\d+)")

# Counters on the closing line that must be zero for the levels to mean anything.
MUST_BE_ZERO = ("late", "dropped", "invalid", "underruns")

WAVE_FORMAT_PCM = 1
WAVE_FORMAT_IEEE_FLOAT = 3
WAVE_FORMAT_EXTENSIBLE = 0xFFFE


class Unreadable(Exception):
    """A file that cannot be read as what it should be."""


def annotation(title: str, message: str) -> str:
    """A GitHub Actions ::error:: command, escaped the way the runner reads one."""

    def escape(text: str) -> str:
        return text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")

    title = escape(title).replace(":", "%3A").replace(",", "%2C")
    return f"::error title={title}::{escape(message)}"


def board_levels(text: str) -> tuple[dict[str, int], list[int]] | None:
    """The last closing line of a stream of bursts in `text`, as its counters,
    and the RMS lines that follow it, in output order. None when there is no
    such line."""
    lines = [line.rstrip("\r") for line in text.split("\n")]
    last = None
    for i, line in enumerate(lines):
        match = STREAM_LINE.search(line)
        if match and match.group(1) == "bursts":
            last = i
    if last is None:
        return None
    counters = {
        key: int(value) for key, value in FIELD.findall(lines[last].split("sendspin.stream=", 1)[1])
    }
    rms: dict[int, int] = {}
    for line in lines[last + 1 :]:
        match = RMS_LINE.search(line)
        if match is None:
            break
        rms[int(match.group(1))] = int(match.group(2))
    return counters, [rms[i] for i in sorted(rms)]


def wav_levels(data: bytes) -> list[int]:
    """Each channel's RMS over the whole of a WAV file, scaled by a million:
    16-, 24- or 32-bit PCM, or 32-bit float."""
    if len(data) < 12 or data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise Unreadable("not a RIFF WAVE file")
    fmt = None
    samples = None
    at = 12
    while at + 8 <= len(data):
        kind, size = struct.unpack_from("<4sI", data, at)
        body = data[at + 8 : at + 8 + size]
        if kind == b"fmt ":
            if len(body) < 16:
                raise Unreadable("a fmt chunk too short to read")
            tag, channels, _, _, _, bits = struct.unpack_from("<HHIIHH", body)
            if tag == WAVE_FORMAT_EXTENSIBLE and len(body) >= 26:
                tag = struct.unpack_from("<H", body, 24)[0]
            fmt = (tag, channels, bits)
        elif kind == b"data":
            samples = body
        at += 8 + size + (size & 1)
    if fmt is None or samples is None:
        raise Unreadable("no fmt or data chunk")
    tag, channels, bits = fmt
    if channels < 1:
        raise Unreadable("no channels")
    if tag == WAVE_FORMAT_IEEE_FLOAT and bits == 32:
        values = array.array("f")
        values.frombytes(samples[: len(samples) // 4 * 4])
        if sys.byteorder != "little":
            values.byteswap()
        scale = 1.0
    elif tag == WAVE_FORMAT_PCM and bits in (16, 32):
        values = array.array("h" if bits == 16 else "i")
        width = bits // 8
        values.frombytes(samples[: len(samples) // width * width])
        if sys.byteorder != "little":
            values.byteswap()
        scale = 1.0 / (1 << (bits - 1))
    elif tag == WAVE_FORMAT_PCM and bits == 24:
        count = len(samples) // 3
        values = array.array(
            "i",
            (
                int.from_bytes(samples[i * 3 : i * 3 + 3], "little", signed=True)
                for i in range(count)
            ),
        )
        scale = 1.0 / (1 << 23)
    else:
        raise Unreadable(f"a format this does not read (tag {tag}, {bits} bits)")
    frames = len(values) // channels
    if frames == 0:
        return [0] * channels
    levels = []
    for channel in range(channels):
        column = values[channel : frames * channels : channels]
        total = math.fsum(float(x) * float(x) for x in column)
        levels.append(round(math.sqrt(total / frames) * scale * 1e6))
    return levels


def compare(
    counters: dict[str, int], board: list[int], reference: list[int], tolerance: float, floor: int
) -> list[str]:
    """What is wrong, one sentence each; empty when the levels match."""
    problems = []
    for key in MUST_BE_ZERO:
        if counters.get(key, 0) != 0:
            problems.append(
                f"the board's closing line has {key}={counters[key]}; a stream held to a "
                "test sink plays every burst"
            )
    if counters.get("bursts", 0) == 0:
        problems.append("the board's closing line has no bursts played")
    if len(board) != len(reference):
        problems.append(
            f"the board has {len(board)} outputs and the test sink's WAV {len(reference)}"
        )
        return problems
    for output, (got, expected) in enumerate(zip(board, reference, strict=True)):
        slack = max(floor, round(expected * tolerance))
        if abs(got - expected) > slack:
            ratio = got / expected if expected else float("inf")
            difference = (
                f"{20 * math.log10(ratio):+.2f} dB"
                if 0 < ratio < float("inf")
                else "not comparable"
            )
            problems.append(
                f"output {output}: the board's RMS is {got} and the test sink's {expected} "
                f"({difference}; tolerance +/-{slack})"
            )
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Hold a board's Sendspin levels to a test sink's.")
    parser.add_argument(
        "--console", type=Path, required=True, help="what the board printed, as captured"
    )
    parser.add_argument(
        "--wav", type=Path, required=True, help="the test sink's WAV file for the same stream"
    )
    parser.add_argument(
        "--tolerance",
        type=float,
        default=0.005,
        help="the fraction an RMS may differ by (default 0.005)",
    )
    parser.add_argument(
        "--floor",
        type=int,
        default=30,
        help="the least difference, in millionths of full scale, that fails (default 30)",
    )
    parser.add_argument(
        "--title", default="Sendspin levels", help="the title of the ::error:: annotations"
    )
    args = parser.parse_args(argv)

    try:
        found = board_levels(args.console.read_bytes().decode("utf-8", errors="replace"))
    except OSError as error:
        print(annotation(args.title, f"cannot read the console capture: {error}"))
        return 2
    if found is None:
        print(
            annotation(
                args.title, f"{args.console}: no sendspin.stream=bursts line, so no stream ended"
            )
        )
        return 1
    counters, board = found
    try:
        reference = wav_levels(args.wav.read_bytes())
    except (OSError, Unreadable) as error:
        print(annotation(args.title, f"cannot read {args.wav}: {error}"))
        return 2
    for output, expected in enumerate(reference):
        got = board[output] if output < len(board) else None
        print(f"output {output}: board {got}, test sink {expected}")
    problems = compare(counters, board, reference, args.tolerance, args.floor)
    for problem in problems:
        print(annotation(args.title, problem))
    if not problems:
        bursts = counters.get("bursts", 0)
        print(
            f"{len(board)} outputs within {args.tolerance:.1%} of the test sink; "
            f"{bursts} bursts, no underruns"
        )
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
