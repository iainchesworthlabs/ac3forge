'use strict';

// A stand-in for ac3forge::Control (esp-idf/ac3forge/src/control.cpp), for the
// web page's tests - planning/esp32-device-ui.md. It serves the page's two
// files with the headers the firmware sends and answers the REST routes with
// the firmware's status codes and reply texts, over a model of the streaming
// example's player (esp-idf/ac3forge/examples/stream_player/main/
// stream_player.cpp): POST /play hands a location to the http source, which
// takes it only if it starts with http://; a play runs for a few /status
// polls and finishes; a location that does not open leaves the state "failed"
// beside the previous play's figures. contract.spec.js holds the replies, the
// headers and the routes to control.cpp's own.
//
// One per test, in the test's own process, on a port of its own: tests reach
// into it directly to script a reply, hold one back, or read what was sent.

const http = require('http');
const fs = require('fs');
const path = require('path');

const UI_DIR = path.resolve(__dirname, '../../../../esp-idf/ac3forge/ui');

const POLICY =
    "default-src 'self'; style-src 'self' 'unsafe-inline'; img-src data:; frame-ancestors 'none'";

// control.cpp's reply texts, character for character.
const REPLIES = {
    api: [
        'ac3forge player',
        'GET  /              a web page that shows and drives the player',
        'GET  /api           this list',
        'GET  /status        what is playing, as JSON',
        'POST /play          body: a URL or path to play',
        'POST /stop',
        'POST /volume        body: 0.0 to 1.0',
        'GET  /layout        the output layout',
        'PUT  /layout        body: a name (5.1.4) or a speaker list; next play',
        '',
    ].join('\n'),
    playEmpty: 'POST /play wants the location as the body\n',
    playRefused: 'this source cannot play that\n',
    accepted: 'accepted\n',
    stopped: 'stopped\n',
    volumeBad: 'POST /volume wants a number 0.0 to 1.0\n',
    volumeRefused: 'this sink has no volume to set\n',
    ok: 'ok\n',
    layoutNone: 'this player has no layout to report\n',
    layoutEmpty: 'PUT /layout wants a name (5.1.4) or a speaker list (L,R,C,LFE,Ls,Rs)\n',
    layoutFixed: "this player's layout is fixed\n",
    layoutRefused:
        'not a layout this player can play: check the name or the list, and that it has no more slots than the sink\n',
    layoutOk: 'ok; takes effect at the next play\n',
};

const ROUTES = [
    'GET /',
    'GET /ui.js',
    'GET /api',
    'GET /status',
    'POST /play',
    'POST /stop',
    'POST /volume',
    'GET /layout',
    'PUT /layout',
];

// The two streams the model plays. The E-AC-3 one is the WASM page's demo,
// as CI's HTTP step plays it; the AC-3 one is the example's own sample.
const STREAMS = {
    eac3: { codec: 'E-AC-3', acmod: 7, channels: 6, substreams: 1, dialnorm: -31, objects: true },
    ac3: { codec: 'AC-3', acmod: 7, channels: 6, substreams: 1, dialnorm: -31, objects: false },
};

// Table E2.5's location names, and the other tokens OutputLayout's list
// takes (esp-idf/ac3forge/include/ac3forge/layout.hpp).
const TOKENS = new Set(
    'l c r ls rs lc rc lrs rrs cs ts lsd rsd lw rw vhl vhr vhc lts rts lfe lfe2 -'.split(' '),
);

// How many slots a layout needs, or 0 if OutputLayout would not parse it.
function slotsOf(text) {
    const name = /^(\d)\.(\d)(?:\.(\d))?$/.exec(text);
    if (name) {
        const [ring, lfe, height] = [Number(name[1]), Number(name[2]), Number(name[3] || 0)];
        const ok = [1, 2, 3, 4, 5, 7, 9].includes(ring) && lfe <= 2 && [0, 2, 4, 6].includes(height);
        return ok ? ring + lfe + height : 0;
    }
    const tokens = text.split(',').map((t) => t.trim().toLowerCase());
    const known = (t) => TOKENS.has(t) || /^-?\d+(\.\d+)?\/-?\d+(\.\d+)?$/.test(t);
    return tokens.length <= 16 && tokens.every(known) ? tokens.length : 0;
}

function idleStats() {
    return {
        frames: 0,
        held: 0,
        us_per_frame: 0,
        worst_frame_us: 0,
        render_us_per_frame: 0,
        sink_us_per_frame: 0,
        realtime_permille: 0,
        resync_bytes: 0,
        fetched_bytes: 0,
        ring_low: null,
        passes: 0,
        layout_mismatches: 0,
        finished: false,
        failed: false,
        why: '',
        error: 0,
    };
}

// GET /status as control.cpp writes it: the same keys in the same order, the
// volume to three places, and a newline at the end.
function statusJson(d) {
    const stream = d.player ? d.player.stream : d.lastStream;
    const stats = d.player ? d.player.stats : d.lastStats;
    const fields = [
        ['state', JSON.stringify(d.state)],
        ['location', JSON.stringify(d.location)],
        ['source', JSON.stringify(d.source)],
        ['sink', JSON.stringify(d.sink)],
        ['layout', JSON.stringify(d.layout)],
        ['volume', d.volume.toFixed(3)],
        ['stream', stream ? JSON.stringify(stream) : 'null'],
    ];
    for (const [key, value] of Object.entries(stats)) {
        fields.push([key, JSON.stringify(value)]);
    }
    return '{' + fields.map(([k, v]) => JSON.stringify(k) + ':' + v).join(',') + '}\n';
}

async function startStub() {
    const device = {
        state: 'stopped',
        location: 'http://10.0.2.2:8000/demo.ec3',
        source: 'http',
        sink: 'capture-i2s',
        sinkSlots: 2,
        layout: '2.0',
        volume: 1,
        player: null, // the play in progress: {stream, stats, total, fails}
        lastStats: idleStats(), // the last play that ended by itself
        lastStream: null,
        framesPerPoll: 50,
    };
    const requests = [];
    const scripted = new Map(); // route -> replies to give before the model's
    const holding = new Map(); // route -> answers waiting for release()
    let payload = null; // a fixed GET /status body, instead of the model's
    let statusInFlight = 0;
    let statusInFlightMost = 0;

    const send = (res, code, body, type = 'text/plain') => {
        res.writeHead(code, { 'Content-Type': type });
        res.end(body);
    };

    const file = (res, name, type) => {
        res.writeHead(200, {
            'Content-Type': type,
            'Cache-Control': 'no-cache',
            'Content-Security-Policy': POLICY,
        });
        res.end(fs.readFileSync(path.join(UI_DIR, name)));
    };

    // A play moves on at each poll, the way a player on its own tasks would
    // between two of them.
    function advance() {
        const p = device.player;
        if (!p || device.state !== 'playing') {
            return;
        }
        const s = p.stats;
        s.frames = Math.min(p.total, s.frames + device.framesPerPoll);
        Object.assign(s, {
            us_per_frame: 4075,
            worst_frame_us: 46299,
            render_us_per_frame: 112,
            sink_us_per_frame: 252,
            realtime_permille: 127,
            fetched_bytes: s.frames * 1792,
            ring_low: 6144,
        });
        p.stream = { ...p.kind, objects_rendered: p.kind.objects && slotsOf(device.layout) > 8, slots: p.slots };
        if (p.fails && s.frames >= p.total / 2) {
            Object.assign(s, { finished: true, failed: true, why: 'decode', error: 5 });
        } else if (s.frames >= p.total) {
            Object.assign(s, { finished: true, passes: 1, why: 'end of stream' });
        } else {
            return;
        }
        device.lastStats = s;
        device.lastStream = p.stream;
        device.player = null;
        device.state = s.failed ? 'failed' : 'finished';
    }

    // What app_main does with a location from the queue: end the play, then
    // hand the location to the source, which takes it or refuses it.
    function play(location) {
        device.player = null;
        if (!location.startsWith('http://') || location.length >= 256) {
            device.state = 'stopped';
            return;
        }
        device.location = location;
        if (location.includes('unreachable')) {
            device.state = 'failed';
            return;
        }
        device.player = {
            kind: location.endsWith('.ac3') ? STREAMS.ac3 : STREAMS.eac3,
            slots: slotsOf(device.layout),
            stream: null,
            stats: idleStats(),
            total: 250,
            fails: location.includes('broken'),
        };
        device.state = 'playing';
    }

    function answer(route, rawBody, res) {
        // read_body: an empty body and one over 2 KB are both "no body", and
        // the whitespace a shell leaves on the end is trimmed.
        const body = rawBody.length > 2048 ? '' : rawBody.replace(/[\r\n ]+$/, '');
        switch (route) {
            case 'GET /':
                return file(res, 'ac3forge_ui.html', 'text/html; charset=utf-8');
            case 'GET /ui.js':
                return file(res, 'ac3forge_ui.js', 'text/javascript; charset=utf-8');
            case 'GET /api':
                return send(res, 200, REPLIES.api);
            case 'GET /status':
                advance();
                return send(res, 200, payload === null ? statusJson(device) : payload, 'application/json');
            case 'POST /play':
                if (!body) {
                    return send(res, 400, REPLIES.playEmpty);
                }
                if (body.length >= 512) {
                    return send(res, 409, REPLIES.playRefused);
                }
                play(body);
                return send(res, 202, REPLIES.accepted);
            case 'POST /stop':
                device.player = null;
                device.state = 'stopped';
                return send(res, 200, REPLIES.stopped);
            case 'POST /volume': {
                const value = Number.parseFloat(body);
                if (!body || Number.isNaN(value) || value < 0 || value > 1) {
                    return send(res, 400, REPLIES.volumeBad);
                }
                device.volume = value;
                return send(res, 200, REPLIES.ok);
            }
            case 'GET /layout':
                return send(res, 200, device.layout + '\n');
            case 'PUT /layout': {
                if (!body) {
                    return send(res, 400, REPLIES.layoutEmpty);
                }
                const slots = slotsOf(body);
                if (!slots || slots > device.sinkSlots) {
                    return send(res, 409, REPLIES.layoutRefused);
                }
                device.layout = body;
                return send(res, 200, REPLIES.layoutOk);
            }
            default: {
                // esp_http_server's own answers, after which it closes the
                // connection.
                const [, url] = route.split(' ');
                const known = ROUTES.some((r) => r.split(' ')[1] === url);
                res.setHeader('Connection', 'close');
                return known
                    ? send(res, 405, 'Specified method is invalid for this resource', 'text/html')
                    : send(res, 404, 'Nothing matches the given URI', 'text/html');
            }
        }
    }

    const server = http.createServer((req, res) => {
        let body = '';
        req.setEncoding('utf8');
        req.on('data', (chunk) => (body += chunk));
        req.on('end', () => {
            const route = req.method + ' ' + (req.url || '/').split('?')[0];
            requests.push({ route, body, at: Date.now(), headers: req.headers });
            if (route === 'GET /status') {
                statusInFlight += 1;
                statusInFlightMost = Math.max(statusInFlightMost, statusInFlight);
                res.on('close', () => (statusInFlight -= 1));
            }
            const next = (scripted.get(route) || []).shift();
            if (next === 'drop') {
                req.socket.destroy();
            } else if (next === 'hang') {
                // Never answered: the page's own timeout has to notice.
            } else if (next) {
                send(res, next.status, next.body, next.type);
            } else if (holding.has(route)) {
                holding.get(route).push(() => answer(route, body, res));
            } else {
                answer(route, body, res);
            }
        });
    });
    await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
    const sockets = new Set();
    server.on('connection', (socket) => {
        sockets.add(socket);
        socket.on('close', () => sockets.delete(socket));
    });

    return {
        url: `http://127.0.0.1:${server.address().port}/`,
        device,
        requests,
        // The requests made to one route, as "METHOD /path".
        sent: (route) => requests.filter((r) => r.route === route),
        // A reply to give the next request to a route instead of the model's:
        // {status, body, type}, 'drop' (close the connection unanswered) or
        // 'hang' (never answer).
        next(route, reply) {
            if (!scripted.has(route)) {
                scripted.set(route, []);
            }
            scripted.get(route).push(reply);
        },
        // Hold the answers to a route until the returned function is called.
        hold(route) {
            holding.set(route, []);
            return () => {
                const waiting = holding.get(route) || [];
                holding.delete(route);
                waiting.forEach((reply) => reply());
            };
        },
        // A fixed GET /status body - a recorded payload, or anything else.
        setStatus(body) {
            payload = body === null ? null : typeof body === 'string' ? body : JSON.stringify(body);
        },
        statusInFlightMost: () => statusInFlightMost,
        close: () =>
            new Promise((resolve) => {
                sockets.forEach((socket) => socket.destroy());
                server.close(() => resolve());
            }),
    };
}

module.exports = { startStub, REPLIES, ROUTES, POLICY, UI_DIR, slotsOf };
