import type { Ac3ForgeDecoderNode } from "./decoder-node.js";
export interface SegmentSink {
    /** Records that [startSeconds, endSeconds) is now buffered - reflected in this SourceBuffer's own `buffered` range. */
    markBuffered(startSeconds: number, endSeconds: number): void;
}
type SegmentListener = (mimeType: string, data: Uint8Array, sink: SegmentSink) => void;
export interface MediaSourceShimOptions {
    /** Which `addSourceBuffer(mimeType)` calls get the fake buffer. Default: `/(ec-3|ac-3)/i`. */
    mimeTypePattern?: RegExp;
    onSegment: SegmentListener;
}
/**
 * Patches `MediaSource.isTypeSupported`/`MediaSource.prototype.
 * addSourceBuffer` for mime types matching `mimeTypePattern`, delegating
 * everything else to the real implementation untouched. Returns an
 * uninstall function.
 */
export declare function installMediaSourceShim(options: MediaSourceShimOptions): () => void;
export interface HlsAudioBridgeOptions {
    mimeTypePattern?: RegExp;
    decoderNode: Ac3ForgeDecoderNode;
}
/**
 * Wires {@link installMediaSourceShim} to an {@link Ac3ForgeDecoderNode}:
 * every captured segment is run through fmp4.ts - an init segment (no
 * `moof`) just records the track's timescale, a media segment's samples are
 * each pushed to the decoder node as their own access unit.
 */
export declare function attachHlsAudioBridge(options: HlsAudioBridgeOptions): () => void;
/**
 * Approximate A/V sync: aligns `audioContext`'s notion of "now" to
 * `mediaElement`'s clock on play/pause/seek. This is a documented
 * approximation, not sample-accurate mux-level sync - see this module's own
 * header comment. Returns a function that stops watching.
 */
export declare function syncTo(mediaElement: HTMLMediaElement, onSeek: (mediaTimeSeconds: number) => void): () => void;
export {};
//# sourceMappingURL=hls-bridge.d.ts.map