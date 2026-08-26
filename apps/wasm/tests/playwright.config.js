// @ts-check
'use strict';

const path = require('path');
const { defineConfig } = require('@playwright/test');

// roadmap VX18(a): headless-browser coverage for the WASM decode demo -
// docs/platforms/wasm.md's "Not yet verified" section names every
// functional claim this closes. WASM_DEMO_DIR points at the directory
// apps/wasm/CMakeLists.txt's build produces
// (${CMAKE_BINARY_DIR}/bin/wasm_decode_demo/); _build.yml's build-wasm job
// sets it to that path. Defaulting to the config-wasm-emscripten preset's
// own build tree keeps `npx playwright test` runnable straight from a local
// build with no extra flags.
const demoDir = path.resolve(
    process.env.WASM_DEMO_DIR ||
        path.join(__dirname, '../../../build/config-wasm-emscripten/bin/wasm_decode_demo'));

const port = 4173;

module.exports = defineConfig({
    testDir: __dirname,
    timeout: 30_000,
    retries: process.env.CI ? 1 : 0,
    reporter: process.env.CI ? 'line' : 'list',
    use: {
        baseURL: `http://127.0.0.1:${port}`,
    },
    webServer: {
        command: `node "${path.join(__dirname, 'serve.js')}" "${demoDir}" ${port}`,
        url: `http://127.0.0.1:${port}/index.html`,
        reuseExistingServer: !process.env.CI,
        timeout: 30_000,
    },
});
