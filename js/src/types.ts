// Shared types for the package - the "typed" half of "typed ES module
// package": everything a consumer touches has a real interface instead of
// the `any` an untyped Embind wrapper would otherwise force on callers.

/** ac3::DownmixTarget's own numeric order (output.hpp) - kept in sync by hand, there being only four values. */
export enum DownmixTarget {
  AsCoded = 0,
  LoRo = 1,
  LtRt = 2,
  Mono = 3,
}

/** Which of PushDecoder's outputs feeds the AudioWorklet ring buffer - see decoder-worker.ts/decoder-node.ts. */
export type WriteTarget = "channels" | "fold";

export interface FoldOptions {
  target: DownmixTarget;
  /** §5.4.2.8 dialnorm normalisation - implied by nothing else, must be asked for explicitly. */
  applyDialnorm?: boolean;
  /** §7.8's optional LFE contribution to the fold. */
  mixLfe?: boolean;
}

export interface PushMetadata {
  sampleRate: number;
  /** Samples per channel this call wrote - 256/512/768/1536 depending on numblkscod. */
  frameSamples: number;
  dialnorm: number;
  channelCount: number;
  channelLabels: string[];
  /** 0 when no fold was requested at construction. */
  foldChannelCount: number;
  objectCount: number;
  objectLabels: string[];
}

export type PushOutcome =
  | { ok: true; holdBack: true }
  | ({ ok: true; holdBack: false } & PushMetadata)
  | { ok: false; error: string };

export interface FlushEntry {
  sampleRate: number;
  frameSamples: number;
  dialnorm: number;
  channelCount: number;
  channelLabels: string[];
  flushIndex: number;
}

export interface ObjectFrame {
  label: string;
  /** [x, y, z, gain_db, width, depth, height] - see ac3::oba::DisplayObject's own comment. */
  position: Float32Array;
}

export type ScanOutcome =
  | { ok: true; kind: string; sampleRate: number; accessUnits: readonly { offset: number; length: number }[] }
  | { ok: false; error: string };

/** The Embind class ac3::forge's WASM build exposes (apps/wasm/decoder_bindings.cpp's `PushDecoder`). */
export interface NativePushDecoder {
  pushAccessUnit(bytes: Uint8Array): RawPushResult;
  flush(): RawFlushEntry[];
  flushedChannelPcm(index: number, channel: number): Float32Array | null;
  channelPcm(channel: number): Float32Array | null;
  foldChannelCount(): number;
  foldPcm(channel: number): Float32Array | null;
  objectCount(): number;
  objectPosition(object: number): Float32Array | null;
  objectAudioPcm(object: number): Float32Array | null;
  objectLabel(object: number): string;
  delete(): void;
}

export interface RawPushResult {
  ok: boolean;
  error?: string;
  holdBack?: boolean;
  sampleRate?: number;
  frameSamples?: number;
  dialnorm?: number;
  channelCount?: number;
  channelLabels?: string[];
  foldChannelCount?: number;
  objectCount?: number;
  objectLabels?: string[];
}

export interface RawFlushEntry {
  ok: boolean;
  holdBack: boolean;
  sampleRate: number;
  frameSamples: number;
  dialnorm: number;
  channelCount: number;
  channelLabels: string[];
  objectCount: number;
  flushIndex: number;
}

export interface RawScanResult {
  ok: boolean;
  error?: string;
  kind?: string;
  sampleRate?: number;
  accessUnits?: { offset: number; length: number }[];
}

/** The Embind module `apps/wasm/decoder_bindings.cpp` builds - what `createAc3ForgeModule()` resolves to. */
export interface Ac3ForgeEmbindModule {
  PushDecoder: new (foldTarget: number, foldApplyDialnorm: boolean, foldMixLfe: boolean) => NativePushDecoder;
  scanStream(bytes: Uint8Array): RawScanResult;
}

/** The MODULARIZE factory Emscripten attaches as `createAc3ForgeModule` - see apps/wasm/CMakeLists.txt's link options. */
export type Ac3ForgeModuleFactory = (moduleOverrides?: Record<string, unknown>) => Promise<Ac3ForgeEmbindModule>;
