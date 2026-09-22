// @ts-check
'use strict';

const { test, expect } = require('@playwright/test');

// Coverage for the Atmos object-authoring page (apps/wasm/atmos/), the UX6
// piece that turns AtmosBedEncoder from "bound but nothing drives it" into a
// working authoring surface. Two layers, same philosophy as encode.spec.js:
// drive the real Embind API end to end with assertions on computed values,
// and separately drive the real page UI the way a person would.

test('AtmosBedEncoder authors a moving object and it round-trip decodes', async ({ page }) => {
    const pageErrors = [];
    page.on('pageerror', (error) => pageErrors.push(String(error)));

    await page.goto('atmos/index.html');

    const result = await page.evaluate(async () => {
        // @ts-ignore - the Emscripten MODULARIZE factories the page's own
        // <script> tags load (from the parent demo directory).
        const encodeModule = await window.createAc3ForgeEncodeModule();
        // @ts-ignore
        const decodeModule = await window.createAc3ForgeModule();

        const sampleRate = 48000;
        const frameSamples = 1536;
        const frames = 32; // ~1s - enough for the JOC delay line and a full pan
        const objectCount = 2;

        const encoder = new encodeModule.AtmosBedEncoder(sampleRate, 640, objectCount);

        const chunks = [];
        let totalBytes = 0;
        let phase0 = 0;
        let phase1 = 0;
        for (let f = 0; f < frames; f++) {
            const signal0 = new Float32Array(frameSamples);
            const signal1 = new Float32Array(frameSamples);
            for (let n = 0; n < frameSamples; n++) {
                signal0[n] = 0.35 * Math.sin(phase0);
                signal1[n] = 0.35 * Math.sin(phase1);
                phase0 += (2 * Math.PI * 220) / sampleRate;
                phase1 += (2 * Math.PI * 440) / sampleRate;
            }
            // Object 0 pans left-to-right across the run; object 1 rises
            // floor-to-ceiling - each frame's placement is different, which
            // is the whole point of the authoring surface.
            const t = f / (frames - 1);
            const placements = [
                { x: t, y: 0.25, z: 0, gain: 0.5 },
                { x: 0.5, y: 0.75, z: t, gain: 0.5 },
            ];
            const bytes = encoder.encodeFrame([signal0, signal1], placements);
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
            return { ok: false, stage: 'scan', error: scanned.error };
        }
        const decoder = new decodeModule.PushDecoder(0, false, false);
        let decodedChannelCount = 0;
        let decodedSampleRate = 0;
        // Per-channel energy of the LAST decoded frame: by then object 0 has
        // panned hard right, so the right-side output must carry more energy
        // than the left - a real assertion on the *rendered pan*, not just
        // "it decoded".
        let lastFrameEnergy = [];
        for (const unit of scanned.accessUnits) {
            const pushed = decoder.pushAccessUnit(
                streamBytes.slice(unit.offset, unit.offset + unit.length));
            if (!pushed.ok) {
                decoder.delete();
                return { ok: false, stage: 'decode', error: pushed.error };
            }
            if (pushed.holdBack) continue;
            decodedChannelCount = pushed.channelCount;
            decodedSampleRate = pushed.sampleRate;
            const energy = [];
            for (let ch = 0; ch < pushed.channelCount; ch++) {
                const pcm = decoder.channelPcm(ch);
                let sum = 0;
                for (let n = 0; n < pcm.length; n++) sum += pcm[n] * pcm[n];
                energy.push(sum);
            }
            lastFrameEnergy = energy;
        }
        decoder.delete();

        return {
            ok: true,
            totalBytes,
            accessUnits: scanned.accessUnits.length,
            decodedChannelCount,
            decodedSampleRate,
            lastFrameEnergy,
        };
    });

    expect(result.ok, `${result.stage} failed: ${result.error}`).toBe(true);
    expect(result.accessUnits).toBe(32);
    expect(result.totalBytes).toBeGreaterThan(32 * 100);
    expect(result.decodedSampleRate).toBe(48000);
    // The decode module renders the objects onto the 5.1 bed.
    expect(result.decodedChannelCount).toBe(6);

    // Table 5.8 order for 3/2+LFE: L, C, R, Ls, Rs, LFE. Object 0 ends the
    // run at x=1.0 (hard right), so R must out-carry L decisively.
    const left = result.lastFrameEnergy[0];
    const right = result.lastFrameEnergy[2];
    expect(right).toBeGreaterThan(left * 4);

    expect(pageErrors).toEqual([]);
});

test('the authoring page encodes a session from its own UI', async ({ page }) => {
    const pageErrors = [];
    page.on('pageerror', (error) => pageErrors.push(String(error)));

    await page.goto('atmos/index.html');
    await expect(page.locator('#status')).toContainText('Ready', { timeout: 15_000 });

    // Start a session, let the real-time cadence produce some frames (the
    // orbit animation is on by default, so the stream carries real motion),
    // then stop and check a stream came out.
    await page.click('#startBtn');
    await expect(page.locator('#status')).toContainText('Encoding 2 object(s)', {
        timeout: 10_000,
    });
    await page.waitForTimeout(1500);
    await page.click('#startBtn');

    await expect(page.locator('#status')).toContainText('Authored');
    await expect(page.locator('#status')).toContainText('bytes');
    await expect(page.locator('#resultPanel')).toBeVisible();

    expect(pageErrors).toEqual([]);
});
