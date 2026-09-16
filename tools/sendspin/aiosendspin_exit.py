"""A4's exit with aiosendspin 9.1.1: Hearth's server plays PCM, FLAC and Opus to a scripted player.

planning/hearth-reference-player.md, A4's exit, with the stand-in for Sendspin's reference player
that planning/hearth-sendspin-extension.md's Decisions chose. For each codec this starts the
scripted player in aiosendspin_player.py on a loopback port, runs ac3tests' hidden [aiosendspin]
case (tests/hearth/test_aiosendspin.cpp) with the player's URL, token and a directory, and checks
what the player took against the programme and start time the case wrote there:

- one stream, in the codec the player offered, that ended;
- every chunk's timestamp puts its first frame where the programme's timeline has it, from the
  time the case's group started, earlier by the codec's look-ahead, which only Opus has;
- PCM and FLAC decode to the programme sample for sample, and Opus, aligned by that look-ahead, to
  within 20 dB of it.

Usage: python tools/sendspin/aiosendspin_exit.py --ac3tests PATH [--codecs pcm,flac,opus]
                                                 [--out DIR] [--verbose]

Needs Python 3.12 or later and tools/sendspin/requirements.txt.
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import logging
import math
import os
import sys
import tempfile
import wave
from array import array
from pathlib import Path

from aiosendspin.models.types import AudioCodec
from aiosendspin_player import CHANNELS, SAMPLE_RATE, Received, ScriptedPlayer, free_port

HOST = "127.0.0.1"
MINIMUM_OPUS_SNR_DB = 20.0
# Left out of the Opus comparison at each end, where the codec settles.
EDGE_FRAMES = 4800


def truncated_division(numerator: int, denominator: int) -> int:
    """Integer division rounding towards zero, as C++ divides."""
    quotient = abs(numerator) // abs(denominator)
    return quotient if (numerator >= 0) == (denominator > 0) else -quotient


def samples(data: bytes) -> array:
    """Little-endian 16-bit samples."""
    values = array("h", data)
    if sys.byteorder == "big":
        values.byteswap()
    return values


def check(codec: str, directory: Path, received: Received) -> tuple[list[str], str]:
    """The problems with what the player took, and a line describing it."""
    problems: list[str] = []
    with wave.open(str(directory / "programme.wav"), "rb") as programme_wav:
        programme = samples(programme_wav.readframes(programme_wav.getnframes()))
    start = int((directory / "start_time_us.txt").read_text(encoding="utf-8").strip())
    decoded = samples(bytes(received.pcm))
    frames = len(programme) // CHANNELS
    decoded_frames = len(decoded) // CHANNELS

    if received.streams != 1 or not received.ended or received.codec != codec:
        problems.append(
            f"{received.streams} streams, ended {received.ended}, in {received.codec or 'nothing'}"
        )
    if not received.chunks:
        problems.append("no chunks")
        return problems, "nothing received"

    # The look-ahead, from how far before the programme's start the first chunk plays.
    lead_us = start - received.chunks[0][0]
    delay = round(lead_us * SAMPLE_RATE / 1_000_000)
    if codec == "opus":
        if not 0 < delay <= SAMPLE_RATE // 50:
            problems.append(f"the first chunk plays {lead_us} us before the programme")
    elif lead_us != 0:
        problems.append(f"the first chunk plays {lead_us} us from the programme's start")
    position = -delay
    for index, (timestamp, chunk_frames) in enumerate(received.chunks):
        expected = start + truncated_division(position * 1_000_000, SAMPLE_RATE)
        if timestamp != expected:
            problems.append(f"chunk {index} at {timestamp} us, where the timeline has {expected}")
            break
        position += chunk_frames

    if codec in ("pcm", "flac"):
        if decoded != programme:
            different = next(
                (i for i, (a, b) in enumerate(zip(decoded, programme, strict=False)) if a != b),
                min(len(decoded), len(programme)),
            )
            problems.append(
                f"decoded {decoded_frames} frames against {frames}; first difference at sample "
                f"{different}"
            )
        detail = "sample for sample"
    else:
        signal = 0.0
        noise = 0.0
        end = min(decoded_frames, frames + delay) - EDGE_FRAMES
        for frame in range(EDGE_FRAMES + delay, end):
            for channel in range(CHANNELS):
                wanted = programme[(frame - delay) * CHANNELS + channel]
                error = decoded[frame * CHANNELS + channel] - wanted
                signal += wanted * wanted
                noise += error * error
        snr = math.inf if noise == 0 else 10.0 * math.log10(signal / noise)
        if decoded_frames < frames or snr < MINIMUM_OPUS_SNR_DB:
            problems.append(f"decoded {decoded_frames} frames against {frames}, at {snr:.1f} dB")
        detail = f"{snr:.1f} dB after a look-ahead of {delay} frames"
    summary = (
        f"{received.streams} stream, {len(received.chunks)} chunks, {decoded_frames} frames, "
        f"{detail}"
    )
    return problems, summary


async def exercise(codec: str, ac3tests: Path, directory: Path) -> list[str]:
    """Runs the host case against a fresh player in `codec`; the problems found."""
    player = ScriptedPlayer(AudioCodec(codec), HOST, free_port(HOST), f"aiosendspin {codec}")
    await player.start()
    environment = dict(
        os.environ,
        AC3FORGE_AIOSENDSPIN_URL=player.url,
        AC3FORGE_AIOSENDSPIN_TOKEN=player.token,
        AC3FORGE_AIOSENDSPIN_OUT=str(directory),
    )
    process = await asyncio.create_subprocess_exec(
        str(ac3tests),
        "[aiosendspin]",
        env=environment,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.STDOUT,
    )
    try:
        output, _ = await asyncio.wait_for(process.communicate(), timeout=180)
    except TimeoutError:
        process.kill()
        await process.wait()
        await player.stop()
        return [f"{codec}: ac3tests did not finish within 180 s"]
    with contextlib.suppress(TimeoutError):
        await asyncio.wait_for(player.closed.wait(), timeout=10)
    await player.stop()
    player.write(directory)

    text = output.decode(errors="replace")
    if process.returncode != 0 or "All tests passed" not in text:
        print(text)
        return [f"{codec}: the host case failed (exit {process.returncode})"]
    problems, summary = check(codec, directory, player.received)
    print(f"{codec}: {summary}")
    return [f"{codec}: {problem}" for problem in problems]


async def run(ac3tests: Path, codecs: list[str], out: Path) -> int:
    problems: list[str] = []
    for codec in codecs:
        problems += await exercise(codec, ac3tests, out / codec)
    for problem in problems:
        print(problem, file=sys.stderr)
    return 1 if problems else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ac3tests", required=True, type=Path, help="the ac3tests binary")
    parser.add_argument("--codecs", default="pcm,flac,opus")
    parser.add_argument(
        "--out", type=Path, help="where each codec's files go; a temporary directory otherwise"
    )
    parser.add_argument("--verbose", action="store_true", help="the SDK's debug log")
    arguments = parser.parse_args()
    logging.basicConfig(level=logging.DEBUG if arguments.verbose else logging.WARNING)
    codecs = [codec for codec in arguments.codecs.split(",") if codec]
    unknown = sorted(set(codecs) - {codec.value for codec in AudioCodec})
    if unknown or not codecs:
        parser.error(f"--codecs takes pcm, flac and opus, not {', '.join(unknown) or 'nothing'}")
    if arguments.out is not None:
        return asyncio.run(run(arguments.ac3tests, codecs, arguments.out))
    with tempfile.TemporaryDirectory(prefix="aiosendspin-exit-") as scratch:
        return asyncio.run(run(arguments.ac3tests, codecs, Path(scratch)))


if __name__ == "__main__":
    sys.exit(main())
