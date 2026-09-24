// @ts-check
'use strict';

// planning/esp32-device-ui.md's accessibility section, checked in the browser:
// landmarks and names (every locator in these tests is by role and name, so a
// control without one fails whatever drives it), every action from the
// keyboard, text contrast in both colour schemes, a visible focus, controls
// large enough to hit, and a page that fits a narrow screen.

const { test, expect } = require('./fixtures');
const { playingSendspin } = require('./stub');

test.beforeEach(async ({ page, stub }) => {
    await page.goto(stub.url);
    await expect(page.locator('#state')).toHaveText('Stopped');
});

test('landmarks, headings and a name for every control', async ({ page }) => {
    await expect(page.getByRole('banner')).toBeVisible();
    await expect(page.getByRole('main')).toBeVisible();
    await expect(page.getByRole('contentinfo')).toBeVisible();
    await expect(page.getByRole('heading', { level: 1 })).toHaveText('hearth-a1b2c3');
    for (const name of ['Now', 'Sendspin', 'Settings', 'Real time']) {
        await expect(page.getByRole('heading', { level: 2, name })).toBeVisible();
    }
    for (const name of ['Now', 'Sendspin', 'Settings', 'Real time']) {
        await expect(page.getByRole('region', { name })).toBeVisible();
    }
    await expect(page.getByRole('textbox', { name: 'Name' })).toBeVisible();
    await expect(page.getByRole('radiogroup', { name: 'Slot width' })).toBeVisible();
    await expect(page.getByRole('radiogroup', { name: 'Slot width' })).toHaveAccessibleDescription('This sink has 2 slots.');
    await expect(page.getByRole('radiogroup', { name: 'Slot width' }).getByRole('radio')).toHaveCount(2);
    await expect(page.getByRole('checkbox', { name: 'A second I2S line is wired to a DAC' })).toHaveAccessibleDescription(
        /^Two lines carry twice the slots of one\./,
    );
    await expect(page.getByRole('textbox', { name: 'Wi-Fi network' })).toBeVisible();
    await expect(page.getByRole('radiogroup', { name: 'Common layouts' }).getByRole('radio')).toHaveCount(8);
    await expect(page.getByRole('textbox', { name: 'Output layout' })).toBeVisible();
    await expect(page.getByRole('textbox', { name: 'Output layout' })).toHaveAccessibleDescription(
        /^The speakers this player drives, one per output slot: .+ It takes effect at the next play\. This sink has 2 slots\.$/,
    );
    await expect(page.getByRole('button', { name: 'Show' })).toHaveAttribute('aria-controls', 'pass-input');
    await expect(page.locator('summary', { hasText: 'What an output layout does' })).toBeVisible();
    await expect(page.getByRole('button', { name: 'Apply' })).toBeVisible();
    await expect(page.getByRole('button', { name: 'Forget every server' })).toBeVisible();
    await expect(page.getByRole('status')).toHaveCount(1);
    await expect(page.locator('html')).toHaveAttribute('lang', 'en');
});

test('every action from the keyboard, in page order', async ({ page, stub }) => {
    await page.locator('body').click({ position: { x: 1, y: 1 } });
    // A radio group is one stop, at its chosen radio.
    const order = ['ss-forget', 'wiring', 'slot-width=32', 'layout=2.0', 'layout-input', 'Apply', 'What an output layout does', 'name-input', 'Save', 'ssid-input', 'pass-input', 'pass-show', 'Save', 'Counters'];
    const focused = () =>
        page.evaluate(() => {
            const el = /** @type {HTMLInputElement} */ (document.activeElement);
            return el.id || (el.type === 'radio' ? el.name + '=' + el.value : el.textContent) || '';
        });
    for (const expected of order) {
        await page.keyboard.press('Tab');
        await expect.poll(focused).toBe(expected);
    }

    await page.getByRole('textbox', { name: 'Name' }).focus();
    await page.keyboard.press('ControlOrMeta+a');
    await page.keyboard.type('Attic');
    await page.keyboard.press('Enter');
    await expect.poll(() => stub.sent('PUT /name').map((r) => r.body)).toEqual(['Attic']);

    await page.getByRole('checkbox', { name: 'A second I2S line is wired to a DAC' }).focus();
    await page.keyboard.press('Space');
    await expect.poll(() => stub.sent('PUT /wiring').map((r) => r.body)).toEqual(['1']);

    // The chosen radio, then the arrow keys: 32-bit wraps round to 16-bit.
    await page.getByRole('radio', { name: '32-bit' }).focus();
    await page.keyboard.press('ArrowDown');
    await expect.poll(() => stub.sent('PUT /slot-width').map((r) => r.body)).toEqual(['16']);
    // Eight 16-bit slots a line, on the two lines wired above.
    await expect(page.locator('#slot-help')).toHaveText('This sink has 16 slots.');

    const layout = page.getByRole('textbox', { name: 'Output layout' });
    await layout.focus();
    await page.keyboard.press('ControlOrMeta+a');
    await page.keyboard.type('1.0');
    await page.keyboard.press('Enter');
    await expect.poll(() => stub.sent('PUT /layout').map((r) => r.body)).toEqual(['1.0']);
    // A preset from the arrow keys, from the one the board now has.
    await expect(page.getByRole('radio', { name: '1.0', exact: true })).toBeChecked();
    await page.getByRole('radio', { name: '1.0', exact: true }).focus();
    await page.keyboard.press('ArrowRight');
    await expect.poll(() => stub.sent('PUT /layout').map((r) => r.body)).toEqual(['1.0', '2.0']);

    await page.getByRole('button', { name: 'Show' }).focus();
    await page.keyboard.press('Enter');
    await expect(page.getByLabel('Passphrase')).toHaveAttribute('type', 'text');

    for (const name of ['What an output layout does', 'Counters']) {
        const summary = page.locator('summary', { hasText: name });
        await summary.focus();
        await page.keyboard.press('Enter');
        await expect(page.locator('details', { has: summary })).toHaveAttribute('open', '');
    }

    // The pairing actions, the confirmation included: the dialog opens on
    // Cancel, so Enter alone forgets nothing, and focus goes back after.
    const forget = page.getByRole('button', { name: 'Forget every server' });
    await forget.focus();
    await page.keyboard.press('Enter');
    await expect(page.getByRole('dialog').getByRole('button', { name: 'Cancel' })).toBeFocused();
    await page.keyboard.press('Enter');
    await expect(page.getByRole('dialog')).toBeHidden();
    await expect(forget).toBeFocused();
    await page.keyboard.press('Enter');
    await page.keyboard.press('Tab');
    await expect(page.getByRole('dialog').getByRole('button', { name: 'Forget', exact: true })).toBeFocused();
    await page.keyboard.press('Enter');
    await expect.poll(() => stub.sent('POST /pairing').map((r) => r.body)).toEqual(['forget']);
    Object.assign(stub.device.sendspin, { pairing_code: '482913', pairing_held: true });
    for (const [name, body] of [['Cancel pairing', 'cancel'], ['Allow pairing again', 'reset']]) {
        const button = page.getByRole('button', { name });
        await expect(button).toBeVisible();
        await button.focus();
        await page.keyboard.press('Space');
        await expect.poll(() => stub.sent('POST /pairing').map((r) => r.body)).toContain(body);
    }
});

for (const colorScheme of /** @type {const} */ (['light', 'dark'])) {
    test(`text contrast of at least 4.5:1 in the ${colorScheme} scheme`, async ({ page, stub }) => {
        await page.emulateMedia({ colorScheme });
        // A page with everything showing: a failed play has the most text.
        stub.device.framesPerPoll = 50;
        stub.setStatus({
            state: 'failed', location: 'http://10.0.2.2:8000/demo.ec3', source: 'http', sink: 'capture-tdm',
            sink_slots: 12, slot_bits: 16, second_line: true, name: 'Attic',
            layout: '5.1', volume: 1, stream: { codec: 'E-AC-3', acmod: 7, channels: 6, substreams: 1,
                dialnorm: -31, objects: true, objects_rendered: false, slots: 12, layout: '7.1.4', render: 'channels',
                coded: 'L,C,R,Ls,Rs,LFE', silent: 'Lrs,Rrs,Vhl,Vhr,Lts,Rts' },
            frames: 100, held: 0, us_per_frame: 5404, worst_frame_us: 7617, render_us_per_frame: 115,
            sink_us_per_frame: 362, realtime_permille: 168, resync_bytes: 0, fetched_bytes: 190464, ring_low: 2048,
            passes: 0, layout_mismatches: 0, finished: true, failed: true, why: 'decode', error: 2,
            // A server playing, a level for each output, and a second pairing's code.
            sendspin: { ...playingSendspin(), pairing_code: '482913', pairing_outcome: 'paired' },
            network: { kind: 'wifi', ssid: 'kitchen', rssi_dbm: -58, address: '192.168.1.23' },
        });
        await page.reload();
        // Both closed sections open, so their text is measured too, and an
        // error in the toast, which the pairing code's announcement is not.
        await page.locator('summary', { hasText: 'What an output layout does' }).click();
        await page.locator('summary', { hasText: 'Counters' }).click();
        await expect(page.locator('#hw-chip')).toBeVisible();
        await expect(page.getByRole('radio', { name: '9.1.6' })).toBeDisabled();
        await expect(page.getByRole('radio', { name: '5.1', exact: true })).toBeChecked();
        await page.getByLabel('Name').fill('   ');
        await page.locator('#name-form').getByRole('button', { name: 'Save' }).click();
        await expect(page.getByRole('status')).toHaveClass(/error/);
        await expect(page.locator('#reason')).toBeVisible();
        await expect(page.locator('#ss-code')).toBeVisible();
        await expect(page.getByRole('table')).toBeVisible();
        const worst = await page.evaluate(() => {
            const channel = (c) => {
                const v = c / 255;
                return v <= 0.04045 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4;
            };
            const parse = (colour) => colour.match(/[\d.]+/g).map(Number);
            const luminance = ([r, g, b]) => 0.2126 * channel(r) + 0.7152 * channel(g) + 0.0722 * channel(b);
            const background = (el) => {
                for (let node = el; node; node = node.parentElement) {
                    const [r, g, b, a = 1] = parse(getComputedStyle(node).backgroundColor);
                    if (a > 0) {
                        return [r, g, b];
                    }
                }
                return parse(getComputedStyle(document.body).backgroundColor);
            };
            let low = { ratio: Infinity, text: '' };
            const walker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT);
            for (let node = walker.nextNode(); node; node = walker.nextNode()) {
                const el = node.parentElement;
                if (!node.textContent.trim() || !el || !el.checkVisibility()) {
                    continue;
                }
                const fg = luminance(parse(getComputedStyle(el).color));
                const bg = luminance(background(el));
                const ratio = (Math.max(fg, bg) + 0.05) / (Math.min(fg, bg) + 0.05);
                if (ratio < low.ratio) {
                    low = { ratio, text: node.textContent.trim().slice(0, 40) };
                }
            }
            return low;
        });
        expect(worst.ratio, `lowest contrast, on "${worst.text}"`).toBeGreaterThanOrEqual(4.5);
    });
}

test('the focused control shows a 3 px outline', async ({ page }) => {
    // A segment's radio is drawn by the segment beside it, which carries the
    // outline in its place.
    const segment = (name) => page.getByRole('radio', { name, exact: true }).locator('xpath=following-sibling::span');
    for (const [control, outlined] of [
        [page.getByRole('textbox', { name: 'Name' })],
        [page.getByRole('radio', { name: '32-bit' }), segment('32-bit')],
        [page.getByRole('radio', { name: '2.0', exact: true }), segment('2.0')],
        [page.getByRole('checkbox', { name: 'A second I2S line is wired to a DAC' })],
        [page.getByRole('button', { name: 'Apply' })],
    ]) {
        await control.focus();
        await page.keyboard.press('Shift+Tab');
        await page.keyboard.press('Tab');
        await expect(control).toBeFocused();
        await expect(outlined || control).toHaveCSS('outline-style', 'solid');
        await expect(outlined || control).toHaveCSS('outline-width', '3px');
    }
});

test('controls at least 44 CSS pixels tall', async ({ page }) => {
    for (const control of [
        page.getByRole('textbox', { name: 'Name' }),
        // A segment is the radio's target: the label around it.
        page.locator('#slot-width label').first(),
        page.locator('#layouts label').first(),
        page.getByRole('textbox', { name: 'Wi-Fi network' }),
        // A password input has no textbox role to ask for, so it is found by
        // the label that names it.
        page.getByLabel('Passphrase'),
        page.getByRole('button', { name: 'Show' }),
        page.getByRole('textbox', { name: 'Output layout' }),
        page.getByRole('button', { name: 'Apply' }),
        page.locator('summary', { hasText: 'What an output layout does' }),
        page.locator('summary', { hasText: 'Counters' }),
        page.getByRole('button', { name: 'Forget every server' }),
    ]) {
        const box = await control.boundingBox();
        expect(box && box.height, await control.evaluate((el) => el.outerHTML.slice(0, 60))).toBeGreaterThanOrEqual(44);
    }
});

test('a 320 px screen, with nothing wider than it', async ({ page, stub }) => {
    // The levels table as wide as it gets: sixteen outputs.
    stub.device.sendspin = {
        ...playingSendspin(),
        pairing_code: '482913',
        peak_db: Array(16).fill(-100.5),
        rms_db: Array(16).fill(-110.5),
    };
    await expect(page.getByRole('table')).toBeVisible();
    await page.setViewportSize({ width: 320, height: 640 });
    const overflow = await page.evaluate(() => document.documentElement.scrollWidth - document.documentElement.clientWidth);
    expect(overflow).toBeLessThanOrEqual(0);
});
