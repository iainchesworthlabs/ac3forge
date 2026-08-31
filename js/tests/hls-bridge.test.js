// Exercises installMediaSourceShim's contract against a minimal fake
// `MediaSource` global - no real hls.js, no browser, no live HLS stream (see
// js/README.md's "what's verified" section for that real gap). This proves
// the shim mechanics: unmatched mimetypes pass through untouched, matched
// ones get a fake buffer whose appendBuffer/updateend timing and buffered
// reporting behave the way hls.js's BufferController expects.

import { test } from "node:test";
import assert from "node:assert/strict";
import { installMediaSourceShim } from "../dist/hls-bridge.js";

class RealSourceBufferStub {}

function installFakeMediaSourceGlobal() {
  class FakeRealMediaSource {
    static isTypeSupported(type) {
      return type === "audio/mp4;codecs=\"mp4a.40.2\"";
    }
    addSourceBuffer(type) {
      if (type === "audio/mp4;codecs=\"mp4a.40.2\"") return new RealSourceBufferStub();
      throw new DOMException("not supported", "NotSupportedError");
    }
  }
  const previous = globalThis.MediaSource;
  globalThis.MediaSource = FakeRealMediaSource;
  return () => {
    globalThis.MediaSource = previous;
  };
}

test("isTypeSupported/addSourceBuffer delegate untouched mimetypes to the real MediaSource", () => {
  const restoreGlobal = installFakeMediaSourceGlobal();
  try {
    const uninstall = installMediaSourceShim({ onSegment: () => {} });
    try {
      assert.equal(MediaSource.isTypeSupported('audio/mp4;codecs="mp4a.40.2"'), true);
      assert.equal(MediaSource.isTypeSupported('audio/mp4;codecs="unknown"'), false);
      const ms = new MediaSource();
      assert.ok(ms.addSourceBuffer('audio/mp4;codecs="mp4a.40.2"') instanceof RealSourceBufferStub);
    } finally {
      uninstall();
    }
  } finally {
    restoreGlobal();
  }
});

test("addSourceBuffer for a matched mimetype returns a fake buffer and isTypeSupported reports it as playable", () => {
  const restoreGlobal = installFakeMediaSourceGlobal();
  try {
    const uninstall = installMediaSourceShim({ onSegment: () => {} });
    try {
      // This is the exact check hls.js's own codec-support gate performs
      // before it ever tries addSourceBuffer - and the one the real
      // MediaSource would fail for ec-3, causing hls.js to drop the audio
      // track entirely (confirmed by reading buffer-controller.ts). The
      // shim has to answer "yes" here for the rest of the bridge to ever
      // run at all.
      assert.equal(MediaSource.isTypeSupported('audio/mp4;codecs="ec-3"'), true);

      const ms = new MediaSource();
      const fake = ms.addSourceBuffer('audio/mp4;codecs="ec-3"');
      assert.ok(!(fake instanceof RealSourceBufferStub));
      assert.equal(fake.updating, false);
    } finally {
      uninstall();
    }
  } finally {
    restoreGlobal();
  }
});

test("appendBuffer captures the bytes, updates asynchronously, and reports a growing buffered range", async () => {
  const restoreGlobal = installFakeMediaSourceGlobal();
  try {
    const segments = [];
    const uninstall = installMediaSourceShim({
      onSegment: (mimeType, data, sink) => {
        segments.push({ mimeType, length: data.length });
        sink.markBuffered(0, 2);
      },
    });
    try {
      const ms = new MediaSource();
      const fake = ms.addSourceBuffer('audio/mp4;codecs="ec-3"');

      assert.equal(fake.updating, false);
      const updateEnded = new Promise((resolve) => fake.addEventListener("updateend", resolve));
      fake.appendBuffer(new Uint8Array([1, 2, 3]));
      assert.equal(fake.updating, true, "updating must flip synchronously, before the microtask runs");

      await updateEnded;
      assert.equal(fake.updating, false);
      assert.equal(segments.length, 1);
      assert.equal(segments[0].mimeType, 'audio/mp4;codecs="ec-3"');
      assert.equal(segments[0].length, 3);
      assert.equal(fake.buffered.length, 1);
      assert.equal(fake.buffered.start(0), 0);
      assert.equal(fake.buffered.end(0), 2);
    } finally {
      uninstall();
    }
  } finally {
    restoreGlobal();
  }
});

test("appendBuffer while already updating throws, matching real SourceBuffer semantics", () => {
  const restoreGlobal = installFakeMediaSourceGlobal();
  try {
    const uninstall = installMediaSourceShim({ onSegment: () => {} });
    try {
      const ms = new MediaSource();
      const fake = ms.addSourceBuffer('audio/mp4;codecs="ec-3"');
      fake.appendBuffer(new Uint8Array([1]));
      assert.throws(() => fake.appendBuffer(new Uint8Array([2])), DOMException);
    } finally {
      uninstall();
    }
  } finally {
    restoreGlobal();
  }
});
