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

## Profile by source line, not by symbol

The single largest performance finding in this codebase was invisible to symbol-level
profiling, and the way it hid is worth repeating because the trap is generic.

`joc::reconstruct_mdct_band` showed 62% self-time in an Atmos decode. That reads like
"vectorise its inner loop", and doing so would have been worth about 2%. Re-profiling
the same run with `perf report --sort=srcline` showed the time was not in that
function's arithmetic at all: 45% of the whole profile was in **joc.hpp**, inlined —
`FrameParameters::object_offset()` walking an O(objects) list on *every* coefficient
access, making a frame O(objects²). Fixing that made a 12-object decode 1.82x faster
in the MDCT domain and 2.90x in the QMF domain (the default), against roughly 18% from
four phases of transform vectorisation.

Symbol self-time attributes inlined header code to whoever inlined it, so an
accidentally-quadratic accessor appears as arithmetic in its caller. When a symbol
looks hot, confirm *which lines* before designing a fix. A `RelWithDebInfo` build with
`-O3 -g` is enough; on this repo it also needs `-Wno-error=null-dereference` for a GCC
false positive in `apps/cli/commands/audio_io.cpp` that the Release preset does not trip.

The same method found the second-largest win: `aht_bin_gaq_bits` fully quantising six
mantissas per candidate gain to read one integer width off each result — 43% of an
E-AC-3 encode profile, and 1.70x on `eac3_51_auto` once the width was derived from the
escape predicate instead.

## A bench that cannot see a bug class cannot gate it

`ac3kernelbench`'s JOC series originally ran at four objects. The quadratic accessor
above moved those rows about 26% — an ordinary-looking optimisation — while moving a
real 12-object decode 1.8-2.9x. The bench was structurally unable to report the
difference, because at four objects an O(objects²) term is nearly invisible.

`joc_reconstruct_mdct_12obj` and `joc_reconstruct_qmf_12obj` exist to close that. Twelve
is representative rather than arbitrary: TS 103 420 caps a programme at 15 dynamic
objects plus the bed LFE, and twelve is three clean batches of four for the batched
transform paths. When adding a series, ask what class of regression it can and cannot
see at the size it runs — a workload sized where the interesting term vanishes will
pass forever while the thing it nominally covers rots.

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
still held after ~200 steady-state frames - on the trunk that check is
absolute rather than trend-relative; see below for how it is scoped before a
merge). These counts are near-deterministic for a fixed workload: a
flagged row is a real change in allocation behaviour, not runner noise. The
memory-usage optimization programme's phases land as visible downward steps in
these series - that is what this table exists to show.

The same question is now asked before the merge as well. These series are
written by `persist-performance-trend`, which is `push` to `main` only, so for
a while a step was reported on the trunk *after* it landed - a red check on an
already-merged commit, blocking nothing and belonging to whoever pushed next.
The E-AC-3 encode step from 67 to 199 allocs/frame in 2026-08 (issue #544) is
exactly how it was found: the gate fired on the merge, and by then the merge
was the thing it was reporting on. The
`Memory vs merge base` job (`tools/ci/compare_memory.py`) closes that: it
builds `ac3membench` at the pull request's head and at its merge base and runs
each once, comparing the same two churn metrics against the same thresholds,
imported from `append_memory_history.py` so the two gates cannot disagree. One
run per side is the whole measurement - these counts do not move between runs
of a fixed binary, which is why this gate needs none of the repetition and
interleaving the `Performance vs merge base` job uses to see past timing
noise. Its hard tier fails the `Memory gate` check;
`memory-regression-approved` on the pull request turns that back into an
annotation, the way `perf-regression-approved` does for speed.

In that pre-merge job the leak check keeps its absolute thresholds but applies
them to what the branch changed - crossing a threshold the merge base was
under, or growing by more than one. Three of the six workloads already retain
bytes across their steady state and two of them sit past the 4 KiB warn line,
so a per-PR check copied over unchanged would annotate every pull request for
the merge base's own findings. `persist-performance-trend` keeps the
unconditional absolute view on the trunk.

One limit still worth knowing when reading these series: the history a trunk
run compares against is per branch, so a branch rename or a gitflow-to-trunk
switch starts one from empty - the trailing window now widens to the sibling
branch series when a branch's own file holds fewer than three records, and a
workload with no history anywhere is annotated as ungated rather than passing
quietly.

Two landed programmes are the biggest steps in these series. The 2026-08
memory-usage programme cut steady-state allocator traffic per frame by 85-88%
on the encode series (**as measured when it landed**, on the `linux-gcc`
runner: AC-3 encode 225,028 → 26,778 bytes/frame and 286 → 86 allocations;
E-AC-3 214,808 → 28,792 and 157 → 67; Atmos 218,960 → 32,656 and
196 → 106) and 54-61% on the decode series - and, outside these tables, took
every output-producing CLI command memory-flat at any programme length (a
3-minute 5.1 encode peaked at 437.8 MiB before the programme and 9.3 MiB
after; decode 217 → 28.5 MiB, `spdif` 225.7 → 18.0 MiB).

Two of those three encode figures no longer describe the code. At `main` =
`e982712b` the same leg records E-AC-3 encode at 53,845.7 bytes/frame and
199.11 allocations, and Atmos at 53,606.4 and 219.10 - both of them above the
*pre*-programme baselines quoted above, 157 and 196 allocations. The decode
series is unaffected, and so is AC-3 encode, which still reads 26,778.5 and
86.04. That last row is why the other two can be read at all: a workload that
still matches its landed figure to the decimal, on the same leg, rules out
platform, stdlib and measurement-context drift. Without that control the two
divergences would be arguable; with it, what moved is the code the other two
share.

It is one step rather than a drift. Both E-AC-3-family workloads sit flat at
the old values through every record up to `3aedec41` and flat at the new ones
from `83546721` (2026-08-25) onward. Bisecting `ac3membench` brackets the
step to PR #352's per-channel exponent-run planner: the commit before it
(`f54ea929`) measures 95.0 allocations/frame for `eac3_51_encode`, and
`fb58aa62` measures 248.2 (a windows-msvc build - the leg differs from this
table's, the step does not). AC-3 encode is untouched because it plans its
exponent runs through its own encoder.

The extra churn is a defect rather than the planner's intended cost, and is
tracked as [#544](https://github.com/iainchesworthlabs/ac3forge/issues/544).
`encode_run` in `src/forge/src/encoder/eac3_frame.cpp` assigns the by-value
return of `ac3::encode_exponents`, which owns a `std::vector`, so each run
reallocates that buffer on every frame; the planner multiplied the number of
runs from one per channel to one per run per channel. The bench's own columns
carry the signature. Before the step each encode workload's steady-state
count sat below its first frame's (E-AC-3 134 first, 67.00 steady), which is
warm-up followed by reuse. After it the steady-state count exceeds the first
frame's (159 first, 199.11 steady), which is a path allocating fresh storage
every frame. `run.decoded` and `run.bap` in the same function reuse their
capacity correctly, as does `ChannelPlan::runs`.

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

Not a trend series — one measured configuration, on the concrete target the
roadmap names: `arm-none-eabi` cross-compiled for QEMU's `mps2-an385` machine (Cortex-M3,
soft float, no OS), `AC3FORGE_MINIMAL_DECODER=ON`, `CMAKE_BUILD_TYPE=MinSizeRel`. See
[Building → Minimum-footprint decoder profile](building.md#minimum-footprint-decoder-profile)
for what the profile changes and why.

`apps/baremetal/probe.cpp` decodes six frames each of nine real streams — 5.1 AC-3 (448 kbit/s,
coupling), 2/0 AC-3 (192 kbit/s) and 1/0 AC-3 (128 kbit/s); 5.1 E-AC-3 (384 kbit/s, AHT + spx +
standard coupling), 5.1 E-AC-3 with §E3.5 enhanced coupling (384 kbit/s, `cpl+ecpl`), E-AC-3
Atmos (448 kbit/s, six objects over a 5.1 bed) and 2/0 E-AC-3 (192 kbit/s, which is the only
layout §7.5.4 rematrixing exists in) and E-AC-3 7.1.4 (640 kbit/s, a bed and two dependent
substreams) and a second Atmos stream with three of its objects raised to the ceiling — and
reports what it cost. That is twelve fixtures: the first Atmos stream is decoded twice, bed-only
and with its objects reconstructed, the two 5.1 streams are decoded a second time through the
§7.8 output stage, folded to Lo/Ro stereo in line mode (`ac3_fold`, `eac3_fold`), and the
height stream's objects are reconstructed and placed onto 7.1.4 (`eac3_atmos_render`). Numbers
below are from a run against `feature/baremetal-encode-timing` on 2026-09-10, `arm-none-eabi`
GCC 14.2.1 under QEMU 10.2.1's `mps2-an385`; `build-footprint` in
`.github/workflows/_build.yml` reproduces them on every push, and
`tools/checks/run_baremetal_probe.sh` reproduces them locally.

The table below was first measured early in PF6/PF7's own feature branch (PR #351). Several
`develop` merges landed on that branch afterwards but before it merged to `main` — most
significantly DC10's QMF-domain JOC reconstruction, which the decode path needs
(`src/forge/src/dsp/qmf.cpp` and `src/forge/src/verify/eac3_mirror.cpp`, both correctly added to
`src/forge/minimal.cmake`'s source list at the time, per that merge's own commit message), plus
the PF3/PF4 FFT/IMDCT rewrite and DC1's decoder output stage — and nobody re-measured the table
or the ceiling before merging. The image had already reached 412,516 bytes by then.

The same thing happened a second time. The largest movement in that re-measurement was a
relocation rather than growth. AP3's Pimpl sweep (`ee5ff91e`) gave both decoders a
`struct Impl; std::unique_ptr<Impl> impl_;`
(`src/forge/include/ac3/decoder/decoder.hpp:435` and `:758`), so `sizeof(ac3::FrameDecoder)` and
`sizeof(ac3::Eac3Decoder)` fell from 12,952 and 27,408 bytes to a single 4-byte pointer each, and
the state they used to hold in place now lives on the heap. That state came out of automatic
storage: both decoders are locals in `decode_ac3()` and `decode_eac3()`, and `.bss` was unchanged
at 237,592 bytes across those two measurements. It has moved since, for unrelated reasons the
Static footprint section below sets out. Peak heap rose by 27,416 bytes, which is the E-AC-3
decoder's former in-place size rather than the two summed — `decode_ac3()` returns before
`decode_eac3()` runs, so only the larger of the two is ever live at the peak. The rest of the
delta is `.text`, up 5,728 bytes and the whole of the image change, from the ordinary work of the
intervening commits.

### Static footprint

| | Bytes |
|---|---|
| `.text` (code + read-only data) | 276,188 |
| `.data` (initialised) | 400 |
| `.bss` (zero-initialised) | 62,205 |
| **Image total** | **338,793** (330.9 KiB) |

These are `arm-none-eabi-size`'s own columns, which is what `AC3FORGE_MAX_IMAGE_BYTES` gates, so
they group sections rather than list them: `.text` here includes `.init`, `.fini` and
`.ARM.exidx`, `.data` includes `.init_array` and `.fini_array`, and `.bss` includes `.tbss`. Read
per-section with `arm-none-eabi-size -A`, `.text` is 256,340, `.data` 388 and `.bss` 46,824.

`.bss` fell from 237,592 bytes in two steps. Moving `ecpl_channel_spectrum`'s 32 KB scratch off
thread-local storage — it made the library unlinkable into any FreeRTOS application, see
[the ESP32-S3 page](platforms/esp32.md) — took
`.tbss` from 32,784 bytes to 24, and `tls.cpp`'s block was resized from 64 KiB to 4 KiB to
match. The decode path then moved to float32 under this profile, halving every coefficient
buffer. `.text` rose 5,680 bytes over the same span, which is the float32 transform
instantiation.

`.text` has risen as fixtures were added, and most of each rise is the bitstreams themselves:
`fixture.hpp` is `constexpr` `std::array` data linked into `probe.cpp.obj`'s read-only section.
It held 19,968 bytes of stream before the enhanced-coupling and 2/0 fixtures, 33,792 with them,
and 52,224 now across seven streams. None of the tools those fixtures reach added code —
`eac3_tools.cpp`, `fft.cpp`, `joc.cpp` and `oamd.cpp` were already in `src/forge/minimal.cmake`'s
source list and already linked, which is the point: what the fixtures added was execution, not
size.

The float decode path moved it again, by less than the size of the conversion suggests. Against
the 320,940 bytes `main` measured before it (223,260 of `.text`, 97,280 of `.bss`), `.text` is
1,264 bytes larger and `.bss` 2,864. The `<double>` instantiations this profile no longer
references left the image as their float forms came in, so most of the conversion was a swap:
`eac3_decoder.cpp.obj` is 912 bytes smaller, `joc.cpp.obj` 590 larger. The `.bss` is two
things. 1,949 bytes are the stage timers' tables, `stage_timers.cpp.obj`, linked into every
shape of the probe so that a timed build and a plain one differ only in the library's include
path; 912 are two tables `eac3_tools.cpp` now fills once at start-up rather than computing per
call, spectral extension's attenuation (32 codes by 3 taps) and the AHT's inverse kernel in
float.

Enhanced coupling's float forms then took 7,067 bytes back off, to 318,001. The double `dft512`
and its tables left the image — `fft.cpp.obj` went from 4,444 to 3,004 bytes of `.text` and
from 9,204 to 5,116 of `.bss`, the float twiddles being half the size — and
`eac3_decoder.cpp.obj` lost 1,150 bytes of `.text` with its double §E3.5 path.

The hot-path sweep's `BitReader` cache and bit-allocation memos cost 2,520 bytes of `.text` and
no `.bss`, for an image of 320,521. 1,686 of it is `decoder.cpp.obj`, whose read sites are the
most numerous; 716 is `eac3_decoder.cpp.obj`. The access unit's `memcpy` and its moved object
description are 80 bytes more: 320,601. The 7.1.4 fixture is 55,496 more, and nearly all of it
is what it says: 30,720 bytes of stream in `.text` and 24,576 of `.bss` for the four channels the
probe's PCM block grew by, against 168 bytes of code. 376,097. The block-granular output forms then
took that block out altogether - the probe reads the decoders' blocks in place and holds no PCM -
and `.bss` fell 73,824 bytes to 46,829, against 872 bytes of `.text` for the forms themselves:
303,145, the smallest image the probe has had since its fixtures were four. The output stage's
float forms and the two fold rows are 472 more - 456 of `.text`, 16 of `.bss` - for 303,617:
`output.cpp.obj` went from 4.5 KiB to 4.6, the narrowed Hilbert kernel being a second static
beside the double one, and `probe.cpp.obj` from 86.0 KiB to 86.3 with the rows. Placing objects
is 33,640 more, for 337,257: the height stream's 10,752 bytes and
`spatial.cpp` in `.text`, and in `.bss` a 12,288-byte render block - twelve channels of one
256-sample block, what a player holds - with a 1,536-byte table of each object's gain per slot.
The stage-timer table's growth from 32 zones to 64, which the encoder's rows needed, is 1,536
more of `.bss` in every shape of the probe: 338,793.

Where it went, objects over 2 KiB (see `tools/checks/footprint_report.py --map` for the full
attribution from the linker map):

| Object | `.text` | `.bss` |
|---|---|---|
| `probe.cpp.obj` (the harness itself — fixtures, checks, allocator hooks) | 86.0 KiB | 809 B |
| `eac3_decoder.cpp.obj` (all of Annex E) | 37.5 KiB | 0 B |
| `eac3_tools.cpp.obj` (spx/ecpl band geometry + §3.5.5 reconstruction) | 19.8 KiB | 11.2 KiB |
| `mdct.cpp.obj` (inverse transform, fast path only) | 15.9 KiB | 14.6 KiB |
| `decoder.cpp.obj` (AC-3) | 20.8 KiB | 0 B |
| `joc.cpp.obj` (§6 object reconstruction from the bed) | 14.0 KiB | 0 B |
| `qmf.cpp.obj` (DC10's QMF-domain JOC reconstruction) | 6.0 KiB | 4.2 KiB |
| `fft.cpp.obj` (the 512-point DFT §3.5.5 enhanced coupling needs) | 2.9 KiB | 5.0 KiB |
| `oamd.cpp.obj` (§H.1 object metadata) | 6.8 KiB | 0 B |
| `output.cpp.obj` (`OutputStage::apply`/`mix_levels`, both decoders') | 4.5 KiB | 16 B |
| `tls.cpp.obj` (the single-thread TLS block — see below) | 8 B | 4.0 KiB |
| `bitalloc.cpp.obj` (§7.2 bit allocation, both generations) | 3.9 KiB | 0 B |
| `transient_prenoise.cpp.obj` (§3.7 post-IMDCT correction) | 744 B | 3.0 KiB |
| `libm_a-e_pow.o` (newlib's `pow`) | 2.9 KiB | 0 B |
| `stage_timers.cpp.obj` (the stage timers' tables, linked into every shape of the probe so that a timed build and a plain one differ only in the library's include path) | 918 B | 1.9 KiB |
| `arm_librdimon_a-syscalls.o` (newlib's semihosting syscalls) | 2.5 KiB | 176 B |
| `libm_a-k_rem_pio2.o` (newlib's trig argument reduction) | 2.2 KiB | 0 B |
| everything else, summed | 22.3 KiB | 769 B |

One earlier correction is worth knowing when comparing this table against older versions of it.
The attribution used to read GNU ld's "Discarded input sections" block as though it were part of
the map proper, so every `--gc-sections` casualty was credited to the object it came from; it
inflated the `.text` column by 63 KiB, `eac3_tools.cpp.obj` most of all (21.3 KiB reported
against 8.4 KiB actually linked). `footprint_report.py` has skipped that block since, and both
columns reconcile with `arm-none-eabi-size`'s own totals.

`tls.cpp.obj`'s 4 KiB is the single-thread `__aeabi_read_tp` stub's static block
(`apps/baremetal/platform/baremetal/tls.cpp`), checked by two `ASSERT()`s in the linker script
rather than trusted.

It was 64 KiB, sized against `ecpl_channel_spectrum`'s `thread_local` scratch. That scratch is no
longer thread-local: at 32 KB it made the library unlinkable into any FreeRTOS application,
because FreeRTOS carves each task's thread-local area out of that task's own stack and ESP-IDF's
1 KB IPC task could not then be created. With the storage moved to the heap behind a
`unique_ptr`, the measured `.tbss` is 24 bytes, so 4 KiB leaves the same order of headroom the
old number did.

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
| Peak heap | 229,630 bytes (224.2 KiB), the 7.1.4 fixture; 210,203 with Atmos objects |
| Retained after teardown | 12 bytes |
| `sizeof(ac3::FrameDecoder)` | 4 bytes (one `unique_ptr` — see above) |
| `sizeof(ac3::Eac3Decoder)` | 4 bytes (one `unique_ptr` — see above) |
| Caller-owned PCM buffer | none: the probe decodes through the `_by_block` forms and reads the decoders' blocks in place |
| AC-3 allocations per frame, steady state | 3 |
| AC-3 2/0 and 1/0 allocations per frame, steady state | 1 |
| E-AC-3 allocations per frame, steady state | 12 |
| E-AC-3 enhanced coupling allocations per frame, steady state | 12 |
| E-AC-3 2/0 allocations per frame, steady state | 10 |
| Atmos bed allocations per frame, steady state | 20 |
| Atmos with objects allocations per frame, steady state | 31 |
| E-AC-3 7.1.4 allocations per frame, steady state | 35 |

The steady-state allocation counts are the gap [Building](building.md#gaps) records: PF7 asks
for zero, and this is 1–35. What is left is no longer the per-block geometry vectors inside the
decoders — those are `Impl` members now, reused frame to frame — but the `std::vector` members
of the returned `DecodedFrame`/`DecodedSubstream`, which the memory programme's [`_into`
forms](#whole-frame-trend) could not remove because they are inherent to those two return types
rather than to allocation *reuse*. `DecodedFrame::blksw` is the whole of AC-3's remaining one
per frame; `DecodedSubstream::channels` is 7 of E-AC-3's 12. Reaching zero means those becoming
fixed-capacity or pooled, a public-type change tracked separately from this profile.

Enhanced coupling used to be the outlier here, at 126 against 43–86, and had its own ceiling of
140. It measures 12 now, level with plain E-AC-3, because the gap was never §E3.5's geometry:
sixty of it were two `std::vector<double>` built per coupled channel per block in the
reconstruction loop, and the rest went when both decoders' frame-scope buffers moved onto the
decoder. There is one ceiling, 100, and no exemption.

Both bare-metal legs report all of these counts identically, on different libstdc++ versions
(GCC 14.2 for `arm-none-eabi`, 15.2 for Xtensa under ESP-IDF 6.1), as they do the peak and the
retained bytes. The counts come from the decoders' own per-block geometry rather than from
anything the standard library is free to vary, so a divergence between the legs would itself be
news.

The peak is what an Atmos fixture decoded **with its objects** costs — it was 179,064 before that
fixture existed, and 449,826 when the object path was first measured. `Domain::kMdctBand`, a
float32 `ReconstructionState`, per-object scratches sized to the stream and handing back the
enhanced-coupling scratch between decodes took it to 233,546. Moving the decoders' frame-scope
buffers onto the decoder — what closed the per-frame churn above — added 2,845 back, because a
buffer's high-water capacity is now held for the decoder's lifetime rather than released each
frame. JOC's mixing then began narrowing the frame's matrix once into a scratch of its own rather
than at every read, 912 bytes more. The float form of the enhanced-coupling scratch, and of
the decoder's own §E3.5 state, then gave 4,108 back, and the bit-allocation memos — each
stream's last exponent set and allocation parameters, kept so an unchanged block reuses its
allocation — hold 1,608 across a frame; they are built on a stream's first block, so a run's
allocation total rises by 41 while every fixture's steady-state count above is unchanged.
Moving a substream's object description into the access unit rather than copying it then gave
24,600 back. 210,203 fits the 280,792 bytes an ESP32-S3 has free with 70,589 to spare - 26,188
below the 236,391 `main` carried before this stack, and the lowest peak the probe has reported
since objects were first reconstructed, with a quarter of the part's free SRAM unused at it.
The 7.1.4 fixture then set a new one: 229,630, the widest programme the format has, against the
257,572 bytes the probe's twelve-channel PCM block leaves free on the part - 27,942 to spare.
The peak by fixture, identical on both legs:

| Fixture | Peak heap | Allocations per frame |
|---|---:|---:|
| `ac3_mono` | 47,524 | 1 |
| `ac3_stereo` | 49,228 | 1 |
| `ac3` 5.1 | 56,329 | 3 |
| `ac3_fold` | 68,617 | 3 |
| `eac3_atmos_bed` | 123,735 | 20 |
| `eac3_stereo` | 140,534 | 10 |
| `eac3_ecpl` | 157,493 | 12 |
| `eac3` 5.1 | 167,042 | 12 |
| `eac3_atmos_objects` | 210,203 | 31 |
| `eac3_fold` | 216,406 | 12 |
| `eac3_atmos_render` | 210,573 | 36 |
| `eac3_714` | 229,630 | 35 |

The AC-3 rows carry the 36,872 bytes of the block form's own frame (`decode_frame_by_block`: AC-3 has
no substream vectors to hand out views of, so it keeps one frame, sized once); the E-AC-3 rows did
not move, since that form copies nothing. Before the block forms the AC-3 rows were 10,652, 12,356
and 19,457. The two fold rows are the 5.1 rows plus the output stage's own buffers: the stereo
frame it writes (12,288 bytes) and, for E-AC-3, the six seats its layout fold stages the
substreams' channels into (36,864) - neither of them the probe's peak.
 [The ESP32-S3 page](platforms/esp32.md#objects) has what each step was worth.

**Retained after teardown** is bytes still live when the probe finishes, after every decoder it
made has been destroyed — so not per-frame growth and not a leak. It is 12 bytes now: one
`__cxa_thread_atexit` registration record, for the pointer to enhanced coupling's spectrum scratch,
the one `thread_local` the library still declares.

It was 34,232 until the probe began calling `ac3::eac3::release_ecpl_scratch()` between fixtures,
and 24 until the per-bin angle buffer stopped being a second `thread_local`. The 34,232 was
enhanced coupling's 32,768-byte spectrum scratch and its 1,440-byte bin-angle vector, both
`thread_local` so §E3.5 neither allocates per call nor puts 32 KB on the stack, and therefore
resident for the life of a task that never exits. Bounded and paid once — but enough to decide
whether something else fits, and it decided: object reconstruction failed on an ESP32-S3 whenever
it ran after an enhanced-coupling decode, on a 6,144-byte request, and succeeds now that the
scratch goes back. The scratch is 23,552 bytes on this profile now (its float form, tables
included) and the angle buffer a stack array. Nothing could measure any of it until a fixture
reached §E3.5.

`tools/checks/run_baremetal_probe.sh` gates the image, the heap peak, the retained bytes and
every fixture's allocation count at ceilings above these measured values, so a regression stops
the build instead of drifting the table silently. The fixture names come from the probe's own
output rather than a list in the script, so a fixture added and forgotten cannot pass unnoticed.

### Instructions per frame

`tools/checks/run_baremetal_probe.sh --icount` builds the probe with its clock on the
mps2-an385's 25 MHz timer and runs QEMU under `-icount shift=0`, where the guest clock advances
one nanosecond per executed instruction; the probe's microseconds are then thousands of Thumb-2
instructions, the same on every host. Measured on the arm-none-eabi leg at `b28e4869`, `-Os`,
soft float throughout (the leg has no FPU, so this is what a part without one pays):

| Fixture | Instructions per frame | Ceiling |
|---|---:|---:|
| `ac3_mono` | 1,625,000 | 2,000,000 |
| `ac3_stereo` | 3,548,000 | 4,500,000 |
| `eac3_stereo` | 4,851,000 | 6,000,000 |
| `eac3_atmos_bed` | 8,940,000 | 11,000,000 |
| `ac3` 5.1 | 10,224,000 | 13,000,000 |
| `ac3_fold` | 10,782,000 | 13,500,000 |
| `eac3` 5.1 | 12,928,000 | 16,000,000 |
| `eac3_fold` | 14,280,000 | 17,000,000 |
| `eac3_atmos_objects` | 28,213,000 | 35,000,000 |
| `eac3_atmos_render` | 28,938,000 | 36,000,000 |
| `eac3_ecpl` | 28,861,000 | 36,000,000 |
| `eac3_714` | 33,793,000 | 42,000,000 |

The two fold rows were measured at `195ba37c` on 2026-09-10, in a run that reproduced every
other row to within the microsecond the probe prints - 1,000 instructions; the fold itself is
558,000 instructions over plain AC-3 5.1 and 1,352,000 over E-AC-3 5.1, 5% and 10%. The render
row is the objects row plus the placing: 5% of its count is the render, the
rest the same reconstruction.

Not cycles on any real part: a Cortex-M3 would take more, an ESP32-S3 with its FPU takes a fifth
of a 5.1 frame's count in cycles. What the column is for is that it is deterministic — two runs
agree to the instruction — so a change that adds one per cent of work to a fixture shows in the
run's own lines, and the ceilings above hold the same headroom the other gates do. The
[ESP32-S3 page](platforms/esp32.md#other-esp32-variants) reads the ESP32-C3's prospects off it.

### Instructions per encoded frame

The same clock on the encode probe, `tools/checks/run_baremetal_probe.sh --encoder --icount`,
measured 2026-09-10 on the same leg. Both encoders are `double` throughout, so on this FPU-less
leg every operation is a software call - which is the gap to the decode rows above, three to
five times for the same layout, rather than anything the encoders' search costs. The ceilings
are `ICOUNT_CEILING_ENCODE` in the runner, with the same headroom as every other gate.

| Row | Instructions per frame | Ceiling | Peak heap | Allocations per frame |
|---|---:|---:|---:|---:|
| `ac3_stereo` 2/0, 192 kbit/s | 12,623,000 | 16,000,000 | 82,367 | 34 |
| `eac3_stereo` 2/0, 192 kbit/s | 24,200,000 | 30,000,000 | 118,962 | 84 |
| `eac3_tools` 2/0, 192 kbit/s, cpl + spx + AHT | 24,486,000 | 31,000,000 | 195,321 | 47 |
| `ac3` 5.1, 448 kbit/s | 34,286,000 | 43,000,000 | 162,602 | 67 |
| `eac3` 5.1, 384 kbit/s | 62,590,000 | 78,000,000 | 220,608 | 180 |
| `eac3_ecpl` 2/0, 192 kbit/s, §E3.5 | 82,975,000 | 104,000,000 | 192,573 | 91 |

The three 2/0 rows are new with the timing; the encode image is 237,325 bytes with them
(159,384 `.text`, 400 `.data`, 77,541 `.bss`), 5,600 more than without: 480 for the rows, the rest
the stage timers' application half, which an encode image links now that the probe reports its
stages, and the 64-zone table. [Building](building.md#what-the-encode-direction-costs)
has what the encode direction cannot fit on an ESP32-S3, with the host profile's numbers.

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
