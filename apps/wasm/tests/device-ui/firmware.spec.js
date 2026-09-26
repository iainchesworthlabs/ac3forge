// @ts-check
'use strict';

// The firmware section - planning/esp32-device-ui.md, decision 29, and
// planning/esp32-ota.md's O3: what GET /firmware says of both slots, a trial,
// an upload and the last update; an update from a file, checked here on its
// head before anything is sent, sent with the bytes shown, and the new
// image's page loaded once the board runs it; Restart and Roll back, each
// behind the dialog. stub.js models the routes as ac3forge::Firmware answers
// them.

const { test, expect } = require('./fixtures');
const { appImage, defaultHardware, emptySlot, firmwareSlot, firmwareTrial } = require('./stub');

const RUNNING = 'v0.10.0-beta.1-42-gee9cf4f';

// A file for the page's file input.
const file = (buffer, name = 'ac3forge_hearth_sink.bin') => ({ name, mimeType: 'application/octet-stream', buffer });

// Whether leaving the page now would be asked about.
const leavingAsks = (page) =>
    page.evaluate(() => {
        const event = new Event('beforeunload', { cancelable: true });
        dispatchEvent(event);
        return event.defaultPrevented;
    });

test.describe('what the board runs', () => {
    test('a firmware that takes no updates has no section, and is asked once', async ({ page, stub }) => {
        stub.device.firmware = undefined;
        await page.goto(stub.url);
        await expect.poll(() => stub.sent('GET /firmware').length).toBe(1);
        await expect(page.getByRole('region', { name: 'Firmware' })).toBeHidden();
        await expect.poll(() => stub.sent('GET /status').length).toBeGreaterThanOrEqual(3);
        expect(stub.sent('GET /firmware')).toHaveLength(1);
    });

    test('a board on its first image: the running slot, an empty one, Update and Restart', async ({ page, stub }) => {
        await page.goto(stub.url);
        const section = page.getByRole('region', { name: 'Firmware' });
        await expect(page.locator('#fw-running')).toHaveText(RUNNING + ' in ota_0, accepted, intact');
        await expect(page.locator('#fw-other')).toHaveText('Empty');
        for (const id of ['#fw-trial', '#fw-upload', '#fw-last', '#fw-crash', '#fw-note', '#fw-sent', '#fw-dump']) {
            await expect(page.locator(id)).toBeHidden();
        }
        await expect(section.getByRole('button', { name: 'Update firmware…' })).toBeVisible();
        await expect(section.getByRole('button', { name: 'Restart' })).toBeVisible();
        await expect(section.getByRole('button', { name: /^Roll back/ })).toBeHidden();
        // Read once: nothing about it changes while the board runs.
        await expect.poll(() => stub.sent('GET /status').length).toBeGreaterThanOrEqual(3);
        expect(stub.sent('GET /firmware')).toHaveLength(1);
    });

    test('each state a slot can be in, what its check found, and when it can be gone back to', async ({ page, stub }) => {
        for (const [fields, text, back] of [
            [{ state: 'valid', intact: true }, 'v0.9.0 in ota_1, accepted, intact', true],
            [{ state: 'valid', intact: null }, 'v0.9.0 in ota_1, accepted', true],
            [{ state: 'valid', intact: false }, 'v0.9.0 in ota_1, accepted, damaged', false],
            [{ state: 'new', intact: null }, 'v0.9.0 in ota_1, written, not started yet', true],
            [{ state: 'invalid', intact: true }, 'v0.9.0 in ota_1, failed its trial, intact', false],
            [{ state: 'aborted', intact: true }, 'v0.9.0 in ota_1, stopped during its trial, intact', false],
            // A state the page has no word for: the image and its check alone.
            [{ state: 'undefined', intact: true }, 'v0.9.0 in ota_1, intact', true],
        ]) {
            stub.device.firmware.other = firmwareSlot({ label: 'ota_1', version: 'v0.9.0', ...fields });
            await page.goto(stub.url);
            await expect(page.locator('#fw-other')).toHaveText(text);
            const rollBack = page.getByRole('button', { name: 'Roll back to v0.9.0' });
            await (back ? expect(rollBack).toBeVisible() : expect(rollBack).toBeHidden());
        }
    });

    // Apart from the states above, which load the page once for each: the
    // coverage of a page is what its last load ran.
    test('a damaged image is not offered to go back to', async ({ page, stub }) => {
        stub.device.firmware.other = firmwareSlot({ label: 'ota_1', version: 'v0.9.0', intact: false });
        await page.goto(stub.url);
        await expect(page.locator('#fw-other')).toHaveText('v0.9.0 in ota_1, accepted, damaged');
        await expect(page.getByRole('button', { name: /^Roll back/ })).toBeHidden();
    });

    test('a board with one app slot offers no update and nothing to go back to', async ({ page, stub }) => {
        stub.device.firmware.other = null;
        await page.goto(stub.url);
        await expect(page.locator('#fw-running')).toBeVisible();
        await expect(page.locator('#fw-other')).toBeHidden();
        await expect(page.getByRole('button', { name: 'Update firmware…' })).toBeHidden();
        await expect(page.getByRole('button', { name: /^Roll back/ })).toBeHidden();
        await expect(page.getByRole('button', { name: 'Restart' })).toBeVisible();
    });

    test('a trial: how long it has held, what it waits for, and the one way out it has', async ({ page, stub }) => {
        Object.assign(stub.device.firmware, {
            running: firmwareSlot({ label: 'ota_1', state: 'trial', version: 'v0.11.0', intact: null }),
            other: firmwareSlot({ label: 'ota_0' }),
            trial: firmwareTrial({ healthy_for_ms: 12400, remaining_ms: 287600, waiting_for: ['the Sendspin player'] }),
            last_update: { version: 'v0.11.0', result: 'on trial', reason: '' },
        });
        await page.goto(stub.url);
        await expect(page.locator('#fw-running')).toHaveText('v0.11.0 in ota_1, on trial');
        await expect(page.locator('#fw-trial')).toHaveText('12 of 30 s held, 4:48 left; waiting for the Sendspin player');
        await expect(page.locator('#fw-last')).toHaveText('v0.11.0, on trial');
        await expect(page.getByRole('button', { name: 'Update firmware…' })).toBeHidden();
        // A restart now would be a rollback, so the board refuses one.
        await expect(page.getByRole('button', { name: 'Restart' })).toBeHidden();
        await expect(page.getByRole('button', { name: 'Roll back to ' + RUNNING })).toBeVisible();

        // Read again at every poll while the trial runs.
        stub.device.firmware.trial = firmwareTrial({ healthy_for_ms: 29000, remaining_ms: 250000, waiting_for: [] });
        await expect(page.locator('#fw-trial')).toHaveText('29 of 30 s held, 4:10 left');

        // Accepted: the trial goes, and with it the reading at every poll.
        Object.assign(stub.device.firmware, {
            running: { ...stub.device.firmware.running, state: 'valid' },
            trial: null,
            last_update: { version: 'v0.11.0', result: 'accepted', reason: '' },
        });
        await expect(page.locator('#fw-trial')).toBeHidden();
        await expect(page.locator('#fw-running')).toHaveText('v0.11.0 in ota_1, accepted');
        await expect(page.locator('#fw-last')).toHaveText('v0.11.0, accepted');
        await expect(page.getByRole('button', { name: 'Restart' })).toBeVisible();
        const reads = stub.sent('GET /firmware').length;
        const polls = stub.sent('GET /status').length;
        await expect.poll(() => stub.sent('GET /status').length).toBeGreaterThanOrEqual(polls + 2);
        expect(stub.sent('GET /firmware')).toHaveLength(reads);
    });

    test("an update another client is sending: flash mode, its stage and bytes, then how it ended", async ({ page, stub }) => {
        Object.assign(stub.device.firmware, { mode: 'flash', upload: { received: 524288, total: 1467104, stage: 'writing' } });
        await page.goto(stub.url);
        await expect(page.locator('#fw-note')).toHaveText('Flash mode: nothing plays until the board restarts.');
        // /status's state in flash mode, in the page's words.
        stub.device.state = 'flash';
        await expect(page.locator('#state')).toHaveText('Flash mode');
        await expect(page.getByRole('status')).toHaveText('Flash mode.');
        await expect(page.locator('#fw-upload')).toHaveText('Writing: 524,288 of 1,467,104 bytes');
        for (const name of ['Update firmware…', 'Restart']) {
            await expect(page.getByRole('button', { name })).toBeHidden();
        }
        // A stage the page has no word for, as the firmware says it.
        stub.device.firmware.upload = { received: 1467104, total: 1467104, stage: 'verifying' };
        await expect(page.locator('#fw-upload')).toHaveText('verifying: 1,467,104 of 1,467,104 bytes');
        // Refused on its head: still in flash mode, and a restart is the way out.
        Object.assign(stub.device.firmware, {
            upload: null,
            last_update: { version: '', result: 'refused', reason: 'that body is too short to be an application image' },
        });
        await expect(page.locator('#fw-last')).toHaveText('refused: that body is too short to be an application image');
        await expect(page.locator('#fw-upload')).toBeHidden();
        await expect(page.locator('#fw-note')).toBeVisible();
        await expect(page.getByRole('button', { name: 'Restart' })).toBeVisible();
    });

    test('why the last update went back', async ({ page, stub }) => {
        stub.device.firmware.last_update = { version: 'o2-panic', result: 'rolled back', reason: 'it panicked' };
        await page.goto(stub.url);
        await expect(page.locator('#fw-last')).toHaveText('o2-panic, rolled back: it panicked');
    });

    test('the core dump a crash left, and the link that saves it', async ({ page, stub }) => {
        stub.device.firmware.coredump = {
            bytes: 23456, intact: true, task: 'fw_trial', pc: '0x4037a1b2',
            reason: 'abort() was called at PC 0x4200abcd on core 0', elf_sha256: '2366bde99',
        };
        stub.device.coredumpBytes = Buffer.from('a core dump');
        await page.goto(stub.url);
        await expect(page.locator('#fw-crash')).toHaveText(
            'fw_trial at 0x4037a1b2; abort() was called at PC 0x4200abcd on core 0; 23,456 bytes',
        );
        const save = page.getByRole('link', { name: 'Save the core dump' });
        await expect(save).toHaveAttribute('href', 'firmware/coredump');
        const download = page.waitForEvent('download');
        await save.click();
        expect((await download).suggestedFilename()).toBe('coredump.bin');
        await expect(page.getByRole('link', { name: 'Recent console output' })).toHaveAttribute('href', 'log');
    });

    test('a core dump that does not check out, and names no task', async ({ page, stub }) => {
        stub.device.firmware.coredump = { bytes: 23456, intact: false, task: '', pc: '', reason: '', elf_sha256: '' };
        await page.goto(stub.url);
        await expect(page.locator('#fw-crash')).toHaveText('23,456 bytes, damaged');
    });

    test('a GET /firmware that is not JSON is read again at the next poll', async ({ page, stub }) => {
        stub.next('GET /firmware', { status: 200, body: '{"mode":', type: 'application/json' });
        await page.goto(stub.url);
        await expect(page.locator('#fw-running')).toHaveText(RUNNING + ' in ota_0, accepted, intact');
        expect(stub.sent('GET /firmware')).toHaveLength(2);
    });

    test('one GET /firmware at a time: a read that hangs is not joined by another', async ({ page, stub }) => {
        // On trial, so that every poll would read it.
        Object.assign(stub.device.firmware, {
            running: firmwareSlot({ label: 'ota_1', state: 'trial', version: 'v0.11.0', intact: null }),
            other: firmwareSlot({ label: 'ota_0' }),
            trial: firmwareTrial(),
        });
        await page.goto(stub.url);
        await expect(page.locator('#fw-trial')).toBeVisible();
        const before = stub.sent('GET /firmware').length;
        stub.next('GET /firmware', 'hang');
        await expect.poll(() => stub.sent('GET /firmware').length).toBe(before + 1);
        const reads = stub.sent('GET /firmware').length;
        const polls = stub.sent('GET /status').length;
        await expect.poll(() => stub.sent('GET /status').length).toBeGreaterThanOrEqual(polls + 2);
        expect(stub.sent('GET /firmware')).toHaveLength(reads);
        // Four seconds on, the read gives up, and the next poll reads again.
        stub.device.firmware.trial = firmwareTrial({ healthy_for_ms: 20000 });
        await expect(page.locator('#fw-trial')).toHaveText(/^20 of 30 s held/, { timeout: 8000 });
    });
});

test.describe('an update from a file', () => {
    test.beforeEach(async ({ page, stub }) => {
        await page.goto(stub.url);
        await expect(page.locator('#fw-running')).toHaveText(RUNNING + ' in ota_0, accepted, intact');
        // The chip and project an image is held to.
        await expect(page.locator('#hw-chip')).toHaveText('ESP32-S3, revision 0.2');
    });

    test('the image is named, sent with its bytes shown, and the new image’s page is loaded', async ({ page, stub }) => {
        const image = appImage({ version: 'v0.11.0', size: 256 * 1024 });
        const answer = stub.hold('PUT /firmware');
        const chooser = page.waitForEvent('filechooser');
        await page.getByRole('button', { name: 'Update firmware…' }).click();
        await (await chooser).setFiles(file(image));
        const dialog = page.getByRole('dialog', { name: 'Update to v0.11.0?' });
        await expect(dialog).toContainText('the board writes ac3forge_hearth_sink.bin into ota_1, then restarts into it');
        await expect(dialog).toContainText('goes back to ' + RUNNING + ' by itself if not');
        expect(await leavingAsks(page)).toBe(false);
        await dialog.getByRole('button', { name: 'Update', exact: true }).click();

        // Sent in full; the board is checking it and has not answered.
        await expect(page.locator('#fw-upload')).toHaveText('Sent. The board checks the image before it answers.');
        await expect(page.locator('#fw-sent')).toBeVisible();
        await expect(page.locator('#fw-sent')).toHaveJSProperty('value', 1);
        for (const name of ['Update firmware…', 'Restart']) {
            await expect(page.getByRole('button', { name })).toBeHidden();
        }
        expect(await leavingAsks(page)).toBe(true);
        await expect.poll(() => stub.sent('PUT /firmware').length).toBe(1);
        const [put] = stub.sent('PUT /firmware');
        expect(put.headers['content-type']).toBe('application/octet-stream');
        expect(put.raw.equals(image)).toBe(true);
        // The status is still read while the image goes, GET /firmware not.
        const reads = stub.sent('GET /firmware').length;
        const polls = stub.sent('GET /status').length;
        await expect.poll(() => stub.sent('GET /status').length).toBeGreaterThanOrEqual(polls + 2);
        expect(stub.sent('GET /firmware')).toHaveLength(reads);

        answer();
        // The board restarts, answers again running the new image, and the
        // page it serves is loaded. (What the page says while it restarts is
        // Restart's test, on a held clock: here a poll already out can meet
        // the restart before the answer has.)
        await expect.poll(() => stub.sent('GET /').length, { timeout: 15000 }).toBe(2);
        await expect(page.locator('#fw-running')).toHaveText('v0.11.0 in ota_1, on trial');
        await expect(page.locator('#fw-trial')).toHaveText('0 of 30 s held, 5:00 left; waiting for a network address, the Sendspin player');
        await expect(page.locator('#fw-last')).toHaveText('v0.11.0, on trial');
        await expect(page.locator('#fw-other')).toHaveText(RUNNING + ' in ota_0, accepted, intact');
        await expect(page.locator('#hw-firmware')).toHaveText('ac3forge_hearth_sink v0.11.0');
    });

    test('an image the board takes is announced, and leaving is no longer asked about', async ({ page, stub }) => {
        // The board's answer without the restart that follows it, so that
        // the announcement stands until the status is read.
        stub.next('PUT /firmware', {
            status: 200,
            body: '{"version":"v0.11.0","slot":"ota_1","sha256":"00","restarting":true}\n',
            type: 'application/json',
        });
        await page.setInputFiles('#fw-file', file(appImage()));
        await page.getByRole('dialog').getByRole('button', { name: 'Update', exact: true }).click();
        await expect(page.getByRole('status')).toHaveText('Written and checked: the board restarts into v0.11.0, on trial.');
        await expect(page.getByRole('status')).not.toHaveClass(/error/);
        expect(await leavingAsks(page)).toBe(false);
    });

    test('Cancel sends nothing, and the same file can be chosen again', async ({ page, stub }) => {
        for (const answer of ['Cancel', 'Cancel']) {
            await page.setInputFiles('#fw-file', file(appImage()));
            const dialog = page.getByRole('dialog', { name: 'Update to v0.11.0?' });
            await dialog.getByRole('button', { name: answer }).click();
            await expect(dialog).toBeHidden();
        }
        // The chooser closed with nothing chosen.
        await page.setInputFiles('#fw-file', []);
        await expect(page.getByRole('dialog')).toBeHidden();
        expect(stub.sent('PUT /firmware')).toHaveLength(0);
    });

    for (const [what, image, why] of [
        ['a file that is not an image', appImage({ magic: 0 }), 'it is not an application image; choose ac3forge_hearth_sink.bin'],
        ['an image with no app description', (() => {
            const b = appImage();
            b.writeUInt32LE(0, 32);
            return b;
        })(), 'it is not an application image; choose ac3forge_hearth_sink.bin'],
        ['a file shorter than a head', Buffer.alloc(64, 0xe9), 'it is not an application image; choose ac3forge_hearth_sink.bin'],
        ['an image for another chip', appImage({ chip: 13 }), 'it is for another chip than this ESP32-S3'],
        ['an image of another project', appImage({ project: 'ac3forge_stream_player' }), 'it is ac3forge_stream_player, not ac3forge_hearth_sink'],
    ]) {
        test(`${what} is not sent, since an upload stops what plays first`, async ({ page, stub }) => {
            await page.setInputFiles('#fw-file', file(image, 'mine.bin'));
            await expect(page.getByRole('status')).toHaveText('mine.bin was not sent: ' + why + '.');
            await expect(page.getByRole('status')).toHaveClass(/error/);
            await expect(page.getByRole('dialog')).toBeHidden();
            expect(stub.sent('PUT /firmware')).toHaveLength(0);
        });
    }

    test('the board refuses an image: its reply, and the section as the board then is', async ({ page, stub }) => {
        stub.device.firmware.slot_bytes = 1024;
        const answer = stub.hold('PUT /firmware');
        await page.setInputFiles('#fw-file', file(appImage({ size: 2048 })));
        await page.getByRole('dialog').getByRole('button', { name: 'Update', exact: true }).click();
        await expect.poll(() => stub.sent('PUT /firmware').length).toBe(1);
        // Leaving while it is out would end it.
        expect(await leavingAsks(page)).toBe(true);
        answer();
        await expect(page.getByRole('status')).toHaveText('Update refused (413): that image is 2048 bytes, and the slot holds 1024');
        await expect(page.getByRole('status')).toHaveClass(/error/);
        await expect(page.getByRole('button', { name: 'Update firmware…' })).toBeVisible();
        await expect(page.locator('#fw-upload')).toBeHidden();
        expect(await leavingAsks(page)).toBe(false);
    });

    test('refused once flash mode had begun: the board says why, and plays nothing until a restart', async ({ page, stub }) => {
        stub.next('PUT /firmware', {
            status: 400,
            body: "this image was built for 4 MB of flash, and this board's is set for 16 MB of flash\n",
        });
        await page.setInputFiles('#fw-file', file(appImage()));
        Object.assign(stub.device.firmware, {
            mode: 'flash',
            last_update: { version: 'v0.11.0', result: 'refused', reason: "this image was built for 4 MB of flash, and this board's is set for 16 MB of flash" },
        });
        await page.getByRole('dialog').getByRole('button', { name: 'Update', exact: true }).click();
        await expect(page.getByRole('status')).toHaveText(
            "Update refused (400): this image was built for 4 MB of flash, and this board's is set for 16 MB of flash",
        );
        await expect(page.locator('#fw-note')).toBeVisible();
        await expect(page.locator('#fw-last')).toHaveText(
            "v0.11.0, refused: this image was built for 4 MB of flash, and this board's is set for 16 MB of flash",
        );
        await expect(page.getByRole('button', { name: 'Restart' })).toBeVisible();
    });

    test('an upload whose connection closes unanswered says so', async ({ page, stub }) => {
        stub.next('PUT /firmware', 'drop');
        await page.setInputFiles('#fw-file', file(appImage()));
        await page.getByRole('dialog').getByRole('button', { name: 'Update', exact: true }).click();
        await expect(page.getByRole('status')).toHaveText('Update: no connection.');
        await expect(page.getByRole('status')).toHaveClass(/error/);
    });

    test('on a board whose chip and project the page does not know, the board is left to judge', async ({ page, stub }) => {
        stub.device.hardware = { ...defaultHardware(), target: 'esp32c5', chip: 'ESP32-C5', project: undefined };
        await page.reload();
        await expect(page.locator('#hw-chip')).toHaveText('ESP32-C5, revision 0.2');
        await page.setInputFiles('#fw-file', file(appImage({ chip: 23, project: 'anything' })));
        await expect(page.getByRole('dialog', { name: 'Update to v0.11.0?' })).toBeVisible();
    });
});

test.describe('restarting and going back', () => {
    test('Restart asks first, and the page says the board is restarting rather than failing', async ({ page, stub }) => {
        // On a held clock, so that the one poll to meet the restart is the
        // one the action makes, and down until the test brings it back.
        stub.device.restartDrops = Infinity;
        const start = new Date('2026-09-25T12:00:00Z');
        await page.clock.install({ time: start });
        await page.clock.pauseAt(start);
        await page.goto(stub.url);
        const restart = page.getByRole('button', { name: 'Restart' });
        await restart.click();
        const dialog = page.getByRole('dialog', { name: 'Restart the board?' });
        await expect(dialog).toContainText('What plays stops, and the board starts ' + RUNNING + ' again.');
        await dialog.getByRole('button', { name: 'Cancel' }).click();
        expect(stub.sent('POST /restart')).toHaveLength(0);

        const reads = stub.sent('GET /firmware').length;
        await restart.click();
        await dialog.getByRole('button', { name: 'Restart', exact: true }).click();
        await expect.poll(() => stub.sent('POST /restart').length).toBe(1);
        await expect(page.getByRole('status')).toHaveText('The board is restarting.');
        await expect(page.getByRole('status')).not.toHaveClass(/error/);
        // Back: the try five seconds on finds it.
        stub.device.down = 0;
        await page.clock.runFor(5000);
        await expect(page.getByRole('status')).toHaveText('The player is answering again.');
        // Read again, since it may run another image now; this one it ran before.
        await expect.poll(() => stub.sent('GET /firmware').length).toBeGreaterThan(reads);
        expect(stub.sent('GET /')).toHaveLength(1);
    });

    test('a restart the board refuses shows its reply', async ({ page, stub }) => {
        stub.next('POST /restart', { status: 409, body: 'an update is under way\n' });
        await page.goto(stub.url);
        await page.getByRole('button', { name: 'Restart' }).click();
        await page.getByRole('dialog').getByRole('button', { name: 'Restart', exact: true }).click();
        await expect(page.getByRole('status')).toHaveText('Restart refused (409): an update is under way');
    });

    test('Roll back names the image, asks first, and loads its page once the board runs it', async ({ page, stub }) => {
        stub.device.firmware.other = firmwareSlot({ label: 'ota_1', version: 'v0.9.0' });
        await page.goto(stub.url);
        await page.getByRole('button', { name: 'Roll back to v0.9.0' }).click();
        const dialog = page.getByRole('dialog', { name: 'Roll back to v0.9.0?' });
        await expect(dialog).toContainText('What plays stops, and the board restarts into v0.9.0.');
        await dialog.getByRole('button', { name: 'Roll back', exact: true }).click();
        await expect.poll(() => stub.sent('PUT /firmware/rollback').length).toBe(1);
        await expect.poll(() => stub.sent('GET /').length, { timeout: 15000 }).toBe(2);
        await expect(page.locator('#fw-running')).toHaveText('v0.9.0 in ota_1, on trial');
        await expect(page.locator('#fw-last')).toHaveText('v0.9.0, rollback requested');
    });

    test('Roll back during a trial gives it up, and the page of the image before is loaded', async ({ page, stub }) => {
        Object.assign(stub.device.firmware, {
            running: firmwareSlot({ label: 'ota_1', state: 'trial', version: 'v0.11.0', intact: null }),
            other: firmwareSlot({ label: 'ota_0' }),
            trial: firmwareTrial(),
        });
        await page.goto(stub.url);
        await page.getByRole('button', { name: 'Roll back to ' + RUNNING }).click();
        await page.getByRole('dialog').getByRole('button', { name: 'Roll back', exact: true }).click();
        await expect.poll(() => stub.sent('GET /').length, { timeout: 15000 }).toBe(2);
        await expect(page.locator('#fw-running')).toHaveText(RUNNING + ' in ota_0, accepted, intact');
        await expect(page.locator('#fw-other')).toHaveText('v0.11.0 in ota_1, failed its trial');
        await expect(page.locator('#fw-last')).toHaveText('v0.11.0, rolled back: rolled back by request during its trial');
    });

    test('after a restart it asked for, the page reads the firmware at each poll for a minute', async ({ page, stub }) => {
        stub.device.restartDrops = 0;
        const start = new Date('2026-09-25T12:00:00Z');
        await page.clock.install({ time: start });
        await page.clock.pauseAt(start);
        await page.goto(stub.url);
        await expect(page.locator('#fw-running')).toHaveText(RUNNING + ' in ota_0, accepted, intact');
        await page.getByRole('button', { name: 'Restart' }).click();
        await page.getByRole('dialog').getByRole('button', { name: 'Restart', exact: true }).click();
        await expect.poll(() => stub.sent('POST /restart').length).toBe(1);
        const reads = () => stub.sent('GET /firmware').length;
        const polls = () => stub.sent('GET /status').length;
        // The clock moved a second at a time until the next poll has gone: a
        // poll is scheduled only once the one before it has been answered.
        const nextPoll = async () => {
            const before = polls();
            await expect.poll(async () => {
                await page.clock.runFor(1000);
                return polls();
            }).toBeGreaterThan(before);
        };
        const before = reads();
        for (let i = 0; i < 4; i += 1) {
            await nextPoll();
        }
        // A read still out when the next poll comes is not joined, so at
        // least most of the polls read it.
        await expect.poll(reads).toBeGreaterThanOrEqual(before + 3);
        // A minute on, the same image still running: back to reading it only
        // when there is a reason to.
        await page.clock.runFor(61000);
        await nextPoll();
        await nextPoll();
        const settled = reads();
        await nextPoll();
        await nextPoll();
        expect(reads()).toBe(settled);
        expect(stub.sent('GET /')).toHaveLength(1);
    });

    test('a board back before a poll fails: the page still finds the image it went back to', async ({ page, stub }) => {
        // As a board did: back within seconds, and the one poll that met the
        // restart sent again by the browser and answered.
        stub.device.restartDrops = 0;
        stub.device.firmware.other = firmwareSlot({ label: 'ota_1', version: 'v0.9.0' });
        await page.goto(stub.url);
        await page.getByRole('button', { name: 'Roll back to v0.9.0' }).click();
        await page.getByRole('dialog').getByRole('button', { name: 'Roll back', exact: true }).click();
        await expect.poll(() => stub.sent('GET /').length, { timeout: 10000 }).toBe(2);
        await expect(page.locator('#fw-running')).toHaveText('v0.9.0 in ota_1, on trial');
    });

    test('a rollback the board refuses shows its reply', async ({ page, stub }) => {
        stub.device.firmware.other = firmwareSlot({ label: 'ota_1', version: 'v0.9.0' });
        stub.next('PUT /firmware/rollback', { status: 409, body: 'there is no image to go back to in ota_1: it failed its trial or does not check out\n' });
        await page.goto(stub.url);
        await page.getByRole('button', { name: 'Roll back to v0.9.0' }).click();
        await page.getByRole('dialog').getByRole('button', { name: 'Roll back', exact: true }).click();
        await expect(page.getByRole('status')).toHaveText(
            'Roll back refused (409): there is no image to go back to in ota_1: it failed its trial or does not check out',
        );
    });
});

test('the stand-in refuses what ac3forge::Firmware refuses', async ({ stub }) => {
    const ask = async (method, route, body, headers) => {
        const r = await fetch(stub.url + route, { method, body, headers });
        return [r.status, (await r.text()).trim()];
    };
    const fw = stub.device.firmware;
    expect(await ask('PUT', 'firmware', '')).toEqual([411, 'PUT /firmware wants the image as the body, with its length']);
    expect(await ask('PUT', 'firmware', 'short')).toEqual([400, 'that body is too short to be an application image']);
    expect(await ask('PUT', 'firmware', appImage(), { 'Content-Type': 'text/plain' })).toEqual([
        415,
        'PUT /firmware wants the image as application/octet-stream, or no Content-Type at all',
    ]);
    // On its head, after flash mode has begun.
    expect(await ask('PUT', 'firmware', appImage({ magic: 0 }))).toEqual([
        400,
        'this is not an ESP-IDF application image (its first byte is not 0xE9); send ac3forge_hearth_sink.bin, not the merged factory image or the ELF',
    ]);
    expect(fw.mode).toBe('flash');
    expect(await ask('PUT', 'firmware/mode', 'sideways')).toEqual([400, 'PUT /firmware/mode wants flash or normal']);
    expect(await ask('PUT', 'firmware/mode', 'flash')).toEqual([200, 'flash mode: nothing plays until the board restarts']);
    expect(await ask('PUT', 'firmware/mode', 'normal')).toEqual([200, 'restarting into the image that runs now']);
    stub.device.down = 0;
    expect(await ask('PUT', 'firmware/mode', 'normal')).toEqual([200, 'not in flash mode']);
    expect(await ask('PUT', 'firmware/rollback', '')).toEqual([409, 'there is no image to go back to in ota_1: it is empty']);
    fw.upload = { received: 0, total: 4096, stage: 'waiting' };
    expect(await ask('POST', 'restart', '')).toEqual([409, 'an update is under way']);
    expect(await ask('PUT', 'firmware/rollback', '')).toEqual([409, 'an update is already under way']);
    expect(await ask('PUT', 'firmware/mode', 'flash')).toEqual([409, 'an update is already under way']);
    fw.upload = null;
    fw.trial = firmwareTrial();
    expect(await ask('POST', 'restart', '')).toEqual([
        409,
        'the running image is still on trial, and a restart now goes back to the image before it; to do that, PUT /firmware/rollback',
    ]);
    expect(await ask('PUT', 'firmware', appImage())).toEqual([409, 'the running image is still on trial; wait until it is accepted (GET /firmware)']);
    expect(await ask('PUT', 'firmware/mode', 'flash')).toEqual([409, 'the running image is still on trial; wait until it is accepted (GET /firmware)']);
    fw.trial = null;
    fw.slot_bytes = 1024;
    expect(await ask('PUT', 'firmware', appImage({ size: 2048 }))).toEqual([413, 'that image is 2048 bytes, and the slot holds 1024']);
    fw.other = null;
    expect(await ask('PUT', 'firmware', appImage())).toEqual([
        409,
        "this board's partition table has one app slot: move it to the two-slot table over USB once",
    ]);
    expect(await ask('PUT', 'firmware/rollback', '')).toEqual([409, 'this board has one app slot, so there is nothing to go back to']);
    stub.device.firmware = undefined;
    for (const [method, route] of [['GET', 'firmware'], ['PUT', 'firmware'], ['PUT', 'firmware/mode'], ['PUT', 'firmware/rollback'], ['POST', 'restart'], ['GET', 'firmware/coredump'], ['DELETE', 'firmware/coredump']]) {
        expect(await ask(method, route, method === 'GET' ? undefined : 'x')).toEqual([404, 'this board takes no firmware updates']);
    }
    // An empty slot as the model writes one.
    expect(emptySlot('ota_1').state).toBe('empty');
});
