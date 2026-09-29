# ac3forge-wasm-decoder

A streaming AC-3/E-AC-3 (Dolby Digital/Digital Plus) decoder for the browser, compiled from
[`ac3::forge`](https://github.com/iainchesworthlabs/ac3forge) to WebAssembly. The
`ac3forge-wasm-decoder/ac4` export decodes and encodes AC-4 ([AC-4](#ac-4)).
Built because a browser cannot be relied on to decode EC-3:
[Chrome reports a decoder error](https://github.com/videojs/http-streaming/issues/1297) when an EC-3
track turns up in an MPD, in a report that has been open since 2023. This package is an embeddable
decoder for it.

Four pieces for AC-3 and E-AC-3:

- **A push-frame decode API** (`PushDecoder`) over `ac3::Eac3Decoder::decode_access_unit_into`'s
  caller-buffer form - the hot path allocates nothing on the C++ side.
- **An `Ac3ForgeDecoderNode`**: an `AudioWorkletNode`. Decoding runs in a Worker (off the
  main thread); the audio-rendering thread itself only drains a `SharedArrayBuffer` ring buffer.
- **Multichannel output, or the §7.8 downmix** (Lo/Ro, Lt/Rt, mono) - `ac3::OutputStage`, the
  library's own output stage, never a hand-rolled fold.
- **An hls.js/MSE bridge** for playing EC-3 in browsers that cannot decode it natively.

This package embeds no compiled `.wasm`/`.js` binary of its own - every API here takes the
`createAc3ForgeModule` factory (or a URL to it) as a parameter. Build it from
[`apps/wasm/`](https://github.com/iainchesworthlabs/ac3forge/tree/main/apps/wasm) in the main
repository (see [docs/platforms/wasm.md](https://iainchesworthlabs.github.io/ac3forge/platforms/wasm/))
and host the resulting `ac3forge_decode.js`/`.wasm` yourself - the same way most WASM packages let
you control your own CORS/CDN story instead of assuming a bundler will do it for you.

## Install

The package is not on the npm registry. Build it from a checkout of the main repository and
install the directory:

```bash
git clone https://github.com/iainchesworthlabs/ac3forge
cd ac3forge/js
npm ci          # TypeScript is the only dependency
npm run build   # compiles src/ to dist/
npm test        # optional: the unit tests, under the coverage floors listed below
```

then, in your own project:

```bash
npm install /path/to/ac3forge/js
```

## Loading the WASM module

Load `ac3forge_decode.js` as a classic script (it defines a global `createAc3ForgeModule`
factory - see `apps/wasm/CMakeLists.txt`'s `-sMODULARIZE=1 -sEXPORT_NAME=createAc3ForgeModule`):

```html
<script src="/path/to/ac3forge_decode.js"></script>
```

```ts
const module = await createAc3ForgeModule();
```

## Whole-file decode

For a complete file already in memory - scrubbing, per-object solo playback, anything that needs
random access into the whole programme:

```ts
import { decodeFile, DownmixTarget } from "ac3forge-wasm-decoder";

const bytes = new Uint8Array(await (await fetch("clip.ec3")).arrayBuffer());
const program = decodeFile(module, bytes, {
  fold: { target: DownmixTarget.LoRo, applyDialnorm: true },
});
// program.channels: Float32Array[] (coded/rendered channels)
// program.fold: Float32Array[] (the §7.8 fold you asked for, e.g. 2ch Lo/Ro)
// program.objectPositions / program.objectAudio: OAMD positions and JOC-reconstructed audio,
// present if the stream carries Atmos objects
```

## Push-frame decode

For a live/streaming source (your own transport, a container demuxer, the hls.js bridge below):

```ts
import { PushDecoder, scanStream, DownmixTarget } from "ac3forge-wasm-decoder";

const decoder = new PushDecoder(module, { target: DownmixTarget.AsCoded });
// unit: one AC-3 syncframe, or one E-AC-3 access unit (an independent substream plus its
// dependents) - exactly what scanStream()'s accessUnits give you for a whole file, or what
// your own demuxer already delimits for a live source.
const outcome = decoder.push(unit);
if (outcome.ok && !outcome.holdBack) {
  for (let ch = 0; ch < outcome.channelCount; ch++) {
    const pcm = decoder.channel(ch); // zero-copy view, valid until the next push()/flush() call
  }
}
decoder.close(); // release the underlying WASM object when done
```

## Realtime playback (AudioWorklet)

```ts
import { Ac3ForgeDecoderNode, DownmixTarget } from "ac3forge-wasm-decoder";

const audioContext = new AudioContext();
const node = await Ac3ForgeDecoderNode.create(audioContext, {
  workletProcessorUrl: new URL("ac3forge-wasm-decoder/worklet-processor", import.meta.url),
  workerUrl: new URL("ac3forge-wasm-decoder/decoder-worker", import.meta.url),
  wasmGlueUrl: "/path/to/ac3forge_decode.js",
  fold: { target: DownmixTarget.LoRo, applyDialnorm: true },
});
node.node.connect(audioContext.destination);
node.addEventListener("streaminfo", (e) => console.log(e.detail));
node.addEventListener("underrun", (e) => console.warn("underrun", e.detail));

// As access units arrive from your own transport:
node.pushAccessUnit(unit);
```

`workletProcessorUrl`/`workerUrl` need to resolve to actual servable URLs for this package's
compiled `worklet-processor.js`/`decoder-worker.js` - a bundler resolves
`new URL("ac3forge-wasm-decoder/...", import.meta.url)` into a real asset automatically; a plain
static site can instead copy `node_modules/ac3forge-wasm-decoder/dist/*` next to its own script
and point directly at those files.

**Requires cross-origin isolation** (`Cross-Origin-Opener-Policy: same-origin`,
`Cross-Origin-Embedder-Policy: require-corp` response headers) - `SharedArrayBuffer` is
unavailable otherwise, and `Ac3ForgeDecoderNode.create()` throws a clear error rather than
failing silently when it's missing.

## AC-4

The `ac3forge-wasm-decoder/ac4` subpath exports `Ac4Decoder` and `Ac4Encoder`, typed wrappers over
the AC-4 Embind module `apps/wasm/` builds as `ac3forge_ac4.js` (`loadAc4Module(glueUrl)` loads
it). The encoder writes channel-based content or one object substream of A-JOC or direct-coded
objects, with each object's metadata and the changes to it given beside the PCM; the decoder
returns each object's properties and the block updates within a frame. Every field an options
object leaves out keeps the C++ default.

```ts
import { Ac4Encoder, Ac4Decoder, loadAc4Module } from "ac3forge-wasm-decoder/ac4";

const module = await loadAc4Module("/wasm/ac3forge_ac4.js");
const encoder = new Ac4Encoder(module, {
  bitrateKbps: 256,
  experimental: { objects: true },
  objects: { objects: [{ properties: { position: [0.1, 0.2, 0], gainDb: -3 } }, { lfe: true }] },
});
if (encoder.constructionError) throw new Error(encoder.constructionError);
// One Float32Array per object; from sample 5000 of this call, object 0 moves over 1024 samples.
const frames = encoder.encode(pcm, [
  { object: 0, sample: 5000, rampSamples: 1024, properties: { position: [0.75, 0.25, 0.4] } },
]);
const decoder = new Ac4Decoder(module);
const decoded = decoder.decodeFrame(frames[0].data); // null until a frame has output
```

`decodeFrame` takes one raw frame (an MP4 sample, or the payload of a sync frame), and its PCM is a
view into the WASM heap that lasts until the next call. `Ac4Decoder` also has `setOutput()`,
`setPresentation()`, `reset()`, `presentations`, `refusalReason` and `latencySamples`; `Ac4Encoder`
has `flush()`, `error`, `codecMode`, `delaySamples`, `decoderDelaySamples`, `buildDac4()` and
`dac4Refusal()`; `syncFrame()` wraps a raw frame for a `.ac4` file. Call `close()` on either class
when done: Embind objects are not garbage collected.

The module builds in CI only where an Emscripten SDK is installed, and no demo page serves it.

## hls.js/MSE bridge

hls.js's own `BufferController` drops the audio track entirely the moment
`MediaSource.addSourceBuffer('audio/mp4;codecs="ec-3"')` throws - which it does in a browser that
cannot decode EC-3 through MSE. A passive event listener never gets a chance to run.
`installMediaSourceShim`/`attachHlsAudioBridge` instead patch `MediaSource.isTypeSupported`/
`addSourceBuffer` so hls.js believes the codec is supported and keeps demuxing/scheduling audio
normally, diverting the real segment bytes to this package's decoder instead of a real
`SourceBuffer`:

```ts
import { attachHlsAudioBridge, Ac3ForgeDecoderNode } from "ac3forge-wasm-decoder";

const decoderNode = await Ac3ForgeDecoderNode.create(audioContext, { /* ... */ });
decoderNode.node.connect(audioContext.destination);

// Install the shim BEFORE constructing Hls - it needs to see MediaSource.isTypeSupported
// report EC-3 as playable during hls.js's own codec-support checks.
const uninstall = attachHlsAudioBridge({ decoderNode });
const hls = new Hls();
hls.loadSource(manifestUrl);
hls.attachMedia(videoElement); // video still decodes natively; only audio is diverted
```

**What this bridge does and does not give you:**

- It extracts AC-3/E-AC-3 access units from the fMP4 segments hls.js's own remuxer produces
  (`fmp4.ts` - a minimal ISOBMFF box walker, not a general MP4 parser) and feeds them to the
  push-frame decoder for playback.
- A/V sync is `syncTo()`'s clock alignment against the host media element on play/pause/seek - an
  **approximation**, not sample-accurate mux-level sync.
- `buffered` range reporting back to hls.js is derived from each fragment's own `tfdt`/`trun`
  timing, trusting that every sample decoded rather than confirming each one did.

## What's verified

- The push-frame API and the AudioWorklet ring-buffer pipeline: unit-tested (`ring-buffer.test.js`)
  and exercised end-to-end in headless Chromium by `apps/wasm/tests/` (the main repository's CI).
- `fmp4.ts`'s box walker: unit-tested against an ffmpeg-remuxed fragmented-MP4 fixture
  (`fmp4.test.js`), asserting every extracted sample lands exactly on an AC-3/E-AC-3 syncword.
- The `MediaSource`/`addSourceBuffer` shim's mechanics: unit-tested against a fake `MediaSource`
  stub (`hls-bridge.test.js`), and `attachHlsAudioBridge` against the same fMP4 fixture split into
  init and media segments (`hls-bridge-attach.test.js`).
- The TypeScript around the WASM module - `PushDecoder`, `decodeFile`, the decode worker, the
  worklet processor and `Ac3ForgeDecoderNode` - unit-tested in Node against a scripted stand-in for
  the Embind module and fake Worker/Web Audio globals (`tests/fake-embind.js`). These check the
  wrappers' own logic; decoding itself is covered by the C++ suite.
- The AC-4 wrappers (`loadAc4Module`, `Ac4Decoder`, `Ac4Encoder`): unit-tested in Node against the
  same kind of stand-in for the AC-4 Embind module (`ac4.test.js`), and `package-exports.test.js`
  checks the `exports` map, `./ac4` included. No browser test loads the compiled
  `ac3forge_ac4.wasm` yet.
- `npm test` fails under 95% line, 90% branch or 95% function coverage of `dist/`.

**Not yet verified**: a live hls.js instance against an HLS manifest and segment server carrying
an EC-3 audio rendition. Each piece above is tested in isolation, but the full integration has
not had a soak test on a live stream. It needs hardening against the range of hls.js versions and
stream shapes before it can be called finished.

## Loading assets safely

The worker `fetch`es the `glueUrl` you pass to `init` and evaluates the response as JavaScript -
the Emscripten glue is `MODULARIZE`d, so it is re-exported through a Blob URL and `import`ed.
Whatever that URL points at therefore runs with the worker's privileges, and `locateFile`
resolves the `.wasm` relative to it.

Point it at an asset you ship. Never derive it from user input, a query parameter, or a
third-party origin. This is the ordinary WASM-loader contract - the caller says where its own
assets live - but the consequence of getting it wrong here is code execution, not a broken
image, so it is worth stating plainly.

## Versioning and publishing

**Not published to npm.** The package name `ac3forge-wasm-decoder` is not on the registry;
consume it from source (`js/`, see [Install](#install)). When publishing is enabled its version will track the
main repository's own release
tags exactly the way the `ac3forge` PyPI package does (see
[docs/releasing.md](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/releasing.md)) -
this package's `package.json` carries only a `0.0.0-dev` placeholder; `npm.yml`'s `publish` job
stamps the release version immediately before publishing.

## License

GPL-3.0-only, same as the rest of [ac3forge](https://github.com/iainchesworthlabs/ac3forge).
