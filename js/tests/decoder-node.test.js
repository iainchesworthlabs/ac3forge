// Ac3ForgeDecoderNode's main-thread orchestration, in Node, with the Web Audio
// and Worker globals it constructs (AudioWorkletNode, Worker,
// crossOriginIsolated) replaced by fakes that record what they were given.
// The fake Worker answers "init" with "ready" the way decoder-worker.ts does,
// and a test can post any worker reply through it - so this pins the node's
// setup (ring sizing, which output feeds the ring, processor options), its
// push/flush/close messages, whenIdle(), and which replies become which events.

import { test } from "node:test";
import assert from "node:assert/strict";
import { Ac3ForgeDecoderNode } from "../dist/decoder-node.js";
import { DownmixTarget } from "../dist/types.js";

class FakePort extends EventTarget {
  started = false;
  start() {
    this.started = true;
  }
  emit(data) {
    this.dispatchEvent(new MessageEvent("message", { data }));
  }
}

let lastNode = null;
globalThis.AudioWorkletNode = class {
  constructor(context, name, options) {
    this.context = context;
    this.name = name;
    this.options = options;
    this.port = new FakePort();
    this.disconnected = false;
    lastNode = this;
  }
  disconnect() {
    this.disconnected = true;
  }
};

let lastWorker = null;
globalThis.Worker = class extends EventTarget {
  constructor(url, options) {
    super();
    this.url = url;
    this.options = options;
    this.posted = [];
    this.terminated = false;
    lastWorker = this;
  }
  postMessage(message, transfer) {
    this.posted.push({ message, transfer });
    if (message.type === "init") queueMicrotask(() => this.reply({ type: "ready" }));
  }
  reply(data) {
    this.dispatchEvent(new MessageEvent("message", { data }));
  }
  terminate() {
    this.terminated = true;
  }
};

function fakeContext(sampleRate = 48000) {
  const added = [];
  return { sampleRate, added, audioWorklet: { addModule: async (url) => added.push(String(url)) } };
}

const urls = {
  workletProcessorUrl: "https://example.test/worklet-processor.js",
  workerUrl: "https://example.test/decoder-worker.js",
  wasmGlueUrl: new URL("https://example.test/ac3forge_decode.js"),
};

function isolated(value) {
  globalThis.crossOriginIsolated = value;
}

const decoded = (overrides = {}) => ({
  ok: true,
  holdBack: false,
  sampleRate: 48000,
  frameSamples: 1536,
  dialnorm: -31,
  channelCount: 6,
  channelLabels: ["L", "R", "C", "LFE", "Ls", "Rs"],
  foldChannelCount: 0,
  objectCount: 0,
  objectLabels: [],
  ...overrides,
});

function collect(node, type) {
  const seen = [];
  node.addEventListener(type, (event) => seen.push(event.detail));
  return seen;
}

test("create refuses to run without cross-origin isolation", async () => {
  isolated(false);
  await assert.rejects(Ac3ForgeDecoderNode.create(fakeContext(), { ...urls, channelCount: 2 }), /cross-origin isolation/);
});

test("create needs a channel count unless a real fold decides it", async () => {
  isolated(true);
  await assert.rejects(Ac3ForgeDecoderNode.create(fakeContext(), urls), /channelCount is required/);
  await assert.rejects(
    Ac3ForgeDecoderNode.create(fakeContext(), { ...urls, fold: { target: DownmixTarget.AsCoded } }),
    /channelCount is required/,
  );
});

test("create wires the worklet, the worker and a power-of-two ring, then waits for ready", async () => {
  isolated(true);
  const context = fakeContext(44100);
  const node = await Ac3ForgeDecoderNode.create(context, { ...urls, channelCount: 6, ringBufferSeconds: 1 });

  assert.deepEqual(context.added, [urls.workletProcessorUrl]);
  assert.equal(node.node, lastNode);
  assert.equal(lastNode.name, "ac3forge-pcm-source");
  assert.deepEqual(lastNode.options.outputChannelCount, [6]);
  assert.equal(lastNode.options.numberOfInputs, 0);
  assert.deepEqual(lastNode.options.processorOptions.layout, { channelCount: 6, capacityFrames: 65536 });
  assert.ok(lastNode.options.processorOptions.sab instanceof SharedArrayBuffer);
  assert.equal(lastNode.port.started, true);

  assert.equal(lastWorker.url, urls.workerUrl);
  assert.deepEqual(lastWorker.options, { type: "module" });
  const init = lastWorker.posted[0].message;
  assert.equal(init.type, "init");
  assert.equal(init.glueUrl, "https://example.test/ac3forge_decode.js");
  assert.equal(init.writeTarget, "channels");
  assert.equal(init.sab, lastNode.options.processorOptions.sab, "worker and worklet share one ring");
});

test("a non-AsCoded fold defaults to stereo, feeds the ring from the fold, and sizes 2 s by default", async () => {
  isolated(true);
  await Ac3ForgeDecoderNode.create(fakeContext(48000), { ...urls, fold: { target: DownmixTarget.LoRo } });
  const init = lastWorker.posted[0].message;
  assert.equal(init.writeTarget, "fold");
  assert.deepEqual(init.fold, { target: DownmixTarget.LoRo });
  assert.deepEqual(init.layout, { channelCount: 2, capacityFrames: 131072 });
});

test("pushAccessUnit transfers a private copy of exactly the unit's bytes", async () => {
  isolated(true);
  const node = await Ac3ForgeDecoderNode.create(fakeContext(), { ...urls, channelCount: 2 });
  const shared = Uint8Array.of(0, 1, 2, 3, 4, 5);
  node.pushAccessUnit(shared.subarray(2, 5));
  const { message, transfer } = lastWorker.posted.at(-1);
  assert.equal(message.type, "push");
  assert.deepEqual(Array.from(new Uint8Array(message.bytes)), [2, 3, 4]);
  assert.deepEqual(transfer, [message.bytes]);
  assert.notEqual(message.bytes, shared.buffer, "the caller's buffer is never the one transferred");
});

test("whenIdle resolves at once when nothing is pending, else after every push is acknowledged", async () => {
  isolated(true);
  const node = await Ac3ForgeDecoderNode.create(fakeContext(), { ...urls, channelCount: 2 });
  await node.whenIdle();

  node.pushAccessUnit(new Uint8Array(1));
  node.pushAccessUnit(new Uint8Array(1));
  let idle = false;
  const waiting = node.whenIdle().then(() => (idle = true));
  lastWorker.reply({ type: "result", outcome: { ok: true, holdBack: true } });
  await Promise.resolve();
  assert.equal(idle, false, "one of two pushes is still outstanding");
  lastWorker.reply({ type: "result", outcome: { ok: true, holdBack: true } });
  await waiting;
  assert.equal(idle, true);
});

test("streaminfo fires once, on the first decoded frame", async () => {
  isolated(true);
  const node = await Ac3ForgeDecoderNode.create(fakeContext(), { ...urls, channelCount: 6 });
  const infos = collect(node, "streaminfo");
  lastWorker.reply({ type: "result", outcome: { ok: true, holdBack: true } });
  lastWorker.reply({ type: "result", outcome: decoded() });
  lastWorker.reply({ type: "result", outcome: decoded({ sampleRate: 44100 }) });
  assert.deepEqual(infos, [
    { sampleRate: 48000, channelCount: 6, channelLabels: ["L", "R", "C", "LFE", "Ls", "Rs"], foldChannelCount: 0 },
  ]);
});

test("decode failures, worker errors, overruns, underruns and object frames each become their event", async () => {
  isolated(true);
  const node = await Ac3ForgeDecoderNode.create(fakeContext(), { ...urls, channelCount: 2 });
  const errors = collect(node, "error");
  const overruns = collect(node, "overrun");
  const underruns = collect(node, "underrun");
  const objects = collect(node, "objectframe");

  lastWorker.reply({ type: "result", outcome: { ok: false, error: "bad crc" } });
  lastWorker.reply({ type: "error", message: "decoder-worker: push before init" });
  lastWorker.reply({ type: "overrun", framesDropped: 512 });
  lastWorker.reply({ type: "result", outcome: decoded(), objectFrames: [] });
  lastWorker.reply({ type: "result", outcome: decoded(), objectFrames: [{ label: "a", position: [0, 0, 0, 0, 0, 0, 0] }] });
  lastWorker.reply({ type: "something-new" });
  lastNode.port.emit({ type: "underrun", framesShort: 128 });
  lastNode.port.emit({ type: "unrelated" });

  assert.deepEqual(errors, ["bad crc", "decoder-worker: push before init"]);
  assert.deepEqual(overruns, [512]);
  assert.deepEqual(underruns, [128]);
  assert.deepEqual(objects, [[{ label: "a", position: [0, 0, 0, 0, 0, 0, 0] }]], "an empty object list fires nothing");
});

test("flush and close send their messages, and close tears the graph down", async () => {
  isolated(true);
  const node = await Ac3ForgeDecoderNode.create(fakeContext(), { ...urls, channelCount: 2 });
  node.flush();
  node.close();
  assert.deepEqual(
    lastWorker.posted.slice(1).map((p) => p.message),
    [{ type: "flush" }, { type: "close" }],
  );
  assert.equal(lastWorker.terminated, true);
  assert.equal(lastNode.disconnected, true);
});
