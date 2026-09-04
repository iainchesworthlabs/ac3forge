"""Render the newest measurements as shields.io endpoint JSON.

The README carries a row of badges that all answer "did the machinery run" -
CI, CodeQL, OSV, Scorecard. None of them answers "and what did it measure",
which for a codec is the more interesting question and is already being
recorded on every merge. These files close that gap: shields.io fetches them
server-side from the quality-history branch, so a reader sees the current
speed, accuracy and memory figures without opening anything.

WRITTEN BY THE JOB THAT ALREADY COMMITS HERE. quality-history has two writers
already (_build.yml's quality-trend job and ci.yml's
persist-performance-trend), and a third would be a third chance to race on the
same branch. This runs inside the second of those, after its appends and
before its commit, and reads every series from that job's own checkout - so
the speed and memory figures are the ones it just wrote, and the accuracy
figure is whatever the quality job last pushed. That can be one run behind on
a commit whose quality leg finished after this one. A badge is a summary and
that is an acceptable staleness; the trend pages are where an exact number
lives, and they are one click away.

EVERY THRESHOLD IS BORROWED, never invented here - the same rule
docs/performance-quality.md's status cards follow, and for the same reason: a
badge that shows amber on a healthy build is worse than no badge. Real time is
1x by definition, accuracy compares each channel against its own floor from
that record's thresholds_db, and memory uses the 4 KiB steady-state retention
line append_memory_history.py already warns at.

stdlib-only (json/argparse/pathlib), matching every other script here.
"""

import argparse
import json
from pathlib import Path

# shields.io's endpoint schema. Only these four fields are used; the badge
# picks up its own styling from the URL the README builds.
SCHEMA_VERSION = 1

GREEN = "brightgreen"
AMBER = "orange"
GREY = "lightgrey"

# The steady-state retention line append_memory_history.py warns at. Not a
# number chosen for this script.
MEMORY_RETENTION_WARN_BYTES = 4 * 1024


def newest_commit_rows(path: Path):
    """Every record belonging to the last commit in an append-only history.

    The last SHA to appear, not the newest commit_date: these files are
    appended to in merge order, and two records written by the same run share
    a timestamp to the second, so a date sort has ties that this does not.
    """
    if not path.exists():
        return []
    records = []
    for raw in path.read_text().splitlines():
        line = raw.strip()
        if not line:
            continue
        try:
            records.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    if not records:
        return []
    newest = records[-1].get("commit")
    return [r for r in records if r.get("commit") == newest]


def badge(label, message, color):
    return {"schemaVersion": SCHEMA_VERSION, "label": label,
            "message": message, "color": color}


def speed_badge(rows):
    """Slowest workload as a multiple of real time.

    The slowest, not the mean: "is it fast enough" is a worst-case question,
    and an average over ten workloads would hide one of them dropping below
    real time behind nine that did not.
    """
    timed = [r for r in rows
             if isinstance(r.get("ms_per_frame"), (int, float)) and r["ms_per_frame"] > 0]
    if not timed:
        return badge("encode speed", "no data", GREY)
    worst = max(timed, key=lambda r: r["ms_per_frame"])
    budget = worst.get("real_time_budget_ms_per_frame") or 32
    times = budget / worst["ms_per_frame"]
    return badge("encode speed", f"{times:.0f}x real time",
                 GREEN if times >= 1 else AMBER)


def _headroom_db(rec):
    """How much room a record's tightest channel has over its own floor.

    tightest_headroom_db arrived with per-channel floors; records written
    before that carry only worst_db and a scalar threshold_db, and fall back to
    exactly the old computation rather than being dropped from the comparison.
    """
    headroom = rec.get("tightest_headroom_db")
    if isinstance(headroom, (int, float)):
        return headroom
    return rec["worst_db"] - rec["threshold_db"]


def accuracy_badge(rows):
    """The tightest per-channel SNR margin, reported as that channel's own SNR.

    Mirrors qualityCard() in docs/performance-quality.md, which is the page
    this badge links to: a reader who clicks through must not land on a
    different number than the one they clicked. Both pick by MARGIN and report
    the channel that owns it.

    Each check is gated per CHANNEL, so the number that matters is the smallest
    per-channel margin, which is NOT the same thing as the lowest SNR. A 58 dB
    centre channel 6 dB above a 52 dB floor is closer to failing than a 22 dB
    surround 6 dB above a 16 dB floor - and the surround is the one this badge
    used to report, every single time, because it reported the lowest number
    rather than the tightest one.
    """
    scored = [r for r in rows
              if isinstance(r.get("worst_db"), (int, float))
              and isinstance(r.get("threshold_db"), (int, float))]
    if not scored:
        return badge("decode accuracy", "no data", GREY)

    tight = min(scored, key=_headroom_db)
    headroom = _headroom_db(tight)

    # The tightest channel's own SNR when per-channel floors are recorded, so
    # the badge shows what the linked card shows. Unlike JavaScript, an
    # out-of-range index raises here and a negative one silently wraps, so the
    # recorded index is bounds-checked rather than trusted.
    thresholds = tight.get("thresholds_db")
    channels = tight.get("channels_db")
    value = tight["worst_db"]
    if isinstance(thresholds, list) and isinstance(channels, list) and channels:
        i = tight.get("tightest_channel")
        if not isinstance(i, int) or not 0 <= i < len(channels):
            i = 0
        value = channels[i]

    # Coloured on the margin, not on worst_db >= threshold_db: the scalar test
    # cannot see a per-channel breach. Against floors [42, 47, 49, 81, 17, 17],
    # a centre channel falling to 48 dB is below its own 49 dB floor while the
    # 18 dB surround still clears the scalar 17 - green on a build the gate
    # itself fails.
    return badge("decode accuracy", f"{value:.1f} dB SNR",
                 GREEN if headroom >= 0 else AMBER)


def memory_badge(rows):
    """Heaviest workload's churn, coloured by steady-state retention.

    Two different workloads: the number shown is the heaviest per-frame churn,
    the colour comes from the largest retention. They are reported together
    because churn alone says nothing about whether anything is being held, and
    retention alone says nothing about how hard the allocator is working.
    """
    scored = [r for r in rows if isinstance(r.get("bytes_per_frame"), (int, float))]
    if not scored:
        return badge("memory", "no data", GREY)
    worst = max(scored, key=lambda r: r["bytes_per_frame"])
    held = max((r.get("steady_live_growth") or 0) for r in scored)
    return badge("memory", f"{worst['bytes_per_frame'] / 1024:.0f} KB/frame",
                 GREEN if held <= MEMORY_RETENTION_WARN_BYTES else AMBER)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--history-dir", type=Path, required=True,
                        help="The quality-history checkout the trend files live in.")
    parser.add_argument("--branch", default="main",
                        help="Which branch's series to summarise (default: main).")
    args = parser.parse_args()

    out_dir = args.history_dir / "badges"
    out_dir.mkdir(parents=True, exist_ok=True)

    b = args.branch
    written = {
        "speed": speed_badge(newest_commit_rows(args.history_dir / f"performance-{b}.jsonl")),
        "accuracy": accuracy_badge(newest_commit_rows(args.history_dir / f"{b}.jsonl")),
        "memory": memory_badge(newest_commit_rows(args.history_dir / f"memory-{b}.jsonl")),
    }
    for name, payload in written.items():
        path = out_dir / f"{name}.json"
        path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
        print(f"{path}: {payload['label']} = {payload['message']} ({payload['color']})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
