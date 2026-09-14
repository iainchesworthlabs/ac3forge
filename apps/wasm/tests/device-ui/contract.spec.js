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
const { REPLIES, ROUTES, POLICY, UI_DIR, startStub } = require('./stub');

const CONTROL = fs.readFileSync(
    path.resolve(__dirname, '../../../../esp-idf/ac3forge/src/control.cpp'),
    'utf8',
);
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

test("the stand-in sends the page with the firmware's headers", () => {
    expect(STRINGS[STRINGS.indexOf('Content-Security-Policy') + 1]).toBe(POLICY);
    expect(STRINGS[STRINGS.indexOf('Cache-Control') + 1]).toBe('no-cache');
    expect(STRINGS).toContain('text/html; charset=utf-8');
    expect(STRINGS).toContain('text/javascript; charset=utf-8');
});

test("the stand-in has the firmware's routes and no others", () => {
    const registered = [...CONTROL.matchAll(/\.uri = "([^"]+)", \.method = HTTP_(GET|POST|PUT)/g)].map(
        (m) => `${m[2]} ${m[1]}`,
    );
    expect(registered.sort()).toEqual([...ROUTES].sort());
});

test('every request the page makes is to a route the firmware registers', () => {
    const made = [...SCRIPT.matchAll(/(?:call|act)\((?:[^,()]+, )?'(GET|POST|PUT)', '([a-z]+)'/g)].map(
        (m) => `${m[1]} /${m[2]}`,
    );
    expect(made.sort()).toEqual(['GET /status', 'POST /play', 'POST /stop', 'POST /volume', 'PUT /layout']);
    for (const route of made) {
        expect(ROUTES).toContain(route);
    }
    // One way out, and no address but the device's own.
    expect(SCRIPT.match(/fetch\(/g)).toHaveLength(1);
    expect(SCRIPT).not.toMatch(/https?:\/\//);
});

test('the page asks for nothing the device does not serve', () => {
    const links = [...PAGE.matchAll(/(?:src|href)="([^"]*)"/g)].map((m) => m[1]);
    expect(links.sort()).toEqual(['api', 'api', 'data:,', 'status', 'ui.js']);
    expect(PAGE).not.toMatch(/url\(|@import|https?:\/\//);
});

test("the stand-in writes GET /status's keys in the firmware's order", async () => {
    // Every key on_status writes, in the order it writes them: the stream's own
    // come straight after "stream".
    const onStatus = CONTROL.slice(CONTROL.indexOf('static esp_err_t on_status'), CONTROL.indexOf('static esp_err_t on_play'));
    const firmware = [...onStatus.matchAll(/append_(?:key|number|bool)\(out, "([a-z_]+)"/g)].map((m) => m[1]);
    const stub = await startStub();
    try {
        await fetch(`${stub.url}play`, { method: 'POST', body: 'http://10.0.2.2:8000/demo.ec3' });
        const body = JSON.parse(await (await fetch(`${stub.url}status`)).text());
        const keys = Object.keys(body).flatMap((key) => (key === 'stream' ? [key, ...Object.keys(body.stream)] : [key]));
        expect(keys).toEqual(firmware);
    } finally {
        await stub.close();
    }
});
