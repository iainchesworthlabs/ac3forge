"""A6's exit with aiosendspin 9.1.1: a group of two test sinks and the scripted player plays one
programme, from the app's own Engine.

planning/hearth-reference-player.md, A6's exit: "from the app, a group of two test sinks and the
reference Python player plays one programme" - read against A4's own exit and Verified-by (same
doc), which always means a real aiosendspin process by that phrase, never a second in-process test
double (aiosendspin_exit.py's own docstring is the A4 case this mirrors).

Starts the scripted player in aiosendspin_player.py on a loopback port, runs ac3tests's hidden
[aiosendspin-group] case (tests/hearth/test_aiosendspin_group.cpp) with the player's URL, token and
a directory, and checks what the player took against programme.wav, the case's own local decode and
render of the programme, carried through the same full-scale-to-16-bit rescale
NetworkGroupSink::submit_pcm() and Group::rescaled() apply before a PCM member's encoder ever sees a
sample (that file's own header comment has the detail) - one PCM stream, ended, decoding sample for
sample to programme.wav, the same rigour aiosendspin_exit.py's own check() applies to A4's PCM and
FLAC. What real third-party interop additionally needs proving - pairing, handshake, negotiation, a
complete stream decoded without error - is checked alongside it; codec correctness itself
(AC-3/E-AC-3 decoding, as opposed to this group's own rescale) is exhaustively covered elsewhere in
this suite.

Usage: python tools/sendspin/aiosendspin_group_exit.py --ac3tests PATH [--out DIR] [--verbose]

Needs Python 3.12 or later and tools/sendspin/requirements.txt.
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import logging
import os
import sys
import tempfile
import wave
from array import array
from pathlib import Path

from aiosendspin.models.types import AudioCodec
from aiosendspin_player import CHANNELS, Received, ScriptedPlayer, free_port

HOST = "127.0.0.1"


def samples(data: bytes) -> array:
    """Little-endian 16-bit samples."""
    values = array("h", data)
    if sys.byteorder == "big":
        values.byteswap()
    return values


def check(directory: Path, received: Received) -> tuple[list[str], str]:
    """The problems with what the player took, and a line describing it."""
    problems: list[str] = []
    with wave.open(str(directory / "programme.wav"), "rb") as programme_wav:
        programme = samples(programme_wav.readframes(programme_wav.getnframes()))
    decoded = samples(bytes(received.pcm))
    frames = len(programme) // CHANNELS
    decoded_frames = len(decoded) // CHANNELS

    if received.streams != 1 or not received.ended or received.codec != "pcm":
        problems.append(
            f"{received.streams} streams, ended {received.ended}, in {received.codec or 'nothing'}"
        )
    if not received.chunks:
        problems.append("no chunks")
        return problems, "nothing received"
    if decoded != programme:
        different = next(
            (i for i, (a, b) in enumerate(zip(decoded, programme, strict=False)) if a != b),
            min(len(decoded), len(programme)),
        )
        problems.append(
            f"decoded {decoded_frames} frames against {frames}; first difference at sample "
            f"{different}"
        )

    summary = f"{received.streams} stream, {len(received.chunks)} chunks, {decoded_frames} frames"
    return problems, summary


async def exercise(ac3tests: Path, directory: Path) -> list[str]:
    """Runs the host case against a fresh player; the problems found."""
    player = ScriptedPlayer(AudioCodec.PCM, HOST, free_port(HOST), "aiosendspin group")
    await player.start()
    environment = dict(
        os.environ,
        AC3FORGE_AIOSENDSPIN_URL=player.url,
        AC3FORGE_AIOSENDSPIN_TOKEN=player.token,
        AC3FORGE_AIOSENDSPIN_OUT=str(directory),
    )
    process = await asyncio.create_subprocess_exec(
        str(ac3tests),
        "[aiosendspin-group]",
        env=environment,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.STDOUT,
    )
    try:
        output, _ = await asyncio.wait_for(process.communicate(), timeout=240)
    except TimeoutError:
        process.kill()
        await process.wait()
        await player.stop()
        return ["ac3tests did not finish within 240 s"]
    with contextlib.suppress(TimeoutError):
        await asyncio.wait_for(player.closed.wait(), timeout=10)
    await player.stop()
    player.write(directory)

    text = output.decode(errors="replace")
    if process.returncode != 0 or "All tests passed" not in text:
        print(text)
        return [f"the host case failed (exit {process.returncode})"]
    problems, summary = check(directory, player.received)
    print(summary)
    return problems


async def run(ac3tests: Path, out: Path) -> int:
    problems = await exercise(ac3tests, out)
    for problem in problems:
        print(problem, file=sys.stderr)
    return 1 if problems else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ac3tests", required=True, type=Path, help="the ac3tests binary")
    parser.add_argument(
        "--out", type=Path, help="where the run's files go; a temporary directory otherwise"
    )
    parser.add_argument("--verbose", action="store_true", help="the SDK's debug log")
    arguments = parser.parse_args()
    logging.basicConfig(level=logging.DEBUG if arguments.verbose else logging.WARNING)
    if arguments.out is not None:
        return asyncio.run(run(arguments.ac3tests, arguments.out))
    with tempfile.TemporaryDirectory(prefix="aiosendspin-group-exit-") as scratch:
        return asyncio.run(run(arguments.ac3tests, Path(scratch)))


if __name__ == "__main__":
    sys.exit(main())
