# ac3forge-wasm-decoder

A streaming AC-3/E-AC-3 (Dolby Digital/Digital Plus) decoder for the browser, compiled from
[`ac3::forge`](https://github.com/iainchesworthlabs/ac3forge) to WebAssembly. Built because
**Chrome still cannot decode EC-3** ([video.js http-streaming#1297](https://github.com/videojs/http-streaming/issues/1297)
is open) - this package is a real, embeddable answer to that, not just a demo of the decoder.

Four pieces:

- **A push-frame decode API** (`PushDecoder`) over `ac3::Eac3Decoder::decode_access_unit_into`'s
  caller-buffer form - the hot path allocates nothing on the C++ side.
- **An `Ac3ForgeDecoderNode`**: a real `AudioWorkletNode`. Decoding runs in a Worker (off the
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

```bash
npm install ac3forge-wasm-decoder
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
// program.objectPositions / program.objectAudio: real OAMD/JOC, if the stream carries Atmos objects
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

## hls.js/MSE bridge

hls.js's own `BufferController` drops the audio track entirely the moment
`MediaSource.addSourceBuffer('audio/mp4;codecs="ec-3"')` throws - which it does in every browser,
since none of them can decode EC-3 via MSE. A passive event listener never gets a chance to run.
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

- It extracts real AC-3/E-AC-3 access units from the fMP4 segments hls.js's own remuxer produces
  (`fmp4.ts` - a minimal ISOBMFF box walker, not a general MP4 parser) and feeds them to the
  push-frame decoder for real playback.
- A/V sync is `syncTo()`'s clock alignment against the host media element on play/pause/seek - an
  **approximation**, not sample-accurate mux-level sync.
- `buffered` range reporting back to hls.js is derived from each fragment's own `tfdt`/`trun`
  timing, trusting that every sample decoded rather than confirming each one did.

## What's verified

- The push-frame API and the AudioWorklet ring-buffer pipeline: unit-tested (`ring-buffer.test.js`)
  and exercised end-to-end in a real headless Chromium by `apps/wasm/tests/` (the main repository's
  CI).
- `fmp4.ts`'s box walker: unit-tested against a real ffmpeg-remuxed fragmented-MP4 fixture
  (`fmp4.test.js`), asserting every extracted sample lands exactly on an AC-3/E-AC-3 syncword.
- The `MediaSource`/`addSourceBuffer` shim's mechanics: unit-tested against a fake `MediaSource`
  stub (`hls-bridge.test.js`).

**Not yet verified**: a live hls.js instance against a real HLS manifest/segment server carrying
an EC-3 audio rendition. The mechanism above is real and each of its pieces is tested in
isolation, but the full integration has not had a real-stream soak test - treat it as a working
mechanism that needs hardening against real-world hls.js versions and stream shapes, not a
finished, battle-tested integration.

## Versioning and publishing

Published to npm as `ac3forge-wasm-decoder`. Its version tracks the main repository's own release
tags exactly the way the `ac3forge` PyPI package does (see
[docs/releasing.md](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/releasing.md)) -
this package's `package.json` carries no version to keep in sync by hand; the release workflow
stamps the real one immediately before publishing.

## License

GPL-3.0-only, same as the rest of [ac3forge](https://github.com/iainchesworthlabs/ac3forge).
