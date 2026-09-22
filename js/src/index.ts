// ac3forge-wasm-decoder: a streaming AC-3/E-AC-3 decoder for the browser,
// compiled from ac3::forge (https://github.com/iainchesworthlabs/ac3forge)
// to WebAssembly. See README.md for usage; docs/platforms/wasm.md in the
// main repository for how the underlying WASM module is built.

export { PushDecoder, scanStream } from "./push-decoder.js";
export { decodeFile } from "./decode-file.js";
export type { DecodedProgram, DecodeFileOptions } from "./decode-file.js";
export { Ac3ForgeDecoderNode } from "./decoder-node.js";
export type { Ac3ForgeDecoderNodeOptions, StreamInfoEventDetail } from "./decoder-node.js";
export { allocateRingBuffer, RingBufferReader, RingBufferWriter } from "./ring-buffer.js";
export type { RingBufferLayout } from "./ring-buffer.js";
export { extractFragments, parseInitSegment } from "./fmp4.js";
export type { Fragment as Fmp4Fragment, Sample as Fmp4Sample, TrackInfo as Fmp4TrackInfo } from "./fmp4.js";
export { attachHlsAudioBridge, installMediaSourceShim, syncTo } from "./hls-bridge.js";
export type { HlsAudioBridgeOptions, MediaSourceShimOptions, SegmentSink } from "./hls-bridge.js";

export { DownmixTarget } from "./types.js";
export type {
  Ac3ForgeEmbindModule,
  Ac3ForgeModuleFactory,
  FlushEntry,
  FoldOptions,
  ObjectFrame,
  PushMetadata,
  PushOutcome,
  ScanOutcome,
} from "./types.js";
