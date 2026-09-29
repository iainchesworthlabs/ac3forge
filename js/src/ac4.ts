// A typed wrapper over the AC-4 embind module apps/wasm/ac4_bindings.cpp
// builds (apps/wasm/CMakeLists.txt's `ac3forge_wasm_ac4` target,
// `-sEXPORT_NAME=createAc3ForgeAc4Module`) - the AC-4 counterpart of
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
// Two sentinel conventions cross into the native constructor/setter calls
// below - see ac4_bindings.cpp's own header comment for why neither AC-3
// binding had an existing convention to copy: NaN for "no output level set"
// (the wire value a JS `undefined` already coerces to for a `double`
// parameter, so passing either works), and -1 for "no presentation id/index
// chosen" (ints have no NaN of their own, and 0 is a real id/index).

/** ac4::DrcMode's own numeric order (decoder.hpp) - kept in sync by hand. */
export enum Ac4DrcMode {
  Off = 0,
  Default = 1,
  HomeTheatre = 2,
  FlatPanelTv = 3,
  PortableSpeakers = 4,
  PortableHeadphones = 5,
}

/** ac4::DownmixTarget's own numeric order (decoder.hpp) - kept in sync by hand. */
export enum Ac4DownmixTarget {
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
  FiveX2 = 10,
}

/** ac4::DecodingMode's own numeric order (decoder.hpp) - kept in sync by hand. */
export enum Ac4DecodingMode {
  Full = 0,
  Core = 1,
}

/** ac4::ConcealmentPolicy's own numeric order (decoder.hpp) - kept in sync by hand. */
export enum Ac4ConcealmentPolicy {
  None = 0,
  RepeatFade = 1,
  Mute = 2,
}

/** ac4::CodecMode's own numeric order (encoder.hpp) - kept in sync by hand. */
export enum Ac4CodecMode {
  Auto = 0,
  Simple = 1,
  Aspx = 2,
  AspxAcpl1 = 3,
  AspxAcpl2 = 4,
  AspxAcpl3 = 5,
  Scpl = 6,
  AspxScpl = 7,
  AspxAjcc = 8,
}

/** ac4::RateMode's own numeric order (encoder.hpp) - kept in sync by hand. */
export enum Ac4RateMode {
  Constant = 0,
  Average = 1,
  Variable = 2,
}

export interface Ac4PresentationChoice {
  /** ac4::PresentationChoice::presentation_id; unset (or omitted) selects by index/preference instead. */
  presentationId?: number;
  /** ac4::PresentationChoice::index, a position in the table of contents. */
  index?: number;
  /** An IETF BCP 47 tag; empty (the default) for none. */
  language?: string;
}

export interface Ac4OutputOptions {
  /** ac4::OutputConfig::output_level_dbfs; unset leaves the stream at its coded level. */
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
  /** ac4::DecoderConfig::level (md_compat); default 3, matching the C++ struct default. */
  mdCompatLevel?: number;
}

export interface Ac4EncoderOptions {
  channels?: number;
  sampleRateHz?: number;
  frameRateIndex?: number;
  bitrateKbps?: number;
  rateMode?: Ac4RateMode;
  codecMode?: Ac4CodecMode;
  iframeInterval?: number;
  dialnormDb?: number;
}

/** ac4::PresentationInfo, as Ac4Decoder.presentations() returns it. */
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
  /** Channel layout as strings (ac4::describe(Speaker)), the same convention decoder_bindings.cpp uses for AC-3. */
  speakers: string[];
}

export interface RawAc4Concealment {
  error: string;
  action: "repeatFade" | "mute";
}

export interface RawAc4ObjectProperties {
  active: boolean;
  gainDb: number;
  /** [x, y, z], Annex F.2. */
  position: [number, number, number];
  priority: number;
}

export interface RawAc4Object {
  kind: "bed" | "dyn" | "isf";
  lfe: boolean;
  /** A bed object's loudspeaker (ac4::describe(Speaker)); null otherwise. */
  speaker: string | null;
  samples: Float32Array;
  properties: RawAc4ObjectProperties;
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
  setOutput(
    outputLevelDbfs: number,
    drc: number,
    headphones: boolean,
    dialogueEnhancementDb: number,
    downmix: number,
    mixLfe: boolean,
    dialogueGainDb: number,
    associatedGainDb: number,
  ): void;
  setPresentation(presentationId: number, presentationIndex: number, language: string): void;
  reset(): void;
  refusalReason(): string;
  latencySamples(): number;
  presentations(): RawAc4Presentation[];
  delete(): void;
}

/** The Embind class ac4_bindings.cpp's `Ac4Encoder` builds. */
export interface NativeAc4Encoder {
  encode(channels: Float32Array[]): RawAc4EncodedFrame[];
  flush(): RawAc4EncodedFrame[];
  error(): string;
  codecMode(): number;
  delaySamples(): number;
  decoderDelaySamples(): number;
  buildDac4(): Uint8Array;
  dac4Refusal(): string;
  delete(): void;
}

/** The Embind module `apps/wasm/ac4_bindings.cpp` builds - what `createAc3ForgeAc4Module()` resolves to. */
export interface Ac4EmbindModule {
  Ac4Decoder: new (
    outputLevelDbfs: number,
    drc: number,
    downmix: number,
    decodingMode: number,
    concealment: number,
    presentationId: number,
    presentationIndex: number,
    language: string,
    level: number,
  ) => NativeAc4Decoder;
  Ac4Encoder: new (
    channels: number,
    sampleRateHz: number,
    frameRateIndex: number,
    bitrateKbps: number,
    rateMode: number,
    codecMode: number,
    iframeInterval: number,
    dialnormDb: number,
  ) => NativeAc4Encoder;
  syncFrame(rawFrame: Uint8Array, crc: boolean): Uint8Array;
}

/** The MODULARIZE factory Emscripten attaches as `createAc3ForgeAc4Module` - see apps/wasm/CMakeLists.txt's link options. */
export type Ac4ModuleFactory = (moduleOverrides?: Record<string, unknown>) => Promise<Ac4EmbindModule>;

async function loadEmscriptenGlue(glueUrl: string): Promise<Ac4ModuleFactory> {
  const source = await (await fetch(glueUrl)).text();
  // createAc3ForgeAc4Module is the MODULARIZE+EXPORT_NAME global the glue
  // defines when evaluated as a plain script (apps/wasm/CMakeLists.txt's
  // link options for ac3forge_wasm_ac4) - re-exporting it is what makes the
  // Blob URL below `import`able, the same technique decoder-worker.ts uses
  // for createAc3ForgeModule.
  const blob = new Blob([source, "\nexport default createAc3ForgeAc4Module;\n"], {
    type: "text/javascript",
  });
  const blobUrl = URL.createObjectURL(blob);
  try {
    const namespace = (await import(/* webpackIgnore: true */ blobUrl)) as { default: Ac4ModuleFactory };
    return namespace.default;
  } finally {
    URL.revokeObjectURL(blobUrl);
  }
}

/**
 * Fetches, loads and instantiates the AC-4 Embind module from `glueUrl`
 * (the compiled `ac3forge_ac4.js`). `locateFile` is set so the glue's own
 * `.wasm` fetch resolves beside `glueUrl` rather than against the Blob URL
 * it was imported from - see this file's header comment.
 */
export async function loadAc4Module(glueUrl: string): Promise<Ac4EmbindModule> {
  const factory = await loadEmscriptenGlue(glueUrl);
  return factory({ locateFile: (path: string) => new URL(path, glueUrl).href });
}

const DEFAULT_DECODER_OPTIONS: Ac4DecoderOptions = {};
const DEFAULT_PRESENTATION_CHOICE: Ac4PresentationChoice = {};

/** A thin, typed wrapper over the Embind `Ac4Decoder` class - see this file's header comment. */
export class Ac4Decoder {
  readonly #native: NativeAc4Decoder;
  #closed = false;

  constructor(module: Ac4EmbindModule, options: Ac4DecoderOptions = DEFAULT_DECODER_OPTIONS) {
    const presentation = options.presentation ?? DEFAULT_PRESENTATION_CHOICE;
    this.#native = new module.Ac4Decoder(
      options.outputLevelDbfs ?? NaN,
      options.drc ?? Ac4DrcMode.Default,
      options.downmix ?? Ac4DownmixTarget.AsCoded,
      options.decodingMode ?? Ac4DecodingMode.Full,
      options.concealment ?? Ac4ConcealmentPolicy.None,
      presentation.presentationId ?? -1,
      presentation.index ?? -1,
      presentation.language ?? "",
      options.mdCompatLevel ?? 3,
    );
  }

  /**
   * Decodes one raw_ac4_frame (an ac4::SyncFrame's raw_ac4_frame, or an MP4
   * sample - strip any container/sync-frame wrapper first). Null for a
   * frame with no output and for a decode error with no concealment
   * configured - {@link refusalReason} says why in either case.
   *
   * The channel/object Float32Arrays on the returned frame are zero-copy
   * views into the WASM heap, valid only until the next `decodeFrame()`/
   * `reset()` call on this instance - copy them out if you need them later.
   */
  decodeFrame(bytes: Uint8Array): RawAc4DecodedFrame | null {
    return this.#native.decodeFrame(bytes);
  }

  /** Changes the output processing from the next frame (ac4::Decoder::set_output()). */
  setOutput(options: Ac4OutputOptions = {}): void {
    this.#native.setOutput(
      options.outputLevelDbfs ?? NaN,
      options.drc ?? Ac4DrcMode.Default,
      options.headphones ?? false,
      options.dialogueEnhancementDb ?? 0,
      options.downmix ?? Ac4DownmixTarget.AsCoded,
      options.mixLfe ?? true,
      options.dialogueGainDb ?? 0,
      options.associatedGainDb ?? 0,
    );
  }

  /** Changes which presentation is decoded from the next frame (ac4::Decoder::set_presentation()). */
  setPresentation(choice: Ac4PresentationChoice = DEFAULT_PRESENTATION_CHOICE): void {
    this.#native.setPresentation(choice.presentationId ?? -1, choice.index ?? -1, choice.language ?? "");
  }

  /** Forgets everything carried between frames (ac4::Decoder::reset()). */
  reset(): void {
    this.#native.reset();
  }

  /** Why the last decodeFrame() failed, returned nothing or returned a concealed frame; empty otherwise. */
  get refusalReason(): string {
    return this.#native.refusalReason();
  }

  /** The decoder's delay at the output rate for the stream as last decoded. */
  get latencySamples(): number {
    return this.#native.latencySamples();
  }

  /** The presentations of the last frame read, in table-of-contents order; empty before one. */
  get presentations(): RawAc4Presentation[] {
    return this.#native.presentations();
  }

  /** Releases the underlying WASM object. Call when done - Embind instances are not garbage collected. */
  close(): void {
    if (this.#closed) return;
    this.#closed = true;
    this.#native.delete();
  }
}

const DEFAULT_ENCODER_OPTIONS: Required<Ac4EncoderOptions> = {
  channels: 2,
  sampleRateHz: 48000,
  frameRateIndex: 13,
  bitrateKbps: 192,
  rateMode: Ac4RateMode.Constant,
  codecMode: Ac4CodecMode.Auto,
  iframeInterval: 24,
  dialnormDb: -31,
};

/** A thin, typed wrapper over the Embind `Ac4Encoder` class - see this file's header comment. */
export class Ac4Encoder {
  readonly #native: NativeAc4Encoder;
  #closed = false;

  constructor(module: Ac4EmbindModule, options: Ac4EncoderOptions = {}) {
    const o = { ...DEFAULT_ENCODER_OPTIONS, ...options };
    this.#native = new module.Ac4Encoder(
      o.channels,
      o.sampleRateHz,
      o.frameRateIndex,
      o.bitrateKbps,
      o.rateMode,
      o.codecMode,
      o.iframeInterval,
      o.dialnormDb,
    );
  }

  /**
   * Planar samples at full scale 1.0, one Float32Array per input channel,
   * any length. Returns the frames this input completes, in order; the
   * encoder's delay holds back the frames the last input still needs. Each
   * returned frame's `data` is an owned copy (unlike decodeFrame()'s PCM
   * views) - see ac4_bindings.cpp's own comment on why.
   */
  encode(channels: Float32Array[]): RawAc4EncodedFrame[] {
    return this.#native.encode(channels);
  }

  /** Ends the stream: returns the frames the delay still held. Takes no input after this. */
  flush(): RawAc4EncodedFrame[] {
    return this.#native.flush();
  }

  /** Why the last encode()/flush() call produced no frames when some were expected; empty otherwise. */
  get error(): string {
    return this.#native.error();
  }

  /** The codec mode the stream is coded in: what Ac4CodecMode.Auto resolved to, never Auto itself. */
  get codecMode(): number {
    return this.#native.codecMode();
  }

  /** Samples of silence the encoder puts before the input, at the input's rate. */
  get delaySamples(): number {
    return this.#native.delaySamples();
  }

  /** The delay ac4::Decoder adds on top of {@link delaySamples}, at the input's rate. */
  get decoderDelaySamples(): number {
    return this.#native.decoderDelaySamples();
  }

  /** The 'dac4' box for the stream as encoded so far (ac4::build_dac4()); empty where {@link dac4Refusal} is non-empty. */
  buildDac4(): Uint8Array {
    return this.#native.buildDac4();
  }

  /** Why buildDac4() has nothing to describe; empty once construction succeeded and every presentation can be described whole. */
  dac4Refusal(): string {
    return this.#native.dac4Refusal();
  }

  /** Releases the underlying WASM object. Call when done - Embind instances are not garbage collected. */
  close(): void {
    if (this.#closed) return;
    this.#closed = true;
    this.#native.delete();
  }
}

/** ac4::sync_frame(): wraps `rawFrame` with the sync word, optional CRC and frame_size, for a raw .ac4 file or MPEG-2 TS. */
export function syncFrame(module: Ac4EmbindModule, rawFrame: Uint8Array, crc: boolean): Uint8Array {
  return module.syncFrame(rawFrame, crc);
}
