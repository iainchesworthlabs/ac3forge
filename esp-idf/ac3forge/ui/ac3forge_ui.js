// The player's web page. planning/esp32-device-ui.md is its design.
(() => {
  'use strict';
  const POLL_MS = 1000; // while the page is visible
  const RETRY_MS = 5000; // while GET /status fails
  const TIMEOUT_MS = 4000; // for any one request
  const FRAME_US = 32000; // 1,536 samples at 48 kHz
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
  // firmware decides; this only explains a refusal and filters suggestions.
  const need = (text) => {
    const m = /^([1234579])\.([012])(?:\.([0246]))?$/.exec(text);
    return m ? +m[1] + +m[2] + +(m[3] || 0) : text.includes(',') ? text.split(',').length : undefined;
  };
  const ms = (us) => (us / 1000).toFixed(1) + ' ms';
  const clock = (t) => new Date(t).toTimeString().slice(0, 8);
  const put = (id, text, bad) => {
    $(id).textContent = text;
    $(id).classList.toggle('error', !!bad);
  };
  // The one live region: what an action did, and the state when it changes.
  const say = (text, bad) => put('outcome', text, bad);
  // A description-list row, hidden when /status does not carry it.
  const row = (id, value) => {
    const dd = $(id);
    dd.hidden = dd.previousElementSibling.hidden = value === undefined;
    dd.textContent = value ?? '';
  };

  let status = {};
  let timer = 0;
  let polling = false;
  let waiting = [];
  let failing = 0;
  let shown = '';
  let filled = false;
  let code = '';
  let hardwareShown = false;

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
      if (failing) say('The player is answering again.');
      failing = 0;
      put('link', 'Status read at ' + clock(Date.now()) + '.');
      render(s);
      if (!hardwareShown) loadHardware();
    } catch (e) {
      delay = RETRY_MS;
      if (!failing) say('No status: ' + e.message + '.', true);
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

  function played(frames) {
    if (!num(frames)) return undefined;
    const sec = Math.floor((frames * FRAME_US) / 1e6);
    return Math.floor(sec / 60) + ':' + String(sec % 60).padStart(2, '0') + ' (' + count(frames) + (frames === 1 ? ' frame)' : ' frames)');
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

  const db = (v) => (num(v) ? (v <= -120 ? 'Silent' : v.toFixed(1) + ' dB') : '');

  // A Sendspin player's part of /status: who plays to it, how well, and a
  // pairing in progress, whose code is announced once when it appears.
  function sendspin(p) {
    const on = !!p && typeof p === 'object';
    $('sendspin').hidden = !on;
    if (!on) return;
    const c = str(p.pairing_code) || '';
    const spaced = c.replace(/(\d{3})(?=\d)/g, '$1 ');
    if (c && c !== code) say('Pairing code ' + spaced + ': enter it where the server asks for it.');
    code = c;
    put('ss-code', c ? 'Pairing code ' + spaced : '');
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
    $('ss-levels').hidden = !peaks.length;
    $('ss-rows').replaceChildren(...peaks.map((peak, i) => {
      const tr = document.createElement('tr');
      const th = document.createElement('th');
      th.scope = 'row';
      th.textContent = String(i + 1);
      tr.append(th);
      tr.insertCell().textContent = db(peak);
      tr.insertCell().textContent = db((p.rms_db || [])[i]);
      return tr;
    }));
    $('ss-cancel').hidden = !c;
    $('ss-reset').hidden = p.pairing_held !== true;
  }

  // What this board is: fetched once, since nothing in it changes while the
  // board runs (unlike everything else this page polls). Tried again at the
  // next successful status poll if it failed the first time - a board whose
  // network comes up slowly should still end up showing this.
  function renderHardware(hw) {
    $('hw-note').hidden = true;
    row('hw-chip', str(hw.chip) && hw.chip + (str(hw.revision) ? ', revision ' + hw.revision : ''));
    row('hw-cores', num(hw.cores) ? String(hw.cores) : undefined);
    row('hw-arithmetic', hw.fpu === true ? 'Hardware floating point' : hw.fpu === false ? 'Fixed-point (no floating-point unit)' : undefined);
    row('hw-psram', mib(hw.psram_bytes));
    row('hw-sink', num(hw.sink_max_slots) ? slots(hw.sink_max_slots) : undefined);
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
      hardwareShown = true;
    } catch {
      // Left as "Reading what this board is."; tried again at the next
      // successful status poll, the same board this page is already reading.
    }
  }

  function render(s) {
    status = s;
    const state = str(s.state) || '';
    const head = state === 'finished' && s.why ? 'Finished (' + s.why + ')' : state ? state[0].toUpperCase() + state.slice(1) : 'Unknown';
    $('state').textContent = head;
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
    // The suggestions the sink can carry, and its size beside the field.
    $('layout-fit').textContent = num(s.sink_slots) ? ' This sink has ' + slots(s.sink_slots) + '.' : '';
    for (const option of $('layouts').options) option.disabled = num(s.sink_slots) && need(option.value) > s.sink_slots;
    timing(s);
    row('c-held', count(s.held));
    row('c-passes', count(s.passes));
    row('c-fetched', bytes(s.fetched_bytes));
    row('c-resync', bytes(s.resync_bytes));
    row('c-mismatches', count(s.layout_mismatches));
    sendspin(s.sendspin);
    // The settings, filled in from what the board says rather than from what
    // this page last sent - and never under someone who is typing in them.
    if (str(s.name)) {
      $('title').textContent = s.name;
      document.title = s.name + ' - Hearth sink';
      if (document.activeElement !== $('name-input') && !$('name-input').value) $('name-input').value = s.name;
    }
    if (num(s.slot_bits) && document.activeElement !== $('slot-width')) $('slot-width').value = String(s.slot_bits);
    $('slot-help').textContent = num(s.sink_slots) ? 'This sink has ' + slots(s.sink_slots) + '.' : '';
    if (typeof s.second_line === 'boolean' && document.activeElement !== $('wiring')) $('wiring').checked = s.second_line;
    if (!filled) {
      filled = true;
      // The device keeps 95 characters of a layout's text and cuts the rest, so a text
      // that long may be part of one. Show it, but leave it out of the field, where
      // Apply would send the part as the whole layout.
      if (str(s.layout) && s.layout.length < 95 && !$('layout-input').value)
        $('layout-input').value = s.layout;
    }
  }

  // One request per action. No retry: a play sent again could restart it.
  // `why` explains a 409 better than the firmware's words can, when given.
  // Play and Stop say what they asked at once, and the state the poll that
  // follows announces (Playing.) replaces it; said after that poll, their
  // words would stand, as a state is announced only when it changes. Nothing
  // replaces a layout's words, so with `after` they wait for that poll and
  // follow what it announces.
  async function act(label, method, path, body, done, why, after) {
    try {
      const r = await call(method, path, body);
      if (r.status >= 200 && r.status < 300) {
        if (after) {
          await poll();
          done();
          return;
        }
        done();
      } else {
        say(label + ' refused (' + r.status + '): ' + (r.status === 409 && why ? why : r.text), true);
      }
    } catch (e) {
      say(label + ': ' + e.message + '.', true);
    }
    poll();
  }

  $('name-form').addEventListener('submit', (event) => {
    event.preventDefault();
    const name = $('name-input').value.trim();
    if (!name) return say('Enter a name.', true);
    act('Name ' + name, 'PUT', 'name', name, () => say('This sink is called ' + name + '.'));
  });

  $('slot-width').addEventListener('change', () => {
    const bits = $('slot-width').value;
    act(bits + '-bit slots', 'PUT', 'slot-width', bits,
      () => say(bits + '-bit slots from the next play.'), 'a play is running, or this sink has one width only.');
  });

  $('wiring').addEventListener('change', () => {
    const wired = $('wiring').checked;
    act(wired ? 'A second line' : 'One line', 'PUT', 'wiring', wired ? '1' : '0',
      () => say(wired ? 'A second I2S line, from the next play.' : 'One I2S line, from the next play.'),
      'a play is running.');
  });

  $('network-form').addEventListener('submit', (event) => {
    event.preventDefault();
    const ssid = $('ssid-input').value.trim();
    if (!ssid) return say('Enter a network name.', true);
    act('Network ' + ssid, 'PUT', 'network', ssid + '\n' + $('pass-input').value, () => {
      $('pass-input').value = '';
      say('Stored ' + ssid + ' for the next restart.');
    });
  });

  $('layout-form').addEventListener('submit', (event) => {
    event.preventDefault();
    const layout = $('layout-input').value.trim();
    if (!layout) return say('Enter an output layout.', true);
    const n = need(layout);
    const room = status.sink_slots;
    act('Output layout ' + layout, 'PUT', 'layout', layout, () => say('Output layout ' + layout + ' from the next play.'),
      num(room) && n > room ? 'it needs ' + n + ' slots and this sink has ' + room + '.' : '', true);
  });

  $('ss-cancel').addEventListener('click', () =>
    act('Cancel pairing', 'POST', 'pairing', 'cancel', () => say('Pairing cancelled.')));

  $('ss-reset').addEventListener('click', () =>
    act('Allow pairing', 'POST', 'pairing', 'reset', () => say('A server may ask to pair again.')));

  $('ss-forget').addEventListener('click', () => {
    if (!confirm('Forget every server this board has paired with? Each has to pair again.')) return;
    act('Forget every server', 'POST', 'pairing', 'forget', () => say('Every server is forgotten: each has to pair again.'));
  });

  document.addEventListener('visibilitychange', () => (document.visibilityState === 'hidden' ? clearTimeout(timer) : poll()));

  poll();
})();
