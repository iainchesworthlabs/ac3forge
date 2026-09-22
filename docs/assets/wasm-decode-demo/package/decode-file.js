// The whole-file convenience helper: decodes an entire elementary-stream
// byte blob up front and returns everything the docs demo's scrub/solo/
// visualize experience needs. Built entirely on PushDecoder/scanStream -
// looping pushAccessUnit over every access unit scanStream finds - which is
// the point: this is a consumer of the one push-frame decode path, not a
// second implementation of it. A live/streaming caller that doesn't need
// arbitrary seeking has no reason to use this; it uses PushDecoder or
// Ac3ForgeDecoderNode directly instead.
import { PushDecoder, scanStream } from "./push-decoder.js";
// About 21ms at 48kHz - small enough for a visualization to feel responsive
// without recomputing energy on every animation frame. Ported from the old
// whole-file Embind Decoder's own kEnergyBlockSamples; this is presentation
// logic, not codec logic, so it belongs here rather than in C++.
const ENERGY_BLOCK_SAMPLES = 1024;
// x, y, z, gain_db, width, depth, height - ac3::oba::DisplayObject's own shape.
const OBJECT_POSITION_STRIDE = 7;
export function decodeFile(module, bytes, options = {}) {
    const scanned = scanStream(module, bytes);
    if (!scanned.ok) {
        throw new Error(scanned.error);
    }
    const decoder = new PushDecoder(module, options.fold);
    try {
        return accumulate(decoder, scanned.kind, scanned.sampleRate, bytes, scanned.accessUnits);
    }
    finally {
        decoder.close();
    }
}
function accumulate(decoder, streamKind, streamSampleRate, bytes, accessUnits) {
    const channelChunks = [];
    const foldChunks = [];
    let channelLabels = [];
    let sampleRate = streamSampleRate;
    let objectCount = 0;
    let objectLabels = [];
    const objectPositions = [];
    const objectAudioChunks = [];
    let objectStartFrameIndex = -1;
    let objectFrameSize = 0;
    let frameIndex = 0;
    for (const unit of accessUnits) {
        const slice = bytes.subarray(unit.offset, unit.offset + unit.length);
        const result = decoder.push(slice);
        if (!result.ok) {
            throw new Error(result.error);
        }
        if (result.holdBack) {
            continue;
        }
        sampleRate = result.sampleRate;
        if (channelLabels.length === 0)
            channelLabels = result.channelLabels;
        appendChannels(channelChunks, result.channelCount, result.frameSamples, (ch) => decoder.channel(ch));
        if (result.foldChannelCount > 0) {
            appendChannels(foldChunks, result.foldChannelCount, result.frameSamples, (ch) => decoder.fold(ch));
        }
        if (result.objectCount > 0) {
            if (objectCount === 0) {
                objectCount = result.objectCount;
                objectLabels = result.objectLabels;
                for (let i = 0; i < objectCount; i++) {
                    objectPositions.push([]);
                    objectAudioChunks.push([]);
                }
                objectStartFrameIndex = frameIndex;
                objectFrameSize = result.frameSamples;
            }
            for (let i = 0; i < Math.min(objectCount, result.objectCount); i++) {
                const frame = decoder.objectFrame(i);
                if (frame)
                    objectPositions[i].push(...frame.position);
                const audio = decoder.objectAudio(i);
                if (audio)
                    objectAudioChunks[i].push(Float32Array.from(audio));
            }
        }
        else if (objectCount > 0) {
            // A gap after objects started (or an arbitrary uploaded stream whose
            // OAMD stopped without the stream ending) - freeze each object's last
            // known position and pad its audio with silence, rather than
            // shortening the arrays and desyncing every later frame against
            // playback time. Same rule the original whole-file Embind Decoder's
            // apply_objects()/pad_object_audio_with_silence() used.
            for (let i = 0; i < objectCount; i++) {
                const positions = objectPositions[i];
                if (positions.length >= OBJECT_POSITION_STRIDE) {
                    positions.push(...positions.slice(positions.length - OBJECT_POSITION_STRIDE));
                }
                objectAudioChunks[i].push(new Float32Array(result.frameSamples));
            }
        }
        frameIndex++;
    }
    const flushed = decoder.flush();
    for (const entry of flushed) {
        if (!entry.holdBack && entry.channelCount === channelChunks.length) {
            appendChannels(channelChunks, entry.channelCount, entry.frameSamples, (ch) => decoder.flushedChannel(entry.flushIndex, ch));
        }
    }
    const channels = channelChunks.map(concat);
    const fold = foldChunks.map(concat);
    const energy = channels.map((pcm) => rmsBlocks(pcm, ENERGY_BLOCK_SAMPLES));
    const durationSeconds = channels.length > 0 ? channels[0].length / sampleRate : 0;
    const objectStartSeconds = objectStartFrameIndex <= 0 || objectFrameSize === 0
        ? 0
        : (objectStartFrameIndex * objectFrameSize) / sampleRate;
    return {
        streamKind,
        sampleRate,
        channelCount: channelLabels.length,
        channelLabels,
        channels,
        energy,
        energyBlockSize: ENERGY_BLOCK_SAMPLES,
        fold,
        durationSeconds,
        objectCount,
        objectLabels,
        objectPositions: objectPositions.map((values) => Float32Array.from(values)),
        objectAudio: objectAudioChunks.map(concat),
        objectFrameSize,
        objectFrameCount: objectCount > 0 ? objectPositions[0].length / OBJECT_POSITION_STRIDE : 0,
        objectStartSeconds,
    };
}
function appendChannels(chunks, count, frameSamples, read) {
    while (chunks.length < count)
        chunks.push([]);
    for (let ch = 0; ch < count; ch++) {
        const view = read(ch);
        // Copied out immediately: the view is only valid until the next
        // push()/flush() call on this PushDecoder (its own doc comment).
        chunks[ch].push(view ? Float32Array.from(view.subarray(0, frameSamples)) : new Float32Array(frameSamples));
    }
}
function concat(chunks) {
    const total = chunks.reduce((sum, chunk) => sum + chunk.length, 0);
    const out = new Float32Array(total);
    let offset = 0;
    for (const chunk of chunks) {
        out.set(chunk, offset);
        offset += chunk.length;
    }
    return out;
}
function rmsBlocks(samples, blockSize) {
    if (blockSize <= 0 || samples.length === 0)
        return new Float32Array(0);
    const blockCount = Math.ceil(samples.length / blockSize);
    const out = new Float32Array(blockCount);
    for (let block = 0; block < blockCount; block++) {
        const start = block * blockSize;
        const end = Math.min(start + blockSize, samples.length);
        let sumSquares = 0;
        for (let i = start; i < end; i++)
            sumSquares += samples[i] * samples[i];
        out[block] = Math.sqrt(sumSquares / (end - start));
    }
    return out;
}
//# sourceMappingURL=decode-file.js.map