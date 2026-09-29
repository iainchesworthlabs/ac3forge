# Branch protection for `main`

*Maintainer notes — repo administration; not published on the docs site.*

Trunk-based development (GitHub Flow): `main` is the only long-lived branch. Every topic
branch (`feature/*`, `bugfix/*`, `hotfix/*`, `docs/*`, `chore/*`, Dependabot's
`dependabot/**`) targets it directly and merges straight there — there is no separate
integration branch, no promotion PR, and no sync-back step. Releases are tags cut from
`main` (see `docs/releasing.md`). Short names are lowercase kebab-case; see
`CONTRIBUTING.md`.

GitHub branch/repo security settings can't be expressed as a workflow file - they're applied
in **Settings → Branches** (or **Settings → Rules → Rulesets**) by someone with admin rights
on the repo. Configure a protection rule (or ruleset) for `main` with:

- **Require a pull request before merging**
  - Required approving review count: **0**
  - Dismiss stale approvals when new commits are pushed

  Zero, not one: this is a solo-maintainer repo, and GitHub does not count an
  author's own approval toward their own PR, so "require 1 approval" was
  unsatisfiable through the normal merge button - PRs had to land via
  `gh pr merge --admin`, bypassing the requirement rather than meeting it.
  Dropping the count to 0 keeps "require a pull
  request before merging" itself (still blocks direct pushes, still requires
  every required status check below to pass, still dismisses stale approvals
  if a second maintainer ever does leave one) while letting a green PR merge
  through the normal button instead of only through an admin override.
- **Require status checks to pass before merging**, selecting:
  - `Branch Name` (from `pr-gate.yml`)
  - `CI Status` (from `pr-gate.yml` - aggregates the gate's jobs and fails
    closed: a job the plan asked for that was skipped is a failure)
  - `Scan dependency diff` (from `dependency-review.yml`) - fails on a
    moderate-or-worse known vulnerability newly introduced by the PR
    (`vcpkg.json` or a GitHub Actions dependency)

  **Since 2026-09-29 these come from `pr-gate.yml`, not `ci.yml`.** A pull
  request and each merge-queue entry run the gate: the static checks (one job,
  `_static.yml`), Linux GCC (with the Qt GUI when the change touches it, and
  always in the queue), and in the queue Windows MSVC. `ci.yml` runs after the
  merge, one run at a time: the legs a merge can break, and nightly and on
  request the whole matrix. Its aggregate is named `Verify Status` so it can
  never be mistaken for the required check on a commit both ran on. The names of
  the three required checks did not change, so this needed no rule edit. See
  [CI for many agents](../docs/ci-agentic.md).

  The quarantine check (no `src/quarantine` in the tree) is not selected in its
  own right - it is a step of the gate's static job, so it still gates every
  merge through `CI Status`. `Analyze (C++)` was removed 2026-08-31: `codeql.yml`'s PR
  trigger then `paths-ignore`d `docs/**`/`**/*.md`, so on a docs-only PR the
  required context never reported and the PR sat green-but-BLOCKED forever (the
  code-scanning ruleset section below records the fuller version of the same
  trap). Since 2026-09 `codeql.yml` has no PR trigger at all - see
  [Nightly analysis and other visible-only scanners](#nightly-analysis-and-other-visible-only-scanners)
  below. "Require branches to be up to date" is off: the merge queue below
  makes each entry up to date server-side, without the rebase treadmill that
  setting used to cause.
- **Require conversation resolution before merging**
- **Do not allow bypassing the above settings** (applies rules to admins too)
- **Restrict who can push to matching branches** - only allow merges via PR;
  block direct pushes
- **Block force pushes**
- **Restrict deletions**

On 2026-09-30 the rule on `main` carries the three required checks, zero
required approvals with stale approvals dismissed, and blocks force pushes and
deletions. It does not require conversation resolution and is not enforced for
administrators (`enforce_admins` is off, and the `merge-queue-main` ruleset
lists the repository admin role as an always-bypass actor), and its only push
restriction is the pull-request requirement. Turning the first two on is the
repository admin's decision.

### Which jobs feed `CI Status`

`CI Status` needs the gate's own jobs and nothing else: `Plan`, `Branch Name`,
`Static checks`, `Toolchain versions`, `Linux GCC` and, when the plan or the
queue asks for it, `Windows MSVC`. It fails closed: a job the plan asked for
that was skipped is a failure. The static job (`_static.yml`) holds the
lint, the oracle scripts' unit tests, the documentation path check, the
generated support matrices, the packaging, fixture and quarantine checks and
patch attribution, so all of them gate every merge through `CI Status`.

Nothing in `ci.yml` is a member: the post-merge legs, the nightly matrix, the
coverage, the ABI gate, `Python coverage`, `Build wheels` (`wheels.yml`),
`Build and test` (`npm.yml`) and `Pack and verify` (`esp-component.yml`) run
after the merge and cannot block one. A failure there reaches
`main-health.yml`, which opens a `main-red` issue naming the merges since the
`verified` branch. None of that is a required check.

Two rules from earlier arrangements still hold:

- Keep `CI Status`'s own `name:` stable. That rendered string is what the
  required check above is selected by, and renaming it leaves every PR pending
  until an admin edits the rule.
- `performance-compare` and `memory-compare` (`_ci-core.yml`, the PR-time
  comparisons with their `perf-regression-approved` and
  `memory-regression-approved` labels) have not run since the gate replaced
  `ci.yml` on pull requests. If they return, they must not be made required:
  they are informational and carry `continue-on-error`, so requiring them would
  turn hosted-runner timing noise into a merge blocker. The trend jobs after
  the merge fail at the same +100% thresholds instead.

Ruleset edits are the repository admin's, not a pull request's. If any check
is wanted as a required one, add it by its exact name as rendered.

## Merge queue

With many topic branches open against `main` at once, "require branches to be up to date
before merging" turns into a rebase treadmill: every merge invalidates every other open PR's
up-to-date status, forcing a fresh rebase and a full CI re-run before the next one can land -
this is exactly what happened during the 2026-08-24 concurrent-PR push under the old
`develop`-as-integration-branch model, where PRs needed repeated rounds of rebase/re-run before
landing. A repository ruleset (`merge-queue-main`, `target: branch`,
`conditions.ref_name.include: refs/heads/main`, one `merge_queue` rule) fixes this the way
GitHub intends: PRs enter the queue once their own checks and review pass, GitHub merges each
entry against the current queue tip server-side and re-runs the required checks against that
up-to-date state automatically, then merges when green - no manual rebase-and-rerun.

Configured `merge_queue` rule parameters: `merge_method: MERGE` (matches
this repo's real-merge-commit convention, not squash), `grouping_strategy:
ALLGREEN`, `max_entries_to_build: 8` (2 to begin with, raised to 4 on
2026-08-28 when the self-hosted fleet grew to 13 Linux / 7 Windows runners
shared org-wide, see `docs/ci-self-hosted-runners.md`, and to 8 by the
ruleset's last edit, on 2026-09-12; GitHub-hosted concurrency is still capped
at 20 jobs account-wide on this org's Free plan, and the gate's jobs run on
hosted runners, so building more queue entries at once than that pool can
bear just adds to the same backlog the queue is meant to relieve),
`max_entries_to_merge: 5`,
`min_entries_to_merge: 1`, `min_entries_to_merge_wait_minutes: 5`,
`check_response_timeout_minutes: 180` (raised from 60 on the same date: a
queue entry's matrix legs can wait more than an hour for a fleet slot under
load, and the default timed entries out before their checks reported). Re-tune
`max_entries_to_build` if the fleet changes size or the account moves off
the Free tier.

**The queue alone does not fix an oversubscribed account.** On
2026-08-24, ~30 topic branches were open and pushing at once; even with only
2 entries building at a time, each PR's *own* pre-queue `pull_request` CI run
still competed for the same ~20-job account-wide ceiling and 3/2-runner
self-hosted fleet, so hundreds of job requests queued behind a handful of
running slots regardless of the queue's throttling. The queue serializes the
*merge* step; it does not - and cannot - create more CI capacity. Keep the
number of topic branches actively pushing at once roughly within what the
fleet above can run concurrently; a burst larger than that will still back
up no matter how the branches are named or which branch they target.

**Every workflow that produces one of `main`'s required status checks must
also trigger on the `merge_group` event**, not just `push`/`pull_request` -
GitHub only runs workflows that opt into `merge_group` on the queue's
temporary `gh-readonly-queue/main/...` ref, so a workflow missing that
trigger never reports its check there and every queue entry sits until
`check_response_timeout_minutes` expires. `pr-gate.yml` and
`dependency-review.yml` carry it (see each workflow's own `merge_group`
comment) - add it to anything else that later becomes a required check on
`main`. The converse also holds: a workflow that produces no required check
must NOT carry `merge_group`, or every queue entry burns a run of it for
nothing - which is why `codeql.yml` and `msvc-analysis.yml` lost theirs in
2026-09, and why `ci.yml` lost its on 2026-09-29 when the gate took over.

With a gate that finishes in about a quarter of an hour,
`min_entries_to_merge_wait_minutes` (5) is a large share of each merge's
latency, and `check_response_timeout_minutes` (180) is far longer than a
healthy entry needs. Lowering the first to 1 and the second to 60 are ruleset
edits for the repository admin, and both are still at 5 and 180.

## Code-scanning gate (ruleset, disabled)

A repository ruleset `code-scanning-gate-main` (`target: branch`,
`refs/heads/main`, one `code_scanning` rule: PREfast at
`errors_and_warnings`, CodeQL at `errors` alerts / `high_or_higher` security
alerts) was created 2026-08-24 to block merges on new scanner findings and
**disabled** on 2026-08-31. It still exists in that state (as of 2026-09-30);
the analysis workflows moved to a nightly schedule in 2026-09. Why it was
disabled: a
`code_scanning` rule waits for every analysis category the target branch has
previously seen, and `main` carries four CodeQL categories - `cpp`,
`python`, `javascript-typescript` and `java-kotlin` - and at the time the
last of those was produced only by `_build.yml`'s `build-android` job,
which `ci.yml` gates behind `changes.outputs.code == 'true'`; `codeql.yml`
and `msvc-analysis.yml` also `paths-ignore`d docs then. A docs-only PR
therefore could never satisfy the rule and sat un-mergeable forever: no
docs-only PR merged between the ruleset's creation and its disabling.

Why it must stay disabled: since 2026-09 every CodeQL category - `cpp`,
`python`, `javascript-typescript` and `java-kotlin`, the last having moved
out of `build-android` into the nightly matrix - and the PREfast analysis
are produced only by nightly runs on `refs/heads/main`, never on a PR merge
commit or a merge-queue ref. A `code_scanning` rule would wait for an
analysis of the PR merge commit in every category `main` has ever seen, so
enabling it would block every PR - docs-only or not - on "Code scanning
is waiting for results" indefinitely. If a merge-time analysis gate is ever
wanted again, it has to come with per-PR analysis in every category, which
this repo has deliberately moved away from.

## Nightly analysis and other visible-only scanners

`codeql.yml`, `msvc-analysis.yml` (MSVC Code Analysis, `/analyze`),
`static-analysis.yml` (clang-tidy) and `sonarcloud.yml` run nightly against
`main` - cron times of 02:17 to 02:35 UTC, see `docs/ci-self-hosted-runners.md`
"Nightly analysis window"; GitHub starts scheduled workflows four to six and a
half hours after their cron time (see "The tiers" in
[CI for many agents](../docs/ci-agentic.md)) - and none of them reports on a PR
at all. The first three
put their alerts in **Security → Code scanning** against `refs/heads/main`;
because nothing reliably notifies anyone about a new default-branch alert,
each workflow fails on findings since the previous nightly and opens or
refreshes a `nightly-analysis` issue (one per engine, via
`.github/actions/report-nightly-failure`). Close the issue once the findings
are fixed or dismissed with a justification. SonarCloud is the exception to
the Security-tab part: its findings live in its own dashboard, and what fails
the job is its quality gate on new code.

`osv-scanner.yml`, `zizmor.yml` and `scorecard.yml` upload SARIF to
**Security → Code scanning** but don't fail PR checks - triage their alerts
there rather than via a required status check (see each workflow's header
comment for why). `scorecard.yml`'s branch-protection sub-check scores more completely
with a fine-grained PAT (read-only, "Administration: read") added as a repo
secret named `SCORECARD_READ_TOKEN`; without it, that one sub-check just
degrades gracefully instead of failing.

## Dependabot auto-merge

`dependabot-auto-merge.yml` only flips the auto-merge bit on a Dependabot PR
(non-major bumps only); GitHub still won't merge it until every required
check above passes. It needs no extra configuration beyond the branch
protection rule itself - once `Branch Name`, `CI Status` and
`Scan dependency diff` are required on `main`, auto-merge is safe to
enable repo-wide in **Settings → General → Pull Requests → Allow auto-merge**.
