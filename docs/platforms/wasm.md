# WebAssembly (browser decode demo and package)

WASM support is not `ac3cli` ported to a browser — it is `ac3::forge`'s AC-3/E-AC-3 decoder
compiled to WebAssembly, reached two ways: **`apps/wasm/`**, a decode-only demo app that runs it
client-side in a static HTML page (load a real elementary stream, hear the decoded bed play
through the Web Audio API, watch real per-channel energy on a speaker-ring visualization, and —
for a stream carrying Atmos objects — watch each object's real decoded position (OAMD) move in a
room view and solo its own real reconstructed audio (JOC)); and **[`js/`](https://github.com/iainchesworthlabs/ac3forge/tree/main/js)**,
the [`ac3forge-wasm-decoder`](https://www.npmjs.com/package/ac3forge-wasm-decoder) npm package
(roadmap UX5) that turns the same decode path into a push-frame API, a realtime AudioWorklet
pipeline, and an hls.js/MSE bridge — a reusable answer to the fact that **Chrome still cannot
decode EC-3** ([video.js http-streaming#1297](https://github.com/videojs/http-streaming/issues/1297)
is open). The demo is a *consumer* of the package (see "What's reused, what's new" below), not a
parallel implementation of it — see [js/README.md](https://github.com/iainchesworthlabs/ac3forge/blob/main/js/README.md)
for the package's own API docs. This page covers what is specific to WASM; for the core library
and the desktop platforms, see [Building from source](../building.md) and the other pages in this
section.

Decode-only, deliberately: WASM-encode is a separate, much larger undertaking (real-time MDCT/bit-
allocation/JOC matrix work in a browser thread) and isn't attempted here.

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

The demo's realtime section (AudioWorklet playback) needs `SharedArrayBuffer`, which needs
cross-origin isolation - a plain `python3 -m http.server` does not send the required headers, so
that section stays disabled under it (the rest of the demo - decode, scrub, solo - works fine
without them):

```bash
cd build/config-wasm-emscripten/bin/wasm_decode_demo
python3 -m http.server 8000   # ES modules and fetch() need http(s), not file://
```

Open `http://localhost:8000/`. For the realtime section too, serve with
`Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp`
response headers instead — `apps/wasm/tests/serve.js` is a small Node static server that already
sets both, and doubles as exactly that.

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

What counts as "an object" there is every JOC output, which `ac3::oba::describe_objects()` spells
out: a dynamic object supplies its own position, size and gain, and a bed channel — what
channel-based-immersive third-party content carries — is drawn at the nominal room position of the
speaker its label names, with that label on its solo button. Each object's per-frame record also
carries TS 103 420 §5.6.1.2's extent, so a sized object draws bigger than a point source.

One wrinkle worth knowing when previewing locally: `docs/assets/wasm-decode-demo/` holds a
*committed* `ac3forge_decode.wasm` (and a committed `package/`, `js/dist`'s own copy) that only the
docs deploy job rebuilds, so a local `mkdocs serve` can be running an older module/package pair
than the checked-in `demo.js`. Both sides of that pairing are rebuilt and committed together by
whoever last refreshed this directory, precisely so they stay a matched pair rather than drifting
independently.

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

The demo builds alongside the desktop packages rather than only ever being hand-built locally:
`.github/workflows/_build.yml`'s `build-wasm` job configures and builds it on every push, the same
continuous-smoke-test role `build-android`'s always-on debug APK plays — proving the Emscripten
toolchain and every file it touches still build. Like `build-android`, it's its own job rather than
a `build` matrix entry: this leg has no ctest suite, no cpack package and no gold-reference gate, so
folding it into that matrix would mean threading new `if:` exclusions through most of that job's
steps for no benefit. The same job also builds and tests `js/` (`npm ci && npm run build && npm
test`) *before* the Emscripten build, since the demo's servable directory needs `js/dist/` copied
in alongside the compiled decoder (see Build and run above) — `js/`'s own `node:test` suite (the
fMP4 box walker against a real fixture, the ring buffer, the `MediaSource` shim) runs there too.

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
`index.html`, `demo.js`, the two favicon files and `assets/demo.ec3` against their
`apps/wasm/` originals — the plain copies, not Emscripten output, so the check needs no
toolchain. `ac3forge_decode.js`/`ac3forge_decode.wasm` are genuine build artifacts with no
source-tree counterpart and are outside this check's scope; they only get refreshed by an actual
Emscripten rebuild.

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

!!! note "Automated in CI (roadmap VX18a)"
    `apps/wasm/tests/` is a small Playwright harness `build-wasm` now runs on every push, right
    after the demo artifact upload: it serves the just-built `wasm_decode_demo/` directory,
    loads `index.html` in a real headless Chromium, and drives the real published package
    (`js/`'s `decodeFile()` and `Ac3ForgeDecoderNode` — the same calls `demo.js` itself makes) to
    decode the bundled fixture and assert on the real values the first note above once had to be
    checked by eye — `E-AC-3, 48000 Hz, 6 channels, 3 Atmos objects, 8.0s`, that the same object's
    decoded position genuinely differs between its first and last frame, and — new for roadmap
    UX5 — that the AudioWorklet pipeline (a real Worker doing the WASM decode, a real
    `SharedArrayBuffer` ring buffer, a real `AudioWorkletNode`) produces genuinely non-silent
    decoded audio out an `OfflineAudioContext`, not just "the worker didn't throw". A regression
    in any of those numbers now fails CI rather than waiting for the next manual pass.

!!! note "Verified locally while building UX5 (this repository's own Windows host, Emscripten 6.0.6)"
    `decoder_bindings.cpp`'s rewrite (the old whole-file `Decoder` class replaced by
    `scanStream()`/`PushDecoder`) was built and linked clean, and both Playwright specs above
    (the whole-file `decodeFile()` path and the new AudioWorklet pipeline) passed against that
    real build, not just against source review. `js/`'s own `node:test` suite — the fMP4 box
    walker against a real ffmpeg-remuxed fixture (every extracted sample landing exactly on an
    AC-3/E-AC-3 syncword), the ring buffer's wraparound/underrun/overrun arithmetic, and the
    `MediaSource`/`addSourceBuffer` shim's mechanics against a fake `MediaSource` stub — passed
    as well. None of this is CI running yet (this was local verification during development);
    `build-wasm` runs the same Playwright specs and `js/` test suite as its own CI leg.

!!! warning "Not yet verified"
    Built and tested on a Windows host only — the toolchain file itself makes no Windows-specific
    assumption, and CI's `build-wasm`/`docs.yml` jobs both run on `ubuntu-latest`, but no macOS run
    has been attempted anywhere. The CI browser test above covers the decode itself, not the page
    around it: real audio playback (`AudioContext.currentTime` advancing), the speaker-ring and
    room-view visualizations, the seek bar, and the "Solo object N" buttons' own audio-isolation
    claim are still manual verification only, not a repeatable check.

    **The hls.js/MSE bridge** (`js/src/hls-bridge.ts`) has no live-HLS-server soak test behind
    it — its `MediaSource` shim mechanics and its fMP4 sample extraction are each unit-tested in
    isolation (see the note above), but the full integration against a real hls.js instance
    playing a real EC-3 HLS stream has not been attempted. See
    [js/README.md](https://github.com/iainchesworthlabs/ac3forge/blob/main/js/README.md#whats-verified)
    for the same gap stated from the package's own side, including the A/V-sync approximation
    it ships with.

    **The demo's realtime section's pacing** (`apps/wasm/demo.js`'s `setTimeout`-based push loop,
    simulating a live feed from the bundled file) is a demo simplification that a genuinely
    backgrounded browser tab can starve — Chrome throttles `setTimeout` heavily once a tab is
    hidden, while the (unthrottled) audio graph keeps consuming, which can read as a stuck
    "buffer underrun" status. This is specific to that synthetic pacing loop, not to the
    AudioWorklet pipeline itself (which the Playwright spec above exercises without any timer
    dependency) or to a real integration (which pushes units as its own transport delivers them,
    not on a per-frame timer).
