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
            command: `node "${serveScript}" "${decodeDemoDir}" ${decodePort}`,
            url: `http://127.0.0.1:${decodePort}/index.html`,
            reuseExistingServer: !process.env.CI,
            timeout: 30_000,
        },
        {
            command: `node "${serveScript}" "${encodeDemoDir}" ${encodePort}`,
            url: `http://127.0.0.1:${encodePort}/index.html`,
            reuseExistingServer: !process.env.CI,
            timeout: 30_000,
        },
    ],
    projects: [
        {
            name: 'decode',
            testMatch: 'decode.spec.js',
            use: { baseURL: `http://127.0.0.1:${decodePort}` },
        },
        {
            name: 'encode',
            testMatch: 'encode.spec.js',
            use: { baseURL: `http://127.0.0.1:${encodePort}` },
        },
    ],
});
