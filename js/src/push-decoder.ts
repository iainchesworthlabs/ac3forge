// The reusable push-frame primitive (roadmap UX5): a thin, typed wrapper
// over the Embind PushDecoder class apps/wasm/decoder_bindings.cpp builds,
// which itself decodes through ac3::Eac3Decoder::decode_access_unit_into -
// the caller-buffer form, so the hot path allocates nothing on the C++ side.
// Every other piece of this package (the whole-file convenience helper in
// decode-file.ts, the AudioWorklet pipeline in decoder-worker.ts) is built
// on top of THIS class, not a second decode path.

import type { Ac3ForgeEmbindModule, FoldOptions, NativePushDecoder, ObjectFrame, PushOutcome, ScanOutcome } from "./types.js";
import { DownmixTarget } from "./types.js";

/**
 * Splits a whole elementary-stream byte blob into the access units
 * {@link PushDecoder.push} expects. A live/streaming caller (a container
 * demuxer, the hls.js bridge) already has its own access-unit boundaries and
 * has no reason to call this.
 */
export function scanStream(module: Ac3ForgeEmbindModule, bytes: Uint8Array): ScanOutcome {
  const raw = module.scanStream(bytes);
  if (!raw.ok) {
    return { ok: false, error: raw.error ?? "scan failed" };
  }
  return { ok: true, kind: raw.kind!, sampleRate: raw.sampleRate!, accessUnits: raw.accessUnits! };
}

export class PushDecoder {
  readonly #native: NativePushDecoder;
  #closed = false;

  constructor(module: Ac3ForgeEmbindModule, fold: FoldOptions = { target: DownmixTarget.AsCoded }) {
    this.#native = new module.PushDecoder(fold.target, fold.applyDialnorm ?? false, fold.mixLfe ?? false);
  }

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
  push(unit: Uint8Array): PushOutcome {
    const raw = this.#native.pushAccessUnit(unit);
    if (!raw.ok) {
      return { ok: false, error: raw.error ?? "decode failed" };
    }
    if (raw.holdBack) {
      return { ok: true, holdBack: true };
    }
    return {
      ok: true,
      holdBack: false,
      sampleRate: raw.sampleRate!,
      frameSamples: raw.frameSamples!,
      dialnorm: raw.dialnorm!,
      channelCount: raw.channelCount!,
      channelLabels: raw.channelLabels!,
      foldChannelCount: raw.foldChannelCount!,
      objectCount: raw.objectCount!,
      objectLabels: raw.objectLabels!,
    };
  }

  /** Zero-copy view into the last push() call's coded/rendered PCM for `channel` - see push()'s own contract. */
  channel(index: number): Float32Array | null {
    return this.#native.channelPcm(index);
  }

  /** The optional §7.8 fold (ac3::OutputStage/DC1) this instance was constructed for - null if `fold.target` was `AsCoded`. */
  fold(index: number): Float32Array | null {
    if (index >= this.#native.foldChannelCount()) return null;
    return this.#native.foldPcm(index);
  }

  get foldChannelCount(): number {
    return this.#native.foldChannelCount();
  }

  get objectCount(): number {
    return this.#native.objectCount();
  }

  /** This frame's object state, or null if `index` carries no data this call (objectCount is the bound). */
  objectFrame(index: number): ObjectFrame | null {
    const position = this.#native.objectPosition(index);
    if (!position) return null;
    return { label: this.#native.objectLabel(index), position };
  }

  objectAudio(index: number): Float32Array | null {
    return this.#native.objectAudioPcm(index);
  }

  /**
   * §3.7's tail: call once after the last push() for a stream, to collect
   * whichever substream identities were still holding a frame back. Empty
   * for every stream that never used transient pre-noise processing - which
   * is the common case.
   */
  flush() {
    return this.#native.flush();
  }

  flushedChannel(flushIndex: number, channel: number): Float32Array | null {
    return this.#native.flushedChannelPcm(flushIndex, channel);
  }

  /** Releases the underlying WASM object. Call when done - Embind instances are not garbage collected. */
  close(): void {
    if (this.#closed) return;
    this.#closed = true;
    this.#native.delete();
  }
}
