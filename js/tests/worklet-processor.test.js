// worklet-processor.ts in Node, with the two AudioWorkletGlobalScope names it
// uses (AudioWorkletProcessor, registerProcessor) provided by hand. The
// processor itself is only a ring-buffer drain, so the real
// RingBufferWriter feeds it and the test reads what it wrote to its output.

import { test, before } from "node:test";
import assert from "node:assert/strict";
import { allocateRingBuffer, RingBufferWriter } from "../dist/ring-buffer.js";

const registered = new Map();

globalThis.AudioWorkletProcessor = class {
  constructor() {
    this.port = { messages: [], postMessage(message) { this.messages.push(message); } };
  }
};
globalThis.registerProcessor = (name, processorClass) => registered.set(name, processorClass);

let Processor;
before(async () => {
  await import("../dist/worklet-processor.js");
  Processor = registered.get("ac3forge-pcm-source");
});

function makeProcessor(channelCount = 2, capacityFrames = 8) {
  const layout = { channelCount, capacityFrames };
  const sab = allocateRingBuffer(layout);
  return { processor: new Processor({ processorOptions: { sab, layout } }), writer: new RingBufferWriter(sab, layout) };
}

test("the processor registers under the name decoder-node.ts constructs", () => {
  assert.equal(typeof Processor, "function");
});

test("constructing without processorOptions is refused by name", () => {
  assert.throws(() => new Processor(), /requires processorOptions/);
  assert.throws(() => new Processor({}), /requires processorOptions/);
});

test("process drains one render quantum from the ring into the output", () => {
  const { processor, writer } = makeProcessor();
  writer.write([Float32Array.of(1, 2, 3), Float32Array.of(4, 5, 6)], 3);
  const output = [new Float32Array(3), new Float32Array(3)];
  assert.equal(processor.process([], [output]), true);
  assert.deepEqual(output.map((c) => Array.from(c)), [
    [1, 2, 3],
    [4, 5, 6],
  ]);
  assert.deepEqual(processor.port.messages, []);
});

test("an underrun fills the rest with silence and reports how many frames were short", () => {
  const { processor, writer } = makeProcessor();
  writer.write([Float32Array.of(1), Float32Array.of(2)], 1);
  const output = [Float32Array.of(9, 9, 9, 9), Float32Array.of(9, 9, 9, 9)];
  assert.equal(processor.process([], [output]), true, "the node stays alive through an underrun");
  assert.deepEqual(Array.from(output[0]), [1, 0, 0, 0]);
  assert.deepEqual(processor.port.messages, [{ type: "underrun", framesShort: 3 }]);
});

test("an output with no channels or no frames is left alone and keeps the node alive", () => {
  const { processor } = makeProcessor();
  assert.equal(processor.process([], []), true);
  assert.equal(processor.process([], [[]]), true);
  assert.equal(processor.process([], [[new Float32Array(0)]]), true);
  assert.deepEqual(processor.port.messages, []);
});
