/** iclforge::ac4::DrcMode's own numeric order (decoder.hpp) - kept in sync by hand. */
export declare enum Ac4DrcMode {
    Off = 0,
    Default = 1,
    HomeTheatre = 2,
    FlatPanelTv = 3,
    PortableSpeakers = 4,
    PortableHeadphones = 5
}
/** iclforge::ac4::DownmixTarget's own numeric order (decoder.hpp) - kept in sync by hand. */
export declare enum Ac4DownmixTarget {
    AsCoded = 0,
    FiveX = 1,
    Stereo = 2,
    LoRo = 3,
    LtRt = 4,
    Mono = 5,
    SevenX4 = 6,
    SevenX2 = 7,
    SevenX0 = 8,
    FiveX4 = 9,
    FiveX2 = 10
}
/** iclforge::ac4::DecodingMode's own numeric order (decoder.hpp) - kept in sync by hand. */
export declare enum Ac4DecodingMode {
    Full = 0,
    Core = 1
}
/** iclforge::ac4::ConcealmentPolicy's own numeric order (decoder.hpp) - kept in sync by hand. */
export declare enum Ac4ConcealmentPolicy {
    None = 0,
    RepeatFade = 1,
    Mute = 2
}
/** iclforge::ac4::CodecMode's own numeric order (encoder.hpp) - kept in sync by hand. */
export declare enum Ac4CodecMode {
    Auto = 0,
    Simple = 1,
    Aspx = 2,
    AspxAcpl1 = 3,
    AspxAcpl2 = 4,
    AspxAcpl3 = 5,
    Scpl = 6,
    AspxScpl = 7,
    AspxAjcc = 8
}
/** iclforge::ac4::RateMode's own numeric order (encoder.hpp) - kept in sync by hand. */
export declare enum Ac4RateMode {
    Constant = 0,
    Average = 1,
    Variable = 2
}
/** iclforge::ac4::BedChannel's own codes, Part 2 Table 66's nonstd_bed_channel_assignment (encoder.hpp) - kept in sync by hand. */
export declare enum Ac4BedChannel {
    Left = 0,
    Right = 1,
    Centre = 2,
    LeftSurround = 4,
    RightSurround = 5,
    LeftBack = 6,
    RightBack = 7,
    TopFrontLeft = 8,
    TopFrontRight = 9,
    TopSideLeft = 10,
    TopSideRight = 11,
    TopBackLeft = 12,
    TopBackRight = 13,
    LeftWide = 14,
    RightWide = 15
}
/** iclforge::ac4::ObjectCoding's own numeric order (encoder.hpp) - kept in sync by hand. */
export declare enum Ac4ObjectCoding {
    /** An A-JOC substream (Part 2 clause 5.7): a downmix and the matrices that rebuild the objects from it. */
    Ajoc = 0,
    /** Direct-coded object substreams (clause 6.2.1.11): dynamic objects and the LFE, no bed objects. */
    Direct = 1
}
/** iclforge::ac4::AjocDownmix's own numeric order (encoder.hpp) - kept in sync by hand. */
export declare enum Ac4AjocDownmix {
    /** Downmix signals the encoder computes, each the sum of a group of objects. */
    Computed = 0,
    /** A static 5.0 bed the objects are panned onto by X and Y (no LFE object). */
    Static50 = 1,
    /** A static 5.1 bed, the LFE object onto the LFE. */
    Static51 = 2
}
/** iclforge::ac4::AdditionalPair's own numeric order (encoder.hpp, Part 1 Table 88) - kept in sync by hand. */
export declare enum Ac4AdditionalPair {
    None = 0,
    /** 3/4/0: Lb and Rb. */
    Back = 1,
    /** 5/2/0: Lw and Rw. */
    Wide = 2,
    /** 3/2/2: Tfl and Tfr. */
    TopFront = 3
}
export interface Ac4PresentationChoice {
    /** iclforge::ac4::PresentationChoice::presentation_id; unset (or omitted) selects by index/preference instead. */
    presentationId?: number;
    /** iclforge::ac4::PresentationChoice::index, a position in the table of contents. */
    index?: number;
    /** An IETF BCP 47 tag; empty (the default) for none. */
    language?: string;
}
export interface Ac4OutputOptions {
    /** iclforge::ac4::OutputConfig::output_level_dbfs; unset leaves the stream at its coded level. */
    outputLevelDbfs?: number;
    drc?: Ac4DrcMode;
    headphones?: boolean;
    dialogueEnhancementDb?: number;
    downmix?: Ac4DownmixTarget;
    mixLfe?: boolean;
    dialogueGainDb?: number;
    associatedGainDb?: number;
}
export interface Ac4DecoderOptions {
    outputLevelDbfs?: number;
    drc?: Ac4DrcMode;
    downmix?: Ac4DownmixTarget;
    decodingMode?: Ac4DecodingMode;
    concealment?: Ac4ConcealmentPolicy;
    presentation?: Ac4PresentationChoice;
    /** iclforge::ac4::DecoderConfig::level (md_compat); default 3, matching the C++ struct default. */
    mdCompatLevel?: number;
}
/**
 * iclforge::ac4::ObjectProperties (Part 2 Annex F.2 to F.10 and add_per_object_md()'s
 * data), as the decoder returns it and the encoder takes it; every field is
 * optional for the encoder and keeps iclforge::ac4::ObjectProperties{}'s default (active,
 * 0 dB, priority 1, the room's centre, depth exponent 1) when left out. Each
 * value is written to the nearest its code has and refused off its range:
 * gainDb +15 to -49 dB in steps of 1, or -Infinity; priority 0 to 1 in steps of
 * 1/31; position X and Y 0 to 1 in steps of 1/62, Z -1 to 1 in steps of 1/15
 * (a dynamic object's); zoneMask 0 to 7; width 0 to 1 in steps of 1/31 per
 * axis; screenFactor 0 or 1/8 to 1 in steps of 1/8; depthExponent exactly
 * 0.25, 0.5, 1 or 2; distance 1 or more, or Infinity, null for none;
 * divergence 0 to 1; headphoneRenderMode 0 to 3, null for none.
 */
export interface RawAc4ObjectProperties {
    active: boolean;
    gainDb: number;
    priority: number;
    /** [x, y, z], Annex F.2. */
    position: [number, number, number];
    zoneMask: number;
    enableElevation: boolean;
    snap: boolean;
    /** The object's width in X, Y and Z, Annex F.6. */
    width: [number, number, number];
    screenFactor: number;
    depthExponent: number;
    distance: number | null;
    divergence: number;
    trimDisabled: boolean;
    headphoneRenderMode: number | null;
    headTrackDisabled: boolean;
}
/** An object's metadata for the encoder: the fields to change from iclforge::ac4::ObjectProperties{}'s defaults. */
export type Ac4ObjectProperties = Partial<RawAc4ObjectProperties>;
/** iclforge::ac4::ObjectConfig: one object of an {@link Ac4ObjectsConfig}, the input channel at its index. */
export interface Ac4ObjectConfig {
    /** A bed object from this loudspeaker; a dynamic object where left out. */
    bed?: Ac4BedChannel;
    /** The LFE, at most one object's: its bed channel and position are ignored. */
    lfe?: boolean;
    /** What is in force from the first sample. */
    properties?: Ac4ObjectProperties;
}
/**
 * iclforge::ac4::ObjectsConfig: the objects of the one object substream a stream can
 * have, and how they are coded. The limits are the encoder's: 1 to 64
 * objects, at most one the LFE and at least one not; as A-JOC a computed
 * downmix of `downmixSignals` signals (1 to 11, no more than the full-band
 * objects) or a static 5.0 bed (no LFE object) or 5.1 bed (with one), and
 * `parameterBands` one of 23, 15, 12, 9, 7, 5, 3 or 1; direct-coded, dynamic
 * objects and the LFE only; frameRateIndex 13 only. {@link Ac4Encoder.constructionError}
 * names the rule a configuration breaks.
 */
export interface Ac4ObjectsConfig {
    objects: Ac4ObjectConfig[];
    coding?: Ac4ObjectCoding;
    downmix?: Ac4AjocDownmix;
    /** A computed downmix's signals; one a 32 kbps of the substream's rate, up to 10, where left out. */
    downmixSignals?: number;
    /** A-JOC's decorrelators (Part 2 clause 5.7.3.5). */
    decorrelation?: boolean;
    /** A-JOC's parameter bands (Table 78); 23, 15 or 12 by the rate where left out. */
    parameterBands?: number;
    coarse?: boolean;
    /** oamd_common_data()'s master_screen_size_ratio_code, 0 to 31. */
    screenSizeRatioCode?: number;
    bedObjectChanDistribute?: boolean;
}
/**
 * iclforge::ac4::ObjectMetadataUpdate: a change to an object's metadata, given to
 * {@link Ac4Encoder.encode} with the input it belongs to. From input sample
 * `sample` of that call's channels (0 its first, and any later one) the object
 * `object` - an index into {@link Ac4ObjectsConfig.objects} - moves to
 * `properties` over `rampSamples` (0 to 2047, or 2048). The decoder reports it
 * at the output sample the input sample comes out at ({@link Ac4Encoder.delaySamples}
 * plus {@link Ac4Encoder.decoderDelaySamples} later), to within 32 samples.
 */
export interface Ac4ObjectMetadataUpdate {
    object: number;
    sample: number;
    rampSamples?: number;
    properties?: Ac4ObjectProperties;
}
/**
 * iclforge::ac4::EncoderConfig::Experimental: syntax only this project's readers have
 * read from this encoder, off unless asked for. drc_gains and three_zero,
 * which need the DRC modes and the substream list this binding does not
 * carry, are not here.
 */
export interface Ac4Experimental {
    /** The ASPX mode's pairs as sum and balance where that is fewer bits. */
    aspxBalance?: boolean;
    /** The ASPX mode's VARVAR framing. */
    aspxVarvar?: boolean;
    /** Frequency interleaved waveform coding above the crossover. */
    aspxInterleave?: boolean;
    /** The 5.X and 7.X elements' coding_config 1 to 3 and 2ch_mode 1. */
    codingConfigs?: boolean;
    /** Seven or eight input channels, with this pair beyond L R C Ls Rs. */
    sevenX?: Ac4AdditionalPair;
    /** The A-CPL modes DEE's streams do not use (ASPX_ACPL_1, stereo A-CPL). */
    acpl?: boolean;
    /** 7.0.4 and 7.1.4 with the back pair: eleven or twelve input channels. */
    backPair?: boolean;
    /** The immersive element's ASPX_AJCC. */
    ajcc?: boolean;
    /** Object audio; required by {@link Ac4EncoderOptions.objects}. */
    objects?: boolean;
}
/** iclforge::ac4::EncoderConfig, less what ac4_bindings.cpp's header comment leaves out. A field left out keeps the C++ default. */
export interface Ac4EncoderOptions {
    /** 1, 2, 5, 6, 9 or 10 (see iclforge::ac4::EncoderConfig::channels); ignored with `objects`. */
    channels?: number;
    sampleRateHz?: number;
    frameRateIndex?: number;
    bitrateKbps?: number;
    rateMode?: Ac4RateMode;
    /** With `objects`, the object substream's. */
    codecMode?: Ac4CodecMode;
    iframeInterval?: number;
    dialnormDb?: number;
    /** Frames, counted from 0, that must be I-frames besides those `iframeInterval` makes. */
    iframes?: number[];
    /** Where the caller's fragments start, in samples of the decoded output from its first: the frame whose output starts there, or the first to start after it, is an I-frame. */
    fragmentStarts?: number[];
    experimental?: Ac4Experimental;
    /** The stream's one object substream in place of channels; needs `experimental.objects`. */
    objects?: Ac4ObjectsConfig;
}
/** iclforge::ac4::PresentationInfo, as Ac4Decoder.presentations() returns it. */
export interface RawAc4Presentation {
    index: number;
    presentationId: number | null;
    mdCompat: number | null;
    enabled: boolean;
    alternative: boolean;
    name: string;
    language: string;
    decodable: boolean;
    selectable: boolean;
    /** Channel layout as strings (iclforge::ac4::describe(Speaker)), the same convention decoder_bindings.cpp uses for AC-3. */
    speakers: string[];
}
export interface RawAc4Concealment {
    error: string;
    action: "repeatFade" | "mute";
}
/** iclforge::ac4::ObjectUpdate (Part 2 Annex F.11): one block update within a decoded frame. */
export interface RawAc4ObjectUpdate {
    /** The output sample of the frame the update takes effect at, counted with the decoder's delay as the frame's channels are. */
    sample: number;
    /** The samples a renderer takes to move to `properties` from what was in force. */
    rampSamples: number;
    properties: RawAc4ObjectProperties;
}
export interface RawAc4Object {
    kind: "bed" | "dyn" | "isf";
    lfe: boolean;
    /** A bed object's loudspeaker (iclforge::ac4::describe(Speaker)); null otherwise. */
    speaker: string | null;
    samples: Float32Array;
    /** What is in force at the frame's first sample. */
    properties: RawAc4ObjectProperties;
    /** The block updates within the frame, in the order they take effect. */
    updates: RawAc4ObjectUpdate[];
}
/** What Ac4Decoder.decodeFrame() returns for a decoded (or concealed) frame. */
export interface RawAc4DecodedFrame {
    sampleRate: number;
    sequenceCounter: number;
    /** Index of the decoded presentation in the frame's table of contents. */
    presentation: number;
    presentationId: number | null;
    samples: number;
    channels: Float32Array[];
    speakers: string[];
    concealed: RawAc4Concealment | null;
    /** In the decoder's order, not the encoder's input order: the LFE first, then the bed objects, then the dynamic objects, each group in the order the encoder's {@link Ac4ObjectsConfig} lists it. */
    objects: RawAc4Object[];
}
export interface RawAc4EncodedFrame {
    data: Uint8Array;
    /** PCM samples per channel this frame decodes to, at the input's rate. */
    samples: number;
    iframe: boolean;
}
/** The Embind class ac4_bindings.cpp's `Ac4Decoder` builds. */
export interface NativeAc4Decoder {
    decodeFrame(bytes: Uint8Array): RawAc4DecodedFrame | null;
    setOutput(outputLevelDbfs: number, drc: number, headphones: boolean, dialogueEnhancementDb: number, downmix: number, mixLfe: boolean, dialogueGainDb: number, associatedGainDb: number): void;
    setPresentation(presentationId: number, presentationIndex: number, language: string): void;
    reset(): void;
    refusalReason(): string;
    latencySamples(): number;
    presentations(): RawAc4Presentation[];
    delete(): void;
}
/** The Embind class ac4_bindings.cpp's `Ac4Encoder` builds. */
export interface NativeAc4Encoder {
    encode(channels: Float32Array[], updates: Ac4ObjectMetadataUpdate[]): RawAc4EncodedFrame[];
    flush(): RawAc4EncodedFrame[];
    error(): string;
    constructionError(): string;
    codecMode(): number;
    delaySamples(): number;
    decoderDelaySamples(): number;
    buildDac4(): Uint8Array;
    dac4Refusal(): string;
    delete(): void;
}
/** The Embind module `apps/wasm/ac4_bindings.cpp` builds - what `createIclForgeAc4Module()` resolves to. */
export interface Ac4EmbindModule {
    Ac4Decoder: new (outputLevelDbfs: number, drc: number, downmix: number, decodingMode: number, concealment: number, presentationId: number, presentationIndex: number, language: string, level: number) => NativeAc4Decoder;
    Ac4Encoder: new (options: Ac4EncoderOptions) => NativeAc4Encoder;
    syncFrame(rawFrame: Uint8Array, crc: boolean): Uint8Array;
}
/** The MODULARIZE factory Emscripten attaches as `createIclForgeAc4Module` - see apps/wasm/CMakeLists.txt's link options. */
export type Ac4ModuleFactory = (moduleOverrides?: Record<string, unknown>) => Promise<Ac4EmbindModule>;
/**
 * Fetches, loads and instantiates the AC-4 Embind module from `glueUrl`
 * (the compiled `iclforge_ac4.js`). `locateFile` is set so the glue's own
 * `.wasm` fetch resolves beside `glueUrl` rather than against the Blob URL
 * it was imported from - see this file's header comment.
 */
export declare function loadAc4Module(glueUrl: string): Promise<Ac4EmbindModule>;
/** A thin, typed wrapper over the Embind `Ac4Decoder` class - see this file's header comment. */
export declare class Ac4Decoder {
    #private;
    constructor(module: Ac4EmbindModule, options?: Ac4DecoderOptions);
    /**
     * Decodes one raw_ac4_frame (an iclforge::ac4::SyncFrame's raw_ac4_frame, or an MP4
     * sample - strip any container/sync-frame wrapper first). Null for a
     * frame with no output and for a decode error with no concealment
     * configured - {@link refusalReason} says why in either case.
     *
     * The channel/object Float32Arrays on the returned frame are zero-copy
     * views into the WASM heap, valid only until the next `decodeFrame()`/
     * `reset()` call on this instance - copy them out if you need them later.
     */
    decodeFrame(bytes: Uint8Array): RawAc4DecodedFrame | null;
    /** Changes the output processing from the next frame (iclforge::ac4::Decoder::set_output()). */
    setOutput(options?: Ac4OutputOptions): void;
    /** Changes which presentation is decoded from the next frame (iclforge::ac4::Decoder::set_presentation()). */
    setPresentation(choice?: Ac4PresentationChoice): void;
    /** Forgets everything carried between frames (iclforge::ac4::Decoder::reset()). */
    reset(): void;
    /** Why the last decodeFrame() failed, returned nothing or returned a concealed frame; empty otherwise. */
    get refusalReason(): string;
    /** The decoder's delay at the output rate for the stream as last decoded. */
    get latencySamples(): number;
    /** The presentations of the last frame read, in table-of-contents order; empty before one. */
    get presentations(): RawAc4Presentation[];
    /** Releases the underlying WASM object. Call when done - Embind instances are not garbage collected. */
    close(): void;
}
/** A thin, typed wrapper over the Embind `Ac4Encoder` class - see this file's header comment. */
export declare class Ac4Encoder {
    #private;
    /**
     * A configuration the encoder refuses leaves no encoder to encode with:
     * {@link constructionError} says why, and encode()/flush() return no frames.
     */
    constructor(module: Ac4EmbindModule, options?: Ac4EncoderOptions);
    /**
     * Planar samples at full scale 1.0, one Float32Array per input channel (or
     * per object of the object substream), any length, and for an encoder of
     * objects the changes to the objects' metadata within this input or after
     * it, in any order ({@link Ac4ObjectMetadataUpdate}). One the encoder
     * refuses - an object it lacks, a sample before this input's first, a
     * property off its range - fails the whole call: no frames, and
     * {@link error} says why. Returns the frames this input completes, in
     * order; the encoder's delay holds back the frames the last input still
     * needs. Each returned frame's `data` is an owned copy (unlike
     * decodeFrame()'s PCM views) - see ac4_bindings.cpp's own comment on why.
     */
    encode(channels: Float32Array[], updates?: Ac4ObjectMetadataUpdate[]): RawAc4EncodedFrame[];
    /** Ends the stream: returns the frames the delay still held. Takes no input after this. */
    flush(): RawAc4EncodedFrame[];
    /** Why the last encode()/flush() call produced no frames when some were expected; empty otherwise. */
    get error(): string;
    /** Why the constructor made no encoder - the first rule the configuration breaks, or the option it could not read; empty once construction succeeded. */
    get constructionError(): string;
    /** The codec mode the stream is coded in: what Ac4CodecMode.Auto resolved to, never Auto itself. */
    get codecMode(): number;
    /** Samples of silence the encoder puts before the input, at the input's rate. */
    get delaySamples(): number;
    /** The delay iclforge::ac4::Decoder adds on top of {@link delaySamples}, at the input's rate. */
    get decoderDelaySamples(): number;
    /** The 'dac4' box for the stream as encoded so far (iclforge::ac4::build_dac4()); empty where {@link dac4Refusal} is non-empty. */
    buildDac4(): Uint8Array;
    /** Why buildDac4() has nothing to describe; empty once construction succeeded and every presentation can be described whole. */
    dac4Refusal(): string;
    /** Releases the underlying WASM object. Call when done - Embind instances are not garbage collected. */
    close(): void;
}
/** iclforge::ac4::sync_frame(): wraps `rawFrame` with the sync word, optional CRC and frame_size, for a raw .ac4 file or MPEG-2 TS. */
export declare function syncFrame(module: Ac4EmbindModule, rawFrame: Uint8Array, crc: boolean): Uint8Array;
//# sourceMappingURL=ac4.d.ts.map