import type { RingBufferLayout } from "./ring-buffer.js";
import type { FoldOptions, WriteTarget } from "./types.js";
export interface InitMessage {
    type: "init";
    /** URL of the Emscripten glue (`ac3forge_decode.js`) built from apps/wasm/. */
    glueUrl: string;
    fold?: FoldOptions;
    sab: SharedArrayBuffer;
    layout: RingBufferLayout;
    /** Which of PushDecoder's outputs feeds the ring buffer. */
    writeTarget: WriteTarget;
}
export interface PushMessage {
    type: "push";
    /** Transferred, not copied - detached from the caller's side after postMessage. */
    bytes: ArrayBuffer;
}
export type InboundMessage = InitMessage | PushMessage | {
    type: "flush";
} | {
    type: "close";
};
//# sourceMappingURL=decoder-worker.d.ts.map