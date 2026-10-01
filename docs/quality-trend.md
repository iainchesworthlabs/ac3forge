# Quality trend

This page has two series. The first, from here to "Where the data lives", is the
[gold-reference
gate](https://github.com/iainchesworthlabs/iclforge/blob/main/tools/checks/verify_gold_reference.sh)'s
(encode the checked-in golden 5.1 WAV, strict-decode with FFmpeg and with
`forge`'s own decoder, delay-compensated SNR between the two), which covers
AC-3 and E-AC-3, and its measure is Decode accuracy: how closely this decoder agrees with
FFmpeg's. [AC-4 decode quality](#ac-4-decode-quality) is the second, scored
against the source instead.

Each run on `main` that gets through the gold-reference gate, the run after a merge
and the nightly run, has its per-channel numbers appended to history
instead of only living in that run's CI log, under the commit it ran on. A run after a
merge covers every merge since the last one finished when they come faster than it does
([CI for many agents](ci-agentic.md#after-the-merge)), so a commit in the
middle of a burst has no point of its own. It turns the gate's own FFmpeg-oracle SNR check into a
lightweight, trended quality signal — the gate itself runs on every pull request and every
queue entry; what's below is what makes the *numbers*, not just the pass/fail,
outlive the run that produced them.

The gate's own thresholds are fixed floors that always fail CI outright, and
there is now one **per channel** rather than one per fixture — each derived
from that channel's own lowest measurement across every leg and every recorded
commit (`tools/checks/derive_channel_floors.py`). `MIN_SNR_DB` in
`verify_gold_reference.sh` remains the scalar fallback for any check with no
derived vector. [Validation](verification.md#one-floor-per-channel-not-one-per-file)
carries the derivation and the reason: one floor across six channels turned out
to be one gate on the worst channel and 30-70 dB of dead slack on the rest.
On top of that, the append step applies two trailing-baseline checks against
each run's own leg/codec history: a soft one (0.5 dB below the trailing
10-run mean) that only annotates a row below, never fails anything, and a
hard one (10 dB below that mean) that *does* fail the run — after the
numbers are still recorded here, so a big regression is never silently
un-recorded just because it also failed. That failure is the `Publish quality
trend` job in `_build.yml`, surfaced through `ci.yml`'s `build-and-test` call;
it is not one of `_ci-core.yml`'s trend jobs. See `REGRESSION_DROP_DB` and
`HARD_REGRESSION_DROP_DB` in `tools/ci/append_quality_history.py`.

Every point the chart plots is measured against `tests/golden/audio/reference_51.wav`,
which is **synthesized** — `sin()`, pseudo-random noise and FIR smoothing,
2.5 s long (the table also lists checks on other fixtures, marked below). That is the right choice for this page, which asks "did the
round trip change" and needs the material to be identical across years of
commits for the answer to mean anything. It is the wrong material for
deciding an encoder policy: it carries a flat noise plateau across its whole
top octave that no real programme material has, and tuning the encoder's
bandwidth default against it once produced a measured 2.1 dB "win" that was
purely an artefact of the fixture. Real speech and music fixtures exist for
that question — see [Landscape](landscape.md) and
[tools/generators/README.md](https://github.com/iainchesworthlabs/iclforge/blob/main/tools/generators/README.md).

<div id="quality-trend-app">
  <p class="quality-trend-status">Loading trend data…</p>
</div>

<style>
#quality-trend-app, #ac4-quality-trend-app { margin: 1.5em 0; }
.quality-trend-status { color: var(--md-default-fg-color--light); font-style: italic; }
.quality-trend-controls { display: flex; gap: 1.25em; align-items: center; margin-bottom: 0.75em; flex-wrap: wrap; }
.quality-trend-controls label { font-size: 0.85em; color: var(--md-default-fg-color--light); display: inline-flex; align-items: center; gap: 0.35em; white-space: nowrap; }
.quality-trend-controls select {
  font: inherit; padding: 0.2em 0.5em; border-radius: 0.2em;
  border: 1px solid var(--md-default-fg-color--lightest);
  background: var(--md-default-bg-color); color: var(--md-default-fg-color);
}
.quality-trend-chart-wrap { overflow-x: auto; margin-bottom: 1em; }
.quality-trend-chart { display: block; }
.quality-trend-legend { display: flex; gap: 1.5em; font-size: 0.85em; margin: 0.5em 0 1em; flex-wrap: wrap; }
.quality-trend-legend span { display: inline-flex; align-items: center; gap: 0.4em; }
.quality-trend-legend i { width: 0.9em; height: 0.9em; border-radius: 50%; display: inline-block; }
.quality-trend-table-wrap { overflow-x: auto; }
#quality-trend-app table, #ac4-quality-trend-app table { width: 100%; border-collapse: collapse; font-size: 0.85em; }
#quality-trend-app th, #quality-trend-app td, #ac4-quality-trend-app th, #ac4-quality-trend-app td { padding: 0.35em 0.6em; text-align: left; border-bottom: 1px solid var(--md-default-fg-color--lightest); white-space: nowrap; }
.quality-trend-regression { color: var(--md-typeset-mark-color, #c62828); font-weight: 600; }
.quality-trend-secondary-check { color: var(--md-default-fg-color--light); cursor: help; }
.quality-trend-release-row { background: color-mix(in srgb, var(--md-accent-fg-color, #7c4dff) 8%, transparent); }
.quality-trend-release { text-decoration: none; font-weight: 600; }
.quality-trend-release:hover { text-decoration: underline; }
</style>

<script>
(function () {
  // The site is served under the repository's name, so the repository is read off the address:
  // it is right before and after the repository is renamed. A local preview, and the stub
  // tools/ci/render_measurement_tiles.js runs the scripts in, have no such address.
  const REPO = typeof location !== "undefined" && location.hostname.endsWith(".github.io")
    ? location.hostname.split(".")[0] + "/" + location.pathname.split("/")[1]
    : "iainchesworthlabs/iclforge";
  const HISTORY_BRANCH = "quality-history";
  const MAIN_COLOR = "#00acc1";
  // Muted and dashed (see buildChart) rather than a third saturated colour -
  // the historical track is deliberately styled to read as archived, not as
  // a second live series competing with main.
  const HISTORICAL_COLOR = "#9e9e9e";
  // Mirrors tools/ci/append_quality_history.py's own constants - keep these two in
  // sync if that script's thresholds change; this is a display-only echo of the
  // same judgment call, not a second source of truth for it. Only the soft
  // (non-failing) tier is displayed here - a hard regression fails its own CI
  // run directly (see _build.yml), so it doesn't need a table annotation to
  // be noticed too.
  const REGRESSION_WINDOW = 10;
  const REGRESSION_DROP_DB = 0.5;
  const TABLE_ROWS = 40;
  // WAV channel order iclforge::ac3::io::ac3_layout_for(6) expects - see
  // tools/generators/gen_gold_reference_wav.py - and so the order compare_wav.py's
  // channels_db is written in. Only meaningful for the current 6-channel 5.1
  // golden reference; anything else (e.g. a future Atmos-bed layout with a
  // different channel count) falls back to a plain index label rather than
  // guessing at a mapping.
  const CHANNEL_LABELS_51 = ["L", "R", "C", "LFE", "Ls", "Rs"];
  // Mirrors the gold_reference legs of .github/ci/legs.jsonc - the same 9 legs
  // tools/ci/append_quality_history.py strips the "gold-reference-" artifact
  // prefix down to. The run after a merge covers the first six; the nightly
  // run adds the last three (the deep-tier arm64 and macOS x64 legs). Fixed
  // order/colors so a leg's line keeps the same color across renders instead
  // of shuffling with whichever legs happen to have data in the current view.
  const LEGS = [
    { leg: "windows-msvc", color: "#7c4dff" },
    { leg: "windows-llvm", color: "#00acc1" },
    { leg: "linux-gcc", color: "#43a047" },
    { leg: "linux-llvm", color: "#fb8c00" },
    { leg: "macos-llvm", color: "#e53935" },
    { leg: "linux-gcc-arm64", color: "#1e88e5" },
    { leg: "windows-msvc-arm64", color: "#8e24aa" },
    { leg: "linux-llvm-arm64", color: "#6d4c41" },
    { leg: "macos-llvm-x64", color: "#d81b60" },
  ];

  const root = document.getElementById("quality-trend-app");

  // Filter/display state, mutated by the controls and re-read on every
  // render() - kept outside render() so a control change doesn't need to
  // thread its way back in as a parameter.
  const state = {
    codec: "ac3",
    // "branch": one line for main, worst-of-legs per commit (the
    // original view - good for "did anything regress"), plus an optional
    // second line for develop's frozen history. "leg": one line per CI leg
    // for a single track, un-folded (good for "is one platform drifting
    // relative to the others over time") - see LEGS above. Never both
    // branch and leg as line dimensions at once: up to 9 legs x 2 tracks =
    // 18 lines was exactly the "unreadable" case worstPerCommit was written
    // to avoid, so leg view picks one track instead of folding it away.
    view: "branch",
    // develop stopped moving on 2026-08-25's move to trunk-based
    // development (see "Where the data lives" below) - its history is real
    // and kept, but it is no longer an ongoing parallel track, so it starts
    // hidden rather than shown by default alongside main.
    showHistorical: false,
    // Which track's per-leg lines are drawn in "leg" view. main by default,
    // now that it is the only track still gaining commits; develop's frozen
    // history is still selectable for the platforms it covered.
    legBranch: "main",
  };

  function rawUrl(branch, file) {
    return `https://raw.githubusercontent.com/${REPO}/${branch}/${file}`;
  }

  function parseJsonl(text) {
    return text.split("\n").filter((l) => l.trim().length > 0).map((l) => JSON.parse(l));
  }

  // The trailing window first, the full history only if it is not there.
  //
  // The history files on quality-history are append-only and unbounded -
  // main.jsonl passed 1.7 MB and develop.jsonl 1.8 MB by September 2026 - and
  // this page was fetching BOTH in full to render TABLE_ROWS entries per
  // series. 3.5 MB of download to display fifty commits' worth of it, growing
  // with every merge.
  //
  // The window is produced by tools/ci/append_quality_history.py rather than
  // requested here, because requesting it does not work: an HTTP suffix Range
  // is the natural fix and `Range` is not a CORS-safelisted request header, so
  // a cross-origin fetch preflights and raw.githubusercontent.com answers
  // OPTIONS with a 403. Verified from this very origin - the plain fetch
  // returns 1789155 bytes, the ranged one fails outright.
  //
  // Falling back rather than assuming: a page served before that script first
  // ran, or a branch whose window has not been generated, still renders from
  // the full file exactly as it always did.
  async function fetchTrack(branch) {
    for (const file of [`${branch}.recent.jsonl`, `${branch}.jsonl`]) {
      try {
        const resp = await fetch(rawUrl(HISTORY_BRANCH, file));
        if (!resp.ok) continue;
        return parseJsonl(await resp.text());
      } catch (e) {
        // Try the next candidate; only a total failure of both yields [].
      }
    }
    return [];
  }

  // Maps a commit SHA to the release tagged at it, keyed off the GitHub
  // REST API rather than anything in quality-history itself - release.yml
  // tags an existing main commit after the fact, so the tag never appears in
  // the quality-history record for that commit. Client-side and
  // best-effort, same as fetchTrack above: a rate-limited or offline
  // response just means no release markers, not a broken page.
  async function fetchReleaseShaMap() {
    let shaMap = {};
    try {
      const resp = await fetch(`https://api.github.com/repos/${REPO}/tags?per_page=100`);
      if (!resp.ok) return shaMap;
      const tags = await resp.json();
      for (const t of tags) {
        if (t.commit && t.commit.sha) shaMap[t.commit.sha] = { tag: t.name, name: t.name, url: `https://github.com/${REPO}/releases/tag/${t.name}` };
      }
    } catch (e) {
      return shaMap;
    }
    try {
      const resp = await fetch(`https://api.github.com/repos/${REPO}/releases?per_page=100`);
      if (resp.ok) {
        const releases = await resp.json();
        const byTag = {};
        for (const rel of releases) byTag[rel.tag_name] = rel;
        for (const sha of Object.keys(shaMap)) {
          const rel = byTag[shaMap[sha].tag];
          if (rel) {
            shaMap[sha].name = rel.name || shaMap[sha].tag;
            shaMap[sha].prerelease = !!rel.prerelease;
            shaMap[sha].url = rel.html_url || shaMap[sha].url;
          }
        }
      }
    } catch (e) {
      // Tag->sha map still usable without release metadata.
    }
    return shaMap;
  }

  function shortSha(sha) {
    return sha.slice(0, 8);
  }

  function commitUrl(sha) {
    return `https://github.com/${REPO}/commit/${sha}`;
  }

  function channelLabel(idx, total) {
    return total === CHANNEL_LABELS_51.length ? CHANNEL_LABELS_51[idx] : `ch${idx}`;
  }

  function worstChannelLabel(r) {
    return channelLabel(r.channels_db.indexOf(r.worst_db), r.channels_db.length);
  }

  function channelBreakdownText(r) {
    return r.channels_db.map((v, i) => `${channelLabel(i, r.channels_db.length)} ${v.toFixed(2)} dB`).join(" · ");
  }

  // --- Per-channel floors ---------------------------------------------------
  // Each channel is gated against its own floor, so the channel closest to
  // FAILING is not generally the channel with the lowest number - on these
  // 5.1 fixtures the surrounds are always lowest and are also, by design, the
  // furthest above their own (much lower) floors. A table that shows only the
  // worst dB therefore shows the same two channels forever and never the one
  // actually at risk.
  //
  // Records written before per-channel floors carry no thresholds_db. They
  // render "-" rather than a computed-from-a-scalar number: the old rows were
  // gated on one floor for all channels, and inventing a per-channel
  // margin for them would make history look like it had a gate it did not.
  function hasPerChannelFloors(r) {
    return Array.isArray(r.thresholds_db) && r.thresholds_db.length === r.channels_db.length;
  }

  function tightestIndex(r) {
    if (typeof r.tightest_channel === "number") return r.tightest_channel;
    let best = 0;
    for (let i = 1; i < r.channels_db.length; i++) {
      if (r.channels_db[i] - r.thresholds_db[i] < r.channels_db[best] - r.thresholds_db[best]) best = i;
    }
    return best;
  }

  function tightestMarginText(r) {
    if (!hasPerChannelFloors(r)) return "—";
    const i = tightestIndex(r);
    const margin = r.channels_db[i] - r.thresholds_db[i];
    return `${channelLabel(i, r.channels_db.length)} +${margin.toFixed(2)} dB`;
  }

  function marginBreakdownText(r) {
    if (!hasPerChannelFloors(r)) {
      return "Gated on a single floor for every channel - this row predates per-channel floors.";
    }
    return r.channels_db.map((v, i) =>
      `${channelLabel(i, r.channels_db.length)} ${(v - r.thresholds_db[i]).toFixed(2)} dB over ${r.thresholds_db[i]}`
    ).join(" · ");
  }

  // The records currently in scope given the historical toggle - both the
  // chart and the table are built from this, not from the raw fetch, so
  // they always agree with what the control says should be visible. main is
  // always in scope; develop's frozen history joins it only when asked for,
  // and always in full (nothing to collapse - it is a fixed, finite series
  // now, not an ever-growing one crowding main's rows out of the table).
  function visibleRecords(allRecords) {
    return allRecords.filter((r) => r.branch === "main" || (state.showHistorical && r.branch === "develop"));
  }

  // verify_gold_reference.sh runs more than one check per codec now - e.g.
  // eac3's tools=none baseline, its tools=cpl variant, and the unrelated
  // eac3_cplbndstrce0 third-party-bitstream interop fixture all share
  // codec:"eac3" but compare fundamentally different things at deliberately
  // different SNR floors (see tools/ci/append_quality_history.py's own
  // "check" field, added for the same reason). This page's chart has only
  // ever shown one line per codec, so it sticks to each codec's original,
  // continuous baseline check (check === codec) rather than folding in the
  // newer variants - a record with no "check" at all is older history
  // written before that field existed, and was always that baseline check
  // by construction, so it counts too.
  function isPrimaryCheck(r) {
    return !r.check || r.check === r.codec;
  }

  // One point per commit per branch: the worst worst_db across every leg for
  // the selected codec, since a per-leg-and-codec chart (up to 9 legs x 2
  // codecs x 2 branches) would be unreadable as lines. Leg-level detail is
  // still in the table below, just not the chart.
  function worstPerCommit(records, codec) {
    const byCommit = new Map();
    for (const r of records) {
      if (r.codec !== codec || !isPrimaryCheck(r)) continue;
      const cur = byCommit.get(r.commit);
      if (!cur || r.worst_db < cur.worst_db) {
        byCommit.set(r.commit, r);
      }
    }
    return Array.from(byCommit.values()).sort((a, b) => a.commit_date.localeCompare(b.commit_date));
  }

  // The un-folded counterpart to worstPerCommit above: every (leg, codec,
  // branch) record, one series per leg, nothing dropped. This is what "leg"
  // view charts - the same per-leg detail worstPerCommit discards, just kept
  // instead of collapsed to a single worst-of-legs point.
  function perLegSeries(records, codec, branch) {
    const byLeg = {};
    for (const legDef of LEGS) byLeg[legDef.leg] = [];
    for (const r of records) {
      if (r.codec !== codec || r.branch !== branch || !isPrimaryCheck(r)) continue;
      if (byLeg[r.leg]) byLeg[r.leg].push(r);
    }
    for (const leg of Object.keys(byLeg)) {
      byLeg[leg].sort((a, b) => a.commit_date.localeCompare(b.commit_date));
    }
    return byLeg;
  }

  // Always computed against the full, unfiltered history for the (leg,
  // codec, check, branch) - collapsing develop's *display* to its latest
  // commit shouldn't change what counts as a regression against its own
  // trailing average. "check" (falling back to codec for pre-"check" history,
  // same reasoning as isPrimaryCheck above) keeps this matched to the same
  // check the row itself came from, instead of averaging across unrelated
  // checks that happen to share a codec.
  function regressionBaseline(allRecords, leg, codec, check, branch, beforeCommitDate) {
    const trail = allRecords
      .filter((r) => r.leg === leg && r.codec === codec && (r.check || r.codec) === check &&
                     r.branch === branch && r.commit_date < beforeCommitDate)
      .sort((a, b) => a.commit_date.localeCompare(b.commit_date))
      .slice(-REGRESSION_WINDOW);
    if (trail.length === 0) return null;
    return trail.reduce((sum, r) => sum + r.worst_db, 0) / trail.length;
  }

  // Shared, calendar-time x-axis (not a per-track point index) - required
  // once tracks can carry very different point counts, e.g. develop's full
  // history against main's single latest entry: an index-based axis would
  // stack main's one point at the start of the line instead of at its actual
  // (recent) date.
  //
  // Generic over what a "track" is: branch view passes one track per branch
  // (worst-of-legs points), leg view passes one track per CI leg for a
  // single branch (un-folded points) - same x/y math and SVG either way,
  // only the line count and labels differ. Each track is
  // { key, label, color, points }.
  function buildChart(tracks, codec, releasesBySha) {
    const width = 760, height = 220, pad = { top: 12, right: 12, bottom: 32, left: 42 };
    const allPoints = tracks.flatMap((t) => t.points);
    if (allPoints.length === 0) {
      return `<p class="quality-trend-status">No ${codec} history in the current view.</p>`;
    }
    const dbValues = allPoints.map((p) => p.worst_db);
    const minDb = Math.min(...dbValues, 20);
    const maxDb = Math.max(...dbValues) + 2;
    const times = allPoints.map((p) => Date.parse(p.commit_date));
    const minT = Math.min(...times);
    const maxT = Math.max(...times);

    const x = (t) => pad.left + (maxT === minT ? (width - pad.left - pad.right) / 2 : ((t - minT) / (maxT - minT)) * (width - pad.left - pad.right));
    const y = (db) => height - pad.bottom - ((db - minDb) / (maxDb - minDb)) * (height - pad.top - pad.bottom);

    let svg = `<svg class="quality-trend-chart" viewBox="0 0 ${width} ${height}" width="${width}" height="${height}" role="img" aria-label="Worst-channel SNR by commit date, ${codec}">`;
    for (let i = 0; i <= 4; i++) {
      const db = minDb + ((maxDb - minDb) * i) / 4;
      const gy = y(db);
      svg += `<line x1="${pad.left}" y1="${gy}" x2="${width - pad.right}" y2="${gy}" stroke="var(--md-default-fg-color--lightest)" stroke-width="1"/>`;
      svg += `<text x="${pad.left - 6}" y="${gy + 3}" text-anchor="end" font-size="10" fill="var(--md-default-fg-color--light)">${db.toFixed(0)}</text>`;
    }
    svg += `<text x="${pad.left}" y="${height - 8}" text-anchor="start" font-size="10" fill="var(--md-default-fg-color--light)">${new Date(minT).toISOString().slice(0, 10)}</text>`;
    svg += `<text x="${width - pad.right}" y="${height - 8}" text-anchor="end" font-size="10" fill="var(--md-default-fg-color--light)">${new Date(maxT).toISOString().slice(0, 10)}</text>`;

    for (const track of tracks) {
      const pts = track.points;
      if (pts.length === 0) continue;
      if (pts.length > 1) {
        const path = pts.map((p, i) => `${i === 0 ? "M" : "L"}${x(Date.parse(p.commit_date)).toFixed(1)},${y(p.worst_db).toFixed(1)}`).join(" ");
        const dash = track.dashed ? ' stroke-dasharray="5,3"' : "";
        svg += `<path d="${path}" fill="none" stroke="${track.color}" stroke-width="2"${dash}/>`;
      }
      pts.forEach((p) => {
        const cx = x(Date.parse(p.commit_date)).toFixed(1);
        const cy = y(p.worst_db).toFixed(1);
        const release = releasesBySha[p.commit];
        const title = `${track.label} ${shortSha(p.commit)} - ${p.worst_db.toFixed(2)} dB (${worstChannelLabel(p)}, worst of ${channelBreakdownText(p)}) on ${p.commit_date.slice(0, 10)}${release ? ` - release ${release.name}` : ""}`;
        svg += `<circle cx="${cx}" cy="${cy}" r="3" fill="${track.color}"><title>${title}</title></circle>`;
        if (release) {
          svg += `<circle cx="${cx}" cy="${cy}" r="6.5" fill="none" stroke="${track.color}" stroke-width="1.5" stroke-dasharray="2,1.5"><title>${title}</title></circle>`;
        }
      });
    }
    svg += "</svg>";
    return svg;
  }

  function buildLegend(tracks, releasesBySha) {
    const items = tracks.map((t) => `<span><i style="background:${t.color}"></i>${t.label}</span>`);
    const anyRelease = tracks.some((t) => t.points.some((p) => releasesBySha[p.commit]));
    if (anyRelease) {
      items.push('<span><i style="background:none;border:1.5px dashed var(--md-default-fg-color--light);"></i>tagged release</span>');
    }
    return `<div class="quality-trend-legend">${items.join("")}</div>`;
  }

  // Every check for the row's codec, in a single dedicated column, so two
  // checks that share a codec (and can carry very different SNR floors -
  // see isPrimaryCheck above) are never left to be told apart only by
  // eyeballing the bitrate or the worst-dB number. checkLabel falls back to
  // the codec name for pre-"check" history, same reasoning as isPrimaryCheck.
  function checkLabel(r) {
    return r.check || r.codec;
  }

  function buildTable(rows, allRecords, releasesBySha) {
    const trs = rows
      .slice()
      .sort((a, b) => b.commit_date.localeCompare(a.commit_date))
      .slice(0, TABLE_ROWS)
      .map((r) => {
        const baseline = regressionBaseline(allRecords, r.leg, r.codec, r.check || r.codec, r.branch, r.commit_date);
        const regressed = baseline !== null && baseline - r.worst_db >= REGRESSION_DROP_DB;
        const flag = regressed
          ? `<span class="quality-trend-regression" title="${(baseline - r.worst_db).toFixed(2)} dB below the trailing ${REGRESSION_WINDOW}-run mean (${baseline.toFixed(2)} dB)">▼ regression</span>`
          : "";
        const release = releasesBySha[r.commit];
        const releaseBadge = release
          ? `<a class="quality-trend-release" href="${release.url}" title="${release.prerelease ? "Prerelease" : "Release"} tagged at this commit">🏷 ${release.name}</a>`
          : "";
        const check = isPrimaryCheck(r)
          ? checkLabel(r)
          : `${checkLabel(r)} <span class="quality-trend-secondary-check" title="Not ${r.codec}'s primary round-trip check - a separate fixture with its own SNR floor and its own trailing baseline, not comparable to the ${r.codec} row above it">†</span>`;
        return `<tr${release ? ' class="quality-trend-release-row"' : ""}>
          <td>${r.commit_date.slice(0, 10)}</td>
          <td>${r.branch}</td>
          <td><a href="${commitUrl(r.commit)}">${shortSha(r.commit)}</a></td>
          <td>${r.leg}</td>
          <td>${r.codec} @ ${r.bitrate_kbps} kbps</td>
          <td>${check}</td>
          <td title="${channelBreakdownText(r)}">${worstChannelLabel(r)} ${r.worst_db.toFixed(2)} dB</td>
          <td title="${marginBreakdownText(r)}">${tightestMarginText(r)}</td>
          <td>${releaseBadge}</td>
          <td>${flag}</td>
        </tr>`;
      })
      .join("");
    if (trs === "") {
      return '<p class="quality-trend-status">No rows in the current view - try a different branch/codec combination.</p>';
    }
    return `<div class="quality-trend-table-wrap"><table>
      <thead><tr><th>Date</th><th>Branch</th><th>Commit</th><th>Leg</th><th>Codec</th><th>Check</th><th>Worst channel</th><th>Tightest margin</th><th>Release</th><th></th></tr></thead>
      <tbody>${trs}</tbody>
    </table></div>`;
  }

  function buildControls() {
    const legView = state.view === "leg";
    return `
      <div class="quality-trend-controls">
        <label for="quality-trend-codec">Codec
          <select id="quality-trend-codec">
            <option value="ac3" ${state.codec === "ac3" ? "selected" : ""}>AC-3</option>
            <option value="eac3" ${state.codec === "eac3" ? "selected" : ""}>E-AC-3</option>
          </select>
        </label>
        <label for="quality-trend-view">Chart
          <select id="quality-trend-view">
            <option value="branch" ${!legView ? "selected" : ""}>Worst of legs</option>
            <option value="leg" ${legView ? "selected" : ""}>By platform leg</option>
          </select>
        </label>
        ${legView ? `
          <label for="quality-trend-leg-branch">Track
            <select id="quality-trend-leg-branch">
              <option value="main" ${state.legBranch === "main" ? "selected" : ""}>main</option>
              <option value="develop" ${state.legBranch === "develop" ? "selected" : ""}>develop (historical, pre-2026-08-25)</option>
            </select>
          </label>
        ` : `
          <label><input type="checkbox" id="quality-trend-show-historical" ${state.showHistorical ? "checked" : ""}/> Show historical (develop, pre-2026-08-25)</label>
        `}
      </div>
    `;
  }

  function attachControlListeners(allRecords, releasesBySha) {
    document.getElementById("quality-trend-codec").addEventListener("change", (e) => {
      state.codec = e.target.value;
      render(allRecords, releasesBySha);
    });
    document.getElementById("quality-trend-view").addEventListener("change", (e) => {
      state.view = e.target.value;
      render(allRecords, releasesBySha);
    });
    const legBranchSelect = document.getElementById("quality-trend-leg-branch");
    if (legBranchSelect) {
      legBranchSelect.addEventListener("change", (e) => {
        state.legBranch = e.target.value;
        render(allRecords, releasesBySha);
      });
    }
    const historicalToggle = document.getElementById("quality-trend-show-historical");
    if (historicalToggle) {
      historicalToggle.addEventListener("change", (e) => {
        state.showHistorical = e.target.checked;
        render(allRecords, releasesBySha);
      });
    }
  }

  // Builds this render's tracks per state.view - branch view folds main
  // (and, if asked for, develop's frozen history) down to a worst-of-legs
  // line each (worstPerCommit); leg view un-folds a single track into one
  // line per CI leg (perLegSeries). Kept out of render() itself so render()
  // stays about assembling the page, not about which view is active.
  function buildTracks(visible) {
    if (state.view === "leg") {
      const byLeg = perLegSeries(visible, state.codec, state.legBranch);
      return LEGS.map((legDef) => ({
        key: legDef.leg,
        label: legDef.leg,
        color: legDef.color,
        points: byLeg[legDef.leg] || [],
      }));
    }
    const tracks = [{
      key: "main",
      label: "main",
      color: MAIN_COLOR,
      points: worstPerCommit(visible.filter((r) => r.branch === "main"), state.codec),
    }];
    if (state.showHistorical) {
      tracks.push({
        key: "develop",
        label: "develop (historical, pre-2026-08-25)",
        color: HISTORICAL_COLOR,
        dashed: true,
        points: worstPerCommit(visible.filter((r) => r.branch === "develop"), state.codec),
      });
    }
    return tracks;
  }

  function render(allRecords, releasesBySha) {
    // Leg view looks at one branch's fetched history window - the whole point
    // is seeing a per-leg trend over many commits, so the develop-collapse
    // and other-branch filters that branch view uses don't apply here.
    const visible = state.view === "leg"
      ? allRecords.filter((r) => r.branch === state.legBranch)
      : visibleRecords(allRecords);
    const tracks = buildTracks(visible);
    // The table used to get the same `visible` list the chart starts from -
    // every codec and every check, unfiltered - while only the chart applied
    // state.codec and isPrimaryCheck. That let the table interleave rows the
    // Codec control claimed to be scoping (even a different codec entirely)
    // with no per-row way to tell them apart, which is exactly the
    // conflation isPrimaryCheck exists to prevent in the chart. Scoping the
    // table to the selected codec too - while still showing every check
    // within it, distinguished by the Check column above - keeps secondary
    // checks visible instead of hidden, but never unlabelled next to a
    // codec's primary series.
    const tableRows = visible.filter((r) => r.codec === state.codec);
    root.innerHTML = `
      ${buildControls()}
      <div class="quality-trend-chart-wrap">${buildChart(tracks, state.codec, releasesBySha)}</div>
      ${buildLegend(tracks, releasesBySha)}
      ${buildTable(tableRows, allRecords, releasesBySha)}
    `;
    attachControlListeners(allRecords, releasesBySha);
  }

  // Both files are always fetched - main to render, develop so the
  // historical toggle above has something to show the instant it is
  // checked, with no second round-trip.
  Promise.all([fetchTrack("main"), fetchTrack("develop"), fetchReleaseShaMap()]).then(([mainRecords, developRecords, releasesBySha]) => {
    const allRecords = [...mainRecords, ...developRecords];
    if (allRecords.length === 0) {
      root.innerHTML = '<p class="quality-trend-status">No quality-trend history yet - the first run on main after this page landed writes it.</p>';
      return;
    }
    render(allRecords, releasesBySha);
  });
})();
</script>

## Reading it

Each row is one (commit, CI leg, codec) result — the gate runs on every
`gold_reference` leg (`windows-msvc`, `windows-llvm`, `linux-gcc`,
`linux-gcc-arm64`, `linux-llvm`, `macos-llvm` after a merge, and in the
nightly run also `windows-msvc-arm64`, `linux-llvm-arm64` and
`macos-llvm-x64`; not the ASan+UBSan leg, which stays
diagnostic-only), so a single run contributes up to six rows per check after a
merge and up to nine in the nightly run.

The **Chart** control picks what the lines represent. "Worst of legs, by
branch" (the default) plots the worst of the legs that ran per commit, one line
per branch, against a shared calendar x-axis — a cross-leg floating-point
difference (a ~62 dB vs. ~68 dB split, the arm64 legs below the x86-64 ones,
is a known, expected effect of platform floating-point differences, explained
in [Validation](verification.md#why-arm64-and-x86-64-disagree)) is expected and not itself a
regression, so folding it away is deliberate here: this view answers "did
*anything* regress," not "which platform." "By platform leg" answers that
second question instead — it un-folds a single branch (picked with the
**Branch** control that replaces the branch checkboxes in this view) into
one line per leg, so a leg drifting relative to the others, or trending
down over many commits while the rest hold steady, is visible as a shape in
the chart rather than something you'd only catch by scanning the table leg
by leg. Both views read the same underlying rows; nothing about which view
is active changes what counts as a regression in the table below.

`main`'s recent window is the default view, with the full history used while
it still fits that window. Before 2026-08-25's move to
trunk-based development (see "Where the data lives" below), `develop` was
the everyday integration branch nearly every commit landed on and `main`
only advanced on a release promotion, so the two really were separate
tracks rather than one delayed copy of the other — which is why this page
used to plot them side by side by default. `develop` is retired now and its
line stopped moving on its last commit before the migration; check **Show
historical (develop, pre-2026-08-25)** to add that frozen history back into
the chart and table as a dashed, muted secondary line, styled to read as
archived rather than as a second live series. "By platform leg" offers the
same choice through its **Track** selector.

The **Codec** control scopes the table as well as the chart — picking
`E-AC-3` shows only `eac3` rows, never an unrelated `ac3` row sorted in by
date alone. Within that codec, the table still shows every **Check**:
`verify_gold_reference.sh` can run more than one check per codec (e.g.
`eac3`'s own baseline round-trip alongside `eac3_cplbndstrce0`, a real
third-party FFmpeg bitstream used to regression-test an Annex E decode fix),
and those checks compare fundamentally different things at deliberately
different SNR floors — one is not a worse day for the other. A `†` marks
any check that isn't that codec's primary, continuous series (the one the
chart plots and the dropdown otherwise implies); hover it for why. Never
read two different Check values as one continuous line, even when they
share a Codec and a date range — a `†` row's own trailing baseline (used
for the regression flag below) is computed only against its own check
history, precisely so a steady 25 dB interop floor can't look like a crash
relative to a steady 68 dB round-trip series, or vice versa.

The **worst channel** column names which of the six channels was worst
(`L R C LFE Ls Rs`, the golden reference's WAV channel order), and hovering
it shows every channel's number. In practice it is almost always one of the
two surrounds (`Ls`/`Rs`) — 15-20 dB below the front channels, consistently,
across every leg and commit recorded so far — which tracks with the encoder
allocating fewer bits to the less-dominant surround channels, not a
per-run fluke.

The **tightest margin** column is the one to watch, and it is deliberately a
different question. Every channel is gated against its *own* floor now (see
[Validation](verification.md#one-floor-per-channel-not-one-per-file) for how
those floors are derived), and the channel closest to failing is not the
channel with the lowest number — usually the reverse. The surrounds are lowest
*and* have the lowest floors, so they typically sit further above their gate
than a front channel does above its much higher one. Hovering shows every
channel's margin.

Rows written before per-channel floors show `—` here rather than a number.
They were gated on one floor shared by all six channels, and
back-computing a per-channel margin for them would make the history look like
it carried a gate it did not have. The `worst channel` column is directly
comparable across that boundary; this one is not, by construction.

A regression is now flagged when **any channel** falls 0.5 dB below its own
trailing average, not only when the worst channel does. That closed a real
hole: the worst channel was the same dither-dominated surround on every single
run, so a front channel could fall a long way without the trend check ever
looking at it.

A 🏷 badge marks a row whose commit was tagged as a GitHub release (fetched
client-side from the GitHub API, best-effort — it silently shows nothing if
that call is rate-limited or offline). Release tagging happens after the
fact, on an existing `main` commit, so the badge is a join against the
commit SHA already in quality-history, not a separate data source.

## The fixed-point decode has its own series too

`_fixed`-suffixed checks are the gate run against a decoder built with
`-DICLFORGE_DECODE_SCALAR=fixed` - Q7.24 integer arithmetic under a block
exponent, the tier for a part with no FPU
([`planning/arithmetic-tiers.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/arithmetic-tiers.md)).
It differs from the double decode at 121 dB and above on the gold streams
(`tools/checks/check_decode_scalar_snr.py`, held to 110 in the same pass), which is as
far below this gate's coding noise as the float32 decode's 139, so the same
reading applies: the series exists to show the arithmetic contributes nothing
this gate can see, and would show a lost bit the day it did. The pass is one of the
`linux-gcc` leg's `scalar_variants` in `.github/ci/legs.jsonc`, which only the nightly run
makes, so the series has a point a night rather than one for each run after a merge.

## The float32 decode has its own series

`_float32`-suffixed checks are the same gate run against a decoder built with
`-DICLFORGE_DECODE_SCALAR=float` — the arithmetic the minimum-footprint profile
uses, and the arithmetic every fixture on both bare-metal legs is decoded
through. Until that option existed the float32 path could not be built into
anything with a CLI, so nothing here had ever measured it: `docs/building.md`
carried a single hand-taken SNR figure and nothing re-derived it.

Measured against the double build, channel for channel, it comes out **the
same to the resolution this gate reports** — 18.47, 45.63, 51.47, 54.67, 88.21,
35.89, 36.45 dB and so on, identically. That is not the two decodes being
bit-identical; they differ at about 139 dB
(`tools/checks/check_decode_scalar_snr.py`). It is that this gate's SNR is
dominated by *coding* noise 80 dB above that difference, so float32 contributes
nothing measurable to the error budget the quality gate actually measures.

The chart shows one line per codec (`check === codec`), so these sit in the
history data rather than on it — the point of trending them is that a future
change to the float32 path shows up as a divergence from the double series,
which is a comparison no single figure in a document can make. This pass is also one
of the nightly run's `scalar_variants`.

## The direct-form transforms and the float32 encoder have series too

`_reference`-suffixed checks are the gate run with `mode=reference`: the spec's own
direct-form transforms in place of the fast paths
([Validation](verification.md#performance-and-reference-modes)). The `linux-gcc` leg makes
this pass in the run after a merge as well as in the nightly run. `_encfloat`-suffixed
checks are the gate run against an encoder built with `-DICLFORGE_ENCODE_SCALAR=float`, the
arithmetic the ESP32-S3's minimum-footprint profile encodes in. A float encoder makes its
own decisions and produces a different, equally valid stream, so these rows are held to the
same floors as the double encoder's and `tools/checks/check_encode_scalar_quality.py` holds
its worst channel to half a dB of the double encoder's. That pass is a nightly one too.
Neither has a line on the chart.

## Where the data lives

Results are appended to a dedicated `quality-history` branch (`develop.jsonl`
/ `main.jsonl`), not `gh-pages` — `mkdocs gh-deploy` replaces gh-pages'
entire tree on every deploy, which would silently discard anything appended
there outside of what `mkdocs build` itself generates. This page prefers each
history's generated `.recent.jsonl` window and falls back to the full file,
both fetched from `raw.githubusercontent.com`, so a new
push shows up here without waiting on a docs deploy (which,
per [docs.yml](https://github.com/iainchesworthlabs/iclforge/blob/main/.github/workflows/docs.yml),
only runs on push to `main`).

`develop.jsonl` stopped gaining rows on 2026-08-24, `develop`'s last commit
before the branch was retired in favour of trunk-based development. It is
kept as-is — real project history, not deleted or rewritten — and is what
the "Show historical" control above reads; every trigger that used to append
to it now targets `main` only (`.github/workflows/ci.yml`'s and
`_build.yml`'s push conditions, and the `--branch` argument
`tools/ci/append_quality_history.py` is called with, both narrowed to `main`
in the same migration).

History is written by the `Publish quality trend` job in `_build.yml`, which
runs once the build legs of a run on `main` have passed, in the run after a
merge and in the nightly run —
never on a pull request or a queue entry, so unmerged work never pollutes the
trend. A leg that a newer run on `main` gave way to leaves a gap in its own
series and does not stop the rest being published; a leg that failed skips the
publish. It reuses numbers the gate already computed rather than re-running the
encode/decode pass, so — unlike a from-scratch perceptual pass, which would
need a nightly cadence to bound cost — publishing after each run costs nothing
extra to compute; only a JSON append and a git push are new.

`main`'s history has a real gap before 2026-08-10: `ci.yml`'s concurrency
group used to key push runs on branch name alone with `cancel-in-progress`
on, so a burst of merges landing within a build's runtime cancelled every
run but the last, silently dropping the gold-reference gate — and this
page's append step with it — before either finished. The specific commits
lost to it were backfilled by hand rather than left blank. Since 2026-09-29
the gap is by design instead: pushes to `main` are verified one at a time and a
burst of merges is one run on the newest commit ([CI for many
agents](ci-agentic.md#after-the-merge)), so a commit in the middle of a burst has no point of
its own, and nothing was dropped.

Two runs can still write here at once, for example the nightly run and a run
after a merge. The run that pushes second is rejected, rebases onto the first,
and meets a conflict: both runs added records to the end of the same file.
[`tools/ci/resolve_history_conflict.py`](https://github.com/iainchesworthlabs/iclforge/blob/main/tools/ci/resolve_history_conflict.py)
settles it by keeping both runs' records and writing the `.recent.jsonl`
window again from the result, so what this page reads carries every run's
numbers whichever run finishes first. Anything it cannot account for fails the
publishing job rather than being guessed at, which is what happened to
`main@681a083a` on 2026-09-18 before the script existed.

## AC-4 decode quality

Everything above is AC-3 and E-AC-3. AC-4 has its own series, because its oracle is different:
FFmpeg does not decode AC-4, so
[`tools/checks/score_ac4_decode.py`](https://github.com/iainchesworthlabs/iclforge/blob/main/tools/checks/score_ac4_decode.py)
decodes the Dolby Encoding Engine's AC-4 streams and scores each against the source it was encoded
from ([Validation](verification.md#ac-4) describes how). Its measure is Decode accuracy against
that source, not against another decoder. It runs in FFmpeg Validate, a nightly job, on the
`linux-llvm` build, and its pinned floors fail that run. The same run writes the scores as a JSON
artifact, and the `Publish quality trend` job appends one row per stream to
`ac4-quality-main.jsonl` on the same `quality-history` branch, by
[`tools/ci/append_ac4_quality_history.py`](https://github.com/iainchesworthlabs/iclforge/blob/main/tools/ci/append_ac4_quality_history.py):
one set of rows per nightly run. It covers 10 of the 15 committed legs, described below the
chart. The AC-4 encoder's own scores (`score_ac4_encode.py`) are held to pinned floors only, and
its race against the Dolby encoder runs locally, so neither has a series; nor do AC-4's objects.

<div id="ac4-quality-trend-app">
  <p class="quality-trend-status">Loading AC-4 decode quality…</p>
</div>

<script>
(function () {
  // The site is served under the repository's name, so the repository is read off the address:
  // it is right before and after the repository is renamed. A local preview, and the stub
  // tools/ci/render_measurement_tiles.js runs the scripts in, have no such address.
  const REPO = typeof location !== "undefined" && location.hostname.endsWith(".github.io")
    ? location.hostname.split(".")[0] + "/" + location.pathname.split("/")[1]
    : "iainchesworthlabs/iclforge";
  const HISTORY_BRANCH = "quality-history";
  const HISTORY_FILE = "ac4-quality-main";
  // Mirrors tools/ci/append_ac4_quality_history.py's thresholds - a display
  // echo of that script's judgment, not a second source of truth for it.
  const REGRESSION_WINDOW = 10;
  const SNR_DROP_DB = 0.5;
  const LSD_RISE_DB = 0.5;
  const TABLE_ROWS = 40;
  const COLOR = "#fb8c00";
  // Which way is worse, per measure: the chart plots each commit's worst
  // stream, so a drop in any one stream shows.
  const METRICS = {
    min_snr_db: { label: "Minimum SNR (dB)", worse: (a, b) => a < b },
    lsd_db: { label: "LSD (dB)", worse: (a, b) => a > b },
    mos_lqo: { label: "MOS-LQO", worse: (a, b) => a < b },
  };
  const root = document.getElementById("ac4-quality-trend-app");
  const state = { metric: "min_snr_db" };

  function rawUrl(file) {
    return `https://raw.githubusercontent.com/${REPO}/${HISTORY_BRANCH}/${file}`;
  }

  // The generated window first, the full file if there is none - the same
  // fallback the AC-3 series above uses.
  async function fetchHistory() {
    for (const file of [`${HISTORY_FILE}.recent.jsonl`, `${HISTORY_FILE}.jsonl`]) {
      try {
        const resp = await fetch(rawUrl(file));
        if (!resp.ok) continue;
        const text = await resp.text();
        return text.split("\n").filter((l) => l.trim()).map((l) => JSON.parse(l));
      } catch (e) {
        // Try the next candidate.
      }
    }
    return [];
  }

  function value(r, key) {
    return typeof r[key] === "number" ? r[key] : null;
  }

  function worstPerCommit(records, key) {
    const byCommit = new Map();
    for (const r of records) {
      const v = value(r, key);
      if (v === null) continue;
      const cur = byCommit.get(r.commit);
      if (!cur || METRICS[key].worse(v, value(cur, key))) byCommit.set(r.commit, r);
    }
    return Array.from(byCommit.values()).sort((a, b) => a.commit_date.localeCompare(b.commit_date));
  }

  function baseline(records, stream, key, beforeDate) {
    const trail = records
      .filter((r) => r.leg === stream && r.commit_date < beforeDate && value(r, key) !== null)
      .sort((a, b) => a.commit_date.localeCompare(b.commit_date))
      .slice(-REGRESSION_WINDOW);
    if (!trail.length) return null;
    return trail.reduce((s, r) => s + value(r, key), 0) / trail.length;
  }

  // Only the soft tier is shown: a hard regression fails its own CI run.
  function flag(r, records) {
    const notes = [];
    const snr = value(r, "min_snr_db");
    const snrBase = baseline(records, r.leg, "min_snr_db", r.commit_date);
    if (snr !== null && snrBase !== null && snrBase - snr >= SNR_DROP_DB) {
      notes.push(`SNR ${(snrBase - snr).toFixed(2)} dB below the trailing mean`);
    }
    const lsd = value(r, "lsd_db");
    const lsdBase = baseline(records, r.leg, "lsd_db", r.commit_date);
    if (lsd !== null && lsdBase !== null && lsd - lsdBase >= LSD_RISE_DB) {
      notes.push(`LSD ${(lsd - lsdBase).toFixed(2)} dB above the trailing mean`);
    }
    return notes.length
      ? `<span class="quality-trend-regression" title="${notes.join("; ")}">▼ regression</span>`
      : "";
  }

  function buildChart(points, key) {
    if (!points.length) {
      return `<p class="quality-trend-status">No ${METRICS[key].label} history yet.</p>`;
    }
    const width = 760, height = 200, pad = { top: 12, right: 12, bottom: 32, left: 48 };
    const values = points.map((p) => value(p, key));
    const span = Math.max(...values) - Math.min(...values);
    const lo = Math.min(...values) - (span * 0.1 || 1);
    const hi = Math.max(...values) + (span * 0.1 || 1);
    const times = points.map((p) => Date.parse(p.commit_date));
    const minT = Math.min(...times);
    const maxT = Math.max(...times);
    const x = (t) => pad.left + (maxT === minT ? (width - pad.left - pad.right) / 2
      : ((t - minT) / (maxT - minT)) * (width - pad.left - pad.right));
    const y = (v) => height - pad.bottom - ((v - lo) / (hi - lo)) * (height - pad.top - pad.bottom);
    let svg = `<svg class="quality-trend-chart" viewBox="0 0 ${width} ${height}" width="${width}" height="${height}" role="img" aria-label="AC-4 ${METRICS[key].label}, worst stream per commit">`;
    for (let i = 0; i <= 4; i++) {
      const v = lo + ((hi - lo) * i) / 4;
      svg += `<line x1="${pad.left}" y1="${y(v)}" x2="${width - pad.right}" y2="${y(v)}" stroke="var(--md-default-fg-color--lightest)" stroke-width="1"/>`;
      svg += `<text x="${pad.left - 6}" y="${y(v) + 3}" text-anchor="end" font-size="10" fill="var(--md-default-fg-color--light)">${v.toFixed(1)}</text>`;
    }
    if (points.length > 1) {
      const path = points.map((p, i) =>
        `${i === 0 ? "M" : "L"}${x(Date.parse(p.commit_date)).toFixed(1)},${y(value(p, key)).toFixed(1)}`).join(" ");
      svg += `<path d="${path}" fill="none" stroke="${COLOR}" stroke-width="2"/>`;
    }
    for (const p of points) {
      svg += `<circle cx="${x(Date.parse(p.commit_date)).toFixed(1)}" cy="${y(value(p, key)).toFixed(1)}" r="3" fill="${COLOR}"><title>${p.commit.slice(0, 8)}: ${value(p, key).toFixed(2)}, ${p.leg}, on ${p.commit_date.slice(0, 10)}</title></circle>`;
    }
    return svg + "</svg>";
  }

  function cell(v) {
    return v === null ? "—" : v.toFixed(2);
  }

  function buildTable(records) {
    const rows = records.slice()
      .sort((a, b) => b.commit_date.localeCompare(a.commit_date) || a.leg.localeCompare(b.leg))
      .slice(0, TABLE_ROWS)
      .map((r) => `<tr>
        <td>${r.commit_date.slice(0, 10)}</td>
        <td><a href="https://github.com/${REPO}/commit/${r.commit}">${r.commit.slice(0, 8)}</a></td>
        <td>${r.leg}</td>
        <td>${cell(value(r, "min_snr_db"))}</td>
        <td>${cell(value(r, "lsd_db"))}</td>
        <td>${cell(value(r, "mos_lqo"))}</td>
        <td>${flag(r, records)}</td>
      </tr>`).join("");
    return `<div class="quality-trend-table-wrap"><table>
      <thead><tr><th>Date</th><th>Commit</th><th>Stream</th><th>Minimum SNR</th><th>LSD</th><th>MOS-LQO</th><th></th></tr></thead>
      <tbody>${rows}</tbody>
    </table></div>`;
  }

  function render(records) {
    const options = Object.entries(METRICS).map(([key, m]) =>
      `<option value="${key}" ${state.metric === key ? "selected" : ""}>${m.label}</option>`).join("");
    root.innerHTML = `
      <div class="quality-trend-controls">
        <label for="ac4-quality-metric">Measure <select id="ac4-quality-metric">${options}</select></label>
      </div>
      <div class="quality-trend-chart-wrap">${buildChart(worstPerCommit(records, state.metric), state.metric)}</div>
      <div class="quality-trend-legend"><span><i style="background:${COLOR}"></i>worst stream per commit</span></div>
      ${buildTable(records)}
    `;
    document.getElementById("ac4-quality-metric").addEventListener("change", (e) => {
      state.metric = e.target.value;
      render(records);
    });
  }

  fetchHistory().then((records) => {
    if (!records.length) {
      root.innerHTML = '<p class="quality-trend-status">No AC-4 decode quality history yet - the nightly run writes it.</p>';
      return;
    }
    render(records);
  });
})();
</script>

Each row is one DEE stream on one commit: the streams scored channel by channel against their
source, 10 of the 15 committed legs (SIMPLE and A-SPX at 2.0 and 5.1, and immersive stereo). The
A-CPL streams, scored through their downmixes, and the 5.1.4 streams, scored with their routing,
are held to their floors in FFmpeg Validate but have no series. **Minimum SNR** is the
stream's lowest full-band channel SNR against its source; the LFE is left out, since its SNR
against the full-band source is about −2.3 dB on every 5.1 stream and its own floor holds it.
**LSD** is the log-spectral distance and **MOS-LQO** ViSQOL's predicted listening score, all
from the same scoring pass. The chart plots the worst stream for each commit
on the chosen measure. A row is flagged when its SNR falls, or its LSD rises, 0.5 dB past that
stream's own trailing ten-run mean. A hard regression (3 dB of SNR, or 2 dB of LSD) fails the
`Publish quality trend` job after the numbers are pushed, as the AC-3 series does.
