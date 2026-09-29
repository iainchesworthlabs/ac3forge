#!/usr/bin/env python3
"""Choose the legs of the build matrix a run needs (_build.yml's `plan-legs` job).

    TIER=t2 LINUX_RUNNER_1='["self-hosted","Linux","X64"]' ... \\
      python3 tools/ci/plan_legs.py >> "$GITHUB_OUTPUT"

    python3 tools/ci/plan_legs.py --check    # validate .github/ci/legs.jsonc and exit
    LEGS=linux-gcc python3 tools/ci/plan_legs.py --lanes   # lane flags for ci.yml's `changes` job

Reads .github/ci/legs.jsonc (its header describes the fields) and prints, for
`$GITHUB_OUTPUT`:

    linux_matrix, windows_matrix, macos_matrix
        the platform's matrix as JSON, `{"include": [...]}`, ready for
        `strategy.matrix: ${{ fromJSON(...) }}`. Each leg has its runner labels
        resolved into `runner` (the labels as JSON text, which is what the
        workflows pass to fromJSON) and loses `tier` and `runner_slot`.
    linux_any, windows_any, macos_any
        `true` when the platform has a leg to run. A matrix with no legs is an
        error in Actions, so the caller skips the job instead.

Environment:

    TIER      `all` (the default) is every leg; `t2` the legs of the run on main after
              a merge, each without the flags it lists in `deep_only` (its slow extra
              passes); `deep` the legs that run only in the scheduled run.
    LEGS      optional, comma-separated presets (`linux-gcc,windows-msvc`). Only
              those legs run, whatever their tier. It is how a dispatch asks for
              exactly the legs it wants. TIER still decides their extra passes: with
              `t2` they run as the run after a merge would run them.
    LINUX_RUNNER_1 ..., WINDOWS_RUNNER_1 ...
              check-runners' outputs: the labels a `runner_slot` leg runs on, as JSON
              text. A leg that needs one and finds it unset is an error, because a
              wrong guess would send it to the wrong pool without a word.

`--lanes` prints `<lane>=true|false` for every lane classify_changes.py knows, for a run that
asked for exact legs (`LEGS`): the platforms those legs are on are true and everything else is
false, so nothing but the requested builds runs.

The catalogue is plain JSON except for comment lines, which start with `//` and
stand alone on their line. Keeping the legs' long comments there, beside the fields
they explain, is why it is not a `.json` file.
"""

from __future__ import annotations

import json
import os
import re
import sys
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

import classify_changes

CATALOGUE = Path(__file__).resolve().parents[2] / ".github" / "ci" / "legs.jsonc"
PLATFORMS = ("linux", "windows", "macos")
TIERS = ("t2", "deep")
SELECTIONS = ("all", *TIERS)
SLOT = re.compile(r"^(linux|windows|macos)_runner_[1-9][0-9]*$")

# Fields the planner consumes; everything else is passed through to the matrix.
PLANNER_FIELDS = ("tier", "runner_slot", "deep_only")
IDENTITY_FIELDS = ("name", "preset", "runner")


class CatalogueError(Exception):
    """The catalogue or the request cannot be planned."""


def parse(text: str) -> dict[str, list[dict[str, Any]]]:
    """Parse catalogue text: JSON once the whole-line `//` comments are dropped."""
    body = "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("//"))
    try:
        data = json.loads(body)
    except json.JSONDecodeError as e:
        raise CatalogueError(f"legs.jsonc is not valid JSON after removing comments: {e}") from e
    return data


def problems(catalogue: Any) -> list[str]:
    """Everything wrong with a parsed catalogue, one message each; empty means valid."""
    if not isinstance(catalogue, dict):
        return ["the catalogue must be an object with one list of legs per platform"]
    found: list[str] = []
    if tuple(catalogue) != PLATFORMS:
        found.append(
            f"the platforms must be {list(PLATFORMS)} in that order, got {list(catalogue)}"
        )
    names: dict[str, str] = {}
    presets: dict[str, str] = {}
    for platform, legs in catalogue.items():
        if not isinstance(legs, list) or not legs:
            found.append(f"{platform}: must be a non-empty list of legs")
            continue
        slots: set[str] = set()
        for i, leg in enumerate(legs):
            where = f"{platform}[{i}]"
            if not isinstance(leg, dict):
                found.append(f"{where}: a leg must be an object")
                continue
            name, preset = leg.get("name"), leg.get("preset")
            if not isinstance(name, str) or not name:
                found.append(f"{where}: `name` must be a non-empty string")
            else:
                where = f"{platform} leg {name!r}"
                if name in names:
                    found.append(f"{where}: name already used by {names[name]}")
                names[name] = where
            if not isinstance(preset, str) or not preset:
                found.append(f"{where}: `preset` must be a non-empty string")
            else:
                if preset in presets:
                    found.append(f"{where}: preset {preset!r} already used by {presets[preset]}")
                presets[preset] = where
            if leg.get("tier") not in TIERS:
                found.append(
                    f"{where}: `tier` must be one of {list(TIERS)}, got {leg.get('tier')!r}"
                )
            deep = leg.get("deep_only", [])
            if not isinstance(deep, list) or not all(isinstance(f, str) for f in deep):
                found.append(f"{where}: `deep_only` must be a list of field names")
            else:
                for flag in deep:
                    if flag in PLANNER_FIELDS + IDENTITY_FIELDS or flag not in leg:
                        found.append(
                            f"{where}: `deep_only` names {flag!r}, which is not a flag of this leg"
                        )
                if deep and leg.get("tier") == "deep":
                    found.append(f"{where}: a deep leg is all deep, so `deep_only` does nothing")
            has_runner, has_slot = "runner" in leg, "runner_slot" in leg
            if has_runner == has_slot:
                found.append(f"{where}: give exactly one of `runner` and `runner_slot`")
            if has_runner:
                labels = leg["runner"]
                if (
                    not isinstance(labels, list)
                    or not labels
                    or not all(isinstance(x, str) and x for x in labels)
                ):
                    found.append(f"{where}: `runner` must be a non-empty list of label strings")
            if has_slot:
                slot = leg["runner_slot"]
                if not isinstance(slot, str) or not SLOT.match(slot):
                    found.append(f"{where}: `runner_slot` must look like {platform}_runner_1")
                elif not slot.startswith(platform + "_"):
                    found.append(f"{where}: `runner_slot` {slot!r} belongs to another platform")
                elif slot in slots:
                    found.append(f"{where}: `runner_slot` {slot!r} is used by another leg here")
                else:
                    slots.add(slot)
    return found


def load(path: Path = CATALOGUE) -> dict[str, list[dict[str, Any]]]:
    """Read and validate the catalogue."""
    catalogue = parse(path.read_text(encoding="utf-8"))
    errors = problems(catalogue)
    if errors:
        raise CatalogueError("\n".join(errors))
    return catalogue


def select(
    catalogue: Mapping[str, Sequence[Mapping[str, Any]]],
    tier: str = "all",
    only: Sequence[str] = (),
) -> dict[str, list[Mapping[str, Any]]]:
    """The legs a run needs, per platform, in catalogue order."""
    if tier not in SELECTIONS:
        raise CatalogueError(f"TIER must be one of {list(SELECTIONS)}, got {tier!r}")
    if only:
        known = {leg["preset"] for legs in catalogue.values() for leg in legs}
        unknown = [p for p in only if p not in known]
        if unknown:
            raise CatalogueError(
                f"unknown preset(s) in LEGS: {', '.join(unknown)}; the legs are: "
                + ", ".join(sorted(known))
            )
        return {p: [leg for leg in legs if leg["preset"] in only] for p, legs in catalogue.items()}
    if tier == "all":
        return {p: list(legs) for p, legs in catalogue.items()}
    return {p: [leg for leg in legs if leg["tier"] == tier] for p, legs in catalogue.items()}


def resolve(
    leg: Mapping[str, Any], env: Mapping[str, str], *, drop_deep: bool = False
) -> dict[str, Any]:
    """A leg as the workflow's matrix wants it: `runner` filled in, planner fields gone.

    `drop_deep` also removes the flags the leg lists in `deep_only`, which is how the run
    after a merge runs a leg without its slow extra passes.
    """
    dropped = set(leg.get("deep_only", ())) if drop_deep else set()
    out = {
        k: v
        for k, v in leg.items()
        if k not in PLANNER_FIELDS and k != "runner" and k not in dropped
    }
    if "runner_slot" in leg:
        var = leg["runner_slot"].upper()
        labels = env.get(var, "").strip()
        if not labels:
            raise CatalogueError(
                f"leg {leg['name']!r} runs on {leg['runner_slot']}, but ${var} is not set"
            )
        try:
            parsed = json.loads(labels)
        except json.JSONDecodeError as e:
            raise CatalogueError(f"${var} is not JSON: {labels!r} ({e})") from e
        if not isinstance(parsed, list) or not parsed:
            raise CatalogueError(f"${var} must be a JSON list of runner labels, got {labels!r}")
        out["runner"] = labels
    else:
        out["runner"] = json.dumps(leg["runner"], separators=(",", ":"))
    return out


def request(env: Mapping[str, str]) -> tuple[str, list[str]]:
    """The tier and the explicit presets a run asked for, from TIER and LEGS."""
    tier = env.get("TIER", "").strip() or "all"
    only = [p.strip() for p in env.get("LEGS", "").split(",") if p.strip()]
    return tier, only


def drops_deep_flags(tier: str) -> bool:
    """Only tier t2, the run after a merge, leaves a leg's slow extra passes out.

    It holds for named legs too: `LEGS=linux-gcc TIER=t2` is the leg as the run after a
    merge would run it, which is how to preview that run cheaply. `LEGS` alone is all of
    the leg.
    """
    return tier == "t2"


def plan(
    catalogue: Mapping[str, Sequence[Mapping[str, Any]]],
    env: Mapping[str, str],
) -> dict[str, str]:
    """`$GITHUB_OUTPUT` values for the request in `env` (TIER, LEGS and the runner slots)."""
    tier, only = request(env)
    chosen = select(catalogue, tier, only)
    drop_deep = drops_deep_flags(tier)
    out: dict[str, str] = {}
    for platform in PLATFORMS:
        legs = [resolve(leg, env, drop_deep=drop_deep) for leg in chosen[platform]]
        out[f"{platform}_matrix"] = json.dumps({"include": legs}, separators=(",", ":"))
        out[f"{platform}_any"] = "true" if legs else "false"
    return out


def describe(catalogue: Mapping[str, Sequence[Mapping[str, Any]]], env: Mapping[str, str]) -> str:
    """One line per platform: the legs that run and the ones left out, for the run's log."""
    tier, only = request(env)
    chosen = select(catalogue, tier, only)
    what = f"LEGS={','.join(only)}" if only else f"TIER={tier}"
    lines = [f"plan-legs ({what}):"]
    drop_deep = drops_deep_flags(tier)
    for platform in PLATFORMS:
        run = []
        for leg in chosen[platform]:
            cut = leg.get("deep_only", []) if drop_deep else []
            run.append(f"{leg['name']} (without {', '.join(cut)})" if cut else leg["name"])
        skipped = [leg["name"] for leg in catalogue[platform] if leg not in chosen[platform]]
        lines.append(f"  {platform}: {', '.join(run) or 'none'}")
        if skipped:
            lines.append(f"    not in this run: {', '.join(skipped)}")
    return "\n".join(lines)


def lanes(
    catalogue: Mapping[str, Sequence[Mapping[str, Any]]], env: Mapping[str, str]
) -> dict[str, bool]:
    """Lane flags for a run that named exact legs: their platforms, and nothing else."""
    chosen = select(catalogue, *request(env))
    flags = dict.fromkeys(classify_changes.LANES, False)
    for platform in PLATFORMS:
        flags[platform] = bool(chosen[platform])
    return flags


def main(argv: list[str]) -> int:
    modes = {"--check", "--lanes"}
    unknown = [a for a in argv[1:] if a not in modes]
    if unknown or len(argv) > 2:
        print(f"usage: {argv[0]} [--check | --lanes]", file=sys.stderr)
        return 2
    try:
        catalogue = load()
        if "--check" in argv[1:]:
            count = sum(len(legs) for legs in catalogue.values())
            print(f"legs.jsonc: {count} legs, valid")
            return 0
        env = dict(os.environ)
        if "--lanes" in argv[1:]:
            for lane, value in lanes(catalogue, env).items():
                print(f"{lane}={'true' if value else 'false'}")
            return 0
        out = plan(catalogue, env)
        print(describe(catalogue, env), file=sys.stderr)
    except CatalogueError as e:
        print(f"::error title=plan-legs::{str(e).splitlines()[0]}", file=sys.stderr)
        print(str(e), file=sys.stderr)
        return 1
    for key, value in out.items():
        print(f"{key}={value}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
