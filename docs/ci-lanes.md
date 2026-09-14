# CI lane partitions

*Maintainer notes - CI structure; not a build or contribution guide.*

`ci.yml`'s `changes` job already tells a docs-only PR from a code one, so a
docs-only edit skips the five-platform `build` matrix entirely (`code` in
that job's outputs, computed by a `docs_re` regex). Every other PR used to pay
the full matrix regardless of what it actually touched; a change confined to
`apps/android/` now skips WASM, ESP-IDF and Rust (see "What's gated today"
below), though it still builds the `build` matrix's own Windows, Linux and
macOS legs - that part is not split by lane yet. This page describes the
finer-grained classification, what it currently gates, and what it does not
gate yet.

## Current status

`tools/ci/classify_changes.py` exists, and the `changes` job in `ci.yml` calls
it, exposing one boolean output per lane (`core`, `windows`, `linux`, `macos`,
`android`, `wasm`, `esp`, `rust`, `python`, `npm`, `ci_self`, `docs`)
alongside the existing `code` output. `ci.yml` forwards seven of them
(`core`, `windows`, `linux`, `macos`, `android`, `wasm`, `esp`, `rust`) to
`_build.yml` as `run_<lane>` inputs, and several of `_build.yml`'s
single-purpose jobs now gate on theirs - see "What's gated today". `python`
and `npm` are computed but not yet forwarded anywhere: nothing in `_build.yml`
builds Python bindings or the npm package (those live in `wheels.yml` and
`npm.yml`, folded into the aggregator only in a later phase). This is still
short of the full plan; see the CI lane partitions plan for what's left
(splitting `_build.yml` into one reusable workflow per lane so the `build`
matrix's own Windows/Linux/macOS legs can be gated too, folding
`wheels.yml`/`npm.yml`/`esp-component.yml` into the aggregator).

## What's gated today

| Lane | Job(s) in `_build.yml` gated by `run_<lane>` |
|---|---|
| `android` | `build-android` |
| `wasm` | `build-wasm`, `device-ui` |
| `esp` | `build-esp32s3`, `build-esp32c3`, `build-footprint` |
| `rust` | `build-rust` |
| `windows` | `windows-driver` |
| `linux` | `linux-appimage` |
| `macos` | `package-macos-universal` (alongside `do_package`, which it already required) |
| `core` | nothing yet - see below |

**Not gated: the `build` job's own matrix legs** (`windows-msvc`,
`windows-llvm`, `windows-msvc-arm64`, `linux-gcc`, `linux-llvm`,
`linux-gcc-arm64`, `linux-llvm-arm64`, `linux-llvm-asan-ubsan`,
`linux-llvm-tsan`, `macos-llvm`, `macos-llvm-x64`).
It is one GitHub Actions job with an 11-entry `strategy.matrix`, and a job's
`if:` cannot see the `matrix` context - confirmed against `actionlint`, which
rejects `matrix.*` in a job-level `if:` with "context 'matrix' is not allowed
here". Skipping only that job's macOS entries, say, would need either a
dynamically-computed `strategy.matrix.include` (duplicating this ~300-line
matrix's definition into filtering logic) or threading a lane condition
through dozens of already-conditional steps across a ~2000-line job - both
larger and riskier than this phase, and exactly the kind of rewrite the CI
lane partitions plan's "What we will not do" section rules out ahead of the
reusable-workflow split. That split (the plan's next-but-one phase) gives
each OS its own job, where `if: inputs.run_windows` works directly with no
matrix-context problem at all. Until then, an Android-only PR skips
`build-wasm`, `build-esp32s3`/`c3`/`build-footprint` and `build-rust`, but the
full 11-leg desktop matrix still runs.

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

## build-leg-composite: preparing `_build.yml` for the split

Two composite actions pulled out of the `build` job's step list, ahead of the
plan's reusable-workflow split (its next phase) so that split does not
duplicate them into each new per-platform file:

- [`./.github/actions/build-leg`](../.github/actions/build-leg/action.yml) -
  the toolchain assert, `./.github/actions/setup-vcpkg`, Configure, Build and
  Test steps every matrix leg runs. Called once per leg, unconditionally.
- [`./.github/actions/gold-reference-gate`](../.github/actions/gold-reference-gate/action.yml) -
  the single canonical `tools/checks/verify_gold_reference.sh` invocation.
  Still called under the leg's own `if: matrix.gold_reference` in `_build.yml`
  - the action itself has no notion of the matrix, so whether to call it at
  all stays the caller's decision, same as `setup-msvc-env`'s `if: matrix.msvc`.

Composite action steps run in the calling job's own runner and workspace, not
a sandboxed one, so this is a pure move: `build/config-<preset>` lands on
disk exactly as before, and every step that still runs after these two in
`_build.yml` - the Crucible checks, the linux-gcc-only scalar-tier gold-
reference variants, the GUI smoke test - reads it the same way. Nothing about
what runs, in what order, or under what condition changed; only where the
step bodies live did.

**Not extracted**, deliberately: the linux-gcc-only mode=reference/float32/
fixed-point-decoder/float32-encoder gold-reference variants, the Crucible
build/coverage checks, the GUI smoke test, and every toolchain-install step
(Qt, MSVC environment, LLVM, ffmpeg, NSIS). Each of those already runs on
only one OS (or one single leg), so a future per-platform file only ever
needs one copy regardless of whether it is a composite - extracting them
would be refactoring for its own sake, not preventing duplication, which is
what this phase exists to do.
