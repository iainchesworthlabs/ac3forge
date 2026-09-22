#!/usr/bin/env python3
"""Generate apps/baremetal/fixture.hpp - the bitstreams the minimum-footprint
decoder probe decodes, and the per-channel levels it checks them against
(roadmap PF7).

The probe runs on a target with no filesystem, so its input has to be linked
in. Everything here is derived from committed inputs by committed tools:
tests/golden/audio/reference_51.wav (real programme material, per
CONTRIBUTING.md's own rule that silence and single tones make weak fixtures)
encoded by this project's own ac3cli, then decoded by the same ac3cli to
produce the expected levels. Re-running this script on an unchanged tree
reproduces the header byte for byte.

    python tools/generators/gen_baremetal_fixture.py --ac3cli build/.../bin/ac3cli

Two streams, because the profile's library contains two decoders and a probe
that exercised only one would leave the other unproven at link time as well as
at run time:

  - AC-3 5.1 at 448 kbit/s: the widest classic layout, coupling on.
  - E-AC-3 5.1 at 384 kbit/s with tools=all: AHT, spectral extension and
    coupling stacked, so the Annex E paths and the tables behind them are all
    reached.

Six frames each. Enough that frame 0's cold MDCT overlap is not the whole
sample (the same reason the C++ suite compares from frame 1 onward), small
enough that the generated header stays readable.
"""

from __future__ import annotations

import argparse
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import wave

SAMPLES_PER_FRAME = 1536
FRAMES = 6

# ac3cli's decode writes a WAV, and a WAV interleaves 5.1 as FL FR FC LFE BL BR
# (WAVE_FORMAT_EXTENSIBLE) - not the order the DECODER hands its channels back
# in, which is AC-3's own Table 5.8 order L C R Ls Rs LFE. The probe reads the
# decoder's output directly, so the levels here have to be permuted into coded
# order or every channel but the first is compared against its neighbour's
# number. Entry i is the WAV position holding coded channel i; it is the
# inverse of ac3::plan::wav_order()'s own mapping for this layout, written out
# rather than derived because this fixture is 5.1 and only 5.1.
WAV_TO_CODED_51 = [0, 2, 1, 4, 5, 3]

REPO = pathlib.Path(__file__).resolve().parents[2]
REFERENCE_WAV = REPO / "tests" / "golden" / "audio" / "reference_51.wav"
OUTPUT = REPO / "apps" / "baremetal" / "fixture.hpp"


def trim_wav(source: pathlib.Path, destination: pathlib.Path, frames: int) -> int:
    """Copy the first `frames` frames' worth of samples, keeping the format."""
    with wave.open(str(source), "rb") as src:
        channels = src.getnchannels()
        wanted = frames * SAMPLES_PER_FRAME
        if src.getnframes() < wanted:
            raise SystemExit(f"{source} holds {src.getnframes()} samples, need {wanted}")
        payload = src.readframes(wanted)
        with wave.open(str(destination), "wb") as dst:
            dst.setnchannels(channels)
            dst.setsampwidth(src.getsampwidth())
            dst.setframerate(src.getframerate())
            dst.writeframes(payload)
    return channels


def run(argv: list[str]) -> None:
    result = subprocess.run(argv, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise SystemExit(f"command failed ({result.returncode}): {' '.join(argv)}")


def channel_rms(path: pathlib.Path) -> list[float]:
    """Per-channel RMS of a WAV, in [0, 1).

    Hand-rolled rather than through the `wave` module: ac3cli's decode writes
    IEEE float32 (format tag 3), which that module refuses outright. Handles
    both that and PCM16 so this keeps working if the CLI's output format
    changes.
    """
    blob = path.read_bytes()
    if blob[:4] != b"RIFF" or blob[8:12] != b"WAVE":
        raise SystemExit(f"{path}: not a RIFF/WAVE file")
    offset = 12
    fmt_tag = bits = channels = 0
    samples: list[float] = []
    while offset + 8 <= len(blob):
        chunk_id = blob[offset : offset + 4]
        (size,) = struct.unpack_from("<I", blob, offset + 4)
        body = blob[offset + 8 : offset + 8 + size]
        if chunk_id == b"fmt ":
            fmt_tag, channels = struct.unpack_from("<HH", body, 0)
            (bits,) = struct.unpack_from("<H", body, 14)
        elif chunk_id == b"data":
            if fmt_tag == 3 and bits == 32:
                samples = list(struct.unpack(f"<{len(body) // 4}f", body))
            elif fmt_tag == 1 and bits == 16:
                samples = [v / 32768.0 for v in struct.unpack(f"<{len(body) // 2}h", body)]
            else:
                raise SystemExit(f"{path}: unsupported format tag {fmt_tag}/{bits}-bit")
        offset += 8 + size + (size & 1)
    if not channels or not samples:
        raise SystemExit(f"{path}: no usable fmt/data chunk")
    sums = [0.0] * channels
    counts = [0] * channels
    for index, sample in enumerate(samples):
        channel = index % channels
        sums[channel] += sample * sample
        counts[channel] += 1
    return [(sums[c] / counts[c]) ** 0.5 if counts[c] else 0.0 for c in range(channels)]


def to_coded_order(wav_values: list[float]) -> list[float]:
    """Reorder per-channel values from WAV interleave order to AC-3 coded order."""
    if len(wav_values) != len(WAV_TO_CODED_51):
        raise SystemExit(
            f"expected {len(WAV_TO_CODED_51)} channels for 5.1, got {len(wav_values)}"
        )
    return [wav_values[position] for position in WAV_TO_CODED_51]


def hex_array(data: bytes, indent: str = "    ") -> str:
    lines = []
    for offset in range(0, len(data), 12):
        chunk = data[offset : offset + 12]
        lines.append(indent + ", ".join(f"0x{byte:02x}" for byte in chunk) + ",")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ac3cli", required=True, help="path to a built ac3cli")
    args = parser.parse_args()

    ac3cli = pathlib.Path(args.ac3cli).resolve()
    if not ac3cli.exists():
        raise SystemExit(f"no such file: {ac3cli}")
    if not REFERENCE_WAV.exists():
        raise SystemExit(f"missing fixture source: {REFERENCE_WAV}")

    work = pathlib.Path(tempfile.mkdtemp(prefix="ac3-baremetal-"))
    try:
        source = work / "source.wav"
        trim_wav(REFERENCE_WAV, source, FRAMES)

        ac3 = work / "fixture.ac3"
        ec3 = work / "fixture.ec3"
        run([str(ac3cli), "encode", str(source), str(ac3), "448", "51", "couple"])
        run([str(ac3cli), "eac3-encode", str(source), str(ec3), "384", "all", "51"])

        ac3_decoded = work / "fixture_ac3.wav"
        ec3_decoded = work / "fixture_ec3.wav"
        run([str(ac3cli), "decode", str(ac3), str(ac3_decoded)])
        run([str(ac3cli), "decode", str(ec3), str(ec3_decoded)])

        streams = {
            "ac3": (ac3.read_bytes(), to_coded_order(channel_rms(ac3_decoded))),
            "eac3": (ec3.read_bytes(), to_coded_order(channel_rms(ec3_decoded))),
        }
    finally:
        shutil.rmtree(work, ignore_errors=True)

    body = [
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "// GENERATED by tools/generators/gen_baremetal_fixture.py - do not edit by hand.",
        "//",
        "// The bitstreams apps/baremetal/probe.cpp decodes and the per-channel levels it",
        "// checks them against (roadmap PF7). Both streams are this project's own encoder",
        f"// over the first {FRAMES} frames of tests/golden/audio/reference_51.wav; the",
        "// expected levels are that encoder's output decoded by this project's own decoder,",
        "// so they are a REGRESSION reference (has this build changed?), not an independent",
        "// oracle - the FFmpeg and Dolby comparisons in tools/ci/ are that.",
        "//",
        "// RMS is stored scaled by 1e6 and rounded, as an integer: newlib-nano's printf has",
        "// no floating-point support unless -u _printf_float is linked in, and a probe whose",
        "// subject is footprint should not drag that in just to report a number.",
        "",
        "namespace ac3probe {",
        "",
        f"inline constexpr int kFrames = {FRAMES};",
        "",
    ]

    for name, (data, rms) in streams.items():
        label = "AC-3 5.1 448 kbit/s, coupling" if name == "ac3" else \
                "E-AC-3 5.1 384 kbit/s, tools=all (AHT + spx + coupling)"
        stream_name = f"k{name.capitalize()}Stream"
        body += [
            f"// {label} - {len(data)} bytes, {FRAMES} frames.",
            f"inline constexpr std::array<std::uint8_t, {len(data)}> {stream_name}{{{{",
            hex_array(data),
            "}};",
            "",
            "// Per-channel RMS x 1e6, in the decoder's own AC-3 coded order",
            "// (Table 5.8: L, C, R, Ls, Rs, LFE) - see WAV_TO_CODED_51 in the generator.",
            f"inline constexpr std::array<std::int32_t, {len(rms)}> k{name.capitalize()}Rms{{{{",
            "    " + ", ".join(str(round(value * 1e6)) for value in rms),
            "}};",
            "",
        ]

    body += [
        "}  // namespace ac3probe",
        "",
    ]

    OUTPUT.write_text("\n".join(body), encoding="utf-8", newline="\n")
    total = sum(len(data) for data, _ in streams.values())
    print(f"wrote {OUTPUT.relative_to(REPO)} ({total} bitstream bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
