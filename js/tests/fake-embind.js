// A scripted stand-in for the Embind module apps/wasm/decoder_bindings.cpp
// builds, so the TypeScript wrappers over it (push-decoder.ts, decode-file.ts,
// decoder-worker.ts) can be tested in Node without an Emscripten build. It
// holds no codec: each push() returns the next scripted result, and the PCM
// views it hands out are whatever the script put there. What the tests check
// is the wrapper's own logic - result mapping, concatenation, gap filling,
// flush handling, lifetime - not decoding, which ac3tests covers in C++.
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
    constructor(channels, sampleRateHz, frameRateIndex, bitrateKbps, rateMode, codecModeArg, iframeInterval, dialnormDb) {
      log.encoderConstructed.push({
        channels, sampleRateHz, frameRateIndex, bitrateKbps, rateMode, codecMode: codecModeArg, iframeInterval, dialnormDb,
      });
    }
    encode(channels) {
      log.encoded.push(channels.map((channel) => Array.from(channel)));
      return encodeScript[this.#encodeStep++] ?? [];
    }
    flush() {
      return flushScript;
    }
    error() {
      return encoderError;
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
