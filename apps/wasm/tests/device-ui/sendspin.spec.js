// @ts-check
'use strict';

// The Sendspin player's part of the page (planning/hearth-reference-player.md,
// B3): what GET /status's "sendspin" object shows - the server, the link, the
// clock, how the stream plays, a level per output, a pairing in progress -
// and the three pairing actions, POST /pairing's reset, cancel and forget,
// against the stand-in device.

const { test, expect } = require('./fixtures');
const { REPLIES, idleSendspin, playingSendspin } = require('./stub');

const ROWS = ['#ss-server', '#ss-link', '#ss-clock', '#ss-timing', '#ss-underruns', '#ss-lost', '#ss-outcome'];

// Load the page with the stand-in's player reporting `sendspin`, and wait for
// the first answer to be on screen.
async function show(page, stub, sendspin) {
    stub.device.sendspin = sendspin;
    await page.goto(stub.url);
    await expect(page.locator('#link')).toHaveText(/^Status read at /);
}

// The header and data cells of the table's row `n`, counted from 0 below its
// column headers.
const cells = (table, n) => table.locator('tbody tr').nth(n).locator('th, td');

// The next GET /status answered after this call.
async function nextPoll(stub) {
    const before = stub.sent('GET /status').length;
    await expect.poll(() => stub.sent('GET /status').length).toBeGreaterThan(before);
}

test('a board no server is connected to', async ({ page, stub }) => {
    await show(page, stub, idleSendspin());
    await expect(page.getByRole('region', { name: 'Sendspin' })).toBeVisible();
    await expect(page.locator('#ss-note')).toHaveText(
        'No server is connected. Servers on this network find this board by its name.',
    );
    await expect(page.locator('#ss-playing')).toHaveText('Nothing');
    await expect(page.locator('#ss-paired')).toHaveText('0');
    for (const id of ROWS) {
        await expect(page.locator(id)).toBeHidden();
    }
    await expect(page.locator('#ss-code')).toBeHidden();
    await expect(page.getByRole('table')).toBeHidden();
    await expect(page.getByRole('button', { name: 'Forget every server' })).toBeVisible();
    await expect(page.getByRole('button', { name: 'Cancel pairing' })).toBeHidden();
    await expect(page.getByRole('button', { name: 'Allow pairing again' })).toBeHidden();
});

test('a paired server playing bursts, with a level for each output', async ({ page, stub }) => {
    await show(page, stub, playingSendspin());
    await expect(page.locator('#ss-note')).toBeHidden();
    await expect(page.locator('#ss-server')).toHaveText('Hearth on the desk (specification)');
    await expect(page.locator('#ss-link')).toHaveText('Paired, encrypted, _ac3forge_player@v1');
    await expect(page.locator('#ss-clock')).toHaveText('In step with the server, within 0.3 ms');
    await expect(page.locator('#ss-playing')).toHaveText('Bursts, decoded here, 1,875 chunks');
    await expect(page.locator('#ss-timing')).toHaveText('0.0 ms early, 0.2 ms at worst');
    await expect(page.locator('#ss-underruns')).toHaveText('0');
    await expect(page.locator('#ss-lost')).toHaveText('0 late, 0 with no room, 0 not valid');
    await expect(page.locator('#ss-paired')).toHaveText('1');
    await expect(page.locator('#ss-outcome')).toBeHidden();
    const table = page.getByRole('table', { name: 'Levels over the last 100 ms' });
    await expect(table).toBeVisible();
    await expect(table.getByRole('columnheader')).toHaveText(['Output', 'Peak', 'RMS']);
    await expect(table.getByRole('rowheader')).toHaveText(['1', '2', '3', '4', '5', '6']);
    await expect(cells(table, 0)).toHaveText(['1', '-3.1 dB', '-18.2 dB']);
    await expect(cells(table, 2)).toHaveText(['3', '-8.9 dB', '-21.0 dB']);
    // An output with nothing on it, at the meter's floor.
    await expect(cells(table, 5)).toHaveText(['6', 'Silent', 'Silent']);
});

test('PCM from a server of the older dialect, not paired, still settling', async ({ page, stub }) => {
    await show(page, stub, {
        ...playingSendspin(),
        server: 'Music Assistant',
        dialect: 'aiosendspin 9.1.1',
        psk: 'sentinel',
        role: 'player@v1',
        clock_converged: false,
        playing: 'pcm',
        bursts: 40,
        underruns: 3,
        late: 2,
        dropped: 1,
        invalid: 4,
        error_us: 1200,
        worst_error_us: -4500,
        peak_db: [-6, -7.5],
        // A reading the page does not have yet.
        rms_db: [-20],
    });
    await expect(page.locator('#ss-server')).toHaveText('Music Assistant (aiosendspin 9.1.1)');
    await expect(page.locator('#ss-link')).toHaveText('Encrypted, not paired, player@v1');
    await expect(page.locator('#ss-clock')).toHaveText('Settling');
    await expect(page.locator('#ss-playing')).toHaveText('PCM, 40 chunks');
    await expect(page.locator('#ss-timing')).toHaveText('1.2 ms late, 4.5 ms at worst');
    await expect(page.locator('#ss-underruns')).toHaveText('3');
    await expect(page.locator('#ss-lost')).toHaveText('2 late, 1 with no room, 4 not valid');
    await expect(cells(page.getByRole('table'), 1)).toHaveText(['2', '-7.5 dB', '']);
});

test('a link, role and dialect the page has no words for are left out', async ({ page, stub }) => {
    await show(page, stub, { ...playingSendspin(), psk: 'something new', dialect: '', role: '', playing: 'video' });
    await expect(page.locator('#ss-server')).toHaveText('Hearth on the desk');
    await expect(page.locator('#ss-link')).toBeHidden();
    await expect(page.locator('#ss-playing')).toBeHidden();
    await expect(page.locator('#ss-timing')).toHaveText('0.0 ms early, 0.2 ms at worst');
    // A link the page knows, with no role yet.
    stub.device.sendspin = { ...playingSendspin(), psk: 'pairing', role: '' };
    await nextPoll(stub);
    await expect(page.locator('#ss-link')).toHaveText('Pairing, encrypted');
});

test('a pairing code is shown and announced once, until the pairing ends', async ({ page, stub }) => {
    await show(page, stub, { ...idleSendspin(), server: 'Hearth on the desk', psk: 'pairing', pairing_code: '482913' });
    await expect(page.locator('#ss-code')).toHaveText('Pairing code 482 913');
    await expect(page.getByRole('status')).toHaveText('Pairing code 482 913: enter it where the server asks for it.');
    await expect(page.getByRole('button', { name: 'Cancel pairing' })).toBeVisible();
    // Something else said since is not said over at the next poll.
    await page.getByLabel('Name').fill('Attic');
    await page.locator('#name-form').getByRole('button', { name: 'Save' }).click();
    await expect(page.getByRole('status')).toHaveText('This sink is called Attic.');
    await nextPoll(stub);
    await nextPoll(stub);
    await expect(page.getByRole('status')).toHaveText('This sink is called Attic.');
    // The server took the code.
    Object.assign(stub.device.sendspin, { pairing_code: '', pairing_outcome: 'paired', paired: 1, psk: 'long-term' });
    await expect(page.locator('#ss-code')).toBeHidden();
    await expect(page.getByRole('button', { name: 'Cancel pairing' })).toBeHidden();
    await expect(page.locator('#ss-outcome')).toHaveText('paired');
    await expect(page.locator('#ss-paired')).toHaveText('1');
    // A second pairing's code is announced in its turn.
    stub.device.sendspin.pairing_code = '1234567';
    await expect(page.getByRole('status')).toHaveText('Pairing code 123 456 7: enter it where the server asks for it.');
});

test('pairing held back after codes that did not match', async ({ page, stub }) => {
    await show(page, stub, { ...idleSendspin(), pairing_held: true, pairing_rounds: 5, pairing_outcome: 'code mismatch' });
    await expect(page.locator('#ss-note')).toHaveText('Pairing is held back after codes that did not match.');
    await expect(page.locator('#ss-outcome')).toHaveText('code mismatch');
    await expect(page.getByRole('button', { name: 'Allow pairing again' })).toBeVisible();
});

test('a firmware with no Sendspin player, or one whose player did not start, has no section', async ({ page, stub }) => {
    await show(page, stub, undefined);
    await expect(page.getByRole('region', { name: 'Sendspin' })).toBeHidden();
    stub.device.sendspin = null;
    await nextPoll(stub);
    await expect(page.locator('#sendspin')).toBeHidden();
    // And one that comes up later appears.
    stub.device.sendspin = idleSendspin();
    await expect(page.getByRole('region', { name: 'Sendspin' })).toBeVisible();
});

test('text from a server goes into the page as text', async ({ page, stub }) => {
    const markup = '<img src=x onerror="document.title=1">';
    await show(page, stub, { ...playingSendspin(), server: markup, dialect: markup, pairing_outcome: markup });
    await expect(page.locator('#ss-server')).toHaveText(`${markup} (${markup})`);
    await expect(page.locator('#ss-outcome')).toHaveText(markup);
    await expect(page.locator('img')).toHaveCount(0);
});

test.describe('the pairing actions', () => {
    test('Cancel pairing sends cancel to POST /pairing', async ({ page, stub }) => {
        await show(page, stub, { ...idleSendspin(), server: 'Hearth on the desk', pairing_code: '482913' });
        await page.getByRole('button', { name: 'Cancel pairing' }).click();
        await expect.poll(() => stub.sent('POST /pairing').map((r) => r.body)).toEqual(['cancel']);
        await expect(page.getByRole('status')).toHaveText('Pairing cancelled.');
        await expect(page.locator('#ss-code')).toBeHidden();
        await expect(page.locator('#ss-outcome')).toHaveText('cancelled');
    });

    test('Allow pairing again sends reset to POST /pairing', async ({ page, stub }) => {
        await show(page, stub, { ...idleSendspin(), pairing_held: true, pairing_rounds: 5 });
        await page.getByRole('button', { name: 'Allow pairing again' }).click();
        await expect.poll(() => stub.sent('POST /pairing').map((r) => r.body)).toEqual(['reset']);
        await expect(page.getByRole('status')).toHaveText('A server may ask to pair again.');
        await expect(page.getByRole('button', { name: 'Allow pairing again' })).toBeHidden();
    });

    test('Forget every server asks first, and sends forget only when told to', async ({ page, stub }) => {
        await show(page, stub, playingSendspin());
        const asked = [];
        page.once('dialog', (dialog) => {
            asked.push(dialog.message());
            return dialog.dismiss();
        });
        await page.getByRole('button', { name: 'Forget every server' }).click();
        await expect.poll(() => asked).toEqual([
            'Forget every server this board has paired with? Each has to pair again.',
        ]);
        await nextPoll(stub);
        expect(stub.sent('POST /pairing')).toHaveLength(0);
        page.once('dialog', (dialog) => dialog.accept());
        await page.getByRole('button', { name: 'Forget every server' }).click();
        await expect.poll(() => stub.sent('POST /pairing').map((r) => r.body)).toEqual(['forget']);
        await expect(page.getByRole('status')).toHaveText('Every server is forgotten: each has to pair again.');
        await expect(page.locator('#ss-paired')).toHaveText('0');
        await expect(page.locator('#ss-server')).toBeHidden();
    });

    test("a pairing action the board refuses shows the board's reply", async ({ page, stub }) => {
        await show(page, stub, playingSendspin());
        stub.next('POST /pairing', { status: 409, body: REPLIES.pairingRefused });
        page.once('dialog', (dialog) => dialog.accept());
        await page.getByRole('button', { name: 'Forget every server' }).click();
        await expect(page.getByRole('status')).toHaveText(
            'Forget every server refused (409): this board is not a Sendspin player',
        );
        await expect(page.getByRole('status')).toHaveClass(/error/);
    });

    test('the stand-in refuses a body the board does not take', async ({ page, stub }) => {
        await show(page, stub, idleSendspin());
        const bad = await page.request.post(stub.url + 'pairing', { data: 'pair' });
        expect(bad.status()).toBe(400);
        expect(await bad.text()).toBe(REPLIES.pairingBad);
        stub.device.sendspin = undefined;
        const none = await page.request.post(stub.url + 'pairing', { data: 'reset' });
        expect(none.status()).toBe(409);
        expect(await none.text()).toBe(REPLIES.pairingRefused);
    });
});
