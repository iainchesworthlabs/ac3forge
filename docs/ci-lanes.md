# CI lane partitions

*Maintainer notes - CI structure; not a build or contribution guide.*

For triaging GitHub Advanced Security code-scanning alerts on pull requests, see
[`tools/ci/code_scanning_triage.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/tools/ci/code_scanning_triage.md).

Pull requests and merge-queue entries run
[`pr-gate.yml`](https://github.com/iainchesworthlabs/iclforge/blob/main/.github/workflows/pr-gate.yml),
described in [CI for many agents](ci-agentic.md), and `ci.yml` has no `pull_request` or
`merge_group` trigger. `ci.yml` runs on every push to main (one run at a time), on the nightly
schedule, and on a dispatch, and it uses the lanes on this page to decide what to run: a lane is a
set of source paths and the jobs that build or test them. The `changes` job classifies the files a
run has to answer for, and a change confined to `apps/android/` runs the Android build and none
of the other platforms. This page describes the classification, what it gates, and what it does not
gate.

The gate has its own, narrower planner (`tools/ci/plan_gate.py`, [The pull-request
gate](ci-agentic.md#the-pull-request-gate)), because a gate that only builds Linux has no use for
most of these lanes.

Which files a run answers for:

| Event | Files classified | Lanes |
|---|---|---|
| Push to main | everything merged since the `verified` ref | from the files; a satellite lane only for a change in its own tree ([The fan-out rule](#the-fan-out-rule)) |
| Nightly (`schedule`) | none | every lane on |
| Dispatch | none | every lane on, or only the platforms of the legs and jobs named with `legs` (`windows-driver` is a job) |

## Current status

`tools/ci/classify_changes.py` exists, and the `changes` job in `ci.yml` calls it, exposing one
boolean output per lane (`core`, `windows`, `linux`, `macos`, `android`, `wasm`, `esp`, `rust`,
`python`, `npm`, `ci_self`, `docs`) beside the `code` output (false for a change of documentation
only) and the `tier` output ([The tiers](ci-agentic.md#the-tiers)). `ci.yml` forwards eight of the
lanes (`core`, `windows`, `linux`, `macos`, `android`, `wasm`, `esp`, `rust`) to `_build.yml` as
`run_<lane>` inputs, though no job in `_build.yml` reads `run_core`, and gates its own `core`,
`wheels`, `npm` and `esp-component` job-calls on their lanes directly. `ci_self` and `docs` gate
nothing themselves: they only fan out to the lanes that do.

## What's gated today

`_build.yml` orchestrates three reusable-workflow calls, one per platform: `build-windows`
(`_ci-windows.yml`), `build-linux` (`_ci-linux.yml`) and `build-macos` (`_ci-macos.yml`). Each is
an ordinary job at the `_build.yml` level, so `if: inputs.run_<lane>` gates it cleanly, and the
matrix lives one level down in each platform's own file. The legs of the matrices are listed in
`.github/ci/legs.jsonc` (three Windows, six Linux and two macOS), the `plan-legs` job picks the ones
a run needs, and each file receives its list as `inputs.matrix` ([The
legs](ci-agentic.md#the-legs)).

| Lane | Job(s) gated by `<lane>` |
|---|---|
| `android` | `build-android` |
| `wasm` | `build-wasm`, `device-ui` |
| `esp` | `build-esp32s3` (also builds the S3 sink image), `hearth-esp32s3` (Hearth Sendspin sink under QEMU), `build-esp32c3` (also builds the ESP32-C6 probe, and the C6 and P4 sink images), `package-esp32-firmware` (the four published sink images and their manifest), `build-footprint`, `ci.yml`'s `esp-component` job-call (`.github/workflows/esp-component.yml`: `pack`, `esphome`) |
| `rust` | `build-rust` |
| `windows` | `build-windows` (windows-msvc, windows-llvm, windows-msvc-arm64), `windows-driver` |
| `linux` | `build-linux` (linux-gcc, linux-llvm, linux-gcc-arm64, linux-llvm-arm64, linux-llvm-asan-ubsan, linux-llvm-tsan), `linux-appimage` |
| `macos` | `build-macos` (macos-llvm, macos-llvm-x64), `package-macos-universal` (alongside `do_package`, which it already required) |
| `core` | the whole `core` job-call (`_ci-core.yml`: coverage, ADM module, Hearth's Sendspin library, the ABI gate, FFmpeg validate, and the external-comparison and object-quality persisters) - see "The core lane" below |
| `python` | `ci.yml`'s `wheels` job-call (`.github/workflows/wheels.yml`: `build`, `python-coverage`) - see "The fold-satellites phase" below |
| `npm` | `ci.yml`'s `npm` job-call (`.github/workflows/npm.yml`: `build`) - see "The fold-satellites phase" below |

Within a lane, the tier picks the legs and the extra passes (the leg table in [The
tiers](ci-agentic.md#the-tiers)), and some jobs are nightly only: `linux-appimage` and the
`core` jobs `coverage`, `abi-gate` and `ffmpeg-validate` do not run in the run after a merge
(`inputs.tier != 't2'`).

A change confined to `apps/android/` runs `build-android` after a merge and skips `build-wasm`,
`build-esp32s3`/`hearth-esp32s3`/`c3`, `build-footprint`, `build-rust`, `build-windows`,
`windows-driver`, `build-linux`, `linux-appimage`, `build-macos` and `package-macos-universal`.

`package-macos-universal` needs `build-macos` and runs only for a release (`do_package`).
`quality-trend` needs `[build-windows, build-linux, build-macos]` and runs only when
`persist_quality_trend` is true, which is true for a push to `main` and for the nightly run. The
nightly run forces every `run_<lane>` true, but the run after a merge takes its lanes from the
files merged since the last verified commit, so one of the three `needs:` can be skipped for a
lane reason. The job accepts a skipped platform as long as one of the three ran and passed, since
with none there is no artifact to record. See that job's own comment in `_build.yml`.

## The core lane

`_ci-core.yml` contains coverage, ADM and Hearth validation, the ABI gate, FFmpeg-oracle
validation, and the external-comparison and object-quality persisters - seven jobs, called as one
`core` job and gated on `needs.changes.outputs.core == 'true'`. Only a change that touches the
library or trips a conservative fallback runs those seven jobs; a platform-only change does not. None tests anything
platform-specific, so this is the intended behaviour, not an accident of the lane boundaries - see
each job's own header comment in `_ci-core.yml` for why.

Three of them run only above tier `t2` (`inputs.tier != 't2'`), that is in the nightly run and in
a run at tier `all`, which the `ci:deep` label dispatches: `coverage`, `abi-gate` and
`ffmpeg-validate`. The two persisters read `ffmpeg-validate`'s artifacts. The performance and
memory comparisons and their two gates have left `_ci-core.yml`: they ran on pull requests until the
gate replaced `ci.yml` there, and now `_compare.yml`, called by `pr-gate.yml`, runs them for a
merge-queue entry that changes `src/` ([The merge queue](ci-agentic.md#the-merge-queue)). `ci.yml`'s
`Verify Status` no longer reads them; the absolute guards that still run after a merge are named
under [The tiers](ci-agentic.md#the-tiers).

The main gold-reference quality persister did not move. `quality-trend` remains in `_build.yml`
because it needs the Windows, Linux and macOS build calls and their gold-reference artifacts. Its
hard trailing-regression failure therefore surfaces as `_build.yml`'s `Publish quality trend` job,
inside `build-and-test`, not as an `_ci-core.yml` result.

**Not moved: `persist-performance-trend` and `performance-trend-arm64`.** Both `needs:
build-and-test` - `_build.yml`'s own call, a *different* reusable workflow - to know the whole
matrix passed before recording a trend point. `needs:` cannot cross a `workflow_call` boundary
the way it crosses between two jobs in the same file, so this dependency can only be expressed by
gating the *entire* `core` call on `build-and-test`'s result - which would serialise the core
jobs behind the full build matrix. Both jobs only ever fire on a direct push to `main`, where the
extra wait costs nothing, so they stay in `ci.yml` (still `needs: [build-and-test,
toolchain-versions, ...]`), which keeps the parallelism and avoids plumbing a cross-file
dependency for a two-job, push-only edge case.

### Keeping the per-job breakdown in `Verify Status`

`ci.yml`'s `ci-status` job, `Verify Status`, is the one verdict for a run of `ci.yml`. It is not a
required check: branch protection points at `pr-gate.yml`'s `CI Status`, and this job has a
different name so the two cannot be mistaken for each other on a commit both ran on.

`_ci-core.yml` threads each of the six jobs `ci-status` needs (`coverage`, `adm-validate`,
`hearth-validate`, `ffmpeg-validate`, `persist-external-comparison-trend`,
`persist-object-quality-trend`) out through its own
`workflow_call.outputs`, rather than folding them into one aggregate result the way
`build-and-test` folds together its build jobs. `ci-status`'s script still prints `coverage:
success`, `adm-validate: failure`, etc. individually, by reading `needs.core.outputs.<x>` instead
of `needs.<job>.result`.

`${{ jobs.<job_id>.result }}` is **not** valid inside `workflow_call.outputs.<name>.value` -
`actionlint` rejects it ("property 'result' is not defined in object type {outputs: {}}"), because
that context only exposes a job's own declared `outputs`, not its pass/fail status. Each of the
six jobs instead ends with a "Record result" step - `if: always()`, so it still runs after an
earlier step failed - that captures `job.status` (a documented context: "the current status of
the job... success, failure, or cancelled") into its own `outputs: result: ...`, and
`_ci-core.yml`'s own `workflow_call.outputs` reads `jobs.<job_id>.outputs.result` from there.
`abi-gate` does not carry the extra step: its result was never surfaced to `ci-status`.

### What `_ci-core.yml` needs from `ci.yml`

Same shape as `_build.yml`'s per-platform inputs: `check-runner` and `toolchain-versions` stay in
`ci.yml` (four of the seven jobs share `runs-on: ${{ fromJSON(inputs.runner) }}` - one live-runner
decision reused by all of them, unlike `_build.yml`'s per-leg `check-runners` fan-out), and
`ci.yml`'s `core` job-call forwards `check-runner.outputs.runner`,
`toolchain-versions.outputs.vcpkg_commit`, `toolchain-versions.outputs.llvm_version` (`llvm_version`
only in two step *names*, for display) and the `tier` as plain `workflow_call` inputs.

## Why a job, not a workflow-level path filter

The same reason `changes`/`code` is a job in `ci.yml` and `plan` is a job in `pr-gate.yml`: a
workflow-level path filter means the workflow never runs at all on a filtered-out change, so a
required check it produces - `CI Status` - would sit pending forever and block the PR. Path
skipping has to live *inside* an always-on workflow, with a skipped job counted as a pass where the
plan did not ask for it. See `.github/branch-protection.md` for the CodeQL incident this constraint
comes from.

## Lane table

| Lane | Paths that light it directly | Also lit by |
|---|---|---|
| `core` | `src/`, `tests/`, `fuzz/`, `cmake/`, `tools/checks/`, `tools/ci/`, `requirements/`, root `CMakeLists.txt`/`CMakePresets.json`/`vcpkg.json` | - |
| `windows` | `apps/windows/`, `apps/notices/platform/windows/`, `packaging/winget/`, `packaging/conan/`, `packaging/vcpkg-port/` | `core`; shared desktop apps below |
| `linux` | `apps/linux/`, `apps/notices/platform/linux/`, `packaging/conan/`, `packaging/vcpkg-port/` | `core`; shared desktop apps below |
| `macos` | `apps/notices/platform/macos/`, `packaging/homebrew/`, `packaging/conan/`, `packaging/vcpkg-port/` | `core`; shared desktop apps below |
| `android` | `apps/android/` | `core` (not after a merge) |
| `wasm` | `apps/wasm/`, `js/` (its E2E demo) | `core` (not after a merge) |
| `esp` | `esp-idf/`, `esphome/`, `apps/baremetal/`, `tools/packaging/`, and the trees its component ships: `src/ac3/`, `src/arithmetic/`, `cmake/`, root `CMakeLists.txt` | `core` (not after a merge) |
| `rust` | `rust/` | `core` (not after a merge) |
| `python` | `python/`, `examples/python/` | `core` (not after a merge) |
| `npm` | `js/` (the package's own unit tests) | nothing - see below |
| `ci_self` | `.github/workflows/`, `.github/actions/`, `.github/toolchain/` | - |
| `docs` | `docs/`, any `*.md`, `LICENSE`, `mkdocs.yml` | - |

`apps/cli/`, `apps/gui/`, `apps/common/`, `apps/crucible/` and `apps/hearth/` light `windows`,
`linux` and `macos` directly - they are one desktop program built and tested on all three, not
three separate programs, so they are not written as "core, therefore fanned out" but as a direct
hit on each of the three lanes.

`tools/packaging/` holds only `pack_esp_component.py`, which `esp-component.yml`'s own path filter
names, and `examples/python/` is named by `wheels.yml`'s; neither belongs to a lane through its
parent directory, so each is listed on its own.

## The fan-out rule

A change under `core`'s paths lights every platform lane (`windows`, `linux`, `macos`) in addition
to `core` itself: a library change has to be validated everywhere it is built, and a Windows-only
leg has no way to discover on its own that it also depends on `src/`. The satellite lanes
(`android`, `wasm`, `esp`, `rust`, `python`) are lit by `core` too in the nightly run and on a
dispatch, but not in the run after a merge, which classifies with `--satellites-direct`: there a
satellite lane lights only for a change in its own tree, and a change to the core library reaches
the satellites in the nightly run. `npm` is deliberately excluded from the fan-out in every mode -
`js/`'s package unit tests only need to run when `js/` itself changes; the platform that embeds
core via WASM is the `wasm` lane.

`ci_self` fans out to every lane, including itself and `docs`: a workflow, action or
toolchain-version edit can change how any lane is built or tested, so it is treated the same as an
unrecognised path (below) rather than modelled precisely.

## Conservative defaults

Three situations mark every lane true rather than trying to be precise, because a false skip is
silent and wrong while a false build only costs a few minutes:

- **An empty file list** - a `gh api` hiccup, a range GitHub will not list in full (3,000 files or
  more), or no `verified` ref yet. Same rule `code` already applies for the same reason.
- **A path the classifier does not recognise** - a new top-level directory, or an existing one
  like `assets/`, `examples/`, `overrides/` or `planning/` that has never been given a lane. One
  unmapped path anywhere in the change is enough; the fallback does not degrade to "build only
  what matched".
- **The nightly `schedule` run or a manual `workflow_dispatch`** - passed as `--force-all` from
  `ci.yml`, bypassing path classification entirely. The nightly run exists to run everything the
  run after a merge did not. A dispatch is how a branch asks for the legs a pull request does not
  run (next section). It does not arrive with a file list: `github.event.before` is unset, so it
  would otherwise classify only its head commit's own diff. `classify_changes.py` also takes
  `--force-all` for a merge-queue entry, but the queue runs `pr-gate.yml`, not `ci.yml`.

A `push` to `main` is classified with `--satellites-direct`, from the files merged since the
`verified` ref.

The ESP-IDF lane has one more way to light: the trees its component ships. They are the ones
`tools/packaging/pack_esp_component.py` stages (`STAGED_TREES` and `STAGED_FILES`): `src/ac3/`,
`src/arithmetic/`, `cmake/` and the root `CMakeLists.txt`. A change there is what breaks the package
and the QEMU images, so it does not wait for the nightly run. The AC-4 trees, staged only for
`--with-ac4`, do wait. `test_classify_changes.py` reads the packer's list, so a tree added to it
without the lane learning about it fails a test.

## Scarce hosted legs

GitHub Free runs 20 GitHub-hosted jobs at a time, org-wide, and 5 for macOS. A pull-request push
used to ask for about 25 with every lane set, and on 2026-09-25 about 400 were queued, some for
over five hours, holding back every PR's `CI Status`. A pull request now runs the gate: the static
checks and one hosted Linux GCC job, and Windows MSVC joins in the merge queue and on main.

The hosted legs of `ci.yml` run after the merge instead: `macos` (`build-macos`), `rust`
(`build-rust`, three runners, one of them macOS), `python` (the `wheels` call, five runners, two of
them macOS), `android`, `wasm` (`build-wasm` and `device-ui`, the device web page), the `linux`
lane's `linux-appimage`, `npm` and `esp` (the `esp-component` call only; the ESP32 QEMU legs run on
the self-hosted fleet). The satellite lanes among them run only when a path in their own tree
changed (for `esp`, or in a tree its component ships), and the nightly run runs them all. `ci.yml` still carries `github.event_name !=
'pull_request'` conditions on several of them from the time it ran on pull requests; they change
nothing now.

**What still guards merges:** the queue runs the gate on the exact merge commit: the static checks,
Linux GCC with Qt, and Windows MSVC. What the gate cannot see is proven by the run after the
merge, and `main-health.yml` names the merges in a batch that failed.

**Running them on a branch before queueing:** `gh workflow run ci.yml --ref <branch>` runs the
full set, since a dispatch forces every lane on, and `-f legs=` narrows it to named legs. The
`ci:deep` label on a pull request does the same with tier `all`.

Windows on Arm (`windows-msvc-arm64`) used to run on pull requests, because it was one entry in
`_ci-windows.yml`'s static matrix beside the two x64 legs and a job-level `if` cannot see matrix
values. Both halves of that are gone: the matrices are in `.github/ci/legs.jsonc`, where this leg
is nightly-only, and pull requests run the gate, not this matrix.

## Only the newest main commit runs the hosted jobs

The run after a merge is single-flight: `ci.yml`'s `concurrency` group for a push is `ci-main`,
with `cancel-in-progress: false`, so one run executes and at most one waits, and the waiting place
goes to the newest push ([After the merge](ci-agentic.md#after-the-merge)). A burst of merges
costs one running and one waiting run, and the newest commit's run covers every merge before it.

**Why:** a push to `main` asked for about 24 GitHub-hosted jobs, and GitHub Free runs 20 at a
time, org-wide. On 2026-09-25 twenty merges landed within ten minutes, and minutes later 219
hosted jobs from `main` pushes sat in the queue, nearly all for commits a newer merge had already
superseded.

Before `ci.yml` was single-flight, each job on a GitHub-hosted runner shared its own concurrency
group with the same job in every other `main` run (`appimage-main`, `android-main`, `wasm-main`,
`device-ui-main`, `rust-main-<os>`, `windows-driver-main`, `coverage-main`, `linux-main-<leg>`,
`windows-main-<leg>`, `macos-main-<leg>`, and the whole-workflow groups of `wheels.yml`, `npm.yml`
and `esp-component.yml`), so that a newer merge's job took the single waiting place. Those groups
are still in `_build.yml`, `_ci-linux.yml`, `_ci-windows.yml`, `_ci-macos.yml`, `_ci-core.yml` and
the three satellite workflows; with one `ci.yml` run at a time, a hosted job of one run on `main`
never waits behind the same job of another, so they no longer change what runs. `docs.yml` and
`osv-scanner.yml` are separate workflows that do not run inside `ci.yml`, and their `main` pushes
still share one group without `cancel-in-progress`, so only the newest main commit's run waits for
a runner. Fuzz, Zizmor and Scorecard keep only the newest `main` push's run through their own
workflow-level groups.

**The cost:** a commit whose run was the waiting one and was replaced gets no results from `ci.yml`
at all, so the hosted legs' quality series (macOS, arm64, any hosted fallback) have gaps for it,
and it has no coverage or wheel results. The newest commit's run includes every merge before it.

**`Verify Status`:** a run replaced while it waited is cancelled before any job starts, and
`main-health.yml` ignores it. `ci-status` also accepts `cancelled` from `build-and-test`, `wheels`,
`npm` and `esp-component` on a push to `main` whose commit `main` has already moved past, found
with one request for `main`'s tip SHA; that tolerance dates from the per-job groups above, which
the single-flight `ci-main` group made unnecessary. A real failure still fails it: a job-call with
a failed leg reads `failure` even when another of its legs was cancelled. Coverage and the core
validate jobs report through step outputs, which come back empty for a job that never started,
and `ci-status` reads empty as a pass.

**Per-commit, though GitHub-hosted:** `Publish quality trend`, the two `persist-*-trend` jobs in
`_ci-core.yml`, `Publish performance trend` and its arm64 measurements. Each records the commit's
own trend point, including the self-hosted legs' numbers, and takes one to three minutes. The two
performance jobs also start only once `Build & Test` finishes.

## Known simplifications

- `requirements/` is entirely `core`, including `requirements-docs.*` - unlike the `docs_re` in the
  `changes` job, which excludes that one file from `code`. A docs-tooling lockfile bump fans out
  further than it needs to; this is the conservative-default trade-off above, applied to a
  directory rather than left unrecognised.
- `packaging/conan/` and `packaging/vcpkg-port/` are treated as touching all three desktop
  platforms, since both package managers support Windows, Linux and macOS. Neither is split further
  by which platform's recipe actually changed.
- Editing `.github/workflows/docs.yml` counts as `ci_self` (fans out to everything) even though
  the `docs_re` in the `changes` job's `code` computation treats that one file as docs-only. The
  two classifiers answer different questions - `code` is "does this change need the build matrix
  at all", `ci_self`/lanes is "could this workflow file affect how a lane is built" - and for the
  latter, any workflow edit is in scope until proven otherwise.
- `_build.yml`'s `build-footprint` job is gated by `run_esp`, not `run_linux`, even though it runs
  on a Linux-fleet runner and its own comments describe it as "Leg 5 of check-runners' Linux
  fan-out". A lane is which *source paths* a job builds, not which runner OS it happens to execute
  on: `build-footprint` cross-compiles `apps/baremetal/`'s probe for `arm-none-eabi` under QEMU,
  the same source tree `build-esp32s3`/`build-esp32c3` build for their own Xtensa/RISC-V targets
  (all three share "the same probe, same fixtures" per their own comments), and `apps/baremetal/`
  is `esp` in the lane table above. Gating it by `run_linux` instead would make an
  `apps/baremetal/`-only change skip it - a false skip, exactly what the conservative-default rule
  above exists to prevent.

## Where the logic lives

`tools/ci/classify_changes.py` is the single source of truth for the table above - see its own
header and `tools/ci/test_classify_changes.py` for the worked examples of the fan-out rule and the
conservative defaults. It is run from `ci.yml`'s `changes` job over the same file list the `code`
classification reads. After a merge that list is the files changed between the `verified` ref and
the pushed commit:

```bash
gh api --paginate "repos/$REPO/compare/$verified...$SHA" --jq '.files[].filename' \
  | python3 tools/ci/classify_changes.py --satellites-direct >> "$GITHUB_OUTPUT"
```

Its own unit tests run in `_static.yml`'s "Oracle unit tests" step, in the gate, alongside every
other script under `tools/ci`.

## build-leg-composite: shared steps, factored out once

Two composite actions were pulled out of the old cross-OS `build` job's step list, so the
three-way split below (each platform its own file) does not duplicate them:

- `.github/actions/build-leg` - the toolchain assert, `./.github/actions/setup-vcpkg`, Configure,
  Build and Test steps every matrix leg runs. Called once per leg, unconditionally, from all three
  of `_ci-windows.yml`/`_ci-linux.yml`/`_ci-macos.yml`, and from the two jobs of `pr-gate.yml`.
- `.github/actions/gold-reference-gate` - the single canonical
  `tools/checks/verify_gold_reference.sh` invocation. Still called under the leg's own `if:
  matrix.gold_reference` at each of the three call sites - the action itself has no notion of the
  matrix, so whether to call it at all stays the caller's decision, same as `setup-msvc-env`'s
  `if: matrix.msvc`. `pr-gate.yml` calls it after its own build.

Composite action steps run in the calling job's own runner and workspace, not a sandboxed one, so
this was a pure move: `build/config-<preset>` lands on disk exactly as it did when these were
inline steps, and every step that still runs after these two - the Crucible checks, the
linux-gcc-only scalar-tier gold-reference variants, the GUI smoke test - reads it the same way.

**Not extracted**, deliberately: the linux-gcc-only mode=reference/float32/fixed-point-decoder/
float32-encoder gold-reference variants, the Crucible build/coverage checks, the GUI smoke test,
and every toolchain-install step (Qt, MSVC environment, LLVM, ffmpeg, NSIS). Each of those already
ran on only one OS (or one single leg), so each platform's own file only ever needed one copy
regardless of whether it was a composite - extracting them would have been refactoring for its own
sake, not preventing duplication.

## The reusable-workflow split

`_build.yml`'s single `build` job (an 11-entry `strategy.matrix` spanning three OSes) became three
files, each an ordinary `workflow_call` reusable workflow with its own matrix. The matrices are no
longer written in those files: the legs are listed in `.github/ci/legs.jsonc`, `_build.yml`'s
`plan-legs` job picks the ones a run needs, and each file takes its list as `inputs.matrix` (see
[CI for many agents](ci-agentic.md#the-legs)):

| File | Legs | Windows/Linux/macOS-only steps it carries |
|---|---|---|
| `.github/workflows/_ci-windows.yml` | windows-msvc, windows-llvm, windows-msvc-arm64 | Install LLVM/ffmpeg/NSIS (Windows), Setup MSVC environment, Install Qt (prebuilt), Crucible translation check + built assert + coverage floor, Assert NSIS installer, Assert Crucible packaged |
| `.github/workflows/_ci-linux.yml` | linux-gcc, linux-llvm, linux-gcc-arm64, linux-llvm-arm64, linux-llvm-asan-ubsan, linux-llvm-tsan | Bootstrap container, Install Qt6 (Linux GUI)/GCC/LLVM/ffmpeg, the linux-gcc-only scalar-tier gold-reference variants, Codec matrix (sanitizer), conformance vectors, ALSA fallback, the Linux Crucible/PipeWire pass, BUILD_SHARED_LIBS=ON pass, the installed-SDK C consumer |
| `.github/workflows/_ci-macos.yml` | macos-llvm, macos-llvm-x64 | Install Qt6/LLVM/ffmpeg (macOS), Assert Crucible built (shared with Windows), the universal-merge install-tree uploads |

Steps that applied to more than one OS in the original job (`Package`, `Upload package
artifacts`, `Assert the Crucible was built`, `Assert the CLI man page and completions
were packaged (Linux/macOS)`, `Install Ninja`) are reproduced verbatim in every file whose OS
their own `if:` condition already covers, rather than pulled into a third composite - each one is
already self-contained (branches on `runner.os`/`matrix.preset` internally), so copying it costs a
few repeated lines, not a second place a future edit could drift out of step with the first.

`_build.yml` itself only orchestrates: `check-runners`, `toolchain-versions` and `plan-legs` stay
there (a live-runner-availability check, a toolchain-version resolver and the leg planner, all
used by satellite jobs that the platform files do not have their own copies of), and three
job-calls - `build-windows`/`build-linux`/`build-macos` - forward those jobs' outputs as plain
`workflow_call` inputs, since `needs:` cannot reach into another file's job the way it reaches
between two jobs in the same one.

Two conditions that referenced a matrix field no single-OS file's own matrix entries define any
more had to change, both confirmed by `actionlint` ("property ... is not defined in object type
..."), neither a behaviour change:

- `Install Ninja`'s `if: ${{ !matrix.container }}` is unconditionally true on Windows and macOS (no
  entry in either file ever sets `container`) and unconditionally false on Linux inside a container
  (where `ninja-build` is apt-installed in "Bootstrap container" instead) - so the Windows/macOS
  copies dropped the `if:` entirely rather than reference a field that no longer exists in scope,
  and the Linux file never carried this step at all.
- `matrix.gui`/`matrix.release_package`/`matrix.experimental` referenced in shared steps but never
  set by any Windows or macOS entry (`gui`, no Windows entry needs the flag - GUI is on by default
  there; `release_package`, no macOS entry has carried one since the universal `.dmg` job took over macOS packaging; `experimental`, no Linux or
  macOS entry is experimental today) - the `build-leg` call passes literal `"false"`/`""` for these
  on the files where they are always unset, and the first macOS leg in `legs.jsonc` declares
  `release_package: false` explicitly, the same "declare it once so the type exists" pattern
  `windows-msvc`'s own `experimental: false` uses for the same reason.

## The fold-satellites phase

`wheels.yml`, `npm.yml` and `esp-component.yml` each used to trigger independently of `ci.yml` -
their own `pull_request`/`push` events with a `paths:` filter. That made every check they produce
(`Build wheels`, `Python coverage`, `Build and test`, `Pack and verify`, `ESPHome external
component`) a **satellite**: it could go red on a genuine Python/npm/ESP regression and block
nothing, because nothing outside that workflow ever looked at its result. `ci.yml` now calls all
three directly - `wheels` (gated on the `python` lane), `npm` (the `npm` lane), `esp-component`
(the `esp` lane) - and all three are in `Verify Status`'s `needs` list, so a red one fails the run
after a merge and `main-health.yml` reports it.

**Each workflow's own `push: tags: v*` trigger is untouched** - that is still what fires the
release-publish path (largely disarmed: `npm.yml`'s and `esp-component.yml`'s `publish` jobs need a
manual dispatch, see [docs/releasing.md](releasing.md)), and it is independent of `ci.yml`'s call:

- `ci.yml` itself never triggers on a tag push (`on.push.branches: [main]` only), so its
  lane-gated calls and each workflow's own tag trigger can never both fire for the same event -
  there is nothing to race or double-run.
- Removing `pull_request` and `push.branches: [main]` from each workflow's own `on:` (the parts that
  existed purely for continuous PR/main-push validation, now `ci.yml`'s job) leaves `push: { paths,
  tags }` with no `branches:` key at all - which restricts the trigger to *only* tag pushes
  matching `v*`, not "any push, filtered by path," the way it read with `branches: [main]` still
  present. The `paths`/`tags` combination itself - the part that matters for a real release - is
  unchanged.
- None of the three has a `merge_group` trigger: they are `workflow_call`-only for the validation
  path, running as nested jobs of `ci.yml`, which runs after a merge and not in the queue.

`tools/packaging/` and `examples/python/` were once in no lane's prefixes, so a change confined to
either hit the conservative "unknown path" fallback - safe, but wider than needed. Both are lane
prefixes now (`esp` and `python`, see the lane table).

**`wheels`/`npm`/`esp-component` are each ONE entry in `Verify Status`**, not threaded per-sub-job
the way `_ci-core.yml`'s six are. Unlike coverage/ADM/ABI/FFmpeg-validate -
independent concerns a reviewer benefits from telling apart at a glance - each of these three
workflows is one coherent "does this package still build and pass its own tests" concern, the same
shape `build-and-test` folds `_build.yml`'s dozen-plus jobs into. `needs.wheels.result` answering
"success" or "failure" is as informative as `needs.build-and-test.result` is for the C++ matrix.

## Analysis and scanner workflows

CodeQL (`codeql.yml`), MSVC Code Analysis (PREfast, `msvc-analysis.yml`), clang-tidy
(`static-analysis.yml`) and SonarCloud (`sonarcloud.yml`) run on a schedule against `main` only,
never on a pull request or in the merge queue. The deeper fuzz sweep is `fuzz.yml`'s nightly job
(the same workflow also runs `Fuzz Regress` on pull requests and pushes, and `Fuzz Short` and
`Fuzz Differential` on pushes). `interop.yml` runs nightly and on a pull request that touches the
decoder or its comparison scripts. `osv-scanner.yml` and `zizmor.yml` run on pull requests and
pushes to `main` and weekly, and `scorecard.yml` on pushes to `main` and weekly; the repository
variable `PAUSE_NONESSENTIAL_CI` pauses the pull-request runs of OSV-Scanner, Zizmor and Fuzz
Regress ([Settings](ci-agentic.md#settings)). See
[Self-hosted CI runners](ci-self-hosted-runners.md#nightly-analysis-window) for the cron table, the
fleet-sharing arrangement with `aqualink-automate`, and why each engine is nightly rather than
per-PR (`.github/branch-protection.md`'s "Nightly analysis and other visible-only scanners" section
has the required-check history behind that choice). None of them reads a `classify_changes.py` lane
and none is gated by one: a nightly run's whole point is evaluating `main` as it stands, not a
diff, so "skip this scanner because the pushed commit didn't touch a relevant lane" is not a
question that applies to them the way it does to a change's own checks.

There is no umbrella workflow that dispatches every scheduled workflow at once. Each has its own
`workflow_dispatch:` trigger (`gh workflow run codeql.yml`, `... msvc-analysis.yml`, and so on, or
the "Run workflow" button on its Actions page); `ci.yml`'s nightly run is dispatched the same
way. An umbrella would only save running nine dispatches instead of one, and would mean
maintaining a list of security-sensitive workflows in a further file.
