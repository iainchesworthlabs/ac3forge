"""Generates the encoder-made substreams the decoder's presentation tests multiplex.

Phase D7 of planning/ac4.md builds presentations of several substreams (music and effects with
dialogue, main with associated audio, main with a dialogue enhancement substream) from the
substreams of separate streams: the main and music and effects substreams from DEE's committed
tone legs (tests/golden/external-baseline/ac4-51-tones-384 and ac4-20-tones-192), and the
dialogue, associated and dialogue enhancement substreams from these, which `ac3cli ac4-encode`
writes, since DEE writes no mono. Each carries a tone of its own at -20 dBFS, so that a mix can
be measured tone by tone:

  stream                  channels  kbps  tone(s)
  dialogue-en-mono.ac4    mono        64  1117 Hz
  dialogue-de-mono.ac4    mono        64  1373 Hz
  ad-mono.ac4             mono        64  1531 Hz
  commentary-stereo.ac4   L R        128  1699, 1847 Hz
  dialogue-fr-stereo.ac4  L R        128  1931, 2083 Hz
  de-mono.ac4             mono        64  977 Hz

All SIMPLE, at frame_rate_index 13, FRAMES frames long. They are committed under
tests/golden/ac4dec/presentations/sources/, and tests/ac4dec/test_ac4dec_presentations.cpp
multiplexes them, byte for byte, into the streams committed beside them; the encoder's bytes are
not promised across toolchains, so the test takes the committed sources rather than encoding
them again. Run this after a change to the encoder that should reach them, then the test with
AC4DEC_WRITE_PRESENTATIONS set to rewrite the multiplexed streams, then
tools/references/ac4_syntax.py for their digests.

Usage:
    python tools/generators/gen_ac4_presentation_sources.py --cli build/config-linux-llvm/bin/ac3cli
"""

import argparse
import math
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
OUT = REPO / "tests" / "golden" / "ac4dec" / "presentations" / "sources"
RATE = 48000
FRAMES = 24
SECONDS = 2.0
AMPLITUDE = 0.1  # -20 dBFS
STREAMS = (
    ("dialogue-en-mono", (1117.0,), 64),
    ("dialogue-de-mono", (1373.0,), 64),
    ("ad-mono", (1531.0,), 64),
    ("commentary-stereo", (1699.0, 1847.0), 128),
    ("dialogue-fr-stereo", (1931.0, 2083.0), 128),
    ("de-mono", (977.0,), 64),
)


def write_tones(path, tones):
    """A 24-bit WAV of one tone per channel."""
    frames = bytearray()
    for n in range(int(SECONDS * RATE)):
        for hz in tones:
            value = round(AMPLITUDE * math.sin(2.0 * math.pi * hz * n / RATE) * 8388607.0)
            frames += struct.pack("<i", value)[:3]
    with wave.open(str(path), "wb") as out:
        out.setnchannels(len(tones))
        out.setsampwidth(3)
        out.setframerate(RATE)
        out.writeframes(bytes(frames))


def first_frames(data, count):
    """The first `count` sync frames of a raw AC-4 stream (Part 1 Annex G)."""
    at = 0
    for _ in range(count):
        size = int.from_bytes(data[at + 2:at + 4], "big")
        header = 4
        if size == 0xFFFF:
            size = int.from_bytes(data[at + 4:at + 7], "big")
            header = 7
        crc = 2 if data[at:at + 2] == b"\xac\x41" else 0
        at += header + size + crc
    return data[:at]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cli", required=True, type=Path, help="the ac3cli to encode with")
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as work:
        for name, tones, kbps in STREAMS:
            wav = Path(work) / f"{name}.wav"
            write_tones(wav, tones)
            stream = Path(work) / f"{name}.ac4"
            command = [str(args.cli), "ac4-encode", str(wav), str(stream), str(kbps),
                       "codec-mode=simple", "quiet"]
            result = subprocess.run(command, capture_output=True, text=True, check=False)
            if result.returncode != 0:
                sys.exit(f"{name}: ac3cli ac4-encode failed ({result.returncode}):\n"
                         f"{result.stdout}{result.stderr}")
            (OUT / f"{name}.ac4").write_bytes(first_frames(stream.read_bytes(), FRAMES))
            print(f"{name}: {len(tones)} channel(s), {kbps} kbps, {FRAMES} frames")
    return 0


if __name__ == "__main__":
    sys.exit(main())
