// The board's web page, a client of the REST routes and nothing else.
// planning/esp32-device-ui.md is its design.
(() => {
  'use strict';
  const POLL_MS = 1000; // while the page is visible
  const RETRY_MS = 5000; // while GET /status fails
  const TIMEOUT_MS = 4000; // for any one request
  const TOAST_MS = 6000; // how long a message that is not an error stays in view
  const RESTART_MS = 60000; // how long GET /firmware is read at each poll after asking for a restart
  const FRAME_US = 32000; // 1,536 samples at 48 kHz
  const FLOOR_DB = -60; // a level meter's left end; 0 dBFS is its right
  const HOT_DB = -6; // a peak past this shows in the accent colour
  const ACMOD = ['1+1', '1/0', '2/0', '3/0', '2/1', '3/1', '2/2', '3/2']; // A/52 Table 5.8
  const HOW = {
    loro: 'folded to two channels by the decoder (Lo/Ro)',
    ltrt: 'folded to two channels by the decoder (Lt/Rt)',
    mono: 'folded to one channel by the decoder',
    channels: 'each channel on the speaker at its location',
    objects: 'objects placed by their positions',
  };
  // The Sendspin player's words for what /status reports of it.
  const LINK = { 'long-term': 'Paired, encrypted', pairing: 'Pairing, encrypted', sentinel: 'Encrypted, not paired' };
  const PLAYING = { bursts: 'Bursts, decoded here', pcm: 'PCM', idle: 'Nothing' };
  const KINDS = { wifi: 'Wi-Fi', ethernet: 'Ethernet' }; // /status's network.kind
  // GET /firmware's words (ac3forge/firmware_status.hpp): a slot's state, and
  // an upload's stage.
  const SLOT = { valid: 'accepted', trial: 'on trial', new: 'written, not started yet', invalid: 'failed its trial', aborted: 'stopped during its trial' };
  const STAGE = { waiting: 'Stopping the player', erasing: 'Erasing', writing: 'Writing', checking: 'Checking', restarting: 'Restarting' };
  // The chip ID an image's header names, for each target a Hearth sink is
  // built for (GET /hardware's "target").
  const CHIPS = { esp32s3: 9, esp32c6: 13, esp32p4: 18 };

  const $ = (id) => document.getElementById(id);
  const num = (v) => typeof v === 'number' && Number.isFinite(v);
  const str = (v) => (typeof v === 'string' ? v : undefined);
  const count = (v) => (num(v) ? v.toLocaleString('en-US') : undefined);
  const bytes = (v) => (num(v) ? count(v) + ' bytes' : undefined);
  const slots = (n) => count(n) + (n === 1 ? ' slot' : ' slots');
  // A byte count no one would want to read as itself: GET /hardware's PSRAM
  // size, which is either 0 (none fitted or brought up) or tens of millions.
  const mib = (v) => (num(v) && v > 0 ? (v / (1024 * 1024)).toLocaleString('en-US', { maximumFractionDigits: 1 }) + ' MiB' : 'None');
  const names = (v) => v.split(',').join(' ');
  // The slots a layout needs, where the page can count them: a name's three
  // figures added, as OutputLayout reads F.L.H, or a list's tokens. The
  // firmware decides; this only explains a refusal and marks the presets
  // the sink cannot carry.
  const need = (text) => {
    const m = /^([1234579])\.([012])(?:\.([0246]))?$/.exec(text);
    return m ? +m[1] + +m[2] + +(m[3] || 0) : text.includes(',') ? text.split(',').length : undefined;
  };
  const ms = (us) => (us / 1000).toFixed(1) + ' ms';
  const minutes = (sec) => Math.floor(sec / 60) + ':' + String(sec % 60).padStart(2, '0');
  const db = (v) => (num(v) ? (v <= -120 ? 'Silent' : v.toFixed(1) + ' dB') : '');
  // Where a level sits along a meter, as a CSS percentage.
  const along = (v) => (num(v) ? Math.min(100, Math.max(0, 100 - (v / FLOOR_DB) * 100)).toFixed(1) : '0') + '%';
  const clock = (t) => new Date(t).toTimeString().slice(0, 8);
  const put = (id, text, bad) => {
    $(id).textContent = text;
    $(id).classList.toggle('error', !!bad);
  };
  // A row and its value - a description list's, or the network's in Settings -
  // hidden when /status does not carry it.
  const row = (id, value) => {
    const dd = $(id);
    dd.parentElement.hidden = value === undefined;
    dd.textContent = value ?? '';
  };
  const radios = (name) => document.querySelectorAll('input[name="' + name + '"]');

  // The one live region, shown as a toast: what an action did, and the state
  // when it changes. An error stays until something replaces it or it is
  // clicked away; anything else leaves by itself.
  let toastTimer = 0;
  const hideToast = () => $('outcome').classList.add('gone');
  function say(text, bad) {
    put('outcome', text, bad);
    $('outcome').classList.remove('gone');
    clearTimeout(toastTimer);
    if (!bad) toastTimer = setTimeout(hideToast, TOAST_MS);
  }

  let status = {};
  let timer = 0;
  let polling = false;
  let waiting = [];
  let failing = 0;
  let shown = '';
  let filled = false;
  let code = '';
  let hardwareShown = false;
  let hardware = {};
  // GET /firmware: the last body (null on a board without the route), whether
  // to read it at the next poll, the running version the page was loaded
  // against, and when the page last asked the board to restart (0: not since
  // the board last answered after failing to).
  let fw = null;
  let fwDue = true;
  let fwReading = false;
  let loaded = '';
  let restarting = 0;
  let sending = false;
  // GET /status reads begun, and for each choice the page sends, the reads
  // begun by the time its request was answered (Infinity while it is out): a
  // read begun before then says what the board had before the choice.
  let reads = 0;
  const settled = {};

  async function call(method, path, body) {
    const abort = new AbortController();
    const t = setTimeout(() => abort.abort(), TIMEOUT_MS);
    try {
      const r = await fetch(path, { method, body, cache: 'no-store', signal: abort.signal });
      return { status: r.status, text: (await r.text()).trim() };
    } catch {
      throw new Error(abort.signal.aborted ? 'no answer in ' + TIMEOUT_MS / 1000 + ' s' : 'no connection');
    } finally {
      clearTimeout(t);
    }
  }

  // One GET /status at a time, the next when it ends, none while hidden.
  function schedule(delay) {
    clearTimeout(timer);
    if (document.visibilityState !== 'hidden') timer = setTimeout(poll, delay);
  }

  // The promise settles once a poll begun by this call has rendered, or,
  // with one already out, the next one, so that nothing read before the
  // call renders after it settles.
  async function poll() {
    if (polling) return new Promise((resolve) => waiting.push(resolve));
    polling = true;
    const asked = waiting.splice(0);
    const read = ++reads;
    clearTimeout(timer);
    let delay = POLL_MS;
    try {
      const r = await call('GET', 'status');
      if (r.status !== 200) throw new Error('GET /status answered ' + r.status);
      let s;
      try {
        s = JSON.parse(r.text);
      } catch {
        // not JSON: reported below
      }
      if (!s || typeof s !== 'object' || Array.isArray(s)) throw new Error('GET /status sent no JSON object');
      if (failing) {
        say('The player is answering again.');
        // It may have restarted, into another image.
        fwDue = true;
        restarting = 0;
      }
      failing = 0;
      put('link', 'Status read at ' + clock(Date.now()) + '.');
      render(s, read);
      board();
    } catch (e) {
      delay = RETRY_MS;
      if (!failing) say(restarting ? 'The board is restarting.' : 'No status: ' + e.message + '.', !restarting);
      failing ||= Date.now();
      put('link', 'No status since ' + clock(failing) + ': ' + e.message + '.', true);
    }
    polling = false;
    schedule(waiting.length ? 0 : delay);
    asked.forEach((resolve) => resolve());
  }

  // "failed" with no failed run behind it: the source did not open.
  const reason = (state, s) =>
    s.failed === true
      ? s.why === 'sample rate' && num(s.error)
        ? "Stopped: the stream's sample rate, " + count(s.error) + " Hz, is not the sink's."
        : 'Stopped by a ' + (str(s.why) || 'player') + ' error' + (num(s.error) ? ' (' + s.error + ')' : '') + '.'
      : state === 'failed'
        ? 'The location may not have opened.'
        : '';

  function stream(s) {
    if (!('stream' in s)) return {};
    const t = s.stream;
    if (!t || typeof t !== 'object') return { codec: 'Not known yet' };
    const codec = [
      str(t.codec),
      num(t.substreams) ? t.substreams + ' substream' + (t.substreams === 1 ? '' : 's') : '',
      num(t.dialnorm) ? 'dialnorm ' + t.dialnorm : '',
    ].filter(Boolean);
    return {
      codec: codec.length ? codec.join(', ') : undefined,
      channels: num(t.channels) ? t.channels + (str(t.coded) ? ': ' + names(t.coded) : ACMOD[t.acmod] ? ' (' + ACMOD[t.acmod] + ')' : '') : undefined,
      objects: t.objects === true ? (t.objects_rendered === true ? 'Carried, placed onto the layout' : 'Carried, not placed') : t.objects === false ? 'None' : undefined,
      // This play's layout, and how it is served; `layout` is the next play's.
      layout: str(t.layout),
      output: num(t.slots) ? (str(t.layout) ? t.layout + ', ' : '') + slots(t.slots) + (HOW[t.render] ? ': ' + HOW[t.render] : '') : undefined,
      silent: str(t.silent) ? names(t.silent).replace(/ /g, ', ') + (t.render === 'objects' ? ': no object has reached these yet' : ': nothing in the stream for these') : undefined,
    };
  }

  // The network the board is on: /status's "network" object.
  function network(n) {
    if (!n || typeof n !== 'object') return undefined;
    const link = KINDS[n.kind] || str(n.kind) || 'A network';
    const where = [n.kind === 'wifi' ? (str(n.ssid) ? link + ' ' + n.ssid : link + ', not joined') : link];
    if (num(n.rssi_dbm)) where.push(n.rssi_dbm + ' dBm');
    where.push(str(n.address) || 'no address');
    return where.join(' \u00b7 ');
  }

  function played(frames) {
    if (!num(frames)) return undefined;
    return minutes(Math.floor((frames * FRAME_US) / 1e6)) + ' (' + count(frames) + (frames === 1 ? ' frame)' : ' frames)');
  }

  // A frame's 32 ms, three ways; with a DAC behind it, the sink's part is
  // mostly the wait for the DAC.
  function timing(s) {
    const ran = num(s.frames) && s.frames > 0 && num(s.us_per_frame);
    const split = ran && num(s.render_us_per_frame) && num(s.sink_us_per_frame);
    const parts = split
      ? [Math.max(0, s.us_per_frame - s.render_us_per_frame - s.sink_us_per_frame), s.render_us_per_frame, s.sink_us_per_frame]
      : [ran ? s.us_per_frame : 0, 0, 0];
    $('timing-note').hidden = ran;
    $('bar').hidden = !ran;
    row('t-decoder', split ? ms(parts[0]) : undefined);
    row('t-render', split ? ms(parts[1]) : undefined);
    row('t-sink', split ? ms(parts[2]) + ', with any wait for the DAC' : undefined);
    row('t-frame', ran ? ms(s.us_per_frame) + ' of every 32 ms' : undefined);
    row('t-worst', ran && num(s.worst_frame_us) ? ms(s.worst_frame_us) : undefined);
    row('t-load', ran && num(s.realtime_permille) ? (s.realtime_permille / 10).toFixed(1) + '% of real time' : undefined);
    row('t-ring', s.ring_low === null ? 'Not measured yet' : num(s.ring_low) ? bytes(s.ring_low) + (s.ring_low ? '' : ': the decoder waited for the source') : undefined);
    let left = 100;
    ['bar-decoder', 'bar-render', 'bar-sink'].forEach((id, i) => {
      const width = Math.min(left, (parts[i] / FRAME_US) * 100);
      left -= width;
      $(id).style.width = width.toFixed(1) + '%';
    });
  }

  // One output's row in the levels table: its number and a meter - the RMS
  // filled in, the peak a mark - then both figures as text. The meter is
  // drawn for the eye only; the figures are what a screen reader reads.
  function levelRow(peak, rms, i) {
    const tr = document.createElement('tr');
    const th = document.createElement('th');
    const n = document.createElement('span');
    const meter = document.createElement('span');
    th.scope = 'row';
    n.className = 'n';
    n.textContent = String(i + 1);
    meter.className = peak > HOT_DB ? 'meter hot' : 'meter';
    meter.setAttribute('aria-hidden', 'true');
    meter.style.setProperty('--r', along(rms));
    meter.style.setProperty('--p', along(peak));
    th.append(n, meter);
    tr.append(th);
    tr.insertCell().textContent = db(peak);
    tr.insertCell().textContent = db(rms);
    return tr;
  }

  // A Sendspin player's part of /status: who plays to it, how well, and a
  // pairing in progress, whose code is announced once when it appears.
  function sendspin(p) {
    const on = !!p && typeof p === 'object';
    $('sendspin').hidden = !on;
    // With a Sendspin player on the board, Now is the board's own player only.
    $('now-note').hidden = !on;
    if (!on) return;
    const c = str(p.pairing_code) || '';
    const spaced = c.replace(/(\d{3})(?=\d)/g, '$1 ');
    if (c && c !== code) say('Pairing code ' + spaced + ': enter it where the server asks for it.');
    code = c;
    $('ss-digits').textContent = spaced;
    $('ss-code').hidden = !c;
    const server = str(p.server);
    const note = p.pairing_held === true ? 'Pairing is held back after codes that did not match.'
      : server ? '' : 'No server is connected. Servers on this network find this board by its name.';
    put('ss-note', note);
    $('ss-note').hidden = !note;
    const playing = str(p.playing) && p.playing !== 'idle';
    row('ss-server', server ? server + (str(p.dialect) ? ' (' + p.dialect + ')' : '') : undefined);
    row('ss-link', LINK[p.psk] ? LINK[p.psk] + (str(p.role) ? ', ' + p.role : '') : undefined);
    row('ss-clock', server ? (p.clock_converged === true && num(p.clock_error_us) ? 'In step with the server, within ' + ms(p.clock_error_us) : 'Settling') : undefined);
    row('ss-playing', PLAYING[p.playing] && (PLAYING[p.playing] + (playing && num(p.bursts) ? ', ' + count(p.bursts) + ' chunks' : '')));
    row('ss-timing', playing && num(p.error_us) && num(p.worst_error_us)
      ? ms(Math.abs(p.error_us)) + (p.error_us < 0 ? ' early' : ' late') + ', ' + ms(Math.abs(p.worst_error_us)) + ' at worst' : undefined);
    row('ss-underruns', num(p.bursts) && p.bursts > 0 ? count(p.underruns) : undefined);
    row('ss-lost', num(p.bursts) && p.bursts > 0 ? count(p.late) + ' late, ' + count(p.dropped) + ' with no room, ' + count(p.invalid) + ' not valid' : undefined);
    row('ss-paired', count(p.paired));
    row('ss-outcome', str(p.pairing_outcome) || undefined);
    const peaks = playing && Array.isArray(p.peak_db) ? p.peak_db : [];
    const rms = Array.isArray(p.rms_db) ? p.rms_db : [];
    $('ss-levels').hidden = !peaks.length;
    $('ss-rows').replaceChildren(...peaks.map((peak, i) => levelRow(peak, rms[i], i)));
    $('ss-cancel').hidden = !c;
    $('ss-reset').hidden = p.pairing_held !== true;
    listServers(p);
  }

  // GET /pairing's servers, read again when what /status says of them
  // changes: how many, who is connected, how the last pairing ended. Not read
  // at all while the count is none.
  let listed = '';
  let listing = 0;
  async function listServers(p) {
    const now = [p.paired, p.connections, p.server_id, p.pairing_outcome].join();
    if (now === listed) return;
    listed = now;
    const mine = ++listing;
    let list = [];
    try {
      if (p.paired > 0) {
        const r = await call('GET', 'pairing');
        list = r.status === 200 ? JSON.parse(r.text).servers : [];
      }
    } catch {
      listed = ''; // read again at the next poll
      list = undefined;
    }
    if (mine !== listing || !Array.isArray(list)) return;
    // Not a row without a server_id: its Forget would send a bare "forget",
    // which the board reads as every server.
    const rows = list.filter((s) => str(s.server_id)).map(serverRow);
    $('ss-servers').hidden = !rows.length;
    $('ss-servers').replaceChildren(...rows);
  }

  // A server with no name yet goes by its server_id, so two of them are still
  // two different buttons.
  function serverRow(s) {
    const li = document.createElement('li');
    const who = li.appendChild(document.createElement('span'));
    const id = s.server_id.slice(0, 8);
    const name = str(s.name) || 'the server ' + id;
    who.textContent = str(s.name) || 'No name yet';
    who.appendChild(document.createElement('small')).textContent = [id,
      s.connected === true ? 'connected' : s.seen === true ? 'seen since the board started' : 'not seen since the board started',
      s.last_playback === true && 'the last to play'].filter(Boolean).join(' · ');
    const forget = li.appendChild(document.createElement('button'));
    forget.type = 'button';
    forget.className = 'danger';
    forget.textContent = 'Forget';
    forget.setAttribute('aria-label', 'Forget ' + name);
    forget.addEventListener('click', () => ask('Forget ' + name + '?',
      'To play here again, ' + name + ' has to pair again. The board keeps its other pairings.', 'Forget',
      () => pairing('Forget ' + name, 'forget ' + s.server_id, 'Forgot ' + name + ': it has to pair again.')));
    return li;
  }

  // What this board is: fetched once, since nothing in it changes while the
  // board runs (unlike everything else this page polls). Tried again at the
  // next successful status poll if it failed the first time - a board whose
  // network comes up slowly should still end up showing this.
  function renderHardware(hw) {
    $('hw-note').hidden = true;
    row('hw-chip', str(hw.chip) && hw.chip + (str(hw.revision) ? ', revision ' + hw.revision : ''));
    row('hw-cores', num(hw.cores) ? String(hw.cores) : undefined);
    row('hw-clock', num(hw.cpu_freq_mhz) ? hw.cpu_freq_mhz + ' MHz' : undefined);
    row('hw-arithmetic', hw.fpu === true ? 'Hardware floating point' : hw.fpu === false ? 'Fixed-point (no floating-point unit)' : undefined);
    row('hw-psram', mib(hw.psram_bytes));
    row('hw-sink', num(hw.sink_max_slots) ? slots(hw.sink_max_slots) + (num(hw.sink_max_slots_bits) ? ' at ' + hw.sink_max_slots_bits + '-bit' : '') : undefined);
    row('hw-firmware', str(hw.project) && hw.project + (str(hw.version) ? ' ' + hw.version : ''));
    row('hw-idf', str(hw.idf_version));
    const notices = Array.isArray(hw.notices) ? hw.notices : [];
    $('hw-notices').hidden = !notices.length;
    $('hw-notices').replaceChildren(...notices.map((text) => {
      const li = document.createElement('li');
      li.textContent = text;
      return li;
    }));
  }

  async function loadHardware() {
    try {
      const r = await call('GET', 'hardware');
      if (r.status !== 200) throw new Error('GET /hardware answered ' + r.status);
      const hw = JSON.parse(r.text);
      if (!hw || typeof hw !== 'object' || Array.isArray(hw)) throw new Error('GET /hardware sent no JSON object');
      renderHardware(hw);
      hardware = hw;
      hardwareShown = true;
    } catch {
      // Left as "Reading what this board is."; tried again at the next
      // successful status poll, the same board this page is already reading.
    }
  }

  // What the board is and what it runs, after a status read and one request
  // at a time, as the board has few sockets: GET /hardware until it has
  // answered, and GET /firmware when it is due. After a restart the page asked
  // for, GET /firmware at each poll for a while: a board back within a few
  // seconds may never fail a poll, since the browser sends a request that
  // meets a closed connection again.
  async function board() {
    if (!hardwareShown) await loadHardware();
    const asked = restarting && Date.now() - restarting < RESTART_MS;
    if (!sending && (fwDue || asked || (fw && (fw.trial || fw.upload)))) await loadFirmware();
  }

  // A slot as GET /firmware reports it: its image, the state the bootloader
  // keeps for it, and whether it checked out when the board last read it.
  const slotText = (s) =>
    s ? (s.state === 'empty' ? 'Empty' : [s.version + ' in ' + s.label, SLOT[s.state], s.intact === true ? 'intact' : s.intact === false ? 'damaged' : ''].filter(Boolean).join(', ')) : undefined;

  // The slots, a trial, an upload and how the last update ended; what can be
  // done is what the board would take now.
  function renderFirmware(f) {
    fw = f;
    $('firmware').hidden = !f;
    if (!f) return;
    const { trial: t, upload: u, other: o, last_update: l } = f;
    const idle = !u && !sending;
    // An image the board can go back to, as PUT /firmware/rollback judges it.
    const back = !!o && !['empty', 'invalid', 'aborted'].includes(o.state) && o.intact !== false;
    $('fw-note').hidden = f.mode !== 'flash';
    row('fw-running', slotText(f.running));
    row('fw-other', slotText(o));
    row('fw-trial', t ? Math.floor(t.healthy_for_ms / 1000) + ' of ' + t.hold_ms / 1000 + ' s held, ' + minutes(Math.ceil(t.remaining_ms / 1000)) + ' left' + (t.waiting_for.length ? '; waiting for ' + t.waiting_for.join(', ') : '') : undefined);
    if (!sending) row('fw-upload', u ? (STAGE[u.stage] || u.stage) + ': ' + count(u.received) + ' of ' + bytes(u.total) : undefined);
    row('fw-last', l ? [l.version, l.result].filter(Boolean).join(', ') + (l.reason ? ': ' + l.reason : '') : undefined);
    $('fw-pick').hidden = !o || !!t || !idle;
    $('fw-restart').hidden = !!t || !idle;
    $('fw-rollback').hidden = !(back || t) || !idle;
    $('fw-rollback').textContent = 'Roll back to ' + (o && o.version);
    $('fw-sent').hidden = !sending;
  }

  // Read when the page loads, when the board answers again after it did not,
  // at each poll while a trial or an upload runs, and for a while after a
  // restart the page asked for (board()). A board running another image than
  // the one the page was loaded against may serve another page, so the page
  // loads again.
  async function loadFirmware() {
    if (fwReading) return;
    fwReading = true;
    fwDue = false;
    try {
      const r = await call('GET', 'firmware');
      if (r.status === 404) return renderFirmware(null);
      const f = JSON.parse(r.text);
      if (loaded && f.running.version !== loaded) return location.reload();
      loaded = f.running.version;
      renderFirmware(f);
    } catch {
      fwDue = true; // read again at the next poll
    } finally {
      fwReading = false;
    }
  }

  // The settings, filled in from what the board says rather than from what
  // this page last sent: a choice whose request is out, or that a read begun
  // before its answer would undo, keeps what the user chose, and a field is
  // never changed under someone typing in it.
  function settings(s, read) {
    const fresh = (key) => read > (settled[key] ?? 0);
    // A setting the board does not report is one it has no choice about.
    $('name-form').hidden = !str(s.name);
    $('wiring-row').hidden = typeof s.second_line !== 'boolean';
    $('slot-row').hidden = !num(s.slot_bits);
    $('layout-row').hidden = !str(s.layout);
    row('network', network(s.network));
    if (str(s.name)) {
      $('title').textContent = s.name;
      document.title = s.name + ' - Hearth sink';
      if (document.activeElement !== $('name-input') && !$('name-input').value) $('name-input').value = s.name;
    }
    const fit = num(s.sink_slots) ? 'This sink has ' + slots(s.sink_slots) + '.' : '';
    $('slot-help').textContent = fit;
    $('layout-fit').textContent = fit && ' ' + fit;
    if (num(s.slot_bits) && fresh('slot')) radios('slot-width').forEach((r) => (r.checked = r.value === String(s.slot_bits)));
    if (typeof s.second_line === 'boolean' && fresh('wiring')) $('wiring').checked = s.second_line;
    for (const preset of radios('layout')) {
      preset.disabled = num(s.sink_slots) && need(preset.value) > s.sink_slots;
      if (fresh('layout')) preset.checked = preset.value === s.layout;
    }
    if (!filled) {
      filled = true;
      // The device keeps 95 characters of a layout's text and cuts the rest, so a text
      // that long may be part of one. Show it, but leave it out of the field, where
      // Apply would send the part as the whole layout.
      if (str(s.layout) && s.layout.length < 95 && !$('layout-input').value)
        $('layout-input').value = s.layout;
    }
  }

  function render(s, read) {
    status = s;
    const state = str(s.state) || '';
    // "flash": an update's flash mode (planning/esp32-ota.md), where nothing plays.
    const head = state === 'finished' && s.why ? 'Finished (' + s.why + ')' : state === 'flash' ? 'Flash mode' : state ? state[0].toUpperCase() + state.slice(1) : 'Unknown';
    $('state').textContent = head;
    $('state').dataset.state = state;
    if (shown && head !== shown) say(head + '.');
    shown = head;
    const why = reason(state, s);
    $('reason').textContent = why;
    $('reason').hidden = !why;
    const t = stream(s);
    row('location', s.location === '' ? 'None' : str(s.location));
    row('source', str(s.source));
    row('sink', str(s.sink) && s.sink + (num(s.sink_slots) ? ', ' + slots(s.sink_slots) : ''));
    row('codec', t.codec);
    row('channels', t.channels);
    row('objects', t.objects);
    row('output', t.output);
    row('silent', t.silent);
    row('next', str(s.layout) !== t.layout ? str(s.layout) : undefined);
    row('played', played(s.frames));
    timing(s);
    row('c-held', count(s.held));
    row('c-passes', count(s.passes));
    row('c-fetched', bytes(s.fetched_bytes));
    row('c-resync', bytes(s.resync_bytes));
    row('c-mismatches', count(s.layout_mismatches));
    sendspin(s.sendspin);
    settings(s, read);
  }

  // One request per action. No retry: a play sent again could restart it.
  // `why` explains a 409 better than the firmware's words can, when given.
  // Every action reads the status again at once. A confirmation is said as
  // soon as the answer comes, and a state that read announces (Playing.)
  // replaces it; said after that read, it would stand, since a state is
  // announced only when it changes. Nothing replaces a layout's words, so
  // with `after` they wait for that read and follow what it announces. `key`
  // names the choice the request carries, which the reads begun before its
  // answer leave alone.
  async function act(label, method, path, body, done, { why = '', after = false, key = '' } = {}) {
    settled[key] = Infinity;
    let ok = false;
    try {
      const r = await call(method, path, body);
      ok = r.status >= 200 && r.status < 300;
      if (!ok) say(label + ' refused (' + r.status + '): ' + (r.status === 409 && why ? why : r.text), true);
      else if (!after) done();
    } catch (e) {
      say(label + ': ' + e.message + '.', true);
    }
    settled[key] = reads;
    await poll();
    if (ok && after) done();
  }

  // A preset and the field both end here: the page sends every layout and
  // lets the firmware decide, counting slots only to explain a refusal.
  function setLayout(layout) {
    const n = need(layout);
    const room = status.sink_slots;
    $('layout-input').value = layout;
    act('Output layout ' + layout, 'PUT', 'layout', layout, () => say('Output layout ' + layout + ' from the next play.'),
      { why: num(room) && n > room ? 'it needs ' + n + ' slots and this sink has ' + room + '.' : '', after: true, key: 'layout' });
  }

  // POST /pairing's bodies: reset, cancel, forget, and forget with a server_id.
  const pairing = (label, body, done) => act(label, 'POST', 'pairing', body, () => say(done));

  const reveal = (on) => {
    $('pass-input').type = on ? 'text' : 'password';
    $('pass-show').setAttribute('aria-pressed', String(on));
  };

  $('name-form').addEventListener('submit', (event) => {
    event.preventDefault();
    const name = $('name-input').value.trim();
    if (!name) return say('Enter a name.', true);
    act('Name ' + name, 'PUT', 'name', name, () => say('This sink is called ' + name + '.'));
  });

  $('slot-width').addEventListener('change', (event) => {
    const bits = event.target.value;
    act(bits + '-bit slots', 'PUT', 'slot-width', bits, () => say(bits + '-bit slots from the next play.'),
      { why: 'a play is running, or this sink has one width only.', key: 'slot' });
  });

  $('wiring').addEventListener('change', () => {
    const wired = $('wiring').checked;
    act(wired ? 'A second line' : 'One line', 'PUT', 'wiring', wired ? '1' : '0',
      () => say(wired ? 'A second I2S line, from the next play.' : 'One I2S line, from the next play.'),
      { why: 'a play is running.', key: 'wiring' });
  });

  $('layouts').addEventListener('change', (event) => setLayout(event.target.value));

  $('layout-form').addEventListener('submit', (event) => {
    event.preventDefault();
    const layout = $('layout-input').value.trim();
    if (!layout) return say('Enter an output layout.', true);
    setLayout(layout);
  });

  $('pass-show').addEventListener('click', () => reveal($('pass-input').type === 'password'));

  $('network-form').addEventListener('submit', (event) => {
    event.preventDefault();
    const ssid = $('ssid-input').value.trim();
    if (!ssid) return say('Enter a network name.', true);
    act('Network ' + ssid, 'PUT', 'network', ssid + '\n' + $('pass-input').value, () => {
      $('pass-input').value = '';
      reveal(false);
      say('Stored ' + ssid + ' for the next restart.');
    });
  });

  $('ss-cancel').addEventListener('click', () => pairing('Cancel pairing', 'cancel', 'Pairing cancelled.'));

  $('ss-reset').addEventListener('click', () => pairing('Allow pairing', 'reset', 'A server may ask to pair again.'));

  // What cannot be taken back asks first: forgetting servers, and what the
  // firmware section does. The dialog keeps the last answer it closed with;
  // Escape closes it without giving one, which must not read as the last
  // time's yes.
  let then;
  function ask(title, text, yes, go) {
    then = go;
    $('ask-title').textContent = title;
    $('ask-text').textContent = text;
    $('ask-yes').textContent = yes;
    $('ask').returnValue = '';
    $('ask').showModal();
  }

  $('ask').addEventListener('close', () => $('ask').returnValue === 'yes' && then());

  $('ss-forget').addEventListener('click', () => ask('Forget every server?',
    'Each server this board has paired with has to pair again, and the board takes a new identity.', 'Forget',
    () => pairing('Forget every server', 'forget', 'Every server is forgotten: each has to pair again.')));

  // The image's head, as parse_image_head reads it (ac3forge/firmware_image.hpp):
  // its chip, version and project, or null for a file that is not an
  // application image. An upload stops what plays before the board reads a
  // byte of it, so a file the board would refuse on these alone stays here.
  async function head(file) {
    const b = new Uint8Array(await file.slice(0, 112).arrayBuffer());
    const v = new DataView(b.buffer);
    const text = (at) => new TextDecoder().decode(b.subarray(at, at + 32)).split('\0')[0];
    return b.length === 112 && b[0] === 0xe9 && v.getUint32(32, true) === 0xabcd5432
      ? { chip: v.getUint16(12, true), version: text(48), project: text(80) }
      : null;
  }

  // PUT /firmware, the one request that is not call()'s: an XMLHttpRequest
  // reports the bytes sent, which fetch cannot, and has no timeout, since the
  // board answers only once the image is written and checked.
  function upload(file, version) {
    const x = new XMLHttpRequest();
    const sent = (text, part) => {
      row('fw-upload', text);
      $('fw-sent').value = part;
    };
    sending = true;
    renderFirmware(fw);
    sent('Sent 0 of ' + bytes(file.size), 0);
    x.upload.onprogress = (e) => sent('Sent ' + count(e.loaded) + ' of ' + bytes(e.total), e.loaded / e.total);
    x.upload.onload = () => sent('Sent. The board checks the image before it answers.', 1);
    x.onloadend = () => {
      sending = false;
      restarting = x.status === 200 ? Date.now() : 0;
      say(restarting ? 'Written and checked: the board restarts into ' + version + ', on trial.'
        : x.status ? 'Update refused (' + x.status + '): ' + x.responseText.trim() : 'Update: no connection.', !restarting);
      fwDue = true;
      poll();
    };
    x.open('PUT', 'firmware');
    x.setRequestHeader('Content-Type', 'application/octet-stream');
    x.send(file);
  }

  $('fw-pick').addEventListener('click', () => $('fw-file').click());

  $('fw-file').addEventListener('change', async () => {
    const file = $('fw-file').files[0];
    $('fw-file').value = ''; // so that the same file can be chosen again
    if (!file) return;
    const h = await head(file);
    const chip = CHIPS[hardware.target];
    const why = !h ? 'it is not an application image; choose ac3forge_hearth_sink.bin'
      : chip !== undefined && h.chip !== chip ? 'it is for another chip than this ' + hardware.chip
        : hardware.project && h.project !== hardware.project ? 'it is ' + h.project + ', not ' + hardware.project : '';
    if (why) return say(file.name + ' was not sent: ' + why + '.', true);
    ask('Update to ' + h.version + '?', 'What plays stops, and the board writes ' + file.name + ' into ' + fw.other.label +
      ', then restarts into it. It keeps the new image once it has shown it works, and goes back to ' + fw.running.version +
      ' by itself if not.', 'Update', () => upload(file, h.version));
  });

  // The board restarts once it has answered: a poll that fails meanwhile says
  // so rather than report an error, and board() reads what it runs after.
  const restart = (label, method, path, text) =>
    act(label, method, path, '', () => {
      restarting = Date.now();
      say(text);
    });

  $('fw-restart').addEventListener('click', () => ask('Restart the board?',
    'What plays stops, and the board starts ' + fw.running.version + ' again.', 'Restart',
    () => restart('Restart', 'POST', 'restart', 'Restarting.')));

  $('fw-rollback').addEventListener('click', () => {
    const v = fw.other.version;
    ask('Roll back to ' + v + '?', 'What plays stops, and the board restarts into ' + v + '.', 'Roll back',
      () => restart('Roll back', 'PUT', 'firmware/rollback', 'Going back to ' + v + '.'));
  });

  // Leaving during an upload ends it, and leaves the board in flash mode.
  addEventListener('beforeunload', (e) => sending && e.preventDefault());

  $('outcome').addEventListener('click', hideToast);

  document.addEventListener('visibilitychange', () => (document.visibilityState === 'hidden' ? clearTimeout(timer) : poll()));

  poll();
})();
