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
    await expect(page.locator('#objects')).toHaveText('Carried, not placed');
    await expect(page.locator('#played')).toHaveText('0:08 (250 frames)');
    // What the output layout did with it: the decoder's fold, onto the capture
    // sink's two slots.
    await expect(page.locator('#sink')).toHaveText('capture-i2s, 2 slots');
    await expect(page.locator('#channels')).toHaveText('6: L C R Ls Rs LFE');
    await expect(page.locator('#output')).toHaveText('2.0, 2 slots: folded to two channels by the decoder (Lo/Ro)');
    await expect(page.locator('#silent')).toBeHidden();

    // What the board says it is and where: QEMU's Ethernet and its address,
    // the firmware this image is, and no wiring to choose on a capture sink,
    // which has no second line.
    await expect(page.locator('#network')).toHaveText('Ethernet \u00b7 10.0.2.15');
    await expect(page.locator('#hw-firmware')).toHaveText(/^ac3forge_hearth_sink /);
    await expect(page.locator('#hw-idf')).toHaveText(/^v\d/);
    await expect(page.locator('#wiring-row')).toBeHidden();
    await expect(page.getByRole('radio', { name: '32-bit' })).toBeChecked();

    // A setting, made from the page and read back from the device: the name
    // it answers to, which is what B2 made the page's business.
    await page.getByLabel('Name').fill('Bench sink');
    await page.locator('#name-form').getByRole('button', { name: 'Save' }).click();
    await expect.poll(async () => (await status()).name, { timeout: 10_000 }).toBe('Bench sink');

    // A layout the capture sink's two slots cannot carry: refused by the
    // firmware, explained by the page; the layout stays.
    await page.getByRole('textbox', { name: 'Output layout' }).fill('5.1');
    await page.getByRole('button', { name: 'Apply' }).click();
    await expect(page.getByRole('status')).toHaveText('Output layout 5.1 refused (409): it needs 6 slots and this sink has 2.');
    expect((await status()).layout).toBe('2.0');

    // The volume, set over the REST surface as a server would, since the page
    // has no slider from B2 on, and read back from the device. The console
    // check after this test holds the play below to it: a quarter of the boot
    // play's levels.
    expect((await request.post('volume', { data: '0.25' })).ok()).toBe(true);
    await expect.poll(async () => (await status()).volume, { timeout: 10_000 }).toBe(0.25);

    // A play through to its end at that volume, started over the REST surface
    // as a server would: the page reports plays from B2 on and does not start
    // them. While the new source opens, /status has the new location beside
    // the boot play's state and figures, which look like this play's end; so
    // first this play under way, then its end.
    await request.post('play', { data: REPLAY });
    await expect
        .poll(async () => {
            const s = await status();
            return s.location === REPLAY && s.state === 'playing';
        }, { timeout: 60_000, intervals: [50] })
        .toBe(true);
    await expect
        .poll(async () => {
            const s = await status();
            return s.location === REPLAY && s.state === 'finished' && s.frames === 250;
        }, { timeout: 120_000 })
        .toBe(true);
    await expect(page.locator('#state')).toHaveText('Finished (end of stream)');
    await expect(page.locator('#location')).toHaveText(REPLAY);

    // Stop, over the same surface, read back through the page.
    await request.post('stop');
    await expect.poll(async () => (await status()).state, { timeout: 10_000 }).toBe('stopped');
    await expect(page.locator('#state')).toHaveText('Stopped');

    // The list of routes, where GET / used to have it.
    const api = await request.get('api');
    expect(api.headers()['content-type']).toBe('text/plain');
    expect(await api.text()).toContain('GET  /status        what is playing, as JSON');

    expect(problems).toEqual([]);
});
