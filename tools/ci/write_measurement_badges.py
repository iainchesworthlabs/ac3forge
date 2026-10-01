"""Render the newest measurements as shields.io endpoint JSON.

The README carries a row of badges that all answer "did the machinery run" -
CI, CodeQL, OSV, Scorecard. None of them answers "and what did it measure",
which for a codec is the more interesting question and is already being
recorded on every merge. These files close that gap: shields.io fetches them
server-side from the quality-history branch, so a reader sees the current
speed, accuracy and memory figures without opening anything.

EVERY NUMBER IS A WORST CASE OF ONE CODEC. The badges used to show one figure
each - "2.0 dB SNR", "2009 KB/frame" - and a reader took them for the project's
headline numbers. They were the tightest E-AC-3 check and the AC-4 5.1
encoder's allocator traffic: two different codecs, two different workloads, and
neither says anything about the other. So each badge now carries one figure per
codec (AC-3, E-AC-3, AC-4) and names the workload that figure comes from, and
the site's tiles (docs/javascripts/hero-stats.js) and cards
(docs/performance-quality.md) apply the same rules.

WORKLOADS ARE CLASSIFIED IN ONE TABLE. performance-*.jsonl and memory-*.jsonl
name a workload by `config` and record nothing about its codec, so WORKLOADS
below says which codec each belongs to and how to describe it. A config the
table does not list fails the run (UnknownWorkload) instead of being guessed
at, and test_write_measurement_badges.py fails the pull request that adds a
workload to tests/performance/ without classifying it. The table is copied
into docs/javascripts/hero-stats.js and docs/performance-quality.md; the same
test fails when the three copies differ.

Every threshold is BORROWED, never invented here - the rule
docs/performance-quality.md's status cards follow, and for the same reason: a
badge that shows amber on a healthy build is worse than no badge. Real time is
1x by definition, accuracy compares each channel against its own floor from
that record's thresholds_db, and live memory growth uses the 4 KiB line
append_memory_history.py already warns at. Where no gate exists the badge says
so in its colour (blue) rather than borrowing one: allocation per frame has
none, and AC-4's decode quality has no floors yet.

WRITTEN BY THE JOB THAT ALREADY COMMITS HERE. quality-history has two writers
already (_build.yml's quality-trend job and ci.yml's
persist-performance-trend), and a third would be a third chance to race on the
same branch. This runs inside the second of those, after its appends and
before its commit, and reads every series from that job's own checkout - so
the speed and memory figures are the ones it just wrote, and the accuracy
figures are whatever the quality jobs last pushed. That can be one run behind
on a commit whose quality leg finished after this one. A badge is a summary and
that is an acceptable staleness; the trend pages are where an exact number
lives, and they are one click away.

stdlib-only (json/argparse/pathlib), matching every other script here.
"""

import argparse
import json
import math
from pathlib import Path

# shields.io's endpoint schema. Only these four fields are used; the badge
# picks up its own styling from the URL the README builds.
SCHEMA_VERSION = 1

GREEN = "brightgreen"
AMBER = "orange"
GREY = "lightgrey"
# A figure with no gate to colour it. Not green: green claims a pass.
INFO = "blue"

NO_DATA = "no data"

# The steady-state retention line append_memory_history.py warns at
# (LIVE_GROWTH_WARN_BYTES, and it warns at >= this, not above it). Not a number
# chosen for this script.
MEMORY_RETENTION_WARN_BYTES = 4 * 1024

# The order every message lists them in.
CODECS = ("AC-3", "E-AC-3", "AC-4")

# The `codec` field of the gold-reference records (main.jsonl), which
# verify_gold_reference.sh's --codec-label writes. AC-4's decode quality is a
# series of its own (ac4-quality-*.jsonl) and carries no such field.
QUALITY_CODECS = {"ac3": "AC-3", "eac3": "E-AC-3"}

# config -> (codec, what the workload is). The names in performance-*.jsonl
# (tests/performance/bench_encoder.cpp) and memory-*.jsonl (bench_memory.cpp).
#
# The names follow a convention: ac3_* and plain_* are AC-3 (plain_51 is the
# AC-3 encoder, iclforge::ac3::FrameEncoder, at 5.1 and 448 kbps - "plain" against the
# E-AC-3 and Atmos rows beside it); eac3_*, ecpl_* and atmos_* are E-AC-3
# (ecpl is its enhanced coupling, atmos is Dolby Atmos carried in E-AC-3 as
# joint object coding); ac4_* are AC-4. The table, not the prefix, is what is
# read, and a test holds the two together. The description always says whether
# the workload encodes or decodes, because the two differ by an order of
# magnitude in both time and allocation and a bare figure hides which one it is.
WORKLOADS = {
    "plain_51": ("AC-3", "5.1 encode"),
    "plain_51_fast_mdct": ("AC-3", "5.1 encode, fast MDCT"),
    "ac3_51_encode": ("AC-3", "5.1 encode"),
    "ac3_51_decode": ("AC-3", "5.1 decode"),
    "eac3_51_auto": ("E-AC-3", "5.1 encode, automatic tools"),
    "eac3_stereo_auto": ("E-AC-3", "stereo encode, automatic tools"),
    "eac3_51_encode": ("E-AC-3", "5.1 encode"),
    "eac3_51_decode": ("E-AC-3", "5.1 decode"),
    "ecpl_51_encode": ("E-AC-3", "5.1 enhanced-coupling encode"),
    "atmos_4obj": ("E-AC-3", "Atmos 4-object encode"),
    "atmos_4obj_fast_mdct": ("E-AC-3", "Atmos 4-object encode, fast MDCT"),
    "atmos_4obj_qmf_fast_mdct": ("E-AC-3", "Atmos 4-object encode, QMF, fast MDCT"),
    "atmos_4obj_encode": ("E-AC-3", "Atmos 4-object encode"),
    "atmos_4obj_decode": ("E-AC-3", "Atmos 4-object decode"),
    "ac4_stereo_encode": ("AC-4", "stereo encode"),
    "ac4_stereo_decode": ("AC-4", "stereo decode"),
    "ac4_51_encode": ("AC-4", "5.1 encode"),
    "ac4_51_decode": ("AC-4", "5.1 decode"),
}

# What the number of channels in a quality record says about its layout.
LAYOUTS = {1: "mono", 2: "stereo", 6: "5.1", 8: "7.1"}

LABELS = {
    "speed": "speed vs real time (slowest workload)",
    "accuracy": "decode accuracy, SNR (tightest check)",
    "memory": "allocation per frame (heaviest workload)",
    "growth": "live growth, steady state (4 KiB line)",
}


class UnknownWorkload(ValueError):
    """A workload or codec label the tables above do not list."""


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


def _number(value):
    """A JSON number. Not a bool: Python counts True as an int, JavaScript does not."""
    return isinstance(value, (int, float)) and not isinstance(value, bool)


# Rounded half up, as Math.round does, rather than to even, as format() does:
# the site's scripts print the same figures, and an exact tie (14.25 KiB) has to
# come out the same on both.
def round0(x):
    return math.floor(x + 0.5)


def round1(x):
    return math.floor(x * 10 + 0.5) / 10


def round2(x):
    return math.floor(x * 100 + 0.5) / 100


def one_decimal(x):
    return f"{round1(x):.1f}"


def two_decimals(x):
    return f"{round2(x):.2f}"


def classify(config):
    """(codec, description) of a performance or memory `config`."""
    try:
        return WORKLOADS[config]
    except (KeyError, TypeError):
        raise UnknownWorkload(
            f"workload {config!r} is not in WORKLOADS in tools/ci/write_measurement_badges.py: "
            "add it there with its codec and a description that says whether it encodes or "
            "decodes, and to the copies of the table in docs/javascripts/hero-stats.js and "
            "docs/performance-quality.md") from None


def quality_codec(record):
    """The codec label of a gold-reference record, as this page spells it."""
    try:
        return QUALITY_CODECS[record.get("codec")]
    except (KeyError, TypeError):
        raise UnknownWorkload(
            f"quality codec {record.get('codec')!r} is not in QUALITY_CODECS in "
            "tools/ci/write_measurement_badges.py") from None


def real_time_multiple(row):
    """How many times faster than playback: the frame's budget over its time.

    The budget is the record's own: an AC-4 frame is 2 048 samples, 42.67 ms,
    against A/52's 1 536 samples and 32 ms.
    """
    return (row.get("real_time_budget_ms_per_frame") or 32) / row["ms_per_frame"]


def format_multiple(times):
    """"7" for 7.3, "0.96" for 0.96: a figure below 1x must not round up to it."""
    return str(round0(times)) if times >= 1 else two_decimals(times)


def slowest_per_codec(rows):
    """codec -> its slowest timed workload, as a multiple of real time.

    The slowest, not the mean: "is it fast enough" is a worst-case question,
    and an average over ten workloads would hide one of them dropping below
    real time behind nine that did not. Slowest against its own budget, not in
    raw ms/frame: an AC-4 frame is 2 048 samples against A/52's 1 536, so 4 ms
    is more headroom for AC-4 (10.7x) than 3.2 ms is for AC-3 (10x). Encode and
    decode workloads compete, and the description says which one won.
    """
    picked = {}
    for row in rows:
        ms = row.get("ms_per_frame")
        if not _number(ms) or ms <= 0:
            continue
        codec, words = classify(row.get("config"))
        times = real_time_multiple(row)
        if codec not in picked or times < picked[codec]["times"]:
            picked[codec] = {"config": row["config"], "words": words, "times": times,
                             "ms": ms, "leg": row.get("leg"),
                             "budget": row.get("real_time_budget_ms_per_frame") or 32}
    return picked


def _headroom_db(rec):
    """How much room a record's tightest channel has over its own floor.

    tightest_headroom_db arrived with per-channel floors; records written
    before that carry only worst_db and a scalar threshold_db, and fall back to
    exactly the old computation rather than being dropped from the comparison.
    """
    headroom = rec.get("tightest_headroom_db")
    if _number(headroom):
        return headroom
    return rec["worst_db"] - rec["threshold_db"]


def layout_of(rec):
    """"stereo", "5.1": what a quality record's channel count says it is."""
    labels = rec.get("channel_labels")
    if not isinstance(labels, list):
        labels = rec.get("channels_db")
    if not isinstance(labels, list) or not labels:
        return None
    return LAYOUTS.get(len(labels), f"{len(labels)} channels")


def check_words(rec):
    """The workload of a gold-reference check: layout, rate, and the signal if named."""
    parts = [layout_of(rec)]
    if _number(rec.get("bitrate_kbps")):
        parts.append(f"{round0(rec['bitrate_kbps'])} kbps")
    if "transient" in str(rec.get("check")):
        parts.append("transient")
    return " ".join(p for p in parts if p)


def _describe_check(rec, headroom):
    """The tightest channel of a record, as the card describes it.

    The channel's own SNR when per-channel floors are recorded, so the badge
    shows what the linked card shows. Unlike JavaScript, an out-of-range index
    raises here and a negative one silently wraps, so the recorded index is
    bounds-checked rather than trusted.
    """
    value, floor, channel = rec["worst_db"], rec["threshold_db"], None
    thresholds = rec.get("thresholds_db")
    channels = rec.get("channels_db")
    if isinstance(thresholds, list) and isinstance(channels, list) and channels:
        i = rec.get("tightest_channel")
        if not isinstance(i, int) or isinstance(i, bool) or not 0 <= i < len(channels):
            i = 0
        value = channels[i]
        if i < len(thresholds):
            floor = thresholds[i]
        labels = rec.get("channel_labels")
        channel = labels[i] if isinstance(labels, list) and i < len(labels) else f"ch{i}"
    return {"check": rec.get("check"), "words": check_words(rec), "value": value,
            "floor": floor, "channel": channel, "headroom": headroom, "leg": rec.get("leg")}


def tightest_per_codec(rows):
    """codec -> its tightest gold-reference check, by margin over its own floor.

    Mirrors qualityCard() in docs/performance-quality.md, which is the page
    the accuracy badge links to: a reader who clicks through must not land on a
    different number than the one they clicked. Both pick by MARGIN and report
    the channel that owns it.

    Each check is gated per CHANNEL, so the number that matters is the smallest
    per-channel margin, which is NOT the same thing as the lowest SNR. A 58 dB
    centre channel 6 dB above a 52 dB floor is closer to failing than a 22 dB
    surround 6 dB above a 16 dB floor - and the surround is the one this badge
    used to report, every single time, because it reported the lowest number
    rather than the tightest one.
    """
    picked = {}
    for rec in rows:
        if not (_number(rec.get("worst_db")) and _number(rec.get("threshold_db"))):
            continue
        codec = quality_codec(rec)
        headroom = _headroom_db(rec)
        if codec not in picked or headroom < picked[codec]["headroom"]:
            picked[codec] = _describe_check(rec, headroom)
    return picked


def ac4_quality(rows):
    """AC-4's decode quality: its lowest SNR and its lowest MOS-LQO, no floors.

    ac4-quality-*.jsonl has one row per Dolby stream, scored against the source
    it was encoded from (docs/quality-trend.md, "AC-4 decode quality"). No
    per-channel floors are recorded, only CI's pinned ones, so nothing here can
    be coloured, and the figures are shown without a verdict.
    """
    snr = [r for r in rows if _number(r.get("min_snr_db"))]
    mos = [r for r in rows if _number(r.get("mos_lqo"))]
    if not snr and not mos:
        return None
    low_snr = min(snr, key=lambda r: r["min_snr_db"]) if snr else None
    low_mos = min(mos, key=lambda r: r["mos_lqo"]) if mos else None
    high_mos = max(mos, key=lambda r: r["mos_lqo"]) if mos else None
    return {"streams": len(rows),
            "snr": low_snr["min_snr_db"] if low_snr else None,
            "snr_leg": low_snr.get("leg") if low_snr else None,
            "mos": low_mos["mos_lqo"] if low_mos else None,
            "mos_leg": low_mos.get("leg") if low_mos else None,
            "mos_best": high_mos["mos_lqo"] if high_mos else None}


def _extreme_per_codec(rows, field):
    """codec -> the workload with the largest `field` (the first one on a tie)."""
    picked = {}
    for row in rows:
        value = row.get(field)
        if not _number(value):
            continue
        codec, words = classify(row.get("config"))
        if codec not in picked or value > picked[codec]["value"]:
            picked[codec] = {"config": row["config"], "words": words, "value": value,
                             "frames": row.get("frames"), "leg": row.get("leg"),
                             "allocs": row.get("allocs_per_frame")}
    return picked


def heaviest_per_codec(rows):
    """codec -> the workload that allocates the most bytes per frame.

    That is allocator traffic - bytes requested from the heap over a frame,
    whatever became of them - and not the memory a codec occupies: a workload
    that allocates and frees a 100 KB scratch buffer every frame scores 100 KB
    here and holds none of it. What is still held is live growth, below.
    """
    return _extreme_per_codec(rows, "bytes_per_frame")


def most_retaining_per_codec(rows):
    """codec -> the workload holding the most live bytes after its steady frames."""
    return _extreme_per_codec(rows, "steady_live_growth")


def _labelled(words, figure):
    """"5.1 encode: 7x"; just the figure when a record gives nothing to name it by."""
    return f"{words}: {figure}" if words else figure


def _per_codec(entries, describe):
    """"AC-3 x | E-AC-3 y | AC-4 z": every codec in order, with no data where it has none."""
    return " | ".join(
        f"{codec} {describe(entries[codec]) if codec in entries else NO_DATA}"
        for codec in CODECS)


def speed_badge(rows):
    picked = slowest_per_codec(rows)
    if not picked:
        return badge(LABELS["speed"], NO_DATA, GREY)
    message = _per_codec(
        picked, lambda e: _labelled(e["words"], f"{format_multiple(e['times'])}x"))
    # Real time is 1x by definition, the one line that means something: below
    # it the codec cannot keep up with playback.
    color = GREEN if all(e["times"] >= 1 for e in picked.values()) else AMBER
    return badge(LABELS["speed"], message, color)


def accuracy_badge(rows, ac4_rows=()):
    gated = tightest_per_codec(rows)
    ac4 = ac4_quality(ac4_rows)
    if not gated and ac4 is None:
        return badge(LABELS["accuracy"], NO_DATA, GREY)

    def describe(codec):
        if codec in gated:
            return _labelled(gated[codec]["words"], f"{one_decimal(gated[codec]['value'])} dB")
        if codec == "AC-4" and ac4 is not None:
            figures = []
            if ac4["snr"] is not None:
                figures.append(f"{one_decimal(ac4['snr'])} dB min")
            if ac4["mos"] is not None:
                figures.append(f"MOS-LQO {two_decimals(ac4['mos'])}")
            return _labelled("Dolby streams", ", ".join(figures))
        return NO_DATA

    message = " | ".join(f"{codec} {describe(codec)}" for codec in CODECS)
    if not gated:
        return badge(LABELS["accuracy"], message, INFO)
    # Coloured on the margin, not on worst_db >= threshold_db: the scalar test
    # cannot see a per-channel breach. Against floors [42, 47, 49, 81, 17, 17],
    # a centre channel falling to 48 dB is below its own 49 dB floor while the
    # 18 dB surround still clears the scalar 17 - green on a build the gate
    # itself fails. AC-4 has no floor and takes no part in the colour.
    return badge(LABELS["accuracy"], message,
                 GREEN if all(e["headroom"] >= 0 for e in gated.values()) else AMBER)


def memory_badge(rows):
    """Allocation per frame, the heaviest workload of each codec.

    Coloured blue: nothing gates allocator traffic. It used to take the
    retention colour (green or orange) while showing a churn figure, which is
    how a reader came to see orange beside "2009 KB/frame" with no way to tell
    what it meant. Retention is growth_badge()'s.
    """
    picked = heaviest_per_codec(rows)
    if not picked:
        return badge(LABELS["memory"], NO_DATA, GREY)
    message = _per_codec(
        picked, lambda e: _labelled(e["words"], f"{round0(e['value'] / 1024)} KB"))
    return badge(LABELS["memory"], message, INFO)


def growth_badge(rows):
    """Live bytes still held after the steady-state frames, coloured by the 4 KiB line."""
    picked = most_retaining_per_codec(rows)
    if not picked:
        return badge(LABELS["growth"], NO_DATA, GREY)
    message = _per_codec(
        picked, lambda e: _labelled(e["words"], f"{one_decimal(e['value'] / 1024)} KiB"))
    # append_memory_history.py's check_leak warns at >= 4 KiB.
    held = all(e["value"] < MEMORY_RETENTION_WARN_BYTES for e in picked.values())
    return badge(LABELS["growth"], message, GREEN if held else AMBER)


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
    memory_rows = newest_commit_rows(args.history_dir / f"memory-{b}.jsonl")
    try:
        written = {
            "speed": speed_badge(newest_commit_rows(args.history_dir / f"performance-{b}.jsonl")),
            "accuracy": accuracy_badge(
                newest_commit_rows(args.history_dir / f"{b}.jsonl"),
                newest_commit_rows(args.history_dir / f"ac4-quality-{b}.jsonl")),
            "memory": memory_badge(memory_rows),
            "growth": growth_badge(memory_rows),
        }
    except UnknownWorkload as e:
        print(f"::error title=Measurement badges::{e}")
        return 1
    for name, payload in written.items():
        path = out_dir / f"{name}.json"
        path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
        print(f"{path}: {payload['label']} = {payload['message']} ({payload['color']})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
