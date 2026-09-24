// @ts-check
'use strict';

// What the page shows for GET /hardware - planning/esp32-device-ui.md's "What
// `/hardware` adds": fetched once when the page loads rather than polled
// every second, and tried again at the next successful status poll if the
// first attempt failed.

const { test, expect } = require('./fixtures');

test('a board with everything to report: chip, revision, cores, clock, arithmetic, PSRAM, sink ceiling and a notice', async ({
    page,
    stub,
}) => {
    stub.device.hardware = {
        target: 'esp32p4',
        chip: 'ESP32-P4',
        revision: '3.0',
        cores: 2,
        fpu: true,
        cpu_freq_mhz: 360,
        psram_bytes: 32 * 1024 * 1024,
        sink_max_slots: 16,
        sink_max_slots_bits: 32,
        project: 'ac3forge_hearth_sink',
        version: 'v0.10.0-beta.1-42-gee9cf4f',
        idf_version: 'v6.1',
        capabilities: [
            '2 cores, a hardware floating-point unit',
            'Running at 360 MHz',
            '32 MiB of PSRAM',
            "This sink's bus reaches up to 16 slots at 32-bit",
        ],
        notices: [
            'The detected chip is v3.0, v3.0 or newer. This build was compiled to also accept ' +
                'older, pre-production silicon: it runs the CPU at 360 MHz rather than 400 ' +
                '(esp32p4/Kconfig.cpu), and falls back to the 40 MHz crystal for I2S rather than ' +
                'the 160 MHz PLL v3.0+ silicon supports (I2S_CLK_SRC_XTAL/PLL_160M, hal/i2s_ll.h) ' +
                '- a build that required v3.0 or newer could reach both.',
        ],
    };
    await page.goto(stub.url);
    await expect(page.locator('#hw-note')).toBeHidden();
    await expect(page.locator('#hw-chip')).toHaveText('ESP32-P4, revision 3.0');
    await expect(page.locator('#hw-cores')).toHaveText('2');
    await expect(page.locator('#hw-clock')).toHaveText('360 MHz');
    await expect(page.locator('#hw-arithmetic')).toHaveText('Hardware floating point');
    await expect(page.locator('#hw-psram')).toHaveText('32 MiB');
    await expect(page.locator('#hw-sink')).toHaveText('16 slots at 32-bit');
    await expect(page.locator('#hw-firmware')).toHaveText('ac3forge_hearth_sink v0.10.0-beta.1-42-gee9cf4f');
    await expect(page.locator('#hw-idf')).toHaveText('v6.1');
    await expect(page.locator('#hw-notices')).toBeVisible();
    await expect(page.locator('#hw-notices li')).toHaveText([
        'The detected chip is v3.0, v3.0 or newer. This build was compiled to also accept ' +
            'older, pre-production silicon: it runs the CPU at 360 MHz rather than 400 ' +
            '(esp32p4/Kconfig.cpu), and falls back to the 40 MHz crystal for I2S rather than ' +
            'the 160 MHz PLL v3.0+ silicon supports (I2S_CLK_SRC_XTAL/PLL_160M, hal/i2s_ll.h) ' +
            '- a build that required v3.0 or newer could reach both.',
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
    // cpu_freq_mhz and sink_max_slots both left out of the JSON entirely, the
    // same rule /status's own optional fields follow: no row, not a reading
    // of zero.
    await expect(page.locator('#hw-clock')).toBeHidden();
    await expect(page.locator('#hw-sink')).toBeHidden();
    await expect(page.locator('#hw-notices')).toBeHidden();
    // A firmware from before it reported itself.
    await expect(page.locator('#hw-firmware')).toBeHidden();
    await expect(page.locator('#hw-idf')).toBeHidden();
});

test('a firmware that reports its project but no version', async ({ page, stub }) => {
    stub.device.hardware = { ...stub.device.hardware, version: '' };
    await page.goto(stub.url);
    await expect(page.locator('#hw-firmware')).toHaveText('ac3forge_hearth_sink');
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

test('a sink ceiling with no slot width to give shows the plain count', async ({ page, stub }) => {
    stub.device.hardware = {
        target: 'esp32s3',
        chip: 'ESP32-S3',
        revision: '0.2',
        cores: 2,
        fpu: true,
        psram_bytes: 0,
        sink_max_slots: 6,
        capabilities: ['2 cores, a hardware floating-point unit', "This sink's bus reaches up to 6 slots"],
        notices: [],
    };
    await page.goto(stub.url);
    await expect(page.locator('#hw-sink')).toHaveText('6 slots');
});

test('a pre-production P4 below v3.0 shows both the clock notice and the TDM hard limit', async ({ page, stub }) => {
    stub.device.hardware = {
        target: 'esp32p4',
        chip: 'ESP32-P4',
        revision: '1.3',
        cores: 2,
        fpu: true,
        cpu_freq_mhz: 360,
        psram_bytes: 32 * 1024 * 1024,
        sink_max_slots: 32,
        sink_max_slots_bits: 16,
        capabilities: [
            '2 cores, a hardware floating-point unit',
            'Running at 360 MHz',
            '32 MiB of PSRAM',
            "This sink's bus reaches up to 32 slots at 16-bit",
        ],
        notices: [
            'The detected chip is v1.3, below v3.0. This chip has no PLL clock source for I2S ' +
                "(hal/i2s_ll.h), and the APLL fallback's own 125 MHz ceiling falls short of what a " +
                'full-width TDM frame needs: TDM output cannot open at any channel count above 2 on ' +
                'this board, regardless of layout - only standard 1-2 channel I2S is reachable.',
        ],
    };
    await page.goto(stub.url);
    await expect(page.locator('#hw-chip')).toHaveText('ESP32-P4, revision 1.3');
    await expect(page.locator('#hw-sink')).toHaveText('32 slots at 16-bit');
    await expect(page.locator('#hw-notices li')).toHaveText([
        'The detected chip is v1.3, below v3.0. This chip has no PLL clock source for I2S ' +
            "(hal/i2s_ll.h), and the APLL fallback's own 125 MHz ceiling falls short of what a " +
            'full-width TDM frame needs: TDM output cannot open at any channel count above 2 on ' +
            'this board, regardless of layout - only standard 1-2 channel I2S is reachable.',
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
