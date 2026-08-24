"""Score a listening session and render the results table for docs/landscape.md.

Reads the trials key tools/listening/gen_listening_stimuli.py wrote and one
filled-in response CSV per listener, and prints a markdown table with a
confidence interval on every number. Handles both methods that script can
produce, picking whichever one the key describes:

  mushra  BS.1534-3 continuous quality scale, 0-100 per condition per
          listener. Reports each condition's mean with a 95% confidence
          interval on that mean.
  abx     Forced-choice A/B/X. Reports each (leg, system) pair's proportion
          correct with a 95% confidence interval and an exact one-sided
          binomial p-value against the 0.5 guessing rate.

Stdlib-only, deliberately - unlike the stimulus generator beside it, which
needs numpy and FFmpeg. Anyone who ran a session should be able to score it
without provisioning anything, including on a machine that never built this
project.

That constraint decides the statistics, and both choices are worth naming
rather than leaving to be inferred:

  MUSHRA's interval is Student's t, which needs a t quantile. T95 below is a
  table of two-sided 95% critical values for 1-30 degrees of freedom, which
  is every panel size this could plausibly see, falling back to the normal
  1.96 above that (t is within 0.5% of it by df=30 and the gap only closes).
  A table is also auditable in a way a series expansion is not.

  ABX's interval is Wilson's score interval, not Clopper-Pearson. Wilson is
  closed-form - it needs no inverse beta - and it is the better interval
  anyway at the small n and near-boundary proportions this will actually
  see: Clopper-Pearson is exact in the sense of never under-covering, which
  it achieves by over-covering, and its intervals are correspondingly wider
  than the data justify. The p-value beside it IS exact (a plain binomial
  tail sum via math.comb), so the significance claim itself owes nothing to
  an approximation.

BS.1534-3 post-screening is applied to MUSHRA responses: a listener who
scored the HIDDEN reference below 90 on more than 15% of trials is excluded,
because on a correctly-run session the hidden reference is the reference and
a listener who did not hear that was not discriminating. Every exclusion is
reported by name, with its own numbers, rather than quietly dropped - a
session that excluded most of its panel has a problem with the session, and
that has to be visible.

Usage (repo root):

    python tools/listening/score_listening_test.py \\
        --key listening-session/trials.csv \\
        --responses listening-session/responses/

    python tools/listening/score_listening_test.py ... --markdown-out results.md

With no response files it prints the empty table and says a session has not
been run, which is the honest state until one has.
"""

import argparse
import csv
import math
import statistics
import sys
from collections import defaultdict
from pathlib import Path

# Two-sided 95% critical values of Student's t, df 1..30. Above 30 the normal
# quantile is used - see the module docstring.
T95 = {
    1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571, 6: 2.447, 7: 2.365,
    8: 2.306, 9: 2.262, 10: 2.228, 11: 2.201, 12: 2.179, 13: 2.160, 14: 2.145,
    15: 2.131, 16: 2.120, 17: 2.110, 18: 2.101, 19: 2.093, 20: 2.086,
    21: 2.080, 22: 2.074, 23: 2.069, 24: 2.064, 25: 2.060, 26: 2.056,
    27: 2.052, 28: 2.048, 29: 2.045, 30: 2.042,
}
Z95 = 1.96

# BS.1534-3 §6.1's post-screening rule for the hidden reference.
POSTSCREEN_MIN_SCORE = 90.0
POSTSCREEN_MAX_FAIL_FRACTION = 0.15


def t_critical(df: int) -> float:
    if df < 1:
        return float("nan")
    return T95.get(df, Z95)


def mean_ci(values):
    """(mean, half-width of the 95% CI on the mean). Half-width is None for
    n < 2, where there is no spread to estimate one from - reported as such
    rather than as a zero-width interval, which would read as certainty."""
    n = len(values)
    if n == 0:
        return None, None
    mean = statistics.fmean(values)
    if n < 2:
        return mean, None
    sd = statistics.stdev(values)
    return mean, t_critical(n - 1) * sd / math.sqrt(n)


def wilson_ci(correct: int, total: int):
    """95% Wilson score interval for a proportion, as (low, high)."""
    if total == 0:
        return None, None
    p = correct / total
    z2 = Z95 * Z95
    denominator = 1.0 + z2 / total
    centre = (p + z2 / (2 * total)) / denominator
    spread = (Z95 * math.sqrt(p * (1.0 - p) / total + z2 / (4 * total * total))) / denominator
    return max(0.0, centre - spread), min(1.0, centre + spread)


def binomial_p(correct: int, total: int, rate: float = 0.5) -> float:
    """Exact one-sided p: P(at least `correct` successes | guessing at `rate`)."""
    if total == 0:
        return float("nan")
    return sum(math.comb(total, k) * rate**k * (1.0 - rate) ** (total - k)
               for k in range(correct, total + 1))


def load_key(path: Path):
    rows = list(csv.DictReader(path.open(encoding="utf-8")))
    if not rows:
        raise SystemExit(f"{path} has no rows")
    method = "abx" if "system" in rows[0] else "mushra"
    return method, rows


def load_responses(paths):
    """Every response row from every named CSV or directory of CSVs.

    A directory is walked for *.csv but responses_template.csv is skipped by
    name: it is the blank the operator copies, and scoring the blank would
    quietly add a listener with no answers.
    """
    rows = []
    for path in paths:
        files = (sorted(p for p in path.glob("*.csv") if p.name != "responses_template.csv")
                 if path.is_dir() else [path])
        for f in files:
            for row in csv.DictReader(f.open(encoding="utf-8")):
                if not any((v or "").strip() for v in row.values()):
                    continue
                row["_source"] = f.name
                rows.append(row)
    return rows


def score_mushra(key_rows, responses):
    """(rows, excluded, condition_order) - one row per (leg, condition)."""
    condition_of = {(r["leg"], r["label"]): r["condition"] for r in key_rows}
    warnings_of = {(r["leg"], r["condition"]): r["decode_warnings"] for r in key_rows}

    # Post-screening first, so an excluded listener's scores never reach the
    # aggregate below.
    hidden = defaultdict(list)
    for r in responses:
        listener = (r.get("listener") or r["_source"]).strip()
        condition = condition_of.get((r["leg"], r["label"]))
        if condition == "reference":
            hidden[listener].append(float(r["score"]))
    excluded = {}
    for listener, scores in hidden.items():
        failed = sum(1 for s in scores if s < POSTSCREEN_MIN_SCORE)
        if scores and failed / len(scores) > POSTSCREEN_MAX_FAIL_FRACTION:
            excluded[listener] = (failed, len(scores))

    by_condition = defaultdict(list)
    listeners = set()
    for r in responses:
        listener = (r.get("listener") or r["_source"]).strip()
        if listener in excluded:
            continue
        listeners.add(listener)
        condition = condition_of.get((r["leg"], r["label"]))
        if condition is None:
            raise SystemExit(f"response for {r['leg']}/{r['label']} is not in the trials key")
        by_condition[(r["leg"], condition)].append(float(r["score"]))

    rows = []
    for (leg, condition), scores in sorted(by_condition.items()):
        mean, half = mean_ci(scores)
        rows.append({
            "leg": leg, "condition": condition, "n": len(scores),
            "mean": mean, "ci": half,
            "decode_warnings": warnings_of.get((leg, condition), ""),
        })
    return rows, excluded, sorted(listeners)


def score_abx(key_rows, responses):
    """(rows, listeners) - one row per (leg, system)."""
    key_of = {r["trial"]: r for r in key_rows}
    tally = defaultdict(lambda: [0, 0])
    listeners = set()
    for r in responses:
        trial = key_of.get(r["trial"])
        if trial is None:
            raise SystemExit(f"response for trial {r['trial']} is not in the trials key")
        listeners.add((r.get("listener") or r["_source"]).strip())
        answer = (r.get("answer") or "").strip().lower()
        if answer not in ("a", "b"):
            raise SystemExit(f"trial {r['trial']}: answer must be 'a' or 'b', got {answer!r}")
        key = (trial["leg"], trial["system"])
        tally[key][1] += 1
        if answer == trial["x"]:
            tally[key][0] += 1

    rows = []
    for (leg, system), (correct, total) in sorted(tally.items()):
        low, high = wilson_ci(correct, total)
        rows.append({
            "leg": leg, "condition": system, "correct": correct, "n": total,
            "proportion": correct / total if total else None,
            "ci_low": low, "ci_high": high,
            "p": binomial_p(correct, total),
            "decode_warnings": next((k["decode_warnings"] for k in key_rows
                                     if k["leg"] == leg and k["system"] == system), ""),
        })
    return rows, sorted(listeners)


def markdown_mushra(rows, excluded, listeners):
    out = ["| Leg | Condition | n | Mean score | 95% CI |", "|---|---|---|---|---|"]
    for r in rows:
        ci = "—" if r["ci"] is None else f"±{r['ci']:.1f}"
        out.append(f"| `{r['leg']}` | `{r['condition']}` | {r['n']} | {r['mean']:.1f} | {ci} |")
    out.append("")
    out.append(f"{len(listeners)} listener(s) after post-screening"
               + (f"; {len(excluded)} excluded" if excluded else ""))
    for listener, (failed, total) in sorted(excluded.items()):
        out.append(f"  - excluded `{listener}`: scored the hidden reference below "
                   f"{POSTSCREEN_MIN_SCORE:.0f} on {failed} of {total} trials")
    return "\n".join(out)


def markdown_abx(rows, listeners):
    out = ["| Leg | System | Correct | Trials | Proportion | 95% CI | p |",
           "|---|---|---|---|---|---|---|"]
    for r in rows:
        ci = "—" if r["ci_low"] is None else f"{r['ci_low']:.2f}–{r['ci_high']:.2f}"
        p = f"{r['p']:.4f}" if r["p"] >= 0.0001 else "<0.0001"
        out.append(f"| `{r['leg']}` | `{r['condition']}` | {r['correct']} | {r['n']} | "
                   f"{r['proportion']:.2f} | {ci} | {p} |")
    out.append("")
    out.append(f"{len(listeners)} listener(s). p is the exact one-sided binomial probability "
               "of scoring at least this well by guessing; below 0.05 the listener could tell "
               "the system from the reference.")
    return "\n".join(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--key", type=Path, required=True,
                        help="trials.csv from gen_listening_stimuli.py.")
    parser.add_argument("--responses", type=Path, nargs="*", default=[],
                        help="Filled-in response CSVs, or directories of them.")
    parser.add_argument("--markdown-out", type=Path, default=None,
                        help="Also write the table to this file.")
    args = parser.parse_args()

    method, key_rows = load_key(args.key)
    responses = load_responses(args.responses)

    if not responses:
        print(f"No responses found — this is a {method} stimulus set that has not been "
              f"listened to yet.")
        print("Nothing is reported rather than a table of zeros: an unrun session has no "
              "result, which is not the same as a result of nothing.")
        return 0

    if method == "mushra":
        rows, excluded, listeners = score_mushra(key_rows, responses)
        table = markdown_mushra(rows, excluded, listeners)
    else:
        rows, listeners = score_abx(key_rows, responses)
        table = markdown_abx(rows, listeners)

    print(table)
    flagged = [r for r in rows if r["decode_warnings"]]
    if flagged:
        print()
        print("FFmpeg's decoder complained while building some of these stimuli, so the "
              "score below is of that decode, not of the encoder alone:")
        for r in flagged:
            print(f"  {r['leg']}/{r['condition']}: {r['decode_warnings']}")

    if args.markdown_out is not None:
        args.markdown_out.parent.mkdir(parents=True, exist_ok=True)
        args.markdown_out.write_text(table + "\n", encoding="utf-8")
        print(f"\nwrote {args.markdown_out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
