// @ts-check
'use strict';

const { test, expect } = require('@playwright/test');

// Closes roadmap VX18(a): docs/platforms/wasm.md records channel count,
// sample rate, object count and "an object's position differs between two
// timestamps" as manual-only verification, because build-wasm only proves
// the module compiles. Roadmap UX5 replaced the demo's own bespoke Embind
// Decoder class with the published ac3forge-wasm-decoder package
// (js/src/decode-file.ts's decodeFile(), built on PushDecoder) - this test
// now drives THAT, the same call apps/wasm/demo.js itself makes, against the
// bundled 8-second, 3-object Atmos-in-DD+ fixture, and asserts the same real
// values a human previously checked by eye.
test('decodes the bundled Atmos-in-DD+ fixture with real, moving object positions', async ({
    page,
}) => {
    const pageErrors = [];
    page.on('pageerror', (error) => pageErrors.push(String(error)));

    // Relative on purpose: baseURL includes the demo's subdirectory (see
    // playwright.config.js), and a leading "/" would resolve back to the
    // server root.
    await page.goto('index.html');

    const result = await page.evaluate(async () => {
        // @ts-ignore - ./package/ is js/dist/, copied in alongside the
        // Emscripten build output (see this repo's build-wasm CI job).
        const { decodeFile, DownmixTarget } = await import('./package/index.js');
        // @ts-ignore - createAc3ForgeModule is the Emscripten MODULARIZE
        // factory ac3forge_decode.js attaches to window; see index.html's
        // plain <script> tag.
        const moduleInstance = await window.createAc3ForgeModule();
        const response = await fetch('assets/demo.ec3');
        const bytes = new Uint8Array(await response.arrayBuffer());

        let program;
        try {
            program = decodeFile(moduleInstance, bytes, {
                fold: { target: DownmixTarget.LoRo, applyDialnorm: true },
            });
        } catch (error) {
            return { ok: false, error: String(error && error.message) };
        }

        // [x, y, z, gain_db, width, depth, height] - decode-file.ts's own
        // fixed stride - copied out with Array.from immediately since
        // Float32Array views into decoder state are only valid until the
        // next push()/flush() call (js/src/push-decoder.ts's own contract);
        // decodeFile() has already finished, but its returned arrays are
        // plain copies either way.
        const stride = 7;
        const positions = Array.from(program.objectPositions[0] ?? []);
        const firstFrame = positions.slice(0, stride);
        const lastFrame = positions.slice(positions.length - stride, positions.length);

        return {
            ok: true,
            sampleRate: program.sampleRate,
            channelCount: program.channelCount,
            objectCount: program.objectCount,
            durationSeconds: program.durationSeconds,
            foldChannelCount: program.fold.length,
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
    // The real ac3::OutputStage Lo/Ro fold this test asked for.
    expect(result.foldChannelCount).toBe(2);

    // x, y, z are the first three values of each stride-wide frame. A real
    // decode of moving object metadata puts the first and last frame at
    // genuinely different positions - a decoder that silently returned a
    // static placement (or garbage) would not clear this bar reliably.
    const [x0, y0, z0] = result.firstFrame;
    const [x1, y1, z1] = result.lastFrame;
    const moved = Math.hypot(x1 - x0, y1 - y0, z1 - z0);
    expect(moved).toBeGreaterThan(0.01);

    expect(pageErrors).toEqual([]);
});
