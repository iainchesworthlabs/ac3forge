// @ts-check
'use strict';

const { test, expect } = require('@playwright/test');

// Roadmap UX5's AudioWorklet pipeline (js/src/decoder-node.ts,
// decoder-worker.ts, worklet-processor.ts, ring-buffer.ts): a real
// AudioWorkletNode, backed by a real Worker doing the WASM decode and a
// real SharedArrayBuffer ring buffer between them - not just "the worker
// didn't throw". Renders through an OfflineAudioContext for deterministic
// output: the Worker decodes independently of playback-thread pacing (it
// writes into the ring buffer as fast as it can, not in real time), so
// waiting for decoderNode.whenIdle() before startRendering() is what makes
// this reproducible rather than racing the Worker.
test('streams the bundled fixture through a real AudioWorkletNode and produces non-silent audio', async ({
    page,
}) => {
    const pageErrors = [];
    page.on('pageerror', (error) => pageErrors.push(String(error)));

    // Relative, and asserted: baseURL already includes the demo's own
    // subdirectory (see playwright.config.js), so a leading "/" would land on
    // the server root instead - where there is no index.html, and where
    // serve.js's 404 path sends no COOP/COEP headers, so the failure surfaced
    // as "not cross-origin isolated" rather than "wrong URL". Checking the
    // navigation response keeps that misdirection from happening again.
    const response = await page.goto('index.html');
    expect(response?.status(), 'the demo page itself must load').toBe(200);

    const result = await page.evaluate(async () => {
        if (!window.crossOriginIsolated) {
            return { ok: false, error: 'page is not cross-origin isolated (COOP/COEP headers missing)' };
        }
        // @ts-ignore
        const { Ac3ForgeDecoderNode, scanStream, DownmixTarget } = await import('./package/index.js');
        // @ts-ignore
        const moduleInstance = await window.createAc3ForgeModule();
        const response = await fetch('assets/demo.ec3');
        const bytes = new Uint8Array(await response.arrayBuffer());

        const scanned = scanStream(moduleInstance, bytes);
        if (!scanned.ok) return { ok: false, error: scanned.error };

        // Only the first ~1s of the fixture - enough to prove real decoded
        // audio reaches the worklet output without waiting for the whole
        // 8-second file.
        const unitsToPush = [];
        let samples = 0;
        for (const unit of scanned.accessUnits) {
            unitsToPush.push(unit);
            samples += 1536;
            if (samples >= scanned.sampleRate) break;
        }

        const offlineCtx = new OfflineAudioContext(2, scanned.sampleRate * 1, scanned.sampleRate);
        const node = await Ac3ForgeDecoderNode.create(offlineCtx, {
            workletProcessorUrl: new URL('./package/worklet-processor.js', location.href),
            workerUrl: new URL('./package/decoder-worker.js', location.href),
            wasmGlueUrl: new URL('./ac3forge_decode.js', location.href),
            fold: { target: DownmixTarget.LoRo, applyDialnorm: true },
        });
        node.node.connect(offlineCtx.destination);

        let underrunCount = 0;
        node.addEventListener('underrun', () => { underrunCount++; });
        let sawStreamInfo = false;
        let streamInfo = null;
        node.addEventListener('streaminfo', (e) => { sawStreamInfo = true; streamInfo = e.detail; });

        for (const unit of unitsToPush) {
            node.pushAccessUnit(bytes.slice(unit.offset, unit.offset + unit.length));
        }
        await node.whenIdle();

        const rendered = await offlineCtx.startRendering();
        const left = rendered.getChannelData(0);
        const right = rendered.getChannelData(1);
        let sumSquares = 0;
        for (let i = 0; i < left.length; i++) sumSquares += left[i] * left[i] + right[i] * right[i];
        const rms = Math.sqrt(sumSquares / (left.length * 2));

        node.close();
        return { ok: true, sawStreamInfo, streamInfo, rms, underrunCount, length: rendered.length };
    });

    expect(result.ok, `worklet pipeline failed: ${result.error}`).toBe(true);
    expect(result.sawStreamInfo).toBe(true);
    expect(result.streamInfo.sampleRate).toBe(48000);
    // The real E-AC-3 bed decoded and folded to Lo/Ro, not silence: this
    // fixture's bed is comfortably above this floor throughout its first
    // second (see docs/platforms/wasm.md's own RMS-based checks elsewhere).
    expect(result.rms).toBeGreaterThan(0.01);

    expect(pageErrors).toEqual([]);
});
