import type { Ac3ForgeEmbindModule, FoldOptions } from "./types.js";
export interface DecodedProgram {
    streamKind: string;
    sampleRate: number;
    channelCount: number;
    channelLabels: string[];
    /** Per channel, the whole file's PCM concatenated. */
    channels: Float32Array[];
    /** Per channel, one RMS value per ENERGY_BLOCK_SAMPLES-sample block. */
    energy: Float32Array[];
    energyBlockSize: number;
    /** 0, 1 or 2 channels depending on whether `fold` was requested - the real ac3::OutputStage fold, never a hand-rolled one. */
    fold: Float32Array[];
    durationSeconds: number;
    objectCount: number;
    objectLabels: string[];
    /** Per object: [x, y, z, gain_db, width, depth, height] repeated once per object-carrying frame. */
    objectPositions: Float32Array[];
    /** Per object, concatenated real JOC-reconstructed audio. */
    objectAudio: Float32Array[];
    objectFrameSize: number;
    objectFrameCount: number;
    objectStartSeconds: number;
}
export interface DecodeFileOptions {
    fold?: FoldOptions;
}
export declare function decodeFile(module: Ac3ForgeEmbindModule, bytes: Uint8Array, options?: DecodeFileOptions): DecodedProgram;
//# sourceMappingURL=decode-file.d.ts.map