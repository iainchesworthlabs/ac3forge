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
  Ac4BedChannel,
  Ac4ObjectCoding,
  Ac4AjocDownmix,
  Ac4AdditionalPair,
  syncFrame,
} from "../dist/ac4.js";
import { ac4Frame, ac4EncodedFrame, makeFakeAc4Module, makeLoopbackAc4Module, pcm } from "./fake-embind.js";

const fetched = [];
let factoryArgs = null;
let fake = null;

// The glue's module factory, reachable from the data: URL the fake
// createObjectURL below produces (a data: module cannot close over this
// file's scope, so it reads it off globalThis) - same indirection
// decoder-worker.test.js uses for createIclForgeModule.
globalThis.__ac4FakeFactory = async (overrides) => {
  factoryArgs = overrides;
  return fake.module;
};

globalThis.fetch = async (url) => {
  fetched.push(String(url));
  return { text: async () => "var createIclForgeAc4Module = globalThis.__ac4FakeFactory;" };
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

const GLUE_URL = "https://example.test/wasm/iclforge_ac4.js";

test("loadAc4Module fetches the glue, resolves the wasm beside it, and returns the fake module", async () => {
  fake = makeFakeAc4Module();
  fetched.length = 0;
  revoked.length = 0;
  const module = await loadAc4Module(GLUE_URL);
  assert.deepEqual(fetched, [GLUE_URL]);
  assert.equal(factoryArgs.locateFile("iclforge_ac4.wasm"), "https://example.test/wasm/iclforge_ac4.wasm");
  assert.equal(revoked.length > 0, true, "the object URL is revoked once imported");
  assert.equal(module, fake.module);
});

test("Ac4Decoder's constructor defaults every option to the NaN/-1 'unset' sentinels", async () => {
  fake = makeFakeAc4Module();
  const module = await loadAc4Module(GLUE_URL);
  const decoder = new Ac4Decoder(module);
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
  decoder.close();
});

test("Ac4Decoder's constructor passes explicit options through untouched", async () => {
  fake = makeFakeAc4Module();
  const module = await loadAc4Module(GLUE_URL);
  const decoder = new Ac4Decoder(module, {
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
  decoder.close();
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

test("Ac4Encoder's constructor hands the native encoder the options as given, and none by default", async () => {
  fake = makeFakeAc4Module();
  const module = await loadAc4Module(GLUE_URL);
  const encoder = new Ac4Encoder(module);
  // A field left out keeps the C++ struct's default: the defaults live in
  // ac4_bindings.cpp, not here.
  assert.deepEqual(fake.log.encoderConstructed[0], {});
  encoder.close();
});

test("Ac4Encoder's constructor passes explicit options through untouched", async () => {
  fake = makeFakeAc4Module();
  const module = await loadAc4Module(GLUE_URL);
  const options = {
    channels: 6,
    sampleRateHz: 44100,
    frameRateIndex: 6,
    bitrateKbps: 384,
    rateMode: Ac4RateMode.Variable,
    codecMode: Ac4CodecMode.AspxAcpl2,
    iframeInterval: 1,
    dialnormDb: -24,
    iframes: [5, 2],
    fragmentStarts: [4096, 9000],
    experimental: { aspxBalance: true, sevenX: Ac4AdditionalPair.Wide, acpl: true },
  };
  const encoder = new Ac4Encoder(module, options);
  assert.deepEqual(fake.log.encoderConstructed[0], options);
  encoder.close();
});

test("Ac4Encoder's constructor passes an object substream through untouched", async () => {
  fake = makeFakeAc4Module();
  const module = await loadAc4Module(GLUE_URL);
  const options = {
    bitrateKbps: 256,
    experimental: { objects: true },
    objects: {
      objects: [
        { properties: { position: [0.1, 0.2, 0], gainDb: -3, width: [0.2, 0.4, 0.6], zoneMask: 3, distance: Infinity } },
        { lfe: true },
        { bed: Ac4BedChannel.TopBackRight, properties: { headphoneRenderMode: 1, trimDisabled: true } },
      ],
      coding: Ac4ObjectCoding.Direct,
      downmix: Ac4AjocDownmix.Static51,
      downmixSignals: 4,
      decorrelation: true,
      parameterBands: 15,
      coarse: false,
      screenSizeRatioCode: 20,
      bedObjectChanDistribute: true,
    },
  };
  const encoder = new Ac4Encoder(module, options);
  assert.deepEqual(fake.log.encoderConstructed[0], options);
  encoder.close();
});

test("constructionError reads why the constructor made no encoder", async () => {
  fake = makeFakeAc4Module({ constructionError: "objects at a frame_rate_index other than 13" });
  const module = await loadAc4Module(GLUE_URL);
  const encoder = new Ac4Encoder(module, { frameRateIndex: 2, objects: { objects: [{}] } });
  assert.equal(encoder.constructionError, "objects at a frame_rate_index other than 13");
  fake = makeFakeAc4Module();
  assert.equal(new Ac4Encoder(await loadAc4Module(GLUE_URL)).constructionError, "");
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
  // No updates given: the native encoder is handed an empty array, never undefined.
  assert.deepEqual(fake.log.encodedUpdates, [[]]);

  const second = encoder.encode([pcm(0.75, 0.125)]);
  assert.deepEqual(second, []);
});

test("encode forwards the metadata updates with the input they belong to", async () => {
  fake = makeFakeAc4Module();
  const module = await loadAc4Module(GLUE_URL);
  const encoder = new Ac4Encoder(module, { experimental: { objects: true }, objects: { objects: [{}, {}] } });
  const updates = [
    { object: 1, sample: 5000, rampSamples: 1024, properties: { position: [0.75, 0.25, 0.4], gainDb: -12 } },
    { object: 0, sample: 0 },
  ];
  encoder.encode([pcm(0.5), pcm(0.25)], updates);
  assert.deepEqual(fake.log.encoded, [[[0.5], [0.25]]]);
  assert.deepEqual(fake.log.encodedUpdates, [updates]);
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

test("decodeFrame's objects carry every property and the updates within the frame", async () => {
  const properties = (over = {}) => ({
    active: true, gainDb: -6, priority: 0.5, position: [0.25, 0.5, -0.4], zoneMask: 3, enableElevation: false,
    snap: true, width: [0.1, 0.2, 0.3], screenFactor: 0.5, depthExponent: 2, distance: 4, divergence: 0.75,
    trimDisabled: true, headphoneRenderMode: 1, headTrackDisabled: true, ...over,
  });
  const object = {
    kind: "dyn", lfe: false, speaker: null, samples: pcm(0, 1), properties: properties(),
    updates: [{ sample: 1313, rampSamples: 1024, properties: properties({ gainDb: -12 }) }],
  };
  fake = makeFakeAc4Module({ decodeScript: [ac4Frame({ objects: [object] })] });
  const module = await loadAc4Module(GLUE_URL);
  const frame = new Ac4Decoder(module).decodeFrame(Uint8Array.of(1));
  assert.deepEqual(frame.objects, [object]);
});

// Three whole frames of input and a partial fourth.
const LOOPBACK_SAMPLES = 3 * 2048 + 1000;
const tone = (cycles) =>
  Float32Array.from({ length: LOOPBACK_SAMPLES }, (_, n) => 0.1 * Math.sin((2 * Math.PI * cycles * n) / LOOPBACK_SAMPLES));

// What one property's code can hold: X and Y in 62 steps, Z in 15, the gain in 1 dB, the priority
// and each width in 31 - the steps iclforge::ac4::ObjectProperties documents.
function assertNear(got, want, dynamic) {
  assert.equal(got.active, want.active ?? true);
  if (want.active === false) return;
  assert.ok(Math.abs(got.gainDb - (want.gainDb ?? 0)) <= 0.5 + 1e-9, `gain ${got.gainDb} for ${want.gainDb}`);
  assert.ok(Math.abs(got.priority - (want.priority ?? 1)) <= 1 / 62 + 1e-9);
  if (!dynamic) return;
  const [x, y, z] = want.position ?? [0.5, 0.5, 0];
  assert.ok(Math.abs(got.position[0] - x) <= 1 / 124 + 1e-9, `x ${got.position[0]} for ${x}`);
  assert.ok(Math.abs(got.position[1] - y) <= 1 / 124 + 1e-9);
  assert.ok(Math.abs(got.position[2] - z) <= 1 / 30 + 1e-9);
  (want.width ?? [0, 0, 0]).forEach((w, axis) => assert.ok(Math.abs(got.width[axis] - w) <= 1 / 62 + 1e-9));
  assert.equal(got.zoneMask, want.zoneMask ?? 0);
}

const LOOPBACK_SCENES = [
  [
    "A-JOC",
    Ac4ObjectCoding.Ajoc,
    [
      { properties: { position: [0.1, 0.2, 0], gainDb: -3 } },
      { lfe: true },
      { properties: { position: [0.9, 0.5, 7 / 15], gainDb: -6.4, priority: 16 / 31, width: [0.2, 0.4, 0.6], zoneMask: 3 } },
      { bed: Ac4BedChannel.Left, properties: { gainDb: -12 } },
    ],
  ],
  [
    "direct-coded",
    Ac4ObjectCoding.Direct,
    [
      { properties: { position: [0, 0, 0], gainDb: -3 } },
      { properties: { position: [0.33, 0.66, 0.2], width: [0.1, 0.3, 0.5], zoneMask: 5 } },
      { lfe: true },
      { properties: { position: [0.67, 0.34, -0.2], active: false } },
    ],
  ],
];

for (const [name, coding, objects] of LOOPBACK_SCENES) {
  test(`${name} objects round-trip through the wrapper (loopback codec) with their metadata within the codec's tolerance`, () => {
    const module = makeLoopbackAc4Module();
    const encoder = new Ac4Encoder(module, {
      bitrateKbps: 256,
      experimental: { objects: true },
      objects: { objects, coding },
    });
    const input = objects.map((_, k) => tone(k + 2));
    // The first dynamic object moves at sample 5000 to a position and gain of its own, over 1 024
    // samples; the update's sample counts from this call's first sample.
    const first = objects.findIndex((o) => !o.lfe && o.bed === undefined);
    const update = {
      object: first,
      sample: 5000,
      rampSamples: 1024,
      properties: { position: [0.75, 0.25, 0.4], gainDb: -12 },
    };
    const frames = [...encoder.encode(input, [update]), ...encoder.flush()];
    assert.equal(frames.length, 4);
    assert.equal(frames[0].iframe, true);

    const decoder = new Ac4Decoder(module);
    const decoded = frames.map((f) => decoder.decodeFrame(f.data));
    // The decoder's order: the LFE first, then the bed objects, then the dynamic objects.
    const indices = [...objects.keys()];
    const order = [
      ...indices.filter((k) => objects[k].lfe),
      ...indices.filter((k) => !objects[k].lfe && objects[k].bed !== undefined),
      ...indices.filter((k) => !objects[k].lfe && objects[k].bed === undefined),
    ];
    const last = decoded.at(-1).objects;
    assert.equal(last.length, order.length);
    order.forEach((k, d) => {
      const dynamic = !objects[k].lfe && objects[k].bed === undefined;
      assert.equal(last[d].lfe, objects[k].lfe ?? false);
      assert.equal(last[d].kind, objects[k].bed !== undefined ? "bed" : "dyn");
      assertNear(last[d].properties, k === first ? update.properties : (objects[k].properties ?? {}), dynamic);
      // Its audio, end to end, is its own tone.
      const samples = Float32Array.from(decoded.flatMap((f) => [...f.objects[d].samples])).subarray(0, LOOPBACK_SAMPLES);
      assert.deepEqual(samples, input[k]);
    });

    // The update is reported by the frame it falls in, at its sample of that frame, with its ramp.
    const moved = order.indexOf(first);
    const reported = decoded.flatMap((f, i) => f.objects[moved].updates.map((u) => ({ ...u, sample: u.sample + i * 2048 })));
    assert.equal(reported.length, 1);
    assert.equal(reported[0].sample, 5000);
    assert.equal(reported[0].rampSamples, 1024);
    assertNear(reported[0].properties, update.properties, true);
    // ...and the frame after it has it in force from its first sample, the one before still not.
    assertNear(decoded[3].objects[moved].properties, update.properties, true);
    assertNear(decoded[1].objects[moved].properties, objects[first].properties ?? {}, true);
    encoder.close();
    decoder.close();
  });
}

test("syncFrame forwards the frame and crc flag to the native module and returns its result", async () => {
  const wrapped = Uint8Array.of(0xac, 0x40, 1, 2, 3);
  fake = makeFakeAc4Module({ syncFrameResult: wrapped });
  const module = await loadAc4Module(GLUE_URL);

  const result = syncFrame(module, Uint8Array.of(1, 2, 3), true);
  assert.equal(result, wrapped);
  assert.deepEqual(fake.log.syncFramed, [{ length: 3, crc: true }]);
});
