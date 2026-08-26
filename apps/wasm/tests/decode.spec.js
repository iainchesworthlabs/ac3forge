// @ts-check
'use strict';

const { test, expect } = require('@playwright/test');

// Closes roadmap VX18(a): docs/platforms/wasm.md records channel count,
// sample rate, object count and "an object's position differs between two
// timestamps" as manual-only verification, because build-wasm only proves
// the module compiles. This drives the real Embind API
// (apps/wasm/decoder_bindings.cpp's WasmDecoder class, called the same way
// apps/wasm/demo.js does) against the bundled 8-second, 3-object
// Atmos-in-DD+ fixture and asserts the same real values a human previously
// checked by eye.
test('decodes the bundled Atmos-in-DD+ fixture with real, moving object positions', async ({
    page,
}) => {
    const pageErrors = [];
    page.on('pageerror', (error) => pageErrors.push(String(error)));

    await page.goto('/index.html');

    const result = await page.evaluate(async () => {
        // @ts-ignore - createAc3ForgeModule is the Emscripten MODULARIZE
        // factory ac3forge_decode.js attaches to window; see index.html's
        // two plain <script> tags.
        const moduleInstance = await window.createAc3ForgeModule();
        const response = await fetch('assets/demo.ec3');
        const bytes = new Uint8Array(await response.arrayBuffer());

        const decoder = new moduleInstance.Decoder();
        const ok = decoder.decode(bytes);
        if (!ok) {
            const error = decoder.error();
            decoder.delete();
            return { ok: false, error };
        }

        const sampleRate = decoder.sampleRate();
        const channelCount = decoder.channelCount();
        const objectCount = decoder.objectCount();
        const frameSize = decoder.objectFrameSize();
        const frameCount = decoder.objectFrameCount();
        const startSeconds = decoder.objectStartSeconds();
        const durationSeconds = startSeconds + (frameCount * frameSize) / sampleRate;

        // The full per-frame time series for object 0, [x,y,z,gain_db,width,
        // depth,height] repeated frameCount times - copied out with
        // Array.from immediately, since the view is only valid until the
        // next call into the module (see decoder_bindings.cpp's own note on
        // this - object positions/audio are zero-copy heap views).
        const positions = Array.from(decoder.objectPositions(0));
        const stride = frameCount > 0 ? positions.length / frameCount : 0;
        const firstFrame = positions.slice(0, stride);
        const lastFrame = positions.slice(positions.length - stride, positions.length);

        decoder.delete();
        return {
            ok: true,
            sampleRate,
            channelCount,
            objectCount,
            durationSeconds,
            stride,
            firstFrame,
            lastFrame,
        };
    });

    expect(result.ok, `decode failed: ${result.error}`).toBe(true);
    // docs/platforms/wasm.md: "E-AC-3, 48000 Hz, 6 ch (L, C, R, Ls, Rs,
    // LFE), 3 Atmos object(s), 8.0s".
    expect(result.sampleRate).toBe(48000);
    expect(result.channelCount).toBe(6);
    expect(result.objectCount).toBe(3);
    expect(result.durationSeconds).toBeCloseTo(8.0, 1);

    // x, y, z are the first three values of each stride-wide frame. A real
    // decode of moving object metadata puts the first and last frame at
    // genuinely different positions - a decoder that silently returned a
    // static placement (or garbage) would not clear this bar reliably.
    expect(result.stride).toBeGreaterThanOrEqual(3);
    const [x0, y0, z0] = result.firstFrame;
    const [x1, y1, z1] = result.lastFrame;
    const moved = Math.hypot(x1 - x0, y1 - y0, z1 - z0);
    expect(moved).toBeGreaterThan(0.01);

    expect(pageErrors).toEqual([]);
});
