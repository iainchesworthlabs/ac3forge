# Performance trend

Four separate mechanisms, not one, and it matters which is which:

- **The hard gate**: `ac3perf` (`tests/performance/test_performance.cpp`) asserts the
  encoder stays faster than real time (with a 2x safety margin), on every push and
  every PR. A failure here blocks CI outright - see
  [CI Status](https://github.com/iainchesworthlabs/ac3forge/blob/main/.github/workflows/ci.yml).
  Not run under the ASan/UBSan leg: instrumented code has nothing useful to say about
  throughput at any slack factor, so that leg excludes the `Performance` label entirely
  (`CMakePresets.json`'s `test-linux-llvm-asan-ubsan` preset).
- **This page's whole-frame tables**: `ac3bench` (`tests/performance/bench_encoder.cpp`)
  runs the same configurations for longer (200 frames) and records the actual
  ms/frame number, not just a pass/fail, on every push to `main`. It exists
  to answer a question the hard gate cannot: is throughput quietly drifting slower
  over time even while it keeps passing.
- **This page's per-kernel tables**: `ac3kernelbench`
  (`tests/performance/kernel_bench.cpp`) times each hot kernel in isolation (ns/call,
  fed real audio through the real windowing + forward MDCT) on the same pushes. It
  answers the question one level below `ac3bench`'s: when a whole-frame number
  drifts, *which stage* moved - without anyone having to reattach a profiler to find
  out.
- **This page's memory tables**: `ac3membench`
  (`tests/performance/bench_memory.cpp`) counts what the others time: heap
  allocations and allocator traffic per frame, live-byte drift, and peak RSS,
  across the same encoder configurations *plus* the decode paths the timing
  benches never covered. It records the memory-usage programme's progress the
  same way the whole-frame series recorded the CPU programme's, and - unlike
  ms/frame - its numbers are near-deterministic for a fixed workload, so a
  flagged row is a real behavioural change, not runner noise.

All of this exists because a severe encoder regression (a per-call recomputation the
forward MDCT should have cached) once shipped with no coverage to catch it: the hard
gate blocks a repeat outright, and the trend tables catch the gradual drift a
pass/fail gate cannot see.

## What is measured, and on what

Both timing producers and the gate cover the same nine workloads: three encoders
(`plain_51` and `plain_51_fast_mdct`, `eac3_51_auto` and `eac3_stereo_auto`,
`atmos_4obj` and `atmos_4obj_fast_mdct`) and the three decoders that read what they
produce (`ac3_51_decode`, `eac3_51_decode`, `atmos_4obj_decode`). Until roadmap PF1
the E-AC-3 encoder — the largest source file in the codec — and every decode path had
no ms/frame number and no real-time gate anywhere, so a regression in any of them was
invisible here. The decode series are timed against streams the encode series in the
same run just produced: a decode number only means something against a stream whose
rate and tool set are known.

Every workload is fed real programme material (`tests/golden/audio/reference_51.wav`,
through `tests/performance/real_audio.hpp`), not the 440 Hz tone `ac3bench` and
`ac3perf` ran on before PF1. A single stationary tone is not a cheaper version of
programme material, it is a different workload: its spectrum is one bin wide, so the
SNR-offset search converges against an allocation almost nothing competes for,
coupling has near-nothing to share between channels, rematrixing sees a pair that is
already identical, and the transient detector never fires — so the block-switched
transform never runs at all. A regression confined to any of those could not move the
number. `ac3kernelbench` had this rule from the start; PF1 applied it to the other
two. The fixture is 78 frames long and the benches run 200, so frame indices wrap;
the seam that creates lands in the same place on every run.

Only `linux-gcc` is measured, not the full CI matrix — see the note below the append
scripts for why. Every number on this page is that one runner's; nothing here is a
cross-platform comparison, and a number from a developer machine is not comparable
to one of these rows.

Roadmap PF2 (inlining `to_fixed25` and fusing it with exponent extraction) does not
show as a clean step in the whole-frame series above: the ~7% of a fast-path frame it
targeted is smaller than this page's own run-to-run variance on a shared runner, so
the effect is real but not visible against that noise floor in a 200-frame series.
Isolated from the rest of the frame - the exact per-bin loop, `~9,100` conversions
sized like one real 5.1 frame's worth, minimum of several runs - it measured
~50 µs before and ~30 µs after per frame's worth of calls (a 1.5-1.8× speedup on that
slice), consistent with the ~33-38 µs the change targeted. The gate that actually
matters for this change is byte-identical output, not a timing number: see the PF2
commit message for the corpus that was checked.

`tools/ci/append_performance_history.py` appends every `main` run's numbers
to the `quality-history` branch (reused, not a new branch - the same reasoning
[Quality trend](quality-trend.md) already gives for a dedicated branch over
`gh-pages`: incremental, no publish-cadence coupling, fetchable client-side with no
auth). On top of the hard gate's absolute 32ms budget, it applies a trailing-baseline
check: a soft one (20% slower than the trailing 10-run mean, `::warning::` only) and
a hard one (100% slower - i.e. at least doubled - `::error::`, fails the
`persist-performance-trend` CI job *after* the numbers are still recorded, so a big
regression is never silently un-recorded just because it also failed the run).

Both append scripts key every series by its own name end to end, so a workload or
kernel added to a bench simply starts its own series: its first run has no trailing
mean to be compared against and cannot trip its own regression gate, and it never
perturbs an existing series' baseline. That is also why an existing series' name is
not reused for a differently-shaped measurement — a trailing mean over two different
workloads is a number with no owner.

`tools/ci/append_kernel_history.py` does the same for `ac3kernelbench`'s per-kernel
numbers (`kernels-develop.jsonl` / `kernels-main.jsonl`, same branch), with the same
two trailing-baseline tiers - but both tiers are `::warning::` annotations and the
kernel series **never fails the job**: a micro-kernel's ns/call on a shared CI runner
is far noisier than a 200-frame whole-frame average, and `ac3perf` plus the
whole-frame series already gate anything a user would feel. This page's per-kernel
tables are where kernel regressions surface. Every series is keyed by its own kernel
name end to end - a trailing mean over mixed kernels would be a number with no owner,
the same conflation the quality-trend history once had to be cured of.

`tools/ci/append_memory_history.py` does the same for `ac3membench`'s numbers
(`memory-develop.jsonl` / `memory-main.jsonl`, same branch), with the same two
tiers on **two** churn metrics per series - allocations/frame and bytes/frame,
either one regressing flags the record - and it gates like the whole-frame
series does (the hard tier fails the job, after the push). One check is absolute
rather than trend-relative: `steady_live_growth`, the bytes still held live
after ~200 steady-state frames, warns above 4 KiB and hard-fails above 1 MiB,
because a leak is a leak regardless of what last week's runs did.

Only `linux-gcc` is measured, not the full CI matrix: a timing trend's value is in
comparing one consistent runner against its own history over time, not in comparing
GitHub's runner classes against each other the way the gold-reference SNR numbers
usefully are cross-platform.

`performance-develop.jsonl`, `kernels-develop.jsonl` and `memory-develop.jsonl`
all stopped gaining rows on 2026-08-24, `develop`'s last commit before
2026-08-25's move to trunk-based development retired the branch - see
[Quality trend](quality-trend.md#where-the-data-lives) for the detail, which
applies here identically. They are kept as-is, not deleted or merged into
`main`'s own files; each section below renders `main`'s ongoing history
directly and folds `develop`'s frozen one into a collapsed *Show historical
data* block beneath it instead.

## Whole-frame trend

Each series below is a chart first, table second. The chart plots that
series' *entire* recorded history (not just the table's last 20 rows) as
ms/frame against commit date, with a dashed red line for the throughput
budget and a dashed ring around any point whose commit was tagged as a
GitHub release - hover a point for the exact commit, date and number. A
downward slope is the improvement this whole page exists to make visible;
release rings turn that into a release-to-release story instead of a wall
of per-commit numbers. `main`'s chart and table render directly, in solid
colour; `develop`'s frozen history sits inside the collapsed *Show
historical data (develop, pre-2026-08-25 GitFlow era)* block below each
series, drawn dashed and muted when opened so it reads as archived rather
than as a second live series - the per-kernel and memory sections below
follow the same convention, table-only (no chart to style).

<div id="performance-trend-app">
  <p class="performance-trend-status">Loading trend data…</p>
</div>

<style>
#performance-trend-app { margin: 1.5em 0; }
.performance-trend-status { color: var(--md-default-fg-color--light); font-style: italic; }
.performance-trend-chart-wrap { overflow-x: auto; margin-bottom: 0.5em; }
.performance-trend-chart { display: block; }
.performance-trend-legend { display: flex; gap: 1.5em; font-size: 0.8em; margin: 0.25em 0 1em; color: var(--md-default-fg-color--light); flex-wrap: wrap; }
.performance-trend-legend span { display: inline-flex; align-items: center; gap: 0.4em; }
.performance-trend-legend i { width: 0.9em; height: 0.9em; border-radius: 50%; display: inline-block; background: #7c4dff; }
.performance-trend-table-wrap { overflow-x: auto; }
#performance-trend-app table { width: 100%; border-collapse: collapse; font-size: 0.85em; }
#performance-trend-app th, #performance-trend-app td { padding: 0.35em 0.6em; text-align: left; border-bottom: 1px solid var(--md-default-fg-color--lightest); white-space: nowrap; }
.performance-trend-over-budget { color: var(--md-typeset-mark-color, #c62828); font-weight: 600; }
.performance-trend-release-row { background: color-mix(in srgb, var(--md-accent-fg-color, #7c4dff) 8%, transparent); }
.performance-trend-release { text-decoration: none; font-weight: 600; }
.performance-trend-release:hover { text-decoration: underline; }
.performance-trend-historical { margin-top: 1.5em; border-top: 1px solid var(--md-default-fg-color--lightest); padding-top: 0.75em; }
.performance-trend-historical summary { cursor: pointer; font-weight: 600; color: var(--md-default-fg-color--light); }
.performance-trend-historical summary:hover { color: var(--md-default-fg-color); }
</style>

<script>
  // Shared by all three sections below (whole-frame, per-kernel, memory) so
  // the fetch/parse plumbing and the main-vs-historical split live in one
  // place instead of three near-identical copies - the three IIFEs used to
  // redefine rawUrl/parseJsonl/shortSha/groupBy identically. Deliberately
  // scoped to this one page, not a separate docs/javascripts asset: every
  // trend page here is self-contained by design (see the top of this repo's
  // docs/CONTRIBUTING notes on the CI-trend pages), and these three sections
  // are the only ones on the same page repeating each other - the sibling
  // pages' own near-duplication is across separate documents, not something
  // a page-local script can reach anyway.
  const PERF_TREND_REPO = "iainchesworthlabs/ac3forge";
  const PERF_TREND_HISTORY_BRANCH = "quality-history";
  const PERF_TREND_MAIN_COLOR = "#7c4dff";
  // Muted and dashed (see each section's buildChart) rather than a second
  // saturated colour - the historical track reads as archived, not as a
  // second live series. Matches the sibling trend pages' identical choice.
  const PERF_TREND_HISTORICAL_COLOR = "#9e9e9e";

  function perfTrendRawUrl(branch, file) {
    return `https://raw.githubusercontent.com/${PERF_TREND_REPO}/${branch}/${file}`;
  }

  function perfTrendParseJsonl(text) {
    return text.split("\n").filter((l) => l.trim().length > 0).map((l) => JSON.parse(l));
  }

  function perfTrendShortSha(sha) {
    return (sha || "").slice(0, 7);
  }

  function perfTrendGroupBy(records, keyFn) {
    const groups = new Map();
    for (const rec of records) {
      const key = keyFn(rec);
      if (!groups.has(key)) groups.set(key, []);
      groups.get(key).push(rec);
    }
    return groups;
  }

  // Renders `sectionRenderer`'s output for main's records directly - no
  // heading, since main is the only ongoing track and this page no longer
  // presents it as one of two choices - then, only if this fetch actually
  // has develop rows, renders develop's output again inside a collapsed
  // <details>, explicitly labelled as frozen pre-2026-08-25 GitFlow-era
  // history rather than an ongoing parallel branch. `sectionRenderer`
  // receives (records, isHistorical); the whole-frame section uses
  // isHistorical to pick a chart colour, the kernel and memory sections
  // (table-only, no chart) ignore it.
  function perfTrendRenderMainAndHistorical(root, allRecords, sectionRenderer, noDataMessage) {
    const mainRecords = allRecords.filter((r) => r.branch === "main");
    const historicalRecords = allRecords.filter((r) => r.branch === "develop");
    if (mainRecords.length === 0 && historicalRecords.length === 0) {
      root.innerHTML = noDataMessage;
      return;
    }
    const mainHtml = mainRecords.length ? sectionRenderer(mainRecords, false) : "<p><em>No data yet on main.</em></p>";
    const historicalHtml = historicalRecords.length
      ? `<details class="performance-trend-historical"><summary>Show historical data (develop, pre-2026-08-25 GitFlow era)</summary>${sectionRenderer(historicalRecords, true)}</details>`
      : "";
    root.innerHTML = mainHtml + historicalHtml;
  }
</script>

<script>
(function () {
  const REPO = PERF_TREND_REPO;
  // How many of each (leg, config) series' most recent rows to show in the
  // table - a trend readout, not a full audit log. Mirrors quality-trend.md's
  // own TABLE_ROWS in spirit, just scoped per-series instead of globally,
  // since performance-trend.md only ever has one leg (linux-gcc) rather than
  // quality-trend's five. The chart above each table is NOT capped to this -
  // it plots the series' full history so a release from further back than
  // the last 20 runs still shows up as a ring.
  const ROWS_PER_SERIES = 20;

  const root = document.getElementById("performance-trend-app");

  async function fetchBranch(branch) {
    try {
      const resp = await fetch(perfTrendRawUrl(PERF_TREND_HISTORY_BRANCH, `performance-${branch}.jsonl`));
      if (!resp.ok) return [];
      return perfTrendParseJsonl(await resp.text());
    } catch (e) {
      return [];
    }
  }

  // Same best-effort tag->commit join quality-trend.md already relies on:
  // release.yml tags an existing main commit after the fact, so the tag
  // never appears in the performance-history record for that commit itself -
  // this is a client-side join against the GitHub API, not a second data
  // source. A rate-limited or offline response just means no release
  // markers, not a broken page.
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

  // sortedRows: full series history, oldest to newest. Budget is read off
  // the most recent row, same as the table below - a config's budget
  // doesn't change often, but this always reflects the current one, not a
  // stale first-run value. color/dashed pick main's or the historical
  // track's styling - see perfTrendRenderMainAndHistorical above.
  function buildChart(sortedRows, releasesBySha, opts) {
    const { color = PERF_TREND_MAIN_COLOR, dashed = false } = opts || {};
    const width = 720, height = 160, pad = { top: 10, right: 12, bottom: 22, left: 46 };
    const budget = sortedRows.length ? sortedRows[sortedRows.length - 1].real_time_budget_ms_per_frame : null;
    const values = sortedRows.map((r) => r.ms_per_frame).concat(budget !== null ? [budget] : []);
    const minMs = Math.min(...values) * 0.95;
    const maxMs = Math.max(...values) * 1.05;
    const times = sortedRows.map((r) => Date.parse(r.commit_date));
    const minT = Math.min(...times);
    const maxT = Math.max(...times);

    const x = (t) => pad.left + (maxT === minT ? (width - pad.left - pad.right) / 2 : ((t - minT) / (maxT - minT)) * (width - pad.left - pad.right));
    const y = (ms) => height - pad.bottom - ((ms - minMs) / (maxMs - minMs)) * (height - pad.top - pad.bottom);

    let svg = `<svg class="performance-trend-chart" viewBox="0 0 ${width} ${height}" width="${width}" height="${height}" role="img" aria-label="ms per frame by commit date">`;
    for (let i = 0; i <= 3; i++) {
      const ms = minMs + ((maxMs - minMs) * i) / 3;
      const gy = y(ms);
      svg += `<line x1="${pad.left}" y1="${gy}" x2="${width - pad.right}" y2="${gy}" stroke="var(--md-default-fg-color--lightest)" stroke-width="1"/>`;
      svg += `<text x="${pad.left - 6}" y="${gy + 3}" text-anchor="end" font-size="10" fill="var(--md-default-fg-color--light)">${ms.toFixed(2)}</text>`;
    }
    if (budget !== null) {
      const by = y(budget);
      svg += `<line x1="${pad.left}" y1="${by}" x2="${width - pad.right}" y2="${by}" stroke="var(--md-typeset-mark-color, #c62828)" stroke-width="1" stroke-dasharray="4,3"><title>Budget: ${budget.toFixed(3)} ms/frame</title></line>`;
    }
    svg += `<text x="${pad.left}" y="${height - 6}" text-anchor="start" font-size="10" fill="var(--md-default-fg-color--light)">${new Date(minT).toISOString().slice(0, 10)}</text>`;
    svg += `<text x="${width - pad.right}" y="${height - 6}" text-anchor="end" font-size="10" fill="var(--md-default-fg-color--light)">${new Date(maxT).toISOString().slice(0, 10)}</text>`;

    if (sortedRows.length > 1) {
      const path = sortedRows.map((r, i) => `${i === 0 ? "M" : "L"}${x(Date.parse(r.commit_date)).toFixed(1)},${y(r.ms_per_frame).toFixed(1)}`).join(" ");
      const dash = dashed ? ' stroke-dasharray="5,3"' : "";
      svg += `<path d="${path}" fill="none" stroke="${color}" stroke-width="2"${dash}/>`;
    }
    sortedRows.forEach((r) => {
      const cx = x(Date.parse(r.commit_date)).toFixed(1);
      const cy = y(r.ms_per_frame).toFixed(1);
      const release = releasesBySha[r.commit];
      const title = `${perfTrendShortSha(r.commit)} - ${r.ms_per_frame.toFixed(3)} ms/frame on ${r.commit_date.slice(0, 10)}${release ? ` - release ${release.name}` : ""}`;
      svg += `<circle cx="${cx}" cy="${cy}" r="3" fill="${color}"><title>${title}</title></circle>`;
      if (release) {
        svg += `<circle cx="${cx}" cy="${cy}" r="6.5" fill="none" stroke="${color}" stroke-width="1.5" stroke-dasharray="2,1.5"><title>${title}</title></circle>`;
      }
    });
    svg += "</svg>";
    return svg;
  }

  function buildLegend(sortedRows, releasesBySha, color) {
    const anyRelease = sortedRows.some((r) => releasesBySha[r.commit]);
    const items = [
      `<span><i style="background:${color}"></i>ms/frame</span>`,
      '<span><i style="background:none;border:1.5px dashed var(--md-typeset-mark-color, #c62828);border-radius:0;"></i>budget</span>',
    ];
    if (anyRelease) {
      items.push('<span><i style="background:none;border:1.5px dashed var(--md-default-fg-color--light);"></i>tagged release</span>');
    }
    return `<div class="performance-trend-legend">${items.join("")}</div>`;
  }

  function renderSeries(leg, config, rows, releasesBySha, isHistorical) {
    const sorted = rows.slice().sort((a, b) => a.commit_date.localeCompare(b.commit_date));
    const recent = sorted.slice(-ROWS_PER_SERIES).reverse();
    const budget = sorted.length ? sorted[sorted.length - 1].real_time_budget_ms_per_frame : null;
    const color = isHistorical ? PERF_TREND_HISTORICAL_COLOR : PERF_TREND_MAIN_COLOR;
    const trs = recent.map((r) => {
      const overBudget = budget !== null && r.ms_per_frame > budget;
      const release = releasesBySha[r.commit];
      const classes = [overBudget ? "performance-trend-over-budget" : "", release ? "performance-trend-release-row" : ""].filter(Boolean).join(" ");
      const releaseBadge = release
        ? `<a class="performance-trend-release" href="${release.url}" title="${release.prerelease ? "Prerelease" : "Release"} tagged at this commit">🏷 ${release.name}</a>`
        : "";
      return `<tr${classes ? ` class="${classes}"` : ""}>
        <td>${r.commit_date ? r.commit_date.slice(0, 10) : ""}</td>
        <td><a href="https://github.com/${REPO}/commit/${r.commit}">${perfTrendShortSha(r.commit)}</a></td>
        <td>${r.ms_per_frame.toFixed(3)}</td>
        <td>${budget !== null ? budget.toFixed(3) : ""}</td>
        <td>${r.frames}</td>
        <td>${releaseBadge}</td>
      </tr>`;
    }).join("");

    return `<h4>${leg} / ${config}</h4>
    <div class="performance-trend-chart-wrap">${buildChart(sorted, releasesBySha, { color, dashed: isHistorical })}</div>
    ${buildLegend(sorted, releasesBySha, color)}
    <div class="performance-trend-table-wrap">
      <table>
        <thead><tr><th>Date</th><th>Commit</th><th>ms/frame</th><th>Budget (ms/frame)</th><th>Frames</th><th>Release</th></tr></thead>
        <tbody>${trs}</tbody>
      </table>
    </div>`;
  }

  async function render() {
    const [mainRecords, developRecords, releasesBySha] = await Promise.all([
      fetchBranch("main"),
      fetchBranch("develop"),
      fetchReleaseShaMap(),
    ]);
    const allRecords = [...mainRecords, ...developRecords];
    const sectionRenderer = (records, isHistorical) => {
      const groups = perfTrendGroupBy(records, (r) => `${r.leg} ${r.config}`);
      return [...groups.entries()]
        .sort((a, b) => a[0].localeCompare(b[0]))
        .map(([key, rows]) => {
          const [leg, config] = key.split(" ");
          return renderSeries(leg, config, rows, releasesBySha, isHistorical);
        })
        .join("\n");
    };
    perfTrendRenderMainAndHistorical(root, allRecords, sectionRenderer,
      '<p class="performance-trend-status">No performance-trend data recorded yet - it appears after the first main push that reaches the persist-performance-trend CI job.</p>');
  }

  render();
})();
</script>

## Per-kernel trend

Same commits, one level finer: each kernel's ns/call from `ac3kernelbench`, one
series per kernel. Both directions of both block sizes are covered in both their
direct and fast forms — `mdct512_forward`/`_fast`, `mdct256_pair`/`_fast`,
`imdct512_windowed`/`_fast`, `imdct256_pair`/`_fast` — so the ratio between a pair
is what `mode=reference` costs, and the fast inverse that became the decoder's
default in 0.9.0 has a series of its own rather than being tracked through the
direct form no decoder runs any more. The Δ column is each run against its own series' trailing
10-run mean - the same window and thresholds `append_kernel_history.py` annotates
with: ≥ +20% is flagged as a soft drift, ≥ +100% as a hard one. Neither ever fails
CI (see above); a flagged row here is an invitation to look, not a broken build.

Several kernels appear twice, as a `<name>` / `<name>_fast` pair: the transforms
exist in two evaluations (the spec's own direct form and an accelerated one - see
[Verification](verification.md#performance-and-reference-modes)), and both are
recorded, because the reference form is maintained code that a `mode=reference` run
actually executes, not dead weight. The `_fast` row is what a default encode or
decode spends; the bare row is what the oracle costs.

<div id="kernel-trend-app">
  <p class="performance-trend-status">Loading kernel trend data…</p>
</div>

<style>
#kernel-trend-app { margin: 1.5em 0; }
#kernel-trend-app table { width: 100%; border-collapse: collapse; font-size: 0.85em; }
#kernel-trend-app th, #kernel-trend-app td { padding: 0.35em 0.6em; text-align: left; border-bottom: 1px solid var(--md-default-fg-color--lightest); white-space: nowrap; }
.kernel-trend-soft { color: var(--md-warning-fg-color, #e65100); font-weight: 600; }
.kernel-trend-hard { color: var(--md-typeset-mark-color, #c62828); font-weight: 600; }
</style>

<script>
(function () {
  const REPO = PERF_TREND_REPO;
  // Fewer rows per series than the whole-frame tables' 20: there are a dozen-plus
  // kernel series to the whole-frame tables' handful of configs, and this page is
  // a trend readout, not an audit log - the JSONL keeps everything.
  const ROWS_PER_SERIES = 10;
  // Mirrors append_kernel_history.py's REGRESSION_TRAILING_WINDOW and its two
  // annotation tiers, so a flagged row here and a ::warning:: in the CI log are
  // the same statement about the same numbers.
  const TRAILING_WINDOW = 10;
  const SOFT_FRACTION = 0.20;
  const HARD_FRACTION = 1.0;

  const root = document.getElementById("kernel-trend-app");

  async function fetchBranch(branch) {
    try {
      const resp = await fetch(perfTrendRawUrl(PERF_TREND_HISTORY_BRANCH, `kernels-${branch}.jsonl`));
      if (!resp.ok) return [];
      return perfTrendParseJsonl(await resp.text());
    } catch (e) {
      return [];
    }
  }

  function formatNs(ns) {
    return ns >= 100 ? ns.toFixed(0) : ns.toFixed(1);
  }

  function renderSeries(leg, kernel, rows) {
    // Each row's baseline is the trailing mean of the rows BEFORE it - the
    // same the-append-script-saw-it semantics as the CI annotations, which
    // compute the baseline before appending the new record.
    const annotated = rows.map((r, i) => {
      const tail = rows.slice(Math.max(0, i - TRAILING_WINDOW), i).map((p) => p.ns_per_call);
      const baseline = tail.length ? tail.reduce((a, b) => a + b, 0) / tail.length : null;
      const slowdown = baseline && baseline > 0 ? (r.ns_per_call - baseline) / baseline : null;
      return { ...r, slowdown };
    });
    const recent = annotated.slice(-ROWS_PER_SERIES).reverse();
    const trs = recent.map((r) => {
      let cls = "";
      if (r.slowdown !== null && r.slowdown >= HARD_FRACTION) cls = ' class="kernel-trend-hard"';
      else if (r.slowdown !== null && r.slowdown >= SOFT_FRACTION) cls = ' class="kernel-trend-soft"';
      const delta = r.slowdown === null ? "" : `${r.slowdown >= 0 ? "+" : ""}${(r.slowdown * 100).toFixed(1)}%`;
      return `<tr${cls}>
        <td>${r.commit_date ? r.commit_date.slice(0, 10) : ""}</td>
        <td><a href="https://github.com/${REPO}/commit/${r.commit}">${perfTrendShortSha(r.commit)}</a></td>
        <td>${formatNs(r.ns_per_call)}</td>
        <td>${delta}</td>
        <td>${r.iters}</td>
      </tr>`;
    }).join("");

    return `<h4>${leg} / ${kernel}</h4>
    <div class="performance-trend-table-wrap">
      <table>
        <thead><tr><th>Date</th><th>Commit</th><th>ns/call</th><th>Δ vs trailing mean</th><th>Iters</th></tr></thead>
        <tbody>${trs}</tbody>
      </table>
    </div>`;
  }

  async function render() {
    const [mainRecords, developRecords] = await Promise.all([fetchBranch("main"), fetchBranch("develop")]);
    const allRecords = [...mainRecords, ...developRecords];
    // Grouped per (leg, kernel), never merged across kernels - a series is
    // only meaningful against its own history.
    const sectionRenderer = (records) => {
      const groups = perfTrendGroupBy(records, (r) => `${r.leg} ${r.kernel}`);
      return [...groups.entries()]
        .sort((a, b) => a[0].localeCompare(b[0]))
        .map(([key, rows]) => {
          const [leg, kernel] = key.split(" ");
          return renderSeries(leg, kernel, rows);
        })
        .join("\n");
    };
    perfTrendRenderMainAndHistorical(root, allRecords, sectionRenderer,
      '<p class="performance-trend-status">No kernel-trend data recorded yet - it appears after the first main push that reaches the persist-performance-trend CI job.</p>');
  }

  render();
})();
</script>

## Memory trend

Same commits, a different resource: each workload's heap-allocation count and
allocator traffic per frame from `ac3membench`, one series per workload -
including the decode paths the timing benches don't cover. The Δ column is
bytes/frame against the series' trailing 10-run mean, the same window and
thresholds `append_memory_history.py` gates with (≥ +20% soft, ≥ +100% hard on
*either* churn metric); a non-zero **live growth** is its own signal (bytes
still held after ~200 steady-state frames - the leak check is absolute, not
trend-relative). These counts are near-deterministic for a fixed workload: a
flagged row is a real change in allocation behaviour, not runner noise. The
memory-usage optimization programme's phases land as visible downward steps in
these series - that is what this table exists to show.

Two landed programmes are the biggest steps in these series. The 2026-08
memory-usage programme cut steady-state allocator traffic per frame by 85-88%
on the encode series (on the measured `linux-gcc` runner: AC-3 encode
225,028 → 26,778 bytes/frame and 286 → 86 allocations; E-AC-3
214,808 → 28,792 and 157 → 67; Atmos 218,960 → 32,656 and 196 → 106) and
54-61% on the decode series - and, outside these tables, took every
output-producing CLI command memory-flat at any programme length (a 3-minute
5.1 encode peaked at 437.8 MiB before the programme and 9.3 MiB after;
decode 217 → 28.5 MiB, `spdif` 225.7 → 18.0 MiB).

The fast-IMDCT rollout that followed
([Validation → Performance and reference modes](verification.md#performance-and-reference-modes))
lands in the decode series as two distinct marks. The flat-substream-state
change shows directly: the decode workloads' setup allocations dropped from
4 allocations / 47,606 bytes to exactly zero. The transform change itself
mostly does not show in heap columns, and knowing why matters for reading
the table: the direct evaluation's 320 KiB of step-3 matrices are lazily
built *static* storage, so switching the default to the FFT path removes
them from the process (a 3-minute CLI decode's peak working set drops
~0.2-0.3 MiB) without moving an allocation count. Its real payoff is time,
which the timing series on this page did not cover when it landed (they timed
encode only; roadmap PF1 added the three decode series after the fact):
measured 180-second decodes went from 3.53 s to 0.79 s (AC-3) and 3.49 s to
0.75 s (E-AC-3) when the fast path became the default - `mode=reference`
runs the old numbers on purpose.

## Minimum-footprint decoder

Roadmap PF7. Not a trend series — one measured configuration, on the concrete target the
roadmap names: `arm-none-eabi` cross-compiled for QEMU's `mps2-an385` machine (Cortex-M3,
soft float, no OS), `AC3FORGE_MINIMAL_DECODER=ON`, `CMAKE_BUILD_TYPE=MinSizeRel`. See
[Building → Minimum-footprint decoder profile](building.md#minimum-footprint-decoder-profile)
for what the profile changes and why.

`apps/baremetal/probe.cpp` decodes six frames each of real 5.1 AC-3 (448 kbit/s, coupling) and
E-AC-3 (384 kbit/s, AHT + spx + coupling) and reports what it cost. Numbers below are from the
run in [this PR](https://github.com/iainchesworthlabs/ac3forge); `build-footprint` in
`.github/workflows/_build.yml` reproduces them on every push, and
`tools/checks/run_baremetal_probe.sh` reproduces them locally.

The table below was first measured early in PF6/PF7's own feature branch (PR #351). Several
`develop` merges landed on that branch afterwards but before it merged to `main` — most
significantly DC10's QMF-domain JOC reconstruction, which the decode path genuinely needs
(`src/dsp/qmf.cpp` and `src/verify/eac3_mirror.cpp`, both correctly added to
`src/forge/minimal.cmake`'s source list at the time, per that merge's own commit message), plus
the PF3/PF4 FFT/IMDCT rewrite and DC1's decoder output stage — and nobody re-measured the table
or the ceiling before merging. The image had already reached 412,516 bytes by then; the numbers
below are that re-measurement, re-based against the current `main`.

### Static footprint

| | Bytes |
|---|---|
| `.text` (code + read-only data) | 174,524 |
| `.data` (initialised) | 400 |
| `.bss` (zero-initialised) | 237,592 |
| **Image total** | **412,516** (402.8 KiB) |

Where it went, objects over 2 KiB (see `tools/checks/footprint_report.py --map` for the full
attribution from the linker map):

| Object | `.text` | `.bss` |
|---|---|---|
| `probe.cpp.obj` (the harness itself — fixture, checks, allocator hooks) | 23.4 KiB | 96.1 KiB |
| `tls.cpp.obj` (the single-thread TLS block — see below) | 8 B | 64.0 KiB |
| `eac3_tools.cpp.obj` (spx/ecpl band geometry + §3.5.5 reconstruction) | 21.3 KiB | 42.3 KiB |
| `eac3_decoder.cpp.obj` (all of Annex E) | 50.5 KiB | 0 |
| `decoder.cpp.obj` (AC-3) | 17.3 KiB | 0 |
| `mdct.cpp.obj` (inverse transform, fast path only) | 15.2 KiB | 12.4 KiB |
| `qmf.cpp.obj` (DC10's QMF-domain JOC reconstruction — new since 354,060) | 8.9 KiB | 4.2 KiB |
| `eac3_mirror.cpp.obj` (E-AC-3 decode-side trace, DecoderConfig::syntax — new since 354,060) | 8.5 KiB | 0 |
| everything else, summed | 87.7 KiB | 12.9 KiB |

`tls.cpp.obj`'s 64 KiB is the single-thread `__aeabi_read_tp` stub's static block
(`apps/baremetal/platform/baremetal/tls.cpp`) — oversized on purpose so ordinary growth in
`ecpl_channel_spectrum`'s `thread_local` scratch does not need it revisited, and checked by two
`ASSERT()`s in the linker script rather than trusted.

### Table ROM budget

The reason PF7 asks for this figure by name: the direct-form transform tables `mode=reference`
needs are **absent from this image entirely**, not merely unused. Measured on the object file
with `dumpbin /HEADERS` (Windows) — the actual `.bss` reservation, not an estimate:

| Table | Bytes | Only needed by |
|---|---|---|
| `ForwardCosTable<512>` | 1,048,576 | Direct-form forward MDCT, long — **encode**, not on this decoder's path at all |
| `ForwardCosTable<256>` × 2 | 524,288 | Direct-form forward MDCT, short — **encode** |
| `InnerSumTable` | 262,144 | Direct-form inverse, long — decode, `mode=reference` only |
| `InnerSumPairTable` | 65,536 | Direct-form inverse, short — decode, `mode=reference` only |
| **Total excluded** | **1,900,544** (1.81 MiB) | |
| *Fast-path tables actually linked in* | *~12,600* | |

A build asking for `mode=reference` in this profile gets `DecodeError::kUnsupported` rather than
a silent fast-path substitution — see the building doc for why.

### Runtime footprint

| | Value |
|---|---|
| Peak heap | 243,470 bytes (237.8 KiB) |
| Leaked at exit | 0 |
| `sizeof(ac3::FrameDecoder)` | 12,952 bytes |
| `sizeof(ac3::Eac3Decoder)` | 27,408 bytes |
| Caller-owned PCM buffer (16 × 1536 `float`, via `decode_*_into`) | 98,304 bytes |
| AC-3 allocations per frame, steady state | 45 |
| E-AC-3 allocations per frame, steady state | 87 |

The steady-state allocation counts are the gap [Building](building.md#gaps) records: PF7 asks
for zero, and this is 45/87 — from the per-block geometry vectors inside the decoders and the
`std::vector` members of the returned `DecodedFrame`/`DecodedSubstream`, none of which the
memory programme's [`_into` forms](#whole-frame-trend) removed because they are inherent to
those two return types, not to allocation *reuse*. Reaching zero means those becoming
fixed-capacity, a public-type change tracked separately from this profile.

`tools/checks/run_baremetal_probe.sh` gates the image, the heap peak and both allocation counts
at ceilings above these measured values, so a regression stops the build instead of drifting
the table silently.

<div id="memory-trend-app">
  <p class="performance-trend-status">Loading memory trend data…</p>
</div>

<style>
#memory-trend-app { margin: 1.5em 0; }
#memory-trend-app table { width: 100%; border-collapse: collapse; font-size: 0.85em; }
#memory-trend-app th, #memory-trend-app td { padding: 0.35em 0.6em; text-align: left; border-bottom: 1px solid var(--md-default-fg-color--lightest); white-space: nowrap; }
</style>

<script>
(function () {
  const REPO = PERF_TREND_REPO;
  const ROWS_PER_SERIES = 10;
  // Mirrors append_memory_history.py's window and tiers, so a flagged row
  // here and an annotation in the CI log are the same statement about the
  // same numbers. The flag is on the WORSE of the two churn metrics.
  const TRAILING_WINDOW = 10;
  const SOFT_FRACTION = 0.20;
  const HARD_FRACTION = 1.0;

  const root = document.getElementById("memory-trend-app");

  async function fetchBranch(branch) {
    try {
      const resp = await fetch(perfTrendRawUrl(PERF_TREND_HISTORY_BRANCH, `memory-${branch}.jsonl`));
      if (!resp.ok) return [];
      return perfTrendParseJsonl(await resp.text());
    } catch (e) {
      return [];
    }
  }

  function formatBytes(b) {
    if (b >= 1024 * 1024) return `${(b / (1024 * 1024)).toFixed(1)} MiB`;
    if (b >= 1024) return `${(b / 1024).toFixed(1)} KiB`;
    return `${Math.round(b)} B`;
  }

  function metricGrowth(rows, i, metric) {
    const tail = rows.slice(Math.max(0, i - TRAILING_WINDOW), i).map((p) => p[metric]);
    const baseline = tail.length ? tail.reduce((a, b) => a + b, 0) / tail.length : null;
    if (baseline === null || baseline <= 0) return null;
    return (rows[i][metric] - baseline) / baseline;
  }

  function renderSeries(leg, config, rows) {
    // Same the-append-script-saw-it semantics as the kernel tables: each
    // row's baseline is the trailing mean of the rows BEFORE it.
    const annotated = rows.map((r, i) => {
      const allocsGrowth = metricGrowth(rows, i, "allocs_per_frame");
      const bytesGrowth = metricGrowth(rows, i, "bytes_per_frame");
      const worst = [allocsGrowth, bytesGrowth].filter((g) => g !== null)
        .reduce((a, b) => Math.max(a, b), -Infinity);
      return { ...r, bytesGrowth, worst: worst === -Infinity ? null : worst };
    });
    const recent = annotated.slice(-ROWS_PER_SERIES).reverse();
    const trs = recent.map((r) => {
      let cls = "";
      if (r.worst !== null && r.worst >= HARD_FRACTION) cls = ' class="kernel-trend-hard"';
      else if (r.worst !== null && r.worst >= SOFT_FRACTION) cls = ' class="kernel-trend-soft"';
      const delta = r.bytesGrowth === null ? "" : `${r.bytesGrowth >= 0 ? "+" : ""}${(r.bytesGrowth * 100).toFixed(1)}%`;
      return `<tr${cls}>
        <td>${r.commit_date ? r.commit_date.slice(0, 10) : ""}</td>
        <td><a href="https://github.com/${REPO}/commit/${r.commit}">${perfTrendShortSha(r.commit)}</a></td>
        <td>${r.allocs_per_frame.toFixed(1)}</td>
        <td>${formatBytes(r.bytes_per_frame)}</td>
        <td>${r.steady_live_growth === 0 ? "0" : formatBytes(r.steady_live_growth)}</td>
        <td>${delta}</td>
        <td>${formatBytes(r.peak_rss_bytes)}</td>
      </tr>`;
    }).join("");

    return `<h4>${leg} / ${config}</h4>
    <div class="performance-trend-table-wrap">
      <table>
        <thead><tr><th>Date</th><th>Commit</th><th>Allocs/frame</th><th>Bytes/frame</th><th>Live growth</th><th>Δ bytes vs trailing mean</th><th>Peak RSS</th></tr></thead>
        <tbody>${trs}</tbody>
      </table>
    </div>`;
  }

  async function render() {
    const [mainRecords, developRecords] = await Promise.all([fetchBranch("main"), fetchBranch("develop")]);
    const allRecords = [...mainRecords, ...developRecords];
    const sectionRenderer = (records) => {
      const groups = perfTrendGroupBy(records, (r) => `${r.leg} ${r.config}`);
      return [...groups.entries()]
        .sort((a, b) => a[0].localeCompare(b[0]))
        .map(([key, rows]) => {
          const [leg, config] = key.split(" ");
          return renderSeries(leg, config, rows);
        })
        .join("\n");
    };
    perfTrendRenderMainAndHistorical(root, allRecords, sectionRenderer,
      '<p class="performance-trend-status">No memory-trend data recorded yet - it appears after the first main push that reaches the persist-performance-trend CI job.</p>');
  }

  render();
})();
</script>
