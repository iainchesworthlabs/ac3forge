/* Homepage "Measured on every merge" stat strip.
   Data-fetch and badge logic copied verbatim from performance-quality.md's
   own page-local script, not reimplemented - see that page for the full
   reasoning behind each judgement call (worst-case not average, per-channel
   headroom not raw SNR, badge thresholds always borrowed from a real gate,
   never invented). Only the *rendering* differs: compact tiles instead of
   full cards, no long-form note. Self-contained on purpose, matching every
   other trend page's own "no shared docs/javascripts asset" convention
   (see performance-trend.md's note on why) - this is the homepage's own
   copy, not a shared module the other pages should start depending on. */
(function () {
  "use strict";

  var mount = document.getElementById("ac3f-stats");
  if (!mount) return; // not the homepage

  var REPO = "iainchesworthlabs/ac3forge";
  var HISTORY_BRANCH = "quality-history";
  var MEMORY_RETENTION_WARN_BYTES = 4 * 1024;

  function rawUrl(file) {
    return "https://raw.githubusercontent.com/" + REPO + "/" + HISTORY_BRANCH + "/" + file;
  }

  function parseJsonl(text) {
    return text.split("\n").filter(function (l) { return l.trim().length > 0; }).map(function (l) { return JSON.parse(l); });
  }

  async function fetchHistory(stem) {
    var candidates = [stem + ".recent.jsonl", stem + ".jsonl"];
    for (var i = 0; i < candidates.length; i++) {
      try {
        var resp = await fetch(rawUrl(candidates[i]));
        if (!resp.ok) continue;
        return parseJsonl(await resp.text());
      } catch (e) { /* fall through to the next candidate */ }
    }
    return [];
  }

  function newestCommitRows(records) {
    if (records.length === 0) return { rows: [], commit: null, date: null };
    var commit = records[records.length - 1].commit;
    var rows = records.filter(function (r) { return r.commit === commit; });
    return { rows: rows, commit: commit, date: rows[0] ? rows[0].commit_date : null };
  }

  function tile(title, value, unit, note, badge) {
    var chip = badge ? '<span class="ac3f-stat-badge ' + badge.cls + '">' + badge.label + "</span>" : "";
    return '<div class="ac3f-stat-card"><div class="ac3f-stat-head">' + title + chip + "</div>" +
      '<div class="ac3f-stat-value">' + value + '<span class="ac3f-stat-unit">' + (unit ? " " + unit : "") + "</span></div>" +
      '<div class="ac3f-stat-note">' + note + "</div></div>";
  }

  var OK = { cls: "ac3f-stat-ok", label: "ok" };
  var WATCH = { cls: "ac3f-stat-watch", label: "watch" };
  var NONE = { cls: "ac3f-stat-unknown", label: "no data" };

  function speedTile(rows) {
    var timed = rows.filter(function (r) { return typeof r.ms_per_frame === "number" && r.ms_per_frame > 0; });
    if (timed.length === 0) return tile("Encode speed", "&mdash;", "", "No measurement recorded.", NONE);
    var worst = timed.reduce(function (a, b) { return a.ms_per_frame > b.ms_per_frame ? a : b; });
    var budget = worst.real_time_budget_ms_per_frame || 32;
    var times = budget / worst.ms_per_frame;
    return tile("Encode speed", times.toFixed(0) + "&times;", "real time",
      "Worst of " + timed.length + " workloads.", times >= 1 ? OK : WATCH);
  }

  function qualityTile(rows) {
    var scored = rows.filter(function (r) { return typeof r.worst_db === "number" && typeof r.threshold_db === "number"; });
    if (scored.length === 0) return tile("Decode accuracy", "&mdash;", "", "No measurement recorded.", NONE);
    function headroomOf(r) {
      return typeof r.tightest_headroom_db === "number" ? r.tightest_headroom_db : r.worst_db - r.threshold_db;
    }
    var tight = scored.reduce(function (a, b) { return headroomOf(a) < headroomOf(b) ? a : b; });
    var headroom = headroomOf(tight);
    var thresholds = Array.isArray(tight.thresholds_db) ? tight.thresholds_db : null;
    var value = thresholds
      ? (tight.channels_db ? tight.channels_db[typeof tight.tightest_channel === "number" ? tight.tightest_channel : 0] : tight.worst_db).toFixed(1)
      : tight.worst_db.toFixed(1);
    return tile("Decode accuracy", value, "dB SNR",
      "Tightest of " + scored.length + " channel checks.", headroom >= 0 ? OK : WATCH);
  }

  function listeningTile(rows) {
    var scored = rows.filter(function (r) { return typeof r.mos_lqo === "number"; });
    if (scored.length === 0) return tile("Listening quality", "&mdash;", "", "No measurement recorded.", NONE);
    var values = scored.map(function (r) { return r.mos_lqo; });
    var best = Math.max.apply(null, values);
    var worstRow = scored.reduce(function (a, b) { return a.mos_lqo < b.mos_lqo ? a : b; });
    return tile("Listening quality", worstRow.mos_lqo.toFixed(2) + "&ndash;" + best.toFixed(2), "MOS-LQO",
      scored.length + " measurements, " + new Set(scored.map(function (r) { return r.leg; })).size + " legs.", null);
  }

  function memoryTile(rows) {
    var scored = rows.filter(function (r) { return typeof r.bytes_per_frame === "number"; });
    if (scored.length === 0) return tile("Memory per frame", "&mdash;", "", "No measurement recorded.", NONE);
    var worst = scored.reduce(function (a, b) { return a.bytes_per_frame > b.bytes_per_frame ? a : b; });
    var retained = scored.reduce(function (a, b) { return (a.steady_live_growth || 0) > (b.steady_live_growth || 0) ? a : b; });
    var held = retained.steady_live_growth || 0;
    return tile("Memory per frame", (worst.bytes_per_frame / 1024).toFixed(0), "KB",
      "Heaviest of " + scored.length + " workloads.", held <= MEMORY_RETENTION_WARN_BYTES ? OK : WATCH);
  }

  (async function render() {
    var perf, quality, external, memory;
    try {
      var results = await Promise.all([
        fetchHistory("performance-main"),
        fetchHistory("main"),
        fetchHistory("external-comparison-main"),
        fetchHistory("memory-main"),
      ]);
      perf = results[0]; quality = results[1]; external = results[2]; memory = results[3];
    } catch (e) {
      perf = quality = external = memory = [];
    }

    if (perf.length === 0 && quality.length === 0 && external.length === 0 && memory.length === 0) {
      mount.innerHTML = '<p class="ac3f-stat-status">Could not reach the measurement history just now — ' +
        '<a href="performance-quality/">the full page</a> fetches the same way, so this is a network or availability problem, not a missing measurement.</p>';
      return;
    }

    var p = newestCommitRows(perf), q = newestCommitRows(quality), e = newestCommitRows(external), m = newestCommitRows(memory);
    mount.innerHTML = speedTile(p.rows) + qualityTile(q.rows) + listeningTile(e.rows) + memoryTile(m.rows);
  })();
})();
