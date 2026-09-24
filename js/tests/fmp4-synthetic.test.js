// fmp4.ts against hand-built boxes, for the ISOBMFF shapes the ffmpeg fixture
// in fmp4.test.js does not happen to contain: 64-bit and to-end box sizes,
// truncated tails, version-1 tkhd/mdhd/tfdt, the optional tfhd/trun fields
// in every combination this walker skips or reads, an explicit
// base-data-offset, handler-type selection between tracks, and the
// structurally incomplete boxes it must pass over rather than misread.

import { test } from "node:test";
import assert from "node:assert/strict";
import { extractFragments, iterateBoxes, parseInitSegment } from "../dist/fmp4.js";

const u32 = (v) => [(v >>> 24) & 0xff, (v >>> 16) & 0xff, (v >>> 8) & 0xff, v & 0xff];
const u64 = (v) => [...u32(Math.floor(v / 2 ** 32)), ...u32(v >>> 0)];
const fourcc = (s) => Array.from(s, (c) => c.charCodeAt(0));
const box = (type, ...content) => {
  const body = content.flat();
  return [...u32(8 + body.length), ...fourcc(type), ...body];
};
const fullBox = (type, version, flags, ...content) => box(type, [version, ...u32(flags).slice(1)], ...content);
const bytes = (...parts) => Uint8Array.from(parts.flat());

function tkhd(version, trackId) {
  const times = version === 1 ? [...u64(0), ...u64(0)] : [...u32(0), ...u32(0)];
  return fullBox("tkhd", version, 0, times, u32(trackId), new Array(60).fill(0));
}
function mdhd(version, timescale) {
  const times = version === 1 ? [...u64(0), ...u64(0)] : [...u32(0), ...u32(0)];
  return fullBox("mdhd", version, 0, times, u32(timescale), u32(0), [0, 0, 0, 0]);
}
const hdlr = (type) => fullBox("hdlr", 0, 0, u32(0), fourcc(type), new Array(12).fill(0), [0]);
const trak = (...children) => box("trak", ...children);
const mdia = (...children) => box("mdia", ...children);

test("iterateBoxes reads a 64-bit largesize box and a size-0 box that runs to the end", () => {
  const large = [...u32(1), ...fourcc("free"), ...u64(20), 1, 2, 3, 4];
  const toEnd = [...u32(0), ...fourcc("mdat"), 9, 9, 9];
  const boxes = [...iterateBoxes(bytes(large, toEnd))];
  assert.deepEqual(
    boxes.map((b) => [b.type, b.boxStart, b.start, b.end]),
    [
      ["free", 0, 16, 20],
      ["mdat", 20, 28, 31],
    ],
  );
});

test("iterateBoxes stops at a truncated or undersized box instead of reading past it", () => {
  const ok = box("free", [1]);
  assert.deepEqual([...iterateBoxes(bytes(ok, u32(100), fourcc("moof")))].map((b) => b.type), ["free"], "size past the end");
  assert.deepEqual([...iterateBoxes(bytes(ok, u32(4), fourcc("moof")))].map((b) => b.type), ["free"], "size under its header");
  assert.deepEqual(
    [...iterateBoxes(bytes(ok, u32(1), fourcc("free"), 0, 0))].map((b) => b.type),
    ["free"],
    "a largesize header cut short",
  );
  assert.deepEqual([...iterateBoxes(bytes(1, 2, 3))], [], "fewer bytes than one header");
});

test("parseInitSegment prefers the sound track, reading version-1 tkhd and mdhd", () => {
  const init = bytes(
    box("ftyp", fourcc("iso6")),
    box(
      "moov",
      trak(tkhd(0, 1), mdia(mdhd(0, 90000), hdlr("vide"))),
      trak(tkhd(1, 2), mdia(mdhd(1, 44100), hdlr("soun"))),
    ),
  );
  assert.deepEqual(parseInitSegment(init), { trackId: 2, timescale: 44100 });
});

test("parseInitSegment falls back to the first complete track when none is tagged soun", () => {
  const init = bytes(
    box(
      "moov",
      trak(mdia(mdhd(0, 1000))), // no tkhd
      trak(tkhd(0, 5), box("edts")), // no mdia
      trak(tkhd(0, 6), mdia(hdlr("soun"))), // no mdhd
      trak(tkhd(0, 7), mdia(mdhd(0, 48000))), // no hdlr at all
      trak(tkhd(0, 8), mdia(mdhd(0, 32000), hdlr("vide"))),
    ),
  );
  assert.deepEqual(parseInitSegment(init), { trackId: 7, timescale: 48000 });
});

test("parseInitSegment returns null without a moov, or when no track is complete", () => {
  assert.equal(parseInitSegment(bytes(box("ftyp", fourcc("iso6")))), null);
  assert.equal(parseInitSegment(bytes(box("moov", trak(tkhd(0, 1))))), null);
});

// A moof whose single traf carries the given tfhd/tfdt/trun, followed by an
// mdat holding `payload`. `trunFor(moofSize)` builds the trun once the moof's
// own size (needed for a data_offset into the mdat) is known.
function segment({ tfhdFlags, tfhdFields, tfdt, trunFor, payload }) {
  const build = (moofSize) =>
    box("moof", box("traf", fullBox("tfhd", 0, tfhdFlags, tfhdFields), tfdt ?? [], trunFor(moofSize)));
  const moofSize = build(0).length;
  return bytes(build(moofSize), box("mdat", payload));
}

test("extractFragments reads per-sample durations and sizes, skipping every optional field it does not use", () => {
  // tfhd: sample-description-index and default-sample-flags present (both skipped).
  // trun: data_offset, first_sample_flags, and per-sample duration/size/flags/cto.
  const trunFlags = 0x001 | 0x004 | 0x100 | 0x200 | 0x400 | 0x800;
  const input = segment({
    tfhdFlags: 0x02 | 0x20 | 0x020000,
    tfhdFields: [...u32(3), ...u32(1), ...u32(0)],
    tfdt: fullBox("tfdt", 1, 0, u64(2 ** 33)),
    trunFor: (moofSize) =>
      fullBox("trun", 0, trunFlags, u32(2), u32(moofSize + 8), u32(0), [...u32(1536), ...u32(2), ...u32(0), ...u32(0)], [
        ...u32(768),
        ...u32(3),
        ...u32(0),
        ...u32(0),
      ]),
    payload: [0xaa, 0xbb, 0xcc, 0xdd, 0xee],
  });
  const [fragment] = extractFragments(input);
  assert.equal(fragment.trackId, 3);
  assert.equal(fragment.baseMediaDecodeTime, 2 ** 33, "a version-1 tfdt's 64-bit time");
  assert.deepEqual(
    fragment.samples.map((s) => [Array.from(s.bytes), s.duration, s.decodeTime]),
    [
      [[0xaa, 0xbb], 1536, 2 ** 33],
      [[0xcc, 0xdd, 0xee], 768, 2 ** 33 + 1536],
    ],
  );
});

test("extractFragments falls back to tfhd defaults, and to time zero without a tfdt", () => {
  const input = segment({
    tfhdFlags: 0x08 | 0x10,
    tfhdFields: [...u32(1), ...u32(1536), ...u32(2)],
    trunFor: (moofSize) => fullBox("trun", 0, 0x001, u32(2), u32(moofSize + 8)),
    payload: [1, 2, 3, 4],
  });
  const [fragment] = extractFragments(input);
  assert.equal(fragment.baseMediaDecodeTime, 0);
  assert.deepEqual(
    fragment.samples.map((s) => [Array.from(s.bytes), s.duration, s.decodeTime]),
    [
      [[1, 2], 1536, 0],
      [[3, 4], 1536, 1536],
    ],
  );
});

test("an explicit base-data-offset replaces the moof as the base of the trun's data offset", () => {
  // The base points at the mdat's payload, so a data_offset of 1 skips its first byte.
  const input = segment({
    tfhdFlags: 0x01 | 0x10,
    tfhdFields: [...u32(1), ...u64(0), ...u32(2)],
    tfdt: fullBox("tfdt", 0, 0, u32(90)),
    trunFor: () => fullBox("trun", 0, 0x001, u32(1), u32(1)),
    payload: [7, 8, 9],
  });
  // Patch the base offset now that the layout is fixed: moof size + mdat header.
  const moofSize = new DataView(input.buffer).getUint32(0);
  const tfhdBaseOffset = 8 + 8 + 12 + 4; // moof hdr, traf hdr, tfhd hdr+flags, track_ID
  new DataView(input.buffer).setUint32(tfhdBaseOffset + 4, moofSize + 8);
  const [fragment] = extractFragments(input);
  assert.equal(fragment.baseMediaDecodeTime, 90);
  assert.deepEqual(Array.from(fragment.samples[0].bytes), [8, 9]);
});

test("moofs without a traf, a tfhd or any samples yield no fragment", () => {
  const empty = bytes(
    box("moof", box("mfhd", u32(0))),
    box("moof", box("traf", fullBox("tfdt", 0, 0, u32(0)))),
    box("moof", box("traf", fullBox("tfhd", 0, 0, u32(1)), fullBox("trun", 0, 0, u32(0)))),
    box("mdat", [1]),
  );
  assert.deepEqual(extractFragments(empty), []);
});
