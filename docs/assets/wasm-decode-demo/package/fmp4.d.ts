export interface Box {
    readonly type: string;
    /** Absolute offset of this box's own size field - i.e. including its header. */
    readonly boxStart: number;
    /** Byte range of this box's content, i.e. after its header. */
    readonly start: number;
    readonly end: number;
}
/**
 * Walks the immediate children of `buffer[rangeStart, rangeEnd)` - not
 * recursive, matching how every caller here already knows which boxes it
 * wants to recurse into and which to skip (recursing into every container
 * uniformly would mean re-deciding that at every level for no benefit).
 */
export declare function iterateBoxes(buffer: Uint8Array, rangeStart?: number, rangeEnd?: number): Generator<Box>;
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
export declare function parseInitSegment(buffer: Uint8Array): TrackInfo | null;
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
export declare function extractFragments(buffer: Uint8Array): Fragment[];
//# sourceMappingURL=fmp4.d.ts.map