# Live decode demo (WASM)

`iclforge::ac3`'s decoder, compiled to WebAssembly, decoding an E-AC-3
elementary stream entirely in your browser — no server-side decode, no
upload. This is the same C++ decode path `forge decode` uses, running as
WASM instead of a native binary.

<!-- The iframe src below is raw HTML (md_in_html), which mkdocs passes through
     verbatim rather than rewriting the way it rewrites real markdown links -
     it has to be relative to this PAGE's own built URL (wasm-demo/index.html,
     directory URLs are on), hence "../". The markdown link further down is
     the opposite case: mkdocs rewrites markdown-syntax links itself, relative
     to this SOURCE file's own location (docs/wasm-demo.md, at the docs root),
     so it does NOT get a "../" even though it points at the same target. -->
<div style="border:1px solid var(--md-default-fg-color--lightest); border-radius:0.4em; overflow:hidden;">
  <iframe
    src="../assets/wasm-decode-demo/index.html"
    title="ICL Forge WASM decode demo"
    style="width:100%; height:1300px; border:0; display:block;"
    loading="lazy">
  </iframe>
</div>

[Open the demo in its own tab](assets/wasm-decode-demo/index.html){ target="_blank" }

## What this demonstrates

The decode and the audio both come from `iclforge::ac3`'s own decode path, running as WASM.
The per-channel bed energy drives the two speaker rings (solid = ear-level, dashed = ceiling —
ported from the desktop GUI's `SoundfieldView.qml`). For a stream carrying Atmos objects, each
object's decoded position (OAMD,
[`iclforge::ac3#168`](https://github.com/iainchesworthlabs/ac3forge/pull/168)) moves in the
top-down/elevation room view, and a "solo object" control plays that object's own
JOC-reconstructed audio ([`iclforge::ac3#169`](https://github.com/iainchesworthlabs/ac3forge/pull/169)) —
its isolated waveform, decoded from the bitstream, rather than a re-panned approximation of its
slice of the bed. Drop in your own `.ac3`, `.ec3` or `.eac3` file to decode something other than
the bundled fixture; a plain (non-Atmos) stream simply has zero objects.

The page decodes AC-3 and E-AC-3 only. The AC-4 decoder is a separate WebAssembly module,
`iclforge_wasm_ac4`, built in the same CI job; it has no demo page yet. See
[WebAssembly → AC-4 module](platforms/wasm.md#ac-4-module).

## Third-party notices

The module this page loads carries more than `iclforge::ac3`. It statically links **{fmt}**
(`cmake/Fmt.cmake` pins 12.2.0, and the committed `iclforge_decode.wasm` carries
`fmt::v12::format_error`'s mangled RTTI name), which is distributed under the MIT licence, whose
text is in this repository at
[`apps/crucible/notices/licences/MIT-fmt.txt`](https://github.com/iainchesworthlabs/ac3forge/blob/main/apps/crucible/notices/licences/MIT-fmt.txt).
The module also carries the Emscripten runtime and the C++ standard library that toolchain
supplies, each under its own licence.

Crucible's packages carry a generated `NOTICES.txt` for this reason; the servable demo directory
does not yet, so this section stands in for one.

## Source and how it's built

Source: [`apps/wasm/`](https://github.com/iainchesworthlabs/ac3forge/tree/main/apps/wasm) —
see [WebAssembly](platforms/wasm.md) for the build/toolchain details and what's reused vs. new. CI
rebuilds this embed fresh from source on every deploy to `main`; see
[Release / CI](platforms/wasm.md#release-ci).

This page is a consumer of `iclforge-wasm-decoder`
(source: [`js/`](https://github.com/iainchesworthlabs/ac3forge/tree/main/js)) rather
than a parallel implementation of its own: the decode, the §7.8 fold and (in the demo's realtime
section further down) the AudioWorklet playback pipeline all come from that package.

**`iclforge-wasm-decoder` has never been published to npm.** The `publish` job in
`.github/workflows/npm.yml` is restricted to a manual `workflow_dispatch`, so pushing a release
tag packs the tarball as a CI artefact and stops there; there is no release of it to install. No
date is set for that changing, and [WebAssembly →
Publishing](platforms/wasm.md#publishing) has the two things that must be cleared
first.

So what a reader can do today is one of two things. Use the demo above, which runs the decoder in
your browser with nothing to install. Or build the package from source: clone the repository, then
`cd js && npm ci && npm run build` — the same install and build CI runs in `js/` on every pull
request that touches it — and point your project at the resulting `js/dist/`. The package ships no
compiled `.wasm` of its own: `decodeFile()` and `PushDecoder` take the instantiated Embind module
as their first argument, and `IclForgeDecoderNode` takes a `wasmGlueUrl` pointing at the Emscripten
glue, so you also need the module built from `apps/wasm/`; [WebAssembly](platforms/wasm.md) covers
that build. The [README](https://github.com/iainchesworthlabs/ac3forge/blob/main/js/README.md) is
the package's own API documentation. The package exists at all because a browser cannot be
relied on to decode EC-3: [Chrome reports a decoder error](https://github.com/videojs/http-streaming/issues/1297)
when an EC-3 track turns up in an MPD, in a report that has been open since 2023.
