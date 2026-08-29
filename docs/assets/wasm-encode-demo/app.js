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
        throw new Error(`unsupported channel count ${count} - this demo supports mono, stereo or 5.1 WAV files`);
    }
    const raw = [];
    for (let i = 0; i < count; i++) raw.push(audioBuffer.getChannelData(i));
    if (count !== 6) return { channels: raw, layout };
    const reordered = WAV_5_1_TO_AC3_ORDER.map((wavIndex) => raw[wavIndex]);
    return { channels: reordered, layout };
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

    setStatus(`Encoding ${layout.label} at ${bitrate} kbps...`, false);
    const encoder = new encodeModule.Encoder(format, layout.wasmLayout, targetRate, bitrate);
    const qc = new encodeModule.QcMeter(layout.wasmLayout, targetRate);
    const spf = encoder.samplesPerFrame();

    const chunks = [];
    let totalBytes = 0;
    for (let start = 0; start < totalSamples; start += spf) {
        const end = Math.min(start + spf, totalSamples);
        const slice = channels.map((c) => c.subarray(start, end));

        // QC sees the real, unpadded tail of the file.
        qc.push(slice);

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
    setStatus(
        `Encoded ${(totalSamples / targetRate).toFixed(1)}s of ${layout.label} audio into ` +
            `${lastStreamBytes.length.toLocaleString()} bytes.`,
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

    const decoder = new decodeModule.Decoder();
    const ok = decoder.decode(lastStreamBytes);
    if (!ok) {
        const message = decoder.error();
        decoder.delete();
        setStatus(`round-trip decode failed: ${message}`, true);
        return;
    }

    const sampleRate = decoder.sampleRate();
    const channelCount = decoder.channelCount();
    const audioCtx = new AudioContext({ sampleRate });
    const frameCount = decoder.channelPcm(0).length;
    const outBuffer = audioCtx.createBuffer(channelCount, frameCount, sampleRate);
    for (let ch = 0; ch < channelCount; ch++) {
        // Copy out of the WASM heap view immediately, same contract as the
        // encode-side view above.
        outBuffer.copyToChannel(Float32Array.from(decoder.channelPcm(ch)), ch);
    }
    decoder.delete();

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
