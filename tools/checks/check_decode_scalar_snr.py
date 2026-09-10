"""Gate a second decode scalar's path against the double one (roadmap PF7).

Written for the float32 path and used for the fixed-point one too
(planning/arithmetic-tiers.md): --float-cli names whichever build is under
test, and --min-snr-db the floor that build is held to.

`AC3FORGE_DECODE_SCALAR=float` builds the decoder's coefficient stores,
transform scratch and overlap-add history as `float` instead of `double` - the
arithmetic the minimum-footprint profile has used since the ESP32-S3 port, and
the arithmetic every bare-metal fixture is decoded with. Nothing measured it.

docs/building.md records "~139 dB worst-channel SNR against the double decode
across four streams", and that number was true when it was taken. It came from
a build made by hand: until the scalar was split onto its own CMake axis
(src/forge/src/internal/scalar/, AC3FORGE_DECODE_SCALAR) it could only be float in a
configuration that was ALSO decode-only and had no `ac3cli` to compare with. So
the figure has sat in prose for months with nothing able to re-derive it, while
the path it describes is the one two CI legs decode every fixture through.

This runs both builds over the same streams and holds the worst channel to a
floor. Delay-compensated per-channel SNR, via tools/checks/compare_wav.py -
the same comparison verify_gold_reference.sh already uses, pointed at two
decodes of one bitstream rather than at a decode and its source.

What it does NOT answer: whether either decode is RIGHT. Two builds agreeing
says they agree. verify_gold_reference.sh is what checks a decode against the
programme it came from, and running that with a float32 CLI is the other half
of this - see docs/building.md's Gaps.

Usage:
    check_decode_scalar_snr.py --double-cli <path> --float-cli <path>
                               --workdir <dir> [--min-snr-db N]

The workdir must already hold the streams verify_gold_reference.sh's encode
step writes (gold.ac3, gold.ec3, gold_cpl.ec3); this does not encode, so the
two CLIs are never asked to agree about anything but decoding.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
COMPARE = REPO_ROOT / "tools" / "checks" / "compare_wav.py"

# The three streams the cross-platform hash gate pins, and for the same
# reason: they are this project's own encoder's output over the gold-reference
# WAV, so they exercise AC-3, plain E-AC-3 and E-AC-3 with coupling without
# anyone having to keep a second corpus agreeing with the first. Plus one the
# hash gate does not pin: enhanced coupling, whose decode is the most involved
# thing either non-double scalar does - three inverse transforms, a 512-point
# DFT and a per-bin complex reconstruction per coupled channel per block - and
# which neither scalar's check measured until 2026-09-10.
STREAMS = ("gold.ac3", "gold.ec3", "gold_cpl.ec3", "gold_ecpl.ec3")

# 120 dB against a measured 138.85. The margin is wide on purpose: this gate
# exists to catch a float32 path that has BROKEN - a lost precision step, a
# double that silently became a float somewhere it mattered - not to police the
# last decibel of a number that is already ~19 dB below the double decode's own
# quantisation noise. A drop to 120 would be a real regression; a drop to 138
# would be arithmetic noise between compilers.
DEFAULT_MIN_SNR_DB = 120.0


def decode(cli: Path, stream: Path, out: Path) -> None:
    result = subprocess.run([str(cli), "decode", str(stream), str(out)],
                            capture_output=True, text=True, check=False)
    if result.returncode != 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise SystemExit(f"decode failed ({result.returncode}): {cli} {stream}")


def worst_channel_snr(reference: Path, actual: Path, floor: float) -> tuple[float, str]:
    """Run compare_wav.py and return its worst channel's SNR and name."""
    result = subprocess.run(
        [sys.executable, str(COMPARE), str(reference), str(actual),
         "--min-snr-db", str(floor)],
        capture_output=True, text=True, check=False)
    # "worst channel: Ls 138.85 dB" - parsed rather than recomputed so this gate
    # and verify_gold_reference.sh are quoting one implementation of SNR, not
    # two that have to be kept agreeing.
    match = re.search(r"worst channel:\s+(\S+)\s+([-\d.]+|inf)\s*dB", result.stdout)
    if not match:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise SystemExit("compare_wav.py printed no 'worst channel' line")
    name = match.group(1)
    raw = match.group(2)
    return (float("inf") if raw == "inf" else float(raw)), name


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--double-cli", required=True, type=Path)
    parser.add_argument("--float-cli", required=True, type=Path)
    parser.add_argument("--workdir", required=True, type=Path)
    parser.add_argument("--min-snr-db", type=float, default=DEFAULT_MIN_SNR_DB)
    args = parser.parse_args()

    for cli in (args.double_cli, args.float_cli):
        if not cli.exists():
            raise SystemExit(f"no such file: {cli}")

    failed = False
    for name in STREAMS:
        stream = args.workdir / name
        if not stream.exists():
            print(f"::error::{stream} missing - run verify_gold_reference.sh first",
                  file=sys.stderr)
            return 1
        reference = args.workdir / f"scalar_double_{name}.wav"
        actual = args.workdir / f"scalar_float_{name}.wav"
        decode(args.double_cli, stream, reference)
        decode(args.float_cli, stream, actual)
        snr, channel = worst_channel_snr(reference, actual, args.min_snr_db)
        if snr >= args.min_snr_db:
            print(f"[ok]       {name}: worst channel {channel} at {snr:.2f} dB "
                  f"(floor {args.min_snr_db:.0f})")
        else:
            print(f"::error::[FAIL] {name}: worst channel {channel} at {snr:.2f} dB, "
                  f"floor is {args.min_snr_db:.0f} dB - the decode under test has diverged "
                  f"from the double one by more than its precision accounts for",
                  file=sys.stderr)
            failed = True

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
