'use strict';

// Atmos object-authoring demo: drives encoder_bindings.cpp's AtmosBedEncoder
// (ac3::oba::AtmosEncoder - a silent 5.1 bed carrying JOC-coded dynamic
// objects) with one ObjectPlacement set per 1536-sample frame, read live from
// the room canvas. See ../app.js (the encode demo) for the shared
// module-loading and round-trip-preview conventions this file follows.

const el = (id) => document.getElementById(id);

const SAMPLE_RATE = 48000; // AtmosEncoder's JOC QMF tables are 48 kHz
const FRAME_SAMPLES = 1536;
const FRAME_SECONDS = FRAME_SAMPLES / SAMPLE_RATE;

// One synthesised tone per object - distinct pitches so the pan of each is
// audible on its own, amplitudes well clear of clipping even summed.
const OBJECT_DEFS = [
    { color: '#60a5fa', freq: 220, label: 'O1' },
    { color: '#34d399', freq: 330, label: 'O2' },
    { color: '#f59e0b', freq: 440, label: 'O3' },
    { color: '#f87171', freq: 587, label: 'O4' },
];

let encodeModule = null;
let decodeModule = null;
let session = null;
let lastStreamBytes = null;

// TS 103 420 §4.2.1 room-anchored positions: x 0..1 left->right, y 0..1
// front->back, z 0..1 floor->ceiling.
const objects = OBJECT_DEFS.map((def, i) => ({
    x: 0.2 + 0.2 * i,
    y: i % 2 === 0 ? 0.25 : 0.75,
    z: 0,
    gain: 0.5,
    phase: 0,
    orbitAngle: Math.PI * 1.5,
    def,
}));

function setStatus(text, isError) {
    const status = el('status');
    status.textContent = text;
    status.classList.toggle('error', Boolean(isError));
}

function activeCount() {
    return Number(el('objectCount').value);
}

// --- Room canvas -----------------------------------------------------------

function drawRoom() {
    const canvas = el('room');
    const ctx = canvas.getContext('2d');
    const w = canvas.width;
    const h = canvas.height;
    ctx.clearRect(0, 0, w, h);

    ctx.strokeStyle = '#263248';
    ctx.lineWidth = 1;
    // Listener position and a front-of-room marker for orientation.
    ctx.beginPath();
    ctx.moveTo(w / 2 - 8, h / 2);
    ctx.lineTo(w / 2 + 8, h / 2);
    ctx.moveTo(w / 2, h / 2 - 8);
    ctx.lineTo(w / 2, h / 2 + 8);
    ctx.stroke();
    ctx.fillStyle = '#94a3b8';
    ctx.font = '12px system-ui';
    ctx.fillText('front', w / 2 - 14, 16);

    const count = activeCount();
    for (let i = 0; i < count; i++) {
        const object = objects[i];
        const px = object.x * w;
        const py = object.y * h;
        // Height shows as a halo: the higher the object, the wider the ring.
        if (object.z > 0.02) {
            ctx.beginPath();
            ctx.strokeStyle = object.def.color;
            ctx.globalAlpha = 0.5;
            ctx.arc(px, py, 9 + object.z * 10, 0, Math.PI * 2);
            ctx.stroke();
            ctx.globalAlpha = 1;
        }
        ctx.beginPath();
        ctx.fillStyle = object.def.color;
        ctx.arc(px, py, 8, 0, Math.PI * 2);
        ctx.fill();
        ctx.fillStyle = '#0b1220';
        ctx.font = 'bold 9px system-ui';
        ctx.fillText(object.def.label, px - 6, py + 3);
    }
}

function canvasPosition(event) {
    const canvas = el('room');
    const rect = canvas.getBoundingClientRect();
    return {
        x: Math.min(1, Math.max(0, (event.clientX - rect.left) / rect.width)),
        y: Math.min(1, Math.max(0, (event.clientY - rect.top) / rect.height)),
    };
}

function wireRoom() {
    const canvas = el('room');
    let dragging = -1;

    canvas.addEventListener('pointerdown', (event) => {
        const pos = canvasPosition(event);
        const count = activeCount();
        let best = -1;
        let bestDistance = 0.06; // grab radius in room units
        for (let i = 0; i < count; i++) {
            const d = Math.hypot(objects[i].x - pos.x, objects[i].y - pos.y);
            if (d < bestDistance) {
                best = i;
                bestDistance = d;
            }
        }
        if (best < 0) return;
        dragging = best;
        canvas.setPointerCapture(event.pointerId);
        // A manual drag takes the object back from the orbit animation.
        if (best === 0) el('orbit').checked = false;
    });
    canvas.addEventListener('pointermove', (event) => {
        if (dragging < 0) return;
        const pos = canvasPosition(event);
        objects[dragging].x = pos.x;
        objects[dragging].y = pos.y;
        drawRoom();
    });
    const release = () => { dragging = -1; };
    canvas.addEventListener('pointerup', release);
    canvas.addEventListener('pointercancel', release);
}

function renderObjectRows() {
    const rows = el('objectRows');
    rows.innerHTML = '';
    const count = activeCount();
    for (let i = 0; i < count; i++) {
        const object = objects[i];
        const row = document.createElement('div');
        row.className = 'objectRow';
        row.innerHTML = `
            <span class="swatch" style="background:${object.def.color}"></span>
            <span>${object.def.label} (${object.def.freq} Hz)</span>
            <label class="inline">height
              <input type="range" min="0" max="1" step="0.01" value="${object.z}" data-object="${i}" data-prop="z">
            </label>
            <label class="inline">gain
              <input type="range" min="0" max="1" step="0.01" value="${object.gain}" data-object="${i}" data-prop="gain">
            </label>`;
        rows.appendChild(row);
    }
    rows.querySelectorAll('input[type="range"]').forEach((slider) => {
        slider.addEventListener('input', () => {
            const object = objects[Number(slider.dataset.object)];
            object[slider.dataset.prop] = Number(slider.value);
            drawRoom();
        });
    });
    drawRoom();
}

// --- Encoding session ------------------------------------------------------

function encodeTick(state) {
    if (session !== state) return;

    // The orbit animation: object 1 circles the listener, one lap every 8
    // seconds of encoded audio - a deterministic pan for anyone (or any
    // test) who wants motion in the stream without drawing it by hand.
    if (el('orbit').checked) {
        const object = objects[0];
        object.orbitAngle += (2 * Math.PI * FRAME_SECONDS) / 8;
        object.x = 0.5 + 0.38 * Math.cos(object.orbitAngle);
        object.y = 0.5 + 0.38 * Math.sin(object.orbitAngle);
        drawRoom();
    }

    const signals = [];
    const placements = [];
    for (let i = 0; i < state.count; i++) {
        const object = objects[i];
        // Phase-continuous tone across frames - the objects sound like
        // steady sources being moved, not like a sequence of clicks.
        const signal = new Float32Array(FRAME_SAMPLES);
        const step = (2 * Math.PI * object.def.freq) / SAMPLE_RATE;
        let phase = object.phase;
        for (let n = 0; n < FRAME_SAMPLES; n++) {
            signal[n] = 0.35 * Math.sin(phase);
            phase += step;
        }
        object.phase = phase % (2 * Math.PI);
        signals.push(signal);
        placements.push({ x: object.x, y: object.y, z: object.z, gain: object.gain });
    }

    const bytes = state.encoder.encodeFrame(signals, placements);
    if (!bytes) {
        stopSession();
        setStatus(`encode failed: ${state.encoder.error()}`, true);
        return;
    }
    const copy = Uint8Array.from(bytes);
    state.chunks.push(copy);
    state.totalBytes += copy.length;
    state.frames += 1;
    setStatus(
        `Encoding ${state.count} object(s) at ${state.bitrate} kbps: ` +
            `${(state.frames * FRAME_SECONDS).toFixed(1)}s, ${state.totalBytes.toLocaleString()} bytes.`,
        false);
}

function startSession() {
    el('resultPanel').style.display = 'none';
    lastStreamBytes = null;

    const count = activeCount();
    const bitrate = Number(el('bitrate').value);
    const state = {
        encoder: new encodeModule.AtmosBedEncoder(SAMPLE_RATE, bitrate, count),
        count,
        bitrate,
        chunks: [],
        totalBytes: 0,
        frames: 0,
        timer: null,
    };
    session = state;
    // One frame per real frame duration - the room is being *performed*, so
    // the encode runs at the speed the pan happens rather than as fast as
    // the CPU allows.
    state.timer = setInterval(() => encodeTick(state), FRAME_SECONDS * 1000);
    el('startBtn').textContent = 'Stop and finish';
    el('objectCount').disabled = true;
    el('bitrate').disabled = true;
}

function stopSession() {
    const state = session;
    if (!state) return;
    session = null;
    clearInterval(state.timer);
    el('startBtn').textContent = 'Start encoding';
    el('objectCount').disabled = false;
    el('bitrate').disabled = false;

    if (state.frames === 0) {
        setStatus('Stopped before any frames were encoded.', true);
        return;
    }
    const out = new Uint8Array(state.totalBytes);
    let offset = 0;
    for (const chunk of state.chunks) {
        out.set(chunk, offset);
        offset += chunk.length;
    }
    lastStreamBytes = out;
    setStatus(
        `Authored ${(state.frames * FRAME_SECONDS).toFixed(1)}s of ${state.count}-object Atmos ` +
            `into ${state.totalBytes.toLocaleString()} bytes.`,
        false);
    el('resultPanel').style.display = '';
}

// --- Result ----------------------------------------------------------------

function downloadStream() {
    if (!lastStreamBytes) return;
    const blob = new Blob([lastStreamBytes], { type: 'application/octet-stream' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = 'ac3forge-atmos.ec3';
    document.body.appendChild(a);
    a.click();
    a.remove();
    URL.revokeObjectURL(url);
}

// Same scanStream()+PushDecoder round trip as ../app.js's preview, on the
// authored stream: the decoder reconstructs the objects from the JOC data
// and renders them, so the pan drawn on the canvas is what plays back.
async function previewRoundTrip() {
    if (!lastStreamBytes) return;
    setStatus('Decoding the authored stream for preview...', false);

    const scanned = decodeModule.scanStream(lastStreamBytes);
    if (!scanned.ok) {
        setStatus(`round-trip decode failed: ${scanned.error}`, true);
        return;
    }
    const decoder = new decodeModule.PushDecoder(0, false, false);
    let sampleRate = scanned.sampleRate;
    let channelCount = 0;
    const frames = [];
    for (const unit of scanned.accessUnits) {
        const result = decoder.pushAccessUnit(
            lastStreamBytes.slice(unit.offset, unit.offset + unit.length));
        if (!result.ok) {
            decoder.delete();
            setStatus(`round-trip decode failed: ${result.error}`, true);
            return;
        }
        if (result.holdBack) continue;
        sampleRate = result.sampleRate;
        channelCount = result.channelCount;
        const frame = [];
        for (let ch = 0; ch < channelCount; ch++) {
            frame.push(Float32Array.from(decoder.channelPcm(ch)));
        }
        frames.push(frame);
    }
    decoder.delete();
    if (channelCount === 0 || frames.length === 0) {
        setStatus('round-trip decode failed: no decodable frames', true);
        return;
    }

    const audioCtx = new AudioContext({ sampleRate });
    const frameCount = frames.reduce((n, f) => n + f[0].length, 0);
    const outBuffer = audioCtx.createBuffer(channelCount, frameCount, sampleRate);
    for (let ch = 0; ch < channelCount; ch++) {
        const merged = new Float32Array(frameCount);
        let at = 0;
        for (const frame of frames) {
            merged.set(frame[ch], at);
            at += frame[ch].length;
        }
        outBuffer.copyToChannel(merged, ch);
    }
    const source = audioCtx.createBufferSource();
    source.buffer = outBuffer;
    source.connect(audioCtx.destination);
    source.start();
    setStatus(`Playing round-trip preview (${sampleRate} Hz, ${channelCount} ch).`, false);
}

// --- Wiring ----------------------------------------------------------------

(async function main() {
    wireRoom();
    renderObjectRows();
    el('objectCount').addEventListener('change', renderObjectRows);
    el('startBtn').addEventListener('click', () => {
        if (session) stopSession();
        else startSession();
    });
    el('downloadBtn').addEventListener('click', downloadStream);
    el('previewBtn').addEventListener('click', () => {
        previewRoundTrip().catch((error) => setStatus(String(error.message || error), true));
    });

    try {
        encodeModule = await createAc3ForgeEncodeModule();
        decodeModule = await createAc3ForgeModule();
        setStatus('Ready - press "Start encoding" and drag the objects.', false);
    } catch (error) {
        el('startBtn').disabled = true;
        setStatus(`failed to load WASM modules: ${error.message || error}`, true);
    }
})();
