// ac3forge-wasm-decoder: a streaming AC-3/E-AC-3 decoder for the browser,
// compiled from ac3::forge (https://github.com/iainchesworthlabs/ac3forge)
// to WebAssembly. See README.md for usage; docs/platforms/wasm.md in the
// main repository for how the underlying WASM module is built.
export { PushDecoder, scanStream } from "./push-decoder.js";
export { decodeFile } from "./decode-file.js";
export { Ac3ForgeDecoderNode } from "./decoder-node.js";
export { allocateRingBuffer, RingBufferReader, RingBufferWriter } from "./ring-buffer.js";
export { extractFragments, parseInitSegment } from "./fmp4.js";
export { attachHlsAudioBridge, installMediaSourceShim, syncTo } from "./hls-bridge.js";
export { DownmixTarget } from "./types.js";
//# sourceMappingURL=index.js.map