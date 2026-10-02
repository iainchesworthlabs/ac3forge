// A typed wrapper over the AC-4 embind module apps/wasm/ac4_bindings.cpp
// builds (apps/wasm/CMakeLists.txt's `iclforge_wasm_ac4` target,
// `-sEXPORT_NAME=createIclForgeAc4Module`) - the AC-4 counterpart of
// push-decoder.ts, not of decoder-worker.ts.
//
// NO WORKER PROTOCOL HERE, unlike decoder-worker.ts's realtime AudioWorklet
// pipeline (a dedicated module Worker, postMessage init/push/flush/close, a
// SharedArrayBuffer ring buffer for the audio-rendering thread to drain).
// That protocol is shaped specifically for one job: continuous PCM feeding a
// live Web Audio graph from a single decode-only class with a "channels" vs
// "fold" output choice (types.ts's WriteTarget). Nothing about this module
// matches that shape - it covers BOTH decode AND encode, the decoder's own
// surface is wider and unrelated to a fold (presentations, concealment,
// object audio), and there is no existing encode-side Worker/ring-buffer
// precedent anywhere in this package to extend either: encoder_bindings.cpp's
// own consumer (apps/wasm/encode/app.js) drives its `Encoder` class directly
// from the page, no Worker involved. Rather than force AC-4's shape onto a
// protocol built for a different job, or invent a second, unrelated Worker
// protocol from nothing, this is a plain ES module: it loads the glue and
// exposes Ac4Decoder/Ac4Encoder directly, the same "thin typed class wrapping
// the native embind class 1:1" shape push-decoder.ts's PushDecoder already
// uses. A Worker-based pipeline - if AC-4 ever needs realtime playback the
// way the AC-3 side does - belongs beside this file later, built ON TOP of
// these two classes exactly as decoder-worker.ts is built on push-decoder.ts,
// not folded into this one.
//
// The one piece of decoder-worker.ts's technique this file DOES need, and
// copies exactly: loadEmscriptenGlue()'s fetch-text/Blob/import() dance and
// its `locateFile` override. `-sMODULARIZE=1` output is a classic/UMD
// script, not an ES module, so it cannot be `import`ed directly; and without
// `locateFile`, the glue's own relative `.wasm` fetch resolves against the
// `blob:` URL it was imported from rather than against `glueUrl`, and fails
// silently deep inside the glue's own async init chain (see memory note
// feedback-emscripten-worker-blob-locatefile.md for the exact failure mode
// this avoids).
//
// Two conventions cross into the native calls below - see ac4_bindings.cpp's
// own header comment for the reasons. The decoder's constructor and setters
// take flat primitives with two sentinels, because neither AC-3 binding had
// an existing convention to copy: NaN for "no output level set" (the wire
// value a JS `undefined` already coerces to for a `double` parameter, so
// passing either works), and -1 for "no presentation id/index chosen" (ints
// have no NaN of their own, and 0 is a real id/index). The encoder's
// constructor and its metadata updates are plain objects, the caller's
// options as given: a field left out keeps the C++ struct's default, so the
// defaults live in one place, the C++ header, and are not repeated here.
/** iclforge::ac4::DrcMode's own numeric order (decoder.hpp) - kept in sync by hand. */
export var Ac4DrcMode;
(function (Ac4DrcMode) {
    Ac4DrcMode[Ac4DrcMode["Off"] = 0] = "Off";
    Ac4DrcMode[Ac4DrcMode["Default"] = 1] = "Default";
    Ac4DrcMode[Ac4DrcMode["HomeTheatre"] = 2] = "HomeTheatre";
    Ac4DrcMode[Ac4DrcMode["FlatPanelTv"] = 3] = "FlatPanelTv";
    Ac4DrcMode[Ac4DrcMode["PortableSpeakers"] = 4] = "PortableSpeakers";
    Ac4DrcMode[Ac4DrcMode["PortableHeadphones"] = 5] = "PortableHeadphones";
})(Ac4DrcMode || (Ac4DrcMode = {}));
/** iclforge::ac4::DownmixTarget's own numeric order (decoder.hpp) - kept in sync by hand. */
export var Ac4DownmixTarget;
(function (Ac4DownmixTarget) {
    Ac4DownmixTarget[Ac4DownmixTarget["AsCoded"] = 0] = "AsCoded";
    Ac4DownmixTarget[Ac4DownmixTarget["FiveX"] = 1] = "FiveX";
    Ac4DownmixTarget[Ac4DownmixTarget["Stereo"] = 2] = "Stereo";
    Ac4DownmixTarget[Ac4DownmixTarget["LoRo"] = 3] = "LoRo";
    Ac4DownmixTarget[Ac4DownmixTarget["LtRt"] = 4] = "LtRt";
    Ac4DownmixTarget[Ac4DownmixTarget["Mono"] = 5] = "Mono";
    Ac4DownmixTarget[Ac4DownmixTarget["SevenX4"] = 6] = "SevenX4";
    Ac4DownmixTarget[Ac4DownmixTarget["SevenX2"] = 7] = "SevenX2";
    Ac4DownmixTarget[Ac4DownmixTarget["SevenX0"] = 8] = "SevenX0";
    Ac4DownmixTarget[Ac4DownmixTarget["FiveX4"] = 9] = "FiveX4";
    Ac4DownmixTarget[Ac4DownmixTarget["FiveX2"] = 10] = "FiveX2";
})(Ac4DownmixTarget || (Ac4DownmixTarget = {}));
/** iclforge::ac4::DecodingMode's own numeric order (decoder.hpp) - kept in sync by hand. */
export var Ac4DecodingMode;
(function (Ac4DecodingMode) {
    Ac4DecodingMode[Ac4DecodingMode["Full"] = 0] = "Full";
    Ac4DecodingMode[Ac4DecodingMode["Core"] = 1] = "Core";
})(Ac4DecodingMode || (Ac4DecodingMode = {}));
/** iclforge::ac4::ConcealmentPolicy's own numeric order (decoder.hpp) - kept in sync by hand. */
export var Ac4ConcealmentPolicy;
(function (Ac4ConcealmentPolicy) {
    Ac4ConcealmentPolicy[Ac4ConcealmentPolicy["None"] = 0] = "None";
    Ac4ConcealmentPolicy[Ac4ConcealmentPolicy["RepeatFade"] = 1] = "RepeatFade";
    Ac4ConcealmentPolicy[Ac4ConcealmentPolicy["Mute"] = 2] = "Mute";
})(Ac4ConcealmentPolicy || (Ac4ConcealmentPolicy = {}));
/** iclforge::ac4::CodecMode's own numeric order (encoder.hpp) - kept in sync by hand. */
export var Ac4CodecMode;
(function (Ac4CodecMode) {
    Ac4CodecMode[Ac4CodecMode["Auto"] = 0] = "Auto";
    Ac4CodecMode[Ac4CodecMode["Simple"] = 1] = "Simple";
    Ac4CodecMode[Ac4CodecMode["Aspx"] = 2] = "Aspx";
    Ac4CodecMode[Ac4CodecMode["AspxAcpl1"] = 3] = "AspxAcpl1";
    Ac4CodecMode[Ac4CodecMode["AspxAcpl2"] = 4] = "AspxAcpl2";
    Ac4CodecMode[Ac4CodecMode["AspxAcpl3"] = 5] = "AspxAcpl3";
    Ac4CodecMode[Ac4CodecMode["Scpl"] = 6] = "Scpl";
    Ac4CodecMode[Ac4CodecMode["AspxScpl"] = 7] = "AspxScpl";
    Ac4CodecMode[Ac4CodecMode["AspxAjcc"] = 8] = "AspxAjcc";
})(Ac4CodecMode || (Ac4CodecMode = {}));
/** iclforge::ac4::RateMode's own numeric order (encoder.hpp) - kept in sync by hand. */
export var Ac4RateMode;
(function (Ac4RateMode) {
    Ac4RateMode[Ac4RateMode["Constant"] = 0] = "Constant";
    Ac4RateMode[Ac4RateMode["Average"] = 1] = "Average";
    Ac4RateMode[Ac4RateMode["Variable"] = 2] = "Variable";
})(Ac4RateMode || (Ac4RateMode = {}));
/** iclforge::ac4::BedChannel's own codes, Part 2 Table 66's nonstd_bed_channel_assignment (encoder.hpp) - kept in sync by hand. */
export var Ac4BedChannel;
(function (Ac4BedChannel) {
    Ac4BedChannel[Ac4BedChannel["Left"] = 0] = "Left";
    Ac4BedChannel[Ac4BedChannel["Right"] = 1] = "Right";
    Ac4BedChannel[Ac4BedChannel["Centre"] = 2] = "Centre";
    Ac4BedChannel[Ac4BedChannel["LeftSurround"] = 4] = "LeftSurround";
    Ac4BedChannel[Ac4BedChannel["RightSurround"] = 5] = "RightSurround";
    Ac4BedChannel[Ac4BedChannel["LeftBack"] = 6] = "LeftBack";
    Ac4BedChannel[Ac4BedChannel["RightBack"] = 7] = "RightBack";
    Ac4BedChannel[Ac4BedChannel["TopFrontLeft"] = 8] = "TopFrontLeft";
    Ac4BedChannel[Ac4BedChannel["TopFrontRight"] = 9] = "TopFrontRight";
    Ac4BedChannel[Ac4BedChannel["TopSideLeft"] = 10] = "TopSideLeft";
    Ac4BedChannel[Ac4BedChannel["TopSideRight"] = 11] = "TopSideRight";
    Ac4BedChannel[Ac4BedChannel["TopBackLeft"] = 12] = "TopBackLeft";
    Ac4BedChannel[Ac4BedChannel["TopBackRight"] = 13] = "TopBackRight";
    Ac4BedChannel[Ac4BedChannel["LeftWide"] = 14] = "LeftWide";
    Ac4BedChannel[Ac4BedChannel["RightWide"] = 15] = "RightWide";
})(Ac4BedChannel || (Ac4BedChannel = {}));
/** iclforge::ac4::ObjectCoding's own numeric order (encoder.hpp) - kept in sync by hand. */
export var Ac4ObjectCoding;
(function (Ac4ObjectCoding) {
    /** An A-JOC substream (Part 2 clause 5.7): a downmix and the matrices that rebuild the objects from it. */
    Ac4ObjectCoding[Ac4ObjectCoding["Ajoc"] = 0] = "Ajoc";
    /** Direct-coded object substreams (clause 6.2.1.11): dynamic objects and the LFE, no bed objects. */
    Ac4ObjectCoding[Ac4ObjectCoding["Direct"] = 1] = "Direct";
})(Ac4ObjectCoding || (Ac4ObjectCoding = {}));
/** iclforge::ac4::AjocDownmix's own numeric order (encoder.hpp) - kept in sync by hand. */
export var Ac4AjocDownmix;
(function (Ac4AjocDownmix) {
    /** Downmix signals the encoder computes, each the sum of a group of objects. */
    Ac4AjocDownmix[Ac4AjocDownmix["Computed"] = 0] = "Computed";
    /** A static 5.0 bed the objects are panned onto by X and Y (no LFE object). */
    Ac4AjocDownmix[Ac4AjocDownmix["Static50"] = 1] = "Static50";
    /** A static 5.1 bed, the LFE object onto the LFE. */
    Ac4AjocDownmix[Ac4AjocDownmix["Static51"] = 2] = "Static51";
})(Ac4AjocDownmix || (Ac4AjocDownmix = {}));
/** iclforge::ac4::AdditionalPair's own numeric order (encoder.hpp, Part 1 Table 88) - kept in sync by hand. */
export var Ac4AdditionalPair;
(function (Ac4AdditionalPair) {
    Ac4AdditionalPair[Ac4AdditionalPair["None"] = 0] = "None";
    /** 3/4/0: Lb and Rb. */
    Ac4AdditionalPair[Ac4AdditionalPair["Back"] = 1] = "Back";
    /** 5/2/0: Lw and Rw. */
    Ac4AdditionalPair[Ac4AdditionalPair["Wide"] = 2] = "Wide";
    /** 3/2/2: Tfl and Tfr. */
    Ac4AdditionalPair[Ac4AdditionalPair["TopFront"] = 3] = "TopFront";
})(Ac4AdditionalPair || (Ac4AdditionalPair = {}));
async function loadEmscriptenGlue(glueUrl) {
    const source = await (await fetch(glueUrl)).text();
    // createIclForgeAc4Module is the MODULARIZE+EXPORT_NAME global the glue
    // defines when evaluated as a plain script (apps/wasm/CMakeLists.txt's
    // link options for iclforge_wasm_ac4) - re-exporting it is what makes the
    // Blob URL below `import`able, the same technique decoder-worker.ts uses
    // for createIclForgeModule.
    const blob = new Blob([source, "\nexport default createIclForgeAc4Module;\n"], {
        type: "text/javascript",
    });
    const blobUrl = URL.createObjectURL(blob);
    try {
        const namespace = (await import(/* webpackIgnore: true */ blobUrl));
        return namespace.default;
    }
    finally {
        URL.revokeObjectURL(blobUrl);
    }
}
/**
 * Fetches, loads and instantiates the AC-4 Embind module from `glueUrl`
 * (the compiled `iclforge_ac4.js`). `locateFile` is set so the glue's own
 * `.wasm` fetch resolves beside `glueUrl` rather than against the Blob URL
 * it was imported from - see this file's header comment.
 */
export async function loadAc4Module(glueUrl) {
    const factory = await loadEmscriptenGlue(glueUrl);
    return factory({ locateFile: (path) => new URL(path, glueUrl).href });
}
const DEFAULT_DECODER_OPTIONS = {};
const DEFAULT_PRESENTATION_CHOICE = {};
/** A thin, typed wrapper over the Embind `Ac4Decoder` class - see this file's header comment. */
export class Ac4Decoder {
    #native;
    #closed = false;
    constructor(module, options = DEFAULT_DECODER_OPTIONS) {
        const presentation = options.presentation ?? DEFAULT_PRESENTATION_CHOICE;
        this.#native = new module.Ac4Decoder(options.outputLevelDbfs ?? NaN, options.drc ?? Ac4DrcMode.Default, options.downmix ?? Ac4DownmixTarget.AsCoded, options.decodingMode ?? Ac4DecodingMode.Full, options.concealment ?? Ac4ConcealmentPolicy.None, presentation.presentationId ?? -1, presentation.index ?? -1, presentation.language ?? "", options.mdCompatLevel ?? 3);
    }
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
    decodeFrame(bytes) {
        return this.#native.decodeFrame(bytes);
    }
    /** Changes the output processing from the next frame (iclforge::ac4::Decoder::set_output()). */
    setOutput(options = {}) {
        this.#native.setOutput(options.outputLevelDbfs ?? NaN, options.drc ?? Ac4DrcMode.Default, options.headphones ?? false, options.dialogueEnhancementDb ?? 0, options.downmix ?? Ac4DownmixTarget.AsCoded, options.mixLfe ?? true, options.dialogueGainDb ?? 0, options.associatedGainDb ?? 0);
    }
    /** Changes which presentation is decoded from the next frame (iclforge::ac4::Decoder::set_presentation()). */
    setPresentation(choice = DEFAULT_PRESENTATION_CHOICE) {
        this.#native.setPresentation(choice.presentationId ?? -1, choice.index ?? -1, choice.language ?? "");
    }
    /** Forgets everything carried between frames (iclforge::ac4::Decoder::reset()). */
    reset() {
        this.#native.reset();
    }
    /** Why the last decodeFrame() failed, returned nothing or returned a concealed frame; empty otherwise. */
    get refusalReason() {
        return this.#native.refusalReason();
    }
    /** The decoder's delay at the output rate for the stream as last decoded. */
    get latencySamples() {
        return this.#native.latencySamples();
    }
    /** The presentations of the last frame read, in table-of-contents order; empty before one. */
    get presentations() {
        return this.#native.presentations();
    }
    /** Releases the underlying WASM object. Call when done - Embind instances are not garbage collected. */
    close() {
        if (this.#closed)
            return;
        this.#closed = true;
        this.#native.delete();
    }
}
/** A thin, typed wrapper over the Embind `Ac4Encoder` class - see this file's header comment. */
export class Ac4Encoder {
    #native;
    #closed = false;
    /**
     * A configuration the encoder refuses leaves no encoder to encode with:
     * {@link constructionError} says why, and encode()/flush() return no frames.
     */
    constructor(module, options = {}) {
        this.#native = new module.Ac4Encoder(options);
    }
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
    encode(channels, updates = []) {
        return this.#native.encode(channels, updates);
    }
    /** Ends the stream: returns the frames the delay still held. Takes no input after this. */
    flush() {
        return this.#native.flush();
    }
    /** Why the last encode()/flush() call produced no frames when some were expected; empty otherwise. */
    get error() {
        return this.#native.error();
    }
    /** Why the constructor made no encoder - the first rule the configuration breaks, or the option it could not read; empty once construction succeeded. */
    get constructionError() {
        return this.#native.constructionError();
    }
    /** The codec mode the stream is coded in: what Ac4CodecMode.Auto resolved to, never Auto itself. */
    get codecMode() {
        return this.#native.codecMode();
    }
    /** Samples of silence the encoder puts before the input, at the input's rate. */
    get delaySamples() {
        return this.#native.delaySamples();
    }
    /** The delay iclforge::ac4::Decoder adds on top of {@link delaySamples}, at the input's rate. */
    get decoderDelaySamples() {
        return this.#native.decoderDelaySamples();
    }
    /** The 'dac4' box for the stream as encoded so far (iclforge::ac4::build_dac4()); empty where {@link dac4Refusal} is non-empty. */
    buildDac4() {
        return this.#native.buildDac4();
    }
    /** Why buildDac4() has nothing to describe; empty once construction succeeded and every presentation can be described whole. */
    dac4Refusal() {
        return this.#native.dac4Refusal();
    }
    /** Releases the underlying WASM object. Call when done - Embind instances are not garbage collected. */
    close() {
        if (this.#closed)
            return;
        this.#closed = true;
        this.#native.delete();
    }
}
/** iclforge::ac4::sync_frame(): wraps `rawFrame` with the sync word, optional CRC and frame_size, for a raw .ac4 file or MPEG-2 TS. */
export function syncFrame(module, rawFrame, crc) {
    return module.syncFrame(rawFrame, crc);
}
//# sourceMappingURL=ac4.js.map