// ac4.ts, run in Node with fetch/Blob/URL.createObjectURL replaced by fakes -
// the same glue-loading harness decoder-worker.test.js uses (see that file's
// own header comment for why a data: URL stands in for blob:, which Node
// cannot import). Unlike decoder-worker.ts, ac4.ts has no module-level side
// effects (no self.addEventListener at import time) - it only touches
// fetch/Blob/URL once loadAc4Module() is actually called - so a plain static
// import at the top of this file is enough; there is no need for
// decoder-worker.test.js's dynamic-import-inside-before() trick, and no
// self/postMessage stub at all, since ac4.ts never references either.

import { test } from "node:test";
import assert from "node:assert/strict";
import {
  loadAc4Module,
  Ac4Decoder,
  Ac4Encoder,
  Ac4DrcMode,
  Ac4DownmixTarget,
  Ac4DecodingMode,
  Ac4ConcealmentPolicy,
  Ac4CodecMode,
  Ac4RateMode,
  syncFrame,
} from "../dist/ac4.js";
import { ac4Frame, ac4EncodedFrame, makeFakeAc4Module, pcm } from "./fake-embind.js";

const fetched = [];
let factoryArgs = null;
let fake = null;

// The glue's module factory, reachable from the data: URL the fake
// createObjectURL below produces (a data: module cannot close over this
// file's scope, so it reads it off globalThis) - same indirection
// decoder-worker.test.js uses for createAc3ForgeModule.
globalThis.__ac4FakeFactory = async (overrides) => {
  factoryArgs = overrides;
  return fake.module;
};

globalThis.fetch = async (url) => {
  fetched.push(String(url));
  return { text: async () => "var createAc3ForgeAc4Module = globalThis.__ac4FakeFactory;" };
};

const RealBlob = globalThis.Blob;
const revoked = [];
globalThis.Blob = class extends RealBlob {
  constructor(parts, options) {
    super(parts, options);
    this.source = parts.join("");
  }
};
URL.createObjectURL = (blob) => `data:text/javascript;base64,${Buffer.from(blob.source).toString("base64")}`;
URL.revokeObjectURL = (url) => revoked.push(url);

const GLUE_URL = "https://example.test/wasm/ac3forge_ac4.js";

test("loadAc4Module fetches the glue, resolves the wasm beside it, and returns the fake module", async () => {
  fake = makeFakeAc4Module();
  fetched.length = 0;
  revoked.length = 0;
  const module = await loadAc4Module(GLUE_URL);
  assert.deepEqual(fetched, [GLUE_URL]);
  assert.equal(factoryArgs.locateFile("ac3forge_ac4.wasm"), "https://example.test/wasm/ac3forge_ac4.wasm");
  assert.equal(revoked.length > 0, true, "the object URL is revoked once imported");
  assert.equal(module, fake.module);
});

test("Ac4Decoder's constructor defaults every option to the NaN/-1 'unset' sentinels", async () => {
  fake = makeFakeAc4Module();
  const module = await loadAc4Module(GLUE_URL);
  new Ac4Decoder(module);
  assert.equal(fake.log.decoderConstructed.length, 1);
  const ctor = fake.log.decoderConstructed[0];
  assert.equal(Number.isNaN(ctor.outputLevelDbfs), true);
  assert.equal(ctor.drc, Ac4DrcMode.Default);
  assert.equal(ctor.downmix, Ac4DownmixTarget.AsCoded);
  assert.equal(ctor.decodingMode, Ac4DecodingMode.Full);
  assert.equal(ctor.concealment, Ac4ConcealmentPolicy.None);
  assert.equal(ctor.presentationId, -1);
  assert.equal(ctor.presentationIndex, -1);
  assert.equal(ctor.language, "");
  assert.equal(ctor.level, 3);
});

test("Ac4Decoder's constructor passes explicit options through untouched", async () => {
  fake = makeFakeAc4Module();
  const module = await loadAc4Module(GLUE_URL);
  new Ac4Decoder(module, {
    outputLevelDbfs: -23,
    drc: Ac4DrcMode.FlatPanelTv,
    downmix: Ac4DownmixTarget.Stereo,
    decodingMode: Ac4DecodingMode.Core,
    concealment: Ac4ConcealmentPolicy.RepeatFade,
    presentation: { presentationId: 5, index: 2, language: "en-US" },
    mdCompatLevel: 1,
  });
  assert.deepEqual(fake.log.decoderConstructed[0], {
    outputLevelDbfs: -23,
    drc: Ac4DrcMode.FlatPanelTv,
    downmix: Ac4DownmixTarget.Stereo,
    decodingMode: Ac4DecodingMode.Core,
    concealment: Ac4ConcealmentPolicy.RepeatFade,
    presentationId: 5,
    presentationIndex: 2,
    language: "en-US",
    level: 1,
  });
});

test("decodeFrame returns the scripted frame, then null once the script is exhausted", async () => {
  const scripted = ac4Frame({ channels: [pcm(1, 2)], speakers: ["L"] });
  fake = makeFakeAc4Module({ decodeScript: [scripted] });
  const module = await loadAc4Module(GLUE_URL);
  const decoder = new Ac4Decoder(module);

  const first = decoder.decodeFrame(Uint8Array.of(1, 2, 3));
  assert.equal(first, scripted);
  assert.deepEqual(fake.log.decoded, [[1, 2, 3]]);

  const second = decoder.decodeFrame(Uint8Array.of(4));
  assert.equal(second, null);
});

test("setOutput/setPresentation/reset forward their arguments to the native decoder", async () => {
  fake = makeFakeAc4Module();
  const module = await loadAc4Module(GLUE_URL);
  const decoder = new Ac4Decoder(module);

  // Every field omitted: each of setOutput's/setPresentation's own `??`
  // fallbacks (distinct from the constructor's own, separately-covered
  // ones) takes its "unset" branch here.
  decoder.setOutput();
  assert.deepEqual(fake.log.outputsSet[0], [NaN, Ac4DrcMode.Default, false, 0, Ac4DownmixTarget.AsCoded, true, 0, 0]);
  decoder.setPresentation();
  assert.deepEqual(fake.log.presentationsSet[0], [-1, -1, ""]);

  // Every field given: the complementary "set" branch.
  decoder.setOutput({
    outputLevelDbfs: -18,
    drc: Ac4DrcMode.HomeTheatre,
    headphones: true,
    dialogueEnhancementDb: 6,
    downmix: Ac4DownmixTarget.Stereo,
    mixLfe: false,
    dialogueGainDb: -3,
    associatedGainDb: -6,
  });
  assert.deepEqual(fake.log.outputsSet[1], [-18, Ac4DrcMode.HomeTheatre, true, 6, Ac4DownmixTarget.Stereo, false, -3, -6]);
  decoder.setPresentation({ presentationId: 7, index: 3, language: "en-US" });
  assert.deepEqual(fake.log.presentationsSet[1], [7, 3, "en-US"]);

  decoder.reset();
  assert.equal(fake.log.decoderReset, 1);
});

test("refusalReason/latencySamples/presentations read the native decoder's current state", async () => {
  const presentations = [{ index: 0, presentationId: 1, mdCompat: 1, enabled: true, alternative: false, name: "", language: "en", decodable: true, selectable: true, speakers: ["L", "R"] }];
  fake = makeFakeAc4Module({ refusalReason: "kUnsupported: 22.2", latencySamples: 1313, presentations });
  const module = await loadAc4Module(GLUE_URL);
  const decoder = new Ac4Decoder(module);

  assert.equal(decoder.refusalReason, "kUnsupported: 22.2");
  assert.equal(decoder.latencySamples, 1313);
  assert.deepEqual(decoder.presentations, presentations);
});

test("Ac4Decoder.close() deletes the native decoder exactly once", async () => {
  fake = makeFakeAc4Module();
  const module = await loadAc4Module(GLUE_URL);
  const decoder = new Ac4Decoder(module);
  decoder.close();
  decoder.close();
  assert.equal(fake.log.decoderDeleted, 1);
});

test("Ac4Encoder's constructor defaults match the C++ EncoderConfig defaults", async () => {
  fake = makeFakeAc4Module();
  const module = await loadAc4Module(GLUE_URL);
  new Ac4Encoder(module);
  assert.deepEqual(fake.log.encoderConstructed[0], {
    channels: 2,
    sampleRateHz: 48000,
    frameRateIndex: 13,
    bitrateKbps: 192,
    rateMode: Ac4RateMode.Constant,
    codecMode: Ac4CodecMode.Auto,
    iframeInterval: 24,
    dialnormDb: -31,
  });
});

test("Ac4Encoder's constructor passes explicit options through untouched", async () => {
  fake = makeFakeAc4Module();
  const module = await loadAc4Module(GLUE_URL);
  new Ac4Encoder(module, {
    channels: 6,
    sampleRateHz: 44100,
    frameRateIndex: 6,
    bitrateKbps: 384,
    rateMode: Ac4RateMode.Variable,
    codecMode: Ac4CodecMode.AspxAcpl2,
    iframeInterval: 1,
    dialnormDb: -24,
  });
  assert.deepEqual(fake.log.encoderConstructed[0], {
    channels: 6,
    sampleRateHz: 44100,
    frameRateIndex: 6,
    bitrateKbps: 384,
    rateMode: Ac4RateMode.Variable,
    codecMode: Ac4CodecMode.AspxAcpl2,
    iframeInterval: 1,
    dialnormDb: -24,
  });
});

test("encode returns the scripted frames per call, then [] once the script is exhausted", async () => {
  const encoded = [ac4EncodedFrame({ data: Uint8Array.of(9, 9), samples: 2048, iframe: true })];
  fake = makeFakeAc4Module({ encodeScript: [encoded] });
  const module = await loadAc4Module(GLUE_URL);
  const encoder = new Ac4Encoder(module);

  // 0.5/0.25 (exact in binary, unlike 0.1/0.2) round-trip through the
  // Float32Array -> Array.from() conversion below without float32 rounding
  // drift, so the log's captured values compare equal to plain number
  // literals rather than needing an epsilon.
  const first = encoder.encode([pcm(0.5, 0.25)]);
  assert.equal(first, encoded);
  assert.deepEqual(fake.log.encoded, [[[0.5, 0.25]]]);

  const second = encoder.encode([pcm(0.75, 0.125)]);
  assert.deepEqual(second, []);
});

test("flush returns the scripted flush frames", async () => {
  const flushed = [ac4EncodedFrame({ samples: 512 })];
  fake = makeFakeAc4Module({ flushScript: flushed });
  const module = await loadAc4Module(GLUE_URL);
  const encoder = new Ac4Encoder(module);
  assert.equal(encoder.flush(), flushed);
});

test("error/codecMode/delaySamples/decoderDelaySamples read the native encoder's current state", async () => {
  fake = makeFakeAc4Module({ encoderError: "bitrate is not valid for this configuration", codecMode: Ac4CodecMode.Simple, delaySamples: 3072, decoderDelaySamples: 1313 });
  const module = await loadAc4Module(GLUE_URL);
  const encoder = new Ac4Encoder(module);

  assert.equal(encoder.error, "bitrate is not valid for this configuration");
  assert.equal(encoder.codecMode, Ac4CodecMode.Simple);
  assert.equal(encoder.delaySamples, 3072);
  assert.equal(encoder.decoderDelaySamples, 1313);
});

test("buildDac4/dac4Refusal read through to the native encoder", async () => {
  const dac4Bytes = Uint8Array.of(1, 0, 0, 2);
  fake = makeFakeAc4Module({ dac4Bytes, dac4Refusal: "" });
  const module = await loadAc4Module(GLUE_URL);
  const encoder = new Ac4Encoder(module);

  assert.equal(encoder.buildDac4(), dac4Bytes);
  assert.equal(encoder.dac4Refusal(), "");
});

test("Ac4Encoder.close() deletes the native encoder exactly once", async () => {
  fake = makeFakeAc4Module();
  const module = await loadAc4Module(GLUE_URL);
  const encoder = new Ac4Encoder(module);
  encoder.close();
  encoder.close();
  assert.equal(fake.log.encoderDeleted, 1);
});

test("syncFrame forwards the frame and crc flag to the native module and returns its result", async () => {
  const wrapped = Uint8Array.of(0xac, 0x40, 1, 2, 3);
  fake = makeFakeAc4Module({ syncFrameResult: wrapped });
  const module = await loadAc4Module(GLUE_URL);

  const result = syncFrame(module, Uint8Array.of(1, 2, 3), true);
  assert.equal(result, wrapped);
  assert.deepEqual(fake.log.syncFramed, [{ length: 3, crc: true }]);
});
