import type { FoldOptions } from "./types.js";
export interface Ac3ForgeDecoderNodeOptions {
    /**
     * URL of this package's compiled `worklet-processor.js` - passed to
     * `audioContext.audioWorklet.addModule()`. A bundler resolves this from
     * `new URL("ac3forge-wasm-decoder/worklet-processor", import.meta.url)`
     * or the package's own `exports["./worklet-processor"]` entry; a plain
     * static site copies the file next to its own script and points here.
     */
    workletProcessorUrl: string | URL;
    /** URL of this package's compiled `decoder-worker.js` - same resolution story as workletProcessorUrl. */
    workerUrl: string | URL;
    /** URL of the Emscripten glue (`ac3forge_decode.js`) built from apps/wasm/ - this package embeds no compiled binary of its own. */
    wasmGlueUrl: string | URL;
    /** Default: no fold (raw/coded channels), so `channelCount` must be supplied. */
    fold?: FoldOptions;
    /** How many channels the node outputs. Required when `fold` is omitted or AsCoded; defaults to 2 for any other fold target. */
    channelCount?: number;
    /** Ring buffer depth. Default 2 seconds - generous enough to absorb ordinary jitter without much latency cost. */
    ringBufferSeconds?: number;
}
export interface StreamInfoEventDetail {
    sampleRate: number;
    channelCount: number;
    channelLabels: string[];
    foldChannelCount: number;
}
/**
 * Push-frame realtime decode + playback: construct once per stream, call
 * {@link pushAccessUnit} as access units arrive (from a container demuxer,
 * the hls.js bridge, or your own transport), connect {@link node} into a
 * Web Audio graph. Decoding happens in a Worker; only ring-buffer draining
 * happens on the audio rendering thread itself.
 */
export declare class Ac3ForgeDecoderNode extends EventTarget {
    #private;
    readonly node: AudioWorkletNode;
    private constructor();
    static create(audioContext: BaseAudioContext, options: Ac3ForgeDecoderNodeOptions): Promise<Ac3ForgeDecoderNode>;
    /**
     * Decodes one access unit and streams its PCM into the audio graph.
     * `unit` is copied (once, here) into its own `ArrayBuffer` before being
     * transferred to the Worker - safe even when `unit` is a view sharing
     * memory with siblings (the hls.js bridge's fmp4-extracted samples all
     * share one fragment's buffer), at the cost of one copy per call.
     */
    pushAccessUnit(unit: Uint8Array): void;
    /** §3.7's tail - call once after the last pushAccessUnit() for a stream. */
    flush(): void;
    /** Resolves once every pushAccessUnit() call so far has been acknowledged by the Worker. */
    whenIdle(): Promise<void>;
    close(): void;
}
//# sourceMappingURL=decoder-node.d.ts.map