'use strict';

// Fixtures for the device page's host tests: a stand-in device per test
// (stub.js), and the page with Chromium's coverage of its script collected and
// written in the form Node's own coverage takes, where c8 reads it (see
// package.json's coverage:device-ui). An uncaught error in the page, or a
// content-security-policy violation, fails the test that caused it.

const fs = require('fs');
const path = require('path');
const { pathToFileURL } = require('url');
const base = require('@playwright/test');
const { startStub, UI_DIR } = require('./stub');

const SCRIPT_URL = pathToFileURL(path.join(UI_DIR, 'ac3forge_ui.js')).href;
const COVERAGE_DIR = path.resolve(__dirname, '..', 'test-results', 'device-ui-coverage', 'tmp');

const test = base.test.extend({
    // eslint-disable-next-line no-empty-pattern
    stub: async ({}, use) => {
        const stub = await startStub();
        await use(stub);
        await stub.close();
    },
    page: async ({ page }, use, testInfo) => {
        const problems = [];
        page.on('pageerror', (error) => problems.push(String(error)));
        page.on('console', (message) => {
            // Chromium logs a failed load - which the error-path tests cause on
            // purpose - as a console error. Anything else logged as one is not
            // expected: a policy violation, an exception.
            if (message.type() === 'error' && !message.text().startsWith('Failed to load resource')) {
                problems.push(message.text());
            }
        });
        await page.coverage.startJSCoverage({ resetOnNavigation: false });
        await use(page);
        // The script's offsets are the file's own: the stand-in serves it from
        // the tree unchanged, so the coverage maps straight onto the source.
        const result = (await page.coverage.stopJSCoverage())
            .filter((entry) => entry.url.endsWith('/ui.js'))
            .map((entry) => ({ scriptId: entry.scriptId, url: SCRIPT_URL, functions: entry.functions }));
        if (result.length > 0) {
            fs.mkdirSync(COVERAGE_DIR, { recursive: true });
            const name = `coverage-${testInfo.testId}-${testInfo.retry}.json`;
            fs.writeFileSync(path.join(COVERAGE_DIR, name), JSON.stringify({ result }));
        }
        base.expect(problems, 'errors in the page').toEqual([]);
    },
});

module.exports = { test, expect: base.expect };
