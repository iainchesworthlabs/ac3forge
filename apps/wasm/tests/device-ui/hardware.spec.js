// @ts-check
'use strict';

// What the page shows for GET /hardware - planning/esp32-device-ui.md's "What
// `/hardware` adds": fetched once when the page loads rather than polled
// every second, and tried again at the next successful status poll if the
// first attempt failed.

const { test, expect } = require('./fixtures');

test('a board with everything to report: chip, revision, cores, arithmetic, PSRAM, sink ceiling and a notice', async ({
    page,
    stub,
}) => {
    stub.device.hardware = {
        target: 'esp32p4',
        chip: 'ESP32-P4',
        revision: '3.0',
        cores: 2,
        fpu: true,
        psram_bytes: 32 * 1024 * 1024,
        sink_max_slots: 16,
        capabilities: [
            '2 cores, a hardware floating-point unit',
            '32 MiB of PSRAM',
            "This sink's bus reaches up to 16 slots",
        ],
        notices: [
            'This build accepts a chip revision as old as v1.0; the detected chip is v3.0, newer than that.',
        ],
    };
    await page.goto(stub.url);
    await expect(page.locator('#hw-note')).toBeHidden();
    await expect(page.locator('#hw-chip')).toHaveText('ESP32-P4, revision 3.0');
    await expect(page.locator('#hw-cores')).toHaveText('2');
    await expect(page.locator('#hw-arithmetic')).toHaveText('Hardware floating point');
    await expect(page.locator('#hw-psram')).toHaveText('32 MiB');
    await expect(page.locator('#hw-sink')).toHaveText('16 slots');
    await expect(page.locator('#hw-notices')).toBeVisible();
    await expect(page.locator('#hw-notices li')).toHaveText([
        'This build accepts a chip revision as old as v1.0; the detected chip is v3.0, newer than that.',
    ]);
});

test('a board with nothing extra to report: no PSRAM, no FPU, no sink ceiling to show and no notices', async ({
    page,
    stub,
}) => {
    stub.device.hardware = {
        target: 'esp32c6',
        chip: 'ESP32-C6',
        revision: '0.2',
        cores: 1,
        fpu: false,
        psram_bytes: 0,
        capabilities: ['1 core, fixed-point arithmetic (no floating-point unit)'],
        notices: [],
    };
    await page.goto(stub.url);
    await expect(page.locator('#hw-chip')).toHaveText('ESP32-C6, revision 0.2');
    await expect(page.locator('#hw-cores')).toHaveText('1');
    await expect(page.locator('#hw-arithmetic')).toHaveText('Fixed-point (no floating-point unit)');
    await expect(page.locator('#hw-psram')).toHaveText('None');
    // sink_max_slots left out of the JSON entirely, the same rule /status's
    // own optional fields follow: no row, not a ceiling of zero.
    await expect(page.locator('#hw-sink')).toBeHidden();
    await expect(page.locator('#hw-notices')).toBeHidden();
});

test('a firmware built for one target running on another shows the mismatch as a notice', async ({ page, stub }) => {
    stub.device.hardware = {
        target: 'esp32c6',
        chip: 'ESP32-S3',
        revision: '0.2',
        cores: 2,
        fpu: true,
        psram_bytes: 0,
        capabilities: [],
        notices: ['This firmware was built for esp32c6, but the chip it is running on identifies itself as ESP32-S3.'],
    };
    await page.goto(stub.url);
    await expect(page.locator('#hw-notices li')).toHaveText([
        'This firmware was built for esp32c6, but the chip it is running on identifies itself as ESP32-S3.',
    ]);
});

test('fetched once when the page loads, not on every status poll', async ({ page, stub }) => {
    await page.goto(stub.url);
    await expect(page.locator('#hw-chip')).not.toBeEmpty();
    expect(stub.sent('GET /hardware').length).toBe(1);
    await expect.poll(() => stub.sent('GET /status').length).toBeGreaterThanOrEqual(3);
    expect(stub.sent('GET /hardware').length).toBe(1);
});

test('a firmware that fails GET /hardware the first time is tried again at the next status poll', async ({
    page,
    stub,
}) => {
    stub.next('GET /hardware', { status: 500, body: 'no' });
    await page.goto(stub.url);
    await expect(page.locator('#hw-note')).toBeVisible();
    await expect(page.locator('#hw-chip')).toBeHidden();
    // The default stand-in board (stub.js's defaultHardware()): an ESP32-S3.
    await expect(page.locator('#hw-chip')).toHaveText('ESP32-S3, revision 0.2', { timeout: 5000 });
    await expect(page.locator('#hw-note')).toBeHidden();
});
