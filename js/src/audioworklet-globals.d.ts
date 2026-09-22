// lib.dom.d.ts does not declare AudioWorkletGlobalScope's own globals -
// AudioWorkletProcessor, registerProcessor(), and the current-time/sample
// globals - because that scope is neither `Window` nor a Worker scope (the
// two lib sets TypeScript ships). This file supplies exactly the pieces
// worklet-processor.ts needs; the DOM lib's AudioWorkletNodeOptions type
// (used for AudioWorkletNode construction on the main thread) is reused
// as-is rather than redeclared here.

declare abstract class AudioWorkletProcessor {
  readonly port: MessagePort;
  constructor(options?: AudioWorkletNodeOptions);
  abstract process(
    inputs: Float32Array[][],
    outputs: Float32Array[][],
    parameters: Record<string, Float32Array>,
  ): boolean;
}

declare function registerProcessor(
  name: string,
  processorCtor: new (options?: AudioWorkletNodeOptions) => AudioWorkletProcessor,
): void;

declare const sampleRate: number;
declare const currentFrame: number;
declare const currentTime: number;
