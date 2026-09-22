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
    await expect(page.getByRole('combobox', { name: 'Slot width' })).toBeVisible();
    await expect(page.getByRole('checkbox', { name: 'A second I2S line is wired to a DAC' })).toBeVisible();
    await expect(page.getByRole('textbox', { name: 'Network' })).toBeVisible();
    await expect(page.getByRole('combobox', { name: 'Output layout' })).toBeVisible();
    await expect(page.getByRole('combobox', { name: 'Output layout' })).toHaveAccessibleDescription(
        /^The speakers this player drives, one per output slot: .+ It takes effect at the next play\. This sink has 2 slots\.$/,
    );
    await expect(page.locator('summary', { hasText: 'What an output layout does' })).toBeVisible();
    await expect(page.getByRole('button', { name: 'Apply' })).toBeVisible();
    await expect(page.getByRole('button', { name: 'Forget every server' })).toBeVisible();
    await expect(page.getByRole('status')).toHaveCount(1);
    await expect(page.locator('html')).toHaveAttribute('lang', 'en');
});

test('every action from the keyboard, in page order', async ({ page, stub }) => {
    await page.locator('body').click({ position: { x: 1, y: 1 } });
    const order = ['ss-forget', 'name-input', 'Save', 'slot-width', 'wiring', 'layout-input', 'Apply', 'What an output layout does', 'ssid-input', 'pass-input', 'Save', 'Counters'];
    const focused = () =>
        page.evaluate(() => {
            const el = /** @type {HTMLElement} */ (document.activeElement);
            return el.id || el.textContent || '';
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

    await page.getByRole('combobox', { name: 'Slot width' }).focus();
    await page.keyboard.press('ArrowDown');
    await expect.poll(() => stub.sent('PUT /slot-width').map((r) => r.body)).toEqual(['16']);

    const layout = page.getByRole('combobox', { name: 'Output layout' });
    await layout.focus();
    await page.keyboard.press('ControlOrMeta+a');
    await page.keyboard.type('1.0');
    await page.keyboard.press('Enter');
    await expect.poll(() => stub.sent('PUT /layout').map((r) => r.body)).toEqual(['1.0']);

    for (const name of ['What an output layout does', 'Counters']) {
        const summary = page.locator('summary', { hasText: name });
        await summary.focus();
        await page.keyboard.press('Enter');
        await expect(page.locator('details', { has: summary })).toHaveAttribute('open', '');
    }

    // The pairing actions, the confirmation included.
    page.once('dialog', (dialog) => dialog.accept());
    await page.getByRole('button', { name: 'Forget every server' }).focus();
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
            sink_slots: 12, layout: '5.1', volume: 1, stream: { codec: 'E-AC-3', acmod: 7, channels: 6, substreams: 1,
                dialnorm: -31, objects: true, objects_rendered: false, slots: 12, layout: '7.1.4', render: 'channels',
                coded: 'L,C,R,Ls,Rs,LFE', silent: 'Lrs,Rrs,Vhl,Vhr,Lts,Rts' },
            frames: 100, held: 0, us_per_frame: 5404, worst_frame_us: 7617, render_us_per_frame: 115,
            sink_us_per_frame: 362, realtime_permille: 168, resync_bytes: 0, fetched_bytes: 190464, ring_low: 2048,
            passes: 0, layout_mismatches: 0, finished: true, failed: true, why: 'decode', error: 2,
            // A server playing, a level for each output, and a second pairing's code.
            sendspin: { ...playingSendspin(), pairing_code: '482913', pairing_outcome: 'paired' },
        });
        await page.reload();
        // Both closed sections open, so their text is measured too.
        await page.locator('summary', { hasText: 'What an output layout does' }).click();
        await page.locator('summary', { hasText: 'Counters' }).click();
        await page.getByLabel('Name').fill('Attic');
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
    for (const control of [
        page.getByRole('textbox', { name: 'Name' }),
        page.getByRole('combobox', { name: 'Slot width' }),
        page.getByRole('checkbox', { name: 'A second I2S line is wired to a DAC' }),
    ]) {
        await control.focus();
        await page.keyboard.press('Shift+Tab');
        await page.keyboard.press('Tab');
        await expect(control).toBeFocused();
        await expect(control).toHaveCSS('outline-style', 'solid');
        await expect(control).toHaveCSS('outline-width', '3px');
    }
});

test('controls at least 44 CSS pixels tall', async ({ page }) => {
    for (const control of [
        page.getByRole('textbox', { name: 'Name' }),
        page.getByRole('combobox', { name: 'Slot width' }),
        page.getByRole('textbox', { name: 'Network' }),
        // A password input has no textbox role to ask for, so it is found by
        // the label that names it.
        page.getByLabel('Passphrase'),
        page.getByRole('combobox', { name: 'Output layout' }),
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
