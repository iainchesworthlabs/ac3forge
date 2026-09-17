# CI lane partitions

*Maintainer notes - CI structure; not a build or contribution guide.*

`ci.yml`'s `changes` job already tells a docs-only PR from a code one, so a
docs-only edit skips the whole build side entirely (`code` in that job's
outputs, computed by a `docs_re` regex). Every other PR used to pay for
Windows, Linux, macOS, Android, WASM, ESP-IDF and Rust regardless of what it
actually touched; a change confined to `apps/android/` now skips every one of
the other six - see "What's gated today" below. This page describes the
finer-grained classification, what it currently gates, and what it does not
gate yet.

## Current status

`tools/ci/classify_changes.py` exists, and the `changes` job in `ci.yml` calls
it, exposing one boolean output per lane (`core`, `windows`, `linux`, `macos`,
`android`, `wasm`, `esp`, `rust`, `python`, `npm`, `ci_self`, `docs`)
alongside the existing `code` output. `ci.yml` forwards seven of them
(`core`, `windows`, `linux`, `macos`, `android`, `wasm`, `esp`, `rust`) to
`_build.yml` as `run_<lane>` inputs, and `ci.yml`'s own `core`/`wheels`/`npm`/
`esp-component` job-calls are each gated on their own lane directly - every
lane except `ci_self` and `docs` (which were never meant to gate a build,
only fan out to the ones that are) now gates real work, see "What's gated
today". This closes out the plan's `fold-satellites` phase; what remains is
optional and lowest priority - a nightly-dispatch index documenting the
existing crons, no lane or gating change.

## What's gated today

`_build.yml`'s old single cross-OS `build` job - one GitHub Actions job with
an 11-entry `strategy.matrix` spanning Windows, Linux and macOS - is gone,
replaced by three reusable-workflow calls: `build-windows`
(`_ci-windows.yml`, 3 legs), `build-linux` (`_ci-linux.yml`, 6 legs) and
`build-macos` (`_ci-macos.yml`, 2 legs). Each is an ordinary job (not a
matrix job) at the `_build.yml` level, so `if: inputs.run_<lane>` gates it
cleanly - the job-level-`if`-cannot-see-`matrix` limitation that blocked this
in the previous phase no longer applies, because the matrix now lives one
level down, inside each platform's own file, invisible to `_build.yml`'s own
job-level conditions.

| Lane | Job(s) gated by `<lane>` |
|---|---|
| `android` | `build-android` |
| `wasm` | `build-wasm`, `device-ui` |
| `esp` | `build-esp32s3`, `hearth-esp32s3` (Hearth Sendspin sink under QEMU), `build-esp32c3` (also builds the ESP32-C6 probe), `build-footprint`, `ci.yml`'s `esp-component` job-call (`.github/workflows/esp-component.yml`: `pack`, `esphome`) |
| `rust` | `build-rust` |
| `windows` | `build-windows` (windows-msvc, windows-llvm, windows-msvc-arm64), `windows-driver` |
| `linux` | `build-linux` (linux-gcc, linux-llvm, linux-gcc-arm64, linux-llvm-arm64, linux-llvm-asan-ubsan, linux-llvm-tsan), `linux-appimage` |
| `macos` | `build-macos` (macos-llvm, macos-llvm-x64), `package-macos-universal` (alongside `do_package`, which it already required) |
| `core` | the whole `core` job-call (`_ci-core.yml`: coverage, ADM module, Hearth's Sendspin library, performance/memory compare+gate, ABI gate, FFmpeg validate, external-comparison and object-quality persisters) - see "The core lane" below |
| `python` | `ci.yml`'s `wheels` job-call (`.github/workflows/wheels.yml`: `build`, `python-coverage`) - see "The fold-satellites phase" below |
| `npm` | `ci.yml`'s `npm` job-call (`.github/workflows/npm.yml`: `build`) - see "The fold-satellites phase" below |

An Android-only PR today skips `build-wasm`, `build-esp32s3`/`hearth-esp32s3`/`c3`,
`build-footprint`, `build-rust`, `build-windows`, `windows-driver`,
`build-linux`, `linux-appimage`, `build-macos` and `package-macos-universal`
- the full win the plan's phase 3 example described, not just the subset the
previous phase could deliver.

`package-macos-universal` and `quality-trend` used to `needs: build` (the one
cross-OS job); they now `needs: build-macos` and
`needs: [build-windows, build-linux, build-macos]` respectively. The latter
still behaves exactly as before in practice: `quality-trend` only ever runs
when `persist_quality_trend` is true, which is only true for a direct push to
`main`, and that same trigger forces every `run_<lane>` true in
`classify_changes.py`'s `--force-all` path - so on the one trigger this job
actually fires on, none of its three `needs:` is ever skipped for a lane
reason. See that job's own comment in `_build.yml`.

## The core lane

`_ci-core.yml` contains coverage, ADM and Hearth validation, PR-time
performance/memory comparisons and their gates, the ABI gate, FFmpeg-oracle
validation, and the external-comparison and object-quality persisters - 11 jobs, moved
out of `ci.yml` wholesale and called as one `core` job, gated on
`needs.changes.outputs.core == 'true'` instead of the `code` output each of
them checked individually before. Only a change that touches the library or
trips a conservative fallback runs those 11 jobs; a platform-only change
does not. None tests anything platform-specific, so this is the intended,
correct behaviour, not an accident of the lane boundaries - see each job's
own header comment in `_ci-core.yml`, unchanged from before the move, for
why.

The main gold-reference quality persister did not move. `quality-trend` remains
in `_build.yml` because it needs the Windows, Linux and macOS build calls and
their gold-reference artifacts. Its hard trailing-regression failure therefore
surfaces as `_build.yml`'s `Publish quality trend` job, inside
`build-and-test`, not as an `_ci-core.yml` result.

**Not moved: `persist-performance-trend` and `performance-trend-arm64`.**
Both `needs: build-and-test` - _build.yml's_ own call, a *different* reusable
workflow - to know the whole matrix passed before recording a trend point.
`needs:` cannot cross a `workflow_call` boundary the way it crosses between
two jobs in the same file, so this dependency can only be expressed by
gating the *entire* `core` call on `build-and-test`'s result - which would
serialise coverage/ADM/FFmpeg-validate/performance-compare/memory-compare/
ABI-gate behind the full build matrix on *every* PR, when they run in
parallel with it today. Both jobs only ever fire on a direct push to `main`
anyway, where the extra wait costs nothing, so leaving them in `ci.yml`
(unchanged, still `needs: [build-and-test, toolchain-versions, ...]`) keeps
today's parallelism and avoids plumbing a cross-file dependency for a
two-job, push-only edge case.

**Every job's own `if:` that checked `needs.changes.outputs.code` lost that
check entirely**, rather than gaining an `inputs.run_core` equivalent: since
the *whole file* only runs when `core` is already true, an internal check
would be redundant. `performance-gate`/`memory-gate`/
`persist-external-comparison-trend`/`persist-object-quality-trend` never
checked `code` in the first place (see each one's own `if:` in
`_ci-core.yml`) and are byte-for-byte unchanged - including the two gates
running unconditionally on every `pull_request` and quietly passing when
their upstream compare job didn't produce a verdict, exactly as before.

### Keeping `CI Status`'s per-job breakdown

`_ci-core.yml` threads each of the eight jobs `ci-status` needs
(`coverage`, `adm-validate`, `hearth-validate`, `ffmpeg-validate`,
`performance-gate`, `memory-gate`, `persist-external-comparison-trend`,
`persist-object-quality-trend`) out through its own `workflow_call.outputs`,
rather than folding them into one aggregate result the way `build-and-test`
already folds together ten-plus build jobs. `ci-status`'s script still
prints `coverage: success`, `adm-validate: failure`, etc. individually -
unchanged from before the move - by reading `needs.core.outputs.<x>` instead
of `needs.<job>.result`.

Getting there needed one more piece than expected: `${{ jobs.<job_id>.result
}}` is **not** valid inside `workflow_call.outputs.<name>.value` -
`actionlint` rejects it ("property 'result' is not defined in object type
{outputs: {}}"), because that context only exposes a job's own declared
`outputs`, not its pass/fail status. Each of the eight jobs instead ends
with a "Record result" step - `if: always()`, so it still runs after an
earlier step failed - that captures `job.status` (a real, documented
context: "the current status of the job... success, failure, or cancelled")
into its own `outputs: result: ...`, and `_ci-core.yml`'s own
`workflow_call.outputs` reads `jobs.<job_id>.outputs.result` from there. Four
jobs (`performance-compare`, `memory-compare`, `abi-gate`, and the `core`
call itself needing none of this for anything not in the list above) don't
carry the extra step - their results were never surfaced to `ci-status`
before the move either.

### What `_ci-core.yml` needs from `ci.yml`

Same shape as `_build.yml`'s per-platform inputs: `check-runner` and
`toolchain-versions` stay in `ci.yml` (five of the 11 jobs share `runs-on:
${{ fromJSON(needs.check-runner.outputs.runner) }}` - one live-runner
decision reused by all of them, unlike `_build.yml`'s per-leg
`check-runners` fan-out), and `ci.yml`'s `core` job-call forwards
`check-runner.outputs.runner`, `toolchain-versions.outputs.vcpkg_commit` and
`toolchain-versions.outputs.llvm_version` (the only two toolchain-versions
outputs any of the 11 jobs actually reads - `llvm_version` only in two step
*names*, for display) as plain `workflow_call` inputs.

## Why a job, not a workflow-level path filter

The same reason `changes`/`code` is a job rather than `paths:` on `ci.yml`
itself (see that job's own comment): a workflow-level path filter means the
workflow never runs at all on a filtered-out change, so a required check it
produces - `CI Status` - would sit pending forever and block the PR. Path
skipping has to live *inside* an always-on workflow, with a skipped job
counted as a pass, the same way `CI Status`'s `needs` already treats
`coverage` and `memory-gate`. See `.github/branch-protection.md` for the
CodeQL incident this constraint comes from.

## Lane table

| Lane | Paths that light it directly | Also lit by |
|---|---|---|
| `core` | `src/`, `tests/`, `fuzz/`, `cmake/`, `tools/checks/`, `tools/ci/`, `requirements/`, root `CMakeLists.txt`/`CMakePresets.json`/`vcpkg.json` | - |
| `windows` | `apps/windows/`, `apps/notices/platform/windows/`, `packaging/winget/`, `packaging/conan/`, `packaging/vcpkg-port/` | `core`; shared desktop apps below |
| `linux` | `apps/linux/`, `apps/notices/platform/linux/`, `packaging/conan/`, `packaging/vcpkg-port/` | `core`; shared desktop apps below |
| `macos` | `apps/notices/platform/macos/`, `packaging/homebrew/`, `packaging/conan/`, `packaging/vcpkg-port/` | `core`; shared desktop apps below |
| `android` | `apps/android/` | `core` |
| `wasm` | `apps/wasm/`, `js/` (its E2E demo) | `core` |
| `esp` | `esp-idf/`, `esphome/`, `apps/baremetal/` | `core` |
| `rust` | `rust/` | `core` |
| `python` | `python/` | `core` |
| `npm` | `js/` (the package's own unit tests) | nothing - see below |
| `ci_self` | `.github/workflows/`, `.github/actions/`, `.github/toolchain/` | - |
| `docs` | `docs/`, any `*.md`, `LICENSE`, `mkdocs.yml` | - |

`apps/cli/`, `apps/gui/`, `apps/common/` and `apps/crucible/` light
`windows`, `linux` and `macos` directly - they are one desktop program built
and tested on all three, not three separate programs, so they are not
written as "core, therefore fanned out" but as a direct hit on each of the
three lanes.

## The fan-out rule

A change under `core`'s paths lights every platform and language lane
(`windows`, `linux`, `macos`, `android`, `wasm`, `esp`, `rust`, `python`) in
addition to `core` itself: a library change has to be validated everywhere
it is built, and a Windows-only leg has no way to discover on its own that
it also depends on `src/`. `npm` is deliberately excluded from the fan-out -
`js/`'s package unit tests only need to run when `js/` itself changes, the
same as today; the platform that embeds core via WASM is the `wasm` lane,
which *is* fanned out.

`ci_self` fans out to every lane, including itself and `docs`: a workflow,
action or toolchain-version edit can change how any lane is built or tested,
so it is treated the same as an unrecognised path (below) rather than
modelled precisely.

## Conservative defaults

Three situations mark every lane true rather than trying to be precise,
because a false skip is silent and wrong while a false build only costs a
few minutes:

- **An empty file list** - a manual `workflow_dispatch`, a `gh api` hiccup.
  Same rule `code` already applies for the same reason.
- **A path the classifier does not recognise** - a new top-level directory,
  or an existing one like `assets/`, `examples/`, `overrides/` or
  `planning/` that has never been given a lane. One unmapped path anywhere
  in the change is enough; the fallback does not degrade to "build only what
  matched".
- **`push` to `main` or a `merge_group` run** - passed as `--force-all` from
  `ci.yml`, bypassing path classification entirely. A queued or
  direct-to-main run has no single PR diff to classify against, and the
  merge queue's purpose is to catch what one PR's own lane subset could not
  see; both must stay full-matrix regardless of what the queue entry's own
  diff looks like.

## Known simplifications

- `requirements/` is entirely `core`, including `requirements-docs.*` -
  unlike the legacy `docs_re`, which excludes that one file from `code`. A
  docs-tooling lockfile bump fans out further than it needs to; this is the
  conservative-default trade-off above, applied to a directory rather than
  left unrecognised.
- `packaging/conan/` and `packaging/vcpkg-port/` are treated as touching all
  three desktop platforms, since both package managers support Windows,
  Linux and macOS. Neither is split further by which platform's recipe
  actually changed.
- Editing `.github/workflows/docs.yml` counts as `ci_self` (fans out to
  everything) even though the legacy `docs_re` in the `changes` job's `code`
  computation treats that one file as docs-only. The two classifiers answer
  different questions - `code` is "does this PR need the build matrix at
  all", `ci_self`/lanes is "could this workflow file affect how a lane is
  built" - and for the latter, any workflow edit is in scope until proven
  otherwise.
- `_build.yml`'s `build-footprint` job is gated by `run_esp`, not `run_linux`,
  even though it runs on a Linux-fleet runner and its own comments describe it
  as "Leg 5 of check-runners' Linux fan-out". A lane is which *source paths*
  a job builds, not which runner OS it happens to execute on: `build-footprint`
  cross-compiles `apps/baremetal/`'s probe for `arm-none-eabi` under QEMU, the
  same source tree `build-esp32s3`/`build-esp32c3` build for their own
  Xtensa/RISC-V targets (all three share "the same probe, same fixtures" per
  their own comments), and `apps/baremetal/` is `esp` in the lane table above.
  Gating it by `run_linux` instead would make an `apps/baremetal/`-only change
  skip it - a false skip, exactly what the conservative-default rule above
  exists to prevent.

## Where the logic lives

`tools/ci/classify_changes.py` is the single source of truth for the table
above - see its own header and `tools/ci/test_classify_changes.py` for the
worked examples of the fan-out rule and the conservative defaults. It is run
from `ci.yml`'s `changes` job, immediately after the existing `code`
classification, over the same file list:

```bash
gh api --paginate "repos/$REPO/pulls/$PR/files" --jq '.[].filename' \
  | python3 tools/ci/classify_changes.py >> "$GITHUB_OUTPUT"
```

Its own unit tests run in `ci.yml`'s `script-lint` job alongside every other
script under `tools/ci` - see that job's "Oracle unit tests" step.

## build-leg-composite: shared steps, factored out once

Two composite actions pulled out of the old cross-OS `build` job's step list,
so the three-way split below (each platform now its own file) does not
duplicate them:

- `.github/actions/build-leg` - the toolchain assert,
  `./.github/actions/setup-vcpkg`, Configure, Build and Test steps every
  matrix leg runs. Called once per leg, unconditionally, from all three of
  `_ci-windows.yml`/`_ci-linux.yml`/`_ci-macos.yml`.
- `.github/actions/gold-reference-gate` - the single canonical
  `tools/checks/verify_gold_reference.sh` invocation. Still called under the
  leg's own `if: matrix.gold_reference` at each of the three call sites - the
  action itself has no notion of the matrix, so whether to call it at all
  stays the caller's decision, same as `setup-msvc-env`'s `if: matrix.msvc`.

Composite action steps run in the calling job's own runner and workspace, not
a sandboxed one, so this was a pure move: `build/config-<preset>` lands on
disk exactly as it did when these were inline steps, and every step that
still runs after these two - the Crucible checks, the linux-gcc-only
scalar-tier gold-reference variants, the GUI smoke test - reads it the same
way. Nothing about what runs, in what order, or under what condition
changed; only where the step bodies live did.

**Not extracted**, deliberately: the linux-gcc-only mode=reference/float32/
fixed-point-decoder/float32-encoder gold-reference variants, the Crucible
build/coverage checks, the GUI smoke test, and every toolchain-install step
(Qt, MSVC environment, LLVM, ffmpeg, NSIS). Each of those already ran on only
one OS (or one single leg), so each platform's own file only ever needed one
copy regardless of whether it was a composite - extracting them would have
been refactoring for its own sake, not preventing duplication.

## The reusable-workflow split

`_build.yml`'s single `build` job (name: `${{ matrix.name }}`, an 11-entry
`strategy.matrix` spanning three OSes) became three files, each an ordinary
`workflow_call` reusable workflow with its own small matrix:

| File | Legs | Windows/Linux/macOS-only steps it carries |
|---|---|---|
| `.github/workflows/_ci-windows.yml` | windows-msvc, windows-llvm, windows-msvc-arm64 | Install LLVM/ffmpeg/NSIS (Windows), Setup MSVC environment, Install Qt (prebuilt), Crucible translation check + built assert + coverage floor, Assert NSIS installer, Assert Crucible packaged |
| `.github/workflows/_ci-linux.yml` | linux-gcc, linux-llvm, linux-gcc-arm64, linux-llvm-arm64, linux-llvm-asan-ubsan, linux-llvm-tsan | Bootstrap container, Install Qt6 (Linux GUI)/GCC/LLVM/ffmpeg, the linux-gcc-only scalar-tier gold-reference variants, Codec matrix (sanitizer), conformance vectors, ALSA fallback, the Linux Crucible/PipeWire pass, BUILD_SHARED_LIBS=ON pass |
| `.github/workflows/_ci-macos.yml` | macos-llvm, macos-llvm-x64 | Install Qt6/LLVM/ffmpeg (macOS), Assert Crucible built (shared with Windows), the universal-merge install-tree uploads |

Steps that applied to more than one OS in the original job (`Package`,
`Upload package artifacts`, `Assert the AC3Forge Crucible was built`, `Assert
the CLI man page and completions were packaged (Linux/macOS)`, `Install
Ninja`) are reproduced verbatim in every file whose OS their own `if:`
condition already covers, rather than pulled into a third composite - each
one is already self-contained (branches on `runner.os`/`matrix.preset`
internally), so copying it costs a few repeated lines, not a second place a
future edit could drift out of step with the first.

`_build.yml` itself now only orchestrates: `check-runners` and
`toolchain-versions` stay there (a live-runner-availability check and a
toolchain-version resolver, both used by satellite jobs `_ci-windows.yml`
etc. don't have their own copies of), and three job-calls -
`build-windows`/`build-linux`/`build-macos` - forward those two jobs'
outputs as plain `workflow_call` inputs, since `needs:` cannot reach into
another file's job the way it reaches between two jobs in the same one.

Two conditions that referenced a matrix field no single-OS file's own matrix
entries define any more had to change, both confirmed by `actionlint`
("property ... is not defined in object type ..."), neither a behaviour
change:

- `Install Ninja`'s `if: ${{ !matrix.container }}` is unconditionally true on
  Windows and macOS (no entry in either file ever sets `container`) and
  unconditionally false on Linux inside a container (where `ninja-build` is
  apt-installed in "Bootstrap container" instead) - so the Windows/macOS
  copies dropped the `if:` entirely rather than reference a field that no
  longer exists in scope, and the Linux file never carried this step at all.
- `matrix.gui`/`matrix.release_package`/`matrix.experimental` referenced in
  shared steps but never set by any Windows or macOS entry (`gui`, no
  Windows entry needs the flag - GUI is on by default there;
  `release_package`, no macOS entry has carried one since DR8;
  `experimental`, no Linux or macOS entry is experimental today) - the
  `build-leg` call passes literal `"false"`/`""` for these on the files
  where they are always unset, and `_ci-macos.yml`'s first matrix entry
  declares `release_package: false` explicitly, the same "declare it once so
  the type exists" pattern `windows-msvc`'s own `experimental: false` already
  used for the same reason in the original matrix.

## The fold-satellites phase

`wheels.yml`, `npm.yml` and `esp-component.yml` each used to trigger
independently of `ci.yml` - their own `pull_request`/`push` events with a
`paths:` filter, same shape `ci.yml` itself used before `changes`/`code`
existed. That made every check they produce (`Build wheels`, `Python
coverage`, `Build and test`, `Pack and verify`, `ESPHome external
component`) a **satellite**: it could go red on a genuine Python/npm/ESP
regression and block nothing, because nothing outside that workflow ever
looked at its result. `ci.yml` now calls all three directly - `wheels`
(gated on the `python` lane), `npm` (the `npm` lane), `esp-component` (the
`esp` lane) - and all three are in `CI Status`'s `needs` list, so their
checks are required the same way `core`'s are.

**Each workflow's own `push: tags: v*` trigger is untouched** - that is
still what fires the (largely disarmed - `npm.yml`'s and
`esp-component.yml`'s `publish` jobs need a manual dispatch, see
docs/releasing.md) release-publish path, and it is genuinely independent of
`ci.yml`'s call:

- `ci.yml` itself never triggers on a tag push (`on.push.branches: [main]`
  only), so its lane-gated calls and each workflow's own tag trigger can
  never both fire for the same event - there is nothing to race or
  double-run.
- Removing `pull_request` and `push.branches: [main]` from each workflow's
  own `on:` (the parts that existed purely for continuous PR/main-push
  validation, now `ci.yml`'s job) leaves `push: { paths, tags }` with no
  `branches:` key at all - which restricts the trigger to *only* tag pushes
  matching `v*`, not "any push, filtered by path," the way it read with
  `branches: [main]` still present. The `paths`/`tags` combination itself -
  the one part of this that actually matters for a real release, and the
  one this project has already cut real, working PyPI releases against - is
  byte-for-byte unchanged.
- None of the three needed a `merge_group` trigger added despite now
  producing required checks: unlike a standalone workflow such as
  `dependency-review.yml` (see `.github/branch-protection.md`'s "Merge
  queue" section for why *that* one needs it), these three are
  `workflow_call`-only for the PR/push path now - they run as nested jobs of
  `ci.yml`'s own already-`merge_group`-aware run, the same as `_build.yml`
  and `_ci-core.yml` already do without a `merge_group` trigger of their own.

**A small classifier gap surfaced while checking each workflow's exact
`paths:` list against the lane table**: `tools/packaging/` (holds only
`pack_esp_component.py`, `esp-component.yml`'s own filter names it directly)
and `examples/python/` (`wheels.yml`'s filter names it directly) were not in
any lane's prefixes, so a change confined to either would have hit the
conservative "unknown path" fallback - safe (still builds), but wider than
needed. Both are now `esp`/`python` prefixes respectively; see
`tools/ci/classify_changes.py`'s own comments.

**`wheels`/`npm`/`esp-component` are each ONE required entry in `CI
Status`**, not threaded per-sub-job the way `_ci-core.yml`'s eight are.
Unlike coverage/ADM/ABI/FFmpeg-validate/perf-gate/memory-gate - genuinely
independent concerns a reviewer benefits from telling apart at a glance -
each of these three workflows is already one coherent "does this package
still build and pass its own tests" concern, the same shape `build-and-test`
already folds `_build.yml`'s dozen-plus jobs into. `needs.wheels.result`
answering "success" or "failure" is exactly as informative as
`needs.build-and-test.result` already was for the C++ matrix.

## Nightly analysis is out of scope, on purpose

CodeQL, MSVC Code Analysis (PREfast), clang-tidy, SonarCloud, the deeper
fuzz sweep, `interop.yml`, and the weekly `osv-scanner.yml`/`zizmor.yml`/
`scorecard.yml` all run on a schedule against `main` only, never on a PR or
in the merge queue - see docs/ci-self-hosted-runners.md's "Nightly analysis
window" for the full cron table, the fleet-sharing arrangement with
`aqualink-automate`, and why each one is nightly rather than per-PR
(`.github/branch-protection.md`'s "Nightly analysis and other visible-only
scanners" section has the required-check history behind that choice). None
of them reads a `classify_changes.py` lane, none is gated by one, and this
plan does not propose changing that: a nightly run's whole point is
evaluating `main` as it stands, not a diff, so "skip this scanner because
the pushed commit didn't touch a relevant lane" is not a question that
applies to them the way it does to a PR's own checks.

The plan's own phased list names an optional `workflow_dispatch` umbrella
that would let a maintainer re-run every nightly workflow together with one
click, for a reason such as verifying a fleet change. It is not built here:
every nightly workflow already has its own `workflow_dispatch:` trigger (`gh
workflow run codeql.yml`, `... msvc-analysis.yml`, and so on, or the
"Run workflow" button in each one's own Actions page), so the umbrella's
only real value-add over that would be running nine dispatches instead of
one - convenience, not a missing capability - and building it means reading
and reasoning about nine security-sensitive scanner workflows this plan
otherwise never touches, for a "nice to have" the plan itself marks
optional. If a maintainer wants it later, docs/ci-self-hosted-runners.md's
table above is already the source of truth for which workflow to add to it.
