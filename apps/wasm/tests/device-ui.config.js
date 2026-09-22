// @ts-check
'use strict';

const path = require('path');
const { defineConfig } = require('@playwright/test');

// The ESP32 player's web page (planning/esp32-device-ui.md), on this harness's
// package, lockfile and Chromium - the WASM demos' - rather than a second
// browser-test stack. Two projects:
//
//   host   the page against device-ui/stub.js, a stand-in for the firmware's
//          REST routes that each test starts for itself, with coverage of the
//          page's script collected for c8 (package.json's coverage:device-ui)
//   board  the page as a device serves it, at AC3FORGE_DEVICE_URL: CI's
//          ESP32 job points it at the emulated board through QEMU's port
//          forward
module.exports = defineConfig({
    testDir: path.join(__dirname, 'device-ui'),
    outputDir: path.join(__dirname, 'test-results', 'device-ui'),
    globalSetup: require.resolve('./device-ui/global-setup.js'),
    timeout: 30_000,
    forbidOnly: !!process.env.CI,
    reporter: process.env.CI ? 'line' : 'list',
    use: { browserName: 'chromium' },
    projects: [
        // UTC, so the times the page prints are the same on every machine.
        { name: 'host', testIgnore: 'board/**', use: { timezoneId: 'UTC' } },
        {
            name: 'board',
            testMatch: 'board/*.spec.js',
            // QEMU decodes slowly and the smoke waits for a whole play.
            timeout: 240_000,
            use: { baseURL: process.env.AC3FORGE_DEVICE_URL },
        },
    ],
});
