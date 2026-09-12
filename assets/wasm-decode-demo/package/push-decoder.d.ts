import type { Ac3ForgeEmbindModule, FoldOptions, ObjectFrame, PushOutcome, ScanOutcome } from "./types.js";
/**
 * Splits a whole elementary-stream byte blob into the access units
 * {@link PushDecoder.push} expects. A live/streaming caller (a container
 * demuxer, the hls.js bridge) already has its own access-unit boundaries and
 * has no reason to call this.
 */
export declare function scanStream(module: Ac3ForgeEmbindModule, bytes: Uint8Array): ScanOutcome;
export declare class PushDecoder {
    #private;
    constructor(module: Ac3ForgeEmbindModule, fold?: FoldOptions);
    /**
     * Decodes one access unit (one AC-3 syncframe, or an E-AC-3 independent
     * substream with its dependents - exactly what {@link scanStream}'s
     * `accessUnits` entries or a container demuxer's own samples give you).
     *
     * The channel/fold/object PCM this call produced is read separately via
     * {@link channel}/{@link fold}/{@link objectAudio} - zero-copy views into
     * the WASM heap, valid only until the next `push()`/`flush()` call on this
     * instance. Copy them out (`Float32Array.from(...)`) before yielding
     * control if you need them later.
     */
    push(unit: Uint8Array): PushOutcome;
    /** Zero-copy view into the last push() call's coded/rendered PCM for `channel` - see push()'s own contract. */
    channel(index: number): Float32Array | null;
    /** The optional §7.8 fold (ac3::OutputStage/DC1) this instance was constructed for - null if `fold.target` was `AsCoded`. */
    fold(index: number): Float32Array | null;
    get foldChannelCount(): number;
    get objectCount(): number;
    /** This frame's object state, or null if `index` carries no data this call (objectCount is the bound). */
    objectFrame(index: number): ObjectFrame | null;
    objectAudio(index: number): Float32Array | null;
    /**
     * §3.7's tail: call once after the last push() for a stream, to collect
     * whichever substream identities were still holding a frame back. Empty
     * for every stream that never used transient pre-noise processing - which
     * is the common case.
     */
    flush(): import("./types.js").RawFlushEntry[];
    flushedChannel(flushIndex: number, channel: number): Float32Array | null;
    /** Releases the underlying WASM object. Call when done - Embind instances are not garbage collected. */
    close(): void;
}
//# sourceMappingURL=push-decoder.d.ts.map