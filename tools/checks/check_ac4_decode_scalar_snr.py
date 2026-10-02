"""Gate the AC-4 decoder's float and fixed-point builds against its double build, on every
committed stream (planning/ac4.md, D14a and D14d).

`ICLFORGE_DECODE_SCALAR=float` builds the decoder's QMF banks, transforms, A-SPX, A-CPL and
the rest of src/ac4dec/src/pcm in `float`, and `ICLFORGE_DECODE_SCALAR=fixed` in Q7.24 with
block exponents; the default builds them in `double`. This decodes each committed AC-4 stream
with the double CLI and the other tier's and holds the second decode to the first, in two
regions of each channel's spectrum:

  below   from 0 to the lowest crossover (sbx) of the frame's aspx_data elements, where every
          channel is waveform-coded and the two builds differ by rounding through the
          transforms and the QMF banks;
  above   from the highest crossover to 24 kHz, where A-SPX has recreated the band from the
          low band's QMF subbands, gains and noise, and the two builds part further.

Between the two, where a channel's crossover is not another's, the streams are left out of
both. A stream with no A-SPX has one region, the whole band, reported as `below`.

The SNR of a region is the signal's energy over the error's, summed over half-overlapped Hann
frames of 2 048 samples, in dB, and the figure of a stream is its worst channel's; a channel
with less than MIN_ENERGY_PER_FRAME in a region is not scored there (the LFE above a crossover).
The error's level in dBFS, the energy of a full-scale sine in a frame being 0 dB, is printed
beside them. What is gated is each figure against the pin of
tests/golden/ac4dec/scalar-agreement.json for the float tier, or of
tests/golden/ac4dec/scalar-agreement-fixed.json for the fixed-point one, which hold floors: the
figure measured with the double CLI as the reference, less a margin.

What it does not answer is whether either decode is RIGHT: the scorers do that, with a float
CLI in CI. Two builds agreeing says they agree.

numpy is needed to measure (the AC-4 scorers need it too) and not to read the pins, which
tools/checks/test_check_ac4_decode_scalar_snr.py does under the plain-stdlib oracle test run.

Usage:
    check_ac4_decode_scalar_snr.py --double-cli <path> (--float-cli <path> | --fixed-cli <path>)
                                   [--workdir <dir>] [--pins <json>] [--write-pins <json>]
                                   [--streams <path>...]

--write-pins records the figures of this run, less the margin, as the pins.
"""

from __future__ import annotations

import argparse
import importlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
PINS = REPO_ROOT / "tests" / "golden" / "ac4dec" / "scalar-agreement.json"
FIXED_PINS = REPO_ROOT / "tests" / "golden" / "ac4dec" / "scalar-agreement-fixed.json"
# The streams: DEE's, the constructed and object streams the decoder's tests decode, the
# presentation streams and the GUI's fixture. The decoder fuzz seeds are the first frames of
# streams already here.
STREAM_GLOBS = (
    "tests/golden/external-baseline/*/dee.ac4",
    "tests/golden/ac4dec/**/*.ac4",
    "apps/gui/tests/fixtures/*.ac4",
)
# Below this the error is at the level of the double decode's own rounding to float in the
# output WAV, and a figure is not a measurement of the float build.
SNR_CAP_DB = 200.0
# A region a channel has less than this in, on average per frame (the sum of squares of the Hann
# frame's transform there; a full-scale sine is about 4e5), is not scored: the LFE above a
# crossover, a silent channel. What either build holds there is rounding noise against rounding
# noise, which the ratio of the two is not a measurement of. The float build's own noise floor
# is 1e-11 a frame, and the weakest channel of the committed streams that this leaves scored
# holds 1e-5.
MIN_ENERGY_PER_FRAME = 1e-6
# What the pins are set under the figures measured, in dB. MSVC, GCC and Clang gave figures
# within 0.1 dB of one another, so this is not for the compilers; it is what a libm that rounds
# a table entry the other way could move a figure by, and this gate is here to catch a float path
# that has broken (a double that silently became a float, a lost precision step), which moves a
# figure by tens of dB.
DEFAULT_MARGIN_DB = 3.0
# score_ac4_decode.FRAME.
FRAME = 2048

_np = None
_scorer = None


def _imports():
    """numpy and score_ac4_decode, imported when a measurement first needs them."""
    global _np, _scorer
    if _scorer is None:
        numpy = importlib.import_module("numpy")
        sys.path.insert(0, str(REPO_ROOT / "tools" / "checks"))
        _np, _scorer = numpy, importlib.import_module("score_ac4_decode")
    return _np, _scorer


def committed_streams() -> list[Path]:
    found: list[Path] = []
    for pattern in STREAM_GLOBS:
        found.extend(sorted(REPO_ROOT.glob(pattern)))
    return found


def relative(path: Path) -> str:
    return path.resolve().relative_to(REPO_ROOT).as_posix()


def region_snr(ref, out, low_hz: float, high_hz: float) -> float | None:
    """SNR in dB of out against ref between low_hz and high_hz, over half-overlapped Hann frames;
    None where ref has less than MIN_ENERGY_PER_FRAME there."""
    np, scorer = _imports()
    window = np.hanning(FRAME)
    bin_hz = scorer.RATE / FRAME
    first = int(low_hz / bin_hz)
    last = int(high_hz / bin_hz)
    signal = error = 0.0
    frames = 0
    for start in range(0, len(ref) - FRAME, FRAME // 2):
        r = np.fft.rfft(window * ref[start : start + FRAME])[first:last]
        o = np.fft.rfft(window * out[start : start + FRAME])[first:last]
        signal += float(np.sum(np.abs(r) ** 2))
        error += float(np.sum(np.abs(o - r) ** 2))
        frames += 1
    if frames == 0 or signal / frames < MIN_ENERGY_PER_FRAME:
        return None
    if error <= 0.0:
        return SNR_CAP_DB
    return min(SNR_CAP_DB, 10.0 * np.log10(signal / error))


def error_dbfs(ref, out) -> float:
    """The error's level over the whole band, in dB of a full-scale sine's energy in a frame: the
    mean over frames of the sum of squares of the Hann frame's transform of out - ref, over that
    of a sine of amplitude 1, which is 3 N^2 / 32 through the window."""
    np, _ = _imports()
    window = np.hanning(FRAME)
    full_scale = 3.0 * FRAME * FRAME / 32.0
    error = 0.0
    frames = 0
    for start in range(0, len(ref) - FRAME, FRAME // 2):
        difference = out[start : start + FRAME] - ref[start : start + FRAME]
        error += float(np.sum(np.abs(np.fft.rfft(window * difference)) ** 2))
        frames += 1
    if frames == 0 or error <= 0.0:
        return -SNR_CAP_DB
    return 10.0 * np.log10(error / frames / full_scale)


def internal_rate(trace: Path) -> float:
    """The rate the QMF banks run at for the stream in `trace` (Part 1 Table 83), from its first
    frame_rate_index; the output rate where the trace has none."""
    _, scorer = _imports()
    for line in trace.read_text(encoding="utf-8").splitlines():
        fields = line.split("\t")
        if len(fields) == 6 and fields[5] == "frame_rate_index":
            index = int(fields[4])
            return scorer.internal_rate(index) if index in scorer.RESAMPLING else scorer.RATE
    return float(scorer.RATE)


def crossovers(trace: Path, rate: float) -> tuple[float, float] | None:
    """The lowest and highest crossover, in Hz, of the frame's aspx_data elements, or None for
    a stream with no A-SPX."""
    _, scorer = _imports()
    found = scorer.trace_values(trace)
    if found is None:
        return None
    values, offsets = found
    borders = [scorer.aspx_groups(values, offset)[0] for offset in offsets]
    hz = scorer.subband_hz(rate)
    return min(borders) * hz, max(borders) * hz


def decode(cli: Path, stream: Path, out: Path, trace: Path | None = None):
    command = [str(cli), "decode", str(stream), str(out)]
    if trace is not None:
        command.append(f"syntax-trace={trace}")
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    return result.returncode, result.stdout + result.stderr


def measure(double_cli: Path, other_cli: Path, stream: Path, work: Path, tier: str = "float"):
    """(below, above, error) for a stream: the worst-channel figures, None where a region does not
    exist, and the loudest channel's error in dBFS; the string 'refused' where both CLIs refuse
    the stream; raises where they differ on that."""
    np, scorer = _imports()
    name = relative(stream).replace("/", "__")
    ref_wav = work / f"{name}.d.wav"
    out_wav = work / f"{name}.{tier}.wav"
    trace = work / f"{name}.tsv"
    code_d, text_d = decode(double_cli, stream, ref_wav, trace)
    code_f, text_f = decode(other_cli, stream, out_wav)
    if code_d != 0 and code_f != 0:
        return "refused"
    if code_d != code_f:
        raise SystemExit(
            f"{relative(stream)}: the double CLI exits {code_d} and the {tier} CLI {code_f}:\n"
            f"{text_d}{text_f}"
        )
    # read_wav gives (frames by channels, rate), as the scorers use it.
    ref = np.asarray(scorer.read_wav(ref_wav)[0], dtype=np.float64)
    out = np.asarray(scorer.read_wav(out_wav)[0], dtype=np.float64)
    if ref.shape != out.shape:
        raise SystemExit(f"{relative(stream)}: the two decodes are {ref.shape} and {out.shape}")
    xover = crossovers(trace, internal_rate(trace))
    nyquist = scorer.RATE / 2.0
    below_hi = xover[0] if xover else nyquist
    channels = range(ref.shape[1])
    below = [region_snr(ref[:, c], out[:, c], 0.0, below_hi) for c in channels]
    above = [region_snr(ref[:, c], out[:, c], xover[1], nyquist) for c in channels] if xover else []
    scored = lambda values: min((v for v in values if v is not None), default=None)  # noqa: E731
    loudest_error = max(error_dbfs(ref[:, c], out[:, c]) for c in channels)
    return scored(below), scored(above), loudest_error


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawTextHelpFormatter
    )
    parser.add_argument("--double-cli", required=True, type=Path)
    other = parser.add_mutually_exclusive_group(required=True)
    other.add_argument("--float-cli", type=Path)
    other.add_argument("--fixed-cli", type=Path)
    parser.add_argument("--workdir", type=Path)
    parser.add_argument("--pins", type=Path)
    parser.add_argument("--write-pins", type=Path)
    parser.add_argument("--margin-db", type=float, default=DEFAULT_MARGIN_DB)
    parser.add_argument(
        "--streams", nargs="*", type=Path, help="these streams instead of the committed ones"
    )
    args = parser.parse_args()
    tier = "fixed" if args.fixed_cli is not None else "float"
    other_cli = args.fixed_cli if args.fixed_cli is not None else args.float_cli
    if args.pins is None:
        args.pins = FIXED_PINS if tier == "fixed" else PINS

    streams = [s.resolve() for s in args.streams] if args.streams else committed_streams()
    pins = None
    if args.write_pins is None:
        pins = json.loads(args.pins.read_text(encoding="utf-8"))["streams"]

    tmp = None
    if args.workdir is None:
        tmp = tempfile.TemporaryDirectory(prefix="ac4-scalar-snr-")
        work = Path(tmp.name)
    else:
        work = args.workdir
        work.mkdir(parents=True, exist_ok=True)

    figures: dict[str, dict[str, float | None]] = {}
    failures = 0
    print(f"{'stream':<70} {'below dB':>9} {'above dB':>9} {'error dBFS':>11}   pin below / above")
    for stream in streams:
        key = relative(stream)
        result = measure(args.double_cli, other_cli, stream, work, tier)
        if result == "refused":
            print(f"{key:<70} {'refused by both':>19}")
            continue
        below, above, level = result
        figures[key] = {"below": below, "above": above}
        below_text = f"{below:9.1f}" if below is not None else f"{'-':>9}"
        above_text = f"{above:9.1f}" if above is not None else f"{'-':>9}"
        line = f"{key:<70} {below_text} {above_text} {level:11.1f}"
        if pins is None:
            print(line, flush=True)
            continue
        pin = pins.get(key)
        if pin is None:
            print(f"{line}   no pin", flush=True)
            print(f"::error::{key} has no pin in {relative(args.pins)}", file=sys.stderr)
            failures += 1
            continue
        bad = any(
            pin.get(region) is not None and (value is None or value < pin[region])
            for region, value in (("below", below), ("above", above))
        )
        pin_text = " / ".join(
            "-" if pin.get(r) is None else f"{pin[r]:.1f}" for r in ("below", "above")
        )
        print(f"{line}   {pin_text}{'   FAIL' if bad else ''}", flush=True)
        failures += bad

    if args.write_pins is not None:
        document = {
            "_comment": (
                f"The AC-4 {tier} decode's agreement with the double decode, per committed stream "
                "(tools/checks/check_ac4_decode_scalar_snr.py): the floors, in dB, of the worst "
                "channel's SNR below the lowest A-SPX crossover and above the highest, over "
                "half-overlapped Hann frames of 2048 samples, a channel with less than 1e-6 in a "
                "region (per frame) left out of it. Each is the figure measured less "
                f"{args.margin_db:.1f} dB. Re-pin when a stream's decode changes on purpose."
            ),
            "margin_db": args.margin_db,
            "streams": {
                key: {
                    region: (None if value is None else round(value - args.margin_db, 1))
                    for region, value in entry.items()
                }
                for key, entry in sorted(figures.items())
            },
        }
        args.write_pins.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
        print(f"wrote {args.write_pins}")
    if tmp is not None:
        tmp.cleanup()
    if failures:
        print(f"{failures} stream(s) below their pin", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
