// A minimal ISOBMFF (fragmented MP4 / CMAF) box walker: enough to pull raw
// AC-3/E-AC-3 access units out of the fragments an HLS/DASH demuxer (hls.js's
// own remuxer always produces fMP4, even from MPEG-TS source segments)
// hands to a SourceBuffer. Not a general MP4 parser - it reads exactly the
// boxes needed for that (`moov`/`trak`/`mdia`/`mdhd` for an init segment's
// track id and timescale, `moof`/`traf`/`tfhd`/`tfdt`/`trun` plus the sample
// bytes for each media segment) and nothing else. See hls-bridge.ts for how
// this is wired to a real hls.js instance.
//
// Verified against a real fixture: `ffmpeg -c copy -frag_duration 2000000
// -movflags default_base_moof` remuxing apps/wasm/assets/demo.ec3 into
// fragmented MP4 (js/tests/fixtures/, generated once and committed - see
// js/tests/fmp4.test.js) - hex-inspected by hand to confirm this file's
// field offsets/flag bits against that real muxer's output before writing
// the general-purpose walk below.

export interface Box {
  readonly type: string;
  /** Absolute offset of this box's own size field - i.e. including its header. */
  readonly boxStart: number;
  /** Byte range of this box's content, i.e. after its header. */
  readonly start: number;
  readonly end: number;
}

const BOX_HEADER_MIN_LENGTH = 8;

/**
 * Walks the immediate children of `buffer[rangeStart, rangeEnd)` - not
 * recursive, matching how every caller here already knows which boxes it
 * wants to recurse into and which to skip (recursing into every container
 * uniformly would mean re-deciding that at every level for no benefit).
 */
export function* iterateBoxes(buffer: Uint8Array, rangeStart = 0, rangeEnd = buffer.length): Generator<Box> {
  const view = new DataView(buffer.buffer, buffer.byteOffset, buffer.byteLength);
  let offset = rangeStart;
  while (offset + BOX_HEADER_MIN_LENGTH <= rangeEnd) {
    let size = view.getUint32(offset);
    let headerLength = 8;
    const type = readFourCc(buffer, offset + 4);
    if (size === 1) {
      // 64-bit "largesize" - present but rare for anything this small.
      if (offset + 16 > rangeEnd) break;
      const high = view.getUint32(offset + 8);
      const low = view.getUint32(offset + 12);
      size = high * 2 ** 32 + low;
      headerLength = 16;
    } else if (size === 0) {
      // "Box extends to end of file/buffer" - ISO/IEC 14496-12 §4.2.
      size = rangeEnd - offset;
    }
    if (size < headerLength || offset + size > rangeEnd) {
      // A truncated/malformed tail - stop rather than read past it.
      break;
    }
    yield { type, boxStart: offset, start: offset + headerLength, end: offset + size };
    offset += size;
  }
}

function readFourCc(buffer: Uint8Array, offset: number): string {
  return String.fromCharCode(buffer[offset]!, buffer[offset + 1]!, buffer[offset + 2]!, buffer[offset + 3]!);
}

function findChild(buffer: Uint8Array, box: Box, type: string): Box | null {
  for (const child of iterateBoxes(buffer, box.start, box.end)) {
    if (child.type === type) return child;
  }
  return null;
}

function* findChildren(buffer: Uint8Array, box: Box, type: string): Generator<Box> {
  for (const child of iterateBoxes(buffer, box.start, box.end)) {
    if (child.type === type) yield child;
  }
}

export interface TrackInfo {
  trackId: number;
  /** Ticks per second for every duration/timestamp this track's fragments carry. */
  timescale: number;
}

/**
 * Reads a fragmented-MP4 *init* segment (`ftyp` + `moov`, no samples - a real
 * CMAF init segment's `stbl` tables are all empty) for the one track whose
 * `hdlr` says `soun`, or the first `trak` if none is tagged that way (an
 * audio-only init segment, which is what hls.js produces per audio
 * rendition, never carries a handler type worth doubting).
 */
export function parseInitSegment(buffer: Uint8Array): TrackInfo | null {
  const moov = [...iterateBoxes(buffer)].find((box) => box.type === "moov");
  if (!moov) return null;

  let fallback: TrackInfo | null = null;
  for (const trak of findChildren(buffer, moov, "trak")) {
    const tkhd = findChild(buffer, trak, "tkhd");
    const mdia = findChild(buffer, trak, "mdia");
    if (!tkhd || !mdia) continue;
    const mdhd = findChild(buffer, mdia, "mdhd");
    if (!mdhd) continue;

    const trackId = readTkhdTrackId(buffer, tkhd);
    const timescale = readMdhdTimescale(buffer, mdhd);
    const info: TrackInfo = { trackId, timescale };

    const hdlr = findChild(buffer, mdia, "hdlr");
    if (hdlr && readHdlrType(buffer, hdlr) === "soun") {
      return info;
    }
    fallback ??= info;
  }
  return fallback;
}

function readTkhdTrackId(buffer: Uint8Array, tkhd: Box): number {
  const view = new DataView(buffer.buffer, buffer.byteOffset, buffer.byteLength);
  const version = buffer[tkhd.start]!;
  // version(1)+flags(3)+creation+modification, each 4 bytes (v0) or 8 (v1), then track_ID(4).
  const timeFieldLength = version === 1 ? 8 : 4;
  const trackIdOffset = tkhd.start + 4 + timeFieldLength * 2;
  return view.getUint32(trackIdOffset);
}

function readMdhdTimescale(buffer: Uint8Array, mdhd: Box): number {
  const view = new DataView(buffer.buffer, buffer.byteOffset, buffer.byteLength);
  const version = buffer[mdhd.start]!;
  const timeFieldLength = version === 1 ? 8 : 4;
  // version(1)+flags(3)+creation+modification, then timescale(4).
  const timescaleOffset = mdhd.start + 4 + timeFieldLength * 2;
  return view.getUint32(timescaleOffset);
}

function readHdlrType(buffer: Uint8Array, hdlr: Box): string {
  // version(1)+flags(3)+pre_defined(4), then the four-byte handler type.
  return readFourCc(buffer, hdlr.start + 8);
}

export interface Sample {
  /** A view into the original buffer - copy it out before the buffer is reused/detached. */
  bytes: Uint8Array;
  /** In the track's own timescale (TrackInfo.timescale), not a sample count. */
  duration: number;
  /** This sample's own decode time, in the track's own timescale - baseMediaDecodeTime plus every earlier sample's duration. */
  decodeTime: number;
}

export interface Fragment {
  trackId: number;
  /** §8.8.12's baseMediaDecodeTime, in the track's own timescale. */
  baseMediaDecodeTime: number;
  samples: Sample[];
}

// tfhd flags (ISO/IEC 14496-12 §8.8.7).
const TFHD_BASE_DATA_OFFSET_PRESENT = 0x000001;
const TFHD_SAMPLE_DESCRIPTION_INDEX_PRESENT = 0x000002;
const TFHD_DEFAULT_SAMPLE_DURATION_PRESENT = 0x000008;
const TFHD_DEFAULT_SAMPLE_SIZE_PRESENT = 0x000010;
const TFHD_DEFAULT_SAMPLE_FLAGS_PRESENT = 0x000020;
const TFHD_DEFAULT_BASE_IS_MOOF = 0x020000;

// trun flags (ISO/IEC 14496-12 §8.8.8).
const TRUN_DATA_OFFSET_PRESENT = 0x000001;
const TRUN_FIRST_SAMPLE_FLAGS_PRESENT = 0x000004;
const TRUN_SAMPLE_DURATION_PRESENT = 0x000100;
const TRUN_SAMPLE_SIZE_PRESENT = 0x000200;
const TRUN_SAMPLE_FLAGS_PRESENT = 0x000400;
const TRUN_SAMPLE_COMPOSITION_TIME_OFFSET_PRESENT = 0x000800;

interface Tfhd {
  trackId: number;
  baseDataOffset: number | null;
  defaultSampleDuration: number | null;
  defaultSampleSize: number | null;
  defaultBaseIsMoof: boolean;
}

function readTfhd(buffer: Uint8Array, tfhd: Box): Tfhd {
  const view = new DataView(buffer.buffer, buffer.byteOffset, buffer.byteLength);
  const flags = view.getUint32(tfhd.start) & 0x00ffffff;
  let p = tfhd.start + 4;
  const trackId = view.getUint32(p);
  p += 4;
  let baseDataOffset: number | null = null;
  if (flags & TFHD_BASE_DATA_OFFSET_PRESENT) {
    // A 64-bit offset, but every real fragment this bridge targets fits well
    // under 2^53 - Number-precision loss only matters past that.
    const high = view.getUint32(p);
    const low = view.getUint32(p + 4);
    baseDataOffset = high * 2 ** 32 + low;
    p += 8;
  }
  if (flags & TFHD_SAMPLE_DESCRIPTION_INDEX_PRESENT) p += 4;
  let defaultSampleDuration: number | null = null;
  if (flags & TFHD_DEFAULT_SAMPLE_DURATION_PRESENT) {
    defaultSampleDuration = view.getUint32(p);
    p += 4;
  }
  let defaultSampleSize: number | null = null;
  if (flags & TFHD_DEFAULT_SAMPLE_SIZE_PRESENT) {
    defaultSampleSize = view.getUint32(p);
    p += 4;
  }
  // default-sample-flags is read for completeness but unused - we only need
  // sizes/durations to slice out access units, not the sync/redundancy bits.
  return {
    trackId,
    baseDataOffset,
    defaultSampleDuration,
    defaultSampleSize,
    defaultBaseIsMoof: (flags & TFHD_DEFAULT_BASE_IS_MOOF) !== 0,
  };
}

function readTfdtBaseMediaDecodeTime(buffer: Uint8Array, tfdt: Box): number {
  const view = new DataView(buffer.buffer, buffer.byteOffset, buffer.byteLength);
  const version = buffer[tfdt.start]!;
  if (version === 1) {
    const high = view.getUint32(tfdt.start + 4);
    const low = view.getUint32(tfdt.start + 8);
    return high * 2 ** 32 + low;
  }
  return view.getUint32(tfdt.start + 4);
}

/**
 * Extracts every AC-3/E-AC-3 access unit from a media segment (one or more
 * `moof`+`mdat` pairs concatenated, exactly what a `SourceBuffer.
 * appendBuffer()` call receives). Scope: one audio `traf` per `moof` and the
 * two base-offset rules ISO/IEC 14496-12 actually specifies
 * (`base-data-offset-present`, or `default-base-is-moof`) - a `traf` using
 * neither falls back to "offset from the start of this moof", which is the
 * common case in practice but not the full pre-`default-base-is-moof`
 * legacy rule. Multiplexed (audio+video in one `traf`) fragments are out of
 * scope - hls.js demuxes each rendition into its own segment.
 */
export function extractFragments(buffer: Uint8Array): Fragment[] {
  const fragments: Fragment[] = [];
  for (const top of iterateBoxes(buffer)) {
    if (top.type !== "moof") continue;
    const moofBoxStart = top.boxStart;
    const traf = findChild(buffer, top, "traf");
    if (!traf) continue;
    const tfhdBox = findChild(buffer, traf, "tfhd");
    if (!tfhdBox) continue;
    const tfhd = readTfhd(buffer, tfhdBox);
    const tfdtBox = findChild(buffer, traf, "tfdt");
    const baseMediaDecodeTime = tfdtBox ? readTfdtBaseMediaDecodeTime(buffer, tfdtBox) : 0;

    // Two of ISO/IEC 14496-12's base-offset rules: an explicit
    // base-data-offset, or (the common case, and the only other one this
    // module implements - see the function's own doc comment)
    // default-base-is-moof / no flag at all, both of which resolve to the
    // start of this moof box.
    const baseOffset = tfhd.baseDataOffset ?? moofBoxStart;

    const samples: Sample[] = [];
    let dataCursor = baseOffset;
    let decodeTimeCursor = baseMediaDecodeTime;
    for (const trunBox of findChildren(buffer, traf, "trun")) {
      const view = new DataView(buffer.buffer, buffer.byteOffset, buffer.byteLength);
      const flags = view.getUint32(trunBox.start) & 0x00ffffff;
      let p = trunBox.start + 4;
      const sampleCount = view.getUint32(p);
      p += 4;
      if (flags & TRUN_DATA_OFFSET_PRESENT) {
        const dataOffset = view.getInt32(p);
        p += 4;
        dataCursor = baseOffset + dataOffset;
      }
      if (flags & TRUN_FIRST_SAMPLE_FLAGS_PRESENT) p += 4;

      for (let i = 0; i < sampleCount; i++) {
        let duration = tfhd.defaultSampleDuration ?? 0;
        let size = tfhd.defaultSampleSize ?? 0;
        if (flags & TRUN_SAMPLE_DURATION_PRESENT) {
          duration = view.getUint32(p);
          p += 4;
        }
        if (flags & TRUN_SAMPLE_SIZE_PRESENT) {
          size = view.getUint32(p);
          p += 4;
        }
        if (flags & TRUN_SAMPLE_FLAGS_PRESENT) p += 4;
        if (flags & TRUN_SAMPLE_COMPOSITION_TIME_OFFSET_PRESENT) p += 4;

        samples.push({
          bytes: buffer.subarray(dataCursor, dataCursor + size),
          duration,
          decodeTime: decodeTimeCursor,
        });
        dataCursor += size;
        decodeTimeCursor += duration;
      }
    }
    if (samples.length > 0) {
      fragments.push({ trackId: tfhd.trackId, baseMediaDecodeTime, samples });
    }
  }
  return fragments;
}
