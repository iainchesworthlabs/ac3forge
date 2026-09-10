// @ts-check
'use strict';

// Every action the page offers, driven through the page against the stand-in
// device, and what the device was sent - planning/esp32-device-ui.md's "What
// the page does". The error paths are the device's own refusals (400, 409), a
// connection closed unanswered and a device that does not answer at all.

const { test, expect } = require('./fixtures');
const { REPLIES } = require('./stub');

const DEMO = 'http://10.0.2.2:8000/demo.ec3';

test.describe('with the clock running', () => {
    test.beforeEach(async ({ page, stub }) => {
        await page.goto(stub.url);
        await expect(page.locator('#state')).toHaveText('Stopped');
    });

    test('Play sends the location to POST /play and shows the play through to its end', async ({ page, stub }) => {
        stub.device.framesPerPoll = 125;
        const location = page.getByLabel('Location to play');
        await expect(location).toHaveValue(DEMO);
        await location.fill('http://10.0.2.2:8000/other.ec3');
        await page.getByRole('button', { name: 'Play' }).click();
        await expect.poll(() => stub.sent('POST /play').map((r) => r.body)).toEqual(['http://10.0.2.2:8000/other.ec3']);
        await expect(page.locator('#state')).toHaveText('Playing');
        await expect(page.locator('#location')).toHaveText('http://10.0.2.2:8000/other.ec3');
        await expect(page.locator('#codec')).toHaveText('E-AC-3, 1 substream, dialnorm -31');
        await expect(page.locator('#state')).toHaveText('Finished (end of stream)');
        await expect(page.getByRole('status')).toHaveText('Finished (end of stream).');
        await expect(page.locator('#played')).toHaveText('0:08 (250 frames)');
    });

    test('Enter in the location field plays what is in it', async ({ page, stub }) => {
        await page.getByLabel('Location to play').press('Enter');
        await expect.poll(() => stub.sent('POST /play').map((r) => r.body)).toEqual([DEMO]);
    });

    test("a refused play shows the device's reply", async ({ page, stub }) => {
        stub.next('POST /play', { status: 409, body: REPLIES.playRefused });
        await page.getByRole('button', { name: 'Play' }).click();
        await expect(page.getByRole('status')).toHaveText('Play refused (409): this source cannot play that');
        await expect(page.getByRole('status')).toHaveClass(/error/);
    });

    test('a play whose connection closes unanswered says so', async ({ page, stub }) => {
        // Chromium sends a request again, once, when a reused connection closes
        // before any answer, so the stand-in closes both.
        stub.next('POST /play', 'drop');
        stub.next('POST /play', 'drop');
        await page.getByRole('button', { name: 'Play' }).click();
        await expect(page.getByRole('status')).toHaveText('Play: no connection.');
    });

    test('a location of only spaces is not sent', async ({ page, stub }) => {
        await page.getByLabel('Location to play').fill('   ');
        await page.getByRole('button', { name: 'Play' }).click();
        await expect(page.getByRole('status')).toHaveText('Enter a location to play.');
        expect(stub.sent('POST /play')).toHaveLength(0);
    });

    test('Stop sends POST /stop', async ({ page, stub }) => {
        await page.getByRole('button', { name: 'Stop' }).click();
        await expect.poll(() => stub.sent('POST /stop').length).toBe(1);
        await expect(page.getByRole('status')).toHaveText('Asked the player to stop.');
    });

    test('Stop ends a play in progress', async ({ page, stub }) => {
        stub.device.framesPerPoll = 1;
        await page.getByRole('button', { name: 'Play' }).click();
        await expect(page.locator('#state')).toHaveText('Playing');
        await page.getByRole('button', { name: 'Stop' }).click();
        await expect(page.locator('#state')).toHaveText('Stopped');
        await expect(page.getByRole('status')).toHaveText('Stopped.');
    });

    test("a refused stop shows the device's reply", async ({ page, stub }) => {
        stub.next('POST /stop', { status: 500, body: 'no\n' });
        await page.getByRole('button', { name: 'Stop' }).click();
        await expect(page.getByRole('status')).toHaveText('Stop refused (500): no');
    });

    test('the volume slider sends its value to POST /volume', async ({ page, stub }) => {
        const slider = page.getByRole('slider', { name: 'Volume' });
        await expect(slider).toHaveValue('100');
        await slider.fill('25');
        await expect.poll(() => stub.sent('POST /volume').map((r) => r.body)).toEqual(['0.25']);
        await expect(page.locator('#volume-out')).toHaveText('25%');
        await expect(slider).toHaveAttribute('aria-valuetext', '25%');
        await expect.poll(() => stub.device.volume).toBe(0.25);
        // The poll after it reads the same volume back, and the slider stays.
        await expect.poll(() => stub.sent('GET /status').length).toBeGreaterThan(2);
        await expect(slider).toHaveValue('25');
    });

    test('volume changes made while one is unanswered go as one request, the latest', async ({ page, stub }) => {
        const slider = page.getByRole('slider', { name: 'Volume' });
        const release = stub.hold('POST /volume');
        await slider.fill('10');
        await expect.poll(() => stub.sent('POST /volume').length).toBe(1);
        for (const value of ['20', '30', '40']) {
            await slider.fill(value);
        }
        await page.waitForTimeout(300);
        expect(stub.sent('POST /volume')).toHaveLength(1);
        release();
        await expect.poll(() => stub.sent('POST /volume').map((r) => r.body)).toEqual(['0.10', '0.40']);
        await expect.poll(() => stub.device.volume).toBe(0.4);
        await expect(slider).toHaveValue('40');
    });

    test("a refused volume shows the device's reply", async ({ page, stub }) => {
        stub.next('POST /volume', { status: 409, body: REPLIES.volumeRefused });
        await page.getByRole('slider', { name: 'Volume' }).fill('30');
        await expect(page.getByRole('status')).toHaveText('Volume 30% refused (409): this sink has no volume to set');
    });

    test('a volume whose connection closes unanswered says so', async ({ page, stub }) => {
        stub.next('POST /volume', 'drop');
        stub.next('POST /volume', 'drop');
        await page.getByRole('slider', { name: 'Volume' }).fill('30');
        await expect(page.getByRole('status')).toHaveText('Volume 30%: no connection.');
    });

    test('Apply sends the layout to PUT /layout, for the next play', async ({ page, stub }) => {
        const layout = page.getByLabel('Layout');
        await expect(layout).toHaveValue('2.0');
        await layout.fill('1.0');
        await page.getByRole('button', { name: 'Apply' }).click();
        await expect.poll(() => stub.sent('PUT /layout').map((r) => r.body)).toEqual(['1.0']);
        await expect(page.getByRole('status')).toHaveText('Layout 1.0 from the next play.');
        await expect(page.locator('#layout')).toHaveText('1.0');
    });

    test('a speaker list is a layout too', async ({ page, stub }) => {
        await page.getByLabel('Layout').fill('L,R');
        await page.getByLabel('Layout').press('Enter');
        await expect.poll(() => stub.sent('PUT /layout').map((r) => r.body)).toEqual(['L,R']);
        await expect(page.getByRole('status')).toHaveText('Layout L,R from the next play.');
    });

    test("a layout the sink cannot carry is refused with the device's reply", async ({ page, stub }) => {
        await page.getByLabel('Layout').fill('5.1');
        await page.getByRole('button', { name: 'Apply' }).click();
        await expect(page.getByRole('status')).toHaveText(
            'Layout 5.1 refused (409): not a layout this player can play: check the name or the list, and that it has no more slots than the sink',
        );
        await expect(page.locator('#layout')).toHaveText('2.0');
    });

    test('a layout of only spaces is not sent', async ({ page, stub }) => {
        await page.getByLabel('Layout').fill('  ');
        await page.getByRole('button', { name: 'Apply' }).click();
        await expect(page.getByRole('status')).toHaveText('Enter a layout.');
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

    test('a play the device never answers times out after four seconds', async ({ page, stub }) => {
        stub.next('POST /play', 'hang');
        await page.getByRole('button', { name: 'Play' }).click();
        await expect.poll(() => stub.sent('POST /play').length).toBe(1);
        await page.clock.runFor(4000);
        await expect(page.getByRole('status')).toHaveText('Play: no answer in 4 s.');
    });

    test('a location the player does not take is reported once it has had time to', async ({ page, stub }) => {
        await page.getByLabel('Location to play').fill('ftp://10.0.2.2/demo.ec3');
        await page.getByRole('button', { name: 'Play' }).click();
        await expect(page.getByRole('status')).toHaveText('Asked the player to play ftp://10.0.2.2/demo.ec3.');
        await expect.poll(() => stub.sent('GET /status').length).toBe(2);
        await everySecondUntil(page, 6);
        await expect(page.getByRole('status')).toHaveText(
            `The player did not take ftp://10.0.2.2/demo.ec3; it is still on ${DEMO}.`,
        );
    });

    test('replaying what is already there is not watched for a refusal', async ({ page, stub }) => {
        stub.device.framesPerPoll = 1;
        await page.getByRole('button', { name: 'Play' }).click();
        await expect(page.locator('#state')).toHaveText('Playing');
        await everySecondUntil(page, 7);
        await expect(page.getByRole('status')).toHaveText('Playing.');
    });

    test('volume requests go at least 200 ms apart', async ({ page, stub }) => {
        const slider = page.getByRole('slider', { name: 'Volume' });
        await slider.fill('10');
        await expect.poll(() => stub.sent('POST /volume').length).toBe(1);
        await expect.poll(() => stub.sent('GET /status').length).toBe(2);
        await slider.fill('20');
        await page.waitForTimeout(200);
        expect(stub.sent('POST /volume')).toHaveLength(1);
        await page.clock.runFor(200);
        await expect.poll(() => stub.sent('POST /volume').map((r) => r.body)).toEqual(['0.10', '0.20']);
    });

    test('a volume the device never answers times out', async ({ page, stub }) => {
        stub.next('POST /volume', 'hang');
        await page.getByRole('slider', { name: 'Volume' }).fill('30');
        await expect.poll(() => stub.sent('POST /volume').length).toBe(1);
        await page.clock.runFor(4000);
        await expect(page.getByRole('status')).toHaveText('Volume 30%: no answer in 4 s.');
    });
});
