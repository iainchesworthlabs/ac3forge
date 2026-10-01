#!/usr/bin/env python3
"""Generate apps/baremetal/ac4_fixture.hpp - the AC-4 bitstreams the bare-metal
AC-4 probe decodes, and the per-channel levels it checks them against
(planning/ac4.md, D14a).

The probe runs on a target with no filesystem, so its input is linked in. Every
stream is a committed one, read as it is, and the levels are what this project's
own decoder writes for it, so they are a regression reference (has this build
moved?) and not an independent oracle; the oracles are in tools/checks and
docs/verification.md. Re-running this script on an unchanged tree reproduces the
header byte for byte.

    python tools/generators/gen_baremetal_ac4_fixture.py --forge build/.../bin/forge

The streams are the table STREAMS below. What each is for:

  - ac4_20_music: a stereo stream DEE wrote, with A-SPX, at frame rate 23.438
    (frame_rate_index 13, the rate a part with a small heap decodes at).
  - ac4_20_acpl: a constructed stereo stream in A-CPL, the mode that adds
    decorrelators per band, so the row that shows what those cost.
  - ac4_51_music: 5.1 from DEE, the layout the P4 and the S3 are aimed at.
  - ac4_51_acpl: constructed 5.1 in A-CPL, with three decorrelators per band.
  - ac4_514_tones: DEE's widest layout in this set, ten channels (5.1.4), in
    full decoding: the row that measures what the widest programme needs of a
    part, on the way to the D14 rows.
  - ac4_20_companding: a stereo stream DEE wrote at 48 kbit/s, whose A-SPX
    runs the companding tool (b_compand_on in every frame): the one fixture
    that reaches float pow and exp2 in the decoder, so that its pinned hash
    is what holds decision 26 for the streams with companding (planning/ac4.md,
    D14a4).

The levels are the WAV that `forge decode` writes, read back per channel and put
in the order the decoder hands its channels over in (the speakers of
iclforge::ac4::DecodedFrame). For the three layouts here that is the file's own order (a
WAV's channel mask puts L R C LFE, the surround pair and the four heights in it),
and LAYOUTS says where each coded channel sits in the file, for a layout where it
is not.
"""

from __future__ import annotations

import argparse
import pathlib
import sys
import tempfile
import typing

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

# The E-AC-3 fixture generator's WAV reader and byte formatting, which read the
# same CLI's output.
from gen_baremetal_fixture import channel_rms, hex_array, run  # noqa: E402

REPO = HERE.parents[1]
OUTPUT = REPO / "apps" / "baremetal" / "ac4_fixture.hpp"


class Layout(typing.NamedTuple):
    coded_order: str
    # Where the coded channel k sits in the interleaved WAV.
    wav_position: tuple[int, ...]


LAYOUTS = {
    "stereo": Layout("L R", (0, 1)),
    "51": Layout("L R C LFE Ls Rs", (0, 1, 2, 3, 4, 5)),
    "514": Layout("L R C LFE Ls Rs Tfl Tfr Tbl Tbr", (0, 1, 2, 3, 4, 5, 6, 7, 8, 9)),
}


class Stream(typing.NamedTuple):
    key: str  # the row's name in the probe's output
    cxx: str  # the generated arrays' name, k<cxx>Stream and k<cxx>Rms
    source: str  # repository-relative path of the committed stream
    label: str
    layout: str


STREAMS = (
    Stream("ac4_20_music", "Ac420Music", "fuzz/seeds/fuzz_ac4_parse/ac4-20-music-192-3frames.ac4",
           "AC-4 2.0 from DEE, A-SPX, 192 kbit/s", "stereo"),
    Stream("ac4_20_acpl", "Ac420Acpl", "tests/golden/ac4dec/constructed/2_0-acpl1-stereoproc.ac4",
           "AC-4 2.0 constructed, A-CPL", "stereo"),
    Stream("ac4_51_music", "Ac451Music", "fuzz/seeds/fuzz_ac4_parse/ac4-51-music-192-3frames.ac4",
           "AC-4 5.1 from DEE, 192 kbit/s", "51"),
    Stream("ac4_51_acpl", "Ac451Acpl",
           "tests/golden/ac4dec/constructed/5_1-acpl1-config1-matsel7.ac4",
           "AC-4 5.1 constructed, A-CPL", "51"),
    Stream("ac4_514_tones", "Ac4514Tones",
           "fuzz/seeds/fuzz_ac4_parse/ac4-514-tones-256-2frames.ac4",
           "AC-4 5.1.4 from DEE, tones, 256 kbit/s", "514"),
    Stream("ac4_20_companding", "Ac420Companding",
           "fuzz/seeds/fuzz_ac4_parse/ac4-20-music-48-3frames.ac4",
           "AC-4 2.0 from DEE, A-SPX with companding, 48 kbit/s", "stereo"),
)

SAMPLES_PER_FRAME = 2048
SAMPLE_RATE_HZ = 48000


def count_sync_frames(data: bytes) -> int:
    """ac4_syncframe()s in a raw stream (Part 1 Annex G.3.1): the sync word 0xAC40 or
    0xAC41, a 16-bit frame_size that is 0xFFFF for a 24-bit one that follows, the
    frame, and a 16-bit crc_word after it when the sync word is 0xAC41."""
    position = 0
    frames = 0
    while position < len(data):
        if position + 4 > len(data):
            raise SystemExit("a truncated sync frame header")
        sync = int.from_bytes(data[position : position + 2], "big")
        if sync not in (0xAC40, 0xAC41):
            raise SystemExit(f"no sync word at byte {position}")
        size = int.from_bytes(data[position + 2 : position + 4], "big")
        header = 4
        if size == 0xFFFF:
            size = int.from_bytes(data[position + 4 : position + 7], "big")
            header = 7
        position += header + size + (2 if sync == 0xAC41 else 0)
        frames += 1
    if position != len(data):
        raise SystemExit("the last sync frame runs past the end of the stream")
    return frames


def wav_samples_per_channel(path: pathlib.Path) -> int:
    blob = path.read_bytes()
    offset = 12
    channels = 0
    while offset + 8 <= len(blob):
        chunk = blob[offset : offset + 4]
        size = int.from_bytes(blob[offset + 4 : offset + 8], "little")
        body = blob[offset + 8 : offset + 8 + size]
        if chunk == b"fmt ":
            channels = int.from_bytes(body[2:4], "little")
            bits = int.from_bytes(body[14:16], "little")
        elif chunk == b"data":
            return size // (channels * (bits // 8))
        offset += 8 + size + (size & 1)
    raise SystemExit(f"{path}: no data chunk")


def to_coded_order(layout: Layout, wav_values: list[float]) -> list[float]:
    if len(wav_values) != len(layout.wav_position):
        raise SystemExit(f"{layout.coded_order}: expected {len(layout.wav_position)} channels, "
                         f"got {len(wav_values)}")
    return [wav_values[position] for position in layout.wav_position]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--forge", required=True, help="path to a built forge")
    args = parser.parse_args()
    forge = pathlib.Path(args.forge).resolve()
    if not forge.exists():
        raise SystemExit(f"no such file: {forge}")

    rows = []
    with tempfile.TemporaryDirectory(prefix="ac4-baremetal-") as work:
        for stream in STREAMS:
            source = REPO / stream.source
            data = source.read_bytes()
            frames = count_sync_frames(data)
            decoded = pathlib.Path(work) / f"{stream.key}.wav"
            run([str(forge), "decode", str(source), str(decoded)])
            per_channel = wav_samples_per_channel(decoded)
            if per_channel != frames * SAMPLES_PER_FRAME:
                raise SystemExit(f"{stream.key}: {per_channel} samples a channel for {frames} "
                                 f"frames of {SAMPLES_PER_FRAME}")
            layout = LAYOUTS[stream.layout]
            rows.append((stream, data, frames, to_coded_order(layout, channel_rms(decoded))))

    body = [
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "// GENERATED by tools/generators/gen_baremetal_ac4_fixture.py - do not edit by hand.",
        "//",
        "// The AC-4 bitstreams apps/baremetal/ac4_probe.cpp decodes and the per-channel levels",
        "// it checks them against (planning/ac4.md, D14a). Every stream is a committed one, read",
        "// as it is (STREAMS in the generator names each); the expected levels are what this",
        "// project's own decoder writes for it, so they are a REGRESSION reference (has this",
        "// build moved?), not an independent oracle.",
        "//",
        "// RMS is stored scaled by 1e6 and rounded, as an integer: newlib-nano's printf has no",
        "// floating-point support unless -u _printf_float is linked in, and a probe whose subject",
        "// is footprint should not drag that in just to report a number.",
        "",
        "namespace iclforge_probe {",
        "",
        f"inline constexpr int kAc4SampleRateHz = {SAMPLE_RATE_HZ};",
        f"inline constexpr int kAc4SamplesPerFrame = {SAMPLES_PER_FRAME};",
        "",
    ]
    for stream, data, frames, rms in rows:
        layout = LAYOUTS[stream.layout]
        body += [
            f"// {stream.label} - {stream.source}, {len(data)} bytes, {frames} frames.",
            f"inline constexpr int k{stream.cxx}Frames = {frames};",
            f"inline constexpr std::array<std::uint8_t, {len(data)}> k{stream.cxx}Stream{{{{",
            hex_array(data),
            "}};",
            "// Per-channel RMS x 1e6, in the decoder's own order",
            f"// ({layout.coded_order}).",
            f"inline constexpr std::array<std::int32_t, {len(rms)}> k{stream.cxx}Rms{{{{",
            "    " + ", ".join(str(round(value * 1e6)) for value in rms),
            "}};",
            "",
        ]
    body += ["}  // namespace iclforge_probe", ""]
    OUTPUT.write_text("\n".join(body), encoding="utf-8", newline="\n")
    print(f"wrote {OUTPUT.relative_to(REPO)} ({sum(len(row[1]) for row in rows)} bitstream bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
