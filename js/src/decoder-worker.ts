// The realtime decode half of the AudioWorklet pipeline. Runs in a dedicated
// module Worker (`new Worker(url, { type: "module" })`) rather than directly
// in the AudioWorkletProcessor: AudioWorkletGlobalScope has neither fetch()
// nor TextDecoder (the Emscripten glue needs both), so decode has to happen
// somewhere that does - a Worker has both, and is still off the main thread,
// which is the actual requirement. worklet-processor.ts, running in the
// audio thread proper, only drains the RingBuffer this file writes into.
//
// The Emscripten glue (apps/wasm/decoder_bindings.cpp's compiled output,
// `ac3forge_decode.js`) is a MODULARIZE-style script, not an ES module
// (apps/wasm/CMakeLists.txt does not set -sEXPORT_ES6) - so it can't be
// `import`ed directly here the way ring-buffer.ts/push-decoder.ts can. It's
// loaded by fetching its source as text and re-exporting the
// `createAc3ForgeModule` global it defines as a Blob-URL ES module - a
// well-known technique for consuming a classic/UMD script from a module
// context without eval() or a bundler-specific loader.

import { PushDecoder } from "./push-decoder.js";
import { RingBufferWriter } from "./ring-buffer.js";
import type { RingBufferLayout } from "./ring-buffer.js";
import type { Ac3ForgeEmbindModule, Ac3ForgeModuleFactory, FoldOptions, WriteTarget } from "./types.js";

export interface InitMessage {
  type: "init";
  /**
   * URL of the Emscripten glue (`ac3forge_decode.js`) built from apps/wasm/.
   *
   * TRUST BOUNDARY: the worker fetches this URL and evaluates what comes back
   * as JavaScript (the glue is `MODULARIZE`d, so it is re-exported through a
   * Blob URL and `import`ed - see `loadEmscriptenGlue`). Whatever this points
   * at therefore runs with the worker's privileges. Point it at an asset you
   * ship; never at a URL derived from user input, a query parameter or a
   * third-party origin. CodeQL flags the fetch as request forgery for exactly
   * this reason, and it is right that the caller, not this package, is the one
   * who has to get it right.
   */
  glueUrl: string;
  fold?: FoldOptions;
  sab: SharedArrayBuffer;
  layout: RingBufferLayout;
  /** Which of PushDecoder's outputs feeds the ring buffer. */
  writeTarget: WriteTarget;
}

export interface PushMessage {
  type: "push";
  /** Transferred, not copied - detached from the caller's side after postMessage. */
  bytes: ArrayBuffer;
}

export type InboundMessage = InitMessage | PushMessage | { type: "flush" } | { type: "close" };

async function loadEmscriptenGlue(glueUrl: string): Promise<Ac3ForgeModuleFactory> {
  const source = await (await fetch(glueUrl)).text();
  // createAc3ForgeModule is the MODULARIZE+EXPORT_NAME global the glue
  // defines when evaluated as a plain script (apps/wasm/CMakeLists.txt's
  // link options) - re-exporting it is what makes the Blob URL below
  // `import`able.
  const blob = new Blob([source, "\nexport default createAc3ForgeModule;\n"], {
    type: "text/javascript",
  });
  const blobUrl = URL.createObjectURL(blob);
  try {
    const namespace = (await import(/* webpackIgnore: true */ blobUrl)) as { default: Ac3ForgeModuleFactory };
    return namespace.default;
  } finally {
    URL.revokeObjectURL(blobUrl);
  }
}

class DecoderWorker {
  #module: Ac3ForgeEmbindModule | null = null;
  #decoder: PushDecoder | null = null;
  #ring: RingBufferWriter | null = null;
  #writeTarget: WriteTarget = "channels";

  async handle(message: InboundMessage): Promise<void> {
    switch (message.type) {
      case "init":
        await this.#init(message);
        break;
      case "push":
        this.#push(new Uint8Array(message.bytes));
        break;
      case "flush":
        this.#flush();
        break;
      case "close":
        this.#decoder?.close();
        self.close();
        break;
    }
  }

  async #init(message: InitMessage): Promise<void> {
    const factory = await loadEmscriptenGlue(message.glueUrl);
    // The glue resolves its own `.wasm` file relative to `locateFile`'s
    // base when given one - without this, it resolves relative to the
    // Blob URL loadEmscriptenGlue() imported it from, which has no
    // meaningful path to resolve against and makes the wasm fetch fail.
    this.#module = await factory({ locateFile: (path: string) => new URL(path, message.glueUrl).href });
    this.#decoder = new PushDecoder(this.#module, message.fold);
    this.#ring = new RingBufferWriter(message.sab, message.layout);
    this.#writeTarget = message.writeTarget;
    postMessage({ type: "ready" });
  }

  #push(bytes: Uint8Array): void {
    const decoder = this.#decoder;
    const ring = this.#ring;
    if (!decoder || !ring) {
      postMessage({ type: "error", message: "decoder-worker: push before init" });
      return;
    }
    const outcome = decoder.push(bytes);
    if (!outcome.ok || outcome.holdBack) {
      postMessage({ type: "result", outcome });
      return;
    }

    const channelCount = this.#writeTarget === "fold" ? outcome.foldChannelCount : outcome.channelCount;
    const channels: (Float32Array | undefined)[] = [];
    for (let ch = 0; ch < channelCount; ch++) {
      channels.push((this.#writeTarget === "fold" ? decoder.fold(ch) : decoder.channel(ch)) ?? undefined);
    }
    const written = ring.write(channels, outcome.frameSamples);
    if (written < outcome.frameSamples) {
      postMessage({ type: "overrun", framesDropped: outcome.frameSamples - written });
    }

    const objectFrames = [];
    for (let i = 0; i < outcome.objectCount; i++) {
      const frame = decoder.objectFrame(i);
      if (frame) objectFrames.push({ label: frame.label, position: Array.from(frame.position) });
    }
    postMessage({ type: "result", outcome, objectFrames });
  }

  #flush(): void {
    const decoder = this.#decoder;
    if (!decoder) return;
    const entries = decoder.flush();
    postMessage({ type: "flushed", entries });
  }
}

const worker = new DecoderWorker();
self.addEventListener("message", (event: MessageEvent<InboundMessage>) => {
  void worker.handle(event.data);
});
