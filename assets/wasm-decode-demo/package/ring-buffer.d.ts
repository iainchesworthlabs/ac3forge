export interface RingBufferLayout {
    channelCount: number;
    /** Must be a power of two - index wraparound is a bitmask, not a modulo. */
    capacityFrames: number;
}
export declare function allocateRingBuffer(layout: RingBufferLayout): SharedArrayBuffer;
/** Shared base: index math and the SAB layout both sides agree on. */
declare abstract class RingBufferEnd {
    protected readonly control: Int32Array;
    protected readonly data: Float32Array;
    readonly channelCount: number;
    readonly capacityFrames: number;
    private readonly mask;
    protected constructor(sab: SharedArrayBuffer, layout: RingBufferLayout);
    protected wrap(index: number): number;
}
export declare class RingBufferWriter extends RingBufferEnd {
    constructor(sab: SharedArrayBuffer, layout: RingBufferLayout);
    get framesFree(): number;
    /**
     * Writes as many of `frameCount` frames as fit; returns how many were
     * actually written. A return value below `frameCount` means the reader
     * isn't draining fast enough - the caller decides whether to drop the
     * remainder or apply backpressure (Ac3ForgeDecoderNode drops it and
     * reports it as an overrun, since realtime playback has no use for stale
     * audio arriving late).
     */
    write(channels: readonly (Float32Array | undefined)[], frameCount: number): number;
}
export declare class RingBufferReader extends RingBufferEnd {
    constructor(sab: SharedArrayBuffer, layout: RingBufferLayout);
    get framesAvailable(): number;
    /**
     * Reads exactly `frameCount` frames into `out` per channel, zero-filling
     * any shortfall - an AudioWorkletProcessor's process() must always return
     * a full render quantum, so an underrun has to be silence, not a partial
     * or garbage buffer. Returns how many real (non-silence) frames were read.
     */
    read(out: readonly Float32Array[], frameCount: number): number;
}
export {};
//# sourceMappingURL=ring-buffer.d.ts.map