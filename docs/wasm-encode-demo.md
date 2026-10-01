# Live encode demo (WASM)

`iclforge::ac3`'s AC-3/E-AC-3 encoder, compiled to WebAssembly, encoding a `.wav` file you drop in,
or audio you record from the microphone, entirely in your browser — no server-side encode, no
upload. This is the same C++ encode path
`forge encode` uses, running as WASM instead of a native binary, alongside a BS.1770
loudness/true-peak QC verdict against five delivery presets — the same measurement `forge qc`
makes.

<!-- Same iframe/link relative-path split as wasm-demo.md's own comment explains: the iframe src is
     raw HTML mkdocs passes through verbatim (relative to this page's own built URL,
     wasm-encode-demo/index.html), the link below is markdown mkdocs rewrites itself (relative to
     this source file's own location, docs/wasm-encode-demo.md). -->
<div style="border:1px solid var(--md-default-fg-color--lightest); border-radius:0.4em; overflow:hidden;">
  <iframe
    src="../assets/wasm-encode-demo/index.html"
    title="ICL Forge WASM encode demo"
    style="width:100%; height:900px; border:0; display:block;"
    loading="lazy">
  </iframe>
</div>

[Open the demo in its own tab](assets/wasm-encode-demo/index.html){ target="_blank" } ·
[Atmos object-authoring page](assets/wasm-encode-demo/atmos/index.html){ target="_blank" } — pan
audio objects around a room canvas and encode the result as E-AC-3 + JOC, live

## What this demonstrates

Dropping a `.wav` decodes it through the browser's own `AudioContext`, which resamples it to the
chosen coding rate (32, 44.1 or 48 kHz). The page then makes two passes. The first measures the PCM
with `iclforge::ac3::meta::LoudnessMeter` and derives the stream's dialnorm from the integrated loudness. The
second encodes it frame by frame through `iclforge::ac3::FrameEncoder` (AC-3) or `iclforge::ac3::eac3::FrameEncoder`
(E-AC-3), with that dialnorm in every frame. The same measurement is evaluated against
[`forge qc`](forge/cli/commands.md)'s own five delivery presets (`iclforge::ac3::meta::evaluate_qc_gate`): a
loud file fails every preset, a properly-mastered one passes the presets it meets.

Mono, stereo and 5.1 files encode as either format. The wide layouts, 7.1, 5.1.4 and 7.1.4 (8, 10
and 12 channels), need E-AC-3, which codes them as a 5.1 bed plus dependent substreams. "Record
from microphone" runs the same encoder live, after measuring about a second and a half of the
microphone for the dialnorm.

The round-trip preview decodes the bytes this page just produced through the existing
[decode demo](wasm-demo.md)'s own module and plays them back, so the encoded stream can be
checked, not just assumed.

The page encodes AC-3 and E-AC-3 only. `iclforge_wasm_ac4`, a separate module, wraps the AC-4
encoder and decoder and has no demo page yet. See
[WebAssembly → AC-4 module](platforms/wasm.md#ac-4-module).

## Third-party notices

This page loads two modules, the encoder and the decoder its round-trip preview uses, and both
carry more than `iclforge::ac3`. Each statically links **{fmt}** (`cmake/Fmt.cmake` pins 12.2.0, and
the committed `iclforge_encode.wasm` and `iclforge_decode.wasm` both carry
`fmt::v12::format_error`'s mangled RTTI name), which is distributed under the MIT licence, whose
text is in this repository at
[`apps/crucible/notices/licences/MIT-fmt.txt`](https://github.com/iainchesworthlabs/iclforge/blob/main/apps/crucible/notices/licences/MIT-fmt.txt).
Both also carry the Emscripten runtime and the C++ standard library that toolchain supplies, each
under its own licence.

Crucible's packages carry a generated `NOTICES.txt` for this reason; the servable demo directory
does not yet, so this section stands in for one.

## Source and how it's built

Source: [`apps/wasm/encode/`](https://github.com/iainchesworthlabs/iclforge/tree/main/apps/wasm/encode)
(the page) and
[`apps/wasm/encoder_bindings.cpp`](https://github.com/iainchesworthlabs/iclforge/blob/main/apps/wasm/encoder_bindings.cpp)
(the Embind wrapper) — see [WebAssembly](platforms/wasm.md#encode-module) for the measured
size/real-time numbers and what's reused vs. new. CI rebuilds this embed fresh from source on every
deploy to `main`, alongside the [decode demo](wasm-demo.md); see
[Release / CI](platforms/wasm.md#release-ci).
