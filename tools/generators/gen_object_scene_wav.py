"""Multi-object scene generator for the object-reconstruction quality leg.

Produces two checked-in files that together define one fixed Atmos scene:

  tests/golden/audio/reference_objects.wav    5 mono essences, one per object,
                                              as the channels of a 48 kHz PCM16
                                              WAV (`ac3cli atmos-encode` makes
                                              each source channel an object)
  tests/golden/audio/reference_objects.paths  where each of those objects sits
                                              in the room, in atmos-path's own
                                              keyframe format

`tools/ci/quality_race.py`'s `objects` mode encodes the pair with
`atmos-encode`, decodes the result back to per-object WAVs with `decode
<stream> <bed> <objects_dir>`, and scores each object against the channel it
came from. Splitting the scene across two files is what makes that possible:
`atmos-encode`'s own default placement fans objects out evenly AND applies a
0.7/sqrt(N) headroom gain, so a reconstructed object comes back at its
intended level rather than the source's, and SNR against the source would be
measuring that deliberate level change rather than reconstruction quality.
The committed placements pin both, at unit gain.

Independent of ac3forge's own encoder/decoder in the same sense as
tools/generators/gen_gold_reference_wav.py: built here from first principles
(sin()/pseudo-random noise/simple FIR smoothing), not bootstrapped by
decoding one of our own encodes.

The scene, borrowing its cast from examples/station_broadcast.cpp (a station
anthem, a comet, an engine, a work pod) because a scene of distinct diegetic
sources is exactly what a parametric object coder has to keep apart:

  0 broadcast  a chord-and-formant radio anthem, nailed front-centre
  1 comet      band-limited noise with a slow swell, drifting left to right
               across the scene - the one object that MOVES, so the leg
               scores the matrix's per-frame interpolation and not only its
               steady state
  2 engine     a low harmonic rumble with a slow tremolo, rear right
  3 pod-hi     a high whine cluster, rear left and overhead
  4 pod-lo     narrow-band chatter, rear left at floor level

3 and 4 are a direction-degenerate pair, and they are in the scene to probe
one specific thing. The bed has no height channels, so elevation costs
nothing in the downmix and two objects that differ only in height are panned
into the five bed channels identically - ac3/oba/atmos.hpp's own module
comment names that as the limit of what JOC can do. Direction is not the
matrix's only axis, though: it solves per FREQUENCY BAND (AtmosConfig::
num_bands_idx, Table 50), so a pair that shares a direction but not a
spectrum is still separable, and this pair measured 22.7/22.0 dB at 448
kbit/s when it was built - as well as the well-separated objects. That is
what makes the two rows worth having: they are the scene's only objects
whose separation depends on band resolution ALONE, so a regression that
coarsened the banded solve would collapse exactly these two and leave the
rest of the scene reading normally. Deliberately not pushed to full spectral
overlap as well: that scores about -6 dB, where a further regression has no
room left to show, and a row pinned to the floor detects nothing.

Levels are chosen so the 5.1 downmix of all five objects at unit gain never
clips - `atmos-encode` prints a per-channel `clipped` column, and it must
read `-` on every channel for this fixture.

Stdlib-only (no numpy), same reasoning as gen_gold_reference_wav.py: this
runs once, locally, to produce the checked-in files, never in CI.

Usage (repo root):  python tools/generators/gen_object_scene_wav.py
"""

import math
import random
import struct
import wave
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
AUDIO_DIR = REPO / "tests" / "golden" / "audio"
WAV_OUT = AUDIO_DIR / "reference_objects.wav"
PATHS_OUT = AUDIO_DIR / "reference_objects.paths"

RATE = 48000
# 2.0 s is 62 syncframes at 1536 samples - far past the "silence and frame 0
# give false passes" threshold, while keeping the checked-in WAV under a
# megabyte (5 channels x 2 bytes x 96,000 frames = 960 KB).
DURATION_S = 2.0
N = int(RATE * DURATION_S)

# Object names, in channel order. quality_race.py's `objects` mode reads this
# same list (it is duplicated there rather than imported - that module is
# numpy-based and CI-side, this one is stdlib-only and local-only) and uses
# each name as the trend series' `variant`, so renaming one here starts a new
# series rather than continuing the old one.
OBJECT_NAMES = ["broadcast", "comet", "engine", "pod-hi", "pod-lo"]


def smooth(samples: list[float], taps: int) -> list[float]:
    """Boxcar FIR, same helper as gen_gold_reference_wav.py's - enough to
    turn white noise into band-limited hiss without a filter design library."""
    out = [0.0] * len(samples)
    window_sum = 0.0
    window: list[float] = []
    for i, s in enumerate(samples):
        window.append(s)
        window_sum += s
        if len(window) > taps:
            window_sum -= window.pop(0)
        out[i] = window_sum / len(window)
    return out


def noise(seed: int, length: int, taps: int) -> list[float]:
    rng = random.Random(seed)
    raw = [rng.uniform(-1.0, 1.0) for _ in range(length)]
    filtered = smooth(raw, taps)
    peak = max(abs(v) for v in filtered) or 1.0
    return [v / peak for v in filtered]


def normalized(samples: list[float], peak: float) -> list[float]:
    scale = max(abs(v) for v in samples) or 1.0
    return [v * peak / scale for v in samples]


def make_objects() -> list[list[float]]:
    t = [i / RATE for i in range(N)]

    # broadcast: a sustained triad with a formant stack over it, the "anthem"
    # coming off the station. Harmonically dense and mid-band, so it is the
    # object with the most spectral overlap with everything else.
    chord = [sum(math.sin(2 * math.pi * f * tt) for f in (196.0, 246.94, 293.66))
             for tt in t]
    formants = [sum(0.3 * math.sin(2 * math.pi * f * tt) for f in (660.0, 1180.0, 2500.0))
                for tt in t]
    tremolo = [0.75 + 0.25 * math.sin(2 * math.pi * 3.1 * tt) for tt in t]
    broadcast = normalized([c * (1.0 + 0.35 * fo) * tr
                            for c, fo, tr in zip(chord, formants, tremolo)], 0.26)

    # comet: band-limited hiss with a slow swell, the only moving object. Its
    # own envelope is deliberately smooth - a moving object whose level also
    # jumped around would confound "did the pan interpolate" with "did the
    # gain interpolate".
    hiss = noise(0x0C0E7, N, 24)
    swell = [0.35 + 0.65 * math.sin(math.pi * tt / DURATION_S) for tt in t]
    comet = normalized([h * s for h, s in zip(hiss, swell)], 0.22)

    # engine: a low harmonic stack with a slow tremolo. Its energy sits below
    # everything else, which is what makes it the easiest object to pull back
    # out - a useful contrast to pod-lo at the other end of the scene.
    stack = [sum(math.sin(2 * math.pi * f * tt + 0.4 * k) / (k + 1.0)
                 for k, f in enumerate((58.0, 116.0, 174.0, 232.0)))
             for tt in t]
    beat = [0.7 + 0.3 * math.sin(2 * math.pi * 1.7 * tt) for tt in t]
    engine = normalized([s * b for s, b in zip(stack, beat)], 0.24)

    # pod-hi: a high whine cluster - two closely-spaced partials plus their
    # difference-tone beat, well above where coupling starts.
    whine = [math.sin(2 * math.pi * 4700.0 * tt) + 0.7 * math.sin(2 * math.pi * 5180.0 * tt)
             + 0.3 * math.sin(2 * math.pi * 9400.0 * tt) for tt in t]
    pod_hi = normalized(whine, 0.20)

    # pod-lo: narrow-band chatter - a carrier bursting on and off under an
    # irregular gate, so the hard pair is not two steady tones (which the
    # per-band power split handles far too easily to be a real test).
    gate_rng = random.Random(0x0D0C4)
    gate = []
    level = 0.0
    hold = 0
    for _ in range(N):
        if hold == 0:
            level = gate_rng.choice((0.0, 0.35, 0.8, 1.0))
            hold = gate_rng.randint(900, 5200)
        hold -= 1
        gate.append(level)
    gate = smooth(gate, 256)
    carrier = [math.sin(2 * math.pi * 1420.0 * tt) + 0.45 * math.sin(2 * math.pi * 2130.0 * tt)
               for tt in t]
    pod_lo = normalized([c * g for c, g in zip(carrier, gate)], 0.22)

    return [broadcast, comet, engine, pod_hi, pod_lo]


# "object time_s x y z gain lfe_send" rows, in atmos-path's own format (see
# parse_path_file in apps/cli/commands/atmos.cpp). x/y/z are room coordinates
# in [0,1]: x runs left(0) to right(1), y front(0) to back(1), z floor(0) to
# ceiling(1). Every gain is 1.0 on purpose - see the module docstring.
PLACEMENTS: list[tuple[int, list[tuple[float, float, float, float]]]] = [
    # broadcast: nailed front-centre, slightly raised, for the whole scene.
    (0, [(0.0, 0.50, 0.02, 0.12)]),
    # comet: a straight left-to-right drift at constant height. Two keyframes
    # is all a straight line needs; the encoder interpolates between them
    # every frame, which is the part being measured.
    (1, [(0.0, 0.04, 0.34, 0.55), (DURATION_S, 0.96, 0.34, 0.55)]),
    # engine: parked rear right.
    (2, [(0.0, 0.93, 0.96, 0.00)]),
    # pod-hi / pod-lo: the direction-degenerate pair - rear left, 0.06 apart
    # in x (a small azimuth offset) and a full room height apart in z, which
    # the bed cannot represent at all. See the module docstring for what the
    # two rows they produce actually measure.
    (3, [(0.0, 0.12, 0.88, 1.00)]),
    (4, [(0.0, 0.18, 0.86, 0.00)]),
]


def write_wav(objects: list[list[float]]) -> None:
    frames = bytearray()
    for n in range(N):
        for essence in objects:
            v = max(-1.0, min(1.0, essence[n]))
            frames += struct.pack("<h", int(round(v * 32767.0)))
    WAV_OUT.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(WAV_OUT), "wb") as w:
        w.setnchannels(len(objects))
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(frames))


def write_paths() -> None:
    lines = [
        "# The fixed placement of tests/golden/audio/reference_objects.wav's",
        "# objects, in atmos-path's keyframe format:",
        "#",
        "#   object time_s x y z gain lfe_send",
        "#",
        "# Generated by tools/generators/gen_object_scene_wav.py - edit that,",
        "# not this. Every gain is 1.0 so a reconstructed object comes back at",
        "# the source channel's own level and SNR against it measures",
        "# reconstruction quality rather than a deliberate headroom gain.",
        "",
    ]
    for index, keyframes in PLACEMENTS:
        lines.append(f"# {index}: {OBJECT_NAMES[index]}")
        for time_s, x, y, z in keyframes:
            lines.append(f"{index} {time_s:.3f} {x:.3f} {y:.3f} {z:.3f} 1.000 0.000")
        lines.append("")
    PATHS_OUT.write_text("\n".join(lines), encoding="utf-8")


def main() -> None:
    objects = make_objects()
    assert len(objects) == len(OBJECT_NAMES)
    write_wav(objects)
    write_paths()
    print(f"wrote {WAV_OUT} ({WAV_OUT.stat().st_size} bytes, "
          f"{len(objects)} objects, {DURATION_S:g}s)")
    print(f"wrote {PATHS_OUT}")


if __name__ == "__main__":
    main()
