# CI for many agents

*Maintainer notes - CI structure; not a build or contribution guide.*

Many agents open, update and merge pull requests here at the same time: about 26 merges a
day, from a few dozen branches. This page describes how CI is arranged for that load, what
runs at each stage, and what an agent should expect to see. The change classifier behind the
full matrix is in [CI lane partitions](ci-lanes.md), and the runner fleet is in
[Self-hosted CI runners](ci-self-hosted-runners.md).

## Why it changed

Measured over 3.5 days in September 2026, across 300 runs of `ci.yml` and about 15,000 jobs:

- One full run asked for 6 to 10 runner-hours. Every change paid for that three times: on each
  push to its pull request, in the merge queue, and again on main.
- About 1,750 runner-hours were requested, roughly 500 a day. 51% went into runs that were
  cancelled, 19% into runs that failed and 27% into runs that passed. In the failed runs, 57%
  of the compute ran after the first failing job had already finished.
- GitHub Free allows 20 hosted jobs at once, organisation-wide, and 5 for macOS. The macOS limit
  is the same on Free, Pro and Team; only Enterprise raises it. The self-hosted fleet was
  saturated too, and the AWS spot overflow sat at its cap of 10 instances for the whole period.
- A Linux GCC build with every ctest case would have caught 20 of the 28 failures that pull
  requests and queue entries actually had (71%). Of the other eight, four were Windows MSVC.
- Nothing enabled ccache, so every job compiled the whole tree, and ctest ran its 3,000 cases
  one at a time.

## The stages

| Stage | Runs | What | Where |
|---|---|---|---|
| Before a push | by hand | `python tools/ci/precheck.py`: the static checks that need no build, and the gate's plan for the diff | your machine |
| Pull request | every push to the branch | [`pr-gate.yml`](https://github.com/iainchesworthlabs/ac3forge/blob/main/.github/workflows/pr-gate.yml): static checks, then Linux GCC build, every ctest case and the gold-reference gate | GitHub-hosted |
| Merge queue | each queue entry | the same on the merged tree, with the Qt GUI always built, plus Windows MSVC | GitHub-hosted |
| After a merge | every push to main, one at a time | [`ci.yml`](https://github.com/iainchesworthlabs/ac3forge/blob/main/.github/workflows/ci.yml), tier `t2`: the legs and lanes a merge can break (see [The tiers](#the-tiers)), then [`main-health.yml`](https://github.com/iainchesworthlabs/ac3forge/blob/main/.github/workflows/main-health.yml) | the fleet, plus hosted for macOS, arm64 and the satellites |
| Nightly | about 19:47 UTC (05:47 in Sydney), and on request | `ci.yml`, tier `all`: every leg with every extra pass, every lane | the same |

A Linux gate cannot see another compiler, another operating system, an architecture, the
sanitizers or a QEMU board. The run after a merge covers the first three and the nightly run the
rest. The trade is deliberate: the gate finishes in minutes and stays cheap, and a failure only a
later stage finds is attributed to the merges that could have caused it.

## The pull-request gate

`pr-gate.yml` produces the required checks `Branch Name` and `CI Status`. `CI Status` fails if
any job the plan asked for did not succeed. A job that was skipped but was needed counts as a
failure, so a wiring mistake cannot turn it green.

A planner ([`tools/ci/plan_gate.py`](https://github.com/iainchesworthlabs/ac3forge/blob/main/tools/ci/plan_gate.py))
reads the changed files and decides:

- **Documentation only**: the static checks run, nothing is built.
- **Nothing a Linux C++ build reads** (`python/`, `rust/`, `js/`, `esp-idf/`, `esphome/`,
  `apps/android/`, `apps/wasm/`, `apps/baremetal/`, `packaging/`, other workflows, editor and
  lint configuration): the static checks run, nothing is built. Those lanes run after the merge.
- **Anything else builds Linux GCC**, and installs Qt and builds the GUI only when the change is
  in `apps/gui`, `apps/hearth`, `apps/crucible`, `apps/common`, their tests, `cmake/`, or the
  top-level CMake and vcpkg files. A path the planner does not recognise builds everything.
- **The gate's own files** (`pr-gate.yml`, `_static.yml`, `.github/actions/`, the toolchain
  scripts) build everything, because they are proven by running.

The static checks are one job, [`_static.yml`](https://github.com/iainchesworthlabs/ac3forge/blob/main/.github/workflows/_static.yml):
ruff, shellcheck, actionlint, the unit tests of the oracle scripts, the documentation path check,
packaging consistency, the fixture corpus, the quarantine check and patch attribution. Every
check runs even when an earlier one failed, so one run lists every failure.

The build goes through ccache and runs ctest in three phases. The Catch2 cases run in parallel.
The Qt Quick suites (`*_qml_tests_*`) then run in a phase of their own, `ctest-qml-jobs` at a time.
They drive a software-rendered window with mouse clicks and timed waits, so the default is one at
a time; nothing has shown that overlapping them breaks them, and the gate currently runs them at
the CPU count to find out, because that phase is the longest part of a warm run. The throughput
guards (label `Performance`) run alone last. A failing case is retried once, and a
case that fails and then passes is reported as a warning, since that can be two tests sharing a
resource. A parallel phase that still has failures runs them again one at a time: a test that
passes alone passes the phase, with a warning that names it, and one that fails alone is a
failure. That rule exists because a concurrency test whose threads had not started when its main
thread finished failed on every hosted Windows run, twice in a row in some of them, and refused a
queue entry for a change that broke nothing. When a run fails, its summary page lists the compiler errors or the failed tests and the
command that reproduces them. The checks that only need the built binaries (the gold-reference
gate, the GUI smoke test, the translation checks) run whenever the build succeeded, even if a test
failed, so one run reports every failure.

To get more than the gate before merging (an ESP-IDF, Android or WASM change, a sanitizer
question), dispatch the full matrix on the branch: `gh workflow run ci.yml --ref <branch>`. A full
run holds a dozen or more hosted runners for most of an hour, and the gate of every other pull
request waits behind them, so when a change touches only how some builds are made, name the
legs instead: `gh workflow run ci.yml --ref <branch> -f legs=linux-llvm,macos-llvm`. Only those
builds run. The names are the presets in `.github/ci/legs.jsonc`.
`gh workflow run pr-gate.yml --ref <branch> -f windows=true` adds Windows MSVC to a gate run.

A pull request that was open when this arrived still shows the old `CI Status`. The merge queue
runs the gate on its own ref regardless. For a branch that needs the new check without a change,
dispatch `pr-gate.yml` on it.

## The merge queue

Each queue entry runs the gate on its merged tree. Qt is always built there, because the queue is
where a library change and a GUI caller written against the old API first meet. Windows MSVC runs
once per entry, on GitHub's `windows-latest`. Entries build in parallel and merge in groups, per
the `merge-queue-main` ruleset (see [branch protection](https://github.com/iainchesworthlabs/ac3forge/blob/main/.github/branch-protection.md)).

## After the merge

`ci.yml` runs on every push to main, but only one run executes at a time and at most one waits,
and the waiting place goes to the newest push. A burst of merges is verified as one batch. When
merges are rare, each is verified alone. When they are frequent, the newest commit's run covers
everything merged before it.

A green run moves the `verified` branch to that commit. It only moves forward. `git log
verified..main` lists what has merged since main was last proven, and `verified` is a safe commit
to branch or release from.

[`main-health.yml`](https://github.com/iainchesworthlabs/ac3forge/blob/main/.github/workflows/main-health.yml)
reads each finished run:

- **Failed jobs that all match a signature** in
  [`tools/ci/known_flakes.json`](https://github.com/iainchesworthlabs/ac3forge/blob/main/tools/ci/known_flakes.json)
  are rerun once (`gh run rerun --failed`). The signatures are error strings that were diagnosed as
  infrastructure: a lost runner, an apt mirror mid-sync, Launchpad 503, a truncated Android SDK
  download, a QEMU segfault at restart, hdiutil busy. When a new flake is diagnosed, add its
  string and the job it belongs to.
- **Any other failure** opens the single `main-red` issue, or adds to it if one is open. The issue
  lists the failed jobs with a log excerpt, the merges since `verified`, the command that
  reproduces each failed leg, and either the revert command (one merge in the range) or a
  `git bisect` recipe. Each suspect pull request gets one comment.
- **A green run** closes the issue.

A scheduled run (the nightly run, planned below) keeps its own books, because it runs legs the
run after a merge leaves out. A green one moves `verified-nightly` as well as `verified` and closes
both issues, since it proves everything the other run does. A red one is blamed on the merges since
the last green nightly, not since `verified` (a sanitizer failure can come from a merge the run
after it passed without running the sanitizers), goes to its own `main-red-nightly` issue, and
comments on no pull request, because a day of merges is too wide a range to name anyone. A green
run after a merge closes only `main-red`.

Nothing is reverted automatically. Opening a revert pull request that CI will then run needs a
token that can start workflows, which the built-in one cannot.

If a comment names your pull request, read the issue and decide: revert with the command it
gives, or push a fix. A fix goes through the gate like any other change.

## The tiers

`ci.yml` has two tiers. The run after a merge (`t2`) has to be cheap enough to run for every batch
of merges, so it keeps what a merge can break on the platforms and compilers the project ships,
and leaves out what is slow or rarely changes. The nightly run (`all`) runs everything, so what
the first leaves out is found within a day.

| | After a merge | Nightly only |
|---|---|---|
| Build legs | Linux GCC and LLVM (x64), Linux GCC (arm64), Windows MSVC and LLVM, macOS arm64 | Linux LLVM (arm64), ASan+UBSan, TSan, Windows MSVC (arm64), macOS x64 |
| Extra passes inside a leg | | Linux GCC's no-ALSA pass and its float32 and fixed-point variants, Linux LLVM's shared-library pass, macOS packaging |
| Core jobs | ADM module, Hearth Sendspin, the performance trend | coverage, ABI gate, FFmpeg Validate and the two trend publishers that read it |
| Other jobs | the lane's own build, when the lane's own tree changed | Linux AppImage, and each satellite (Android, WASM, ESP-IDF, Rust, wheels, npm) whatever changed |

The run after a merge also picks its lanes from what changed. It lists the files merged since
`verified` (the range a failure is blamed on) and runs the lanes they touch, so a batch of
documentation or a Python-only change builds little or nothing. A satellite lane runs only when a
path in its own tree changed: a change to the core library lights the desktop platforms and reaches
the satellites in the nightly run. The ESP-IDF lane also lights for the trees its component ships
(`src/forge/`, `src/arithmetic/`, `cmake/` and the root `CMakeLists.txt`), because a change there is
what breaks its package and its QEMU images. Anything the classifier does not recognise, and any change to
the workflows themselves, lights every lane. With no `verified` ref yet, every lane runs.

The nightly run is wanted at about 19:47 UTC. GitHub starts this repository's scheduled workflows
four to six and a half hours after their cron time: over the two weeks to 29 September 2026,
CodeQL's cron said 02:17 and its run began between 07:20 and 08:50 UTC. So `ci.yml` sets the cron
6.5 hours earlier, at 13:17 UTC. When the delay changes, move the cron by the difference.

To get the nightly tier on a branch before it merges, label the pull request `ci:deep`, or run
`gh workflow run ci.yml --ref <branch>` (the default tier is `all`). `-f tier=t2` runs the legs the
run after a merge uses. To run a few legs, name them: `-f legs=linux-llvm-asan-ubsan`. Naming legs
with `-f tier=t2` runs each as the run after a merge would, without its nightly-only passes, which
is a cheap way to see what that run will cost a change.

The leg and job placement is a decision, and it is in three places: `tier` and `deep_only` in
`.github/ci/legs.jsonc`, the `inputs.tier` conditions in `_ci-core.yml` and `_build.yml`, and the
satellite rule in `classify_changes.py`. Moving something between tiers is a change to one of them.

Two comparisons made only on pull requests, `performance-compare` and `memory-compare` in
`_ci-core.yml` with their `perf-regression-approved` and `memory-regression-approved` labels, have
not run since the gate replaced `ci.yml` on pull requests. Their absolute guards still do: `ac3perf`
runs in the gate, and the performance and memory trend jobs after a merge fail at the same +100%
thresholds, after the fact. Bringing the comparisons back before the merge would cost two Release
builds per pull request that touches `src/`, and the compiler cache makes the base build cheap.

## The legs

The build matrix is data. [`.github/ci/legs.jsonc`](https://github.com/iainchesworthlabs/ac3forge/blob/main/.github/ci/legs.jsonc)
lists every leg of the Linux, Windows and macOS builds, with the flags its steps read and the
comments that used to sit beside the matrices. [`tools/ci/plan_legs.py`](https://github.com/iainchesworthlabs/ac3forge/blob/main/tools/ci/plan_legs.py)
picks the legs a run needs. The `plan-legs` job in `_build.yml` runs it and passes each platform's
list to `_ci-linux.yml`, `_ci-windows.yml` or `_ci-macos.yml`, which run it as their matrix. A
platform with no leg in the run is skipped, because Actions rejects an empty matrix.

Each leg has a `tier`: `t2` for the legs of the run on main after a merge, `deep` for the legs only
the nightly run has. A `t2` leg can list `deep_only` flags, the slow extra passes its steps test,
and the run after a merge drops them from the leg. The planner's inputs are `TIER` (`all`, `t2` or
`deep`) and `LEGS`, a comma-separated list of presets such as `linux-gcc,windows-msvc` that runs
exactly those legs whatever their tier, with all their passes unless `TIER` is `t2`.

To add a leg, add it to the catalogue with either `runner` (labels as written) or `runner_slot` (a
`check-runners` output, for a leg that may run on the fleet). `python3 tools/ci/plan_legs.py
--check` validates the file. The unit tests also check that the platform workflows read no field the
catalogue lacks and that no leg sets a field the workflows never read, which `actionlint` can no
longer check now that the matrix arrives at run time.

## Caches

The gate, the queue and the legs of the run on main restore compiler caches (ccache); only a
push to main saves them. GitHub cache entries are immutable and evicted against a 10 GB budget
shared by the whole repository, so pull-request pushes that each saved a copy would push out the
entry every other run restores from. A cache saved on main is visible to pull requests and to
queue entries; one saved on a branch is not. That is why Linux GCC and Windows MSVC also run in
the gate when a push reaches main: the run exists to save the cache.

In the run on main the plain legs use it: Linux GCC and LLVM on x64 and arm64, Windows MSVC on
x64 and arm64, and both macOS legs. Each of them also runs ctest in the three phases described
above. A leg saves its cache when it compiled at least 25 objects the restored cache did not
have, so a push that changed two files does not upload another copy. The sanitizer legs use
neither, because their test presets carry label filters the phases would replace. Windows LLVM
(clang-cl) runs its tests in phases and compiles without the cache. A release build
(`do_package`) uses neither. The first cache saved for Linux GCC was 51 MB.

Each cache is keyed by leg, operating system and architecture, and ccache is configured to hash
the compiler binary rather than its file time, because each job installs a fresh copy of the
compiler. The directory is in the job's temp directory. The runner empties that around each job,
so a persistent fleet machine does not add every leg it has ever built to the upload, and GitHub
versions a cache by its path relative to the workspace, which is the same for a hosted and a
fleet runner there and differs under the home directory. A runner that cannot install ccache
builds without it and says so in a warning.

## Settings

| Setting | Effect |
|---|---|
| repository variable `GATE_RUNNER_JSON` | Runner labels for the gate's Linux and control jobs, e.g. `["self-hosted","Linux","X64"]`. Unset means `ubuntu-latest`. Fork pull requests stay hosted regardless. |
| repository variable `GATE_WINDOWS_RUNNER_JSON` | Runner labels for the Windows job. Unset means `windows-latest`. |
| repository variable `CONTROL_RUNNER_JSON` | Control jobs of `ci.yml`, as before. |
| `pr-gate.yml` input `windows` | Adds Windows MSVC to a dispatched run. |
| `pr-gate.yml` input `save_cache` | Saves the compiler caches from a dispatched run. |
| `ci.yml` input `legs` | Comma-separated presets. A dispatch runs exactly those build legs and nothing else. |
| `ci.yml` input `tier` | `all` (the default) or `t2`: the legs of the run after a merge, without their nightly-only passes. With `legs`, `t2` runs those legs that way. |
| label `ci:deep` on a pull request | Runs `ci.yml` (tier `all`) on the pull request's branch. Add it again to run it again. The label has to exist in the repository. |

## Not built yet

Test-level impact selection, from a per-test coverage map built by the nightly coverage run. It
needs the map first, and the nightly run now produces the coverage it would be built from.

The ABI gate rebuilds the last release tag on every run, and that build is identical until the
next release. It runs nightly, where one build a day costs little; a cached baseline is worth
adding only if the gate moves back to the run after a merge.
