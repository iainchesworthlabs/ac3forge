"""Conformance vector set generator (roadmap VX20).

Builds the versioned bundle of AC-3 / E-AC-3 / Atmos streams this project
publishes as a release artifact, so another decoder implementer has something
concrete to test against. No free ATSC or ETSI conformance bitstreams exist
publicly; this is the project's answer to that, going the opposite direction
from every other check in the repo (which consumes other people's streams as
oracles rather than producing any).

What it emits, under --out:

    MANIFEST.json    every vector, what it exercises, its hashes, and whether
                     FFmpeg can read it
    README.md        how to use the set, written for someone who did not build
                     it
    source/          the PCM each vector was encoded from
    vectors/         the streams themselves, by codec

Everything published is this project's own output, encoded from source PCM
this project synthesized - nothing from tests/golden/external-baseline (Dolby
Media Encoder / FFmpeg output) is ever copied in. See README.md's "Licensing"
section, which this script writes.

Reproducibility: run it twice on one machine and every hash matches, which is
what --check-determinism asserts. Hashes do NOT carry across compilers or
architectures - docs/building.md records a measured cross-toolchain
difference, and the arm64 legs sit 6 dB off the x86 gold numbers (roadmap
VX11/VX12) - so the manifest records the exact toolchain that produced it and
a consumer compares hashes only against a bundle built the same way.

Usage (repo root, after a build):

    python tools/generators/gen_conformance_vectors.py \
        --cli build/config-linux-gcc/bin/ac3cli \
        --out dist/conformance-vectors

    python tools/generators/gen_conformance_vectors.py --cli <path> \
        --out dist/conformance-vectors --archive

Signing: --sign asks for the Atmos signed-object vector, which needs a key
this project does not ship. Supply one through AC3FORGE_SIGNING_KEY /
AC3FORGE_SIGNING_KEY_FILE (the same environment ac3cli itself reads) or
--signing-key <path>. Without a key the signed vector is omitted and the
manifest records why; nothing here ever invents or forges one.
"""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import math
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import wave
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
GOLDEN_AUDIO = REPO / "tests" / "golden" / "audio"

SCHEMA = "ac3forge-conformance-vectors/1"

# Annex E fscod2 (§E2.3.1.3). Reduced-rate streams FFmpeg walks but refuses to
# decode - see docs/verification.md's own note.
FSCOD2_RATES = (24000, 22050, 16000)

# The one layout FFmpeg cannot read at all: ff_ac3_parse_header rejects a
# second dependent substream (substreamid != 0), which is exactly what 7.1.4
# needs. Same rule tools/ci/run_codec_matrix.sh applies to decide which of its
# streams get an FFmpeg strict decode.
TWO_DEPENDENT_LAYOUTS = ("714",)

# Annex E tools FFmpeg's parser has no model of at all - not a known refusal,
# just no syntax for the bits. Also from docs/verification.md.
UNMODELLED_TOOLS = ("ecpl", "tpn")


# --------------------------------------------------------------------------
# Source PCM
# --------------------------------------------------------------------------
#
# The 48 kHz sources are the repo's own checked-in gate fixtures, so a vector
# is an encode of exactly the material docs/verification.md's numbers are
# measured on. The five Annex E / AC-3 alternate rates have no such fixture,
# so they are synthesized here the same way tools/generators/gen_gold_
# reference_wav.py builds its own: sin() plus seeded, FIR-smoothed noise, from
# first principles rather than by resampling or by decoding one of our own
# encodes. Stdlib only, and deterministic - a fixed seed per channel.
#
# Roadmap VX7 will add redistributable (CC0) speech and music beside the
# synthetic fixtures. When it lands, this is where those files join the set;
# until then every vector's source is synthetic, which the README states
# plainly because it bounds what the set proves.


def _smooth(samples: list[float], taps: int) -> list[float]:
    """Boxcar FIR - band-limits white noise without a filter-design library."""
    out = [0.0] * len(samples)
    total = 0.0
    window: list[float] = []
    for i, s in enumerate(samples):
        window.append(s)
        total += s
        if len(window) > taps:
            total -= window.pop(0)
        out[i] = total / len(window)
    return out


def _noise(seed: int, length: int, taps: int) -> list[float]:
    rng = random.Random(seed)
    filtered = _smooth([rng.uniform(-1.0, 1.0) for _ in range(length)], taps)
    peak = max((abs(v) for v in filtered), default=0.0)
    return [v / peak for v in filtered] if peak > 0.0 else filtered


def synth_six_channel(rate: int, seconds: float) -> list[list[float]]:
    """Six decorrelated channels in WAV order (FL FR FC LFE BL BR).

    Every channel carries genuinely different material, so coupling,
    rematrixing and the LFE path all see real signal - the same reasoning
    gen_gold_reference_wav.py's own comment gives. Frequencies are set as a
    fraction of the sample rate rather than in Hz, so the 16 kHz version
    exercises the same relative band structure as the 48 kHz one instead of
    crowding everything into the bottom of the band.
    """
    n = int(rate * seconds)
    peak = 0.55
    nyquist = rate / 2.0

    def tone(fraction: float, phase: float) -> list[float]:
        w = 2.0 * math.pi * (fraction * nyquist) / rate
        return [math.sin(w * i + phase) for i in range(n)]

    def mix(parts: list[tuple[list[float], float]]) -> list[float]:
        out = [0.0] * n
        for values, gain in parts:
            for i, v in enumerate(values):
                out[i] += v * gain
        limit = max((abs(v) for v in out), default=0.0)
        scale = peak / limit if limit > 0.0 else 0.0
        return [v * scale for v in out]

    left = mix([(tone(0.02, 0.0), 1.0), (tone(0.11, 0.7), 0.5), (_noise(1, n, 9), 0.35)])
    right = mix([(tone(0.021, 1.1), 1.0), (tone(0.113, 0.2), 0.5), (_noise(2, n, 9), 0.35)])
    centre = mix([(tone(0.035, 0.4), 1.0), (_noise(3, n, 21), 0.4)])
    lfe = mix([(tone(0.002, 0.0), 1.0), (tone(0.0035, 0.9), 0.6)])
    back_l = mix([(tone(0.07, 2.0), 1.0), (_noise(4, n, 5), 0.6)])
    back_r = mix([(tone(0.073, 0.3), 1.0), (_noise(5, n, 5), 0.6)])
    return [left, right, centre, lfe, back_l, back_r]


def write_pcm16(path: Path, channels: list[list[float]], rate: int) -> None:
    frames = len(channels[0])
    payload = bytearray()
    for i in range(frames):
        for channel in channels:
            value = max(-1.0, min(1.0, channel[i]))
            payload += struct.pack("<h", round(value * 32767.0))
    with wave.open(str(path), "wb") as out:
        out.setnchannels(len(channels))
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(bytes(payload))


# --------------------------------------------------------------------------
# Vector definitions
# --------------------------------------------------------------------------


@dataclass(frozen=True)
class Vector:
    """One published stream and everything the manifest says about it."""

    ident: str
    codec: str  # "AC-3" | "E-AC-3" | "E-AC-3 (Atmos)"
    exercises: str
    args: list[str]  # ac3cli argv after the output path is substituted
    source: str | None  # key into SOURCES, or None for a synthesis command
    suffix: str
    sample_rate: int = 48000
    layout: str = ""
    bitrate_kbps: int | None = None
    tools: list[str] = field(default_factory=list)
    extra_inputs: list[str] = field(default_factory=list)


def _ffmpeg_support(vector: Vector) -> dict[str, str]:
    """Which vectors FFmpeg can read, per docs/verification.md's oracle table.

    Three states, not two: `full` (FFmpeg decodes the audio, so it is a real
    external oracle), `header_only` (it walks the framing correctly but
    refuses the audio), and `none` (it cannot read the stream at all). The
    distinction matters to an implementer choosing what to cross-check
    against.
    """
    if vector.codec == "AC-3":
        return {"support": "full", "note": "every AC-3 mode this encoder produces"}
    if vector.sample_rate in FSCOD2_RATES:
        return {
            "support": "header_only",
            "note": (
                "ffprobe walks every syncframe of a reduced-rate stream (count, size, spacing "
                "and sample_rate all confirmed) but FFmpeg's E-AC-3 decoder refuses the audio "
                "('Not yet implemented in FFmpeg'); so does Dolby's own Reference Player"
            ),
        }
    if vector.layout in TWO_DEPENDENT_LAYOUTS:
        return {
            "support": "none",
            "note": (
                "ff_ac3_parse_header rejects substreamid != 0, so a second dependent "
                "substream is unreadable in any container"
            ),
        }
    if any(tool in UNMODELLED_TOOLS for tool in vector.tools):
        return {
            "support": "none",
            "note": (
                "FFmpeg's Annex E parser has no syntax for enhanced coupling or transient "
                "pre-noise processing - it does not reject these streams, it has no model "
                "of the bits"
            ),
        }
    return {
        "support": "full",
        "note": "FFmpeg decodes this stream, so it is available as an independent oracle",
    }


# The 48 kHz sources are copied from the repo; the rest are synthesized above.
# `channels` is what the file holds, not what a vector encodes it as - the
# encoder folds a wider source down (§7.8) and leaves a narrower one's missing
# channels silent, so one 6-channel source drives every layout at its rate.
SOURCES: dict[str, dict] = {
    "reference_51": {
        "file": "reference_51.wav",
        "copy_from": GOLDEN_AUDIO / "reference_51.wav",
        "sample_rate": 48000,
        "channels": 6,
        "origin": (
            "tests/golden/audio/reference_51.wav, generated by "
            "tools/generators/gen_gold_reference_wav.py - the fixture the project's own "
            "gold-reference CI gate runs on"
        ),
    },
    "reference_stereo": {
        "file": "reference_stereo.wav",
        "copy_from": GOLDEN_AUDIO / "reference_stereo.wav",
        "sample_rate": 48000,
        "channels": 2,
        "origin": (
            "tests/golden/audio/reference_stereo.wav, generated by "
            "tools/generators/gen_stereo_reference_wav.py - the fixture the encoder "
            "landscape comparison runs on"
        ),
    },
}

for _rate in (44100, 32000, *FSCOD2_RATES):
    SOURCES[f"synth_{_rate}"] = {
        "file": f"synth_6ch_{_rate}.wav",
        "synthesize": _rate,
        "sample_rate": _rate,
        "channels": 6,
        "seconds": 1.0,
        "origin": (
            "synthesized by tools/generators/gen_conformance_vectors.py - no checked-in "
            "fixture exists at this rate"
        ),
    }


def build_vector_list() -> list[Vector]:
    """Every vector in the set, in manifest order.

    Coverage is a cross of two axes rather than their full product, which
    would be enormous and mostly redundant: every layout and every coding tool
    at 48 kHz (the rate real content uses), and every sample rate the encoder
    can emit at a representative pair of layouts. The manifest says so too, so
    nobody reads a missing 7.1.4-at-16-kHz vector as a coverage claim.
    """
    v: list[Vector] = []

    # --- AC-3, 48 kHz: every coding mode ---------------------------------
    ac3_modes = [
        ("mono", "mono", 192, "1/0 mono"),
        ("stereo", "stereo", 192, "2/0 stereo"),
        ("stereo-96", "stereo", 96, "2/0 stereo at the low end of the rate range"),
        ("stereo-448", "stereo", 448, "2/0 stereo at 448 kbit/s"),
        ("51", "51", 384, "3/2 + LFE"),
        ("51-640", "51", 640, "3/2 + LFE at the top of Table 5.18"),
        ("30", "L,C,R", 192, "3/0, addressed by Table 5.8 location list (no preset names it)"),
        ("21", "L,R,Cs", 192, "2/1 mono surround, by location list"),
        ("31", "L,C,R,Cs", 192, "3/1 mono surround, by location list"),
        ("22", "L,R,Ls,Rs", 192, "2/2 quad, by location list"),
    ]
    for ident, layout, kbps, what in ac3_modes:
        v.append(
            Vector(
                ident=f"ac3-{ident}",
                codec="AC-3",
                exercises=f"{what} at {kbps} kbit/s, 48 kHz",
                args=["encode", "@source", "@out", str(kbps), layout],
                source="reference_51",
                suffix=".ac3",
                layout=layout,
                bitrate_kbps=kbps,
            )
        )

    # Channel coupling (§7.4) is `encode`'s own `couple` option token - the 'c'
    # layout suffix (stereoc, 51c) is a `sine`-only spelling of the same idea.
    for ident, layout, kbps in (("stereo-couple", "stereo", 192), ("51-couple", "51", 384)):
        v.append(
            Vector(
                ident=f"ac3-{ident}",
                codec="AC-3",
                exercises=f"{layout} with channel coupling on, {kbps} kbit/s, 48 kHz",
                args=["encode", "@source", "@out", str(kbps), layout, "couple"],
                source="reference_51",
                suffix=".ac3",
                layout=layout,
                bitrate_kbps=kbps,
                tools=["cpl"],
            )
        )

    # 1+1 needs exactly two source channels: its routing is a strict identity
    # on Ch1/Ch2, never a fold-down, so the 5.1 source is refused here.
    v.append(
        Vector(
            ident="ac3-dualmono",
            codec="AC-3",
            exercises=("1+1 dual mono - two independent programmes in one syncframe, "
                       "each with its own dialnorm"),
            args=["encode", "@source", "@out", "192", "1+1", "dialnorm=27", "dialnorm2=18"],
            source="reference_stereo",
            suffix=".ac3",
            layout="1+1",
            bitrate_kbps=192,
        )
    )
    v.append(
        Vector(
            ident="ac3-silence",
            codec="AC-3",
            exercises=("a silent stream - the degenerate but legal case a decoder "
                       "still has to frame correctly"),
            args=["silence", "@out", "1", "192"],
            source=None,
            suffix=".ac3",
            layout="stereo",
            bitrate_kbps=192,
        )
    )
    v.append(
        Vector(
            ident="ac3-drc-film-standard",
            codec="AC-3",
            exercises=("§7.7.1 dynrng words from the Film Standard profile, plus "
                       "dialnorm measured from the source"),
            args=["encode", "@source", "@out", "256", "51", "drc=film-standard", "dialnorm=auto"],
            source="reference_51",
            suffix=".ac3",
            layout="51",
            bitrate_kbps=256,
        )
    )
    v.append(
        Vector(
            ident="ac3-heavy-compr",
            codec="AC-3",
            exercises=("§7.7.2 heavy compression (compr), the word an RF-mode "
                       "decoder prefers over dynrng"),
            args=["encode", "@source", "@out", "192", "mono", "heavy",
                  "ceiling=-1.0", "dialogue=-24"],
            source="reference_51",
            suffix=".ac3",
            layout="mono",
            bitrate_kbps=192,
        )
    )
    v.append(
        Vector(
            ident="ac3-reference-transform",
            codec="AC-3",
            exercises=("the same 5.1 encode with the spec's direct §8.2.3.2 MDCT "
                       "instead of the default fast path - differs from ac3-51 "
                       "only at coefficient-rounding level"),
            args=["encode", "@source", "@out", "384", "51", "mode=reference"],
            source="reference_51",
            suffix=".ac3",
            layout="51",
            bitrate_kbps=384,
        )
    )

    # --- AC-3, the other two Table 5.6 sample rates ----------------------
    for rate in (44100, 32000):
        for layout, kbps in (("stereo", 192), ("51", 384)):
            v.append(
                Vector(
                    ident=f"ac3-{layout}-{rate}",
                    codec="AC-3",
                    exercises=(f"{layout} at {rate} Hz - Table 5.6's fscod "
                               f"{1 if rate == 44100 else 2}"),
                    args=["encode", "@source", "@out", str(kbps), layout],
                    source=f"synth_{rate}",
                    suffix=".ac3",
                    sample_rate=rate,
                    layout=layout,
                    bitrate_kbps=kbps,
                )
            )

    # --- E-AC-3, 48 kHz: every layout ------------------------------------
    for layout, kbps in (
        ("mono", 96),
        ("stereo", 128),
        ("51", 192),
        ("71", 256),
        ("512", 256),
        ("514", 256),
        ("714", 384),
    ):
        if layout in TWO_DEPENDENT_LAYOUTS:
            substream_desc = "two dependent substreams"
        elif layout in ("71", "512", "514"):
            substream_desc = "its dependent substream"
        else:
            substream_desc = "a single substream"
        v.append(
            Vector(
                ident=f"eac3-{layout}",
                codec="E-AC-3",
                exercises=(
                    f"{layout} with no Annex E coding tools - the bed plus {substream_desc}"
                ),
                args=["eac3-encode", "@source", "@out", str(kbps), "none", layout],
                source="reference_51",
                suffix=".ec3",
                layout=layout,
                bitrate_kbps=kbps,
            )
        )
    v.append(
        Vector(
            ident="eac3-dualmono",
            codec="E-AC-3",
            exercises="1+1 dual mono in Annex E syntax, each programme with its own dialnorm",
            args=[
                "eac3-encode", "@source", "@out", "192", "none", "1+1", "off",
                "dialnorm=27", "dialnorm2=18",
            ],
            source="reference_stereo",
            suffix=".ec3",
            layout="1+1",
            bitrate_kbps=192,
        )
    )

    # --- E-AC-3, 48 kHz: every Annex E tool and the combinations ---------
    tool_sets = [
        ("none", "no Annex E tool - the baseline every tool vector differs from"),
        ("cpl", "§E3.4 standard channel coupling"),
        ("spx", "§E3.6 spectral extension"),
        ("aht", "§E3.3 adaptive hybrid transform"),
        ("ecpl", "§E3.5 enhanced coupling"),
        ("tpn", "§3.7 transient pre-noise processing"),
        ("cpl+ecpl", "standard coupling with enhanced coupling on top"),
        ("spx+aht", "spectral extension and AHT together"),
        ("cpl:4+spx:5",
         "coupling and spectral extension with the band edges pinned rather than chosen"),
        ("all", "coupling, spectral extension and AHT stacked"),
        ("cpl+ecpl+tpn", "enhanced coupling and transient pre-noise processing together"),
        ("auto", "the rate-driven tool policy choosing for itself at 192 kbit/s over 5.1"),
        ("auto+spx:5", "the rate policy choosing, with the spectral-extension band edge pinned"),
    ]
    for tools, what in tool_sets:
        ident = "eac3-tools-" + tools.replace(":", "").replace("+", "-")
        v.append(
            Vector(
                ident=ident,
                codec="E-AC-3",
                exercises=f"5.1 at 192 kbit/s - {what}",
                args=["eac3-encode", "@source", "@out", "192", tools, "51"],
                source="reference_51",
                suffix=".ec3",
                layout="51",
                bitrate_kbps=192,
                tools=[t.split(":")[0] for t in tools.split("+")] if tools != "none" else [],
            )
        )

    # The wide layouts under the tools too, not just at 5.1: chanmap and the
    # dependent-substream paths behave differently once a tool is on.
    for layout in ("514", "714"):
        v.append(
            Vector(
                ident=f"eac3-{layout}-all",
                codec="E-AC-3",
                exercises=(f"{layout} with coupling, spectral extension and AHT "
                           "stacked across the dependent substreams"),
                args=["eac3-encode", "@source", "@out", "256", "all", layout],
                source="reference_51",
                suffix=".ec3",
                layout=layout,
                bitrate_kbps=256,
                tools=["cpl", "spx", "aht"],
            )
        )

    # --- E-AC-3: VBR -----------------------------------------------------
    for vbr, what in (
        ("q:0.3", "quality-targeted VBR with no bounds"),
        ("q:0.6,min:96,max:256", "quality-targeted VBR bounded top and bottom"),
    ):
        ident = "eac3-vbr-" + vbr.replace(":", "").replace(".", "").replace(",", "-")
        v.append(
            Vector(
                ident=ident,
                codec="E-AC-3",
                exercises=f"5.1, {what} - frame sizes vary across the stream",
                args=["eac3-encode", "@source", "@out", "192", "none", "51", vbr],
                source="reference_51",
                suffix=".ec3",
                layout="51",
                bitrate_kbps=None,
                tools=["vbr"],
            )
        )

    # --- E-AC-3: the other sample rates, including fscod2 ----------------
    for rate in (44100, 32000, *FSCOD2_RATES):
        for layout, kbps in (("stereo", 96), ("51", 192)):
            fscod2 = rate in FSCOD2_RATES
            v.append(
                Vector(
                    ident=f"eac3-{layout}-{rate}",
                    codec="E-AC-3",
                    exercises=(
                        f"{layout} at {rate} Hz - §E2.3.1.3 fscod2, a reduced-rate substream "
                        "(always six blocks, numblkscod never sent)"
                        if fscod2
                        else f"{layout} at {rate} Hz"
                    ),
                    args=["eac3-encode", "@source", "@out", str(kbps), "none", layout],
                    source=f"synth_{rate}",
                    suffix=".ec3",
                    sample_rate=rate,
                    layout=layout,
                    bitrate_kbps=kbps,
                )
            )

    # --- Atmos -----------------------------------------------------------
    v.append(
        Vector(
            ident="atmos-4obj",
            codec="E-AC-3 (Atmos)",
            exercises=("a 5.1 bed plus four synthetic orbiting objects - OAMD + JOC "
                       "in the EMDF container (TS 103 420), unsigned"),
            args=["atmos", "@out", "2", "256", "4", "4", "objects"],
            source=None,
            suffix=".ec3",
            layout="51",
            bitrate_kbps=256,
            tools=["joc", "oamd"],
        )
    )
    v.append(
        Vector(
            ident="atmos-bed51",
            codec="E-AC-3 (Atmos)",
            exercises=("the same programme in bed-only mode - objects panned into "
                       "the 5.1 bed with no EMDF container at all, the "
                       "graceful-fallback half of the either/or"),
            args=["atmos", "@out", "2", "256", "4", "4", "bed51"],
            source=None,
            suffix=".ec3",
            layout="51",
            bitrate_kbps=256,
        )
    )
    v.append(
        Vector(
            ident="atmos-encode-6obj",
            codec="E-AC-3 (Atmos)",
            exercises=("every source channel carried as its own object rather than "
                       "a bed - six objects from the 5.1 fixture"),
            args=["atmos-encode", "@source", "@out", "256", "6"],
            source="reference_51",
            suffix=".ec3",
            layout="51",
            bitrate_kbps=256,
            tools=["joc", "oamd"],
        )
    )
    v.append(
        Vector(
            ident="atmos-path",
            codec="E-AC-3 (Atmos)",
            exercises=("object motion from an authored keyframe file instead of "
                       "the built-in orbit - two objects on crossing paths"),
            args=["atmos-path", "@out", "@input:paths.txt", "3", "256", "2"],
            source=None,
            suffix=".ec3",
            layout="51",
            bitrate_kbps=256,
            tools=["joc", "oamd"],
            extra_inputs=["paths.txt"],
        )
    )
    # Ids become file names and manifest keys, so keep them to one boring
    # shape rather than whatever a tool token happened to spell.
    for vector in v:
        assert re.fullmatch(r"[a-z0-9-]+", vector.ident), f"unusable vector id: {vector.ident}"
    assert len({vector.ident for vector in v}) == len(v), "duplicate vector id"
    return v


# The keyframe file atmos-path reads: 'object time_s x y z gain lfe_send',
# '#' starts a comment. Same grammar the GUI's timeline exports.
ATMOS_PATHS = """\
# object  time_s   x     y     z    gain  lfe_send
0         0.0      0.1   0.5   0.0  0.7   0.0
0         3.0      0.9   0.5   1.0  0.7   0.0
1         0.0      0.5   0.1   0.0  0.7   0.0
1         3.0      0.5   0.9   1.0  0.7   0.0
"""

SIGNED_VECTOR = Vector(
    ident="atmos-4obj-signed",
    codec="E-AC-3 (Atmos)",
    exercises=("atmos-4obj's programme with the EMDF object container signed - the "
               "authenticity tag a licensed decoder gates object decoding on"),
    args=["atmos", "@out", "2", "256", "4", "4", "objects", "sign-objects"],
    source=None,
    suffix=".ec3",
    layout="51",
    bitrate_kbps=256,
    tools=["joc", "oamd", "signing"],
)


# --------------------------------------------------------------------------
# Running the CLI
# --------------------------------------------------------------------------


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


LEVEL_ROW = re.compile(r"^\s{2}(\S+)\s+(-?\d+\.\d+|-inf)\s+(-?\d+\.\d+|-inf)\s")


def parse_levels(text: str) -> list[dict]:
    """Per-channel peak/RMS out of `ac3cli levels`.

    This is the one part of a decode that IS comparable across
    implementations: peak and RMS in dBFS survive any correct decoder's own
    rounding, where a PCM hash does not. See the README this script writes.
    """
    rows: list[dict] = []
    in_table = False
    for line in text.splitlines():
        if line.startswith("  ch "):
            in_table = True
            continue
        if not in_table:
            continue
        if not line.startswith("  ") or line.startswith("  soundfield"):
            break
        match = LEVEL_ROW.match(line)
        if match is None:
            break
        rows.append(
            {
                "channel": match.group(1),
                "peak_dbfs": _db(match.group(2)),
                "rms_dbfs": _db(match.group(3)),
            }
        )
    return rows


def _db(text: str) -> float | None:
    """A dBFS reading as a number; null for a channel with no signal at all."""
    return None if text == "-inf" else float(text)


class CliError(RuntimeError):
    pass


def run_cli(cli: Path, args: list[str], cwd: Path, env: dict | None = None) -> str:
    result = subprocess.run(
        [str(cli), *args],
        cwd=str(cwd),
        capture_output=True,
        text=True,
        env=env,
        check=False,
    )
    if result.returncode != 0:
        raise CliError(
            f"ac3cli {' '.join(args)} failed ({result.returncode})\n"
            f"--- stdout ---\n{result.stdout}\n--- stderr ---\n{result.stderr}"
        )
    return result.stdout + result.stderr


# `decode`'s own object line, e.g. "4 dynamic objects + the bed's LFE = 5
# objects, OAMD present, JOC audio reconstructed". Kept verbatim rather than
# scraped down to a number: which objects are dynamic, whether OAMD was found
# and whether JOC audio reconstructed are three separate claims.
OBJECT_LINE = re.compile(r"^\s*(\d+ dynamic object.*)$", re.MULTILINE)


def generate(
    cli: Path,
    out_dir: Path,
    version: str,
    sign: bool,
    signing_key: Path | None,
) -> dict:
    source_dir = out_dir / "source"
    vector_dir = out_dir / "vectors"
    source_dir.mkdir(parents=True)
    vector_dir.mkdir(parents=True)

    cli_version = run_cli(cli, ["--version"], out_dir).strip()

    # --- sources ---------------------------------------------------------
    sources: list[dict] = []
    for key, spec in SOURCES.items():
        target = source_dir / spec["file"]
        if "copy_from" in spec:
            origin = spec["copy_from"]
            if not origin.is_file():
                raise CliError(f"source fixture missing: {origin}")
            shutil.copyfile(origin, target)
        else:
            write_pcm16(target, synth_six_channel(spec["synthesize"], spec["seconds"]),
                        spec["sample_rate"])
        spec["path"] = f"source/{spec['file']}"
        sources.append(
            {
                "id": key,
                "path": spec["path"],
                "sample_rate": spec["sample_rate"],
                "channels": spec["channels"],
                "format": "PCM16",
                "bytes": target.stat().st_size,
                "sha256": sha256_file(target),
                "origin": spec["origin"],
            }
        )

    (out_dir / "paths.txt").write_text(ATMOS_PATHS, encoding="utf-8", newline="\n")

    vectors = build_vector_list()
    if sign:
        vectors.append(SIGNED_VECTOR)

    env = dict(os.environ)
    if signing_key is not None:
        env["AC3FORGE_SIGNING_KEY_FILE"] = str(signing_key.resolve())

    entries: list[dict] = []
    scratch = out_dir / ".scratch"
    scratch.mkdir()
    for vector in vectors:
        codec_dir = {"AC-3": "ac3", "E-AC-3": "eac3"}.get(vector.codec, "atmos")
        rel = f"vectors/{codec_dir}/{vector.ident}{vector.suffix}"
        target = out_dir / rel
        target.parent.mkdir(parents=True, exist_ok=True)

        args: list[str] = []
        for token in vector.args:
            if token == "@out":
                args.append(str(target))
            elif token == "@source":
                args.append(str(out_dir / SOURCES[vector.source]["path"]))
            elif token.startswith("@input:"):
                args.append(str(out_dir / token.removeprefix("@input:")))
            else:
                args.append(token)
        run_cli(cli, args, out_dir, env)

        # Decode with this project's own decoder, hash the PCM, then discard
        # it: the WAV is the bulk of what a bundle would weigh and adds
        # nothing a consumer cannot regenerate. The hash proves the bundle
        # was built by the toolchain the manifest names; the levels below are
        # what another implementation actually compares against.
        decoded = scratch / f"{vector.ident}.wav"
        decode_log = run_cli(cli, ["decode", str(target), str(decoded)], out_dir)
        decode_sha = sha256_file(decoded)
        decoded.unlink()
        levels = parse_levels(run_cli(cli, ["levels", str(target)], out_dir))

        object_match = OBJECT_LINE.search(decode_log)
        objects = object_match.group(1).strip() if object_match else None

        entries.append(
            {
                "id": vector.ident,
                "path": rel,
                "codec": vector.codec,
                "exercises": vector.exercises,
                "sample_rate": vector.sample_rate,
                "layout": vector.layout,
                "bitrate_kbps": vector.bitrate_kbps,
                "tools": vector.tools,
                "source": SOURCES[vector.source]["path"] if vector.source else None,
                "extra_inputs": vector.extra_inputs,
                "command": ["ac3cli", *[a.replace(str(out_dir) + os.sep, "").replace("\\", "/")
                                        for a in args]],
                "bytes": target.stat().st_size,
                "sha256": sha256_file(target),
                "decoded_pcm_sha256": decode_sha,
                "decoded_objects": objects,
                "decoded_levels": levels,
                "ffmpeg": _ffmpeg_support(vector),
            }
        )

    shutil.rmtree(scratch)

    manifest = {
        "schema": SCHEMA,
        "version": version,
        "generator": "tools/generators/gen_conformance_vectors.py",
        "built_with": cli_version,
        "hash_scope": (
            "sha256 values are per-toolchain. Encoded output is not currently bit-identical "
            "across compilers and architectures (docs/building.md; roadmap VX11/VX12 - the "
            "arm64 legs sit 6.0 dB off every x86 leg on the gold gate), so a bundle "
            "regenerated with a different compiler or on a different architecture will differ. "
            "Regenerating with the toolchain named in built_with reproduces every hash here "
            "exactly."
        ),
        "coverage": (
            "Every layout and coding tool at 48 kHz, and every sample rate the encoder can emit "
            "at a representative pair of layouts - a cross of two axes, not their full product. "
            "A combination absent here is absent because it is redundant, not because it is "
            "unsupported."
        ),
        "signing": (
            {
                "signed_vectors": True,
                "note": ("signed with an operator-supplied key; the key itself is not "
                         "part of this bundle and is not recoverable from it"),
            }
            if sign
            else {
                "signed_vectors": False,
                "note": (
                    "Object signing needs a key this project does not ship (docs/concepts/"
                    "object-signing.md). Regenerate with --sign and AC3FORGE_SIGNING_KEY_FILE "
                    "to add the signed Atmos vector."
                ),
            }
        ),
        "licensing": (
            "Every stream here is this project's own encoder output, encoded from source PCM "
            "this project generated. Nothing derived from a third-party encoder or decoder is "
            "included. Same licence as the project (see LICENSE)."
        ),
        "sources": sources,
        "vectors": entries,
    }
    return manifest


# --------------------------------------------------------------------------
# Bundle README
# --------------------------------------------------------------------------


def write_readme(out_dir: Path, manifest: dict) -> None:
    counts: dict[str, int] = {}
    for entry in manifest["vectors"]:
        counts[entry["codec"]] = counts.get(entry["codec"], 0) + 1
    by_codec = ", ".join(f"{n} {codec}" for codec, n in sorted(counts.items()))
    ffmpeg_full = sum(1 for e in manifest["vectors"] if e["ffmpeg"]["support"] == "full")
    ffmpeg_header = sum(1 for e in manifest["vectors"] if e["ffmpeg"]["support"] == "header_only")
    ffmpeg_none = sum(1 for e in manifest["vectors"] if e["ffmpeg"]["support"] == "none")

    text = f"""\
# ac3forge conformance vectors {manifest["version"]}

{len(manifest["vectors"])} AC-3 / E-AC-3 / Dolby Atmos streams ({by_codec}), each with the PCM it
was encoded from and a description of what it exercises. Produced by
[ac3forge](https://github.com/iainchesworthlabs/ac3forge), a clean-room C++23 implementation.

No free ATSC or ETSI conformance bitstreams exist publicly. This set exists so another decoder
implementer has something concrete to test against.

## Layout

```
MANIFEST.json    every vector: what it exercises, its hashes, and whether FFmpeg can read it
source/          the PCM each vector was encoded from
vectors/ac3/     AC-3 (A/52 Annex A-D)
vectors/eac3/    E-AC-3 (A/52 Annex E)
vectors/atmos/   E-AC-3 carrying Dolby Atmos objects (OAMD + JOC, ETSI TS 103 420)
paths.txt        the authored object-motion keyframes one Atmos vector was built from
```

## Using it to test a decoder

**Decode the stream, measure against the source PCM.** `MANIFEST.json` gives each vector's
`source`. Decode the vector, align it against that WAV and measure SNR. This is the check that
means something across implementations.

**Do not compare PCM byte-for-byte.** `decoded_pcm_sha256` is this project's own decoder's
output on the toolchain named in `built_with`. Two correct decoders disagree in the last bits of
every float sample; the hash is here so a regenerated bundle can be checked against this one, not
so your decoder can be checked against ours.

**Do compare levels.** Each vector's `decoded_levels` gives per-channel peak and RMS in dBFS.
Those survive any correct decoder's own rounding, so a channel that is more than a fraction of a
dB out - or in the wrong slot - is a real finding. Channel names are A/52 Table 5.8 order for
AC-3 and Table E2.5 location order for E-AC-3.

**Read `exercises` before chasing a failure.** It names the syntax each vector is there for,
down to the clause: which Annex E tools are on, whether the stream carries dependent substreams,
whether it is a reduced-rate (`fscod2`) stream, whether object metadata rides in the EMDF
container.

## What FFmpeg can and cannot read

{ffmpeg_full} of these vectors decode under FFmpeg, {ffmpeg_header} are framed correctly by it
but refuse to decode, and {ffmpeg_none} it cannot read at all. Every vector's `ffmpeg` field says
which and why. The three states are not interchangeable: a stream FFmpeg has no syntax for is not
the same as one it rejects.

The gaps are FFmpeg's, not this encoder's - a second dependent substream (7.1.4), enhanced
coupling, transient pre-noise processing, and `fscod2` audio. Those four are exactly where an
independent implementation is most useful, because nothing else public reads them either.

## Reproducing this bundle

```
git clone https://github.com/iainchesworthlabs/ac3forge && cd ac3forge
git checkout {manifest["version"]}
cmake --preset config-linux-gcc && cmake --build --preset build-linux-gcc
python tools/generators/gen_conformance_vectors.py \\
    --cli build/config-linux-gcc/bin/ac3cli --out dist/vectors
```

{manifest["hash_scope"]}

## Coverage

{manifest["coverage"]}

Every vector's source PCM is synthetic - sine tones plus seeded, band-limited noise. That is
enough to exercise every coding tool, and it is what makes the set redistributable, but it is
not real programme material: a defect that only shows on speech or music is not something this
set can find.

## Signing

{manifest["signing"]["note"]}

## Licensing

{manifest["licensing"]}
"""
    (out_dir / "README.md").write_text(text, encoding="utf-8", newline="\n")


# --------------------------------------------------------------------------
# Deterministic archive
# --------------------------------------------------------------------------


def write_archive(out_dir: Path, archive: Path) -> None:
    """tar.gz with every timestamp, owner and mode pinned.

    A plain tarfile/gzip write embeds mtimes and the build's own clock, so two
    identical bundles would produce two different archives and the hash would
    stop meaning anything.
    """
    root = out_dir.name
    files = sorted(p for p in out_dir.rglob("*") if p.is_file())
    raw = archive.parent / archive.name.removesuffix(".gz")
    with tarfile.open(raw, "w", format=tarfile.GNU_FORMAT) as tar:
        for path in files:
            arcname = f"{root}/{path.relative_to(out_dir).as_posix()}"
            info = tar.gettarinfo(str(path), arcname=arcname)
            info.mtime = 0
            info.uid = 0
            info.gid = 0
            info.uname = ""
            info.gname = ""
            info.mode = 0o644
            with path.open("rb") as handle:
                tar.addfile(info, handle)
    with (
        raw.open("rb") as src,
        archive.open("wb") as dst,
        gzip.GzipFile(fileobj=dst, mode="wb", compresslevel=9, mtime=0) as gz,
    ):
        shutil.copyfileobj(src, gz)
    raw.unlink()


# --------------------------------------------------------------------------


def strip_volatile(manifest: dict) -> dict:
    """The manifest minus the fields a second run is allowed to differ in.

    `built_with` carries `git describe`, which moves with every commit - it is
    provenance, not content. Everything else must match exactly.
    """
    trimmed = dict(manifest)
    trimmed.pop("built_with", None)
    return trimmed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cli", required=True, type=Path, help="path to a built ac3cli")
    parser.add_argument("--out", required=True, type=Path, help="bundle directory to create")
    parser.add_argument(
        "--version",
        default="",
        help="version stamped into the manifest; defaults to what ac3cli --version reports",
    )
    parser.add_argument("--archive", action="store_true", help="also write <out>.tar.gz")
    parser.add_argument(
        "--sign",
        action="store_true",
        help="also emit the signed Atmos vector (needs an operator-supplied key)",
    )
    parser.add_argument("--signing-key", type=Path, default=None, help="key file for --sign")
    parser.add_argument(
        "--check-determinism",
        action="store_true",
        help="generate a second bundle into a temporary directory and fail if any hash differs",
    )
    args = parser.parse_args()

    if not args.cli.is_file():
        print(f"error: no such ac3cli: {args.cli}", file=sys.stderr)
        return 1
    # Every run_cli call below sets cwd to the bundle directory, so anything
    # relative would resolve differently (or not at all) once it does: --cli
    # against the bundle instead of here, and every path derived from --out
    # against itself a second time. Both are absolute from here on.
    args.cli = args.cli.resolve()
    args.out = args.out.resolve()
    if args.sign and args.signing_key is None and not (
        os.environ.get("AC3FORGE_SIGNING_KEY") or os.environ.get("AC3FORGE_SIGNING_KEY_FILE")
    ):
        print(
            "error: --sign needs a key: pass --signing-key, or set AC3FORGE_SIGNING_KEY / "
            "AC3FORGE_SIGNING_KEY_FILE. This project ships no key and none is invented here.",
            file=sys.stderr,
        )
        return 1

    version = args.version
    if not version:
        first = run_cli(args.cli, ["--version"], Path.cwd()).splitlines()[0]
        version = first.split()[-1]

    if args.out.exists():
        shutil.rmtree(args.out)
    args.out.mkdir(parents=True)

    try:
        manifest = generate(args.cli, args.out, version, args.sign, args.signing_key)
    except CliError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    (args.out / "MANIFEST.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=False) + "\n", encoding="utf-8", newline="\n"
    )
    write_readme(args.out, manifest)

    total = sum(p.stat().st_size for p in args.out.rglob("*") if p.is_file())
    print(f"{len(manifest['vectors'])} vectors, {len(manifest['sources'])} sources, "
          f"{total / 1e6:.1f} MB in {args.out}")

    if args.check_determinism:
        with tempfile.TemporaryDirectory() as tmp:
            second = Path(tmp) / args.out.name
            again = generate(args.cli, second, version, args.sign, args.signing_key)
        if strip_volatile(again) != strip_volatile(manifest):
            first_ids = {e["id"]: e for e in manifest["vectors"]}
            for entry in again["vectors"]:
                other = first_ids.get(entry["id"])
                if other != entry:
                    print(f"error: vector {entry['id']} differs between runs", file=sys.stderr)
            print("error: regenerating did not reproduce the same bundle", file=sys.stderr)
            return 1
        print("determinism: a second run reproduced every hash")

    if args.archive:
        # Not with_suffix(): a bundle directory named for a version ends in
        # ".1", which Path would treat as the suffix to replace.
        archive = args.out.parent / (args.out.name + ".tar.gz")
        write_archive(args.out, archive)
        print(f"{archive} ({archive.stat().st_size / 1e6:.1f} MB), sha256 {sha256_file(archive)}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
