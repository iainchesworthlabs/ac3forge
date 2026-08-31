# WebAssembly (browser demos and package)

WASM support is not `ac3cli` ported to a browser — it is `ac3::forge` compiled to WebAssembly,
reached three ways. Two small demo apps under **`apps/wasm/`** run it client-side in a static HTML
page: a **decode** demo (roadmap F3) — load a real elementary stream, hear the decoded bed play
through the Web Audio API, watch real per-channel energy on a speaker-ring visualization, and — for
a stream carrying Atmos objects — watch each object's real decoded position (OAMD) move in a room
view and solo its own real reconstructed audio (JOC) — and, as of roadmap UX6, an **encode** demo:
drop a `.wav` file, get back a real AC-3/E-AC-3 elementary stream plus a real BS.1770
loudness/true-peak QC verdict against five delivery presets, and a round-trip preview through the
decode module. The third surface is **[`js/`](https://github.com/iainchesworthlabs/ac3forge/tree/main/js)**,
the [`ac3forge-wasm-decoder`](https://www.npmjs.com/package/ac3forge-wasm-decoder) npm package
(roadmap UX5) that turns the same decode path into a push-frame API, a realtime AudioWorklet
pipeline, and an hls.js/MSE bridge — a reusable answer to the fact that **Chrome still cannot
decode EC-3** ([video.js http-streaming#1297](https://github.com/videojs/http-streaming/issues/1297)
is open). The decode demo is a *consumer* of the package (see "What's reused, what's new" below),
not a parallel implementation of it — see [js/README.md](https://github.com/iainchesworthlabs/ac3forge/blob/main/js/README.md)
for the package's own API docs. The demos exist to prove the codec runs correctly outside a native
process and to give the documentation site live demos (see [Live decode demo](../wasm-demo.md) and
[Live encode demo](../wasm-encode-demo.md)) — not to be general-purpose in-browser tools. This page
covers what is specific to WASM; for the core library and the desktop platforms, see
[Building from source](../building.md) and the other pages in this section.

## Encode module

The decode demo's own docs used to call WASM-encode "a separate, much larger undertaking" and leave
it deliberately out of scope, reasoning from first principles about "real-time MDCT/bit-allocation/
JOC matrix work in a browser thread." Roadmap UX6 measured it instead of assuming it, on the same
WSL2/Emscripten 6.0.6 toolchain `build-wasm` uses:

- **Binary size.** A module binding the AC-3 encoder (`ac3::FrameEncoder`), the E-AC-3 encoder
  (`ac3::eac3::FrameEncoder`), the Atmos/JOC bed encoder (`ac3::oba::AtmosEncoder`) and the QC
  loudness meter (`ac3::meta::LoudnessMeter`/`evaluate_qc_gate`) together — everything
  `encoder_bindings.cpp` binds, not a cut-down subset — compiles to **390 KB raw / 153 KB gzip**,
  against the decode module's own **372 KB raw / 133 KB gzip**. Comparable order of magnitude, not
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
top of the bound `AtmosBedEncoder` is page-only work for a later PR, not a module change.

## Build and run

Two steps now: the Emscripten build (unchanged) produces the decoder itself; the npm package
build produces the JS/TS glue the demo (and any other consumer) imports.

An [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html) on `PATH`
(`source <emsdk>/emsdk_env.sh` / `emsdk_env.bat`/`.ps1`), then:

```bash
cmake --preset config-wasm-emscripten
cmake --build build/config-wasm-emscripten
```

Then build `js/` and assemble it alongside the Emscripten output — `apps/wasm/CMakeLists.txt`
only knows how to copy its own static files, so this is a plain shell step, the same one
`.github/workflows/_build.yml`'s `build-wasm` job runs:

```bash
cd js && npm ci && npm run build && cd ..
mkdir -p build/config-wasm-emscripten/bin/wasm_decode_demo/package
cp -r js/dist/. build/config-wasm-emscripten/bin/wasm_decode_demo/package/
```

The decode demo's realtime section (AudioWorklet playback) needs `SharedArrayBuffer`, which needs
cross-origin isolation - a plain `python3 -m http.server` does not send the required headers, so
that section stays disabled under it (the rest of the demo - decode, scrub, solo - works fine
without them):

```bash
cd build/config-wasm-emscripten/bin/wasm_decode_demo   # or .../wasm_encode_demo
python3 -m http.server 8000   # fetch()/WASM streaming need http(s), not file://
```

Open `http://localhost:8000/`. Each demo directory is independently servable — `wasm_encode_demo/`
carries its own copy of the decode module (for its round-trip preview) rather than assuming the
decode demo's directory is a sibling. For the decode demo's realtime section, serve with
`Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp`
response headers instead — `apps/wasm/tests/serve.js` is a small Node static server that already
sets both, and doubles as exactly that.

## What's reused, what's new

`ac3::forge` (`src/forge/`) — the codec, `FrameDecoder`/`Eac3Decoder`, elementary-stream scanning — is
fully platform-independent and is linked into both demos **unmodified**, the same way `apps/wasm/CMakeLists.txt`
links it as any other consumer would: `add_executable` + `target_link_libraries(... ac3::forge ...)`,
no fork, no `#ifdef`. Unlike `apps/android/`, this doesn't need a separate build system reached
from the other direction — WASM is a plain CMake cross-compile, so `apps/wasm/` is a normal
`add_subdirectory()` from the root `CMakeLists.txt`, gated on `EMSCRIPTEN` (set by
`cmake/toolchains/wasm.emscripten.toolchain.cmake`) rather than an `AC3FORGE_BUILD_*` option.
`ac3::audio` (`src/audio/`) gains **no** WASM backend — there is no live-capture/passthrough
equivalent to add; a browser gets audio playback from the Web Audio API in JavaScript instead, and
`src/audio` is skipped from the configure entirely under `EMSCRIPTEN` (the skip lives in the root
`CMakeLists.txt`'s `add_subdirectory` gate; `src/audio/CMakeLists.txt` itself hard-fails otherwise,
for having no browser platform directory).

`decoder_bindings.cpp` (the Embind wrapper) is new, but is now deliberately minimal: `scanStream()`
(a thin wrapper over `ac3::io::scan`) and `PushDecoder`, one `ac3::Eac3Decoder` per instance
decoding through `decode_access_unit_into`'s caller-buffer form - buffers allocated once at
construction, reused for every call, so the hot path allocates nothing on the C++ side (roadmap
UX5's explicit ask). `Eac3Decoder` alone handles every `ac3::io::StreamKind` - a plain AC-3
syncframe "comes back as substream (kIndependent, 0)" per `decode_access_unit`'s own doc comment -
so `scanStream()`'s reported kind is informational only, not something `PushDecoder` branches on.
The optional §7.8 fold (`ac3::OutputStage`/DC1, never a hand-rolled one) is applied over a small
reused copy of the just-decoded channels, so both the coded channels and the fold are available
from one decode - see `decoder_bindings.cpp`'s own `apply_fold()` comment for why it can't be done
in place. Everything the OLD whole-file Embind `Decoder` class used to accumulate itself (per-file
channel/energy buffers, object position/audio bookkeeping, the stereo fold) now lives in
`js/src/decode-file.ts`, built on top of `PushDecoder` rather than duplicating it.

`js/` is the published package: `push-decoder.ts` (the typed wrapper over the Embind class above),
`decode-file.ts` (the whole-file convenience helper the demo's scrub/solo experience needs),
`ring-buffer.ts`/`decoder-worker.ts`/`worklet-processor.ts`/`decoder-node.ts` (the realtime
AudioWorklet pipeline - decode runs in a Worker, since `AudioWorkletGlobalScope` has neither
`fetch()` nor `TextDecoder`, both of which the Emscripten glue needs; only a lock-free
`SharedArrayBuffer` ring-buffer drain runs on the audio thread itself), and `fmp4.ts`/
`hls-bridge.ts` (the hls.js/MSE bridge - see [js/README.md](https://github.com/iainchesworthlabs/ac3forge/blob/main/js/README.md)
for what that bridge does and does not cover). The package embeds no compiled `.wasm`/`.js`
binary of its own; every API takes the `createAc3ForgeModule` factory (or a URL to it) as a
parameter, so a consumer controls their own hosting/CORS story for the binary this page's build
step produces.

`index.html`/`demo.js` (the page, Web Audio playback of already-decoded PCM, the Canvas
visualizations ported from `apps/gui/qml/SoundfieldView.qml` and Main.qml's Objects tab) are the
one remaining piece specific to the demo, and are now a *consumer* of `js/` - they hold no decode
logic, no WASM-module loading, and no hand-rolled fold. The object visualization/audio is a thin
JS-facing surface over `Eac3Decoder`'s own real `object_metadata` (OAMD positions/gain,
`ac3::forge#168`) and `object_audio` (JOC-reconstructed per-object audio, `ac3::forge#169`) fields,
reached through the package rather than directly.

The encode demo follows the identical shape, one level down: `encoder_bindings.cpp` is a second,
independent Embind wrapper (its own `add_executable`, its own `EMSCRIPTEN_BINDINGS` block, its own
`EXPORT_NAME` so the two modules can load on one page without colliding), linking `ac3::forge`
**unmodified** the same way the decode target does — no fork, no `#ifdef`, confirming the "encoders
are already proven platform-free" premise this depended on (the same `ac3::forge` target already
links unmodified into `apps/android`'s NDK build and `python/`'s pybind11 module). `apps/wasm/encode/`
(`index.html`/`app.js`) is the page: a drop zone and file picker, format (AC-3/E-AC-3)/sample-rate/
bitrate controls (the channel layout is derived from the dropped WAV itself), the QC verdict
table, and the round-trip preview. It reorders a dropped WAV's WAVEFORMATEXTENSIBLE channel order
into AC-3's Table 5.8 order before encoding — see `app.js`'s own comment on the exact mapping — and
resamples via the browser's own `AudioContext`, rather than writing a sample-rate converter.

What counts as "an object" in the decode demo's room view is every JOC output, which `ac3::oba::describe_objects()` spells
out: a dynamic object supplies its own position, size and gain, and a bed channel — what
channel-based-immersive third-party content carries — is drawn at the nominal room position of the
speaker its label names, with that label on its solo button. Each object's per-frame record also
carries TS 103 420 §5.6.1.2's extent, so a sized object draws bigger than a point source.

One wrinkle worth knowing when previewing locally: `docs/assets/wasm-decode-demo/` holds a
*committed* `ac3forge_decode.wasm` (and a committed `package/`, `js/dist`'s own copy) that only the
docs deploy job rebuilds, so a local `mkdocs serve` can be running an older module/package pair
than the checked-in `demo.js`. Both sides of that pairing are rebuilt and committed together by
whoever last refreshed this directory, precisely so they stay a matched pair rather than drifting
independently. `docs/assets/wasm-encode-demo/` carries the same hazard: it holds committed
modules of its own — its copy of the decode module included — and nothing keeps the committed
copies across the two directories in step with each other except a human refreshing them all at
once.

## Toolchain

No vcpkg. Every other platform preset in `CMakePresets.json` chainloads through vcpkg for
consistency, but `ac3::forge`'s decode path has zero third-party dependencies (`vcpkg.json`'s own
description says so), so `config-wasm-emscripten`'s toolchain file goes straight to Emscripten's own
`Emscripten.cmake` — see that toolchain file's own header for why going through vcpkg's community
`wasm32-emscripten` triplet would be pure cost for nothing this preset needs.

Verified against **Emscripten 6.0.6**. No version is pinned in the toolchain file itself (unlike the
Android NDK's explicit pin) — there is no CMake-side equivalent of `local.properties`' `sdk.dir` to
pin against yet; whatever `$EMSDK` resolves to is what gets used.

## Publishing (roadmap UX5)

`ac3forge-wasm-decoder` is published to the npm registry, versioned from the same release tag the
`ac3forge` PyPI package uses (see [docs/releasing.md](../releasing.md#publishing-to-npm)) —
`js/package.json` carries a `0.0.0-dev` placeholder in the tree, and the release workflow stamps
the real version immediately before publishing, mirroring CMake's own untagged-build fallback.
**Publishing is gated on a not-yet-provisioned `npm` GitHub environment** — the workflow job
exists (`release.yml`'s `publish-npm`) but stays unrunnable until the one-time npmjs.com trusted-
publisher setup `docs/releasing.md` describes is done by hand, the same way the `pypi` environment
was provisioned for PyPI.

## Release / CI

The demos build alongside the desktop packages rather than only ever being hand-built locally:
`.github/workflows/_build.yml`'s `build-wasm` job configures and builds both (one `cmake --build`
over the whole preset) on every push, the same continuous-smoke-test role `build-android`'s
always-on debug APK plays — proving the Emscripten toolchain and every file it touches still build.
Like `build-android`, it's its own job rather than a `build` matrix entry: this leg has no ctest
suite, no cpack package and no gold-reference gate, so folding it into that matrix would mean
threading new `if:` exclusions through most of that job's steps for no benefit. The same job also
builds and tests `js/` (`npm ci && npm run build && npm test`) *before* the Emscripten build, since
the decode demo's servable directory needs `js/dist/` copied in alongside the compiled decoder (see
Build and run above) — `js/`'s own `node:test` suite (the fMP4 box walker against a real fixture,
the ring buffer, the `MediaSource` shim) runs there too.

**The published demos are rebuilt fresh, not shipped from committed copies.** `docs/assets/wasm-decode-demo/`
and `docs/assets/wasm-encode-demo/` are committed to the repo as working fallbacks (so a plain local
`mkdocs build` — or this repo's own PR-time docs check — still has something to embed without anyone
needing Emscripten installed just to preview docs), but `.github/workflows/docs.yml`'s `deploy` job
(push to `main` only) installs Emscripten, rebuilds `apps/wasm/` from source, and overwrites both
directories *before* `mkdocs gh-deploy` runs — so what actually reaches the live site always
reflects current source, never a possibly-stale commit. Both jobs share one Emscripten install step,
`.github/actions/setup-emscripten` (pinned to the same version this page's Toolchain section names),
so the two never drift onto different SDK versions.

The committed fallbacks are not immune to going stale, though: nothing rewrites them except a human
manually re-copying `apps/wasm/`'s output, and `docs.yml`'s own `deploy` job overwrites its
working copy in a throwaway CI workspace rather than committing the refresh back. `docs.yml`'s
`build` job (run for any PR that touches the docs or the WASM sources — see the trigger `paths:`
list below — `mkdocs build --strict`) therefore also byte-compares
`index.html`, `demo.js`, the two favicon files and `assets/demo.ec3` against their `apps/wasm/`
originals, and does the same for the encode demo's `encode/index.html`/`encode/app.js` and its
own favicon copies against `docs/assets/wasm-encode-demo/` — the plain copies, not Emscripten
output, so the check needs no toolchain. The `.js`/`.wasm` build artifacts (both modules) have no source-tree counterpart and are
outside this check's scope; they only get refreshed by an actual Emscripten rebuild.

`docs.yml`'s trigger `paths:` list includes `apps/wasm/**`, `CMakeLists.txt`,
`CMakePresets.json`, `.github/actions/setup-emscripten/**` and the WASM toolchain file
specifically — without them, a source change there
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
    the bound `Encoder`/`QcMeter` in a real Chromium tab: the produced byte count is real (not a canned
    number), the QC verdict table's measured LUFS/true-peak values land where the known signal's
    level predicts, and every delivery preset genuinely fails against a tone far louder than any
    of their targets — proving the gate discriminates rather than always reading "pass". The
    round-trip preview genuinely decodes the just-produced bytes through the decode module (not
    the source audio replayed) and reports the right sample rate and channel count back.

!!! note "Automated in CI (roadmap VX18a)"
    `apps/wasm/tests/` is a Playwright harness `build-wasm` now runs on every push, right after the
    demo artifact uploads: two projects, one per demo, each serving its own just-built directory.
    `decode.spec.js` loads `index.html` in a real headless Chromium and drives the real published
    package (`js/`'s `decodeFile()` and `Ac3ForgeDecoderNode` — the same calls `demo.js` itself
    makes) to decode the bundled fixture and assert on real values — `48000 Hz, 6 channels,
    3 Atmos objects, 8.0s`, that the same object's decoded position genuinely differs between its
    first and last frame, and — new for roadmap UX5 — that the AudioWorklet pipeline (a real Worker
    doing the WASM decode, a real `SharedArrayBuffer` ring buffer, a real `AudioWorkletNode`)
    produces genuinely non-silent decoded audio out an `OfflineAudioContext`, not just "the worker
    didn't throw". `encode.spec.js` does the same for the encode module: encodes a real 997 Hz tone
    through the bound `Encoder`, measures it with `QcMeter`, asserts the true peak and every preset
    verdict land where that known signal predicts, and round-trips the result through the decode
    module. A regression in any of those numbers now fails CI rather than waiting for the next
    manual pass.

!!! note "Verified locally while building UX5 (this repository's own Windows host, Emscripten 6.0.6)"
    `decoder_bindings.cpp`'s rewrite (the old whole-file `Decoder` class replaced by
    `scanStream()`/`PushDecoder`) was built and linked clean, and both decode Playwright specs
    (the whole-file `decodeFile()` path and the new AudioWorklet pipeline) passed against that
    real build, not just against source review. `js/`'s own `node:test` suite — the fMP4 box
    walker against a real ffmpeg-remuxed fixture (every extracted sample landing exactly on an
    AC-3/E-AC-3 syncword), the ring buffer's wraparound/underrun/overrun arithmetic, and the
    `MediaSource`/`addSourceBuffer` shim's mechanics against a fake `MediaSource` stub — passed
    as well. None of this was CI at the time (local verification during development);
    `build-wasm` now runs the same Playwright specs and `js/` test suite as its own CI leg.

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

    **The hls.js/MSE bridge** (`js/src/hls-bridge.ts`) has no live-HLS-server soak test behind
    it — its `MediaSource` shim mechanics and its fMP4 sample extraction are each unit-tested in
    isolation (see the note above), but the full integration against a real hls.js instance
    playing a real EC-3 HLS stream has not been attempted. See
    [js/README.md](https://github.com/iainchesworthlabs/ac3forge/blob/main/js/README.md#whats-verified)
    for the same gap stated from the package's own side, including the A/V-sync approximation
    it ships with.

    **The decode demo's realtime section's pacing** (`apps/wasm/demo.js`'s `setTimeout`-based push
    loop, simulating a live feed from the bundled file) is a demo simplification that a genuinely
    backgrounded browser tab can starve — Chrome throttles `setTimeout` heavily once a tab is
    hidden, while the (unthrottled) audio graph keeps consuming, which can read as a stuck
    "buffer underrun" status. This is specific to that synthetic pacing loop, not to the
    AudioWorklet pipeline itself (which the Playwright spec above exercises without any timer
    dependency) or to a real integration (which pushes units as its own transport delivers them,
    not on a per-frame timer).
