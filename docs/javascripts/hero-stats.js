/* Homepage "Measured on every merge" stat strip.
   The rules that pick each figure - worst case of one codec, per-channel headroom
   not raw SNR, thresholds always borrowed from a gate that exists, never invented -
   are the ones performance-quality.md's own page-local script applies, and the text
   between the "measurement summary" markers is identical in both files (a test in
   tools/ci fails when it is not). See that page for the reasoning behind each
   judgement call. Only the *rendering* differs: compact tiles instead of full
   cards. Self-contained on purpose, matching every other trend page's own "no
   shared docs/javascripts asset" convention (see performance-trend.md's note on
   why) - this is the homepage's own copy, not a shared module the other pages
   should start depending on. */
(function () {
  "use strict";

  const mount = document.getElementById("ac3f-stats");
  if (!mount) return; // not the homepage

  // The site is served under the repository's name, so the repository is read off the address:
  // it is right before and after the repository is renamed. A local preview, and the stub
  // tools/ci/render_measurement_tiles.js runs the scripts in, have no such address.
  const REPO = typeof location !== "undefined" && location.hostname.endsWith(".github.io")
    ? location.hostname.split(".")[0] + "/" + location.pathname.split("/")[1]
    : "iainchesworthlabs/iclforge";
  const HISTORY_BRANCH = "quality-history";

  // >>> measurement summary
  // The text between these markers is the same in docs/javascripts/hero-stats.js and in the
  // script of docs/performance-quality.md, and tools/ci/test_write_measurement_badges.py fails
  // when the two differ or when the tables differ from tools/ci/write_measurement_badges.py,
  // which writes the README's badges by the same rules. Change all three together.
  //
  // Every line is ONE codec's and names the workload it comes from: the worst case among that
  // codec's workloads, except for listening quality, which is a range. The cards used to show
  // one figure each - "2.0 dB SNR", "2009 KB/frame" - and a reader took them for the project's
  // headline numbers: they were the tightest E-AC-3 check and the AC-4 5.1 encoder's allocator
  // traffic, which say nothing about each other.
  const CODECS = ["AC-3", "E-AC-3", "AC-4"];

  // The `codec` field of a gold-reference record (main.jsonl) and of a tool-comparison one.
  const QUALITY_CODECS = { "ac3": "AC-3", "eac3": "E-AC-3" };

  // config -> [codec, what the workload is]. The names in performance-main.jsonl and
  // memory-main.jsonl, which record no codec of their own. A config missing here fails
  // loudly (UnknownWorkload) rather than being filed under a guessed codec.
  const WORKLOADS = {
    "plain_51": ["AC-3", "5.1 encode"],
    "plain_51_fast_mdct": ["AC-3", "5.1 encode, fast MDCT"],
    "ac3_51_encode": ["AC-3", "5.1 encode"],
    "ac3_51_decode": ["AC-3", "5.1 decode"],
    "eac3_51_auto": ["E-AC-3", "5.1 encode, automatic tools"],
    "eac3_stereo_auto": ["E-AC-3", "stereo encode, automatic tools"],
    "eac3_51_encode": ["E-AC-3", "5.1 encode"],
    "eac3_51_decode": ["E-AC-3", "5.1 decode"],
    "ecpl_51_encode": ["E-AC-3", "5.1 enhanced-coupling encode"],
    "atmos_4obj": ["E-AC-3", "Atmos 4-object encode"],
    "atmos_4obj_fast_mdct": ["E-AC-3", "Atmos 4-object encode, fast MDCT"],
    "atmos_4obj_qmf_fast_mdct": ["E-AC-3", "Atmos 4-object encode, QMF, fast MDCT"],
    "atmos_4obj_encode": ["E-AC-3", "Atmos 4-object encode"],
    "atmos_4obj_decode": ["E-AC-3", "Atmos 4-object decode"],
    "ac4_stereo_encode": ["AC-4", "stereo encode"],
    "ac4_stereo_decode": ["AC-4", "stereo decode"],
    "ac4_51_encode": ["AC-4", "5.1 encode"],
    "ac4_51_decode": ["AC-4", "5.1 decode"],
  };

  const LAYOUTS = { "1": "mono", "2": "stereo", "6": "5.1", "8": "7.1" };
  const CHANNEL_NAMES = { L: "left", R: "right", C: "centre", LFE: "LFE", Ls: "left surround", Rs: "right surround" };

  // append_memory_history.py's LIVE_GROWTH_WARN_BYTES, which warns at this many bytes or more
  // (and fails the build at 1 MiB). Borrowed, not chosen for a card.
  const MEMORY_RETENTION_WARN_BYTES = 4 * 1024;

  class UnknownWorkload extends Error {}

  const has = (table, key) => typeof key === "string" && Object.prototype.hasOwnProperty.call(table, key);
  const isNumber = (v) => typeof v === "number";

  // Rounded half up, as Math.round does, and the same in tools/ci/write_measurement_badges.py,
  // so a figure on a card and on the badge beside it cannot differ in its last digit.
  const round0 = (x) => Math.floor(x + 0.5);
  const round1 = (x) => Math.floor(x * 10 + 0.5) / 10;
  const round2 = (x) => Math.floor(x * 100 + 0.5) / 100;
  const oneDecimal = (x) => round1(x).toFixed(1);
  const twoDecimals = (x) => round2(x).toFixed(2);
  // A figure below 1x must not round up to it: 0.96 reads "0.96", not "1".
  const formatMultiple = (times) => (times >= 1 ? String(round0(times)) : twoDecimals(times));
  const esc = (s) => String(s).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));

  function classify(config) {
    if (has(WORKLOADS, config)) return WORKLOADS[config];
    throw new UnknownWorkload(`The workload "${config}" is not classified. Add it to WORKLOADS in ` +
      "tools/ci/write_measurement_badges.py, docs/javascripts/hero-stats.js and docs/performance-quality.md.");
  }

  function qualityCodec(rec) {
    if (has(QUALITY_CODECS, rec.codec)) return QUALITY_CODECS[rec.codec];
    throw new UnknownWorkload(`The codec "${rec.codec}" is not classified. Add it to QUALITY_CODECS in ` +
      "tools/ci/write_measurement_badges.py, docs/javascripts/hero-stats.js and docs/performance-quality.md.");
  }

  // The slowest timed workload of each codec, by how many times real time it runs: the
  // frame's own budget over its time. The slowest, not the mean: "is it fast enough" is a
  // worst-case question, and an average over ten workloads would hide one of them dropping
  // below real time behind nine that did not. Against its own budget, not by raw ms/frame:
  // an AC-4 frame is 2 048 samples (42.67 ms) against A/52's 1 536 (32 ms). Encode and
  // decode workloads compete, and the description says which one won.
  function slowestPerCodec(rows) {
    const picked = {};
    for (const row of rows) {
      if (!isNumber(row.ms_per_frame) || !(row.ms_per_frame > 0)) continue;
      const [codec, words] = classify(row.config);
      const budget = row.real_time_budget_ms_per_frame || 32;
      const times = budget / row.ms_per_frame;
      if (!picked[codec] || times < picked[codec].times) {
        picked[codec] = { config: row.config, words, times, ms: row.ms_per_frame, budget, leg: row.leg };
      }
    }
    return picked;
  }

  // tightest_headroom_db arrived with per-channel floors; records written before that carry
  // only worst_db and a scalar threshold_db, and fall back to exactly the old computation
  // rather than being dropped from the comparison.
  const headroomOf = (r) => (isNumber(r.tightest_headroom_db) ? r.tightest_headroom_db : r.worst_db - r.threshold_db);

  const layoutOf = (rec) => {
    const labels = Array.isArray(rec.channel_labels) ? rec.channel_labels : rec.channels_db;
    if (!Array.isArray(labels) || labels.length === 0) return null;
    return LAYOUTS[labels.length] || `${labels.length} channels`;
  };

  // The workload of a gold-reference check: its layout, rate, and the signal when it is named.
  const checkWords = (rec) => [
    layoutOf(rec),
    isNumber(rec.bitrate_kbps) ? `${round0(rec.bitrate_kbps)} kbps` : null,
    String(rec.check).includes("transient") ? "transient" : null,
  ].filter(Boolean).join(" ");

  // What the decode is scored against. verify_gold_reference.sh's check_against_source names
  // its checks *_source: FFmpeg cannot read those streams, so the reference is the source WAV
  // the third-party encoder was given. Every other check compares this decoder with FFmpeg's.
  const checkAgainst = (rec) => (/_source(_reference)?$/.test(String(rec.check)) ? "the source WAV" : "FFmpeg's decode");

  // The tightest channel of a record, by its own SNR. The index is bounds-checked: an
  // out-of-range one reads as missing here, as it must in tools/ci/write_measurement_badges.py.
  function describeCheck(rec, headroom) {
    let value = rec.worst_db;
    let floor = rec.threshold_db;
    let channel = null;
    if (Array.isArray(rec.thresholds_db) && Array.isArray(rec.channels_db) && rec.channels_db.length > 0) {
      const i = Number.isInteger(rec.tightest_channel) && rec.tightest_channel >= 0 &&
        rec.tightest_channel < rec.channels_db.length ? rec.tightest_channel : 0;
      value = rec.channels_db[i];
      if (i < rec.thresholds_db.length) floor = rec.thresholds_db[i];
      channel = Array.isArray(rec.channel_labels) && i < rec.channel_labels.length ? rec.channel_labels[i] : `ch${i}`;
    }
    return { check: rec.check, words: checkWords(rec), against: checkAgainst(rec), value, floor, channel, headroom, leg: rec.leg };
  }

  // The tightest gold-reference check of each codec, by margin over its own floor. Each check
  // is gated per CHANNEL, so the number that matters is the smallest per-channel margin, which
  // is NOT the lowest SNR: a 58 dB centre channel 6 dB above a 52 dB floor is closer to failing
  // than a 22 dB surround 6 dB above a 16 dB floor.
  function tightestPerCodec(rows) {
    const picked = {};
    for (const rec of rows) {
      if (!isNumber(rec.worst_db) || !isNumber(rec.threshold_db)) continue;
      const codec = qualityCodec(rec);
      const headroom = headroomOf(rec);
      if (!picked[codec] || headroom < picked[codec].headroom) picked[codec] = describeCheck(rec, headroom);
    }
    return picked;
  }

  // AC-4's decode quality: its lowest SNR and lowest MOS-LQO, one row per Dolby stream scored
  // against the source it was encoded from. Only CI's pinned floors exist, none recorded per
  // row, so nothing here can be coloured, and the figures are shown without a verdict.
  function ac4Quality(rows) {
    const snr = rows.filter((r) => isNumber(r.min_snr_db));
    const mos = rows.filter((r) => isNumber(r.mos_lqo));
    if (snr.length === 0 && mos.length === 0) return null;
    const lowest = (list, key) => list.reduce((a, b) => (b[key] < a[key] ? b : a));
    const highest = (list, key) => list.reduce((a, b) => (b[key] > a[key] ? b : a));
    const lowSnr = snr.length ? lowest(snr, "min_snr_db") : null;
    const lowMos = mos.length ? lowest(mos, "mos_lqo") : null;
    return {
      streams: rows.length,
      snr: lowSnr ? lowSnr.min_snr_db : null, snrLeg: lowSnr ? lowSnr.leg : null,
      mos: lowMos ? lowMos.mos_lqo : null, mosLeg: lowMos ? lowMos.leg : null,
      mosBest: mos.length ? highest(mos, "mos_lqo").mos_lqo : null,
    };
  }

  // The workload with the largest `field` for each codec (the first one on a tie).
  function extremePerCodec(rows, field) {
    const picked = {};
    for (const row of rows) {
      if (!isNumber(row[field])) continue;
      const [codec, words] = classify(row.config);
      if (!picked[codec] || row[field] > picked[codec].value) {
        picked[codec] = { config: row.config, words, value: row[field], frames: row.frames, leg: row.leg, allocs: row.allocs_per_frame };
      }
    }
    return picked;
  }

  // Allocator traffic: bytes requested from the heap over a frame, whatever became of them.
  // A workload that allocates and frees a 100 KB scratch buffer every frame scores 100 KB and
  // holds none of it, so this is not the memory a codec occupies. That is live growth.
  const heaviestPerCodec = (rows) => extremePerCodec(rows, "bytes_per_frame");
  const mostRetainingPerCodec = (rows) => extremePerCodec(rows, "steady_live_growth");

  // The tool comparison's encoder output, scored by ViSQOL against its source: the range of
  // MOS-LQO for each codec and the rows at both ends of it.
  function listeningPerCodec(rows) {
    const picked = {};
    for (const row of rows) {
      if (!isNumber(row.mos_lqo)) continue;
      const codec = qualityCodec(row);
      const p = picked[codec] || (picked[codec] = { count: 0, low: null, high: null, legs: new Set() });
      p.count += 1;
      p.legs.add(row.leg);
      if (!p.low || row.mos_lqo < p.low.mos_lqo) p.low = row;
      if (!p.high || row.mos_lqo > p.high.mos_lqo) p.high = row;
    }
    return picked;
  }

  // "5.1, 256 kbps" from a leg named "eac3-51-256"; "music stereo, 96 kbps" from
  // "eac3-music-stereo-96". A name in another shape comes back as it is.
  function legWords(leg) {
    const parts = String(leg).split("-");
    if (parts[0] === "ac3" || parts[0] === "eac3") parts.shift();
    const rate = parts.length > 1 && /^\d+$/.test(parts[parts.length - 1]) ? `${parts.pop()} kbps` : null;
    return [parts.map((p) => (p === "51" ? "5.1" : p)).join(" "), rate].filter(Boolean).join(", ");
  }
  // <<< measurement summary

  function rawUrl(file) {
    return "https://raw.githubusercontent.com/" + REPO + "/" + HISTORY_BRANCH + "/" + file;
  }

  function parseJsonl(text) {
    return text.split("\n").filter((l) => l.trim().length > 0).map((l) => JSON.parse(l));
  }

  async function fetchHistory(stem) {
    // Append producers create the recent sidecar only after the full history
    // exceeds the shared window; the full JSONL remains the normal fallback.
    const candidates = [stem + ".recent.jsonl", stem + ".jsonl"];
    for (const file of candidates) {
      try {
        const resp = await fetch(rawUrl(file));
        if (!resp.ok) continue;
        return parseJsonl(await resp.text());
      } catch (e) { /* fall through to the next candidate */ }
    }
    return [];
  }

  function newestCommitRows(records) {
    if (records.length === 0) return { rows: [], commit: null, date: null };
    const commit = records[records.length - 1].commit;
    const rows = records.filter((r) => r.commit === commit);
    return { rows: rows, commit: commit, date: rows[0] ? rows[0].commit_date : null };
  }

  // One tile per measure, one line per codec: the codec, its figure, and the workload the
  // figure comes from. A codec with no rows says so on its own line instead of vanishing.
  function line(codec, figure, unit, what, watch) {
    return '<div class="ac3f-stat-line"><span class="ac3f-stat-codec">' + codec + "</span>" +
      '<span class="ac3f-stat-figure' + (watch ? " ac3f-stat-figure-watch" : "") + '">' + figure +
      '<span class="ac3f-stat-unit">' + (unit ? " " + unit : "") + "</span></span>" +
      '<span class="ac3f-stat-what">' + what + "</span></div>";
  }

  function tile(title, sub, lines, note, chip) {
    const badge = chip ? '<span class="ac3f-stat-badge ' + chip.cls + '">' + chip.label + "</span>" : "";
    return '<div class="ac3f-stat-card"><div class="ac3f-stat-head">' + title + badge + "</div>" +
      (sub ? '<div class="ac3f-stat-sub">' + sub + "</div>" : "") + lines.join("") +
      '<div class="ac3f-stat-note">' + note + "</div></div>";
  }

  // A chip may be null, meaning NO chip - not an unknown one. A tile with a real figure on it
  // must never be labelled "no data" just because nothing gates that figure.
  const OK = { cls: "ac3f-stat-ok", label: "ok" };
  const WATCH = { cls: "ac3f-stat-watch", label: "watch" };
  const NONE = { cls: "ac3f-stat-unknown", label: "no data" };

  const NO_MEASUREMENT = "No measurement recorded.";
  const emptyTile = (title) => tile(title, "", [], NO_MEASUREMENT, NONE);
  const noLine = (codec, what) => line(codec, "&mdash;", "", what || NO_MEASUREMENT);

  function speedTile(rows) {
    const picked = slowestPerCodec(rows);
    if (Object.keys(picked).length === 0) return emptyTile("Speed");
    const timed = rows.filter((r) => isNumber(r.ms_per_frame) && r.ms_per_frame > 0);
    const lines = CODECS.map((codec) => {
      const e = picked[codec];
      return e ? line(codec, formatMultiple(e.times) + "&times;", "",
        esc(e.words) + ", " + oneDecimal(e.ms) + " ms of a " + round1(e.budget) + " ms frame") : noLine(codec);
    });
    // Real time is 1x by definition: below it the codec cannot keep up with playback. How
    // much headroom above it is "enough" is a judgement no threshold here should make.
    const ok = Object.values(picked).every((e) => e.times >= 1);
    return tile("Speed", "Times real time, slowest workload of each codec.", lines,
      "Slowest of " + new Set(timed.map((r) => r.config)).size + " workloads on " +
      new Set(timed.map((r) => r.leg)).size + " CI legs. Decodes count as well as encodes. " +
      "1&times; is live playback.", ok ? OK : WATCH);
  }

  function qualityTile(rows, ac4Rows) {
    const gated = tightestPerCodec(rows);
    const ac4 = ac4Quality(ac4Rows);
    if (Object.keys(gated).length === 0 && !ac4) return emptyTile("Decode accuracy");
    const lines = CODECS.map((codec) => {
      const g = gated[codec];
      if (g) {
        const channel = g.channel ? ", " + esc(has(CHANNEL_NAMES, g.channel) ? CHANNEL_NAMES[g.channel] : g.channel) + " channel" : "";
        return line(codec, oneDecimal(g.value), "dB", esc(g.words) + channel + ". Floor " +
          oneDecimal(g.floor) + " dB, scored against " + g.against + ".", g.headroom < 0);
      }
      if (codec === "AC-4" && ac4 && ac4.snr !== null) {
        return line(codec, oneDecimal(ac4.snr), "dB", "Lowest of " + ac4.streams + " Dolby streams (<code>" +
          esc(ac4.snrLeg) + "</code>), scored against their sources. No floor yet." +
          (ac4.mos !== null ? " MOS-LQO " + twoDecimals(ac4.mos) + "&ndash;" + twoDecimals(ac4.mosBest) + "." : ""));
      }
      return noLine(codec);
    });
    // Coloured on the margin over each channel's own floor. AC-4 has none and takes no part.
    const chip = Object.keys(gated).length === 0 ? null
      : Object.values(gated).every((g) => g.headroom >= 0) ? OK : WATCH;
    return tile("Decode accuracy", "Tightest check of each codec: decoded audio against a reference, channel by channel.",
      lines, "The colour is the margin over each channel's own floor. SNR measures agreement with a " +
      "reference and says nothing about perceived audio quality.", chip);
  }

  function listeningTile(rows) {
    const picked = listeningPerCodec(rows);
    if (Object.keys(picked).length === 0) return emptyTile("Listening quality");
    const lines = CODECS.map((codec) => {
      const p = picked[codec];
      if (!p) return noLine(codec, codec === "AC-4" ? "No series yet. Its decoder's MOS-LQO is under Decode accuracy." : null);
      const range = p.low.mos_lqo === p.high.mos_lqo ? twoDecimals(p.low.mos_lqo)
        : twoDecimals(p.low.mos_lqo) + "&ndash;" + twoDecimals(p.high.mos_lqo);
      return line(codec, range, "MOS-LQO", p.count + " measurements on " + p.legs.size + " legs. Lowest: " +
        esc(legWords(p.low.leg)) + (p.low.variant ? ", tool set " + esc(p.low.variant) : "") + ".");
    });
    // NO chip, deliberately: a low-bitrate leg is SUPPOSED to score lower, so any absolute floor
    // would flag the encoder for doing its job. A MOS change is judged on the trend pages,
    // against that leg's own past.
    return tile("Listening quality", "Encoder output as ViSQOL predicts a listener would rate it, 1 to 5.",
      lines, "5 is indistinguishable from the original. A lower bitrate scores lower by design.", null);
  }

  function allocationTile(rows) {
    const picked = heaviestPerCodec(rows);
    if (Object.keys(picked).length === 0) return emptyTile("Allocation per frame");
    const lines = CODECS.map((codec) => {
      const e = picked[codec];
      return e ? line(codec, round0(e.value / 1024), "KB", esc(e.words) +
        (isNumber(e.allocs) ? ", " + round0(e.allocs) + " allocations" : "") + ".") : noLine(codec);
    });
    // NO chip: nothing gates allocator traffic, so there is no line to borrow.
    return tile("Allocation per frame", "Heaviest workload of each codec. Bytes requested from the heap per frame.",
      lines, "This counts allocator traffic, not memory in use. What stays in memory is live growth.", null);
  }

  function growthTile(rows) {
    const picked = mostRetainingPerCodec(rows);
    if (Object.keys(picked).length === 0) return emptyTile("Live growth");
    const windows = [...new Set(Object.values(picked).filter((e) => isNumber(e.frames)).map((e) => e.frames - 1))];
    const frames = windows.length === 1 ? String(windows[0]) : "about 200";
    const lines = CODECS.map((codec) => {
      const e = picked[codec];
      const over = e && e.value >= MEMORY_RETENTION_WARN_BYTES;
      return e ? line(codec, oneDecimal(e.value / 1024), "KiB", esc(e.words) + (over ? ", over the line." : "."), over)
        : noLine(codec);
    });
    // Live bytes still held after the steady-state frames is a leak signal, and a leak is a
    // leak whatever last week's runs did: append_memory_history.py warns at 4 KiB and fails
    // the build at 1 MiB. A couple of KiB of retained working set is expected.
    const held = Object.values(picked).every((e) => e.value < MEMORY_RETENTION_WARN_BYTES);
    return tile("Live growth", "Bytes still held after " + frames + " steady-state frames, worst workload of each codec.",
      lines, "The warning line is 4 KiB, the one append_memory_history.py warns at.", held ? OK : WATCH);
  }

  // A workload no table lists is a maintainer's problem, not a reader's: say so in the tile
  // it would have gone in rather than leaving the strip on "Loading".
  function guarded(title, build) {
    try {
      return build();
    } catch (e) {
      if (!(e instanceof UnknownWorkload)) throw e;
      return tile(title, "", [], esc(e.message), NONE);
    }
  }

  (async function render() {
    let perf, quality, ac4, external, memory;
    try {
      [perf, quality, ac4, external, memory] = await Promise.all([
        fetchHistory("performance-main"),
        fetchHistory("main"),
        fetchHistory("ac4-quality-main"),
        fetchHistory("external-comparison-main"),
        fetchHistory("memory-main"),
      ]);
    } catch (e) {
      perf = quality = ac4 = external = memory = [];
    }

    if (perf.length === 0 && quality.length === 0 && ac4.length === 0 && external.length === 0 && memory.length === 0) {
      mount.innerHTML = '<p class="ac3f-stat-status">Could not reach the measurement history just now — ' +
        '<a href="performance-quality/">the full page</a> fetches the same way, so this is a network or availability problem, not a missing measurement.</p>';
      return;
    }

    const p = newestCommitRows(perf), q = newestCommitRows(quality), a = newestCommitRows(ac4);
    const e = newestCommitRows(external), m = newestCommitRows(memory);
    mount.innerHTML = [
      guarded("Speed", () => speedTile(p.rows)),
      guarded("Decode accuracy", () => qualityTile(q.rows, a.rows)),
      guarded("Listening quality", () => listeningTile(e.rows)),
      guarded("Allocation per frame", () => allocationTile(m.rows)),
      guarded("Live growth", () => growthTile(m.rows)),
    ].join("");
  })();
})();
