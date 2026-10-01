// iclforge-wasm-decoder: a streaming AC-3/E-AC-3 decoder for the browser,
// compiled from iclforge (https://github.com/iainchesworthlabs/iclforge)
// to WebAssembly. See README.md for usage; docs/platforms/wasm.md in the
// main repository for how the underlying WASM module is built.

export { PushDecoder, scanStream } from "./push-decoder.js";
export { decodeFile } from "./decode-file.js";
export type { DecodedProgram, DecodeFileOptions } from "./decode-file.js";
export { IclForgeDecoderNode } from "./decoder-node.js";
export type { IclForgeDecoderNodeOptions, StreamInfoEventDetail } from "./decoder-node.js";
export { allocateRingBuffer, RingBufferReader, RingBufferWriter } from "./ring-buffer.js";
export type { RingBufferLayout } from "./ring-buffer.js";
export { extractFragments, parseInitSegment } from "./fmp4.js";
export type { Fragment as Fmp4Fragment, Sample as Fmp4Sample, TrackInfo as Fmp4TrackInfo } from "./fmp4.js";
export { attachHlsAudioBridge, installMediaSourceShim, syncTo } from "./hls-bridge.js";
export type { HlsAudioBridgeOptions, MediaSourceShimOptions, SegmentSink } from "./hls-bridge.js";

export { DownmixTarget } from "./types.js";
export type {
  IclForgeEmbindModule,
  IclForgeModuleFactory,
  FlushEntry,
  FoldOptions,
  ObjectFrame,
  PushMetadata,
  PushOutcome,
  ScanOutcome,
} from "./types.js";
