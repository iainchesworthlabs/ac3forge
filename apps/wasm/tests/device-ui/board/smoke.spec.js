// @ts-check
'use strict';

// The page as a device serves it, driving that device - planning/
// esp32-device-ui.md's test on the target. CI's ESP32 job runs it against the
// emulated board through QEMU's port forward (AC3FORGE_DEVICE_URL), after the
// boot play has finished, and then checks the console: the play this test
// starts must come out at the volume this test sets.
//
// What the page shows is read from the page; what the device did is read
// from GET /status, apart from the page, so the test holds each against the
// other.

const fs = require('fs');
const path = require('path');
const { test, expect } = require('@playwright/test');

const UI = path.resolve(__dirname, '../../../../../esp-idf/ac3forge/ui');
// The boot play's stream again, under a location of its own, so that its end
// cannot be mistaken for the boot play's: the static server ignores the query.
const REPLAY = `${process.env.AC3FORGE_STREAM_URL || 'http://10.0.2.2:8000/demo.ec3'}?from=the-page`;

test('the board serves the page, and the page drives the board', async ({ page, request }) => {
    expect(process.env.AC3FORGE_DEVICE_URL, 'AC3FORGE_DEVICE_URL names the device').toBeTruthy();
    const problems = [];
    page.on('pageerror', (error) => problems.push(String(error)));
    const status = async () => (await request.get('status')).json();

    // The page and its script, from the firmware's flash: the files in this
    // tree, byte for byte, with the headers control.cpp sends.
    const response = await page.goto('./');
    expect(response && response.status()).toBe(200);
    const headers = response ? response.headers() : {};
    expect(headers['content-type']).toBe('text/html; charset=utf-8');
    expect(headers['cache-control']).toBe('no-cache');
    expect(headers['content-security-policy']).toContain("default-src 'self'");
    expect(Number(headers['content-length'])).toBe(fs.statSync(path.join(UI, 'ac3forge_ui.html')).size);
    const script = await request.get('ui.js');
    expect(await script.body()).toEqual(fs.readFileSync(path.join(UI, 'ac3forge_ui.js')));

    // What the boot play decoded: the WASM page's demo, E-AC-3 5.1 with objects.
    await expect(page.locator('#state')).toHaveText('Finished (end of stream)', { timeout: 120_000 });
    await expect(page.locator('#codec')).toHaveText('E-AC-3, 1 substream, dialnorm -31');
    await expect(page.locator('#channels')).toHaveText('6 (3/2)');
    await expect(page.locator('#objects')).toHaveText('Carried, not placed');
    await expect(page.locator('#played')).toHaveText('0:08 (250 frames)');

    // The volume, set with the slider and read back from the device.
    await page.getByRole('slider', { name: 'Volume' }).fill('25');
    await expect.poll(async () => (await status()).volume, { timeout: 10_000 }).toBe(0.25);

    // A layout the capture sink's two slots cannot carry, refused by the
    // firmware in its own words; the layout stays.
    await page.getByRole('combobox', { name: 'Layout' }).fill('5.1');
    await page.getByRole('button', { name: 'Apply' }).click();
    await expect(page.getByRole('status')).toHaveText(
        /^Layout 5\.1 refused \(409\): not a layout this player can play/,
    );
    expect((await status()).layout).toBe('2.0');

    // A play from the form, through to its end, at the volume set above.
    await page.getByRole('textbox', { name: 'Location to play' }).fill(REPLAY);
    await page.getByRole('button', { name: 'Play' }).click();
    await expect
        .poll(async () => {
            const s = await status();
            return s.location === REPLAY && s.state === 'finished' && s.frames === 250;
        }, { timeout: 120_000 })
        .toBe(true);
    await expect(page.locator('#state')).toHaveText('Finished (end of stream)');
    await expect(page.locator('#location')).toHaveText(REPLAY);

    // Stop, from the button, read back from the device.
    await page.getByRole('button', { name: 'Stop' }).click();
    await expect.poll(async () => (await status()).state, { timeout: 10_000 }).toBe('stopped');
    await expect(page.locator('#state')).toHaveText('Stopped');

    // The list of routes, where GET / used to have it.
    const api = await request.get('api');
    expect(api.headers()['content-type']).toBe('text/plain');
    expect(await api.text()).toContain('GET  /status        what is playing, as JSON');

    expect(problems).toEqual([]);
});
