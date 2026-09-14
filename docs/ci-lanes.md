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
`_build.yml` as `run_<lane>` inputs, and every one of them except `core` now
gates real work - see "What's gated today". `python` and `npm` are computed
but not yet forwarded anywhere: nothing in `_build.yml` builds Python
bindings or the npm package (those live in `wheels.yml` and `npm.yml`, folded
into the aggregator only in a later phase). This is still short of the full
plan; see the CI lane partitions plan for what's left (moving `ci.yml`'s
coverage/sanitizer/ABI/perf/memory jobs into a `_ci-core.yml` so `core`
finally gates something, folding `wheels.yml`/`npm.yml`/`esp-component.yml`
into the aggregator).

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

| Lane | Job(s) gated by `run_<lane>` |
|---|---|
| `android` | `build-android` |
| `wasm` | `build-wasm`, `device-ui` |
| `esp` | `build-esp32s3`, `build-esp32c3`, `build-footprint` |
| `rust` | `build-rust` |
| `windows` | `build-windows` (windows-msvc, windows-llvm, windows-msvc-arm64), `windows-driver` |
| `linux` | `build-linux` (linux-gcc, linux-llvm, linux-gcc-arm64, linux-llvm-arm64, linux-llvm-asan-ubsan, linux-llvm-tsan), `linux-appimage` |
| `macos` | `build-macos` (macos-llvm, macos-llvm-x64), `package-macos-universal` (alongside `do_package`, which it already required) |
| `core` | nothing yet - `_ci-core.yml` (coverage, sanitizers, ABI, FFmpeg/ADM validate, perf/memory gates) is a later phase, still living in `ci.yml`/inside `_ci-linux.yml`'s sanitizer legs, not gated by any lane |

An Android-only PR today skips `build-wasm`, `build-esp32s3`/`c3`,
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
