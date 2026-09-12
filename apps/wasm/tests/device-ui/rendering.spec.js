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
// The layouts the field suggests - the ones not disabled for the sink's size.
const enabled = (page) =>
    page.locator('#layouts option').evaluateAll((options) => options.filter((o) => !o.disabled).map((o) => o.value));

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
    // Recorded before the firmware reported how a play serves its layout: the
    // slots, and `layout` as the next play's, since the payload cannot say
    // this play's.
    await expect(page.locator('#output')).toHaveText('2 slots');
    await expect(page.locator('#next')).toHaveText('2.0');
    await expect(page.locator('#silent')).toBeHidden();
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

test('a location that did not open', async ({ page, stub }) => {
    // Recorded before the example cleared the last play's figures when a
    // play begins, so the figures beside it are the previous play's.
    await show(page, stub, recorded('failed-open.json'));
    await expect(page.locator('#state')).toHaveText('Failed');
    await expect(page.locator('#reason')).toHaveText('The location may not have opened.');
});

test("a location that did not open, the last play's figures cleared", async ({ page, stub }) => {
    // The same, as the example reports it since it clears them.
    const s = parsed('failed-open.json');
    Object.assign(s, {
        stream: null, frames: 0, held: 0, us_per_frame: 0, worst_frame_us: 0, render_us_per_frame: 0,
        sink_us_per_frame: 0, realtime_permille: 0, fetched_bytes: 0, ring_low: null, passes: 0,
        finished: false, why: '',
    });
    await show(page, stub, s);
    await expect(page.locator('#state')).toHaveText('Failed');
    await expect(page.locator('#reason')).toHaveText('The location may not have opened.');
    await expect(page.locator('#codec')).toHaveText('Not known yet');
    await expect(page.locator('#timing-note')).toHaveText('Nothing decoded yet.');
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
    // The emulated network shape has no PSRAM for the object reconstruction a
    // height layout needs, so this is finished-714.json with the fields such a
    // play changes: the stream's, and what the player did with it.
    const s = parsed('finished-714.json');
    Object.assign(s.stream, {
        channels: 6, substreams: 1, objects: true, objects_rendered: true, render: 'objects',
        coded: 'L,C,R,Ls,Rs,LFE', silent: 'Lrs,Rrs',
    });
    await show(page, stub, s);
    await expect(page.locator('#objects')).toHaveText('Carried, placed onto the layout');
    await expect(page.locator('#output')).toHaveText('7.1.4, 12 slots: objects placed by their positions');
    await expect(page.locator('#silent')).toHaveText('Lrs, Rrs: no object has reached these yet');
    await expect(page.locator('#next')).toBeHidden();
});

test('a source still opening, before the play has figures of its own', async ({ page, stub }) => {
    // A state of "opening", with the last play's figures cleared and no stream
    // yet, as a firmware reports it while a new source opens.
    const s = parsed('playing-eac3.json');
    Object.assign(s, { state: 'opening', stream: null, frames: 0, us_per_frame: 0, ring_low: null, passes: 0 });
    await show(page, stub, s);
    await expect(page.locator('#state')).toHaveText('Opening');
    await expect(page.locator('#reason')).toBeHidden();
    await expect(page.locator('#codec')).toHaveText('Not known yet');
    await expect(page.locator('#timing-note')).toHaveText('Nothing decoded yet.');
    await expect(page.locator('#bar')).toBeHidden();
});

test('a stream not known yet', async ({ page, stub }) => {
    await show(page, stub, { ...parsed('playing-eac3.json'), stream: null });
    await expect(page.locator('#codec')).toHaveText('Not known yet');
    for (const id of ['#channels', '#objects', '#output', '#silent']) {
        await expect(page.locator(id)).toBeHidden();
    }
    await expect(page.locator('#next')).toHaveText('2.0');
});

test('a firmware that reports only the state and the figures', async ({ page, stub }) => {
    const s = parsed('playing-eac3.json');
    for (const key of ['location', 'source', 'sink', 'layout', 'volume', 'stream']) {
        delete s[key];
    }
    await show(page, stub, s);
    await expect(page.locator('#state')).toHaveText('Playing');
    for (const id of ['#location', '#source', '#sink', '#codec', '#channels', '#objects', '#output', '#silent', '#next']) {
        await expect(page.locator(id)).toBeHidden();
    }
    // No sink size: every suggestion offered, and none claimed for the sink.
    await expect(page.locator('#layout-fit')).toHaveText('');
    expect(await enabled(page)).toEqual(['1.0', '2.0', '5.1', '7.1', '5.1.2', '5.1.4', '7.1.4', '9.1.6']);
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

test('a layout text the device may have cut is not offered for editing', async ({ page, stub }) => {
    // OutputLayout keeps 95 characters of a layout's text, so a text that long
    // may be part of a longer one - a sixteen-slot list of angles is. Applying
    // the part would set a shorter layout than the device has.
    const s = parsed('finished-714.json');
    s.layout = '-110/30,'.repeat(12).slice(0, 95);
    await show(page, stub, s);
    await expect(page.getByLabel('Output layout')).toHaveValue('');
    // One character shorter is whole, and the field offers it for editing.
    s.layout = s.layout.slice(0, 94);
    stub.setStatus(s);
    await page.reload();
    await expect(page.getByLabel('Output layout')).toHaveValue(s.layout);
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
    const s = parsed('failed-decode.json');
    Object.assign(s.stream, { layout: markup, render: 'channels', coded: markup, silent: markup });
    await show(page, stub, { ...s, location: `http://h/${markup}`, why: markup });
    await expect(page.locator('#output')).toHaveText(`${markup}, 2 slots: each channel on the speaker at its location`);
    await expect(page.locator('#location')).toHaveText(`http://h/${markup}`);
    await expect(page.locator('#reason')).toHaveText(`Stopped by a ${markup} error (${parsed('failed-decode.json').error}).`);
    await expect(page.locator('img')).toHaveCount(0);
    await expect(page).toHaveTitle('ac3forge player');
});

// Bodies the firmware sent once it reported how a play serves its layout,
// recorded under QEMU from sdkconfig.ci-http714's twelve-slot shape and
// sdkconfig.ci-http's two-slot one (payloads/README.md).

test('a 7.1.4 stream on a twelve-slot sink at 7.1.4', async ({ page, stub }) => {
    await show(page, stub, recorded('finished-714.json'));
    await expect(page.locator('#sink')).toHaveText('capture-tdm, 12 slots');
    await expect(page.locator('#channels')).toHaveText('12: L C R Ls Rs Lrs Rrs Vhl Vhr Lts Rts LFE');
    await expect(page.locator('#output')).toHaveText('7.1.4, 12 slots: each channel on the speaker at its location');
    await expect(page.locator('#silent')).toBeHidden();
    await expect(page.locator('#next')).toBeHidden();
    await expect(page.getByRole('combobox', { name: 'Output layout' })).toHaveAccessibleDescription(/This sink has 12 slots\.$/);
    expect(await enabled(page)).toEqual(['1.0', '2.0', '5.1', '7.1', '5.1.2', '5.1.4', '7.1.4']);
});

test('the same stream, still playing', async ({ page, stub }) => {
    await show(page, stub, recorded('playing-714.json'));
    await expect(page.locator('#state')).toHaveText('Playing');
    await expect(page.locator('#output')).toHaveText('7.1.4, 12 slots: each channel on the speaker at its location');
});

test('a 5.1 stream on 7.1.4 leaves the rear surrounds and the heights silent', async ({ page, stub }) => {
    await show(page, stub, recorded('finished-51-on-714.json'));
    await expect(page.locator('#channels')).toHaveText('6: L C R Ls Rs LFE');
    await expect(page.locator('#output')).toHaveText('7.1.4, 12 slots: each channel on the speaker at its location');
    await expect(page.locator('#silent')).toHaveText('Lrs, Rrs, Vhl, Vhr, Lts, Rts: nothing in the stream for these');
});

test("the next play's layout beside this play's own", async ({ page, stub }) => {
    await show(page, stub, recorded('next-51.json'));
    await expect(page.locator('#output')).toHaveText(/^7\.1\.4, 12 slots: /);
    await expect(page.locator('#next')).toHaveText('5.1');
});

test('a 7.1.4 stream on 5.1 spreads its rears and heights over the room', async ({ page, stub }) => {
    await show(page, stub, recorded('finished-714-on-51.json'));
    await expect(page.locator('#channels')).toHaveText('12: L C R Ls Rs Lrs Rrs Vhl Vhr Lts Rts LFE');
    await expect(page.locator('#output')).toHaveText('5.1, 6 slots: each channel on the speaker at its location');
    await expect(page.locator('#silent')).toBeHidden();
});

test('a 5.1 stream folded to 2.0 on a twelve-slot sink', async ({ page, stub }) => {
    await show(page, stub, recorded('finished-51-on-20.json'));
    await expect(page.locator('#sink')).toHaveText('capture-tdm, 12 slots');
    await expect(page.locator('#output')).toHaveText('2.0, 2 slots: folded to two channels by the decoder (Lo/Ro)');
    await expect(page.locator('#silent')).toBeHidden();
});

test('dual mono: two programmes on the left and the right', async ({ page, stub }) => {
    await show(page, stub, recorded('finished-dualmono-on-714.json'));
    await expect(page.locator('#channels')).toHaveText('2: Ch1 Ch2');
    await expect(page.locator('#silent')).toHaveText('C, Ls, Rs, Lrs, Rrs, Vhl, Vhr, Lts, Rts, LFE: nothing in the stream for these');
});

test('objects played as their bed', async ({ page, stub }) => {
    await show(page, stub, recorded('finished-objects-as-bed.json'));
    await expect(page.locator('#objects')).toHaveText('Carried, not placed');
    await expect(page.locator('#output')).toHaveText('7.1.4, 12 slots: each channel on the speaker at its location');
});

test('a stream at a sample rate the sink does not run at', async ({ page, stub }) => {
    await show(page, stub, recorded('failed-sample-rate.json'));
    await expect(page.locator('#state')).toHaveText('Failed');
    await expect(page.locator('#reason')).toHaveText("Stopped: the stream's sample rate, 44,100 Hz, is not the sink's.");
});

test('the WASM demo folded onto a two-slot sink', async ({ page, stub }) => {
    await show(page, stub, recorded('finished-20.json'));
    await expect(page.locator('#sink')).toHaveText('capture-i2s, 2 slots');
    await expect(page.locator('#channels')).toHaveText('6: L C R Ls Rs LFE');
    await expect(page.locator('#output')).toHaveText('2.0, 2 slots: folded to two channels by the decoder (Lo/Ro)');
    expect(await enabled(page)).toEqual(['1.0', '2.0']);
});

test('a render the page has no words for, and a fold to one channel', async ({ page, stub }) => {
    const s = parsed('finished-714.json');
    s.stream.render = 'binaural';
    await show(page, stub, s);
    await expect(page.locator('#output')).toHaveText('7.1.4, 12 slots');
    Object.assign(s.stream, { layout: '1.0', slots: 1, render: 'mono' });
    stub.setStatus(s);
    await page.reload();
    await expect(page.locator('#output')).toHaveText('1.0, 1 slot: folded to one channel by the decoder');
});
