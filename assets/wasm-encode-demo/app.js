// ac3forge WASM encode demo - glue between the two Embind modules
// (ac3forge_encode.js/.wasm, built from encoder_bindings.cpp; and
// ac3forge_decode.js/.wasm, the existing decode module copied alongside this
// page's own build output for the round-trip preview) and the page
// (index.html).
//
// Everything on this page is real: a dropped .wav is decoded by the
// browser's own Web Audio API, encoded frame-by-frame by the actual AC-3/
// E-AC-3 encoder (ac3::FrameEncoder/ac3::eac3::FrameEncoder, compiled to
// WASM), measured by the actual BS.1770 loudness meter
// (ac3::meta::LoudnessMeter) used for `ac3cli qc`, and the "round-trip
// preview" plays back the actual encoded bytes through the actual decoder -
// not the source audio replayed directly.

const el = (id) => document.getElementById(id);

// AC-3 Table 5.8 full-bandwidth channel order (LFE always last when
// present). WasmLayout values match encoder_bindings.cpp's WasmLayout enum.
const LAYOUTS = {
    1: { wasmLayout: 0, label: 'mono' },
    2: { wasmLayout: 1, label: 'stereo' },
    6: { wasmLayout: 2, label: '5.1' },
    // The wide layouts (E-AC-3 only - a 5.1 bed plus dependent substreams)
    // route through ac3::plan INSIDE the module: encodeFrame() takes these in
    // plain WAV order and the binding owns the channel-order knowledge, so no
    // JS-side reorder table exists for them. Ambiguous counts read the way
    // ac3::plan::generic_wav_layout reads them: 8 as 7.1, 10 as 5.1.4, 12 as
    // 7.1.4.
    8: { wasmLayout: 3, label: '7.1', wide: true },
    10: { wasmLayout: 4, label: '5.1.4', wide: true },
    12: { wasmLayout: 5, label: '7.1.4', wide: true },
};

// A dropped WAV's channel order follows the WAVEFORMATEXTENSIBLE default
// speaker mask, not AC-3's Table 5.8 order - for 5.1 that is
// FL,FR,FC,LFE,BL,BR (Microsoft's own default mask order), which this page
// reorders into AC-3's L,C,R,Ls,Rs,LFE before it ever reaches the encoder.
// Mono/stereo need no reordering (Table 5.8's C-only / L,R order already
// matches the WAV order for those channel counts).
const WAV_5_1_TO_AC3_ORDER = [0, 2, 1, 4, 5, 3]; // ac3Index -> wavIndex

let encodeModule = null;
let decodeModule = null;
let lastStreamBytes = null;
let lastFormat = 0;
let lastSampleRate = 48000;

function setStatus(text, isError) {
    const status = el('status');
    status.textContent = text;
    status.classList.toggle('error', Boolean(isError));
}

async function loadModules() {
    // createAc3ForgeEncodeModule / createAc3ForgeModule are the global
    // factory functions MODULARIZE+EXPORT_NAME produce for each module (see
    // apps/wasm/CMakeLists.txt's link options) - distinct names so both can
    // load on this one page without colliding.
    encodeModule = await createAc3ForgeEncodeModule();
    decodeModule = await createAc3ForgeModule();
}

function reorderedChannels(audioBuffer) {
    const count = audioBuffer.numberOfChannels;
    const layout = LAYOUTS[count];
    if (!layout) {
        throw new Error(`unsupported channel count ${count} - this demo supports mono, stereo, 5.1, 7.1, 5.1.4 or 7.1.4 WAV files`);
    }
    const raw = [];
    for (let i = 0; i < count; i++) raw.push(audioBuffer.getChannelData(i));
    if (count !== 6) return { channels: raw, layout };
    const reordered = WAV_5_1_TO_AC3_ORDER.map((wavIndex) => raw[wavIndex]);
    return { channels: reordered, layout };
}

// §5.4.2.8 dialnorm from a measured integrated loudness: the value a real
// decoder subtracts to bring the programme to -31 LKFS, clamped to the
// field's 1..31 range. Unmeasurable input (everything below BS.1770's
// absolute gate) keeps 31 - the attenuation-free maximum.
function dialnormFor(integratedLkfs) {
    if (integratedLkfs === null || integratedLkfs === undefined) return 31;
    return Math.min(31, Math.max(1, Math.round(-integratedLkfs)));
}

function concatBytes(chunks, totalLength) {
    const out = new Uint8Array(totalLength);
    let offset = 0;
    for (const chunk of chunks) {
        out.set(chunk, offset);
        offset += chunk.length;
    }
    return out;
}

function renderQcTable(verdicts) {
    const tbody = document.querySelector('#qcTable tbody');
    tbody.innerHTML = '';
    for (const row of verdicts) {
        const tr = document.createElement('tr');
        const targetText = row.isCeiling
            ? `≤ ${row.targetLkfs.toFixed(1)} LKFS`
            : `${row.targetLkfs.toFixed(1)} ± ${row.toleranceLu.toFixed(1)} LU`;
        const measuredText = row.loudnessDeltaLu === null ? 'n/a' : `${(row.targetLkfs + row.loudnessDeltaLu).toFixed(1)} LKFS`;
        const loudnessCell = row.loudnessPass ? 'pass' : 'fail';
        const truePeakCell = row.truePeakPass ? 'pass' : 'fail';
        tr.innerHTML = `
            <td>${row.preset}<br><span class="hint">${row.source}</span></td>
            <td>${targetText}</td>
            <td>${measuredText}</td>
            <td class="${loudnessCell}">${row.loudnessPass ? 'pass' : 'fail'}</td>
            <td class="${truePeakCell}">${row.truePeakPass ? 'pass' : 'fail'} (≤ ${row.maxTruePeakDbtp.toFixed(1)} dBTP)</td>
            <td class="${row.pass ? 'pass' : 'fail'}">${row.pass ? 'PASS' : 'FAIL'}</td>`;
        tbody.appendChild(tr);
    }
    el('qcPanel').style.display = '';
}

async function encodeFile(file) {
    setStatus(`Decoding ${file.name}...`, false);
    el('resultPanel').style.display = 'none';
    el('qcPanel').style.display = 'none';
    lastStreamBytes = null;

    const format = Number(el('format').value);
    const targetRate = Number(el('sampleRate').value);
    const bitrate = Number(el('bitrate').value);

    const arrayBuffer = await file.arrayBuffer();
    // The AudioContext's own sampleRate resamples decodeAudioData's output to
    // one of AC-3's three supported rates - no hand-written sample-rate
    // converter needed.
    const audioCtx = new AudioContext({ sampleRate: targetRate });
    const audioBuffer = await audioCtx.decodeAudioData(arrayBuffer);
    await audioCtx.close();

    const { channels, layout } = reorderedChannels(audioBuffer);
    const totalSamples = channels[0].length;

    if (layout.wide && format !== 1) {
        throw new Error(`${layout.label} needs E-AC-3 (a 5.1 bed plus dependent substreams) - switch Format to E-AC-3`);
    }

    // Pass 1: measure. The QC meter now feeds the ENCODER too - dialnorm is
    // derived from the measured integrated loudness - so the whole file is
    // metered before the encoder is even constructed, and QC still sees the
    // real, unpadded tail of the file. The meter's channel order for the
    // wide layouts comes from the module itself (QcMeter.meterOrderForWav),
    // not from a second JS-side table.
    setStatus(`Measuring ${layout.label} loudness...`, false);
    const qc = new encodeModule.QcMeter(layout.wasmLayout, targetRate);
    const meterOrder = layout.wide
        ? encodeModule.QcMeter.meterOrderForWav(layout.wasmLayout)
        : null;
    const spf = 1536; // one AC-3 frame; any chunk size suits the meter
    for (let start = 0; start < totalSamples; start += spf) {
        const end = Math.min(start + spf, totalSamples);
        const slice = channels.map((c) => c.subarray(start, end));
        qc.push(meterOrder ? meterOrder.map((wavIndex) => slice[wavIndex]) : slice);
    }
    const dialnorm = dialnormFor(qc.integratedLkfs());

    // Pass 2: encode, carrying the measured dialnorm in every frame's BSI.
    setStatus(`Encoding ${layout.label} at ${bitrate} kbps, dialnorm ${dialnorm}...`, false);
    const encoder = new encodeModule.Encoder(format, layout.wasmLayout, targetRate, bitrate, dialnorm);

    const chunks = [];
    let totalBytes = 0;
    for (let start = 0; start < totalSamples; start += spf) {
        const end = Math.min(start + spf, totalSamples);
        const slice = channels.map((c) => c.subarray(start, end));

        // The encoder needs exactly spf samples per call; the final frame is
        // silence-padded to that length rather than dropped, so the encoded
        // stream covers the whole input (at the cost of up to one frame's
        // worth of trailing silence - kSamplesPerFrame/sampleRate, well
        // under 32ms).
        const frame = end - start === spf ? slice : slice.map((c) => {
            const padded = new Float32Array(spf);
            padded.set(c);
            return padded;
        });

        const bytes = encoder.encodeFrame(frame);
        if (!bytes) {
            throw new Error(`encode failed: ${encoder.error()}`);
        }
        // Copy out immediately - the view is only valid until the next call
        // into the module (see encoder_bindings.cpp's own note).
        const copy = Uint8Array.from(bytes);
        chunks.push(copy);
        totalBytes += copy.length;
    }

    lastStreamBytes = concatBytes(chunks, totalBytes);
    lastFormat = format;
    lastSampleRate = targetRate;

    renderQcTable(qc.verdicts());
    // On the wide path, say HOW the source landed on the layout: carried
    // channel-for-channel, or rendered onto it by direction (the plan
    // routing's own distinction) - "encoded 7.1.4" should never silently
    // mean "panned around a 12-speaker room".
    const routeNote = layout.wide
        ? (encoder.sourceWasCarried()
            ? ' Channels carried 1:1.'
            : ' Source rendered onto the layout by direction.')
        : '';
    setStatus(
        `Encoded ${(totalSamples / targetRate).toFixed(1)}s of ${layout.label} audio into ` +
            `${lastStreamBytes.length.toLocaleString()} bytes, dialnorm ${dialnorm}.${routeNote}`,
        false);
    el('resultPanel').style.display = '';
}

// --- Live microphone capture ---------------------------------------------
//
// getUserMedia -> AudioWorklet -> 1536-sample frames -> the same Encoder /
// QcMeter classes the file path uses. The worklet processor is inlined as a
// Blob so the demo stays a flat directory of files with no extra fetch; it
// posts each 128-frame render quantum's de-interleaved input to the main
// thread, which re-blocks into encoder frames.
//
// dialnorm needs a measured loudness BEFORE the first frame is encoded, so
// capture opens with a short measure-only pre-roll (~1.5s): audio is metered
// and buffered but not yet encoded, then the encoder is created from the
// pre-roll's reading and the buffered audio drains through it - nothing of
// the take is lost. BS.1770's integrated measure may still be gated that
// early, so the momentary reading is the fallback before the default 31.
const CAPTURE_WORKLET = `
class CaptureProcessor extends AudioWorkletProcessor {
    process(inputs) {
        const input = inputs[0];
        if (input.length > 0) {
            this.port.postMessage(input.map((c) => Float32Array.from(c)));
        }
        return true;
    }
}
registerProcessor('ac3forge-capture', CaptureProcessor);
`;

let micState = null;

function onMicChunk(state, channels) {
    if (!state.layout) {
        const count = Math.min(channels.length, 2);
        state.layout = LAYOUTS[count];
        state.qc = new encodeModule.QcMeter(state.layout.wasmLayout, state.targetRate);
        state.pending = Array.from({ length: count }, () => []);
    }
    const count = state.pending.length;
    const usable = [];
    for (let i = 0; i < count; i++) usable.push(channels[i] || channels[0]);
    state.qc.push(usable);
    for (let i = 0; i < count; i++) state.pending[i].push(usable[i]);
    state.pendingSamples += usable[0].length;

    if (!state.encoder) {
        if (state.pendingSamples < state.preRollSamples) return;
        startMicEncoder(state);
    }
    drainMicFrames(state, false);
}

function startMicEncoder(state) {
    const integrated = state.qc.integratedLkfs();
    const momentary = state.qc.momentaryLkfs();
    state.dialnorm = dialnormFor(
        integrated === null || integrated === undefined ? momentary : integrated);
    state.encoder = new encodeModule.Encoder(
        state.format, state.layout.wasmLayout, state.targetRate, state.bitrate, state.dialnorm);
}

function drainMicFrames(state, flush) {
    const spf = state.encoder.samplesPerFrame();
    while (state.pendingSamples >= spf || (flush && state.pendingSamples > 0)) {
        // Assemble one frame per channel from the queued worklet chunks; on
        // flush the tail is silence-padded to a whole frame, same policy as
        // the file path's final frame.
        const frame = state.pending.map((queue) => {
            const out = new Float32Array(spf);
            let at = 0;
            while (at < spf && queue.length > 0) {
                const head = queue[0];
                const take = Math.min(head.length, spf - at);
                out.set(head.subarray(0, take), at);
                if (take === head.length) queue.shift();
                else queue[0] = head.subarray(take);
                at += take;
            }
            return out;
        });
        state.pendingSamples = Math.max(0, state.pendingSamples - spf);
        const bytes = state.encoder.encodeFrame(frame);
        if (!bytes) throw new Error(`encode failed: ${state.encoder.error()}`);
        const copy = Uint8Array.from(bytes);
        state.chunks.push(copy);
        state.totalBytes += copy.length;
        state.encodedSamples += spf;
    }
}

async function startMicCapture() {
    const format = Number(el('format').value);
    const targetRate = Number(el('sampleRate').value);
    const bitrate = Number(el('bitrate').value);

    el('resultPanel').style.display = 'none';
    el('qcPanel').style.display = 'none';
    lastStreamBytes = null;

    // Processing (echo cancellation and friends) off: this is a capture
    // tool, not a call - meter and encode what the microphone actually
    // heard.
    const stream = await navigator.mediaDevices.getUserMedia({
        audio: {
            channelCount: { ideal: 2 },
            echoCancellation: false,
            noiseSuppression: false,
            autoGainControl: false,
        },
    });
    // The AudioContext's own rate makes the browser resample the microphone
    // to the selected coding rate, same trick as decodeAudioData above.
    const audioCtx = new AudioContext({ sampleRate: targetRate });
    const workletUrl = URL.createObjectURL(
        new Blob([CAPTURE_WORKLET], { type: 'text/javascript' }));
    try {
        await audioCtx.audioWorklet.addModule(workletUrl);
    } finally {
        URL.revokeObjectURL(workletUrl);
    }
    const source = audioCtx.createMediaStreamSource(stream);
    const capture = new AudioWorkletNode(audioCtx, 'ac3forge-capture');
    source.connect(capture);

    const state = {
        stream, audioCtx,
        format, targetRate, bitrate,
        layout: null, qc: null, encoder: null, dialnorm: null,
        pending: [], pendingSamples: 0,
        preRollSamples: Math.round(targetRate * 1.5),
        chunks: [], totalBytes: 0, encodedSamples: 0,
        ticker: null,
    };
    micState = state;

    capture.port.onmessage = (event) => {
        if (micState !== state) return;
        try {
            onMicChunk(state, event.data);
        } catch (error) {
            stopMicCapture().catch(() => {});
            setStatus(String(error.message || error), true);
        }
    };

    state.ticker = setInterval(() => {
        if (micState !== state || !state.encoder) return;
        const momentary = state.qc.momentaryLkfs();
        const level = momentary === null || momentary === undefined
            ? 'below gate'
            : `${momentary.toFixed(1)} LKFS momentary`;
        setStatus(
            `Recording ${state.layout.label}: ${(state.encodedSamples / state.targetRate).toFixed(1)}s, ` +
                `${state.totalBytes.toLocaleString()} bytes, dialnorm ${state.dialnorm}, ${level}.`,
            false);
    }, 500);

    el('micBtn').textContent = 'Stop and finish';
    setStatus('Measuring the microphone for dialnorm (about 1.5s)...', false);
}

async function stopMicCapture() {
    const state = micState;
    if (!state) return;
    micState = null;
    clearInterval(state.ticker);
    for (const track of state.stream.getTracks()) track.stop();
    await state.audioCtx.close().catch(() => {});
    el('micBtn').textContent = 'Record from microphone';

    if (!state.encoder && state.layout && state.pendingSamples > 0) {
        // Stopped inside the pre-roll: the take still gets delivered, from
        // whatever the meter managed to read.
        startMicEncoder(state);
    }
    if (!state.encoder) {
        setStatus('Capture stopped before any audio arrived.', true);
        return;
    }
    drainMicFrames(state, true);
    lastStreamBytes = concatBytes(state.chunks, state.totalBytes);
    lastFormat = state.format;
    lastSampleRate = state.targetRate;
    renderQcTable(state.qc.verdicts());
    setStatus(
        `Captured ${(state.encodedSamples / state.targetRate).toFixed(1)}s of ${state.layout.label} ` +
            `microphone audio into ${state.totalBytes.toLocaleString()} bytes, dialnorm ${state.dialnorm}.`,
        false);
    el('resultPanel').style.display = '';
}

function downloadStream() {
    if (!lastStreamBytes) return;
    const ext = lastFormat === 1 ? 'ec3' : 'ac3';
    const blob = new Blob([lastStreamBytes], { type: 'application/octet-stream' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `ac3forge-encode.${ext}`;
    document.body.appendChild(a);
    a.click();
    a.remove();
    URL.revokeObjectURL(url);
}

async function previewRoundTrip() {
    if (!lastStreamBytes) return;
    setStatus('Decoding the encoded stream for preview...', false);

    // The old whole-file Decoder class was replaced by scanStream() +
    // PushDecoder (UX5's push-frame rework of decoder_bindings.cpp): scan
    // once for access-unit boundaries, then push each unit and collect that
    // frame's PCM. holdBack frames (dependent-substream latency) carry no
    // PCM yet; the flush() tail (at most one frame) is skipped - inaudible
    // for a preview.
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
            // Copy out of the WASM heap view immediately, same contract as
            // the encode-side view above.
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
    setStatus(`Playing round-trip preview (${lastSampleRate} Hz, ${channelCount} ch).`, false);
}

function wireUp() {
    const fileInput = el('fileInput');
    const dropZone = el('dropZone');
    const encodeBtn = el('encodeBtn');
    let selectedFile = null;

    const onFile = (file) => {
        if (!file) return;
        selectedFile = file;
        encodeBtn.disabled = false;
        setStatus(`Selected ${file.name}`, false);
    };

    fileInput.addEventListener('change', () => onFile(fileInput.files[0]));

    dropZone.addEventListener('dragover', (event) => {
        event.preventDefault();
        dropZone.classList.add('drag');
    });
    dropZone.addEventListener('dragleave', () => dropZone.classList.remove('drag'));
    dropZone.addEventListener('drop', (event) => {
        event.preventDefault();
        dropZone.classList.remove('drag');
        const file = event.dataTransfer.files[0];
        if (file) {
            fileInput.files = event.dataTransfer.files;
            onFile(file);
        }
    });

    encodeBtn.addEventListener('click', async () => {
        if (!selectedFile) return;
        encodeBtn.disabled = true;
        try {
            await encodeFile(selectedFile);
        } catch (error) {
            setStatus(String(error.message || error), true);
        } finally {
            encodeBtn.disabled = false;
        }
    });

    el('micBtn').addEventListener('click', () => {
        if (micState) {
            stopMicCapture().catch((error) => setStatus(String(error.message || error), true));
        } else {
            startMicCapture().catch((error) => {
                micState = null;
                el('micBtn').textContent = 'Record from microphone';
                setStatus(String(error.message || error), true);
            });
        }
    });
    el('downloadBtn').addEventListener('click', downloadStream);
    el('previewBtn').addEventListener('click', () => {
        previewRoundTrip().catch((error) => setStatus(String(error.message || error), true));
    });
}

(async function main() {
    wireUp();
    setStatus('Loading WASM modules...', false);
    try {
        await loadModules();
        setStatus('Ready - drop a .wav file to begin.', false);
    } catch (error) {
        setStatus(`failed to load WASM modules: ${error.message || error}`, true);
    }
})();
