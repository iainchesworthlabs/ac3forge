// @ts-check
'use strict';

// Every action the page offers, driven through the page against the stand-in
// device, and what the device was sent - planning/esp32-device-ui.md's "What
// the page does". The error paths are the device's own refusals (400, 409), a
// connection closed unanswered and a device that does not answer at all.

const { test, expect } = require('./fixtures');
const { REPLIES } = require('./stub');

const DEMO = 'http://10.0.2.2:8000/demo.ec3';

// THE PAGE NO LONGER PLAYS ANYTHING. A server owns playback from B2 on
// (planning/hearth-reference-player.md): this page is what the BOARD is, and
// POST /play stays in the REST surface for a person with curl. So the plays
// these tests need are started through the device's own API rather than
// through the page, as ac3hearth or Music Assistant would.
const startPlay = (page, stub, location = DEMO) => page.request.post(stub.url + 'play', { data: location });

test.describe('with the clock running', () => {
    test.beforeEach(async ({ page, stub }) => {
        await page.goto(stub.url);
        await expect(page.locator('#state')).toHaveText('Stopped');
    });

    test('Save sends the name to PUT /name', async ({ page, stub }) => {
        const name = page.getByLabel('Name');
        await expect(name).toHaveValue('hearth-a1b2c3');
        await name.fill('Sitting room');
        await page.locator('#name-form').getByRole('button', { name: 'Save' }).click();
        await expect.poll(() => stub.sent('PUT /name').map((r) => r.body)).toEqual(['Sitting room']);
        await expect(page.getByRole('status')).toHaveText('This sink is called Sitting room.');
        // What the board answers to is what the page then shows itself as.
        await expect(page.locator('#title')).toHaveText('Sitting room');
    });

    test('a name of only spaces is not sent', async ({ page, stub }) => {
        await page.getByLabel('Name').fill('   ');
        await page.locator('#name-form').getByRole('button', { name: 'Save' }).click();
        await expect(page.getByRole('status')).toHaveText('Enter a name.');
        expect(stub.sent('PUT /name')).toHaveLength(0);
    });

    test("a refused name shows the device's reply", async ({ page, stub }) => {
        stub.next('PUT /name', { status: 409, body: REPLIES.nameRefused });
        await page.getByLabel('Name').fill('Kitchen');
        await page.locator('#name-form').getByRole('button', { name: 'Save' }).click();
        await expect(page.getByRole('status')).toHaveText(
            'Name Kitchen refused (409): not a name this player can hold: it is too long, or it could not be stored',
        );
        await expect(page.getByRole('status')).toHaveClass(/error/);
    });

    test('the slot width goes to PUT /slot-width, and the sink grows with it', async ({ page, stub }) => {
        await expect(page.getByRole('radio', { name: '32-bit' })).toBeChecked();
        await expect(page.locator('#slot-help')).toHaveText('This sink has 2 slots.');
        await page.getByRole('radio', { name: '16-bit' }).check();
        await expect.poll(() => stub.sent('PUT /slot-width').map((r) => r.body)).toEqual(['16']);
        await expect(page.getByRole('status')).toHaveText('16-bit slots from the next play.');
        // Eight 16-bit slots a line where four 32-bit ones fitted.
        await expect(page.locator('#slot-help')).toHaveText('This sink has 8 slots.');
    });

    test('a slot width refused while a play runs shows the device\'s reply', async ({ page, stub }) => {
        stub.device.framesPerPoll = 1;
        await startPlay(page, stub);
        await expect(page.locator('#state')).toHaveText('Playing');
        await page.getByRole('radio', { name: '16-bit' }).check();
        await expect(page.getByRole('status')).toContainText('16-bit slots refused (409)');
        // The choice goes back to what the board still has.
        await expect(page.getByRole('radio', { name: '32-bit' })).toBeChecked();
    });

    test('the wiring checkbox goes to PUT /wiring and doubles the slots', async ({ page, stub }) => {
        const wiring = page.getByLabel('A second I2S line is wired to a DAC');
        await expect(wiring).not.toBeChecked();
        await wiring.check();
        await expect.poll(() => stub.sent('PUT /wiring').map((r) => r.body)).toEqual(['1']);
        await expect(page.getByRole('status')).toHaveText('A second I2S line, from the next play.');
        await expect(page.locator('#slot-help')).toHaveText('This sink has 8 slots.');
        await wiring.uncheck();
        await expect.poll(() => stub.sent('PUT /wiring').map((r) => r.body)).toEqual(['1', '0']);
        await expect(page.getByRole('status')).toHaveText('One I2S line, from the next play.');
    });

    test('the network form sends the SSID and passphrase, and keeps neither', async ({ page, stub }) => {
        await page.getByLabel('Wi-Fi network').fill('attic');
        await page.getByLabel('Passphrase').fill('hunter2');
        await page.locator('#network-form').getByRole('button', { name: 'Save' }).click();
        await expect.poll(() => stub.sent('PUT /network').map((r) => r.body)).toEqual(['attic\nhunter2']);
        await expect(page.getByRole('status')).toHaveText('Stored attic for the next restart.');
        // The passphrase is cleared from the page once it has gone: a board's
        // page is on an open network and this one is over plain HTTP.
        await expect(page.getByLabel('Passphrase')).toHaveValue('');
        await expect.poll(() => stub.device.ssid).toBe('attic');
    });

    test('the passphrase can be shown while it is typed, and is hidden again once sent', async ({ page, stub }) => {
        const pass = page.getByLabel('Passphrase');
        const show = page.getByRole('button', { name: 'Show' });
        await expect(pass).toHaveAttribute('type', 'password');
        await expect(show).toHaveAttribute('aria-pressed', 'false');
        await pass.fill('hunter2');
        await show.click();
        await expect(pass).toHaveAttribute('type', 'text');
        await expect(show).toHaveAttribute('aria-pressed', 'true');
        await show.click();
        await expect(pass).toHaveAttribute('type', 'password');
        await show.click();
        await page.getByLabel('Wi-Fi network').fill('attic');
        await page.locator('#network-form').getByRole('button', { name: 'Save' }).click();
        await expect.poll(() => stub.sent('PUT /network').map((r) => r.body)).toEqual(['attic\nhunter2']);
        await expect(pass).toHaveValue('');
        await expect(pass).toHaveAttribute('type', 'password');
        await expect(show).toHaveAttribute('aria-pressed', 'false');
    });

    test('a network with no name is not sent', async ({ page, stub }) => {
        await page.getByLabel('Wi-Fi network').fill('  ');
        await page.locator('#network-form').getByRole('button', { name: 'Save' }).click();
        await expect(page.getByRole('status')).toHaveText('Enter a network name.');
        expect(stub.sent('PUT /network')).toHaveLength(0);
    });

    test('a setting whose connection closes unanswered says so', async ({ page, stub }) => {
        // The stand-in closes its idle connections before a drop, so the
        // request is not one Chromium sends again (stub.js, next()).
        stub.next('PUT /name', 'drop');
        await page.getByLabel('Name').fill('Kitchen');
        await page.locator('#name-form').getByRole('button', { name: 'Save' }).click();
        await expect(page.getByRole('status')).toHaveText('Name Kitchen: no connection.');
    });

    test('a play someone else started shows through to its end', async ({ page, stub }) => {
        // The page still reports a play; it just does not start one.
        stub.device.framesPerPoll = 125;
        await startPlay(page, stub, 'http://10.0.2.2:8000/other.ec3');
        await expect(page.locator('#state')).toHaveText('Playing');
        await expect(page.locator('#location')).toHaveText('http://10.0.2.2:8000/other.ec3');
        await expect(page.locator('#codec')).toHaveText('E-AC-3, 1 substream, dialnorm -31');
        await expect(page.locator('#state')).toHaveText('Finished (end of stream)');
        await expect(page.locator('#played')).toHaveText('0:08 (250 frames)');
    });

    test('Apply sends the output layout to PUT /layout, for the next play', async ({ page, stub }) => {
        const layout = page.getByLabel('Output layout');
        await expect(layout).toHaveValue('2.0');
        await layout.fill('1.0');
        await page.getByRole('button', { name: 'Apply' }).click();
        await expect.poll(() => stub.sent('PUT /layout').map((r) => r.body)).toEqual(['1.0']);
        await expect(page.getByRole('status')).toHaveText('Output layout 1.0 from the next play.');
        await expect(page.locator('#next')).toHaveText('1.0');
    });

    test("a layout's confirmation comes after a play's end that a poll already out brings", async ({ page, stub }) => {
        stub.device.framesPerPoll = 1;
        await startPlay(page, stub);
        await expect(page.locator('#state')).toHaveText('Playing');
        // A poll out while Apply is answered, whose answer then brings the
        // play's end: board/layouts.spec.js on CI, where the test reads a
        // play's end from the device before the page has.
        const release = stub.hold('GET /status');
        const polls = stub.sent('GET /status').length;
        await expect.poll(() => stub.sent('GET /status').length).toBe(polls + 1);
        stub.device.framesPerPoll = 250;
        await page.getByLabel('Output layout').fill('1.0');
        const answered = page.waitForEvent('requestfinished', (request) => request.url().endsWith('/layout'));
        await page.getByRole('button', { name: 'Apply' }).click();
        await answered;
        // Time for the page to take Apply's answer in before the poll's comes.
        await page.waitForTimeout(300);
        release();
        await expect(page.locator('#state')).toHaveText('Finished (end of stream)');
        await expect(page.getByRole('status')).toHaveText('Output layout 1.0 from the next play.');
    });

    test('a speaker list is a layout too', async ({ page, stub }) => {
        await page.getByLabel('Output layout').fill('L,R');
        await page.getByLabel('Output layout').press('Enter');
        await expect.poll(() => stub.sent('PUT /layout').map((r) => r.body)).toEqual(['L,R']);
        await expect(page.getByRole('status')).toHaveText('Output layout L,R from the next play.');
    });

    test('a layout the sink cannot carry is refused, and the page says why', async ({ page, stub }) => {
        await page.getByLabel('Output layout').fill('5.1');
        await page.getByRole('button', { name: 'Apply' }).click();
        await expect.poll(() => stub.sent('PUT /layout').map((r) => r.body)).toEqual(['5.1']);
        await expect(page.getByRole('status')).toHaveText('Output layout 5.1 refused (409): it needs 6 slots and this sink has 2.');
        await expect(page.locator('#next')).toHaveText('2.0');
    });

    test("a layout refused for what it says is refused in the device's words", async ({ page, stub }) => {
        // Not a name OutputLayout has, so the page has no count to give.
        await page.getByLabel('Output layout').fill('6.1');
        await page.getByRole('button', { name: 'Apply' }).click();
        await expect(page.getByRole('status')).toHaveText(
            'Output layout 6.1 refused (409): not a layout this player can play: check the name or the list, and that it has no more slots than the sink',
        );
    });

    test('a preset sends its layout to PUT /layout at once', async ({ page, stub }) => {
        await expect(page.getByRole('radio', { name: '2.0', exact: true })).toBeChecked();
        await page.getByRole('radio', { name: '1.0', exact: true }).check();
        await expect.poll(() => stub.sent('PUT /layout').map((r) => r.body)).toEqual(['1.0']);
        await expect(page.getByRole('status')).toHaveText('Output layout 1.0 from the next play.');
        await expect(page.getByLabel('Output layout')).toHaveValue('1.0');
        await expect(page.locator('#next')).toHaveText('1.0');
        await expect(page.getByRole('radio', { name: '1.0', exact: true })).toBeChecked();
    });

    test('a refused preset goes back to the layout the board has', async ({ page, stub }) => {
        stub.device.sinkSlots = 8;
        await page.reload();
        stub.next('PUT /layout', { status: 409, body: REPLIES.layoutRefused });
        await page.getByRole('radio', { name: '5.1', exact: true }).check();
        await expect(page.getByRole('status')).toHaveText(`Output layout 5.1 refused (409): ${REPLIES.layoutRefused.trim()}`);
        await expect(page.getByRole('radio', { name: '2.0', exact: true })).toBeChecked();
    });

    test('the presets the sink cannot carry cannot be chosen', async ({ page, stub }) => {
        const enabled = () =>
            page.locator('#layouts input').evaluateAll((presets) => presets.filter((p) => !p.disabled).map((p) => p.value));
        await expect.poll(enabled).toEqual(['1.0', '2.0']);
        await expect(page.getByRole('radio', { name: '5.1', exact: true })).toBeDisabled();
        await expect(page.getByLabel('Output layout')).toHaveAccessibleDescription(/This sink has 2 slots\.$/);
        stub.device.sinkSlots = 16;
        await page.reload();
        await expect.poll(enabled).toEqual(['1.0', '2.0', '5.1', '7.1', '5.1.2', '5.1.4', '7.1.4', '9.1.6']);
        await expect(page.getByLabel('Output layout')).toHaveAccessibleDescription(/This sink has 16 slots\.$/);
    });

    test("a play's own output, the speakers it leaves silent, and the next play's layout", async ({ page, stub }) => {
        Object.assign(stub.device, { sinkSlots: 12, layout: '7.1.4', framesPerPoll: 1 });
        await page.reload();
        // AC-3 5.1 in a 7.1.4 room: the rear surrounds and the heights have nothing.
        await startPlay(page, stub, 'http://10.0.2.2:8000/sample.ac3');
        await expect(page.locator('#output')).toHaveText('7.1.4, 12 slots: each channel on the speaker at its location');
        await expect(page.locator('#silent')).toHaveText('Lrs, Rrs, Vhl, Vhr, Lts, Rts: nothing in the stream for these');
        await expect(page.locator('#next')).toBeHidden();
        // A new layout is the next play's; this play keeps its own.
        await page.getByLabel('Output layout').fill('5.1');
        await page.getByRole('button', { name: 'Apply' }).click();
        await expect(page.getByRole('status')).toHaveText('Output layout 5.1 from the next play.');
        await expect(page.locator('#next')).toHaveText('5.1');
        await expect(page.locator('#output')).toHaveText(/^7\.1\.4, 12 slots/);
        // The 7.1.4 walk at 5.1: its rears and heights are spread over the room.
        await startPlay(page, stub, 'http://10.0.2.2:8000/714-walk.ec3');
        await expect(page.locator('#output')).toHaveText('5.1, 6 slots: each channel on the speaker at its location');
        await expect(page.locator('#channels')).toHaveText('12: L C R Ls Rs Lrs Rrs Vhl Vhr Lts Rts LFE');
        await expect(page.locator('#silent')).toBeHidden();
        await expect(page.locator('#next')).toBeHidden();
    });

    test('a layout of only spaces is not sent', async ({ page, stub }) => {
        await page.getByLabel('Output layout').fill('  ');
        await page.getByRole('button', { name: 'Apply' }).click();
        await expect(page.getByRole('status')).toHaveText('Enter an output layout.');
        expect(stub.sent('PUT /layout')).toHaveLength(0);
    });
});

// A poll a second up to `last`, each let finish before the clock moves on:
// moving it further at once would expire the answer's own four-second wait
// while the answer is still on its way.
async function everySecondUntil(page, last) {
    for (let second = 1; second <= last; second += 1) {
        await page.clock.runFor(1000);
        await expect(page.locator('#link')).toHaveText(`Status read at 12:00:0${second}.`);
    }
}

test.describe('with the clock held', () => {
    test.beforeEach(async ({ page, stub }) => {
        await page.clock.install({ time: new Date('2026-09-11T12:00:00Z') });
        await page.clock.pauseAt(new Date('2026-09-11T12:00:00Z'));
        await page.goto(stub.url);
        await expect(page.locator('#state')).toHaveText('Stopped');
    });

    test('a setting the device never answers times out after four seconds', async ({ page, stub }) => {
        stub.next('PUT /name', 'hang');
        await page.getByLabel('Name').fill('Kitchen');
        await page.locator('#name-form').getByRole('button', { name: 'Save' }).click();
        await expect.poll(() => stub.sent('PUT /name').length).toBe(1);
        await page.clock.runFor(4000);
        await expect(page.getByRole('status')).toHaveText('Name Kitchen: no answer in 4 s.');
    });

    test('a status read begun before a choice was answered does not undo it', async ({ page, stub }) => {
        // What the board said before the change, answered to a read that was
        // already out when the change went: the page keeps the choice, and the
        // read after it - held here until the clock moves - brings the board's.
        const before = await (await page.request.get(stub.url + 'status')).text();
        const release = stub.hold('GET /status');
        const polls = stub.sent('GET /status').length;
        await page.clock.runFor(1000);
        await expect.poll(() => stub.sent('GET /status').length).toBe(polls + 1);
        await page.getByRole('radio', { name: '16-bit' }).check();
        await expect.poll(() => stub.sent('PUT /slot-width').map((r) => r.body)).toEqual(['16']);
        await expect(page.getByRole('status')).toHaveText('16-bit slots from the next play.');
        stub.setStatus(before);
        release();
        stub.setStatus(null);
        await expect(page.locator('#link')).toHaveText('Status read at 12:00:01.');
        await expect(page.getByRole('radio', { name: '16-bit' })).toBeChecked();
        await page.clock.runFor(1);
        await expect.poll(() => stub.sent('GET /status').length).toBe(polls + 2);
        await expect(page.locator('#slot-help')).toHaveText('This sink has 8 slots.');
        await expect(page.getByRole('radio', { name: '16-bit' })).toBeChecked();
    });

    test('a message leaves after six seconds; an error stays until it is replaced or clicked', async ({ page, stub }) => {
        const toast = page.getByRole('status');
        // A second at a time, each poll let finish before the clock moves on
        // (see everySecondUntil below).
        const seconds = async (from, to) => {
            for (let second = from; second <= to; second += 1) {
                await page.clock.runFor(1000);
                await expect(page.locator('#link')).toHaveText(`Status read at 12:00:${String(second).padStart(2, '0')}.`);
            }
        };
        await page.getByLabel('Name').fill('Attic');
        await page.locator('#name-form').getByRole('button', { name: 'Save' }).click();
        await expect(toast).toHaveText('This sink is called Attic.');
        await seconds(1, 5);
        await expect(toast).not.toHaveClass(/gone/);
        await seconds(6, 6);
        await expect(toast).toHaveClass(/gone/);
        stub.next('PUT /name', { status: 409, body: REPLIES.nameRefused });
        await page.locator('#name-form').getByRole('button', { name: 'Save' }).click();
        await expect(toast).toHaveClass(/error/);
        await expect(toast).not.toHaveClass(/gone/);
        await seconds(7, 13);
        await expect(toast).not.toHaveClass(/gone/);
        await toast.click();
        await expect(toast).toHaveClass(/gone/);
        // Still read out: the text stays where a screen reader found it.
        await expect(toast).toContainText('refused (409)');
    });

    test("a play someone else started keeps the page's own clock lines coming", async ({ page, stub }) => {
        stub.device.framesPerPoll = 1;
        await startPlay(page, stub);
        // The page learns of it at its next poll rather than at once: nothing
        // it did started this play, so nothing it did asks the device again.
        await everySecondUntil(page, 7);
        await expect(page.locator('#state')).toHaveText('Playing');
    });
});
