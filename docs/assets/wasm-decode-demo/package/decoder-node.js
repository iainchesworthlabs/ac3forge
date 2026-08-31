// Main-thread orchestration for the realtime pipeline: wires an
// AudioWorkletNode (worklet-processor.ts), a decode Worker
// (decoder-worker.ts) and the SharedArrayBuffer ring buffer connecting them
// into one object with a push-frame surface, so a consumer never touches
// the Worker/SAB/worklet plumbing directly.
import { allocateRingBuffer } from "./ring-buffer.js";
import { DownmixTarget } from "./types.js";
function nextPowerOfTwo(value) {
    let n = 1;
    while (n < value)
        n *= 2;
    return n;
}
function resolveChannelCount(options) {
    if (options.channelCount)
        return options.channelCount;
    if (options.fold && options.fold.target !== DownmixTarget.AsCoded)
        return 2;
    throw new Error("Ac3ForgeDecoderNode: channelCount is required unless a non-AsCoded fold is given");
}
/**
 * Push-frame realtime decode + playback: construct once per stream, call
 * {@link pushAccessUnit} as access units arrive (from a container demuxer,
 * the hls.js bridge, or your own transport), connect {@link node} into a
 * Web Audio graph. Decoding happens in a Worker; only ring-buffer draining
 * happens on the audio rendering thread itself.
 */
export class Ac3ForgeDecoderNode extends EventTarget {
    node;
    #worker;
    #streamInfoSeen = false;
    #pending = 0;
    #idleWaiters = [];
    constructor(node, worker) {
        super();
        this.node = node;
        this.#worker = worker;
    }
    static async create(audioContext, options) {
        if (!crossOriginIsolated) {
            throw new Error("Ac3ForgeDecoderNode requires cross-origin isolation (COOP: same-origin, COEP: " +
                "require-corp) for SharedArrayBuffer - see js/README.md.");
        }
        const channelCount = resolveChannelCount(options);
        const capacityFrames = nextPowerOfTwo(Math.ceil(audioContext.sampleRate * (options.ringBufferSeconds ?? 2)));
        const layout = { channelCount, capacityFrames };
        const sab = allocateRingBuffer(layout);
        await audioContext.audioWorklet.addModule(options.workletProcessorUrl);
        const node = new AudioWorkletNode(audioContext, "ac3forge-pcm-source", {
            numberOfInputs: 0,
            numberOfOutputs: 1,
            outputChannelCount: [channelCount],
            processorOptions: { sab, layout },
        });
        const worker = new Worker(options.workerUrl, { type: "module" });
        const instance = new Ac3ForgeDecoderNode(node, worker);
        worker.addEventListener("message", (event) => instance.#onWorkerMessage(event.data));
        node.port.addEventListener("message", (event) => instance.#onProcessorMessage(event.data));
        node.port.start();
        const writeTarget = options.fold && options.fold.target !== DownmixTarget.AsCoded ? "fold" : "channels";
        const ready = new Promise((resolve) => {
            const onReady = (event) => {
                if (event.data?.type === "ready") {
                    worker.removeEventListener("message", onReady);
                    resolve();
                }
            };
            worker.addEventListener("message", onReady);
        });
        worker.postMessage({
            type: "init",
            glueUrl: String(options.wasmGlueUrl),
            fold: options.fold,
            sab,
            layout,
            writeTarget,
        });
        await ready;
        return instance;
    }
    /**
     * Decodes one access unit and streams its PCM into the audio graph.
     * `unit` is copied (once, here) into its own `ArrayBuffer` before being
     * transferred to the Worker - safe even when `unit` is a view sharing
     * memory with siblings (the hls.js bridge's fmp4-extracted samples all
     * share one fragment's buffer), at the cost of one copy per call.
     */
    pushAccessUnit(unit) {
        const bytes = unit.buffer.slice(unit.byteOffset, unit.byteOffset + unit.byteLength);
        this.#pending++;
        this.#worker.postMessage({ type: "push", bytes }, [bytes]);
    }
    /** §3.7's tail - call once after the last pushAccessUnit() for a stream. */
    flush() {
        this.#worker.postMessage({ type: "flush" });
    }
    /** Resolves once every pushAccessUnit() call so far has been acknowledged by the Worker. */
    async whenIdle() {
        if (this.#pending === 0)
            return;
        return new Promise((resolve) => this.#idleWaiters.push(resolve));
    }
    close() {
        this.#worker.postMessage({ type: "close" });
        this.#worker.terminate();
        this.node.disconnect();
    }
    #onWorkerMessage(data) {
        switch (data.type) {
            case "result": {
                this.#pending = Math.max(0, this.#pending - 1);
                if (this.#pending === 0) {
                    this.#idleWaiters.splice(0).forEach((resolve) => resolve());
                }
                const outcome = data.outcome;
                if (outcome.ok && !outcome.holdBack && !this.#streamInfoSeen) {
                    this.#streamInfoSeen = true;
                    const detail = {
                        sampleRate: outcome.sampleRate,
                        channelCount: outcome.channelCount,
                        channelLabels: outcome.channelLabels,
                        foldChannelCount: outcome.foldChannelCount,
                    };
                    this.dispatchEvent(new CustomEvent("streaminfo", { detail }));
                }
                if (!outcome.ok) {
                    this.dispatchEvent(new CustomEvent("error", { detail: outcome.error }));
                }
                const objectFrames = data.objectFrames;
                if (objectFrames && objectFrames.length > 0) {
                    this.dispatchEvent(new CustomEvent("objectframe", { detail: objectFrames }));
                }
                break;
            }
            case "overrun":
                this.dispatchEvent(new CustomEvent("overrun", { detail: data.framesDropped }));
                break;
            case "error":
                this.dispatchEvent(new CustomEvent("error", { detail: data.message }));
                break;
            default:
                break;
        }
    }
    #onProcessorMessage(data) {
        if (data.type === "underrun") {
            this.dispatchEvent(new CustomEvent("underrun", { detail: data.framesShort }));
        }
    }
}
//# sourceMappingURL=decoder-node.js.map