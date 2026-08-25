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
  - Require at least 1 approval
  - Dismiss stale approvals when new commits are pushed
- **Require status checks to pass before merging** (enable "Require branches
  to be up to date" too), selecting:
  - `Branch Name` (from `ci.yml`)
  - `No Quarantine On Main` (from `ci.yml`)
  - `CI Status` (from `ci.yml` - aggregates every required CI job; add or
    rename a matrix leg without ever touching this rule)
  - `Analyze (C++)` (from `codeql.yml`)
  - `Scan dependency diff` (from `dependency-review.yml`) - fails on a
    moderate-or-worse known vulnerability newly introduced by the PR
    (`vcpkg.json` or a GitHub Actions dependency)
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
- `codeql.yml` became a language matrix, but its C++ leg is still named
  `Analyze (C++)` exactly - the job's `name:` interpolates a `display` value
  chosen for that reason, since a rename would leave the required check above
  pending forever. The two new legs report as `Analyze (Python)` and
  `Analyze (JavaScript)`; adding them as required checks is optional, and
  matches how the existing CodeQL leg is treated.
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
ALLGREEN`, `max_entries_to_build: 2` (deliberately low - self-hosted
capacity is 3 Linux/2 Windows runners shared org-wide, see
`docs/ci-self-hosted-runners.md`, and GitHub-hosted concurrency is capped at
20 jobs account-wide on this org's Free plan; building more queue entries at
once than that can bear just adds to the same backlog it's meant to
relieve), `max_entries_to_merge: 5`, `min_entries_to_merge: 1`,
`min_entries_to_merge_wait_minutes: 5`. Re-tune `max_entries_to_build` up if
the self-hosted fleet grows or the account moves off the Free tier.

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
`check_response_timeout_minutes` expires. `ci.yml`, `codeql.yml`, and
`dependency-review.yml` all carry it (see each workflow's own `merge_group`
comment) - add it to anything else that later becomes a required check on
`main`.

## Other scanners (visible-only)

`osv-scanner.yml`, `zizmor.yml` and `scorecard.yml` upload SARIF to
**Security → Code scanning** but don't fail PR checks - triage their alerts
there rather than via a required status check (see each workflow's header
comment for why). `msvc-analysis.yml` (MSVC Code Analysis, `/analyze`) is
the same shape and runs on PRs to `main` too (docs-only changes skipped):
its findings land in code scanning for triage, not in a required status
check. `scorecard.yml`'s branch-protection sub-check scores more completely
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
