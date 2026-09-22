// Atomics-based index arithmetic, exercised from a single thread (Node has
// no AudioWorklet to test the real cross-thread handoff against - the
// headless-browser Playwright spec covers that end-to-end). This is purely
// "does the wraparound/underrun/overrun math do what it says".

import { test } from "node:test";
import assert from "node:assert/strict";
import { allocateRingBuffer, RingBufferReader, RingBufferWriter } from "../dist/ring-buffer.js";

function makePair(channelCount, capacityFrames) {
  const layout = { channelCount, capacityFrames };
  const sab = allocateRingBuffer(layout);
  return { writer: new RingBufferWriter(sab, layout), reader: new RingBufferReader(sab, layout) };
}

test("allocateRingBuffer rejects a non-power-of-two capacity", () => {
  assert.throws(() => allocateRingBuffer({ channelCount: 2, capacityFrames: 100 }), RangeError);
});

test("round-trips exactly what was written, per channel", () => {
  const { writer, reader } = makePair(2, 16);
  const left = Float32Array.from({ length: 8 }, (_, i) => i + 1);
  const right = Float32Array.from({ length: 8 }, (_, i) => -(i + 1));
  const written = writer.write([left, right], 8);
  assert.equal(written, 8);

  const outLeft = new Float32Array(8);
  const outRight = new Float32Array(8);
  const read = reader.read([outLeft, outRight], 8);
  assert.equal(read, 8);
  assert.deepEqual(Array.from(outLeft), Array.from(left));
  assert.deepEqual(Array.from(outRight), Array.from(right));
});

test("wraps around the ring correctly", () => {
  const { writer, reader } = makePair(1, 8);
  // Fill, drain, then write again past the wrap boundary.
  writer.write([Float32Array.from([1, 2, 3, 4, 5, 6])], 6);
  const drain = new Float32Array(6);
  reader.read([drain], 6);
  assert.deepEqual(Array.from(drain), [1, 2, 3, 4, 5, 6]);

  writer.write([Float32Array.from([7, 8, 9, 10])], 4);
  const out = new Float32Array(4);
  reader.read([out], 4);
  assert.deepEqual(Array.from(out), [7, 8, 9, 10]);
});

test("an underrun zero-fills the shortfall rather than returning garbage", () => {
  const { writer, reader } = makePair(1, 8);
  writer.write([Float32Array.from([1, 2, 3])], 3);
  const out = new Float32Array(6).fill(-1);
  const read = reader.read([out], 6);
  assert.equal(read, 3, "should report only the real frames read");
  assert.deepEqual(Array.from(out), [1, 2, 3, 0, 0, 0]);
});

test("an overrun (writing more than free space) drops the tail rather than corrupting the ring", () => {
  const { writer, reader } = makePair(1, 4);
  const written = writer.write([Float32Array.from([1, 2, 3, 4, 5, 6])], 6);
  assert.equal(written, 4, "capacity is 4, so only 4 frames should have been accepted");
  const out = new Float32Array(4);
  reader.read([out], 4);
  assert.deepEqual(Array.from(out), [1, 2, 3, 4]);
});

test("framesAvailable/framesFree track each other", () => {
  const { writer, reader } = makePair(1, 8);
  assert.equal(writer.framesFree, 8);
  writer.write([Float32Array.from([1, 2, 3])], 3);
  assert.equal(writer.framesFree, 5);
  assert.equal(reader.framesAvailable, 3);
  reader.read([new Float32Array(3)], 3);
  assert.equal(reader.framesAvailable, 0);
  assert.equal(writer.framesFree, 8);
});
