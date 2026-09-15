"""Music Assistant's Sendspin server, scripted on aiosendspin 9.1.1, playing to ac3hearth-testsink.

A rehearsal of A4's exit with Music Assistant (planning/hearth-reference-player.md, A4's exit):
Music Assistant's Sendspin provider runs aiosendspin 9.1.1's SendspinServer, and this script drives
one the way the provider does. For each codec it starts ac3hearth-testsink on a loopback port,
offering only that codec and player@v1, dials it, pairs by the sink's SP:0 token or by the dynamic
code the sink shows, and plays it three seconds of two tones. It then stops the sink and checks the
WAV file the sink wrote against the programme: PCM and FLAC sample for sample, Opus within 20 dB.

Usage: python tools/sendspin/aiosendspin_server.py --testsink PATH [--pair token|code]
                                                   [--codecs pcm,flac,opus] [--out DIR]
                                                   [--verbose]

Needs Python 3.12 or later and tools/sendspin/requirements.txt.
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import logging
import math
import re
import struct
import sys
import tempfile
from array import array
from collections.abc import Callable
from pathlib import Path
from typing import Any

import numpy as np
from aiosendspin.audio.format import AudioFormat
from aiosendspin.models.types import PairMethod
from aiosendspin.noise.keys import Identity
from aiosendspin.noise.pairing import PairingAttempt
from aiosendspin.noise.pairing_token import decode_token
from aiosendspin.noise.trust_store import InMemoryServerPairingStore
from aiosendspin.server.server import SendspinServer

SAMPLE_RATE = 48000
CHANNELS = 2
FRAMES = 3 * SAMPLE_RATE
BLOCK_FRAMES = SAMPLE_RATE // 50
MINIMUM_OPUS_SNR_DB = 20.0
EDGE_FRAMES = 4800
MAXIMUM_HEAD_LOSS = SAMPLE_RATE // 4
MAXIMUM_TAIL_LOSS = SAMPLE_RATE // 10


def programme() -> array:
    """Two tones, 441 Hz on the left and 997 Hz on the right, swelling from -16 to -11 dBFS, as
    16-bit samples: no stretch of it repeats, so a repeated or missing chunk shows."""
    samples = array("h")
    for frame in range(FRAMES):
        t = frame / SAMPLE_RATE
        level = 5000.0 + 4000.0 * frame / FRAMES
        samples.append(round(level * math.sin(2.0 * math.pi * 441.0 * t)))
        samples.append(round(level * math.sin(2.0 * math.pi * 997.0 * t)))
    return samples


def read_float_wav(path: Path) -> tuple[int, array]:
    """A float32 WAV file's channel count and interleaved samples."""
    data = path.read_bytes()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError(f"{path} is not a WAV file")
    offset = 12
    channels = 0
    samples = array("f")
    while offset + 8 <= len(data):
        chunk, size = struct.unpack_from("<4sI", data, offset)
        body = data[offset + 8 : offset + 8 + size]
        if chunk == b"fmt ":
            tag, channels = struct.unpack_from("<HH", body, 0)
            bits = struct.unpack_from("<H", body, 14)[0]
            if tag == 0xFFFE:
                tag = struct.unpack_from("<H", body, 24)[0]
            if tag != 3 or bits != 32:
                raise ValueError(f"{path} is not float32")
        elif chunk == b"data":
            samples.frombytes(body[: len(body) - len(body) % 4])
        offset += 8 + size + (size & 1)
    if sys.byteorder == "big":
        samples.byteswap()
    return channels, samples


class TestSink:
    """ac3hearth-testsink as a child process, with what it has logged."""

    def __init__(self, binary: Path, directory: Path, codec: str, pair: str) -> None:
        self.binary = binary
        self.directory = directory
        self.codec = codec
        self.pair = pair
        self.lines: list[str] = []
        self.changed = asyncio.Condition()
        self._process: asyncio.subprocess.Process | None = None
        self._reader: asyncio.Task[None] | None = None

    async def start(self) -> None:
        self._process = await asyncio.create_subprocess_exec(
            str(self.binary),
            "--name",
            "Hearth test sink",
            "--address",
            "127.0.0.1",
            "--port",
            "0",
            "--state",
            str(self.directory / "state"),
            "--out",
            str(self.directory / "out"),
            "--codecs",
            self.codec,
            "--pair",
            "dynamic" if self.pair == "code" else "none",
            "--no-extension",
            "--no-mdns",
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT,
        )
        self._reader = asyncio.create_task(self._read())

    async def _read(self) -> None:
        assert self._process is not None and self._process.stdout is not None
        while line := await self._process.stdout.readline():
            async with self.changed:
                self.lines.append(line.decode(errors="replace").rstrip())
                self.changed.notify_all()

    async def wait_for(self, pattern: str, timeout: float = 30.0) -> re.Match[str]:
        """The first logged line matching `pattern`, from the start of the log."""
        expression = re.compile(pattern)

        def found() -> re.Match[str] | None:
            return next(filter(None, map(expression.search, self.lines)), None)

        async with self.changed:
            await asyncio.wait_for(self.changed.wait_for(found), timeout)
        match = found()
        assert match is not None
        return match

    async def stop(self) -> None:
        if self._process is None:
            return
        if self._process.returncode is None and self._process.stdin is not None:
            with contextlib.suppress(ConnectionError):
                self._process.stdin.write(b"quit\n")
                await self._process.stdin.drain()
            try:
                await asyncio.wait_for(self._process.wait(), 10)
            except TimeoutError:
                self._process.kill()
                await self._process.wait()
        if self._reader is not None:
            await self._reader


async def play(sink: TestSink, pair: str, samples: array) -> None:
    """Dials the sink, pairs, and plays `samples` to it as Music Assistant's provider would."""
    port = (await sink.wait_for(r"listening on 127\.0\.0\.1:(\d+)")).group(1)
    client_id = (await sink.wait_for(r"client_id (\S+)")).group(1)
    token = (await sink.wait_for(r"pairing token (SP:\S+)")).group(1)
    url = f"ws://127.0.0.1:{port}/sendspin"

    loop = asyncio.get_running_loop()
    server = SendspinServer(
        loop,
        Identity.generate(),
        "aiosendspin scripted server",
        pairing_store=InMemoryServerPairingStore(),
    )
    try:
        await server.connect_to_client_and_wait(url)
        client = await wait_until(lambda: server.get_client(client_id), "the sink's hello")
        if pair == "token":
            attempt = PairingAttempt(
                PairMethod.PAIRING_PSK, pairing_psk=decode_token(token).pairing_psk
            )
        else:

            async def entered_code() -> str:
                code = await sink.wait_for(r"PAIRING CODE (\d+)-?(\d*)")
                return code.group(1) + code.group(2)

            attempt = PairingAttempt(PairMethod.DYNAMIC_PIN, pin_provider=entered_code)
        await server.initiate_pairing(client_id, attempt)
        await wait_until(lambda: client.is_connected, "the sink on its long-term PSK")
        # Starting the stream activates playback, as pressing play in Music Assistant does, so
        # the first chunks race the sink's first client/state and its first clock exchange.

        stream = client.group.start_stream()
        source = AudioFormat(sample_rate=SAMPLE_RATE, bit_depth=16, channels=CHANNELS)
        if sys.byteorder == "big":
            samples = array("h", samples)
            samples.byteswap()
        for first in range(0, FRAMES, BLOCK_FRAMES):
            block = samples[first * CHANNELS : (first + BLOCK_FRAMES) * CHANNELS]
            stream.prepare_audio(block.tobytes(), source)
            await stream.commit_audio()
            await stream.sleep_to_limit_buffer(1_500_000)
        await asyncio.sleep(1.0)
        await client.group.stop()
        await asyncio.sleep(1.0)
    finally:
        await server.close()


async def wait_until(check: Callable[[], Any], what: str, timeout: float = 30.0) -> Any:
    """Polls `check` until it returns something true, and returns that."""
    deadline = asyncio.get_running_loop().time() + timeout
    while not (result := check()):
        if asyncio.get_running_loop().time() > deadline:
            raise TimeoutError(f"no {what} within {timeout:.0f} s")
        await asyncio.sleep(0.05)
    return result


def lag_of(played: np.ndarray, wanted: np.ndarray) -> int:
    """The lag at which `played` best matches `wanted` (played[k] against wanted[k - lag]), from
    MAXIMUM_HEAD_LOSS frames late to one Opus packet early: the least error over a window from
    the middle of the programme, whose tones repeat too closely for a correlation peak."""
    width = SAMPLE_RATE // 5
    middle = FRAMES // 2
    reference = wanted[middle : middle + width].astype(np.float64)
    best_error = math.inf
    best_lag = 0
    for lag in range(-MAXIMUM_HEAD_LOSS, BLOCK_FRAMES + 1):
        start = middle + lag
        if start < 0 or start + width > len(played):
            continue
        error = played[start : start + width].astype(np.float64) - reference
        energy = float(np.sum(error * error))
        if energy < best_error:
            best_error, best_lag = energy, lag
    return best_lag


def check(codec: str, directory: Path, samples: array) -> tuple[list[str], str]:
    """The problems with the sink's WAV file, and a line describing it.

    aiosendspin 9.1.1 sends the first chunks of a stream started from idle only in a replay the
    sink drops (the extension page's C13), and keeps its FLAC encoder's last partial block when the
    stream stops, so up to MAXIMUM_HEAD_LOSS frames may be missing at the start and
    MAXIMUM_TAIL_LOSS at the end. What arrived must be the programme: PCM and FLAC sample for
    sample, Opus within MINIMUM_OPUS_SNR_DB.
    """
    files = sorted((directory / "out").glob("stream-*.wav"))
    if len(files) != 1:
        return [f"{len(files)} WAV files"], "no single stream"
    channels, flat = read_float_wav(files[0])
    if channels != CHANNELS:
        return [f"{channels} channels"], "the wrong layout"
    played = np.frombuffer(flat.tobytes(), dtype=np.float32).reshape(-1, CHANNELS)
    wanted = (np.frombuffer(samples.tobytes(), dtype=np.int16) / 32768.0).astype(np.float32)
    wanted = wanted.reshape(-1, CHANNELS)
    lag = lag_of(played, wanted)
    # Programme frame `first` is the file's first frame; the file runs to programme frame `last`.
    first = -lag
    last = first + len(played)
    head_loss = max(first, 0)
    tail_loss = FRAMES - last
    problems: list[str] = []
    if head_loss > MAXIMUM_HEAD_LOSS:
        problems.append(f"the first {head_loss} frames are missing")
    if codec in ("pcm", "flac"):
        if first < 0 or tail_loss < 0 or tail_loss > MAXIMUM_TAIL_LOSS:
            problems.append(f"programme frames {first} to {last} of {FRAMES}")
        elif not np.array_equal(played, wanted[first:last]):
            problems.append(f"programme frames {first} to {last} differ from the programme")
        detail = "sample for sample"
    else:
        start = max(first, 0) + EDGE_FRAMES
        end = min(last, FRAMES) - EDGE_FRAMES
        reference = wanted[start:end].astype(np.float64)
        error = played[start - first : end - first].astype(np.float64) - reference
        noise = float(np.sum(error * error))
        snr = math.inf if noise == 0 else 10.0 * math.log10(float(np.sum(reference**2)) / noise)
        if snr < MINIMUM_OPUS_SNR_DB:
            problems.append(f"{snr:.1f} dB")
        detail = f"{snr:.1f} dB"
    summary = (
        f"programme frames {max(first, 0)} to {min(last, FRAMES)} of {FRAMES} "
        f"({head_loss} lost at the start, {max(tail_loss, 0)} at the end), {detail}"
    )
    return problems, summary


async def exercise(testsink: Path, codec: str, pair: str, directory: Path) -> list[str]:
    sink = TestSink(testsink, directory, codec, pair)
    await sink.start()
    samples = programme()
    try:
        await play(sink, pair, samples)
    except Exception as error:  # the log says where it stopped
        await sink.stop()
        print("\n".join(sink.lines))
        return [f"{codec}: {type(error).__name__}: {error}"]
    await sink.stop()
    problems, summary = check(codec, directory, samples)
    print(f"{codec}, paired by {pair}: {summary}")
    if problems:
        print("\n".join(sink.lines))
    return [f"{codec}: {problem}" for problem in problems]


async def run(testsink: Path, codecs: list[str], pair: str, out: Path) -> int:
    problems: list[str] = []
    for codec in codecs:
        problems += await exercise(testsink, codec, pair, out / f"{codec}-{pair}")
    for problem in problems:
        print(problem, file=sys.stderr)
    return 1 if problems else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--testsink", required=True, type=Path, help="ac3hearth-testsink's binary")
    parser.add_argument("--pair", choices=["token", "code"], default="token")
    parser.add_argument("--codecs", default="pcm,flac,opus")
    parser.add_argument(
        "--out", type=Path, help="where each codec's files go; a temporary directory otherwise"
    )
    parser.add_argument("--verbose", action="store_true", help="the SDK's debug log")
    arguments = parser.parse_args()
    logging.basicConfig(level=logging.DEBUG if arguments.verbose else logging.WARNING)
    codecs = [codec for codec in arguments.codecs.split(",") if codec]
    unknown = sorted(set(codecs) - {"pcm", "flac", "opus"})
    if unknown or not codecs:
        parser.error(f"--codecs takes pcm, flac and opus, not {', '.join(unknown) or 'nothing'}")
    if arguments.out is not None:
        return asyncio.run(run(arguments.testsink, codecs, arguments.pair, arguments.out))
    with tempfile.TemporaryDirectory(prefix="aiosendspin-server-") as scratch:
        return asyncio.run(run(arguments.testsink, codecs, arguments.pair, Path(scratch)))


if __name__ == "__main__":
    sys.exit(main())
