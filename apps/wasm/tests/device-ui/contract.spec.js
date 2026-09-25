// @ts-check
'use strict';

// The stand-in device (stub.js) against the firmware it stands in for, and the
// page against the routes the firmware registers. If control.cpp's replies,
// headers or routes change, these fail until the stand-in follows; a request
// the page makes to a route the firmware does not have fails here, before it
// reaches a board.

const fs = require('fs');
const path = require('path');
const { test, expect } = require('@playwright/test');
const {
    REPLIES,
    FIRMWARE,
    FIRMWARE_PARTS,
    ROUTES,
    POLICY,
    UI_DIR,
    startStub,
    idleSendspin,
    playingSendspin,
    pairedServer,
    firmwareTrial,
} = require('./stub');

const COMPONENT = path.resolve(__dirname, '../../../../esp-idf/ac3forge');
const CONTROL = fs.readFileSync(path.join(COMPONENT, 'src/control.cpp'), 'utf8');
// ac3forge::Firmware answers the firmware routes that Control carries, and
// firmware_image.hpp words a refused image; firmware_status.hpp writes
// GET /firmware.
const FIRMWARE_CPP = fs.readFileSync(path.join(COMPONENT, 'src/firmware.cpp'), 'utf8');
const FIRMWARE_IMAGE = fs.readFileSync(path.join(COMPONENT, 'include/ac3forge/firmware_image.hpp'), 'utf8');
const FIRMWARE_STATUS = fs.readFileSync(path.join(COMPONENT, 'include/ac3forge/firmware_status.hpp'), 'utf8');
const SCRIPT = fs.readFileSync(path.join(UI_DIR, 'ac3forge_ui.js'), 'utf8');
const PAGE = fs.readFileSync(path.join(UI_DIR, 'ac3forge_ui.html'), 'utf8');

// Every string control.cpp spells out, read the way the compiler reads them:
// past comments and character literals, adjacent literals joined, and the
// escapes the file uses resolved.
function literals(source) {
    const found = [];
    let end = -1;
    for (let i = 0; i < source.length; ) {
        if (source.startsWith('//', i)) {
            i = source.indexOf('\n', i) + 1 || source.length;
        } else if (source.startsWith('/*', i)) {
            i = source.indexOf('*/', i) + 2;
        } else if (source[i] === "'") {
            i += source[i + 1] === '\\' ? 4 : 3;
        } else if (source[i] === '"') {
            let j = i + 1;
            let text = '';
            for (; source[j] !== '"'; j += 1) {
                if (source[j] === '\\') {
                    j += 1;
                    text += source[j] === 'n' ? '\n' : source[j];
                } else {
                    text += source[j];
                }
            }
            if (end >= 0 && /^\s*$/.test(source.slice(end, i))) {
                found[found.length - 1] += text;
            } else {
                found.push(text);
            }
            i = end = j + 1;
        } else {
            i += 1;
        }
    }
    return found;
}

const STRINGS = literals(CONTROL);

test("the stand-in answers with the firmware's reply texts", () => {
    for (const [name, text] of Object.entries(REPLIES)) {
        expect(STRINGS, `REPLIES.${name} is not a string in control.cpp`).toContain(text);
    }
});

test("the stand-in answers the firmware routes with ac3forge::Firmware's texts", () => {
    // reply_text adds the newline, so the literals have none.
    const strings = [...literals(FIRMWARE_CPP), ...literals(FIRMWARE_IMAGE)];
    for (const [name, text] of Object.entries(FIRMWARE)) {
        expect(strings, `FIRMWARE.${name} is not a string in firmware.cpp or firmware_image.hpp`).toContain(
            text.replace(/\n$/, ''),
        );
    }
    for (const [name, parts] of Object.entries(FIRMWARE_PARTS)) {
        for (const part of parts) {
            expect(strings, `FIRMWARE_PARTS.${name} has "${part}", which is not a string in firmware.cpp`).toContain(part);
        }
    }
});

test("the stand-in sends the page with the firmware's headers", () => {
    expect(STRINGS[STRINGS.indexOf('Content-Security-Policy') + 1]).toBe(POLICY);
    expect(STRINGS[STRINGS.indexOf('Cache-Control') + 1]).toBe('no-cache');
    expect(STRINGS).toContain('text/html; charset=utf-8');
    expect(STRINGS).toContain('text/javascript; charset=utf-8');
});

test("the stand-in has the firmware's routes and no others", () => {
    // The designated initialisers sit on one line or several, depending on
    // how long the route's name is, so the gap between them is any whitespace
    // rather than one space - a route whose name pushed it onto two lines was
    // silently not checked here.
    const registered = [...CONTROL.matchAll(/\.uri = "([^"]+)",\s*\.method = HTTP_(GET|POST|PUT)/g)].map(
        (m) => `${m[2]} ${m[1]}`,
    );
    const byName = (a, b) => a.localeCompare(b);
    expect(registered.sort(byName)).toEqual([...ROUTES].sort(byName));
});

test('every request the page makes is to a route the firmware registers', () => {
    // A hyphen is part of a route (/slot-width), and so is a slash
    // (/firmware/rollback), so the name is [a-z/-]+ and not [a-z]+ - which
    // matched a route up to the hyphen and then nothing, leaving a real
    // request out of this list rather than failing it. The requests are
    // call()'s and act()'s, the firmware section's restart() around act(),
    // and the upload's XMLHttpRequest open().
    const made = [...SCRIPT.matchAll(/(?:call|act|restart|open)\((?:[^,()]+, )?'(GET|POST|PUT)', '([a-z/-]+)'/g)].map(
        (m) => `${m[1]} /${m[2]}`,
    );
    // No POST /play, /stop or /volume: a server owns playback from B2 on, and
    // the page is what the board itself is. One call each: the pairing
    // actions share one, and the layout's presets and its field another. No
    // PUT /firmware/mode: an upload enters flash mode by itself, and a
    // restart leaves it.
    expect(made.sort((a, b) => a.localeCompare(b))).toEqual([
        'GET /firmware',
        'GET /hardware',
        'GET /pairing',
        'GET /status',
        'POST /pairing',
        'POST /restart',
        'PUT /firmware',
        'PUT /firmware/rollback',
        'PUT /layout',
        'PUT /name',
        'PUT /network',
        'PUT /slot-width',
        'PUT /wiring',
    ]);
    for (const route of made) {
        expect(ROUTES).toContain(route);
    }
    // Two ways out - fetch, and for the upload's progress an XMLHttpRequest -
    // and no address but the device's own.
    expect(SCRIPT.match(/fetch\(/g)).toHaveLength(1);
    expect(SCRIPT.match(/new XMLHttpRequest\(/g)).toHaveLength(1);
    expect(SCRIPT).not.toMatch(/https?:\/\//);
});

test('the page asks for nothing the device does not serve', () => {
    const links = [...PAGE.matchAll(/(?:src|href)="([^"]*)"/g)].map((m) => m[1]);
    // The icon is inline, a single pixel of the signal red, so that a browser
    // does not ask for /favicon.ico, which the firmware does not serve.
    const inline = links.filter((link) => link.startsWith('data:'));
    expect(inline).toEqual([expect.stringMatching(/^data:image\/png;base64,[A-Za-z0-9+/=]+$/)]);
    const routes = links.filter((link) => !inline.includes(link));
    expect(routes.sort((a, b) => a.localeCompare(b))).toEqual(['api', 'api', 'status', 'ui.js']);
    expect(PAGE).not.toMatch(/url\(|@import|https?:\/\//);
});

test("the stand-in writes GET /status's keys in the firmware's order", async () => {
    // Every key on_status writes, in the order it writes them: the stream's own
    // come straight after "stream", and the Sendspin player's, which
    // append_sendspin writes, straight after "sendspin".
    const written = (from, to) =>
        [
            ...CONTROL.slice(CONTROL.indexOf(from), CONTROL.indexOf(to)).matchAll(
                /append_(?:key|number|bool|signed|string|optional|levels)\(out, "([a-z_]+)"/g,
            ),
        ].map((m) => m[1]);
    const player = written('void append_sendspin', 'esp_err_t send_text');
    const firmware = written('static esp_err_t on_status', 'static esp_err_t on_play').flatMap((key) =>
        key === 'sendspin' ? [key, ...player] : [key],
    );
    expect(player.length).toBeGreaterThan(30);
    const stub = await startStub();
    try {
        await fetch(`${stub.url}play`, { method: 'POST', body: 'http://10.0.2.2:8000/demo.ec3' });
        for (const sendspin of [idleSendspin(), playingSendspin()]) {
            stub.device.sendspin = sendspin;
            const body = JSON.parse(await (await fetch(`${stub.url}status`)).text());
            const keys = Object.keys(body).flatMap((key) =>
                ['stream', 'sendspin', 'network'].includes(key) ? [key, ...Object.keys(body[key])] : [key],
            );
            expect(keys).toEqual(firmware);
        }
    } finally {
        await stub.close();
    }
});

test("the stand-in writes GET /pairing's keys in the firmware's order", async () => {
    // on_pairing_get writes the object, then each server's keys inside
    // "servers"; on_pairing, the POST, follows it.
    const firmware = [
        ...CONTROL.slice(
            CONTROL.indexOf('static esp_err_t on_pairing_get'),
            CONTROL.indexOf('static esp_err_t on_pairing('),
        ).matchAll(/append_(?:key|number|bool|string)\(out, "([a-z_]+)"/g),
    ].map((m) => m[1]);
    expect(firmware).toEqual(['capacity', 'servers', 'server_id', 'name', 'connected', 'last_playback', 'seen']);
    const stub = await startStub();
    try {
        stub.device.pairings = [pairedServer({ server_id: 'Yx3kP0aZtYk4Q9mLr2vNw8sJc1bXe5hGd7fAu3oKp6i', name: 'Hearth on the desk' })];
        const body = JSON.parse(await (await fetch(`${stub.url}pairing`)).text());
        const keys = Object.keys(body).flatMap((key) =>
            key === 'servers' ? [key, ...Object.keys(body.servers[0])] : [key],
        );
        expect(keys).toEqual(firmware);
    } finally {
        await stub.close();
    }
});

test("the stand-in writes GET /firmware's keys in the firmware's order", async () => {
    // render_firmware_status writes each part with a JsonObject of its own -
    // `object` for the body, then trial, upload, last and part - and
    // append_slot writes a slot. A part that does not apply is written with
    // null() under the same key, so a key comes twice in a row there.
    const keys = (from, to, name) => {
        const text = FIRMWARE_STATUS.slice(FIRMWARE_STATUS.indexOf(from), FIRMWARE_STATUS.indexOf(to));
        return [...text.matchAll(new RegExp(`\\b${name}\\.(?:text|number|null|key)\\("([a-z_0-9]+)"`, 'g'))]
            .map((m) => m[1])
            .filter((key, i, all) => key !== all[i - 1]);
    };
    const render = (name) => keys('inline std::string render_firmware_status', 'bootloader_version);', name);
    const slot = keys('inline void append_slot', '}  // namespace detail', 'object');
    const firmware = {
        body: [...render('object'), 'bootloader_version'].filter((key, i, all) => all.indexOf(key) === i),
        slot,
        trial: render('trial'),
        upload: render('upload'),
        last: render('last'),
        part: render('part'),
    };
    expect(firmware.body).toEqual(expect.arrayContaining(['mode', 'running', 'other', 'partitions']));
    expect(firmware.slot.length).toBe(8);
    const stub = await startStub();
    try {
        // Every part present: a trial, an upload and a last update.
        Object.assign(stub.device.firmware, {
            trial: firmwareTrial(),
            upload: { received: 0, total: 4096, stage: 'waiting' },
            last_update: { version: 'v0.11.0', result: 'refused', reason: 'no' },
        });
        const body = JSON.parse(await (await fetch(`${stub.url}firmware`)).text());
        expect(Object.keys(body)).toEqual(firmware.body);
        expect(Object.keys(body.running)).toEqual(firmware.slot);
        expect(Object.keys(body.other)).toEqual(firmware.slot);
        expect(Object.keys(body.trial)).toEqual(firmware.trial);
        expect(Object.keys(body.upload)).toEqual(firmware.upload);
        expect(Object.keys(body.last_update)).toEqual(firmware.last);
        expect(Object.keys(body.partitions[0])).toEqual(firmware.part);
    } finally {
        await stub.close();
    }
});

test("the stand-in writes GET /hardware's keys in the firmware's order", async () => {
    // build_hardware_json, not on_hardware itself: on_hardware only sends
    // what Control::start built once, and control.hpp's own comment says why
    // this is a route of its own rather than part of /status - nothing here
    // depends on a play, unlike every field on_status writes.
    const firmware = [
        ...CONTROL.slice(
            CONTROL.indexOf('std::string build_hardware_json'),
            CONTROL.indexOf('void append_sendspin'),
        ).matchAll(/append_\w+\(out, "([a-z_]+)"/g),
    ].map((m) => m[1]);
    expect(firmware.length).toBeGreaterThan(5);
    const stub = await startStub();
    try {
        const body = JSON.parse(await (await fetch(`${stub.url}hardware`)).text());
        expect(Object.keys(body)).toEqual(firmware);
    } finally {
        await stub.close();
    }
});
