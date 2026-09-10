// The player's web page. planning/esp32-device-ui.md is its design.
(() => {
  'use strict';
  const POLL_MS = 1000; // while the page is visible
  const RETRY_MS = 5000; // while GET /status fails
  const TIMEOUT_MS = 4000; // for any one request
  const VOLUME_GAP_MS = 200; // POST /volume's queue: four deep, emptied every 100 ms
  const REFUSAL_MS = 6000; // an accepted location not in /status by then was refused
  const FRAME_US = 32000; // 1,536 samples at 48 kHz
  const ACMOD = ['1+1', '1/0', '2/0', '3/0', '2/1', '3/1', '2/2', '3/2']; // A/52 Table 5.8

  const $ = (id) => document.getElementById(id);
  const num = (v) => typeof v === 'number' && Number.isFinite(v);
  const str = (v) => (typeof v === 'string' ? v : undefined);
  const count = (v) => (num(v) ? v.toLocaleString('en-US') : undefined);
  const bytes = (v) => (num(v) ? count(v) + ' bytes' : undefined);
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
  let again = false;
  let failing = 0;
  let shown = '';
  let filled = false;
  let watch = null;
  let volWanted = null;
  let volBusy = false;
  let volSent = -Infinity;
  let volTimer = 0;

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

  async function poll() {
    if (polling) {
      again = true;
      return;
    }
    polling = true;
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
    } catch (e) {
      delay = RETRY_MS;
      if (!failing) say('No status: ' + e.message + '.', true);
      failing ||= Date.now();
      put('link', 'No status since ' + clock(failing) + ': ' + e.message + '.', true);
    }
    polling = false;
    schedule(again ? 0 : delay);
    again = false;
  }

  // "failed" with no failed run behind it: the source did not open.
  const reason = (state, s) =>
    s.failed === true
      ? 'Stopped by a ' + (str(s.why) || 'player') + ' error' + (num(s.error) ? ' (' + s.error + ')' : '') + '.'
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
      channels: num(t.channels) ? t.channels + (ACMOD[t.acmod] ? ' (' + ACMOD[t.acmod] + ')' : '') : undefined,
      objects: t.objects === true ? (t.objects_rendered === true ? 'Carried, placed onto the layout' : 'Carried, not placed') : t.objects === false ? 'None' : undefined,
      slots: count(t.slots),
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
    row('sink', str(s.sink));
    row('codec', t.codec);
    row('channels', t.channels);
    row('objects', t.objects);
    row('layout', str(s.layout));
    row('slots', t.slots);
    row('played', played(s.frames));
    timing(s);
    row('c-held', count(s.held));
    row('c-passes', count(s.passes));
    row('c-fetched', bytes(s.fetched_bytes));
    row('c-resync', bytes(s.resync_bytes));
    row('c-mismatches', count(s.layout_mismatches));
    if (num(s.volume) && volWanted === null && !volBusy) volume(String(Math.round(s.volume * 100)));
    if (!filled) {
      filled = true;
      if (str(s.location) && !$('location-input').value) $('location-input').value = s.location;
      if (str(s.layout) && !$('layout-input').value) $('layout-input').value = s.layout;
    }
    if (watch && s.location === watch.location) watch = null;
    if (watch && Date.now() >= watch.until) {
      say('The player did not take ' + watch.location + '; it is still on ' + (str(s.location) || 'nothing') + '.', true);
      watch = null;
    }
  }

  // One request per action. No retry: a play sent again could restart it.
  async function act(label, method, path, body, done) {
    try {
      const r = await call(method, path, body);
      if (r.status >= 200 && r.status < 300) done();
      else say(label + ' refused (' + r.status + '): ' + r.text, true);
    } catch (e) {
      say(label + ': ' + e.message + '.', true);
    }
    poll();
  }

  $('play-form').addEventListener('submit', (event) => {
    event.preventDefault();
    const location = $('location-input').value.trim();
    if (!location) return say('Enter a location to play.', true);
    const before = status.location;
    act('Play', 'POST', 'play', location, () => {
      say('Asked the player to play ' + location + '.');
      watch = location === before ? null : { location, until: Date.now() + REFUSAL_MS };
    });
  });

  $('stop').addEventListener('click', () => act('Stop', 'POST', 'stop', undefined, () => say('Asked the player to stop.')));

  $('layout-form').addEventListener('submit', (event) => {
    event.preventDefault();
    const layout = $('layout-input').value.trim();
    if (!layout) return say('Enter a layout.', true);
    act('Layout ' + layout, 'PUT', 'layout', layout, () => say('Layout ' + layout + ' from the next play.'));
  });

  // One volume request in flight at most; /status leaves the slider alone.
  function volume(percent) {
    $('volume').value = percent;
    $('volume').setAttribute('aria-valuetext', percent + '%');
    $('volume-out').textContent = percent + '%';
  }

  $('volume').addEventListener('input', () => {
    volume($('volume').value);
    volWanted = (Number($('volume').value) / 100).toFixed(2);
    sendVolume();
  });

  function sendVolume() {
    clearTimeout(volTimer);
    if (volBusy || volWanted === null) return;
    const wait = volSent + VOLUME_GAP_MS - Date.now();
    if (wait > 0) {
      volTimer = setTimeout(sendVolume, wait);
      return;
    }
    const value = volWanted;
    const label = 'Volume ' + Math.round(value * 100) + '%';
    volWanted = null;
    volBusy = true;
    volSent = Date.now();
    call('POST', 'volume', value)
      .then(
        (r) => r.status === 200 || say(label + ' refused (' + r.status + '): ' + r.text, true),
        (e) => say(label + ': ' + e.message + '.', true),
      )
      .then(() => {
        volBusy = false;
        if (volWanted === null) poll();
        else sendVolume();
      });
  }

  document.addEventListener('visibilitychange', () => (document.visibilityState === 'hidden' ? clearTimeout(timer) : poll()));

  poll();
})();
