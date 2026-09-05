// The hls.js/MSE bridge (roadmap UX5) - the piece that actually addresses
// Chrome's lack of native EC-3 decode (video.js http-streaming#1297).
//
// A passive `Hls.Events.BUFFER_APPENDING` listener is not enough: reading
// hls.js's own buffer-controller confirmed that when `MediaSource.
// addSourceBuffer('audio/mp4;codecs="ec-3"')` throws (which it does, in
// every browser, since none of them can decode EC-3 via MSE), hls.js treats
// that as a non-fatal error, DELETES the audio track from its own state, and
// never calls BUFFER_APPENDING for audio again - so a passive listener would
// simply never fire for the case this bridge exists for.
//
// Instead this patches `MediaSource.isTypeSupported`/`addSourceBuffer` so
// hls.js believes the codec is supported and keeps demuxing/scheduling audio
// normally, handing a `FakeSourceBuffer` real bytes get appended to instead
// of a real one - which this module extracts (via fmp4.ts) and feeds to an
// Ac3ForgeDecoderNode.
//
// Known limitation, stated plainly rather than left to be discovered: A/V
// sync here is `syncTo()`'s clock alignment against the host media element,
// an approximation - not sample-accurate mux-level sync - and this has not
// been soak-tested against a real HLS server/live stream (see js/README.md's
// own "what's verified" section). Treat it as a working mechanism that needs
// real-stream hardening, not a finished integration.
import { extractFragments, parseInitSegment } from "./fmp4.js";
const DEFAULT_MIME_PATTERN = /(ec-3|ac-3)/i;
/**
 * The slice of the real `SourceBuffer` interface hls.js's `BufferController`
 * actually calls. `updateend` fires asynchronously (a microtask after
 * `appendBuffer`/`remove`), matching a real SourceBuffer's async-completion
 * contract closely enough for hls.js's own operation queue to behave as it
 * would against one.
 */
class FakeSourceBuffer extends EventTarget {
    updating = false;
    mode = "segments";
    timestampOffset = 0;
    #mimeType;
    #onSegment;
    #ranges = [];
    constructor(mimeType, onSegment) {
        super();
        this.#mimeType = mimeType;
        this.#onSegment = onSegment;
    }
    get buffered() {
        const ranges = this.#ranges;
        return {
            length: ranges.length,
            start: (i) => ranges[i].start,
            end: (i) => ranges[i].end,
        };
    }
    appendBuffer(data) {
        if (this.updating) {
            throw new DOMException("Cannot append while updating", "InvalidStateError");
        }
        this.updating = true;
        const bytes = data instanceof Uint8Array ? data : new Uint8Array(ArrayBuffer.isView(data) ? data.buffer : data);
        queueMicrotask(() => {
            try {
                this.#onSegment(this.#mimeType, bytes, { markBuffered: (start, end) => this.#ranges.push({ start, end }) });
                this.updating = false;
                this.dispatchEvent(new Event("update"));
            }
            catch {
                this.updating = false;
                this.dispatchEvent(new Event("error"));
            }
            finally {
                this.dispatchEvent(new Event("updateend"));
            }
        });
    }
    remove(start, end) {
        this.updating = true;
        queueMicrotask(() => {
            this.#ranges = this.#ranges.filter((range) => range.end <= start || range.start >= end);
            this.updating = false;
            this.dispatchEvent(new Event("updateend"));
        });
    }
    abort() {
        this.updating = false;
    }
}
/**
 * Patches `MediaSource.isTypeSupported`/`MediaSource.prototype.
 * addSourceBuffer` for mime types matching `mimeTypePattern`, delegating
 * everything else to the real implementation untouched. Returns an
 * uninstall function.
 */
export function installMediaSourceShim(options) {
    const pattern = options.mimeTypePattern ?? DEFAULT_MIME_PATTERN;
    const RealMediaSource = MediaSource;
    const originalIsTypeSupported = RealMediaSource.isTypeSupported;
    const originalAddSourceBuffer = RealMediaSource.prototype.addSourceBuffer;
    const fakeBuffers = new WeakMap();
    RealMediaSource.isTypeSupported = (type) => pattern.test(type) || originalIsTypeSupported.call(RealMediaSource, type);
    RealMediaSource.prototype.addSourceBuffer = function (type) {
        if (!pattern.test(type)) {
            return originalAddSourceBuffer.call(this, type);
        }
        const fake = new FakeSourceBuffer(type, options.onSegment);
        fakeBuffers.set(fake, fake);
        return fake;
    };
    return function uninstall() {
        RealMediaSource.isTypeSupported = originalIsTypeSupported;
        RealMediaSource.prototype.addSourceBuffer = originalAddSourceBuffer;
    };
}
/**
 * Wires {@link installMediaSourceShim} to an {@link Ac3ForgeDecoderNode}:
 * every captured segment is run through fmp4.ts - an init segment (no
 * `moof`) just records the track's timescale, a media segment's samples are
 * each pushed to the decoder node as their own access unit.
 */
export function attachHlsAudioBridge(options) {
    let track = null;
    return installMediaSourceShim({
        ...(options.mimeTypePattern ? { mimeTypePattern: options.mimeTypePattern } : {}),
        onSegment: (_mimeType, data, sink) => {
            const fragments = extractFragments(data);
            if (fragments.length === 0) {
                track = parseInitSegment(data) ?? track;
                return;
            }
            for (const fragment of fragments) {
                for (const sample of fragment.samples) {
                    if (sample.bytes.length === 0)
                        continue;
                    options.decoderNode.pushAccessUnit(sample.bytes);
                }
                // §8.8.12's baseMediaDecodeTime/duration, in the track's own
                // timescale - an approximation of the real buffered range (it
                // trusts every sample decoded rather than confirming each one did),
                // good enough for hls.js's own gap-detection/back-buffer logic to
                // behave sanely. See this module's header comment on what "sync"
                // means here.
                if (track && fragment.samples.length > 0) {
                    const first = fragment.samples[0];
                    const last = fragment.samples[fragment.samples.length - 1];
                    sink.markBuffered(first.decodeTime / track.timescale, (last.decodeTime + last.duration) / track.timescale);
                }
            }
        },
    });
}
/**
 * Approximate A/V sync: aligns `audioContext`'s notion of "now" to
 * `mediaElement`'s clock on play/pause/seek. This is a documented
 * approximation, not sample-accurate mux-level sync - see this module's own
 * header comment. Returns a function that stops watching.
 */
export function syncTo(mediaElement, onSeek) {
    const handleSeeked = () => onSeek(mediaElement.currentTime);
    mediaElement.addEventListener("seeked", handleSeeked);
    mediaElement.addEventListener("play", handleSeeked);
    return () => {
        mediaElement.removeEventListener("seeked", handleSeeked);
        mediaElement.removeEventListener("play", handleSeeked);
    };
}
//# sourceMappingURL=hls-bridge.js.map