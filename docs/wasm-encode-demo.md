# Live encode demo (WASM)

`ac3::forge`'s AC-3/E-AC-3 encoder, compiled to WebAssembly, encoding a `.wav` file you drop in
entirely in your browser — no server-side encode, no upload. This is the same C++ encode path
`ac3cli encode` uses, running as WASM instead of a native binary, alongside a real BS.1770
loudness/true-peak QC verdict against five delivery presets — the same measurement `ac3cli qc`
makes.

<!-- Same iframe/link relative-path split as wasm-demo.md's own comment explains: the iframe src is
     raw HTML mkdocs passes through verbatim (relative to this page's own built URL,
     wasm-encode-demo/index.html), the link below is markdown mkdocs rewrites itself (relative to
     this source file's own location, docs/wasm-encode-demo.md). -->
<div style="border:1px solid var(--md-default-fg-color--lightest); border-radius:0.4em; overflow:hidden;">
  <iframe
    src="../assets/wasm-encode-demo/index.html"
    title="ac3forge WASM encode demo"
    style="width:100%; height:900px; border:0; display:block;"
    loading="lazy">
  </iframe>
</div>

[Open the demo in its own tab](assets/wasm-encode-demo/index.html){ target="_blank" }

## What's real

Everything: dropping a `.wav` decodes it through the browser's own `AudioContext`, encodes it
frame by frame through the real `ac3::FrameEncoder`/`ac3::eac3::FrameEncoder`, measures the same
PCM with the real `ac3::meta::LoudnessMeter`, and evaluates it against
[`ac3cli qc`](cli/commands.md)'s own five delivery presets
(`ac3::meta::evaluate_qc_gate`) — a loud file genuinely fails every preset, a properly-mastered one
genuinely passes the presets it meets. The round-trip preview decodes the bytes this page just
produced through the existing [decode demo](wasm-demo.md)'s own module and plays them back — proof
the encoded stream is real and decodable, not just "some bytes came out."

## Source and how it's built

Source: [`apps/wasm/encode/`](https://github.com/iainchesworthlabs/ac3forge/tree/main/apps/wasm/encode)
(the page) and
[`apps/wasm/encoder_bindings.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/apps/wasm/encoder_bindings.cpp)
(the Embind wrapper) — see [WebAssembly](platforms/wasm.md#encode-module) for the measured
size/real-time numbers and what's reused vs. new. CI rebuilds this embed fresh from source on every
deploy to `main`, alongside the [decode demo](wasm-demo.md); see
[Release / CI](platforms/wasm.md#release-ci).
