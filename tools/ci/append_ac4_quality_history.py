"""Append one CI run's AC-4 decode quality summary to the quality-history branch.

Reads the JSON written by tools/checks/score_ac4_decode.py --json-out and appends
one JSONL record per leg to <history-dir>/ac4-quality-<branch>.jsonl. Mirrors
append_quality_history.py's trailing-baseline soft/hard annotations for SNR (lower
is worse) and LSD (higher is worse).
"""

import argparse
import json
import os
import sys
from pathlib import Path

from append_quality_history import write_recent_window

REGRESSION_TRAILING_WINDOW = 10
SNR_DROP_DB = 0.5
SNR_HARD_DROP_DB = 3.0
LSD_RISE_DB = 0.5
LSD_HARD_RISE_DB = 2.0


def trailing_mean(history_path: Path, leg: str, field: str, window: int):
    if not history_path.exists():
        return None
    values = []
    for raw in history_path.read_text().splitlines():
        line = raw.strip()
        if not line:
            continue
        rec = json.loads(line)
        if rec.get("leg") == leg and field in rec and rec[field] is not None:
            values.append(float(rec[field]))
    if not values:
        return None
    tail = values[-window:]
    return sum(tail) / len(tail)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--json-in", type=Path, required=True)
    parser.add_argument("--history-dir", type=Path, required=True)
    parser.add_argument("--branch", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--commit-date", required=True)
    args = parser.parse_args()

    payload = json.loads(args.json_in.read_text())
    history_path = args.history_dir / f"ac4-quality-{args.branch}.jsonl"
    hard_regression = False

    with history_path.open("a") as out:
        for leg in payload.get("legs", []):
            name = leg["leg"]
            record = {
                "branch": args.branch,
                "commit": args.commit,
                "commit_date": args.commit_date,
                "leg": name,
                "min_snr_db": leg.get("min_snr_db"),
                "lsd_db": leg.get("lsd_db"),
                "mos_lqo": leg.get("mos_lqo"),
            }
            snr = record["min_snr_db"]
            lsd = record["lsd_db"]
            if snr is not None:
                mean_snr = trailing_mean(history_path, name, "min_snr_db", REGRESSION_TRAILING_WINDOW)
                if mean_snr is not None:
                    drop = mean_snr - snr
                    if drop >= SNR_HARD_DROP_DB:
                        print(f"::error title=AC-4 quality hard regression::{name} min SNR "
                              f"{snr:.2f} dB is {drop:.2f} dB below trailing mean {mean_snr:.2f} dB")
                        hard_regression = True
                    elif drop >= SNR_DROP_DB:
                        print(f"::warning title=AC-4 quality soft regression::{name} min SNR "
                              f"{snr:.2f} dB is {drop:.2f} dB below trailing mean {mean_snr:.2f} dB")
            if lsd is not None:
                mean_lsd = trailing_mean(history_path, name, "lsd_db", REGRESSION_TRAILING_WINDOW)
                if mean_lsd is not None:
                    rise = lsd - mean_lsd
                    if rise >= LSD_HARD_RISE_DB:
                        print(f"::error title=AC-4 quality hard regression::{name} LSD "
                              f"{lsd:.2f} dB is {rise:.2f} dB above trailing mean {mean_lsd:.2f} dB")
                        hard_regression = True
                    elif rise >= LSD_RISE_DB:
                        print(f"::warning title=AC-4 quality soft regression::{name} LSD "
                              f"{lsd:.2f} dB is {rise:.2f} dB above trailing mean {mean_lsd:.2f} dB")
            out.write(json.dumps(record) + "\n")
            print(f"Appended AC-4 quality for {name}")

    write_recent_window(history_path)

    if hard_regression:
        github_output = os.environ.get("GITHUB_OUTPUT")
        if github_output:
            with open(github_output, "a") as gh_out:
                gh_out.write("hard_regression=true\n")

    return 0


if __name__ == "__main__":
    sys.exit(main())
