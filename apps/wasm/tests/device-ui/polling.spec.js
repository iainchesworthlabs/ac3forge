// @ts-check
'use strict';

// How the page reads the device - planning/esp32-device-ui.md's "How the page
// updates": GET /status once a second while the page is visible, one at a
// time, none while it is hidden, every five seconds while the status cannot be
// read, and at once after an action. The clock is Playwright's, held still
// and moved by hand, so each interval is exact.

const { test, expect } = require('./fixtures');

const START = new Date('2026-09-11T12:00:00Z');

test.beforeEach(async ({ page, stub }) => {
    await page.clock.install({ time: START });
    await page.clock.pauseAt(START);
    await page.goto(stub.url);
    await expect(page.locator('#link')).toHaveText('Status read at 12:00:00.');
});

const polls = (stub) => stub.sent('GET /status').length;

async function setVisibility(page, state) {
    await page.evaluate((value) => {
        Object.defineProperty(document, 'visibilityState', { value, configurable: true });
        document.dispatchEvent(new Event('visibilitychange'));
    }, state);
}

test('once a second while the page is visible', async ({ page, stub }) => {
    expect(polls(stub)).toBe(1);
    for (const expected of [2, 3, 4]) {
        await page.clock.runFor(999);
        expect(polls(stub)).toBe(expected - 1);
        await page.clock.runFor(1);
        await expect.poll(() => polls(stub)).toBe(expected);
        await expect(page.locator('#link')).toHaveText(`Status read at 12:00:0${expected - 1}.`);
    }
});

test('never more than one at a time', async ({ page, stub }) => {
    const release = stub.hold('GET /status');
    await page.clock.runFor(1000);
    await expect.poll(() => polls(stub)).toBe(2);
    await page.clock.runFor(3000);
    expect(polls(stub)).toBe(2);
    release();
    await expect(page.locator('#link')).toHaveText('Status read at 12:00:04.');
    await page.clock.runFor(1000);
    await expect.poll(() => polls(stub)).toBe(3);
    expect(stub.statusInFlightMost()).toBe(1);
});

test('none while the page is hidden, and one as soon as it is shown', async ({ page, stub }) => {
    await setVisibility(page, 'hidden');
    await page.clock.runFor(10_000);
    expect(polls(stub)).toBe(1);
    await setVisibility(page, 'visible');
    await expect.poll(() => polls(stub)).toBe(2);
});

test('a poll that answers while the page is hidden schedules no other', async ({ page, stub }) => {
    const release = stub.hold('GET /status');
    await page.clock.runFor(1000);
    await expect.poll(() => polls(stub)).toBe(2);
    await setVisibility(page, 'hidden');
    release();
    await expect(page.locator('#link')).toHaveText('Status read at 12:00:01.');
    await page.clock.runFor(10_000);
    expect(polls(stub)).toBe(2);
});

test('a device that stops answering is tried every five seconds, and says since when', async ({ page, stub }) => {
    stub.next('GET /status', 'hang');
    await page.clock.runFor(1000);
    await expect.poll(() => polls(stub)).toBe(2);
    await page.clock.runFor(4000);
    await expect(page.getByRole('status')).toHaveText('No status: no answer in 4 s.');
    await expect(page.locator('#link')).toHaveText('No status since 12:00:05: no answer in 4 s.');
    await expect(page.locator('#link')).toHaveClass(/error/);
    await page.clock.runFor(4999);
    expect(polls(stub)).toBe(2);
    stub.next('GET /status', 'drop');
    await page.clock.runFor(1);
    await expect.poll(() => polls(stub)).toBe(3);
    // Still failing: the time it started failing stays, the reason changes.
    await expect(page.locator('#link')).toHaveText('No status since 12:00:05: no connection.');
    await page.clock.runFor(5000);
    await expect.poll(() => polls(stub)).toBe(4);
    await expect(page.getByRole('status')).toHaveText('The player is answering again.');
    await expect(page.locator('#link')).toHaveText('Status read at 12:00:15.');
    await expect(page.locator('#link')).not.toHaveClass(/error/);
});

test('an error status, a body that is not JSON, and JSON that is not an object', async ({ page, stub }) => {
    stub.next('GET /status', { status: 500, body: 'no' });
    await page.clock.runFor(1000);
    await expect(page.getByRole('status')).toHaveText('No status: GET /status answered 500.');
    for (const body of ['{"state":', '[1,2]', 'null']) {
        stub.setStatus(body);
        await page.clock.runFor(5000);
        await expect(page.locator('#link')).toHaveText(/: GET \/status sent no JSON object\.$/);
    }
});

test('an action reads the status at once, without waiting for the next poll', async ({ page, stub }) => {
    await page.getByRole('button', { name: 'Stop' }).click();
    await expect.poll(() => stub.sent('POST /stop').length).toBe(1);
    await expect.poll(() => polls(stub)).toBe(2);
});

test('a poll asked for while one is out runs as soon as that one ends', async ({ page, stub }) => {
    const release = stub.hold('GET /status');
    await page.clock.runFor(1000);
    await expect.poll(() => polls(stub)).toBe(2);
    await page.getByRole('button', { name: 'Stop' }).click();
    await expect.poll(() => stub.sent('POST /stop').length).toBe(1);
    release();
    await page.clock.runFor(1);
    await expect.poll(() => polls(stub)).toBe(3);
});

test('the state is announced when it changes, and the figures are not', async ({ page, stub }) => {
    stub.device.framesPerPoll = 50;
    await page.getByRole('button', { name: 'Play' }).click();
    await expect(page.getByRole('status')).toHaveText('Playing.');
    for (let i = 0; i < 3; i += 1) {
        await page.clock.runFor(1000);
        await expect(page.locator('#played')).toHaveText(new RegExp(`\\(${(i + 2) * 50} frames\\)`));
        await expect(page.getByRole('status')).toHaveText('Playing.');
    }
    await page.clock.runFor(2000);
    await expect(page.getByRole('status')).toHaveText('Finished (end of stream).');
});
