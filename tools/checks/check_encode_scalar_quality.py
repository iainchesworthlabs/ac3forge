#!/usr/bin/env python3
"""Hold the float encode path against the double one, decoded by one decoder.

`AC3FORGE_ENCODE_SCALAR=float` builds the encoders' analysis front end - and,
as the conversion proceeds, more of them - in float: the arithmetic the
minimum-footprint profile runs on an ESP32-S3. Unlike the decoder's float path,
which reproduces the double decode to ~139 dB, a float ENCODER makes different
decisions at thresholds and rounds its coefficients differently, so it produces
a different, equally valid bitstream. The question is not whether the two
agree; it is whether what the float encoder's stream decodes to is as close to
the programme as what the double encoder's does.

So this encodes the gold-reference WAVs with both CLIs, decodes every stream
with the DOUBLE CLI (one decoder, so the encoders are the only variable),
compares each decode to its source with tools/checks/compare_wav.py - the same
delay-compensated per-channel SNR verify_gold_reference.sh uses - and holds the
float encoder's worst channel to within --max-drop-db of the double encoder's.
It also reports how far the two decodes are from each other, which is the size
of the change, and, when ffmpeg is on PATH or given, decodes the float stream
with it as the independent decoder the gold-reference gate uses.

What it does NOT answer: perceptual quality. ViSQOL is the quality trend's
measure and runs in CI; a channel SNR is what a laptop can hold in a minute.

Usage:
    check_encode_scalar_quality.py --double-cli <path> --float-cli <path>
                                   --workdir <dir> [--max-drop-db 0.5] [--ffmpeg <path>]
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
COMPARE = REPO_ROOT / "tools" / "checks" / "compare_wav.py"
AUDIO = REPO_ROOT / "tests" / "golden" / "audio"


@dataclass(frozen=True)
class Stream:
    label: str
    source: str          # under tests/golden/audio
    command: str         # ac3cli subcommand
    args: tuple[str, ...]  # after <in> <out>
    suffix: str


# Dither off on every row so the comparison is between two encoders' decisions
# and arithmetic, not between two dither sequences. The 5.1 rows are the
# gold-reference gate's own three; the 2/0 rows are the shapes a small part is
# most likely to encode, and the first the float front end took to real time.
STREAMS = (
    Stream("ac3 5.1 448", "reference_51.wav", "encode", ("448", "51", "dither=off"), "ac3"),
    Stream("eac3 5.1 256", "reference_51.wav", "eac3-encode", ("256", "nodither", "51"), "ec3"),
    Stream(
        "eac3 5.1 256 cpl", "reference_51.wav", "eac3-encode", ("256", "cpl+nodither", "51"), "ec3"
    ),
    Stream("ac3 2/0 192", "reference_stereo.wav", "encode", ("192", "stereo", "dither=off"), "ac3"),
    Stream(
        "eac3 2/0 192", "reference_stereo.wav", "eac3-encode", ("192", "nodither", "stereo"), "ec3"
    ),
)

DEFAULT_MAX_DROP_DB = 0.5


def run(argv: list[str], what: str) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(argv, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise SystemExit(f"{what} failed ({result.returncode}): {' '.join(argv)}")
    return result


def channel_snrs(reference: Path, actual: Path) -> tuple[list[float], float, str]:
    """Every channel's SNR, the worst, and the worst's name, from compare_wav.py."""
    result = subprocess.run(
        [sys.executable, str(COMPARE), str(reference), str(actual), "--min-snr-db", "0"],
        capture_output=True, text=True, check=False)
    # "inf" is what compare_wav.py prints for two identical files - which two
    # encoders' streams can be, when float rounding moved no decision.
    per_channel = [float(m.group(1)) for m in
                   re.finditer(r"^channel \d+ \([^)]*\): ([-\d.]+|inf) dB", result.stdout, re.M)]
    match = re.search(r"worst channel:\s+(\S+)\s+([-\d.]+|inf)\s*dB", result.stdout)
    if not match or not per_channel:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise SystemExit("compare_wav.py printed no per-channel lines")
    raw = match.group(2)
    worst = float("inf") if raw == "inf" else float(raw)
    return per_channel, worst, match.group(1)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--double-cli", required=True, type=Path)
    parser.add_argument("--float-cli", required=True, type=Path)
    parser.add_argument("--workdir", required=True, type=Path)
    parser.add_argument("--max-drop-db", type=float, default=DEFAULT_MAX_DROP_DB,
                        help="how far below the double encoder's worst channel the float "
                             "encoder's may sit (dB)")
    parser.add_argument("--ffmpeg", type=Path, default=None,
                        help="ffmpeg for an independent decode of the float streams "
                             "(default: the one on PATH, if any)")
    args = parser.parse_args()

    for cli in (args.double_cli, args.float_cli):
        if not cli.exists():
            raise SystemExit(f"no such file: {cli}")
    ffmpeg = args.ffmpeg or (Path(shutil.which("ffmpeg")) if shutil.which("ffmpeg") else None)
    args.workdir.mkdir(parents=True, exist_ok=True)

    print(f"{'stream':18} {'double->src':>12} {'float->src':>11} {'drop':>7} "
          f"{'float vs double':>16} {'ffmpeg(float)->src':>19}")
    failed = False
    for s in STREAMS:
        source = AUDIO / s.source
        if not source.exists():
            raise SystemExit(f"missing fixture source: {source}")
        tag = s.label.replace(" ", "_").replace("/", "")
        double_stream = args.workdir / f"double_{tag}.{s.suffix}"
        float_stream = args.workdir / f"float_{tag}.{s.suffix}"
        run([str(args.double_cli), s.command, str(source), str(double_stream), *s.args],
            f"double encode of {s.label}")
        run([str(args.float_cli), s.command, str(source), str(float_stream), *s.args],
            f"float encode of {s.label}")

        double_wav = args.workdir / f"double_{tag}.wav"
        float_wav = args.workdir / f"float_{tag}.wav"
        run([str(args.double_cli), "decode", str(double_stream), str(double_wav)],
            f"decode of the double {s.label}")
        run([str(args.double_cli), "decode", str(float_stream), str(float_wav)],
            f"decode of the float {s.label}")

        d_channels, d_worst, d_name = channel_snrs(source, double_wav)
        f_channels, f_worst, f_name = channel_snrs(source, float_wav)
        _, between, _ = channel_snrs(double_wav, float_wav)
        drop = d_worst - f_worst

        ffmpeg_col = "-"
        if ffmpeg is not None:
            ffmpeg_wav = args.workdir / f"ffmpeg_float_{tag}.wav"
            run([str(ffmpeg), "-y", "-loglevel", "error", "-i", str(float_stream),
                 "-f", "wav", "-c:a", "pcm_f32le", str(ffmpeg_wav)],
                f"ffmpeg decode of the float {s.label}")
            _, ff_worst, _ = channel_snrs(source, ffmpeg_wav)
            ffmpeg_col = f"{ff_worst:.2f} dB"

        verdict = "ok" if drop <= args.max_drop_db else "FAIL"
        if verdict == "FAIL":
            failed = True
        d_mean = sum(d_channels) / len(d_channels)
        f_mean = sum(f_channels) / len(f_channels)
        print(f"{s.label:18} {d_worst:9.2f} dB {f_worst:8.2f} dB {drop:+6.2f} "
              f"{between:13.2f} dB {ffmpeg_col:>19}  [{verdict}] "
              f"(worst {d_name}/{f_name}; means {d_mean:.2f}/{f_mean:.2f})")

    if failed:
        print(f"::error::the float encoder's worst channel sits more than "
              f"{args.max_drop_db:.2f} dB below the double encoder's on at least one stream",
              file=sys.stderr)
        return 1
    print(f"float encode path: within {args.max_drop_db:.2f} dB of the double one on every stream")
    return 0


if __name__ == "__main__":
    sys.exit(main())
