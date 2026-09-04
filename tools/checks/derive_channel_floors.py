"""Derive per-channel SNR floors for verify_gold_reference.sh from measured history.

The floors in tools/checks/verify_gold_reference.sh are not judgement calls
typed in by hand - they are a function of what every CI leg has actually
measured, and this script is that function. Run it against the quality-history
branch's JSONL and it prints the vectors to paste back, plus what each one
changes, so a floor move is always reviewable as a diff against evidence
rather than an assertion.

    curl -sL https://raw.githubusercontent.com/iainchesworthlabs/ac3forge/\\
quality-history/main.jsonl -o main.jsonl
    python3 tools/checks/derive_channel_floors.py --history main.jsonl

Policy: floor = floor(min_observed - HEADROOM_DB), where min_observed is the
channel's lowest value across every leg and every commit in the file.

HEADROOM_DB covers commit-to-commit noise and NOTHING ELSE, because
min_observed has already absorbed everything else.

That is the correction VX11 produced. The first version of this script used
6.02 dB, on the reasoning that a floor tighter than the cross-platform split
would risk a new platform tripping it. But min_observed is a minimum across
EVERY leg, so for any channel where the split appears it is already the arm64
value - the low side. Subtracting the size of the split from a number that is
already the bottom of the split counts the same margin twice, and cost about
5 dB of sensitivity on every channel.

What is actually left to absorb is how much one leg's own number moves between
commits, and the recorded answer is: almost nothing. Across 520
(check, leg, channel) series the median spread over the full history is
0.000 dB and 495 of them stay under 0.5 dB. The 25 that do not are one real
step change on one check (eac3_cplbndstrce0, 2026-08-17, 25.42 -> 22.41, flat
for the 63 commits since), not noise. 1.0 dB is over ten times the largest
genuine commit-to-commit movement this project has documented (0.02-0.08 dB).

A NEW leg landing below a floor is not a false alarm under this policy - it is
a new platform behaviour that has never been reviewed, and stopping to look at
it is the correct outcome rather than something to pre-authorise with margin.

Regenerate after a deliberate, reviewed quality change. Never to make a red
gate green: a floor that has to move to pass is a regression with extra steps,
and the whole point of deriving it from history is that the derivation is
visible when someone tries.

stdlib-only, matching compare_wav.py's own no-new-CI-provisioning reasoning.
"""

import argparse
import collections
import json
import math
import sys
from pathlib import Path

HEADROOM_DB = 1.0

# Same list as compare_wav.py's CHANNEL_LABELS and docs/quality-trend.md's
# CHANNEL_LABELS_51 - the golden reference's WAV channel order.
CHANNEL_LABELS = {2: ["L", "R"], 6: ["L", "R", "C", "LFE", "Ls", "Rs"]}


def labels(count: int) -> list[str]:
    return CHANNEL_LABELS.get(count, [f"ch{i}" for i in range(count)])


def load(history: Path) -> dict[str, list[dict]]:
    """Group records by check, folding each TRANSFORM_MODE=reference run in
    with its base check: the two share a fixture and a comparison, differing
    only in which transform evaluation produced the audio, so they share a
    floor too."""
    by_check = collections.defaultdict(list)
    for raw in history.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line:
            continue
        try:
            record = json.loads(line)
        except json.JSONDecodeError:
            continue
        check = record.get("check") or record.get("codec")
        if not check or "channels_db" not in record:
            continue
        by_check[check.removesuffix("_reference")].append(record)
    return by_check


def derive(records: list[dict]) -> tuple[list[float], list[int], int]:
    """(observed minima, derived floors, channel count) for one check.

    Records whose channel count differs from the newest one are dropped rather
    than reconciled - a layout change means the older rows are measuring a
    different thing, and averaging across them would produce a floor that
    describes neither."""
    channels = len(records[-1]["channels_db"])
    usable = [r for r in records if len(r["channels_db"]) == channels]
    minima = [min(r["channels_db"][i] for r in usable) for i in range(channels)]
    return minima, [math.floor(m - HEADROOM_DB) for m in minima], channels


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--history", type=Path, required=True,
                         help="quality-history JSONL (main.jsonl or main.recent.jsonl).")
    parser.add_argument("--check", default=None,
                         help="Only this check, instead of every check in the file.")
    args = parser.parse_args()

    by_check = load(args.history)
    if not by_check:
        print(f"no usable records in {args.history}", file=sys.stderr)
        return 1

    for check in sorted(by_check):
        if args.check and check != args.check:
            continue
        records = by_check[check]
        minima, floors, channels = derive(records)
        names = labels(channels)
        current = records[-1].get("threshold_db")
        legs = len({r["leg"] for r in records if "leg" in r})
        commits = len({r["commit"] for r in records if "commit" in r})

        print(f"{check}")
        print(f"    evidence     : {len(records)} rows, {commits} commit(s), {legs} leg(s)")
        print(f"    channels     : {' '.join(f'{n:>7s}' for n in names)}")
        print(f"    min observed : {' '.join(f'{m:7.2f}' for m in minima)}")
        print(f"    derived floor: {' '.join(f'{f:7d}' for f in floors)}")
        if current is not None:
            deltas = [f - current for f in floors]
            print(f"    vs {current:g} dB scalar: "
                  f"{' '.join(f'{d:+7.0f}' for d in deltas)}")
        print(f"    vector       : {','.join(str(f) for f in floors)}")
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
