// A SharedArrayBuffer-backed, single-producer/single-consumer lock-free ring
// buffer of planar Float32 audio. One instance's SharedArrayBuffer is shared
// between decoder-worker.ts (the producer, decoding off the main thread) and
// worklet-processor.ts (the consumer, pulling one render quantum at a time)
// so handing decoded PCM to the audio thread costs no postMessage/copy once
// the buffer itself is set up.
//
// Requires cross-origin isolation (COOP: same-origin, COEP: require-corp) -
// SharedArrayBuffer is unavailable otherwise. See js/README.md.
const WRITE_INDEX = 0;
const READ_INDEX = 1;
const HEADER_INT32_LENGTH = 2;
function assertPowerOfTwo(value) {
    if (value <= 0 || (value & (value - 1)) !== 0) {
        throw new RangeError(`capacityFrames must be a power of two, got ${value}`);
    }
}
export function allocateRingBuffer(layout) {
    assertPowerOfTwo(layout.capacityFrames);
    const headerBytes = HEADER_INT32_LENGTH * Int32Array.BYTES_PER_ELEMENT;
    const dataBytes = layout.channelCount * layout.capacityFrames * Float32Array.BYTES_PER_ELEMENT;
    return new SharedArrayBuffer(headerBytes + dataBytes);
}
/** Shared base: index math and the SAB layout both sides agree on. */
class RingBufferEnd {
    control;
    data;
    channelCount;
    capacityFrames;
    mask;
    constructor(sab, layout) {
        assertPowerOfTwo(layout.capacityFrames);
        this.channelCount = layout.channelCount;
        this.capacityFrames = layout.capacityFrames;
        this.mask = layout.capacityFrames - 1;
        this.control = new Int32Array(sab, 0, HEADER_INT32_LENGTH);
        this.data = new Float32Array(sab, HEADER_INT32_LENGTH * Int32Array.BYTES_PER_ELEMENT, layout.channelCount * layout.capacityFrames);
    }
    wrap(index) {
        return index & this.mask;
    }
}
export class RingBufferWriter extends RingBufferEnd {
    constructor(sab, layout) {
        super(sab, layout);
    }
    get framesFree() {
        const writeIndex = Atomics.load(this.control, WRITE_INDEX);
        const readIndex = Atomics.load(this.control, READ_INDEX);
        return this.capacityFrames - (writeIndex - readIndex);
    }
    /**
     * Writes as many of `frameCount` frames as fit; returns how many were
     * actually written. A return value below `frameCount` means the reader
     * isn't draining fast enough - the caller decides whether to drop the
     * remainder or apply backpressure (Ac3ForgeDecoderNode drops it and
     * reports it as an overrun, since realtime playback has no use for stale
     * audio arriving late).
     */
    write(channels, frameCount) {
        const writeIndex = Atomics.load(this.control, WRITE_INDEX);
        const readIndex = Atomics.load(this.control, READ_INDEX);
        const free = this.capacityFrames - (writeIndex - readIndex);
        const n = Math.min(frameCount, free);
        for (let ch = 0; ch < this.channelCount; ch++) {
            const source = channels[ch];
            if (!source)
                continue;
            const base = ch * this.capacityFrames;
            for (let i = 0; i < n; i++) {
                this.data[base + this.wrap(writeIndex + i)] = source[i] ?? 0;
            }
        }
        Atomics.store(this.control, WRITE_INDEX, writeIndex + n);
        return n;
    }
}
export class RingBufferReader extends RingBufferEnd {
    constructor(sab, layout) {
        super(sab, layout);
    }
    get framesAvailable() {
        const writeIndex = Atomics.load(this.control, WRITE_INDEX);
        const readIndex = Atomics.load(this.control, READ_INDEX);
        return writeIndex - readIndex;
    }
    /**
     * Reads exactly `frameCount` frames into `out` per channel, zero-filling
     * any shortfall - an AudioWorkletProcessor's process() must always return
     * a full render quantum, so an underrun has to be silence, not a partial
     * or garbage buffer. Returns how many real (non-silence) frames were read.
     */
    read(out, frameCount) {
        const writeIndex = Atomics.load(this.control, WRITE_INDEX);
        const readIndex = Atomics.load(this.control, READ_INDEX);
        const available = writeIndex - readIndex;
        const n = Math.min(frameCount, available);
        for (let ch = 0; ch < this.channelCount; ch++) {
            const destination = out[ch];
            if (!destination)
                continue;
            const base = ch * this.capacityFrames;
            let i = 0;
            for (; i < n; i++) {
                destination[i] = this.data[base + this.wrap(readIndex + i)];
            }
            for (; i < frameCount; i++)
                destination[i] = 0;
        }
        Atomics.store(this.control, READ_INDEX, readIndex + n);
        return n;
    }
}
//# sourceMappingURL=ring-buffer.js.map