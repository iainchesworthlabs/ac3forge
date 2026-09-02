// @ts-check
'use strict';

const path = require('path');
const { defineConfig } = require('@playwright/test');

// roadmap VX18(a): headless-browser coverage for the WASM demos -
// docs/platforms/wasm.md's "Not yet verified" section names every
// functional claim this closes. WASM_DEMO_DIR/WASM_ENCODE_DEMO_DIR point at
// the directories apps/wasm/CMakeLists.txt's build produces
// (${CMAKE_BINARY_DIR}/bin/wasm_decode_demo/, .../wasm_encode_demo/);
// _build.yml's build-wasm job sets both. Defaulting to the
// config-wasm-emscripten preset's own build tree keeps `npx playwright test`
// runnable straight from a local build with no extra flags.
const decodeDemoDir = path.resolve(
    process.env.WASM_DEMO_DIR ||
        path.join(__dirname, '../../../build/config-wasm-emscripten/bin/wasm_decode_demo'));
const encodeDemoDir = path.resolve(
    process.env.WASM_ENCODE_DEMO_DIR ||
        path.join(__dirname, '../../../build/config-wasm-emscripten/bin/wasm_encode_demo'));

const decodePort = 4173;
const encodePort = 4174;
const serveScript = path.join(__dirname, 'serve.js');

// Each demo is served from its *parent* directory and navigated to under its
// directory name, mirroring the published docs site (which embeds both demos
// as docs/assets/wasm-*-demo/ subdirectories) rather than the
// demo-dir-as-server-root layout of the local serve instructions. Root
// serving masks a whole bug class: a parent-relative asset path (the
// "../ac3forge_decode.js" the encode page once used) clamps at the origin
// when the demo dir is the root, so it resolves anyway - and only 404s, as
// on the docs site, once the page actually sits in a subdirectory.
const decodeBase = `http://127.0.0.1:${decodePort}/${path.basename(decodeDemoDir)}/`;
const encodeBase = `http://127.0.0.1:${encodePort}/${path.basename(encodeDemoDir)}/`;

module.exports = defineConfig({
    testDir: __dirname,
    timeout: 30_000,
    retries: process.env.CI ? 1 : 0,
    reporter: process.env.CI ? 'line' : 'list',
    // Two independent static servers, one per demo directory - each demo is
    // meant to be independently servable (see apps/wasm/CMakeLists.txt's own
    // "any static file server" framing for each), so the test harness serves
    // them the same way rather than assuming one directory nests the other.
    webServer: [
        {
            command: `node "${serveScript}" "${path.dirname(decodeDemoDir)}" ${decodePort}`,
            url: `${decodeBase}index.html`,
            reuseExistingServer: !process.env.CI,
            timeout: 30_000,
        },
        {
            command: `node "${serveScript}" "${path.dirname(encodeDemoDir)}" ${encodePort}`,
            url: `${encodeBase}index.html`,
            reuseExistingServer: !process.env.CI,
            timeout: 30_000,
        },
    ],
    projects: [
        {
            name: 'decode',
            testMatch: 'decode.spec.js',
            use: { baseURL: decodeBase },
        },
        {
            name: 'encode',
            testMatch: 'encode.spec.js',
            use: {
                baseURL: encodeBase,
                // The microphone-capture test needs a microphone: Chromium's
                // fake media stack supplies a synthetic device (a tone) and
                // auto-grants the getUserMedia permission prompt, so the test
                // exercises the real capture path headlessly.
                launchOptions: {
                    args: [
                        '--use-fake-ui-for-media-stream',
                        '--use-fake-device-for-media-stream',
                    ],
                },
            },
        },
        {
            // The Atmos authoring page ships as a subdirectory of the encode
            // demo (see apps/wasm/CMakeLists.txt), so it is served by the
            // same server under the same base URL.
            name: 'atmos',
            testMatch: 'atmos.spec.js',
            use: { baseURL: encodeBase },
        },
    ],
});
