# WebAssembly (browser demos)

WASM support is not `ac3cli` ported to a browser — it is two small demo apps under **`apps/wasm/`**
that compile `ac3::forge` to WebAssembly and run it client-side in a static HTML page: a **decode**
demo (roadmap F3) and, as of roadmap UX6, an **encode** demo. Decode: load a real elementary stream,
hear the decoded bed play through the Web Audio API, watch real per-channel energy on a speaker-ring
visualization, and — for a stream carrying Atmos objects — watch each object's real decoded position
(OAMD) move in a room view and solo its own real reconstructed audio (JOC). Encode: drop a `.wav`
file, get back a real AC-3/E-AC-3 elementary stream plus a real BS.1770 loudness/true-peak QC verdict
against five delivery presets, and a round-trip preview through the decode module. Both exist to
prove the codec runs correctly outside a native process, and to give the documentation site live
demos (see [Live decode demo](../wasm-demo.md) and [Live encode demo](../wasm-encode-demo.md)) — not
to be general-purpose in-browser tools. This page covers what is specific to WASM; for the core
library and the desktop platforms, see [Building from source](../building.md) and the other pages in
this section.

## Encode module

The decode demo's own docs used to call WASM-encode "a separate, much larger undertaking" and leave
it deliberately out of scope, reasoning from first principles about "real-time MDCT/bit-allocation/
JOC matrix work in a browser thread." Roadmap UX6 measured it instead of assuming it, on the same
WSL2/Emscripten 6.0.6 toolchain `build-wasm` uses:

- **Binary size.** A module binding the AC-3 encoder (`ac3::FrameEncoder`), the E-AC-3 encoder
  (`ac3::eac3::FrameEncoder`), the Atmos/JOC bed encoder (`ac3::oba::AtmosEncoder`) and the QC
  loudness meter (`ac3::meta::LoudnessMeter`/`evaluate_qc_gate`) together — everything
  `encoder_bindings.cpp` binds, not a cut-down subset — compiles to **390 KB raw / 149 KB gzip**,
  against the decode module's own **372 KB raw / 130 KB gzip**. Comparable order of magnitude, not
  the multi-megabyte blow-up "much larger undertaking" implied; a third module split (e.g. Atmos
  bound separately) was not worth pursuing.
- **Real-time factor.** Timed under Node/V8 (a reasonable proxy for Chrome's own engine),
  single-threaded, no WASM SIMD, `-O3`, real encoder code paths (not a timing loop around a stub):
  AC-3 2.0 encodes at **385x real-time**, E-AC-3 3/2+LFE at **120x**, a 4-object Atmos/JOC encode at
  **82x**. There is enormous headroom below 1x even accounting for a slower mobile CPU and for
  optional encoder work not exercised in that measurement (`search=distortion`, coupling). This is
  why the encode module needs no `pthreads`/`SharedArrayBuffer` — everything above runs on the main
  thread (or a plain `postMessage`-fed Worker) with room to spare, which also means a future
  real-time (microphone-capture) product is a plumbing problem, not a CPU one.
- **QC is genuinely no new DSP.** `ac3::meta::LoudnessMeter` and `ac3::meta::evaluate_qc_gate`/
  `qc_preset()` are the exact functions `ac3cli qc` calls — pure, third-party-dependency-free,
  streaming (`push()` per block). One real nuance: `integrated_lkfs()` is a gated, whole-programme
  measure (`std::nullopt` until BS.1770's absolute gate has seen enough), so the delivery-preset
  pass/fail table is necessarily an end-of-file readout, not a live one — `momentaryLkfs()`/
  `shortTermLkfs()` are what a future live product would show updating in real time.

`apps/wasm/encoder_bindings.cpp` binds the full surface above (including Atmos/JOC) even though
`apps/wasm/encode/`'s page only exposes AC-3/E-AC-3 bed encoding today — an object-authoring UI on
top of `WasmAtmosBedEncoder` is page-only work for a later PR, not a module change.

## Build and run

An [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html) on `PATH`
(`source <emsdk>/emsdk_env.sh` / `emsdk_env.bat`/`.ps1`), then:

```bash
cmake --preset config-wasm-emscripten
cmake --build build/config-wasm-emscripten
cd build/config-wasm-emscripten/bin/wasm_decode_demo   # or .../wasm_encode_demo
python3 -m http.server 8000   # ES modules and fetch() need http(s), not file://
```

Open `http://localhost:8000/`. Each demo directory is independently servable — `wasm_encode_demo/`
carries its own copy of the decode module (for its round-trip preview) rather than assuming the
decode demo's directory is a sibling.

## What's reused, what's new

`ac3::forge` (`src/forge/`) — the codec, `FrameDecoder`/`Eac3Decoder`, elementary-stream scanning — is
fully platform-independent and is linked into the demo **unmodified**, the same way `apps/wasm/CMakeLists.txt`
links it as any other consumer would: `add_executable` + `target_link_libraries(... ac3::forge ...)`,
no fork, no `#ifdef`. Unlike `apps/android/`, this doesn't need a separate build system reached
from the other direction — WASM is a plain CMake cross-compile, so `apps/wasm/` is a normal
`add_subdirectory()` from the root `CMakeLists.txt`, gated on `EMSCRIPTEN` (set by
`cmake/toolchains/wasm.emscripten.toolchain.cmake`) rather than an `AC3FORGE_BUILD_*` option.
`ac3::audio` (`src/audio/`) gains **no** WASM backend — there is no live-capture/passthrough
equivalent to add; a browser gets audio playback from the Web Audio API in JavaScript instead, and
`src/audio` is skipped from the configure entirely under `EMSCRIPTEN` (it hard-fails otherwise, for
having no browser platform directory — see `src/audio/CMakeLists.txt`).

Everything else — `decoder_bindings.cpp` (the Embind wrapper), `index.html`/`demo.js` (the page, Web
Audio playback, the Canvas visualizations ported from `apps/gui/qml/SoundfieldView.qml` and Main.qml's
Objects tab) — is new and lives entirely under `apps/wasm/`, outside anything the desktop tools
build from. The object visualization/audio is a thin JS-facing surface over `Eac3Decoder`'s own real
`object_metadata` (OAMD positions/gain, `ac3::forge#168`) and `object_audio` (JOC-reconstructed
per-object audio, `ac3::forge#169`) fields — `decoder_bindings.cpp` does no decoding of its own, it
just accumulates what `Eac3Decoder` already produced per frame and exposes it as typed-array views.

The encode demo follows the identical shape, one level down: `encoder_bindings.cpp` is a second,
independent Embind wrapper (its own `add_executable`, its own `EMSCRIPTEN_BINDINGS` block, its own
`EXPORT_NAME` so the two modules can load on one page without colliding), linking `ac3::forge`
**unmodified** the same way the decode target does — no fork, no `#ifdef`, confirming the "encoders
are already proven platform-free" premise this depended on (the same `ac3::forge` target already
links unmodified into `apps/android`'s NDK build and `python/`'s pybind11 module). `apps/wasm/encode/`
(`index.html`/`app.js`) is the page: a drop zone, coding-mode/rate/bitrate controls, the QC verdict
table, and the round-trip preview. It reorders a dropped WAV's WAVEFORMATEXTENSIBLE channel order
into AC-3's Table 5.8 order before encoding — see `app.js`'s own comment on the exact mapping — and
resamples via the browser's own `AudioContext`, rather than writing a sample-rate converter.

What counts as "an object" there is every JOC output, which `ac3::oba::describe_objects()` spells
out: a dynamic object supplies its own position, size and gain, and a bed channel — what
channel-based-immersive third-party content carries — is drawn at the nominal room position of the
speaker its label names, with that label on its solo button. Each object's per-frame record also
carries TS 103 420 §5.6.1.2's extent, so a sized object draws bigger than a point source.

One wrinkle worth knowing when previewing locally: `docs/assets/wasm-decode-demo/` holds a
*committed* `ac3forge_decode.wasm` that only the docs deploy job rebuilds, so a local `mkdocs
serve` can be running an older module than the checked-in `demo.js`. `demo.js` therefore feature-
detects the newer bindings and derives its per-object record stride from the data rather than
hard-coding it.

## Toolchain

No vcpkg. Every other platform preset in `CMakePresets.json` chainloads through vcpkg for
consistency, but `ac3::forge`'s decode path has zero third-party dependencies (`vcpkg.json`'s own
description says so), so `config-wasm-emscripten`'s toolchain file goes straight to Emscripten's own
`Emscripten.cmake` — see that toolchain file's own header for why going through vcpkg's community
`wasm32-emscripten` triplet would be pure cost for nothing this preset needs.

Verified against **Emscripten 6.0.6**. No version is pinned in the toolchain file itself (unlike the
Android NDK's explicit pin) — there is no CMake-side equivalent of `local.properties`' `sdk.dir` to
pin against yet; whatever `$EMSDK` resolves to is what gets used.

## Release / CI

The demos build alongside the desktop packages rather than only ever being hand-built locally:
`.github/workflows/_build.yml`'s `build-wasm` job configures and builds both (one `cmake --build`
over the whole preset) on every push, the same continuous-smoke-test role `build-android`'s
always-on debug APK plays — proving the Emscripten toolchain and every file it touches still build.
Like `build-android`, it's its own job rather than a `build` matrix entry: this leg has no ctest
suite, no cpack package and no gold-reference gate, so folding it into that matrix would mean
threading new `if:` exclusions through most of that job's steps for no benefit.

**The published demo is rebuilt fresh, not shipped from a committed copy.** `docs/assets/wasm-decode-demo/`
is committed to the repo as a working fallback (so a plain local `mkdocs build` — or this repo's own
PR-time docs check — still has something to embed without anyone needing Emscripten installed just
to preview docs), but `.github/workflows/docs.yml`'s `deploy` job (push to `main` only) installs
Emscripten, rebuilds `apps/wasm/` from source, and overwrites that directory *before* `mkdocs
gh-deploy` runs — so what actually reaches the live site always reflects current source, never a
possibly-stale commit. Both jobs share one Emscripten install step,
`.github/actions/setup-emscripten` (pinned to the same version this page's Toolchain section names),
so the two never drift onto different SDK versions.

The committed fallback is not immune to going stale, though: nothing rewrites it except a human
manually re-copying `apps/wasm/`'s output, and `docs.yml`'s own `deploy` job overwrites its
working copy in a throwaway CI workspace rather than committing the refresh back. `docs.yml`'s
`build` job (the one every PR runs, `mkdocs build --strict`) therefore also byte-compares
`index.html`, `demo.js`, the two favicon files and `assets/demo.ec3` against their `apps/wasm/`
originals, and does the same for the encode demo's `encode/index.html`/`encode/app.js` against
`docs/assets/wasm-encode-demo/` — the plain copies, not Emscripten output, so the check needs no
toolchain. The `.js`/`.wasm` build artifacts (both modules) have no source-tree counterpart and are
outside this check's scope; they only get refreshed by an actual Emscripten rebuild.

`docs.yml`'s trigger `paths:` list includes `apps/wasm/**`, `CMakeLists.txt`,
`CMakePresets.json` and the WASM toolchain file specifically — without them, a source change there
would never trigger a redeploy at all, and the live demo would silently drift from what's in
`apps/wasm/`.

## What has and has not been verified

!!! note "Verified in a real browser"
    Both `cmake --preset config-wasm-emscripten` and the full desktop presets configure and build
    clean from the same source tree (confirmed repeatedly across this PR's history, including after
    merging in the then-current integration branch and #169's own branch directly). A real
    Chromium instance loading the built page — both standalone and embedded in the actual
    `mkdocs build --strict`-built docs site — genuinely decodes a bundled 8-second, 3-object Atmos-in-DD+ fixture
    (`E-AC-3, 48000 Hz, 6 ch (L, C, R, Ls, Rs, LFE), 3 Atmos object(s), 8.0s`, matching what was
    encoded), plays real audio with `AudioContext.currentTime` genuinely advancing, and paints a
    speaker-ring visualization driven by real, time-varying per-channel RMS (confirmed non-degenerate
    per channel, including a genuinely-silent LFE since nothing was routed to it) that changes with
    playback position and responds to the seek bar.

    **Object decode specifically**: the same object's decoded position genuinely differs between two
    different playback timestamps (confirmed by direct comparison, not just "the code ran") and the
    room-view canvas paints real, non-empty content from it. Each "Solo object N" button was confirmed
    to switch playback to a buffer that (a) sample-for-sample matches `tanh()` of that specific
    object's own `object_audio`, (b) differs from every other object's audio, and (c) differs from the
    bed downmix — not just "some audio plays," the *correct* isolated object's audio plays.

!!! note "Encode module, verified in a real browser"
    A dropped multi-second WAV (a known tone at a known level) genuinely encodes through
    `WasmEncoder`/`QcMeter` in a real Chromium tab: the produced byte count is real (not a canned
    number), the QC verdict table's measured LUFS/true-peak values land where the known signal's
    level predicts, and every delivery preset genuinely fails against a tone far louder than any
    of their targets — proving the gate discriminates rather than always reading "pass". The
    round-trip preview genuinely decodes the just-produced bytes through the decode module (not
    the source audio replayed) and reports the right sample rate and channel count back.

!!! note "Automated in CI (roadmap VX18a)"
    `apps/wasm/tests/` is a Playwright harness `build-wasm` now runs on every push, right after the
    demo artifact uploads: two projects, one per demo, each serving its own just-built directory.
    `decode.spec.js` loads `index.html` in a real headless Chromium and drives the `WasmDecoder`
    Embind API directly (the same calls `demo.js` makes) to decode the bundled fixture and assert
    on real values — `E-AC-3, 48000 Hz, 6 channels, 3 Atmos objects, 8.0s`, and that the same
    object's decoded position genuinely differs between its first and last frame. `encode.spec.js`
    does the same for the encode module: encodes a real 997 Hz tone through `WasmEncoder`, measures
    it with `QcMeter`, asserts the true peak and every preset verdict land where that known signal
    predicts, and round-trips the result through the decode module. A regression in any of those
    numbers now fails CI rather than waiting for the next manual pass.

!!! warning "Not yet verified"
    Built and tested on a Windows host only — the toolchain file itself makes no Windows-specific
    assumption, and CI's `build-wasm`/`docs.yml` jobs both run on `ubuntu-latest`, but no macOS run
    has been attempted anywhere. The CI browser tests above cover the codec calls themselves, not
    every pixel of either page: the decode demo's real audio playback (`AudioContext.currentTime`
    advancing), speaker-ring/room-view visualizations, seek bar and "Solo object N" audio-isolation
    claim, and the encode demo's drag-and-drop zone and download button, are manual verification
    only, not a repeatable check. Mono and 5.1 WAV inputs (as opposed to the stereo case CI checks)
    are verified locally but not in CI. Real-time (microphone-capture) encoding, and any UI for
    Atmos/object authoring, are not built at all yet — see roadmap UX6.
