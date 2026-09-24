// PushDecoder/scanStream against a scripted Embind stand-in (fake-embind.js).
// These pin the wrapper's contract - how raw native results map onto the
// typed PushOutcome/ScanOutcome unions, the defaults it passes the native
// constructor, the fold/object accessors' bounds, and close()'s lifetime
// rule - not decoding itself, which is C++ and tested there.

import { test } from "node:test";
import assert from "node:assert/strict";
import { PushDecoder, scanStream } from "../dist/push-decoder.js";
import { DownmixTarget } from "../dist/types.js";
import { failure, frame, holdBack, makeFakeModule, pcm } from "./fake-embind.js";

test("scanStream passes a successful scan through as a typed outcome", () => {
  const scan = { ok: true, kind: "AC-3", sampleRate: 44100, accessUnits: [{ offset: 0, length: 1536 }] };
  const { module } = makeFakeModule({ scan });
  assert.deepEqual(scanStream(module, new Uint8Array(4)), {
    ok: true,
    kind: "AC-3",
    sampleRate: 44100,
    accessUnits: [{ offset: 0, length: 1536 }],
  });
});

test("scanStream keeps the native error, and names one when the native side gave none", () => {
  assert.deepEqual(scanStream(makeFakeModule({ scan: { ok: false, error: "no sync word" } }).module, new Uint8Array(1)), {
    ok: false,
    error: "no sync word",
  });
  assert.deepEqual(scanStream(makeFakeModule({ scan: { ok: false } }).module, new Uint8Array(1)), {
    ok: false,
    error: "scan failed",
  });
});

test("the constructor defaults to no fold, no dialnorm and no LFE mix", () => {
  const { module, log } = makeFakeModule();
  new PushDecoder(module);
  assert.deepEqual(log.constructed, [{ foldTarget: DownmixTarget.AsCoded, applyDialnorm: false, mixLfe: false }]);
});

test("the constructor forwards every fold option it is given", () => {
  const { module, log } = makeFakeModule();
  new PushDecoder(module, { target: DownmixTarget.LtRt, applyDialnorm: true, mixLfe: true });
  new PushDecoder(module, { target: DownmixTarget.Mono });
  assert.deepEqual(log.constructed, [
    { foldTarget: DownmixTarget.LtRt, applyDialnorm: true, mixLfe: true },
    { foldTarget: DownmixTarget.Mono, applyDialnorm: false, mixLfe: false },
  ]);
});

test("push maps a decoded frame's metadata onto the outcome", () => {
  const { module, log } = makeFakeModule({
    script: [frame({ frameSamples: 256, dialnorm: -27, channels: [pcm(1), pcm(2)], channelLabels: ["L", "R"], fold: [pcm(3)] })],
  });
  const decoder = new PushDecoder(module);
  const outcome = decoder.push(Uint8Array.of(7, 8));
  assert.deepEqual(log.pushed, [[7, 8]]);
  assert.deepEqual(outcome, {
    ok: true,
    holdBack: false,
    sampleRate: 48000,
    frameSamples: 256,
    dialnorm: -27,
    channelCount: 2,
    channelLabels: ["L", "R"],
    foldChannelCount: 1,
    objectCount: 0,
    objectLabels: [],
  });
});

test("push reports a held-back frame without any metadata", () => {
  const { module } = makeFakeModule({ script: [holdBack()] });
  assert.deepEqual(new PushDecoder(module).push(new Uint8Array(1)), { ok: true, holdBack: true });
});

test("push keeps the native decode error, and names one when the native side gave none", () => {
  const { module } = makeFakeModule({ script: [failure("CRC mismatch"), failure()] });
  const decoder = new PushDecoder(module);
  assert.deepEqual(decoder.push(new Uint8Array(1)), { ok: false, error: "CRC mismatch" });
  assert.deepEqual(decoder.push(new Uint8Array(1)), { ok: false, error: "decode failed" });
});

test("channel and fold return the native views, and fold refuses an index past the fold's width", () => {
  const left = pcm(0.5, -0.5);
  const lo = pcm(0.25);
  const { module } = makeFakeModule({ script: [frame({ channels: [left], fold: [lo] })] });
  const decoder = new PushDecoder(module);
  decoder.push(new Uint8Array(1));
  assert.equal(decoder.channel(0), left);
  assert.equal(decoder.channel(1), null);
  assert.equal(decoder.foldChannelCount, 1);
  assert.equal(decoder.fold(0), lo);
  assert.equal(decoder.fold(1), null);
});

test("objectFrame pairs an object's label with its position, and is null when the object carries none", () => {
  const position = pcm(0.1, 0.2, 0.3, -6, 0, 0, 0);
  const audio = pcm(9);
  const { module } = makeFakeModule({
    script: [frame({ objects: [audio, pcm(0)], objectLabels: ["dialog", "fx"], positions: [position, null] })],
  });
  const decoder = new PushDecoder(module);
  decoder.push(new Uint8Array(1));
  assert.equal(decoder.objectCount, 2);
  assert.deepEqual(decoder.objectFrame(0), { label: "dialog", position });
  assert.equal(decoder.objectFrame(1), null);
  assert.equal(decoder.objectAudio(0), audio);
});

test("flush and flushedChannel hand back the native tail", () => {
  const tail = pcm(4, 5);
  const entries = [{ ok: true, holdBack: false, flushIndex: 0, channelCount: 1 }];
  const { module } = makeFakeModule({ flushEntries: entries, flushed: [[tail]] });
  const decoder = new PushDecoder(module);
  assert.equal(decoder.flush(), entries);
  assert.equal(decoder.flushedChannel(0, 0), tail);
  assert.equal(decoder.flushedChannel(0, 1), null);
});

test("close releases the native object exactly once however often it is called", () => {
  const { module, log } = makeFakeModule();
  const decoder = new PushDecoder(module);
  decoder.close();
  decoder.close();
  assert.equal(log.deleted, 1);
});
