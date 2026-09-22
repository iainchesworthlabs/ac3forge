// Verifies the fMP4 box walker against a real fixture: apps/wasm/assets/demo.ec3
// remuxed with `ffmpeg -c copy -frag_duration 500000 -movflags default_base_moof`
// (js/tests/fixtures/demo.fmp4, regenerate with that exact command if this ever
// needs updating). No browser needed - this is pure box-offset arithmetic.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { parseInitSegment, extractFragments } from "../dist/fmp4.js";

const fixturePath = fileURLToPath(new URL("./fixtures/demo.fmp4", import.meta.url));

test("parseInitSegment reads the real track id and 48kHz timescale", async () => {
  const buffer = new Uint8Array(await readFile(fixturePath));
  const track = parseInitSegment(buffer);
  assert.ok(track, "expected a track to be found");
  assert.equal(track.trackId, 1);
  assert.equal(track.timescale, 48000);
});

test("parseInitSegment returns null when there is no moov", () => {
  const buffer = new Uint8Array([0, 0, 0, 8, 0x6d, 0x64, 0x61, 0x74]); // a bare, empty 'mdat'
  assert.equal(parseInitSegment(buffer), null);
});

test("extractFragments finds every moof/trun and slices real E-AC-3 syncframes out of mdat", async () => {
  const buffer = new Uint8Array(await readFile(fixturePath));
  const fragments = extractFragments(buffer);

  // The fixture was cut into three ~0.5s fragments (see this file's header).
  assert.equal(fragments.length, 3);

  let totalSamples = 0;
  for (const fragment of fragments) {
    assert.equal(fragment.trackId, 1);
    assert.ok(fragment.samples.length > 0, "expected at least one sample per fragment");
    let previousDecodeTime = fragment.baseMediaDecodeTime - 1;
    for (const sample of fragment.samples) {
      // 0x0B77 is AC-3/E-AC-3's own syncword (A/52 §5.4.1) - landing on it for
      // EVERY extracted sample is the strongest possible end-to-end check
      // that this module's offset arithmetic (base-data-offset,
      // default-base-is-moof, per-sample size accumulation) is correct: a
      // single off-by-one anywhere upstream would desync every sample after
      // it and this assertion would fail immediately.
      assert.equal(sample.bytes[0], 0x0b, "sample did not start on an AC-3/E-AC-3 syncword");
      assert.equal(sample.bytes[1], 0x77, "sample did not start on an AC-3/E-AC-3 syncword");
      assert.ok(sample.bytes.length > 0);
      assert.ok(sample.decodeTime >= previousDecodeTime, "decodeTime must be non-decreasing");
      previousDecodeTime = sample.decodeTime;
      totalSamples++;
    }
  }
  assert.ok(totalSamples > 0);
});
