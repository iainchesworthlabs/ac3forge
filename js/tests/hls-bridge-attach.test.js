// attachHlsAudioBridge, syncTo, and the parts of the fake SourceBuffer
// hls-bridge.test.js does not reach (remove, abort, a throwing segment
// handler). The bridge is driven with the real fMP4 fixture from
// fmp4.test.js, split where hls.js would split it - the init segment (ftyp +
// moov) appended first, then the media segments - and a recording stand-in for
// Ac3ForgeDecoderNode, so the assertions are about which access units reach
// the decoder and what buffered range hls.js is told about.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { attachHlsAudioBridge, installMediaSourceShim, syncTo } from "../dist/hls-bridge.js";
import { extractFragments, iterateBoxes, parseInitSegment } from "../dist/fmp4.js";

const fixturePath = fileURLToPath(new URL("./fixtures/demo.fmp4", import.meta.url));
const EC3 = 'audio/mp4;codecs="ec-3"';

function withFakeMediaSource(run) {
  class FakeRealMediaSource {
    static isTypeSupported() {
      return false;
    }
    addSourceBuffer() {
      throw new DOMException("not supported", "NotSupportedError");
    }
  }
  const previous = globalThis.MediaSource;
  globalThis.MediaSource = FakeRealMediaSource;
  return Promise.resolve()
    .then(run)
    .finally(() => {
      globalThis.MediaSource = previous;
    });
}

function append(buffer, data) {
  const done = new Promise((resolve) => buffer.addEventListener("updateend", resolve, { once: true }));
  buffer.appendBuffer(data);
  return done;
}

async function splitFixture() {
  const whole = new Uint8Array(await readFile(fixturePath));
  const firstMoof = [...iterateBoxes(whole)].find((b) => b.type === "moof");
  return { whole, init: whole.subarray(0, firstMoof.boxStart), media: whole.subarray(firstMoof.boxStart) };
}

test("the bridge pushes every media sample to the decoder and reports each fragment's time range", () =>
  withFakeMediaSource(async () => {
    const { whole, init, media } = await splitFixture();
    const track = parseInitSegment(whole);
    const fragments = extractFragments(whole);
    const expectedUnits = fragments.flatMap((f) => f.samples).filter((s) => s.bytes.length > 0);

    const pushed = [];
    const uninstall = attachHlsAudioBridge({ decoderNode: { pushAccessUnit: (unit) => pushed.push(unit) } });
    try {
      const buffer = new MediaSource().addSourceBuffer(EC3);
      await append(buffer, init);
      assert.equal(pushed.length, 0, "an init segment carries no samples");
      assert.equal(buffer.buffered.length, 0);

      await append(buffer, media);
      assert.equal(pushed.length, expectedUnits.length);
      assert.deepEqual(Array.from(pushed[0]), Array.from(expectedUnits[0].bytes));
      assert.equal(buffer.buffered.length, fragments.length);
      const last = fragments.at(-1).samples.at(-1);
      assert.equal(buffer.buffered.start(0), fragments[0].baseMediaDecodeTime / track.timescale);
      assert.equal(buffer.buffered.end(fragments.length - 1), (last.decodeTime + last.duration) / track.timescale);
    } finally {
      uninstall();
    }
  }));

test("media appended before any init segment is still decoded, but no range is reported", () =>
  withFakeMediaSource(async () => {
    const { media } = await splitFixture();
    const pushed = [];
    const uninstall = attachHlsAudioBridge({
      decoderNode: { pushAccessUnit: (unit) => pushed.push(unit) },
      mimeTypePattern: /ec-3/,
    });
    try {
      const buffer = new MediaSource().addSourceBuffer(EC3);
      await append(buffer, media);
      assert.ok(pushed.length > 0);
      assert.equal(buffer.buffered.length, 0, "without a timescale there is no time to report");
    } finally {
      uninstall();
    }
  }));

test("a segment handler that throws surfaces as an error event and still ends the update", () =>
  withFakeMediaSource(async () => {
    const uninstall = installMediaSourceShim({
      onSegment: () => {
        throw new Error("unparseable");
      },
    });
    try {
      const buffer = new MediaSource().addSourceBuffer(EC3);
      const events = [];
      for (const type of ["update", "error", "updateend"]) buffer.addEventListener(type, () => events.push(type));
      await append(buffer, new Uint8Array(1));
      assert.deepEqual(events, ["error", "updateend"]);
      assert.equal(buffer.updating, false);
    } finally {
      uninstall();
    }
  }));

test("appendBuffer accepts a bare ArrayBuffer", () =>
  withFakeMediaSource(async () => {
    let received = null;
    const uninstall = installMediaSourceShim({ onSegment: (_type, data) => (received = data) });
    try {
      const buffer = new MediaSource().addSourceBuffer(EC3);
      await append(buffer, Uint8Array.of(4, 5, 6).buffer);
      assert.deepEqual(Array.from(received), [4, 5, 6]);
    } finally {
      uninstall();
    }
  }));

test("remove drops only the ranges inside the removed span, asynchronously", () =>
  withFakeMediaSource(async () => {
    let calls = 0;
    const uninstall = installMediaSourceShim({
      onSegment: (_type, _data, sink) => {
        sink.markBuffered(calls * 2, calls * 2 + 2);
        calls++;
      },
    });
    try {
      const buffer = new MediaSource().addSourceBuffer(EC3);
      for (let i = 0; i < 3; i++) await append(buffer, new Uint8Array(1)); // [0,2) [2,4) [4,6)
      const done = new Promise((resolve) => buffer.addEventListener("updateend", resolve, { once: true }));
      buffer.remove(1.5, 4);
      assert.equal(buffer.updating, true);
      await done;
      assert.equal(buffer.updating, false);
      const ranges = Array.from({ length: buffer.buffered.length }, (_, i) => [buffer.buffered.start(i), buffer.buffered.end(i)]);
      assert.deepEqual(ranges, [[4, 6]], "[0,2) overlaps the span and goes too; [4,6) starts at its end and stays");
    } finally {
      uninstall();
    }
  }));

test("abort clears the updating flag so the next append is accepted", () =>
  withFakeMediaSource(() => {
    const uninstall = installMediaSourceShim({ onSegment: () => {} });
    try {
      const buffer = new MediaSource().addSourceBuffer(EC3);
      buffer.appendBuffer(new Uint8Array(1));
      buffer.abort();
      assert.equal(buffer.updating, false);
      assert.doesNotThrow(() => buffer.appendBuffer(new Uint8Array(1)));
    } finally {
      uninstall();
    }
  }));

test("uninstall restores the real MediaSource behaviour", () =>
  withFakeMediaSource(() => {
    const uninstall = installMediaSourceShim({ onSegment: () => {} });
    assert.equal(MediaSource.isTypeSupported(EC3), true);
    uninstall();
    assert.equal(MediaSource.isTypeSupported(EC3), false);
    assert.throws(() => new MediaSource().addSourceBuffer(EC3), DOMException);
  }));

test("syncTo reports the media time on seeked and play, and stops when told to", () => {
  const element = new EventTarget();
  element.currentTime = 12.5;
  const seen = [];
  const stop = syncTo(element, (t) => seen.push(t));
  element.dispatchEvent(new Event("seeked"));
  element.currentTime = 20;
  element.dispatchEvent(new Event("play"));
  element.dispatchEvent(new Event("pause"));
  stop();
  element.dispatchEvent(new Event("seeked"));
  assert.deepEqual(seen, [12.5, 20]);
});
