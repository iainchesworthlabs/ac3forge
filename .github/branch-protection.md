# Branch protection for `main` and `develop`

*Maintainer notes — repo administration; not published on the docs site.*

GitHub branch/repo security settings can't be expressed as a workflow file -
they're applied in **Settings → Branches** (or **Settings → Rules → Rulesets**)
by someone with admin rights on the repo. Configure a protection rule (or
ruleset) for `main` with:

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

`develop` is where `feature/*`/`bugfix/*` work actually lands and where
Dependabot opens its PRs (`.github/dependabot.yml` targets `develop`, not
`main`), so give it the same rule with the same required checks - that's
where most vulnerable dependencies or CI regressions would actually be
introduced, well before a release PR ever reaches `main`.

## Merge queue (`develop` only)

With many topic branches open against `develop` at once, "require branches
to be up to date before merging" turns into a rebase treadmill: every merge
invalidates every other open PR's up-to-date status, forcing a fresh rebase
and a full CI re-run before the next one can land. A repository ruleset
(`merge-queue-develop`, `target: branch`, `conditions.ref_name.include:
refs/heads/develop`, one `merge_queue` rule) fixes this the way GitHub
intends: PRs enter the queue once their own checks and review pass, GitHub
merges each entry against the current queue tip server-side and re-runs the
required checks against that up-to-date state automatically, then merges
when green - no manual rebase-and-rerun. `main` doesn't get one: it only
receives rare, single release-promotion merges from `develop`, none of the
concurrent-PR churn this solves.

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

**Every workflow that produces one of `develop`'s required status checks
must also trigger on the `merge_group` event**, not just `push`/
`pull_request` - GitHub only runs workflows that opt into `merge_group` on
the queue's temporary `gh-readonly-queue/develop/...` ref, so a workflow
missing that trigger never reports its check there and every queue entry
sits until `check_response_timeout_minutes` expires. `ci.yml`, `codeql.yml`,
and `dependency-review.yml` all carry it (see each workflow's own
`merge_group` comment) - add it to anything else that later becomes a
required check on `develop`.

Since `main` only receives merges from `develop`, `release/*`, `hotfix/*` and
`support/*` branches under this project's gitflow model (see
`CONTRIBUTING.md` and the `branch-name` job in `ci.yml`), you may also want a
rule restricting which branches can open PRs against `main` - GitHub
rulesets support this directly (`main` ruleset → target branch pattern
restrictions), whereas classic branch protection does not; the `branch-name`
job enforces the naming convention as a required status check either way.

## Other scanners (visible-only)

`osv-scanner.yml`, `zizmor.yml` and `scorecard.yml` upload SARIF to
**Security → Code scanning** but don't fail PR checks - triage their alerts
there rather than via a required status check (see each workflow's header
comment for why). `msvc-analysis.yml` (MSVC Code Analysis, `/analyze`) is
the same shape and runs on PRs to `main`/`develop` too (docs-only changes
skipped): its findings land in code scanning for triage, not in a required
status check. `scorecard.yml`'s branch-protection sub-check scores more
completely with a fine-grained PAT (read-only, "Administration: read") added
as a repo secret named `SCORECARD_READ_TOKEN`; without it, that one
sub-check just degrades gracefully instead of failing.

## Dependabot auto-merge

`dependabot-auto-merge.yml` only flips the auto-merge bit on a Dependabot PR
(non-major bumps only); GitHub still won't merge it until every required
check above passes. It needs no extra configuration beyond the branch
protection rule itself - once `Branch Name`, `CI Status` and
`Scan dependency diff` are required on `develop`, auto-merge is safe to
enable repo-wide in **Settings → General → Pull Requests → Allow auto-merge**.
