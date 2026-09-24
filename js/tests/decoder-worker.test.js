// decoder-worker.ts's message protocol, run in Node with the Worker globals
// it touches (self, postMessage, fetch, Blob/URL.createObjectURL) replaced by
// fakes. The Emscripten glue it fetches is a two-line script defining
// `createAc3ForgeModule`, which resolves to the scripted Embind stand-in from
// fake-embind.js - so what runs here is the worker's real glue loading
// (fetch, re-export through an object URL, dynamic import), its ring-buffer
// writes and its replies, with only the codec swapped out.
//
// node --test runs each file in its own process, so the module-level
// listener decoder-worker.js registers on import cannot leak into other files.

import { test, before } from "node:test";
import assert from "node:assert/strict";
import { allocateRingBuffer, RingBufferReader } from "../dist/ring-buffer.js";
import { frame, holdBack, failure, makeFakeModule, pcm } from "./fake-embind.js";

const posted = [];
const fetched = [];
let listener = null;
let closed = 0;
let factoryArgs = null;
let fake = null;

// The glue's module factory, reachable from the data: URL the fake
// createObjectURL below produces (a data: module cannot close over this
// file's scope, so it reads it off globalThis).
globalThis.__ac3forgeFakeFactory = async (overrides) => {
  factoryArgs = overrides;
  return fake.module;
};

globalThis.self = {
  addEventListener(type, handler) {
    assert.equal(type, "message");
    listener = handler;
  },
  close() {
    closed++;
  },
};
globalThis.postMessage = (message) => posted.push(message);
globalThis.fetch = async (url) => {
  fetched.push(String(url));
  return { text: async () => "var createAc3ForgeModule = globalThis.__ac3forgeFakeFactory;" };
};

// Node can import data: URLs but not blob: URLs, so the object URL the worker
// imports from is made a data: URL carrying the same source.
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

before(async () => {
  await import("../dist/decoder-worker.js");
  assert.ok(listener, "the worker registers a message listener on import");
});

const send = (data) => listener({ data });
const flushTasks = () => new Promise((resolve) => setTimeout(resolve, 0));

function ring(channelCount, capacityFrames) {
  const layout = { channelCount, capacityFrames };
  const sab = allocateRingBuffer(layout);
  return { sab, layout, reader: new RingBufferReader(sab, layout) };
}

async function init(fakeModule, { writeTarget = "channels", channelCount = 2, capacityFrames = 8, fold } = {}) {
  fake = fakeModule;
  posted.length = 0;
  const r = ring(channelCount, capacityFrames);
  send({ type: "init", glueUrl: "https://example.test/wasm/ac3forge_decode.js", sab: r.sab, layout: r.layout, writeTarget, fold });
  await flushTasks();
  return r;
}

// Order matters for this one: it has to run before any init.
test("a push before init is answered with an error, not a crash", async () => {
  posted.length = 0;
  send({ type: "push", bytes: new ArrayBuffer(1) });
  await flushTasks();
  assert.deepEqual(posted, [{ type: "error", message: "decoder-worker: push before init" }]);
});

test("a flush before init is ignored", async () => {
  posted.length = 0;
  send({ type: "flush" });
  await flushTasks();
  assert.deepEqual(posted, []);
});

test("init loads the glue from its URL, resolves the wasm beside it, and reports ready", async () => {
  await init(makeFakeModule(), { fold: { target: 1 } });
  assert.deepEqual(fetched.at(-1), "https://example.test/wasm/ac3forge_decode.js");
  assert.equal(factoryArgs.locateFile("ac3forge_decode.wasm"), "https://example.test/wasm/ac3forge_decode.wasm");
  assert.equal(revoked.length > 0, true, "the object URL is revoked once imported");
  assert.equal(fake.log.constructed[0].foldTarget, 1);
  assert.deepEqual(posted, [{ type: "ready" }]);
});

test("a decoded frame's channels are written to the ring and its outcome posted", async () => {
  const r = await init(
    makeFakeModule({ script: [frame({ frameSamples: 2, channels: [pcm(1, 2), pcm(3, 4)], channelLabels: ["L", "R"] })] }),
  );
  posted.length = 0;
  send({ type: "push", bytes: Uint8Array.of(9).buffer });
  await flushTasks();

  assert.deepEqual(fake.log.pushed, [[9]]);
  assert.equal(posted.length, 1);
  assert.equal(posted[0].type, "result");
  assert.equal(posted[0].outcome.frameSamples, 2);
  assert.deepEqual(posted[0].objectFrames, []);
  const out = [new Float32Array(2), new Float32Array(2)];
  assert.equal(r.reader.read(out, 2), 2);
  assert.deepEqual(out.map((c) => Array.from(c)), [
    [1, 2],
    [3, 4],
  ]);
});

test("with writeTarget fold, the fold is what reaches the ring", async () => {
  const r = await init(
    makeFakeModule({ script: [frame({ frameSamples: 1, channels: [pcm(9), pcm(9), pcm(9)], fold: [pcm(0.5), pcm(-0.5)] })] }),
    { writeTarget: "fold" },
  );
  send({ type: "push", bytes: new ArrayBuffer(1) });
  await flushTasks();
  const out = [new Float32Array(1), new Float32Array(1)];
  assert.equal(r.reader.read(out, 1), 1);
  assert.deepEqual(out.map((c) => c[0]), [0.5, -0.5]);
});

test("a frame that does not fit in the ring reports how much was dropped", async () => {
  await init(makeFakeModule({ script: [frame({ frameSamples: 6, channels: [new Float32Array(6)] })] }), {
    channelCount: 1,
    capacityFrames: 4,
  });
  posted.length = 0;
  send({ type: "push", bytes: new ArrayBuffer(1) });
  await flushTasks();
  assert.deepEqual(posted[0], { type: "overrun", framesDropped: 2 });
  assert.equal(posted[1].type, "result");
});

test("object positions travel with the result as plain arrays", async () => {
  const position = pcm(0.5, 0.25, 1, -6, 0, 0, 0);
  await init(
    makeFakeModule({
      script: [frame({ frameSamples: 1, channels: [pcm(0)], objects: [pcm(0), pcm(0)], objectLabels: ["a", "b"], positions: [position, null] })],
    }),
    { channelCount: 1 },
  );
  posted.length = 0;
  send({ type: "push", bytes: new ArrayBuffer(1) });
  await flushTasks();
  assert.deepEqual(posted[0].objectFrames, [{ label: "a", position: Array.from(position) }]);
});

test("a held-back frame or a decode failure is posted without touching the ring", async () => {
  const r = await init(makeFakeModule({ script: [holdBack(), failure("bad crc")] }));
  posted.length = 0;
  send({ type: "push", bytes: new ArrayBuffer(1) });
  send({ type: "push", bytes: new ArrayBuffer(1) });
  await flushTasks();
  assert.deepEqual(posted, [
    { type: "result", outcome: { ok: true, holdBack: true } },
    { type: "result", outcome: { ok: false, error: "bad crc" } },
  ]);
  assert.equal(r.reader.framesAvailable, 0);
});

test("flush posts the decoder's flush entries", async () => {
  const entries = [{ flushIndex: 0, channelCount: 2 }];
  await init(makeFakeModule({ flushEntries: entries }));
  posted.length = 0;
  send({ type: "flush" });
  await flushTasks();
  assert.deepEqual(posted, [{ type: "flushed", entries }]);
});

test("close releases the decoder and closes the worker", async () => {
  await init(makeFakeModule());
  send({ type: "close" });
  await flushTasks();
  assert.equal(fake.log.deleted, 1);
  assert.equal(closed, 1);
});
