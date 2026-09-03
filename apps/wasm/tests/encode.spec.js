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

        // The same two-pass flow app.js runs: meter the whole programme
        // first, derive dialnorm from the measured integrated loudness, then
        // encode with that dialnorm in every frame's BSI.
        const qc = new encodeModule.QcMeter(layout, sampleRate);
        const spf = 1536;
        for (let start = 0; start < totalSamples; start += spf) {
            const end = Math.min(start + spf, totalSamples);
            qc.push([left.subarray(start, end), right.subarray(start, end)]);
        }
        const measured = qc.integratedLkfs();
        const dialnorm =
            measured === null || measured === undefined
                ? 31
                : Math.min(31, Math.max(1, Math.round(-measured)));

        const encoder = new encodeModule.Encoder(format, layout, sampleRate, bitrate, dialnorm);

        const chunks = [];
        let totalBytes = 0;
        for (let start = 0; start < totalSamples; start += spf) {
            const end = Math.min(start + spf, totalSamples);
            const slice = [left.subarray(start, end), right.subarray(start, end)];
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

        // Ported to UX5's scanStream()+PushDecoder surface (the whole-file
        // Decoder class no longer exists): push every scanned access unit
        // and take the format from the last non-held-back frame.
        const scanned = decodeModule.scanStream(streamBytes);
        if (!scanned.ok) {
            return { ok: false, stage: 'round-trip scan', error: scanned.error };
        }
        const decoder = new decodeModule.PushDecoder(0, false, false);
        let decodedSampleRate = 0;
        let decodedChannelCount = 0;
        for (const unit of scanned.accessUnits) {
            const result = decoder.pushAccessUnit(
                streamBytes.slice(unit.offset, unit.offset + unit.length));
            if (!result.ok) {
                decoder.delete();
                return { ok: false, stage: 'round-trip decode', error: result.error };
            }
            if (result.holdBack) continue;
            decodedSampleRate = result.sampleRate;
            decodedChannelCount = result.channelCount;
        }
        decoder.delete();
        if (decodedChannelCount === 0) {
            return { ok: false, stage: 'round-trip decode', error: 'no decodable frames' };
        }

        // dialnorm must actually land in the bitstream: the identical
        // first frame encoded with a different dialnorm has to produce
        // different bytes (BSI field + CRC), and the same dialnorm the same
        // bytes - a bitstream-level check that the constructor argument is
        // wired through, without needing a BSI parser in the test.
        const firstFrame = [left.subarray(0, spf), right.subarray(0, spf)];
        const encodeOne = (value) => {
            const one = new encodeModule.Encoder(format, layout, sampleRate, bitrate, value);
            const bytes = one.encodeFrame(firstFrame);
            return bytes ? Array.from(bytes) : null;
        };
        const frameAtMeasured = encodeOne(dialnorm);
        const frameAtDefault = encodeOne(31);
        const frameAtMeasuredAgain = encodeOne(dialnorm);
        if (!frameAtMeasured || !frameAtDefault || !frameAtMeasuredAgain) {
            return { ok: false, stage: 'dialnorm re-encode', error: 'encodeFrame failed' };
        }

        return {
            ok: true,
            totalBytes,
            dialnorm,
            dialnormChangesBytes:
                JSON.stringify(frameAtMeasured) !== JSON.stringify(frameAtDefault),
            dialnormDeterministic:
                JSON.stringify(frameAtMeasured) === JSON.stringify(frameAtMeasuredAgain),
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

    // The derived dialnorm is the page's own formula applied to the real
    // measurement (a ~-6 LKFS tone lands far from the default 31), and it
    // demonstrably changes the produced bytes - deterministically.
    expect(result.dialnorm).toBe(
        Math.min(31, Math.max(1, Math.round(-result.integratedLkfs))));
    expect(result.dialnorm).not.toBe(31);
    expect(result.dialnormChangesBytes).toBe(true);
    expect(result.dialnormDeterministic).toBe(true);

    expect(pageErrors).toEqual([]);
});

test('encodes a 12-channel 7.1.4 WAV through the wide-layout plan routing', async ({ page }) => {
    const pageErrors = [];
    page.on('pageerror', (error) => pageErrors.push(String(error)));

    await page.goto('index.html');

    const result = await page.evaluate(async () => {
        // @ts-ignore
        const encodeModule = await window.createAc3ForgeEncodeModule();
        // @ts-ignore
        const decodeModule = await window.createAc3ForgeModule();

        const sampleRate = 48000;
        const spf = 1536;
        const frames = 16;
        const totalSamples = spf * frames;
        const channelCount = 12; // reads as 7.1.4 (generic_wav_layout)
        const layout = 5; // WasmLayout::k7_1_4
        const format = 1; // E-AC-3 - the wide layouts are E-AC-3 only
        const bitrate = 384; // the docs' own 7.1.4 example rate

        // One distinct tone per channel, all phase-continuous.
        const channels = [];
        for (let ch = 0; ch < channelCount; ch++) {
            const out = new Float32Array(totalSamples);
            const step = (2 * Math.PI * (200 + 60 * ch)) / sampleRate;
            for (let i = 0; i < totalSamples; i++) out[i] = 0.25 * Math.sin(step * i);
            channels.push(out);
        }

        // meterOrderForWav must be a permutation of 0..11 - the module's own
        // statement of how a 12-channel WAV maps onto the BS.1770-5 rendered
        // location set.
        const meterOrder = encodeModule.QcMeter.meterOrderForWav(layout);
        const qc = new encodeModule.QcMeter(layout, sampleRate);
        qc.push(meterOrder.map((wavIndex) => channels[wavIndex]));
        const integrated = qc.integratedLkfs();

        const encoder = new encodeModule.Encoder(format, layout, sampleRate, bitrate, 27);
        const chunks = [];
        let totalBytes = 0;
        for (let start = 0; start < totalSamples; start += spf) {
            const frame = channels.map((c) => c.subarray(start, start + spf));
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

        const scanned = decodeModule.scanStream(streamBytes);
        if (!scanned.ok) {
            return { ok: false, stage: 'round-trip scan', error: scanned.error };
        }
        const decoder = new decodeModule.PushDecoder(0, false, false);
        let decodedChannelCount = 0;
        let decodedSampleRate = 0;
        for (const unit of scanned.accessUnits) {
            const pushed = decoder.pushAccessUnit(
                streamBytes.slice(unit.offset, unit.offset + unit.length));
            if (!pushed.ok) {
                decoder.delete();
                return { ok: false, stage: 'round-trip decode', error: pushed.error };
            }
            if (pushed.holdBack) continue;
            decodedChannelCount = pushed.channelCount;
            decodedSampleRate = pushed.sampleRate;
        }
        decoder.delete();

        return {
            ok: true,
            totalBytes,
            integratedLkfs: integrated,
            meterOrder: Array.from(meterOrder),
            sourceChannels: encoder.channelCount(),
            carried: encoder.sourceWasCarried(),
            decodedChannelCount,
            decodedSampleRate,
        };
    });

    expect(result.ok, `${result.stage} failed: ${result.error}`).toBe(true);

    // A 12-channel source aimed at 7.1.4 is a straight carry (the plan
    // routing's permutation case), and the routing fixes the source width.
    expect(result.sourceChannels).toBe(12);
    expect(result.carried).toBe(true);

    // The meter order is a real permutation of the 12 WAV slots.
    expect([...result.meterOrder].sort((a, b) => a - b)).toEqual(
        Array.from({ length: 12 }, (_, i) => i));

    // 16 frames of 12 loud tones: a real measured loudness, and real bytes.
    expect(result.integratedLkfs).not.toBeNull();
    expect(result.totalBytes).toBeGreaterThan(16 * 500);

    // The round trip decodes the full assembled program: an independent 5.1
    // bed plus dependent substreams carrying the wide/height channels.
    expect(result.decodedSampleRate).toBe(48000);
    expect(result.decodedChannelCount).toBeGreaterThanOrEqual(6);

    expect(pageErrors).toEqual([]);
});

test('records from the (fake) microphone through the page UI', async ({ page }) => {
    const pageErrors = [];
    page.on('pageerror', (error) => pageErrors.push(String(error)));

    await page.goto('index.html');
    await expect(page.locator('#status')).toContainText('Ready', { timeout: 15_000 });

    // Chromium's --use-fake-device-for-media-stream supplies a synthetic
    // tone as the microphone and --use-fake-ui-for-media-stream grants the
    // permission prompt (see playwright.config.js), so this drives the real
    // getUserMedia -> AudioWorklet -> Encoder path.
    await page.click('#micBtn');
    // The measure-only pre-roll ends ~1.5s in, when the encoder spins up and
    // the ticker starts reporting real encoded bytes.
    await expect(page.locator('#status')).toContainText('Recording', { timeout: 15_000 });
    await page.waitForTimeout(1200);
    await page.click('#micBtn'); // now "Stop and finish"

    await expect(page.locator('#status')).toContainText('Captured', { timeout: 10_000 });
    await expect(page.locator('#status')).toContainText('dialnorm');
    await expect(page.locator('#resultPanel')).toBeVisible();
    await expect(page.locator('#qcPanel')).toBeVisible();

    expect(pageErrors).toEqual([]);
});
