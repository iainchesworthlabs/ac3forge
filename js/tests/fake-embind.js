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
