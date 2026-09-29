#!/usr/bin/env python3
"""What happens after a post-merge run of ci.yml completes (main-health.yml).

    python3 tools/ci/main_health.py --repo o/r --run-id 123 --head-sha <sha> \\
        --conclusion failure --attempt 1 --run-url <url>

Post-merge verification is batched (one run at a time, the newest push waits its
turn), so a red run says "something in the merges since the last green run broke
this", not which one. This script turns that into an answer, and does the parts of
the remediation that need no judgement:

  success    advance the `verified` ref to this commit (never backwards), and close
             the open `main-red` issue if there is one.
  failure    if every failed job matches a signature in known_flakes.json and this
             is the first attempt, rerun the failed jobs once. Otherwise name the
             merges in `verified..head` as suspects, open (or update) the single
             `main-red` issue with the failed jobs, their evidence and the command
             that reproduces each, and comment once on each suspect pull request.
  cancelled  a superseded run: nothing.

The scheduled run (`--event schedule`) is the whole matrix, including the legs the run
after a merge leaves out, so it keeps its own books. Its green moves `verified-nightly`
as well as `verified` and closes both issues, because it proves everything the other
run does. Its red is blamed on the merges since the last green nightly, not since
`verified` (a sanitizer failure can come from a merge that the run after it passed
without ever running the sanitizers), goes to its own `main-red-nightly` issue, and does
not comment on pull requests: a day of merges is too wide a range to name anyone. A green
run after a merge closes only the `main-red` issue.

It never reverts anything. A revert is one command, printed in the issue, and
someone (or an agent) decides. Auto-merging a revert needs a token that can start
CI on the pull request it opens, which the built-in GITHUB_TOKEN cannot.

`--dry-run` prints what it would change and changes nothing; reads still happen.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field
from pathlib import Path

LABEL = "main-red"
LABEL_NIGHTLY = "main-red-nightly"
VERIFIED_REF = "verified"
VERIFIED_NIGHTLY_REF = "verified-nightly"
FLAKES_PATH = Path(__file__).with_name("known_flakes.json")

MAX_JOBS = 10  # failed jobs examined for evidence
LOG_TAIL_LINES = 300
EXCERPT_CHARS = 2700
EXCERPT_LINE_CHARS = 220  # a compiler's diagnostic can run to thousands of characters
EXCERPT_TAIL = 8  # lines before the marker that ends a failed step
EXCERPT_CAUSES = 3  # lines that name the cause, when it is further up than the tail
CAUSE_WINDOW = 250  # how far above the tail to look for them
MAX_SUSPECTS = 8
NO_BASELINE_LOOKBACK = 10

# The leg name (the last part of "Build & Test / Build (Linux) / Linux GCC") to the
# CMake preset that builds and tests it, so the issue can say how to reproduce.
PRESET_BY_LEG = {
    "Linux GCC": "linux-gcc",
    "Linux LLVM (clang)": "linux-llvm",
    "Linux GCC (arm64)": "linux-gcc-arm64",
    "Linux LLVM (arm64)": "linux-llvm-arm64",
    "Linux LLVM ASan+UBSan": "linux-llvm-asan-ubsan",
    "Linux LLVM TSan": "linux-llvm-tsan",
    "Windows MSVC": "windows-msvc",
    "Windows LLVM (clang-cl)": "windows-llvm",
    "Windows MSVC (arm64)": "windows-msvc-arm64",
    "macOS LLVM (Homebrew)": "macos-llvm",
    "macOS LLVM x64 (Homebrew, Intel)": "macos-llvm-x64",
}

MERGE_SUBJECT = re.compile(r"^Merge pull request #(\d+) from (\S+)")

# Lines that say what failed, in the output of the tools this repository builds and
# tests with: gcc and clang ("file:1:2: error: ..."), MSVC ("error C2065"), the linker,
# rustc, ninja and Catch2 ("FAILED:"), ctest, CMake, Python.
CAUSE_LINE = re.compile(
    r"(?:^|[\s:])(?:fatal )?error:"
    r"|\berror (?:C|LNK|CS|TS)\d+"
    r"|\berror\[E\d+\]"
    r"|\bFAILED: "
    r"|\*\*\*\s?Failed"
    r"|CMake Error"
    r"|Traceback \(most recent call last\)"
    r"|undefined reference to"
)
FAILED_LINE = re.compile(r"\bFAILED: ")
LOG_TIMESTAMP = re.compile(r"^\d{4}-\d\d-\d\dT[\d:.]+Z ")
ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")
ESCAPES_FLAG = "--allow-escape-sequences"

Runner = Callable[[Sequence[str], "str | None"], str]


class CommandError(Exception):
    def __init__(self, cmd: Sequence[str], returncode: int, stderr: str):
        super().__init__(f"{' '.join(cmd[:4])}... exited {returncode}: {stderr.strip()[:200]}")
        self.cmd = tuple(cmd)
        self.returncode = returncode
        self.stderr = stderr


def shell(cmd: Sequence[str], stdin: str | None = None) -> str:
    res = subprocess.run(
        list(cmd), input=stdin, capture_output=True, text=True, encoding="utf-8", check=False
    )
    if res.returncode != 0:
        raise CommandError(cmd, res.returncode, res.stderr)
    return res.stdout


@dataclass(frozen=True)
class Context:
    repo: str
    run_id: int
    head_sha: str
    conclusion: str
    attempt: int
    run_url: str
    dry_run: bool = False
    event: str = ""  # what started the run: push, schedule or workflow_dispatch


def nightly(ctx: Context) -> bool:
    """A scheduled run: the whole matrix, with its own baseline and its own issue."""
    return ctx.event == "schedule"


def label_of(ctx: Context) -> str:
    return LABEL_NIGHTLY if nightly(ctx) else LABEL


def baseline_ref_of(ctx: Context) -> str:
    return VERIFIED_NIGHTLY_REF if nightly(ctx) else VERIFIED_REF


@dataclass(frozen=True)
class Flake:
    id: str
    job: re.Pattern[str]
    pattern: re.Pattern[str]
    why: str


@dataclass
class FailedJob:
    id: int
    name: str
    url: str
    steps: list[str] = field(default_factory=list)
    evidence: str = ""
    flake: Flake | None = None


@dataclass(frozen=True)
class Suspect:
    sha: str
    pr: int | None
    subject: str
    branch: str


def load_flakes(path: Path = FLAKES_PATH) -> list[Flake]:
    return [
        Flake(e["id"], re.compile(e["job"]), re.compile(e["pattern"]), e["why"])
        for e in json.loads(path.read_text(encoding="utf-8"))
    ]


def match_flake(job_name: str, evidence: str, flakes: Sequence[Flake]) -> Flake | None:
    for flake in flakes:
        if flake.job.search(job_name) and flake.pattern.search(evidence):
            return flake
    return None


def decide(conclusion: str, attempt: int, jobs: Sequence[FailedJob]) -> str:
    """One of advance, ignore, rerun, report."""
    if conclusion == "success":
        return "advance"
    if conclusion in ("cancelled", "skipped", "stale", "neutral"):
        return "ignore"
    if attempt <= 1 and jobs and all(job.flake for job in jobs):
        return "rerun"
    return "report"


def parse_merge_subject(subject: str) -> tuple[int | None, str]:
    m = MERGE_SUBJECT.match(subject)
    return (int(m.group(1)), m.group(2)) if m else (None, "")


def run_marker(run_id: int) -> str:
    return f"<!-- main-health run:{run_id} -->"


def suspects(sh: Runner, base: str | None, head: str) -> tuple[list[Suspect], int]:
    """The merges in base..head, newest first, capped, and how many there were."""
    cmd = ["git", "log", "--first-parent", "--format=%H%x09%s"]
    if base:
        cmd.append(f"{base}..{head}")
    else:
        cmd += ["-n", str(NO_BASELINE_LOOKBACK), head]
    found: list[Suspect] = []
    for line in sh(cmd, None).splitlines():
        sha, _, subject = line.partition("\t")
        if not sha:
            continue
        pr, branch = parse_merge_subject(subject)
        found.append(Suspect(sha, pr, subject, branch))
    return found[:MAX_SUSPECTS], len(found)


def _cause_lines(lines: Sequence[str], lo: int, hi: int) -> list[int]:
    """Indexes in lines[lo:hi] that name the cause: the last failed command and what it
    printed, else the newest few. A test that checks a refusal prints "error:" too, so
    what is nearest the end of the step is the better guess."""
    hits = [i for i in range(lo, hi) if CAUSE_LINE.search(lines[i])]
    failed = [i for i in hits if FAILED_LINE.search(lines[i])]
    if failed:
        return [i for i in hits if i >= failed[-1]][:EXCERPT_CAUSES]
    return hits[-EXCERPT_CAUSES:]


def _clip(line: str) -> str:
    line = LOG_TIMESTAMP.sub("", line)
    return line if len(line) <= EXCERPT_LINE_CHARS else line[:EXCERPT_LINE_CHARS] + "..."


def excerpt(text: str) -> str:
    """The lines that say what went wrong.

    A failed step ends with an `##[error]` line, and what explains it is usually the
    output just before it. A compiler's diagnostic is often further up, above the build
    tool's own "stopped" lines and the progress lines of the jobs that were still
    running, so the lines that name a cause are added ahead of that tail. Matching every
    line that contains the word "error" picks up expected output; it is only the
    fallback when the log has no marker, and then only the newest few.
    """
    lines = text.splitlines()
    marks = [i for i, ln in enumerate(lines) if "##[error]" in ln]
    if marks:
        end = marks[-1]
        first_tail = max(0, end - EXCERPT_TAIL)
        causes = _cause_lines(lines, max(0, first_tail - CAUSE_WINDOW), first_tail)
        chosen = [lines[i] for i in causes]
        if chosen:
            chosen.append("...")
        chosen += lines[first_tail : end + 1]
    else:
        picked = [ln for ln in lines if re.search(r"\berror\b", ln, re.I)]
        chosen = picked[-12:] if picked else lines[-15:]
    return "\n".join(_clip(ln) for ln in chosen)[-EXCERPT_CHARS:]


def _leg(name: str) -> str:
    return name.rsplit(" / ", maxsplit=1)[-1].strip()


def render_issue_body(
    ctx: Context,
    jobs: Sequence[FailedJob],
    found: Sequence[Suspect],
    total: int,
    baseline: str | None,
) -> str:
    short = ctx.head_sha[:8]
    if nightly(ctx):
        lead = (
            f"The nightly run of `main` failed at `{short}`: [run {ctx.run_id}]({ctx.run_url}). "
            "It runs every leg, including the ones the run after a merge leaves out."
        )
    else:
        lead = (
            f"Post-merge verification of `main` failed at `{short}`: "
            f"[run {ctx.run_id}]({ctx.run_url})."
        )
    out = [lead, "", "### Failed jobs", ""]
    if not jobs:
        out.append("No failed job was found: the run failed outside a job (a workflow error).")
    for j in jobs:
        steps = ", ".join(f"`{s}`" for s in j.steps) or "no step recorded"
        out.append(f"- [{j.name}]({j.url}) - failed at {steps}")
        if j.evidence:
            out += [
                "",
                "  ```",
                *[f"  {ln}" for ln in excerpt(j.evidence).splitlines()],
                "  ```",
                "",
            ]
    out += ["", "### Merges in the range", ""]
    if baseline and nightly(ctx):
        out.append(
            f"Everything merged after the last commit the nightly run verified, `{baseline[:8]}`:"
        )
    elif baseline:
        out.append(f"Everything merged after the last verified commit `{baseline[:8]}`:")
    elif nightly(ctx):
        out.append(
            "The nightly run has not verified a commit yet, so these are only the newest "
            "merges, not a proven range:"
        )
    else:
        out.append(
            "There is no verified commit yet, so these are only the newest merges, "
            "not a proven range:"
        )
    out.append("")
    for s in found:
        who = f"#{s.pr} (`{s.branch}`)" if s.pr else s.subject
        out.append(f"- {who} - `{s.sha[:8]}`")
    if total > len(found):
        out.append(f"- ... and {total - len(found)} more")

    out += ["", "### Reproduce", ""]
    legs = [(j, PRESET_BY_LEG.get(_leg(j.name))) for j in jobs]
    shown = False
    for j, preset in legs:
        if preset:
            shown = True
            out.append(
                f"- {_leg(j.name)}: `cmake --preset config-{preset} && "
                f"cmake --build --preset build-{preset} && ctest --preset test-{preset}`"
            )
    if not shown:
        out.append("Open the failed job above; its log has the command that failed.")

    out += ["", "### Remediate", ""]
    if len(found) == 1 and total == 1:
        s = found[0]
        flag = "-m 1 " if s.subject.startswith("Merge ") else ""
        out.append(f"One merge is in the range, so it is the cause: `git revert {flag}{s.sha}`.")
    else:
        good = baseline or "<last good commit>"
        out.append(
            f"{total} merges are in the range. Find the one: "
            f"`git bisect start {ctx.head_sha} {good}`, "
            "then run the failing job's command at each step."
        )
    out += ["", run_marker(ctx.run_id)]
    return "\n".join(out)


def _mutate(ctx: Context, sh: Runner, cmd: Sequence[str], stdin: str | None = None) -> str:
    if ctx.dry_run:
        print(f"[dry-run] {' '.join(cmd)}")
        if stdin:
            print("\n".join(f"[dry-run]   {ln}" for ln in stdin.splitlines()))
        return ""
    return sh(cmd, stdin)


def failed_jobs(ctx: Context, sh: Runner) -> list[FailedJob]:
    jq = (
        '.jobs[] | select(.conclusion == "failure") | '
        '{id, name, html_url, steps: [.steps[] | select(.conclusion == "failure") | .name]}'
    )
    raw = sh(
        [
            "gh", "api", f"repos/{ctx.repo}/actions/runs/{ctx.run_id}/jobs",
            "--method", "GET", "-f", "filter=latest", "-f", "per_page=100",
            "--paginate", "--jq", jq,
        ],
        None,
    )  # fmt: skip
    jobs = []
    for line in raw.splitlines():
        if not line.strip():
            continue
        d = json.loads(line)
        if _leg(d["name"]) in ("Verify Status", "CI Status"):
            continue  # an aggregator fails because something else did
        jobs.append(FailedJob(d["id"], d["name"], d["html_url"], d.get("steps", [])))
    return jobs[:MAX_JOBS]


def read_text(sh: Runner, cmd: Sequence[str]) -> str:
    """Run a gh command whose output is text from a job. A newer gh refuses to print a
    response that holds terminal escape sequences, which a log with colour codes does,
    unless it is told to; an older one does not know the flag and does not need it.
    The escape sequences are dropped, so the excerpt and the signatures see plain text."""
    try:
        out = sh(cmd, None)
    except CommandError as e:
        if ESCAPES_FLAG not in e.stderr:
            raise
        out = sh([*cmd, ESCAPES_FLAG], None)
    return ANSI_ESCAPE.sub("", out)


def gather_evidence(ctx: Context, sh: Runner, jobs: Sequence[FailedJob], flakes) -> None:
    for j in jobs:
        parts = []
        # The annotation is what a lost runner leaves behind: its log is usually gone.
        note = "annotations", f"repos/{ctx.repo}/check-runs/{j.id}/annotations"
        log = "log", f"repos/{ctx.repo}/actions/jobs/{j.id}/logs"
        for what, path in (note, log):
            cmd = ["gh", "api", path] + (["--jq", ".[].message"] if what == "annotations" else [])
            try:
                out = read_text(sh, cmd)
            except CommandError as e:
                # Say so, in the report and in this run's log: a report built from the
                # annotations alone ("Process completed with exit code 1") names no cause.
                reason = (e.stderr.strip().splitlines() or [str(e)])[0][:160]
                parts.append(f"(main-health could not read the {what} of this job: {reason})")
                trouble = f"could not read the {what} of {j.name}: {reason}"
                print(f"::warning title=main-health::{trouble}")
                continue
            parts.append("\n".join(out.splitlines()[-LOG_TAIL_LINES:]))
        j.evidence = "\n".join(parts)
        j.flake = match_flake(j.name, j.evidence, flakes)


def is_ancestor(sh: Runner, older: str, newer: str) -> bool:
    try:
        sh(["git", "merge-base", "--is-ancestor", older, newer], None)
    except CommandError as e:
        if e.returncode == 1:
            return False
        raise
    return True


def verified_sha(sh: Runner, ref: str = VERIFIED_REF) -> str | None:
    try:
        out = sh(["git", "rev-parse", "--verify", "--quiet", f"refs/remotes/origin/{ref}"], None)
    except CommandError:
        return None
    return out.strip() or None


def open_issue(ctx: Context, sh: Runner, label: str = LABEL) -> int | None:
    out = sh(
        ["gh", "issue", "list", "-R", ctx.repo, "--label", label, "--state", "open",
         "--json", "number", "--limit", "1"],
        None,
    )  # fmt: skip
    rows = json.loads(out or "[]")
    return int(rows[0]["number"]) if rows else None


def advance(ctx: Context, sh: Runner) -> None:
    # A green nightly run is the whole matrix, so it proves what the run after a merge
    # proves and more: it moves both refs and closes both issues. The run after a merge
    # proves only its own tier.
    refs = [VERIFIED_REF, VERIFIED_NIGHTLY_REF] if nightly(ctx) else [VERIFIED_REF]
    labels = [LABEL, LABEL_NIGHTLY] if nightly(ctx) else [LABEL]
    for ref in refs:
        current = verified_sha(sh, ref)
        if current is None or is_ancestor(sh, current, ctx.head_sha):
            # A plain push, never forced: if the ref has moved past this commit (a
            # rerun finishing late), the remote refuses and nothing is lost.
            try:
                _mutate(ctx, sh, ["git", "push", "origin", f"{ctx.head_sha}:refs/heads/{ref}"])
            except CommandError as e:
                print(f"::notice::{ref} not advanced: {e}")
    for label in labels:
        issue = open_issue(ctx, sh, label)
        if issue is not None:
            note = (
                f"main is green again: verified at `{ctx.head_sha[:8]}` "
                f"([run {ctx.run_id}]({ctx.run_url}))."
            )
            _mutate(
                ctx, sh, ["gh", "issue", "close", str(issue), "-R", ctx.repo, "--comment", note]
            )


def report(ctx: Context, sh: Runner, jobs: Sequence[FailedJob]) -> None:
    baseline = verified_sha(sh, baseline_ref_of(ctx))
    found, total = suspects(sh, baseline, ctx.head_sha)
    body = render_issue_body(ctx, jobs, found, total, baseline)
    label = label_of(ctx)
    issue = open_issue(ctx, sh, label)
    if issue is None:
        described = (
            "the nightly run of main failed"
            if nightly(ctx)
            else "main failed post-merge verification"
        )
        _mutate(
            ctx, sh,
            ["gh", "label", "create", label, "-R", ctx.repo, "--color", "B60205", "--force",
             "--description", described],
        )  # fmt: skip
        first = jobs[0].name if jobs else "the run failed outside a job"
        more = f" and {len(jobs) - 1} more" if len(jobs) > 1 else ""
        what = "the nightly run is red" if nightly(ctx) else "main is red"
        _mutate(
            ctx, sh,
            ["gh", "issue", "create", "-R", ctx.repo, "--title",
             f"{what} at {ctx.head_sha[:8]}: {_leg(first)}{more}",
             "--label", label, "--body-file", "-"],
            body,
        )  # fmt: skip
    else:
        _mutate(
            ctx,
            sh,
            ["gh", "issue", "comment", str(issue), "-R", ctx.repo, "--body-file", "-"],
            body,
        )
    if nightly(ctx):
        return  # a day of merges is too wide a range to name anyone; the issue lists them
    tracking = f"#{issue}" if issue is not None else "the main-red issue"
    for s in found:
        if s.pr is None:
            continue
        seen = sh(
            [
                "gh",
                "api",
                f"repos/{ctx.repo}/issues/{s.pr}/comments",
                "--paginate",
                "--jq",
                ".[].body",
            ],
            None,
        )
        if run_marker(ctx.run_id) in seen:
            continue
        names = ", ".join(_leg(j.name) for j in jobs[:4]) or "a workflow error"
        note = (
            f"Post-merge verification of `main` failed at `{ctx.head_sha[:8]}` "
            f"([run {ctx.run_id}]({ctx.run_url})): {names}. This pull request merged in the range "
            f"since the last verified commit, so it is a suspect (tracking: {tracking}). "
            "If the failure is yours, the reproduction is in that issue.\n\n"
            f"{run_marker(ctx.run_id)}"
        )
        _mutate(
            ctx, sh, ["gh", "pr", "comment", str(s.pr), "-R", ctx.repo, "--body-file", "-"], note
        )


def health(ctx: Context, sh: Runner) -> str:
    if ctx.conclusion in ("cancelled", "skipped", "stale", "neutral"):
        return "ignore"
    if ctx.conclusion == "success":
        advance(ctx, sh)
        return "advance"
    jobs = failed_jobs(ctx, sh)
    gather_evidence(ctx, sh, jobs, load_flakes())
    action = decide(ctx.conclusion, ctx.attempt, jobs)
    if action == "rerun":
        _mutate(ctx, sh, ["gh", "run", "rerun", str(ctx.run_id), "--failed", "-R", ctx.repo])
    elif action == "report":
        report(ctx, sh, jobs)
    return action


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument("--repo", required=True)
    p.add_argument("--run-id", type=int, required=True)
    p.add_argument("--head-sha", required=True)
    p.add_argument("--conclusion", required=True)
    p.add_argument("--attempt", type=int, default=1)
    p.add_argument("--run-url", required=True)
    p.add_argument("--event", default="", help="what started the run (schedule is the nightly run)")
    p.add_argument("--dry-run", action="store_true")
    return p.parse_args(argv)


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv[1:])
    ctx = Context(
        repo=args.repo,
        run_id=args.run_id,
        head_sha=args.head_sha,
        conclusion=args.conclusion,
        attempt=args.attempt,
        run_url=args.run_url,
        dry_run=args.dry_run or os.environ.get("MAIN_HEALTH_DRY_RUN") == "1",
        event=args.event,
    )
    action = health(ctx, shell)
    print(f"main-health: {action} (run {ctx.run_id}, {ctx.conclusion}, attempt {ctx.attempt})")
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as f:
            f.write(f"main-health: **{action}** for run {ctx.run_id} ({ctx.conclusion}).\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
