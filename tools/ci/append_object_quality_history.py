"""Append one CI run's object-reconstruction scores to the quality-history
branch's per-branch JSONL file, and flag a trailing-baseline regression at
two tiers: a soft one (::warning::, never fails this script) and a hard one
(::error::, signalled via $GITHUB_OUTPUT's hard_regression so the caller can
fail the job *after* still committing and pushing the data - the same
"commit first, fail the job after" ordering as append_quality_history.py,
append_performance_history.py and append_external_comparison_history.py, so
a regression is never silently un-recorded just because it also failed the
run).

Reads tools/ci/quality_race.py's `objects` mode JSON output - one row per
(leg, object) plus a `scene` row per leg, every number produced by encoding
the committed tests/golden/audio/reference_objects.wav scene with this build
and decoding it back with this build - and appends one JSONL record per row
to <history-dir>/object-quality-<branch>.jsonl.

There is no external-baseline half here, and there cannot be one. The three
codec legs append_external_comparison_history.py handles carry vs_ffmpeg/
vs_dee deltas against tests/golden/external-baseline/manifest.json; object
decode has no external oracle at all - FFmpeg implements no JOC
reconstruction, and Dolby's own decoder gates object decoding on a keyed
authenticity tag this project ships no key for, so it renders these streams
as their 5.1 bed and never produces objects to compare against. This series
is self-consistency only, which is why it takes no --manifest-json argument
and writes no vs_* keys. docs/object-quality-trend.md says so on the page.

Thresholds are this metric's own, not a copy of the codec legs'. The
existing series score 21-40 dB and use 0.5 dB soft / 10 dB hard; the object
rows sit at 9.6-22.7 dB across the two rates, so a 10 dB hard threshold
would be most of a healthy row's whole headroom. 5 dB is the hard tier here
- still far more than any legitimate encoder change moves a row, and still
well inside what a real JOC regression costs: the object-reconstruction
defect class this exists to catch collapses a row toward 0 dB, not by a
fraction of one. The soft tier stays 0.5 dB, which is a trend annotation
rather than a failure either way.

`leak_db` can legitimately be null on a row - an object occupying every Bark
band has nowhere outside for anything to leak into, so there is no
measurement rather than a floor value (see object_spectral_scores). It is
carried through as-is, read-only, with no regression handling, exactly as
lsd_db and mos_lqo are on the external-comparison series.

stdlib-only (json/argparse/pathlib), matching every other append-*.py
script's no-new-CI-provisioning reasoning. Takes commit and commit-date as
arguments rather than reading the clock itself - see this project's general
rule against Date.now()-style nondeterminism in tooling; the caller sources
commit-date from `git show -s --format=%cI`.
"""

import argparse
import json
import os
import sys
from pathlib import Path

REGRESSION_TRAILING_WINDOW = 10
REGRESSION_DROP_DB = 0.5
HARD_REGRESSION_DROP_DB = 5.0


def load_object_rows(objects_json: Path):
    payload = json.loads(objects_json.read_text())
    return payload["rows"]


def trailing_mean(history_path: Path, leg: str, variant: str, window: int):
    if not history_path.exists():
        return None
    matches = []
    for line in history_path.read_text().splitlines():
        if not line.strip():
            continue
        rec = json.loads(line)
        if rec.get("leg") == leg and rec.get("variant") == variant:
            matches.append(rec["snr_db"])
    if not matches:
        return None
    tail = matches[-window:]
    return sum(tail) / len(tail)


def emit_github_output(name: str, value: str) -> None:
    """No-op outside GitHub Actions (GITHUB_OUTPUT unset) - safe to call from
    a plain local run of this script."""
    path = os.environ.get("GITHUB_OUTPUT")
    if not path:
        return
    with open(path, "a") as f:
        f.write(f"{name}={value}\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--objects-json", type=Path, required=True)
    parser.add_argument("--history-dir", type=Path, required=True)
    parser.add_argument("--branch", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--commit-date", required=True,
                        help="Committer date, ISO 8601 (from `git show -s --format=%%cI`).")
    args = parser.parse_args()

    history_path = args.history_dir / f"object-quality-{args.branch}.jsonl"
    rows = load_object_rows(args.objects_json)
    if not rows:
        print(f"::warning::no rows found in {args.objects_json}; nothing to append")
        return 0

    lines = []
    hard_regression = False
    for row in rows:
        leg, variant = row["leg"], row["variant"]
        baseline = trailing_mean(history_path, leg, variant, REGRESSION_TRAILING_WINDOW)
        lines.append(json.dumps({
            "commit": args.commit,
            "branch": args.branch,
            "commit_date": args.commit_date,
            "leg": leg,
            "bitrate_kbps": row["bitrate_kbps"],
            "variant": variant,
            "snr_db": row["snr_db"],
            "lsd_db": row["lsd_db"],
            "leak_db": row.get("leak_db"),
            "mos_lqo": row.get("mos_lqo"),
        }, sort_keys=True))

        drop = None if baseline is None else baseline - row["snr_db"]
        if drop is not None and drop >= HARD_REGRESSION_DROP_DB:
            hard_regression = True
            print(f"::error title=Object-quality trend hard regression::{leg}/{variant}: "
                  f"SNR {row['snr_db']:.2f} dB is {drop:.2f} dB below the trailing "
                  f"{REGRESSION_TRAILING_WINDOW}-run mean ({baseline:.2f} dB) on {args.branch} - "
                  f"more than the {HARD_REGRESSION_DROP_DB:.0f} dB hard-regression threshold. "
                  "Still recorded below; the run this came from is failed separately so this "
                  "doesn't go unnoticed.")
        elif drop is not None and drop >= REGRESSION_DROP_DB:
            print(f"::warning title=Object-quality trend regression::{leg}/{variant}: "
                  f"SNR {row['snr_db']:.2f} dB is {drop:.2f} dB below the trailing "
                  f"{REGRESSION_TRAILING_WINDOW}-run mean ({baseline:.2f} dB) on {args.branch}. "
                  "This is a trend warning, not a failure.")

    args.history_dir.mkdir(parents=True, exist_ok=True)
    with history_path.open("a") as f:
        for line in lines:
            f.write(line + "\n")

    print(f"Appended {len(lines)} record(s) to {history_path}")
    emit_github_output("hard_regression", "true" if hard_regression else "false")
    return 0


if __name__ == "__main__":
    sys.exit(main())
