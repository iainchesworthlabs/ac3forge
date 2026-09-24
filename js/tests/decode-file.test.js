// decodeFile's accumulation logic against a scripted Embind stand-in
// (fake-embind.js). The stand-in's default scan makes one access unit per
// input byte, so `new Uint8Array(n)` is an n-unit stream here. What these pin
// is presentation logic that lives only in TypeScript: concatenation across
// frames, held-back frames, the flush tail, the RMS energy blocks, and how an
// object layer that starts late or pauses keeps its arrays aligned with
// playback time.

import { test } from "node:test";
import assert from "node:assert/strict";
import { decodeFile } from "../dist/decode-file.js";
import { DownmixTarget } from "../dist/types.js";
import { failure, frame, holdBack, makeFakeModule, pcm } from "./fake-embind.js";

const units = (n) => new Uint8Array(n);

test("channels from every frame are concatenated in order, per channel", () => {
  const { module } = makeFakeModule({
    script: [
      frame({ frameSamples: 2, channels: [pcm(1, 2), pcm(-1, -2)], channelLabels: ["L", "R"] }),
      frame({ frameSamples: 2, channels: [pcm(3, 4), pcm(-3, -4)], channelLabels: ["L", "R"] }),
    ],
  });
  const program = decodeFile(module, units(2));
  assert.equal(program.streamKind, "E-AC-3");
  assert.equal(program.sampleRate, 48000);
  assert.equal(program.channelCount, 2);
  assert.deepEqual(program.channelLabels, ["L", "R"]);
  assert.deepEqual(Array.from(program.channels[0]), [1, 2, 3, 4]);
  assert.deepEqual(Array.from(program.channels[1]), [-1, -2, -3, -4]);
  assert.equal(program.durationSeconds, 4 / 48000);
  assert.equal(program.objectCount, 0);
  assert.equal(program.objectFrameCount, 0);
  assert.equal(program.objectStartSeconds, 0);
});

test("only frameSamples of each view are kept, and a missing view becomes silence", () => {
  const { module } = makeFakeModule({
    script: [frame({ frameSamples: 2, channels: [pcm(1, 2, 99, 99), null] })],
  });
  const program = decodeFile(module, units(1));
  assert.deepEqual(Array.from(program.channels[0]), [1, 2]);
  assert.deepEqual(Array.from(program.channels[1]), [0, 0]);
});

test("a held-back frame contributes nothing, and the flush tail is appended", () => {
  const { module } = makeFakeModule({
    script: [holdBack(), frame({ frameSamples: 2, channels: [pcm(1, 2)] })],
    flushEntries: [
      { holdBack: false, channelCount: 1, frameSamples: 2, flushIndex: 0 },
      // Neither of these belongs: one is still held back, one has the wrong width.
      { holdBack: true, channelCount: 1, frameSamples: 2, flushIndex: 1 },
      { holdBack: false, channelCount: 2, frameSamples: 2, flushIndex: 2 },
    ],
    flushed: [[pcm(5, 6)], [pcm(7, 7)], [pcm(8, 8), pcm(8, 8)]],
  });
  const program = decodeFile(module, units(2));
  assert.deepEqual(Array.from(program.channels[0]), [1, 2, 5, 6]);
});

test("the sample rate follows the decoded frames rather than the scan", () => {
  const { module } = makeFakeModule({
    scan: { ok: true, kind: "AC-3", sampleRate: 48000, accessUnits: [{ offset: 0, length: 1 }] },
    script: [frame({ sampleRate: 32000, frameSamples: 2, channels: [pcm(0, 0)] })],
  });
  const program = decodeFile(module, units(1));
  assert.equal(program.sampleRate, 32000);
  assert.equal(program.durationSeconds, 2 / 32000);
});

test("the fold is accumulated alongside the channels when one was requested", () => {
  const { module, log } = makeFakeModule({
    script: [
      frame({ frameSamples: 1, channels: [pcm(1), pcm(1), pcm(1)], fold: [pcm(0.5), pcm(-0.5)] }),
      frame({ frameSamples: 1, channels: [pcm(2), pcm(2), pcm(2)], fold: [pcm(0.25), pcm(-0.25)] }),
    ],
  });
  const program = decodeFile(module, units(2), { fold: { target: DownmixTarget.LoRo } });
  assert.equal(log.constructed[0].foldTarget, DownmixTarget.LoRo);
  assert.equal(program.fold.length, 2);
  assert.deepEqual(Array.from(program.fold[0]), [0.5, 0.25]);
  assert.deepEqual(Array.from(program.fold[1]), [-0.5, -0.25]);
});

test("energy is the RMS of each 1024-sample block, the last block over what remains", () => {
  const samples = new Float32Array(1024 + 4);
  samples.fill(0.5, 0, 1024); // RMS 0.5
  samples.set([1, -1, 1, -1], 1024); // RMS 1 over the 4-sample tail
  const { module } = makeFakeModule({ script: [frame({ frameSamples: samples.length, channels: [samples] })] });
  const program = decodeFile(module, units(1));
  assert.equal(program.energyBlockSize, 1024);
  assert.deepEqual(Array.from(program.energy[0]), [0.5, 1]);
});

test("an object layer that starts late records when it started and every frame after", () => {
  const p0 = [0.1, 0.2, 0.3, 0, 0, 0, 0];
  const p1 = [0.4, 0.5, 0.6, -3, 0, 0, 0];
  const { module } = makeFakeModule({
    script: [
      frame({ frameSamples: 2, channels: [pcm(0, 0)] }),
      frame({ frameSamples: 2, channels: [pcm(0, 0)], objects: [pcm(1, 1)], objectLabels: ["obj"], positions: [pcm(...p0)] }),
      frame({ frameSamples: 2, channels: [pcm(0, 0)], objects: [pcm(2, 2)], objectLabels: ["obj"], positions: [pcm(...p1)] }),
    ],
  });
  const program = decodeFile(module, units(3));
  assert.equal(program.objectCount, 1);
  assert.deepEqual(program.objectLabels, ["obj"]);
  assert.equal(program.objectFrameSize, 2);
  assert.equal(program.objectFrameCount, 2);
  assert.equal(program.objectStartSeconds, 2 / 48000, "objects began on the second 2-sample frame");
  assert.deepEqual(Array.from(program.objectAudio[0]), [1, 1, 2, 2]);
  assert.deepEqual(Array.from(program.objectPositions[0]), Array.from(pcm(...p0, ...p1)));
});

test("a gap in the object layer freezes each object's position and pads its audio with silence", () => {
  const p0 = [0.1, 0.2, 0.3, 0, 0, 0, 0];
  const objectFrame = (audio) =>
    frame({ frameSamples: 2, channels: [pcm(0, 0)], objects: [audio], positions: [pcm(...p0)] });
  const { module } = makeFakeModule({
    script: [objectFrame(pcm(1, 1)), frame({ frameSamples: 2, channels: [pcm(0, 0)] }), objectFrame(pcm(3, 3))],
  });
  const program = decodeFile(module, units(3));
  assert.equal(program.objectStartSeconds, 0, "objects present from the first frame start at zero");
  assert.equal(program.objectFrameCount, 3, "the gap still counts as a frame, so arrays stay aligned");
  assert.deepEqual(Array.from(program.objectAudio[0]), [1, 1, 0, 0, 3, 3]);
  assert.deepEqual(Array.from(program.objectPositions[0]), Array.from(pcm(...p0, ...p0, ...p0)));
});

test("an object with no position data this frame keeps its audio but adds no position", () => {
  const { module } = makeFakeModule({
    script: [frame({ frameSamples: 1, channels: [pcm(0)], objects: [pcm(1)], positions: [null] })],
  });
  const program = decodeFile(module, units(1));
  assert.deepEqual(Array.from(program.objectAudio[0]), [1]);
  assert.equal(program.objectPositions[0].length, 0);
});

test("a stream that scans but yields no frames decodes to an empty program", () => {
  const { module } = makeFakeModule({ script: [holdBack()] });
  const program = decodeFile(module, units(1));
  assert.equal(program.channels.length, 0);
  assert.equal(program.channelCount, 0);
  assert.equal(program.durationSeconds, 0);
});

test("a failed scan throws its error without constructing a decoder", () => {
  const { module, log } = makeFakeModule({ scan: { ok: false, error: "not an AC-3 stream" } });
  assert.throws(() => decodeFile(module, units(1)), /not an AC-3 stream/);
  assert.equal(log.constructed.length, 0);
});

test("a failed decode throws its error and still releases the native decoder", () => {
  const { module, log } = makeFakeModule({ script: [frame({ channels: [pcm(0)] }), failure("bad frame")] });
  assert.throws(() => decodeFile(module, units(2)), /bad frame/);
  assert.equal(log.deleted, 1);
});

test("a successful decode releases the native decoder too", () => {
  const { module, log } = makeFakeModule({ script: [frame({ channels: [pcm(0)] })] });
  decodeFile(module, units(1));
  assert.equal(log.deleted, 1);
});
