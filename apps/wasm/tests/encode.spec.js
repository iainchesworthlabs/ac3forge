// @ts-check
'use strict';

const { test, expect } = require('@playwright/test');

// Closes roadmap VX18(a) for the encode module the same way decode.spec.js
// closes it for the decode one: drives the real Embind API
// (apps/wasm/encoder_bindings.cpp's Encoder/QcMeter classes, called the same
// way apps/wasm/encode/app.js does) end to end - encode a real signal,
// measure it with the real BS.1770 QC meter, and round-trip it through the
// real decode module - and asserts on real, computed values rather than "no
// error was thrown".
test('encodes a known stereo tone, QC verdict matches it, and it round-trip decodes', async ({
    page,
}) => {
    const pageErrors = [];
    page.on('pageerror', (error) => pageErrors.push(String(error)));

    // Relative on purpose: baseURL includes the demo's subdirectory (see
    // playwright.config.js), and a leading "/" would resolve back to the
    // server root.
    await page.goto('index.html');

    const result = await page.evaluate(async () => {
        // @ts-ignore - both createAc3ForgeEncodeModule and createAc3ForgeModule
        // are the Emscripten MODULARIZE factories index.html's <script> tags
        // attach to window.
        const encodeModule = await window.createAc3ForgeEncodeModule();
        // @ts-ignore
        const decodeModule = await window.createAc3ForgeModule();

        const sampleRate = 48000;
        const seconds = 2;
        const totalSamples = sampleRate * seconds;
        const freq = 997; // standard test-tone frequency
        const amplitude = 0.5; // -6.02 dBFS peak, well inside [-1, 1)

        const makeTone = () => {
            const out = new Float32Array(totalSamples);
            for (let i = 0; i < totalSamples; i++) {
                out[i] = amplitude * Math.sin((2 * Math.PI * freq * i) / sampleRate);
            }
            return out;
        };
        const left = makeTone();
        const right = makeTone();

        const layout = 1; // stereo (encoder_bindings.cpp's WasmLayout)
        const format = 0; // AC-3
        const bitrate = 192;

        const encoder = new encodeModule.Encoder(format, layout, sampleRate, bitrate);
        const qc = new encodeModule.QcMeter(layout, sampleRate);
        const spf = encoder.samplesPerFrame();

        const chunks = [];
        let totalBytes = 0;
        for (let start = 0; start < totalSamples; start += spf) {
            const end = Math.min(start + spf, totalSamples);
            const slice = [left.subarray(start, end), right.subarray(start, end)];
            qc.push(slice);
            const frame =
                end - start === spf
                    ? slice
                    : slice.map((c) => {
                          const padded = new Float32Array(spf);
                          padded.set(c);
                          return padded;
                      });
            const bytes = encoder.encodeFrame(frame);
            if (!bytes) {
                return { ok: false, stage: 'encode', error: encoder.error() };
            }
            const copy = Uint8Array.from(bytes);
            chunks.push(copy);
            totalBytes += copy.length;
        }
        const streamBytes = new Uint8Array(totalBytes);
        let offset = 0;
        for (const chunk of chunks) {
            streamBytes.set(chunk, offset);
            offset += chunk.length;
        }

        const verdicts = qc.verdicts();
        const integratedLkfs = qc.integratedLkfs();
        const truePeakDbtp = qc.truePeakDbtp();

        const decoder = new decodeModule.Decoder();
        const decodeOk = decoder.decode(streamBytes);
        if (!decodeOk) {
            const error = decoder.error();
            decoder.delete();
            return { ok: false, stage: 'round-trip decode', error };
        }
        const decodedSampleRate = decoder.sampleRate();
        const decodedChannelCount = decoder.channelCount();
        decoder.delete();

        return {
            ok: true,
            totalBytes,
            verdicts,
            integratedLkfs,
            truePeakDbtp,
            decodedSampleRate,
            decodedChannelCount,
        };
    });

    expect(result.ok, `${result.stage} failed: ${result.error}`).toBe(true);
    expect(result.totalBytes).toBeGreaterThan(1000);

    // A 997 Hz tone at amplitude 0.5 has a true peak right at -6.02 dBTP -
    // a wide but real band, wide enough to tolerate the oversampled
    // reconstruction's own small deviation without being so wide it would
    // also accept a badly wrong measurement (e.g. one that forgot to
    // convert to dB, or read digital full scale).
    expect(result.truePeakDbtp).not.toBeNull();
    expect(result.truePeakDbtp).toBeGreaterThan(-8);
    expect(result.truePeakDbtp).toBeLessThan(-4);

    // 2s of continuous tone clears BS.1770's absolute gate well before the
    // programme ends, so integrated loudness is a real number, not the
    // "not enough signal yet" std::nullopt.
    expect(result.integratedLkfs).not.toBeNull();

    // This tone is far louder than every delivery preset's target/ceiling
    // (all in the -18 to -27 LKFS range) - if the QC gate could not tell
    // that apart from a compliant programme, it would not be checking
    // anything.
    expect(result.verdicts).toHaveLength(5);
    expect(result.verdicts.every((verdict) => verdict.pass === false)).toBe(true);

    expect(result.decodedSampleRate).toBe(48000);
    expect(result.decodedChannelCount).toBe(2);

    expect(pageErrors).toEqual([]);
});
