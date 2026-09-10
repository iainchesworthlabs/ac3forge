// @ts-check
'use strict';

// What the page shows for what the device reports. The payloads are GET
// /status bodies recorded from the emulated board (payloads/README.md says
// how), served as the device sent them; the rest are a recorded payload with
// fields changed or taken out, as a firmware with fewer handlers, or an older
// one, would send it - planning/esp32-device-ui.md's "What the page shows".

const fs = require('fs');
const path = require('path');
const { test, expect } = require('./fixtures');

const recorded = (name) => fs.readFileSync(path.join(__dirname, 'payloads', name), 'utf8');
const parsed = (name) => JSON.parse(recorded(name));
const ms = (us) => (us / 1000).toFixed(1) + ' ms';
const TIMING = ['#t-decoder', '#t-render', '#t-sink', '#t-frame', '#t-worst', '#t-load'];

// Load the page with the stand-in answering GET /status with `body`, and wait
// for the first answer to be on screen.
async function show(page, stub, body) {
    stub.setStatus(body);
    await page.goto(stub.url);
    await expect(page.locator('#link')).toHaveText(/^Status read at /);
}

test('a play in progress', async ({ page, stub }) => {
    const s = parsed('playing-eac3.json');
    await show(page, stub, recorded('playing-eac3.json'));
    await expect(page.locator('#state')).toHaveText('Playing');
    await expect(page.locator('#reason')).toBeHidden();
    await expect(page.locator('#location')).toHaveText(s.location);
    await expect(page.locator('#source')).toHaveText('http');
    await expect(page.locator('#sink')).toHaveText('capture-i2s');
    await expect(page.locator('#codec')).toHaveText('E-AC-3, 1 substream, dialnorm -31');
    await expect(page.locator('#channels')).toHaveText('6 (3/2)');
    await expect(page.locator('#objects')).toHaveText('Carried, not placed');
    await expect(page.locator('#layout')).toHaveText('2.0');
    await expect(page.locator('#slots')).toHaveText('2');
    const seconds = Math.floor((s.frames * 32) / 1000);
    await expect(page.locator('#played')).toHaveText(
        `${Math.floor(seconds / 60)}:${String(seconds % 60).padStart(2, '0')} (${s.frames.toLocaleString('en-US')} frames)`,
    );
    await expect(page.locator('#t-decoder')).toHaveText(ms(s.us_per_frame - s.render_us_per_frame - s.sink_us_per_frame));
    await expect(page.locator('#t-render')).toHaveText(ms(s.render_us_per_frame));
    await expect(page.locator('#t-sink')).toHaveText(`${ms(s.sink_us_per_frame)}, with any wait for the DAC`);
    await expect(page.locator('#t-frame')).toHaveText(`${ms(s.us_per_frame)} of every 32 ms`);
    await expect(page.locator('#t-worst')).toHaveText(ms(s.worst_frame_us));
    await expect(page.locator('#t-load')).toHaveText(`${(s.realtime_permille / 10).toFixed(1)}% of real time`);
    await expect(page.locator('#t-ring')).toHaveText(`${s.ring_low.toLocaleString('en-US')} bytes`);
    await expect(page.locator('#timing-note')).toBeHidden();
    await expect(page.locator('#bar')).toBeVisible();
    await expect(page.getByRole('slider', { name: 'Volume' })).toHaveValue('100');
});

test('a play that reached the end of its stream', async ({ page, stub }) => {
    await show(page, stub, recorded('finished-eac3.json'));
    await expect(page.locator('#state')).toHaveText('Finished (end of stream)');
    await expect(page.locator('#played')).toHaveText('0:08 (250 frames)');
    await expect(page.locator('#c-passes')).toHaveText('1');
    await expect(page.locator('#c-fetched')).toHaveText('448,000 bytes');
    await expect(page.locator('#c-resync')).toHaveText('0 bytes');
    await expect(page.locator('#c-held')).toHaveText('0');
    await expect(page.locator('#c-mismatches')).toHaveText('0');
});

test('an AC-3 stream, which carries no objects', async ({ page, stub }) => {
    await show(page, stub, recorded('finished-ac3.json'));
    await expect(page.locator('#codec')).toHaveText(/^AC-3, 1 substream, dialnorm -\d+$/);
    await expect(page.locator('#objects')).toHaveText('None');
});

test('a play the decoder stopped', async ({ page, stub }) => {
    const s = parsed('failed-decode.json');
    await show(page, stub, recorded('failed-decode.json'));
    await expect(page.locator('#state')).toHaveText('Failed');
    await expect(page.locator('#reason')).toHaveText(`Stopped by a decode error (${s.error}).`);
    await expect(page.locator('#reason')).toHaveClass(/error/);
});

test('a location that did not open, beside the previous play', async ({ page, stub }) => {
    await show(page, stub, recorded('failed-open.json'));
    await expect(page.locator('#state')).toHaveText('Failed');
    await expect(page.locator('#reason')).toHaveText("The location may not have opened; the figures are the previous play's.");
});

test('a stopped player, and one that did not take a location', async ({ page, stub }) => {
    await show(page, stub, recorded('stopped.json'));
    await expect(page.locator('#state')).toHaveText('Stopped');
    await expect(page.locator('#reason')).toBeHidden();
    stub.setStatus(recorded('refused.json'));
    await page.reload();
    await expect(page.locator('#state')).toHaveText('Stopped');
    await expect(page.locator('#location')).toHaveText(parsed('refused.json').location);
});

test('objects placed onto a height layout (a recorded payload, changed)', async ({ page, stub }) => {
    // QEMU has no PSRAM for the object reconstruction a height layout needs,
    // so this is finished-eac3.json with the three fields such a play changes.
    const s = parsed('finished-eac3.json');
    Object.assign(s, { layout: '7.1.4' });
    Object.assign(s.stream, { objects_rendered: true, slots: 12 });
    await show(page, stub, s);
    await expect(page.locator('#objects')).toHaveText('Carried, placed onto the layout');
    await expect(page.locator('#layout')).toHaveText('7.1.4');
    await expect(page.locator('#slots')).toHaveText('12');
});

test('a stream not known yet', async ({ page, stub }) => {
    await show(page, stub, { ...parsed('playing-eac3.json'), stream: null });
    await expect(page.locator('#codec')).toHaveText('Not known yet');
    for (const id of ['#channels', '#objects', '#slots']) {
        await expect(page.locator(id)).toBeHidden();
    }
});

test('a firmware that reports only the state and the figures', async ({ page, stub }) => {
    const s = parsed('playing-eac3.json');
    for (const key of ['location', 'source', 'sink', 'layout', 'volume', 'stream']) {
        delete s[key];
    }
    await show(page, stub, s);
    await expect(page.locator('#state')).toHaveText('Playing');
    for (const id of ['#location', '#source', '#sink', '#codec', '#channels', '#objects', '#layout', '#slots']) {
        await expect(page.locator(id)).toBeHidden();
    }
    await expect(page.locator('#played')).toBeVisible();
    await expect(page.locator('#t-frame')).toBeVisible();
    await expect(page.getByLabel('Location to play')).toHaveValue('');
});

test('a firmware from before the render and sink figures', async ({ page, stub }) => {
    const s = parsed('playing-eac3.json');
    delete s.render_us_per_frame;
    delete s.sink_us_per_frame;
    await show(page, stub, s);
    for (const id of ['#t-decoder', '#t-render', '#t-sink']) {
        await expect(page.locator(id)).toBeHidden();
    }
    await expect(page.locator('#t-frame')).toHaveText(`${ms(s.us_per_frame)} of every 32 ms`);
    await expect(page.locator('#bar-decoder')).toHaveAttribute(
        'style',
        `width: ${((s.us_per_frame / 32000) * 100).toFixed(1)}%;`,
    );
});

test('a play with nothing decoded yet', async ({ page, stub }) => {
    await show(page, stub, { ...parsed('playing-eac3.json'), frames: 0, ring_low: null });
    await expect(page.locator('#timing-note')).toHaveText('Nothing decoded yet.');
    await expect(page.locator('#bar')).toBeHidden();
    for (const id of TIMING) {
        await expect(page.locator(id)).toBeHidden();
    }
    await expect(page.locator('#t-ring')).toHaveText('Not measured yet');
    await expect(page.locator('#played')).toHaveText('0:00 (0 frames)');
});

test('a ring that ran dry, and a frame longer than the frame', async ({ page, stub }) => {
    const s = { ...parsed('playing-eac3.json'), ring_low: 0, us_per_frame: 40000, render_us_per_frame: 1000, sink_us_per_frame: 30000 };
    await show(page, stub, s);
    await expect(page.locator('#t-ring')).toHaveText('0 bytes: the decoder waited for the source');
    // 9 + 1 + 30 ms of a 32 ms frame: the bar is full, and no wider.
    await expect(page.locator('#bar-decoder')).toHaveAttribute('style', 'width: 28.1%;');
    await expect(page.locator('#bar-render')).toHaveAttribute('style', 'width: 3.1%;');
    await expect(page.locator('#bar-sink')).toHaveAttribute('style', 'width: 68.8%;');
});

test("a state the page has no word for is shown as the firmware's own", async ({ page, stub }) => {
    await show(page, stub, { ...parsed('stopped.json'), state: 'paused' });
    await expect(page.locator('#state')).toHaveText('Paused');
    const s = parsed('stopped.json');
    delete s.state;
    stub.setStatus(s);
    await page.reload();
    await expect(page.locator('#state')).toHaveText('Unknown');
});

test('the figures that come in ones, and a coded layout outside the table', async ({ page, stub }) => {
    const s = parsed('playing-eac3.json');
    Object.assign(s, { frames: 1, location: '' });
    Object.assign(s.stream, { substreams: 2, acmod: 9 });
    await show(page, stub, s);
    await expect(page.locator('#played')).toHaveText('0:00 (1 frame)');
    await expect(page.locator('#codec')).toHaveText('E-AC-3, 2 substreams, dialnorm -31');
    await expect(page.locator('#channels')).toHaveText('6');
    await expect(page.locator('#location')).toHaveText('None');
});

test('a failed run that gives no reason or code', async ({ page, stub }) => {
    await show(page, stub, { ...parsed('failed-decode.json'), why: '', error: null });
    await expect(page.locator('#reason')).toHaveText('Stopped by a player error.');
});

test('the volume the device reports moves the slider', async ({ page, stub }) => {
    await show(page, stub, { ...parsed('stopped.json'), volume: 0.35 });
    await expect(page.getByRole('slider', { name: 'Volume' })).toHaveValue('35');
    await expect(page.locator('#volume-out')).toHaveText('35%');
});

test('text from the device goes into the page as text', async ({ page, stub }) => {
    const markup = '<img src=x onerror="document.title=1">';
    await show(page, stub, { ...parsed('failed-decode.json'), location: `http://h/${markup}`, why: markup });
    await expect(page.locator('#location')).toHaveText(`http://h/${markup}`);
    await expect(page.locator('#reason')).toHaveText(`Stopped by a ${markup} error (${parsed('failed-decode.json').error}).`);
    await expect(page.locator('img')).toHaveCount(0);
    await expect(page).toHaveTitle('ac3forge player');
});
