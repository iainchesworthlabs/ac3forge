// @ts-check
'use strict';

// The page on a twelve-slot board - CI's sdkconfig.ci-http714 shape under QEMU,
// with the stream set (esp-idf/ac3forge/examples/stream_player/www/) served
// beside it - and what it says a layout does with a stream: planning/
// esp32-device-ui.md's "The output layout". CI runs it after
// tools/checks/check_stream_set.py has played the set, with the device's
// layout at 7.1.4, and it leaves the layout at 7.1.4.

const { test, expect } = require('@playwright/test');

const BASE = process.env.AC3FORGE_STREAM_BASE || 'http://10.0.2.2:8000';

test('the page says what each output layout does with each stream', async ({ page, request }) => {
    expect(process.env.AC3FORGE_DEVICE_URL, 'AC3FORGE_DEVICE_URL names the device').toBeTruthy();
    const problems = [];
    page.on('pageerror', (error) => problems.push(String(error)));
    const status = async () => (await request.get('status')).json();
    const field = page.getByRole('combobox', { name: 'Output layout' });

    // A play from the form, through to its end: first the new location past
    // "opening", then its end, so that the previous play's end is never taken
    // for this one's.
    async function play(file) {
        const location = `${BASE}/${file}`;
        await page.getByRole('textbox', { name: 'Location to play' }).fill(location);
        await page.getByRole('button', { name: 'Play' }).click();
        await expect
            .poll(async () => {
                const s = await status();
                return s.location === location && s.state !== 'opening';
            }, { timeout: 60_000, intervals: [50] })
            .toBe(true);
        await expect
            .poll(async () => {
                const s = await status();
                return s.location === location && (s.state === 'finished' || s.state === 'failed');
            }, { timeout: 120_000 })
            .toBe(true);
        await expect(page.locator('#location')).toHaveText(location);
    }

    async function choose(layout) {
        await field.fill(layout);
        await page.getByRole('button', { name: 'Apply' }).click();
        await expect(page.getByRole('status')).toHaveText(`Output layout ${layout} from the next play.`);
    }

    await page.goto('./');
    await expect(page.locator('#sink')).toHaveText('capture-tdm, 12 slots');
    await expect(field).toHaveAccessibleDescription(/This sink has 12 slots\.$/);
    const enabled = await page
        .locator('#layouts option')
        .evaluateAll((options) => options.filter((o) => !o.disabled).map((o) => o.value));
    expect(enabled).toEqual(['1.0', '2.0', '5.1', '7.1', '5.1.2', '5.1.4', '7.1.4']);

    // 7.1.4 on 7.1.4: every speaker has its own channel.
    await play('714-walk.ec3');
    await expect(page.locator('#channels')).toHaveText('12: L C R Ls Rs Lrs Rrs Vhl Vhr Lts Rts LFE');
    await expect(page.locator('#output')).toHaveText('7.1.4, 12 slots: each channel on the speaker at its location');
    await expect(page.locator('#silent')).toBeHidden();

    // 5.1 on 7.1.4: nothing is upmixed, so the rears and the heights are silent.
    await play('layout-51.ec3');
    await expect(page.locator('#silent')).toHaveText('Lrs, Rrs, Vhl, Vhr, Lts, Rts: nothing in the stream for these');

    // 5.1 for the next play, while this one keeps 7.1.4.
    await choose('5.1');
    await expect(page.locator('#next')).toHaveText('5.1');
    await expect(page.locator('#output')).toHaveText(/^7\.1\.4, 12 slots/);
    // 7.1.4 on 5.1: the rears and the heights spread over the room's speakers.
    await play('714-walk.ec3');
    await expect(page.locator('#output')).toHaveText('5.1, 6 slots: each channel on the speaker at its location');
    await expect(page.locator('#silent')).toBeHidden();
    await expect(page.locator('#next')).toBeHidden();

    // 2.0: the decoder's fold. Of a 5.1 stream: folding 7.1.4 needs more
    // internal RAM than this shape has without PSRAM (planning/esp32-stream-set.md).
    await choose('2.0');
    await play('layout-51.ec3');
    await expect(page.locator('#output')).toHaveText('2.0, 2 slots: folded to two channels by the decoder (Lo/Ro)');
    await expect(page.locator('#silent')).toBeHidden();

    // Wider than the bus: refused, and the page says why.
    await field.fill('9.1.6');
    await page.getByRole('button', { name: 'Apply' }).click();
    await expect(page.getByRole('status')).toHaveText(
        'Output layout 9.1.6 refused (409): it needs 16 slots and this sink has 12.',
    );

    // A stream at 44.1 kHz: the player refuses it rather than play it fast.
    await choose('7.1.4');
    await play('ac3-51-44k.ac3');
    await expect(page.locator('#state')).toHaveText('Failed');
    await expect(page.locator('#reason')).toHaveText(
        "Stopped: the stream's sample rate, 44,100 Hz, is not the sink's.",
    );
    expect((await status()).layout).toBe('7.1.4');
    expect(problems).toEqual([]);
});
