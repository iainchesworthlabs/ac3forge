# Branch protection for `main`

*Maintainer notes — repo administration; not published on the docs site.*

Trunk-based development: `main` is the only long-lived branch. Every topic branch
(`feature/*`, `bugfix/*`, Dependabot's `dependabot/**`) targets it directly and merges
straight there — there is no separate integration branch, no promotion PR, and no sync-back
step. Releases are tags cut from `main` (see `docs/releasing.md`).

GitHub branch/repo security settings can't be expressed as a workflow file - they're applied
in **Settings → Branches** (or **Settings → Rules → Rulesets**) by someone with admin rights
on the repo. Configure a protection rule (or ruleset) for `main` with:

- **Require a pull request before merging**
  - Required approving review count: **0**
  - Dismiss stale approvals when new commits are pushed

  Zero, not one: this is a solo-maintainer repo, and GitHub does not count an
  author's own approval toward their own PR, so "require 1 approval" was
  unsatisfiable through the normal merge button - every PR in this repo's
  history has landed via `gh pr merge --admin`, bypassing the requirement
  rather than meeting it. Dropping the count to 0 keeps "require a pull
  request before merging" itself (still blocks direct pushes, still requires
  every required status check below to pass, still dismisses stale approvals
  if a second maintainer ever does leave one) while letting a green PR merge
  through the normal button instead of only through an admin override.
- **Require status checks to pass before merging**, selecting:
  - `Branch Name` (from `ci.yml`)
  - `CI Status` (from `ci.yml` - aggregates every required CI job; add or
    rename a matrix leg without ever touching this rule)
  - `Scan dependency diff` (from `dependency-review.yml`) - fails on a
    moderate-or-worse known vulnerability newly introduced by the PR
    (`vcpkg.json` or a GitHub Actions dependency)

  `No Quarantine On Main` is not selected in its own right - it sits in
  `CI Status`'s `needs` list, so it still gates every merge through that one
  aggregate check. `Analyze (C++)` was removed 2026-08-31: `codeql.yml`'s PR
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

### What the 2026-08 CI additions did and did not change here

Nothing in the `VX14`-`VX17` batch (script lint, the `apps/cli` coverage floor,
the ThreadSanitizer leg, the PR-time performance comparison) **requires** a
ruleset edit, and the list above is deliberately unchanged:

- `Script Lint` (`ci.yml`) is in `CI Status`'s `needs` list, so it already
  gates through the required check that exists. Selecting it as a required
  check in its own right is optional - it would only make a lint failure name
  itself in the merge box rather than showing up as `CI Status` failing.
- `Linux LLVM TSan` is a `_build.yml` matrix leg, and `CI Status` covers the
  whole matrix by design - that is what the parenthetical above means.
- `Performance vs merge base` (`ci.yml`) must NOT be made required. It is
  informational, carries `continue-on-error`, and is deliberately absent from
  `CI Status`'s `needs`; requiring it would turn hosted-runner timing noise
  into a merge blocker.
- `codeql.yml` is nightly-only since 2026-09 and no check-name constraint
  remains on it (its legs never report on a PR). Keep `CI Status`'s own
  `name:` stable - that rendered string is what the required check above is
  selected by, and renaming it leaves every PR pending until an admin edits
  the rule.
- `Python coverage` (`wheels.yml`) is a new check on a workflow that has no
  required checks today; leaving it that way is consistent with `Build wheels`.

Ruleset edits are the repository admin's, not a pull request's. If any of the
optional checks above are wanted as required ones, add them by their exact
names as rendered here.

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
ALLGREEN`, `max_entries_to_build: 4` (raised from 2 on 2026-08-28 when the
self-hosted fleet grew to 13 Linux / 7 Windows runners shared org-wide, see
`docs/ci-self-hosted-runners.md`; GitHub-hosted concurrency is still capped
at 20 jobs account-wide on this org's Free plan, and building more queue
entries at once than the two pools can bear just adds to the same backlog
the queue is meant to relieve), `max_entries_to_merge: 5`,
`min_entries_to_merge: 1`, `min_entries_to_merge_wait_minutes: 5`,
`check_response_timeout_minutes: 180` (raised from 60 on the same date: a
queue entry's matrix legs can wait more than an hour for a fleet slot under
load, and the default timed entries out before their checks reported). Re-tune
`max_entries_to_build` if the fleet changes size or the account moves off
the Free tier.

**The queue alone does not fix a genuinely oversubscribed account.** On
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
`check_response_timeout_minutes` expires. `ci.yml` and
`dependency-review.yml` carry it (see each workflow's own `merge_group`
comment) - add it to anything else that later becomes a required check on
`main`. The converse also holds: a workflow that produces no required check
must NOT carry `merge_group`, or every queue entry burns a run of it for
nothing - which is why `codeql.yml` and `msvc-analysis.yml` lost theirs in
2026-09.

## Code-scanning gate (ruleset, deleted)

A repository ruleset `code-scanning-gate-main` (`target: branch`,
`refs/heads/main`, one `code_scanning` rule: PREfast at
`errors_and_warnings`, CodeQL at `errors` alerts / `high_or_higher` security
alerts) was created 2026-08-24 to block merges on new scanner findings,
**disabled** on 2026-08-31 and **deleted** by the owner in 2026-09 when the
analysis workflows moved to a nightly schedule. Why it was disabled: a
`code_scanning` rule waits for every analysis category the target branch has
previously seen, and `main` carries four CodeQL categories - `cpp`,
`python`, `javascript-typescript` and `java-kotlin` - of which the last is
produced only by `_build.yml`'s `build-android` job, which `ci.yml` gates
behind `changes.outputs.code == 'true'`; `codeql.yml` and
`msvc-analysis.yml` also `paths-ignore`d docs at the time. A docs-only PR
therefore could never satisfy the rule and sat un-mergeable forever: no
docs-only PR merged between the ruleset's creation and its disabling.

Why it must not come back: since 2026-09 the `cpp`, `python` and
`javascript-typescript` CodeQL categories and the PREfast analysis are
produced only by nightly runs on `refs/heads/main`, never on a PR merge
commit or a merge-queue ref. A `code_scanning` rule would wait for an
analysis of the PR merge commit in every category `main` has ever seen, so
re-creating it would block every PR - docs-only or not - on "Code scanning
is waiting for results" indefinitely. If a merge-time analysis gate is ever
wanted again, it has to come with per-PR analysis in every category, which
this repo has deliberately moved away from.

## Nightly analysis and other visible-only scanners

`codeql.yml` and `msvc-analysis.yml` (MSVC Code Analysis, `/analyze`) run
nightly against `main` - 02:17 and 02:23 UTC, see
`docs/ci-self-hosted-runners.md` "Nightly analysis window" - and neither
reports on a PR at all. Their alerts land in **Security → Code scanning**
against `refs/heads/main`; because nothing reliably notifies anyone about a
new default-branch alert, each workflow's `surface` job fails on alerts created
since the previous nightly and opens or refreshes a `nightly-analysis`
issue (one per engine, via `.github/actions/report-nightly-failure`). Close
the issue once the findings are fixed or dismissed with a justification.

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
