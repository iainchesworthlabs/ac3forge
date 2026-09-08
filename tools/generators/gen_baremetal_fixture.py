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

The streams are declared in one table (STREAMS below), each naming a layout
from another (LAYOUTS). Adding a configuration is a row in the first; adding a
channel layout is a row in the second. What each row is for:

  - AC-3 5.1 at 448 kbit/s: the widest classic layout, coupling on.
  - E-AC-3 5.1 at 384 kbit/s with tools=all: AHT, spectral extension and
    standard coupling stacked.
  - E-AC-3 5.1 at 384 kbit/s with tools=cpl+ecpl: §E3.5 enhanced coupling,
    which "all" does not select (parse_tools maps it to cpl+spx+aht) and which
    no other fixture here reaches. It is the branch behind ecpl_channel_spectrum,
    and behind the 512-point DFT that src/forge/src/core/fft.cpp is in the minimal
    source list for - both linked by every build of this profile and, until this
    stream existed, executed by none of them.
  - E-AC-3 Atmos at 448 kbit/s, six objects over a 5.1 bed, decoded BED ONLY
    (DecoderConfig::skip_object_reconstruction). The bed is ordinary E-AC-3 and
    fits; JOC's own reconstruction state does not, on any target this profile
    builds for - see docs/platforms/esp32.md. The fixture is here to hold that
    distinction: an Atmos stream PLAYS on a part that cannot render its objects,
    and this is what says so on the target rather than on a host.
  - E-AC-3 2/0 at 192 kbit/s with tools=all: a non-5.1 layout, and with it
    §7.5.4 rematrixing, which is 2/0-only and so unreachable from any of the
    above however their tools are set.

The profile's library contains two decoders, and a probe that exercised only
one would leave the other unproven at link time as well as at run time - which
is why the AC-3 row is here at all.

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
import typing
import wave

SAMPLES_PER_FRAME = 1536
FRAMES = 6

# --- the fixture tables ------------------------------------------------------
# ac3cli's decode writes a WAV, and a WAV interleaves 5.1 as FL FR FC LFE BL BR
# (WAVE_FORMAT_EXTENSIBLE) - not the order the DECODER hands its channels back
# in, which is AC-3's own Table 5.8 order L C R Ls Rs LFE. The probe reads the
# decoder's output directly, so the levels here have to be permuted into coded
# order or every channel but the first is compared against its neighbour's
# number.
#
# `wav_position[i]` is the WAV position holding coded channel i - the inverse of
# ac3::plan::wav_order()'s own mapping for that layout. Written out per layout
# rather than derived, because deriving it means reimplementing wav_order() and
# kWavSpeakerOrder in Python and then keeping two statements of the same
# permutation agreeing. One row is checked here (it must be a permutation) and
# the whole row is checked on the target: a wrong entry makes the probe compare
# a channel against its neighbour's level, which for these fixtures differ by
# well over the 5% tolerance in probe.cpp's level_matches.


class Layout(typing.NamedTuple):
    cli_name: str  # what ac3cli's [layout] positional calls it
    source: str  # programme material under tests/golden/audio/
    wav_position: tuple[int, ...]
    coded_order: str  # the channel names in coded order, for the emitted comment


LAYOUTS = {
    "51": Layout(
        cli_name="51",
        source="reference_51.wav",
        wav_position=(0, 2, 1, 4, 5, 3),
        coded_order="Table 5.8: L, C, R, Ls, Rs, LFE",
    ),
    # The object-scene source. atmos-encode makes each of its channels an object
    # and codes them over a 5.1 bed, so the STREAM is 5.1 even though the source
    # is five channels - which is why this row's permutation is 5.1's.
    "objects": Layout(
        cli_name="51",
        source="reference_objects.wav",
        wav_position=(0, 2, 1, 4, 5, 3),
        coded_order="Table 5.8: L, C, R, Ls, Rs, LFE",
    ),
    # Two channels, and both orders agree: WAV's FL FR and coded L R are the
    # same sequence, so this row is an identity permutation rather than a
    # simplification of one.
    "stereo": Layout(
        cli_name="stereo",
        source="reference_stereo.wav",
        wav_position=(0, 1),
        coded_order="Table 5.8 acmod 2: L, R",
    ),
    # One channel, so the permutation is the one-element identity.
    #
    # The SOURCE is the stereo file: there is no mono programme under
    # tests/golden/audio/ and adding one would be a third reference file to keep
    # in step for a single channel. ac3cli's encode folds a stereo source down
    # to 1/0 itself (plan.cpp's mono_downmix), which is also the more honest
    # fixture - a mono stream that real material was folded into, rather than
    # one channel of something that was never anything else.
    "mono": Layout(
        cli_name="mono",
        source="reference_stereo.wav",
        wav_position=(0,),
        coded_order="Table 5.8 acmod 1: C",
    ),
}


class Stream(typing.NamedTuple):
    cxx: str  # the generated array's name, minus the k prefix and the suffix
    key: str  # what probe.cpp's output labels this stream's lines with
    label: str  # the emitted comment
    layout: str  # a key into LAYOUTS
    encode: tuple[str, ...]  # ac3cli's argv after <in> <out>


STREAMS = (
    Stream(
        cxx="Ac3",
        key="ac3",
        label="AC-3 5.1 448 kbit/s, coupling",
        layout="51",
        encode=("encode", "448", "51", "couple"),
    ),
    # AC-3 2/0. §7.5.4 rematrixing exists in this layout and no other, and it is
    # a DIFFERENT code path from the E-AC-3 fixture's - Annex E carries its own
    # rematrixing syntax - so the eac3_stereo row below does not cover it.
    #
    # No tools argument, so no coupling: the 448 kbit/s row above passes
    # `couple` and was the only AC-3 fixture, which left the uncoupled path
    # linked into every build of this profile and executed by none of them. The
    # same gap enhanced coupling had.
    Stream(
        cxx="Ac3Stereo",
        key="ac3_stereo",
        label="AC-3 2/0 192 kbit/s, no coupling (§7.5.4 rematrixing)",
        layout="stereo",
        encode=("encode", "192", "stereo"),
    ),
    # AC-3 1/0. The narrowest programme the syntax has: one full-bandwidth
    # channel, no LFE, no coupling possible (§7.4 needs two channels to share a
    # band between), and no downmix to apply. Everything the decoder does per
    # channel it does exactly once here, which is what makes it worth a row -
    # the per-channel loops are all bounded by a count that is 6 in every other
    # AC-3 fixture, and 1 is the value that catches an off-by-one they cannot.
    Stream(
        cxx="Ac3Mono",
        key="ac3_mono",
        label="AC-3 1/0 128 kbit/s, single channel",
        layout="mono",
        encode=("encode", "128", "mono"),
    ),
    Stream(
        cxx="Eac3",
        key="eac3",
        label="E-AC-3 5.1 384 kbit/s, tools=all (AHT + spx + standard coupling)",
        layout="51",
        encode=("eac3-encode", "384", "all", "51"),
    ),
    Stream(
        cxx="Eac3Ecpl",
        key="eac3_ecpl",
        label="E-AC-3 5.1 384 kbit/s, tools=cpl+ecpl (§E3.5 enhanced coupling)",
        layout="51",
        encode=("eac3-encode", "384", "cpl+ecpl", "51"),
    ),
    Stream(
        cxx="Eac3AtmosBed",
        key="eac3_atmos_bed",
        label="E-AC-3 Atmos 448 kbit/s, 6 objects over a 5.1 bed - decoded BED ONLY",
        layout="objects",
        encode=("atmos-encode", "448"),
    ),
    Stream(
        cxx="Eac3Stereo",
        key="eac3_stereo",
        label="E-AC-3 2/0 192 kbit/s, tools=all (§7.5.4 rematrixing)",
        layout="stereo",
        encode=("eac3-encode", "192", "all", "stereo"),
    ),
)

REPO = pathlib.Path(__file__).resolve().parents[2]
AUDIO = REPO / "tests" / "golden" / "audio"
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


def to_coded_order(layout: Layout, wav_values: list[float]) -> list[float]:
    """Reorder per-channel values from WAV interleave order to coded order."""
    if sorted(layout.wav_position) != list(range(len(layout.wav_position))):
        raise SystemExit(f"{layout.cli_name}: wav_position is not a permutation")
    if len(wav_values) != len(layout.wav_position):
        raise SystemExit(
            f"{layout.cli_name}: expected {len(layout.wav_position)} channels, "
            f"got {len(wav_values)}"
        )
    return [wav_values[position] for position in layout.wav_position]


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
    for layout in LAYOUTS.values():
        if not (AUDIO / layout.source).exists():
            raise SystemExit(f"missing fixture source: {AUDIO / layout.source}")

    work = pathlib.Path(tempfile.mkdtemp(prefix="ac3-baremetal-"))
    try:
        # One trimmed source per layout, shared by every stream that names it.
        sources = {}
        for name, layout in LAYOUTS.items():
            sources[name] = work / f"source_{name}.wav"
            trim_wav(AUDIO / layout.source, sources[name], FRAMES)

        streams = []
        for stream in STREAMS:
            layout = LAYOUTS[stream.layout]
            command, *tail = stream.encode
            suffix = "ac3" if command == "encode" else "ec3"
            coded = work / f"{stream.key}.{suffix}"
            decoded = work / f"{stream.key}.wav"
            run([str(ac3cli), command, str(sources[stream.layout]), str(coded), *tail])
            run([str(ac3cli), "decode", str(coded), str(decoded)])
            streams.append(
                (stream, coded.read_bytes(), to_coded_order(layout, channel_rms(decoded)))
            )
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
        "// checks them against (roadmap PF7). Every stream is this project's own encoder",
        f"// over the first {FRAMES} frames of a file under tests/golden/audio/ (named per",
        "// layout by LAYOUTS in the generator); the expected levels are that encoder's",
        "// output decoded by this project's own decoder, so they are a REGRESSION reference",
        "// (has this build changed?), not an independent oracle - the FFmpeg and Dolby",
        "// comparisons in tools/ci/ are that.",
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

    for stream, data, rms in streams:
        layout = LAYOUTS[stream.layout]
        body += [
            f"// {stream.label} - {len(data)} bytes, {FRAMES} frames.",
            f"inline constexpr std::array<std::uint8_t, {len(data)}> k{stream.cxx}Stream{{{{",
            hex_array(data),
            "}};",
            "",
            "// Per-channel RMS x 1e6, in the decoder's own coded order",
            f"// ({layout.coded_order}) - see LAYOUTS in the generator.",
            f"inline constexpr std::array<std::int32_t, {len(rms)}> k{stream.cxx}Rms{{{{",
            "    " + ", ".join(str(round(value * 1e6)) for value in rms),
            "}};",
            "",
        ]

    body += [
        "}  // namespace ac3probe",
        "",
    ]

    OUTPUT.write_text("\n".join(body), encoding="utf-8", newline="\n")
    total = sum(len(data) for _, data, _ in streams)
    print(f"wrote {OUTPUT.relative_to(REPO)} ({total} bitstream bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
