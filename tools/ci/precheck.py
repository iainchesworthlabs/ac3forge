#!/usr/bin/env python3
"""Run the gate's static checks locally, before a push.

    python tools/ci/precheck.py                  # the diff against github/main (or origin/main)
    python tools/ci/precheck.py --base <ref>     # against another base
    python tools/ci/precheck.py --unit           # and the unit tests of the tools/ scripts

These are the checks pr-gate.yml's static job runs, minus the ones that need a
tool this machine may not have (each says SKIP and why rather than failing). A
push that would fail one of them costs a round trip of several minutes; running
this first costs seconds and catches a misnamed branch, a broken doc link, a
missing fixture entry or an unattributed commit before anything is queued.

It also prints what the gate will plan for the diff (see plan_gate.py), so the
author knows whether a Linux build is about to run and whether Qt is in it.

Exit status is 1 if any check fails; a SKIP is not a failure.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import plan_gate  # noqa: E402

BRANCH_RE = re.compile(r"^(feature|bugfix|hotfix|docs|chore)/[a-z0-9]+(-[a-z0-9]+)*$")
# The identity ci's patch-attribution step expects, as in _static.yml.
EXPECT_NAME = "Iain Chesworth"
EXPECT_EMAIL = "iain.chesworth@gmail.com"


@dataclass(frozen=True)
class Result:
    name: str
    status: str  # pass | fail | skip
    detail: str
    seconds: float = 0.0


def branch_name_ok(name: str) -> bool:
    return name == "main" or name.startswith("dependabot/") or bool(BRANCH_RE.match(name))


def exit_code(results: list[Result]) -> int:
    return 1 if any(r.status == "fail" for r in results) else 0


def render(results: list[Result]) -> str:
    width = max(len(r.name) for r in results)
    lines = []
    for r in results:
        note = f"  {r.detail}" if r.detail else ""
        lines.append(f"{r.status.upper():4}  {r.name:<{width}}  {r.seconds:5.1f}s{note}")
    return "\n".join(lines)


def run(cmd: list[str], *, name: str, tail: int = 25) -> Result:
    start = time.monotonic()
    try:
        res = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, check=False)
    except OSError as e:
        return Result(name, "skip", f"could not run {cmd[0]}: {e}")
    took = time.monotonic() - start
    if res.returncode == 0:
        return Result(name, "pass", "", took)
    output = (res.stdout + res.stderr).strip().splitlines()
    return Result(name, "fail", "\n" + "\n".join(f"      {ln}" for ln in output[-tail:]), took)


def git(*args: str) -> str:
    res = subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, check=False)
    return res.stdout.strip()


def default_base() -> str:
    for ref in ("github/main", "origin/main", "main"):
        if (
            subprocess.run(
                ["git", "rev-parse", "--verify", "--quiet", ref],
                cwd=ROOT,
                capture_output=True,
                check=False,
            ).returncode
            == 0
        ):
            return ref
    return "main"


def plan_line(paths: list[str]) -> str:
    p = plan_gate.plan(paths)
    return f"build={p['build']} gui={p['gui']} docs_only={p['docs_only']} ({p['reason']})"


def check_branch() -> Result:
    branch = git("branch", "--show-current")
    if not branch:
        return Result("branch name", "skip", "detached HEAD")
    if branch_name_ok(branch):
        return Result("branch name", "pass", "")
    return Result(
        "branch name",
        "fail",
        f"'{branch}' must be <feature|bugfix|hotfix|docs|chore>/<lowercase-kebab-name>",
    )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--base", default=None, help="Base ref for the diff (default github/main).")
    parser.add_argument("--unit", action="store_true", help="Also run the tools/ unit tests.")
    args = parser.parse_args(argv[1:])

    base = args.base or default_base()
    py = sys.executable
    changed = [p for p in git("diff", "--name-only", f"{base}...HEAD").splitlines() if p]

    print(f"base {base}; {len(changed)} file(s) changed")
    print(f"gate plan: {plan_line(changed)}\n")

    results = [check_branch()]
    results.append(run([py, "tools/checks/check_doc_paths.py"], name="doc and script paths"))
    results.append(run([py, "tools/checks/check_platform_matrix.py"], name="platform matrix"))
    results.append(
        run([py, "tools/checks/check_esp_efuse_free.py"], name="esp efuse-free settings")
    )
    if os.name == "nt" and git("config", "core.autocrlf") == "true":
        # The corpus manifest hashes bytes, and a Windows checkout with autocrlf
        # rewrites the text fixtures' line endings, so the hash would differ here
        # while matching in CI's Linux checkout.
        results.append(Result("fixture corpus", "skip", "core.autocrlf=true changes fixture bytes"))
    else:
        results.append(run([py, "tools/checks/check_corpus.py"], name="fixture corpus"))
    results.append(
        run([py, "tools/checks/generate_support_matrices.py", "--check"], name="support matrices")
    )
    if git("rev-list", "--count", f"{base}..HEAD") == "0":
        results.append(Result("patch attribution", "skip", "no commits ahead of the base"))
    else:
        attribution = [py, "tools/checks/check_patch_attribution.py", "--range", f"{base}..HEAD"]
        attribution += ["--expect-name", EXPECT_NAME, "--expect-email", EXPECT_EMAIL]
        results.append(run(attribution, name="patch attribution"))

    pwsh = shutil.which("pwsh")
    if pwsh:
        results.append(
            run(
                [
                    pwsh,
                    "-NoProfile",
                    "-File",
                    "tools/checks/check_platform_macros.ps1",
                    "-Root",
                    str(ROOT),
                ],
                name="platform macros",
            )
        )
    else:
        results.append(Result("platform macros", "skip", "pwsh not found"))

    ruff = shutil.which("ruff")
    if ruff:
        results.append(run([ruff, "check", "--no-cache", "."], name="ruff"))
    else:
        results.append(
            Result(
                "ruff",
                "skip",
                "ruff not on PATH (pip install -r requirements/requirements-lint.txt)",
            )
        )

    if args.unit:
        for suite in ("tools/checks", "tools/ci", "tools/hearth"):
            results.append(
                run(
                    [
                        py,
                        "-m",
                        "unittest",
                        "discover",
                        "--start-directory",
                        suite,
                        "--pattern",
                        "test_*.py",
                    ],
                    name=f"unit tests {suite}",
                )
            )

    print(render(results))
    code = exit_code(results)
    print("\nprecheck: " + ("failed" if code else "ok"))
    return code


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
