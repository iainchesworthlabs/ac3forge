"""Append one CI run's gold-reference SNR results to the quality-history
branch's per-branch JSONL file, and flag a trailing-baseline regression at
two tiers: a soft one (::warning::, never fails this script) and a hard one
(::error::, signalled via $GITHUB_OUTPUT's hard_regression so the caller can
fail the job *after* still committing and pushing the data - see
.github/workflows/_build.yml's "Fail on hard quality regression" step, which
runs after the push so a big regression is never silently un-recorded just
because it also failed the run).

Reads one JSON file per (leg, codec) - compare_wav.py's --json-out schema,
collected under --results-dir/<leg>/<codec>.json by CI's download-artifact
step - and appends one JSONL record per (leg, codec) to
<history-dir>/<branch>.jsonl.

This only ever runs after tools/checks/verify_gold_reference.sh has already passed
on every leg (see .github/workflows/_build.yml: the job that calls this
`needs: build`, which fails - and skips this - if any required leg's gate
failed), so what lands in history is never a broken run's numbers. The
regression check below is a separate, softer signal on top of that hard gate:
a relative-to-history warning, not a threshold this script can fail CI with.

stdlib-only (json/argparse/pathlib), matching compare_wav.py's own
no-new-CI-provisioning reasoning. Takes commit and commit-date as arguments
rather than reading the clock itself - see docs/quality-trend.md and this
project's general rule against Date.now()-style nondeterminism in tooling;
the caller sources commit-date from `git show -s --format=%cI`, i.e. the
commit's own recorded date, not whenever this script happens to run.
"""

import argparse
import json
import os
import sys
from pathlib import Path

# How many trailing same-(branch,leg,codec) entries the regression check
# averages over. Real commit-to-commit noise within one leg has stayed inside
# 0.02-0.08 dB across every run recorded so far (see docs/quality-trend.md) -
# a compiler minor-version bump or a different runner's libm has never moved
# the needle anywhere near a full dB, so REGRESSION_DROP_DB no longer needs
# the multi-dB slack a 30 dB hard floor used to lean on instead.
REGRESSION_TRAILING_WINDOW = 10
REGRESSION_DROP_DB = 0.5
# A much larger drop - the kind that used to only cost a "pass" against the
# old, looser MIN_SNR_DB - is worth failing the run over outright, not just
# annotating a table row someone has to go looking for. Still well above the
# largest single-commit legitimate move seen to date (well under 1 dB).
HARD_REGRESSION_DROP_DB = 10.0


def load_leg_results(results_dir: Path):
    """results_dir holds one subdirectory per leg (an artifact named
    'gold-reference-<preset>', downloaded with the prefix stripped back off by
    the caller - see the workflow step), each holding one JSON file per
    check - verify_gold_reference.sh's check_one writes one
    <label>.json per (codec, tool-set, fixture) combination it runs (e.g.
    eac3.json, eac3_cpl.json, eac3_cplbndstrce0.json), not one per codec:
    two checks can share a codec while comparing fundamentally different
    things (this project's own encoder round-tripped through two decodes,
    vs. a real third-party bitstream decoded by both) with deliberately
    different SNR floors. The filename stem is the only thing that reliably
    tells those apart, so it rides along as "check"."""
    for leg_dir in sorted(results_dir.iterdir()):
        if not leg_dir.is_dir():
            continue
        leg = leg_dir.name.removeprefix("gold-reference-")
        for json_file in sorted(leg_dir.glob("*.json")):
            record = json.loads(json_file.read_text())
            record["leg"] = leg
            record["check"] = json_file.stem
            yield record


# How many of the most recent COMMITS the sidecar window keeps.
#
# docs/quality-trend.md renders TABLE_ROWS (40) entries per series and computes
# a REGRESSION_WINDOW (10) trailing baseline behind the oldest of them, so 50
# commits is what the page actually needs. 80 is that with margin, and still
# an order of magnitude smaller than the full history.
RECENT_WINDOW_COMMITS = 80


def write_recent_window(history_path: Path) -> None:
    """Write a <branch>.recent.jsonl beside the full history.

    The history files are append-only and unbounded - main.jsonl passed 1.7 MB
    and develop.jsonl 1.8 MB by September 2026 - and docs/quality-trend.md
    fetches BOTH in full on every page load to render forty rows per series.
    That is 3.5 MB of history to display fifty commits' worth of it, and it
    grows with every merge.

    The obvious fix is an HTTP suffix Range request from the page, which does
    not work and cannot be made to: raw.githubusercontent.com serves ranges
    happily to curl, but `Range` is not a CORS-safelisted request header, so a
    cross-origin fetch preflights - and that host answers OPTIONS with a 403.
    Verified from the live docs origin: the plain fetch returns 1789155 bytes,
    the ranged one fails outright. So the window has to be produced here, where
    there is no CORS, rather than requested there.

    The full file is still written and still authoritative - this is a derived
    view, regenerated in full each run from the file that was just appended to,
    so it cannot drift from it. A reader wanting the whole series (or a page
    published before this file existed) reads the original.

    Whole commits, not the last N lines: a commit writes one record per
    (leg, codec, check), so a line-count window would cut the newest commit in
    half and the page would render a partial column for it.
    """
    if not history_path.exists():
        return
    records = []
    for raw in history_path.read_text().splitlines():
        line = raw.strip()
        if not line:
            continue
        try:
            records.append((json.loads(line)["commit"], line))
        except (json.JSONDecodeError, KeyError):
            # A malformed or schema-less line is kept out of the window rather
            # than failing the trend job over it; the full file still has it.
            continue

    # Commit order as recorded, not sorted by date: this file is appended to in
    # merge order, which is the order the page walks it in.
    seen = []
    for commit, _ in records:
        if commit not in seen:
            seen.append(commit)
    recent_path = history_path.with_suffix(".recent.jsonl")

    # Nothing to gain while the whole history still fits in the window, and
    # a real cost to writing it anyway: the sidecar would be a byte-for-byte
    # second copy, doubling this branch's size to save the reader nothing.
    # The page falls back to the full file when this is absent, which is
    # exactly the right behaviour in that case. As of September 2026 the
    # quality history is 65 commits deep and 1.7 MB - big because each
    # commit writes ~73 records, not because it is long - so this returns
    # here today and starts producing a window once the history outgrows
    # RECENT_WINDOW_COMMITS. A stale window left by an earlier run is
    # removed rather than left to go out of date.
    if len(seen) <= RECENT_WINDOW_COMMITS:
        recent_path.unlink(missing_ok=True)
        print(f"History is {len(seen)} commit(s), within the {RECENT_WINDOW_COMMITS}-commit "
              f"window - no sidecar written; the page reads {history_path.name}.")
        return

    keep = set(seen[-RECENT_WINDOW_COMMITS:])
    kept = [line for commit, line in records if commit in keep]
    recent_path.write_text("\n".join(kept) + ("\n" if kept else ""))
    print(f"Wrote {len(kept)} record(s) from the last {len(keep)} commit(s) "
          f"to {recent_path}")


def trailing_mean(history_path: Path, leg: str, codec: str, check: str, window: int):
    """Matches on (leg, codec, check), not just (leg, codec): see
    load_leg_results's docstring for why codec alone can conflate two
    checks with unrelated SNR floors (e.g. eac3's tools=none/tools=cpl
    round-trip checks, ~55 dB floor, vs. eac3_cplbndstrce0's real-bitstream
    interop check, ~15 dB floor - averaging the latter into the former's
    trailing mean makes an always-passing, unmoving check look like a
    ~25 dB hard regression against a mean it was never part of). Older
    history entries recorded before this field existed have no "check" key
    and so never match a current (non-None) check - they simply age out of
    the trailing window rather than being mismatched to the wrong series.
    """
    if not history_path.exists():
        return None
    matches = []
    for line in history_path.read_text().splitlines():
        if not line.strip():
            continue
        rec = json.loads(line)
        if rec.get("leg") == leg and rec.get("codec") == codec and rec.get("check") == check:
            matches.append(rec["worst_db"])
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
    parser.add_argument("--results-dir", type=Path, required=True)
    parser.add_argument("--history-dir", type=Path, required=True)
    parser.add_argument("--branch", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--commit-date", required=True,
                         help="Committer date, ISO 8601 (from `git show -s --format=%%cI`).")
    args = parser.parse_args()

    history_path = args.history_dir / f"{args.branch}.jsonl"
    records = list(load_leg_results(args.results_dir))
    if not records:
        print("::warning::no gold-reference JSON results found under "
              f"{args.results_dir}; nothing to append")
        return 0

    lines = []
    hard_regression = False
    for rec in records:
        baseline = trailing_mean(history_path, rec["leg"], rec["codec"], rec["check"],
                                  REGRESSION_TRAILING_WINDOW)
        entry = {
            "commit": args.commit,
            "branch": args.branch,
            "commit_date": args.commit_date,
            "leg": rec["leg"],
            "codec": rec["codec"],
            "check": rec["check"],
            "bitrate_kbps": rec["bitrate_kbps"],
            "worst_db": rec["worst_db"],
            "channels_db": rec["channels_db"],
            "threshold_db": rec["threshold_db"],
        }
        lines.append(json.dumps(entry, sort_keys=True))
        drop = None if baseline is None else baseline - rec["worst_db"]
        if drop is not None and drop >= HARD_REGRESSION_DROP_DB:
            hard_regression = True
            print(f"::error title=Quality trend hard regression::{rec['leg']}/{rec['check']}: "
                  f"worst-channel SNR {rec['worst_db']:.2f} dB is "
                  f"{drop:.2f} dB below the trailing {REGRESSION_TRAILING_WINDOW}-run mean "
                  f"({baseline:.2f} dB) on {args.branch} - more than the "
                  f"{HARD_REGRESSION_DROP_DB:.0f} dB hard-regression threshold. Still recorded "
                  "below; the run this came from is failed separately so this doesn't go "
                  "unnoticed.")
        elif drop is not None and drop >= REGRESSION_DROP_DB:
            print(f"::warning title=Quality trend regression::{rec['leg']}/{rec['check']}: "
                  f"worst-channel SNR {rec['worst_db']:.2f} dB is "
                  f"{drop:.2f} dB below the trailing "
                  f"{REGRESSION_TRAILING_WINDOW}-run mean ({baseline:.2f} dB) on {args.branch}. "
                  f"Still above the hard {rec['threshold_db']:.0f} dB gate - this is a trend "
                  "warning, not a failure.")

    args.history_dir.mkdir(parents=True, exist_ok=True)
    with history_path.open("a") as f:
        for line in lines:
            f.write(line + "\n")

    print(f"Appended {len(lines)} record(s) to {history_path}")
    write_recent_window(history_path)
    emit_github_output("hard_regression", "true" if hard_regression else "false")
    return 0


if __name__ == "__main__":
    sys.exit(main())
