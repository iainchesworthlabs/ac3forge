// A scripted stand-in for the Embind module apps/wasm/decoder_bindings.cpp
// builds, so the TypeScript wrappers over it (push-decoder.ts, decode-file.ts,
// decoder-worker.ts) can be tested in Node without an Emscripten build. It
// holds no codec: each push() returns the next scripted result, and the PCM
// views it hands out are whatever the script put there. What the tests check
// is the wrapper's own logic - result mapping, concatenation, gap filling,
// flush handling, lifetime - not decoding, which iclforge-tests covers in C++.
//
// Not a *.test.js file, so `node --test` does not run it on its own.

/**
 * One scripted access unit's outcome. `channels`/`fold`/`objects` are the
 * per-channel (or per-object) PCM the native side would expose after that
 * push; `positions` the per-object 7-float position, or null for "no data".
 */
export function frame({
  sampleRate = 48000,
  frameSamples = 4,
  dialnorm = -31,
  channels = [],
  channelLabels = channels.map((_, i) => `C${i}`),
  fold = [],
  objects = [],
  objectLabels = objects.map((_, i) => `obj${i}`),
  positions = objects.map(() => null),
} = {}) {
  return {
    raw: {
      ok: true,
      holdBack: false,
      sampleRate,
      frameSamples,
      dialnorm,
      channelCount: channels.length,
      channelLabels,
      foldChannelCount: fold.length,
      objectCount: objects.length,
      objectLabels,
    },
    channels,
    fold,
    objects,
    positions,
  };
}

export const holdBack = () => ({ raw: { ok: true, holdBack: true } });
export const failure = (error) => ({ raw: error === undefined ? { ok: false } : { ok: false, error } });

/**
 * Builds a fake module. `script` is the sequence of push() outcomes;
 * `flushEntries` what flush() returns, with `flushed[flushIndex][channel]`
 * the PCM behind each entry; `scan` what scanStream() returns.
 */
export function makeFakeModule({ script = [], flushEntries = [], flushed = [], scan = null } = {}) {
  const log = { constructed: [], pushed: [], deleted: 0, scanned: [] };
  let current = null;

  class FakeNativePushDecoder {
    #step = 0;
    constructor(foldTarget, applyDialnorm, mixLfe) {
      log.constructed.push({ foldTarget, applyDialnorm, mixLfe });
    }
    pushAccessUnit(bytes) {
      log.pushed.push(Array.from(bytes));
      current = script[this.#step++] ?? failure("script exhausted");
      return current.raw;
    }
    channelPcm(ch) {
      return current?.channels?.[ch] ?? null;
    }
    foldChannelCount() {
      return current?.fold?.length ?? 0;
    }
    foldPcm(ch) {
      return current?.fold?.[ch] ?? null;
    }
    objectCount() {
      return current?.objects?.length ?? 0;
    }
    objectPosition(i) {
      return current?.positions?.[i] ?? null;
    }
    objectAudioPcm(i) {
      return current?.objects?.[i] ?? null;
    }
    objectLabel(i) {
      return current?.raw?.objectLabels?.[i] ?? "";
    }
    flush() {
      return flushEntries;
    }
    flushedChannelPcm(index, ch) {
      return flushed[index]?.[ch] ?? null;
    }
    delete() {
      log.deleted++;
    }
  }

  const module = {
    PushDecoder: FakeNativePushDecoder,
    scanStream(bytes) {
      log.scanned.push(bytes.length);
      if (scan) return scan;
      // Default: one access unit per byte, so a test's input length is its
      // access-unit count and each unit's content is its own index.
      return {
        ok: true,
        kind: "E-AC-3",
        sampleRate: 48000,
        accessUnits: Array.from(bytes, (_, i) => ({ offset: i, length: 1 })),
      };
    },
  };
  return { module, log };
}

export const pcm = (...values) => Float32Array.from(values);

// --- AC-4 (js/src/ac4.ts) ---------------------------------------------------
//
// A scripted stand-in for the Embind module apps/wasm/ac4_bindings.cpp
// builds, so ac4.ts can be tested in Node without an Emscripten build - the
// same "no codec, just scripted outputs, same method names as the real
// Embind classes 1:1" approach as makeFakeModule() above, for
// Ac4Decoder/Ac4Encoder/syncFrame instead of PushDecoder/scanStream.

/** One scripted decodeFrame() outcome - see ac4.ts's RawAc4DecodedFrame. */
export function ac4Frame({
  sampleRate = 48000,
  sequenceCounter = 0,
  presentation = 0,
  presentationId = null,
  samples = 4,
  channels = [],
  speakers = channels.map((_, i) => `C${i}`),
  concealed = null,
  objects = [],
} = {}) {
  return { sampleRate, sequenceCounter, presentation, presentationId, samples, channels, speakers, concealed, objects };
}

/** One scripted encode()/flush() entry - see ac4.ts's RawAc4EncodedFrame. */
export function ac4EncodedFrame({ data = new Uint8Array(0), samples = 0, iframe = false } = {}) {
  return { data, samples, iframe };
}

/**
 * Builds a fake AC-4 module. `decodeScript` is decodeFrame()'s sequence of
 * outcomes (undefined/missing entries return null, the same "nothing to
 * output" contract ac4_bindings.cpp's real decodeFrame() has); `encodeScript`
 * is encode()'s sequence of per-call frame arrays (missing entries return
 * `[]`); `flushScript` is what every flush() call returns; `presentations`,
 * `refusalReason` and `latencySamples` are read on every call (a real
 * decoder's own presentations()/refusalReason()/latencySamples() likewise
 * read current state rather than a per-call script).
 */
export function makeFakeAc4Module({
  decodeScript = [],
  presentations = [],
  refusalReason = "",
  latencySamples = 0,
  encodeScript = [],
  flushScript = [],
  encoderError = "",
  constructionError = "",
  codecMode = 0,
  delaySamples = 0,
  decoderDelaySamples = 0,
  dac4Bytes = new Uint8Array(0),
  dac4Refusal = "",
  syncFrameResult = new Uint8Array(0),
} = {}) {
  const log = {
    decoderConstructed: [],
    decoded: [],
    outputsSet: [],
    presentationsSet: [],
    decoderReset: 0,
    decoderDeleted: 0,
    encoderConstructed: [],
    encoded: [],
    encodedUpdates: [],
    encoderDeleted: 0,
    syncFramed: [],
  };

  class FakeNativeAc4Decoder {
    #step = 0;
    constructor(outputLevelDbfs, drc, downmix, decodingMode, concealment, presentationId, presentationIndex, language, level) {
      log.decoderConstructed.push({
        outputLevelDbfs, drc, downmix, decodingMode, concealment, presentationId, presentationIndex, language, level,
      });
    }
    decodeFrame(bytes) {
      log.decoded.push(Array.from(bytes));
      const next = decodeScript[this.#step++];
      return next === undefined ? null : next;
    }
    setOutput(...args) {
      log.outputsSet.push(args);
    }
    setPresentation(...args) {
      log.presentationsSet.push(args);
    }
    reset() {
      log.decoderReset++;
    }
    refusalReason() {
      return refusalReason;
    }
    latencySamples() {
      return latencySamples;
    }
    presentations() {
      return presentations;
    }
    delete() {
      log.decoderDeleted++;
    }
  }

  class FakeNativeAc4Encoder {
    #encodeStep = 0;
    constructor(options) {
      // The options object as it was when the constructor ran: the real
      // constructor reads it once and keeps nothing of it.
      log.encoderConstructed.push(structuredClone(options));
    }
    encode(channels, updates) {
      log.encoded.push(channels.map((channel) => Array.from(channel)));
      log.encodedUpdates.push(structuredClone(updates));
      return encodeScript[this.#encodeStep++] ?? [];
    }
    flush() {
      return flushScript;
    }
    error() {
      return encoderError;
    }
    constructionError() {
      return constructionError;
    }
    codecMode() {
      return codecMode;
    }
    delaySamples() {
      return delaySamples;
    }
    decoderDelaySamples() {
      return decoderDelaySamples;
    }
    buildDac4() {
      return dac4Bytes;
    }
    dac4Refusal() {
      return dac4Refusal;
    }
    delete() {
      log.encoderDeleted++;
    }
  }

  const module = {
    Ac4Decoder: FakeNativeAc4Decoder,
    Ac4Encoder: FakeNativeAc4Encoder,
    syncFrame(rawFrame, crc) {
      log.syncFramed.push({ length: rawFrame.length, crc });
      return syncFrameResult;
    },
  };
  return { module, log };
}

/**
 * A stand-in for the same module with a codec model in place of the script: an
 * encoder that writes what it is given as frames (JSON, one 2 048-sample frame
 * of every object's PCM, its properties in force at the frame's first sample
 * and the updates within the frame) and a decoder that reads them back, with
 * every property quantised to the steps its code has (Part 2 Annex F, as
 * iclforge::ac4::ObjectProperties documents them) and an inactive object sending none.
 * Objects come out in the decoder's order: the LFE, then the bed objects, then
 * the dynamic objects, each in the order they were listed. There is no delay,
 * and no refusal of any configuration: what it tests is the wrapper's traffic
 * with the native module (the options in, the updates in, the frames and the
 * objects out), not the codec, which is the C API's, Rust's and Python's tests'
 * to hold to iclforge::ac4::Encoder and this module's C++ side's to a real Emscripten
 * build. It implements only what a round trip calls: encode(), flush(),
 * decodeFrame() and delete().
 */
export function makeLoopbackAc4Module() {
  const FRAME = 2048;
  const DEFAULTS = {
    active: true,
    gainDb: 0,
    priority: 1,
    position: [0.5, 0.5, 0],
    zoneMask: 0,
    enableElevation: true,
    snap: false,
    width: [0, 0, 0],
    screenFactor: 0,
    depthExponent: 1,
    distance: null,
    divergence: 0,
    trimDisabled: false,
    headphoneRenderMode: null,
    headTrackDisabled: false,
  };
  const steps = (v, n) => Math.round(v * n) / n;
  const complete = (p = {}) => ({
    ...DEFAULTS,
    ...Object.fromEntries(Object.entries(p).filter(([, v]) => v !== undefined)),
  });
  const quantise = (given, dynamic) => {
    const p = complete(given);
    if (!p.active) return { ...DEFAULTS, active: false, gainDb: -Infinity, priority: 0, enableElevation: false };
    return {
      ...p,
      gainDb: p.gainDb === -Infinity ? p.gainDb : Math.min(15, Math.max(-49, Math.round(p.gainDb))),
      priority: steps(p.priority, 31),
      position: dynamic ? [steps(p.position[0], 62), steps(p.position[1], 62), steps(p.position[2], 15)] : DEFAULTS.position,
      width: dynamic ? p.width.map((w) => steps(w, 31)) : DEFAULTS.width,
      screenFactor: p.screenFactor === 0 ? 0 : Math.max(1, steps(p.screenFactor * 8, 1)) / 8,
    };
  };

  class LoopbackEncoder {
    #objects;
    #pcm;
    #updates = [];
    #consumed = 0;
    #index = 0;
    constructor(options) {
      this.#objects = options.objects.objects.map((o) => ({
        lfe: o.lfe ?? false,
        bed: o.bed ?? null,
        properties: complete(o.properties),
      }));
      this.#pcm = this.#objects.map(() => []);
    }
    #frame(count) {
      const start = this.#consumed;
      const end = start + FRAME;
      const inFrame = this.#updates.filter((u) => u.sample >= start && u.sample < end);
      const inForce = this.#objects.map((o, k) => {
        const before = this.#updates.filter((u) => u.object === k && u.sample < start).at(-1);
        return before ? before.properties : o.properties;
      });
      const frame = {
        index: this.#index++,
        objects: this.#objects.map((o, k) => ({
          lfe: o.lfe,
          bed: o.bed,
          properties: quantise(inForce[k], !o.lfe && o.bed === null),
          pcm: Array.from({ length: FRAME }, (_, n) => (n < count ? this.#pcm[k][n] : 0)),
        })),
        updates: inFrame.map((u) => ({
          object: u.object,
          sample: u.sample - start,
          rampSamples: u.rampSamples,
          properties: quantise(u.properties, !this.#objects[u.object].lfe && this.#objects[u.object].bed === null),
        })),
      };
      this.#consumed = end;
      this.#pcm = this.#pcm.map((channel) => channel.slice(count));
      return { data: new TextEncoder().encode(JSON.stringify(frame)), samples: FRAME, iframe: frame.index === 0 };
    }
    encode(channels, updates) {
      // An update's sample is counted from the start of this call's input.
      for (const u of updates) {
        this.#updates.push({
          object: u.object,
          sample: this.#consumed + this.#pcm[0].length + u.sample,
          rampSamples: u.rampSamples ?? 0,
          properties: complete(u.properties),
        });
      }
      channels.forEach((channel, k) => this.#pcm[k].push(...channel));
      const frames = [];
      while (this.#pcm[0].length >= FRAME) frames.push(this.#frame(FRAME));
      return frames;
    }
    flush() {
      const left = this.#pcm[0].length;
      return left > 0 ? [this.#frame(left)] : [];
    }
    delete() {}
  }

  class LoopbackDecoder {
    decodeFrame(bytes) {
      const frame = JSON.parse(new TextDecoder().decode(bytes));
      const order = [...frame.objects.keys()];
      const rank = (o) => (o.lfe ? 0 : o.bed !== null ? 1 : 2);
      order.sort((a, b) => rank(frame.objects[a]) - rank(frame.objects[b]) || a - b);
      const objects = order.map((k) => {
        const o = frame.objects[k];
        return {
          kind: o.bed !== null ? "bed" : "dyn",
          lfe: o.lfe,
          speaker: o.lfe ? "LFE" : o.bed !== null ? "L" : null,
          samples: Float32Array.from(o.pcm),
          properties: o.properties,
          updates: frame.updates
            .filter((u) => u.object === k)
            .map((u) => ({ sample: u.sample, rampSamples: u.rampSamples, properties: u.properties })),
        };
      });
      return ac4Frame({ sequenceCounter: frame.index, samples: FRAME, objects });
    }
    delete() {}
  }

  return { Ac4Decoder: LoopbackDecoder, Ac4Encoder: LoopbackEncoder };
}
