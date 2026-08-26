// The AudioWorkletProcessor half of the realtime pipeline: pure JS, no WASM
// and no fetch() (AudioWorkletGlobalScope has neither - see decoder-worker.ts's
// own header for why decoding happens in a Worker instead). All this does is
// drain a RingBuffer, one render quantum (128 frames) at a time, into the
// node's output - the actual decode work already happened off this thread
// before the audio ever reached the ring buffer.
//
// Loaded via `audioContext.audioWorklet.addModule(...)` - see
// decoder-node.ts's own comment on resolving this file's URL from a bundled
// consumer.

import { RingBufferReader } from "./ring-buffer.js";
import type { RingBufferLayout } from "./ring-buffer.js";

export interface Ac3ForgeProcessorOptions {
  sab: SharedArrayBuffer;
  layout: RingBufferLayout;
}

class Ac3ForgeSourceProcessor extends AudioWorkletProcessor {
  readonly #reader: RingBufferReader;

  constructor(options?: AudioWorkletNodeOptions) {
    super(options);
    if (!options?.processorOptions) {
      throw new Error("ac3forge-pcm-source requires processorOptions: { sab, layout }");
    }
    const { sab, layout } = options.processorOptions as Ac3ForgeProcessorOptions;
    this.#reader = new RingBufferReader(sab, layout);
  }

  override process(_inputs: Float32Array[][], outputs: Float32Array[][]): boolean {
    const output = outputs[0];
    if (!output || output.length === 0) return true;
    const frameCount = output[0]?.length ?? 0;
    if (frameCount === 0) return true;

    const read = this.#reader.read(output, frameCount);
    if (read < frameCount) {
      this.port.postMessage({ type: "underrun", framesShort: frameCount - read });
    }
    return true; // Keep the node alive across silence/underruns.
  }
}

registerProcessor("ac3forge-pcm-source", Ac3ForgeSourceProcessor);
