"""Append one CI run's AC-4 decode quality to the quality-history branch.

Reads the JSON tools/checks/score_ac4_decode.py --json-out writes (one entry per
DEE stream it scores: the minimum per-channel SNR, the LSD and the MOS-LQO) and
appends one JSONL record per stream to <history-dir>/ac4-quality-<branch>.jsonl,
which docs/quality-trend.md's AC-4 section reads.

The score script's pinned floors already fail CI; this adds the trailing-baseline
tiers append_quality_history.py applies to the AC-3 gate. A soft drift warns.
A hard one sets $GITHUB_OUTPUT's ac4_hard_regression, and _build.yml fails the
job on it after the data is pushed, so a large regression is still recorded.
Its own output name rather than hard_regression, which append_quality_history.py
writes in the same step.

stdlib-only, like every tools/ci/append_*.py script.
"""

import argparse
import json
import sys
from pathlib import Path

from append_quality_history import emit_github_output, write_recent_window

REGRESSION_TRAILING_WINDOW = 10
# SNR falls and LSD rises when quality drops. The soft tier matches the AC-3
# gate's 0.5 dB; the hard tiers are lower than its 10 dB because these are
# scores against the source, not decoder-to-decoder agreement, and the score
# script's own pins sit within a few dB of the measured values.
SNR_DROP_DB = 0.5
SNR_HARD_DROP_DB = 3.0
LSD_RISE_DB = 0.5
LSD_HARD_RISE_DB = 2.0


def trailing_mean(history: list[dict], leg: str, field: str, window: int):
    values = [rec[field] for rec in history
              if rec.get("leg") == leg and isinstance(rec.get(field), (int, float))]
    if not values:
        return None
    tail = values[-window:]
    return sum(tail) / len(tail)


def verdict(value, baseline, soft: float, hard: float, rising_is_worse: bool):
    """'hard', 'soft' or None for one metric against its trailing mean."""
    if value is None or baseline is None:
        return None
    worse_by = value - baseline if rising_is_worse else baseline - value
    if worse_by >= hard:
        return "hard"
    if worse_by >= soft:
        return "soft"
    return None


def read_history(path: Path) -> list[dict]:
    if not path.exists():
        return []
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--json-in", type=Path, required=True)
    parser.add_argument("--history-dir", type=Path, required=True)
    parser.add_argument("--branch", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--commit-date", required=True)
    args = parser.parse_args(argv)

    legs = json.loads(args.json_in.read_text()).get("legs", [])
    if not legs:
        print(f"::warning title=AC-4 quality trend::{args.json_in} scored no streams; "
              "nothing appended")
        return 0

    history_path = args.history_dir / f"ac4-quality-{args.branch}.jsonl"
    history = read_history(history_path)
    hard_regression = False
    lines = []
    for leg in legs:
        rec = {
            "branch": args.branch,
            "commit": args.commit,
            "commit_date": args.commit_date,
            "leg": leg["leg"],
            "min_snr_db": leg.get("min_snr_db"),
            "lsd_db": leg.get("lsd_db"),
            "mos_lqo": leg.get("mos_lqo"),
        }
        checks = (
            ("min SNR", "min_snr_db", SNR_DROP_DB, SNR_HARD_DROP_DB, False, "below"),
            ("LSD", "lsd_db", LSD_RISE_DB, LSD_HARD_RISE_DB, True, "above"),
        )
        for label, field, soft, hard, rising_is_worse, direction in checks:
            baseline = trailing_mean(history, rec["leg"], field, REGRESSION_TRAILING_WINDOW)
            tier = verdict(rec[field], baseline, soft, hard, rising_is_worse)
            if tier is None:
                continue
            message = (f"{rec['leg']} {label} {rec[field]:.2f} dB is "
                       f"{abs(rec[field] - baseline):.2f} dB {direction} its trailing "
                       f"{REGRESSION_TRAILING_WINDOW}-run mean of {baseline:.2f} dB")
            if tier == "hard":
                hard_regression = True
                print(f"::error title=AC-4 quality hard regression::{message}. Still recorded; "
                      "the run is failed separately.")
            else:
                print(f"::warning title=AC-4 quality soft regression::{message}")
        lines.append(json.dumps(rec))

    args.history_dir.mkdir(parents=True, exist_ok=True)
    with history_path.open("a") as out:
        for line in lines:
            out.write(line + "\n")
    print(f"Appended {len(lines)} record(s) to {history_path}")
    write_recent_window(history_path)
    emit_github_output("ac4_hard_regression", "true" if hard_regression else "false")
    return 0


if __name__ == "__main__":
    sys.exit(main())
