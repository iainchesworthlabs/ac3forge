import type { RingBufferLayout } from "./ring-buffer.js";
import type { FoldOptions, WriteTarget } from "./types.js";
export interface InitMessage {
    type: "init";
    /**
     * URL of the Emscripten glue (`ac3forge_decode.js`) built from apps/wasm/.
     *
     * TRUST BOUNDARY: the worker fetches this URL and evaluates what comes back
     * as JavaScript (the glue is `MODULARIZE`d, so it is re-exported through a
     * Blob URL and `import`ed - see `loadEmscriptenGlue`). Whatever this points
     * at therefore runs with the worker's privileges. Point it at an asset you
     * ship; never at a URL derived from user input, a query parameter or a
     * third-party origin. CodeQL flags the fetch as request forgery for exactly
     * this reason, and it is right that the caller, not this package, is the one
     * who has to get it right.
     */
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