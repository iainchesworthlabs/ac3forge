# Performance & quality

This project measures itself on every merge and keeps the numbers forever. This
page is the front door to those measurements: what they mean in plain terms,
what they say right now, and which page to open for detail.

If you have never read a codec benchmark before, start with
[How to read these numbers](#how-to-read-these-numbers) at the bottom — it is
written for exactly that.

<div id="pq-status" class="pq-grid">
  <p class="pq-loading">Loading the latest measurements from <code>main</code>…</p>
</div>

<p class="pq-asof" id="pq-asof"></p>

## The three questions

Everything measured here is in service of three questions. They pull against
each other — the settings that make a stream sound better generally make it
slower to produce — so each is tracked separately and none is allowed to quietly
pay for another.

### Is it fast enough?

Encoding and decoding are measured in **milliseconds per frame**, then expressed
as a multiple of **real time**. One AC-3 frame carries 32 ms of audio, so a
frame that takes 0.32 ms to encode runs at 100x real time: a minute of audio in
0.6 seconds.

Anything above 1x is fast enough to keep up with playback. The margin above that
is what buys you batch transcoding, live capture with headroom, and running on a
Raspberry Pi rather than a workstation.

→ [Performance trend](performance-trend.md) tracks this per workload, per
kernel, and alongside memory use.

### Does it sound right?

Two different kinds of answer, because neither alone is enough.

**SNR** (signal-to-noise ratio, in dB) compares the decoded waveform against the
original sample by sample. Higher is better. It is objective and unforgiving,
and it is the right tool for catching a decoder that has genuinely broken —
but it punishes techniques that are *designed* to discard inaudible detail, so a
low SNR is not automatically a quality problem.

!!! note "What the SNR number on this page is actually measuring"
    The **Decode accuracy** card above is not a measure of how good this codec
    sounds. It is a measure of how closely this project's decoder agrees with
    an independent one (FFmpeg's) given the *same* bitstream — including
    bitstreams produced by Dolby's own encoder, which neither decoder controls.

    That distinction matters for reading the number. Perfect agreement would be
    limited only by floating-point noise, and on the channels that carry most
    of the audio it very nearly is: 57–88 dB, run after run, on every platform.
    Where the two decoders diverge, it is usually not because either is wrong.
    A/52 §7.3.4 says a decoder may substitute *"any reasonably random
    sequence"* for the bins the encoder spent no bits on, so two
    spec-correct decoders are *required* to differ there — and those bins
    cluster in the surround channels, which is why a 5.1 fixture's Ls/Rs land
    around 22 dB while its centre channel sits near 58 dB.

    So: a low number on one channel of one fixture is a statement about how
    much freedom the standard leaves, not about audio quality. Read
    [Quality trend](quality-trend.md) for what the number does over time, which
    is the part that carries information, and see
    [Validation](verification.md#what-would-make-these-numbers-excellent) for
    what it would take to remove that spec-permitted divergence from the
    measurement entirely.

**MOS-LQO** (1 to 5) is a prediction of what a listening panel would say,
produced by [ViSQOL](https://github.com/google/visqol). It models hearing rather
than arithmetic, so it credits a stream that sounds right even where the
waveform has moved.

→ [Quality trend](quality-trend.md) tracks the gold-reference SNR gate.
→ [Tool comparison trend](tool-comparison-trend.md) tracks per-tool quality.
→ [Object quality trend](object-quality-trend.md) covers Atmos objects.
→ [Landscape](landscape.md) puts this encoder beside FFmpeg's and Dolby's.

### Does it stay within its budget?

Memory is tracked as **bytes and allocations per frame**, plus peak RSS. A codec
that leaks, or whose working set grows with the length of the file, fails on
long content and on small devices even when it is fast.

→ [Performance trend](performance-trend.md#memory-trend) carries the memory tables.

## What happens when a number moves

Not every change is a regression, so the thresholds are tiered and only the
serious tier blocks a merge:

| Tier | What it means | What happens |
| --- | --- | --- |
| Noise | Under ~3%, or inside the run-to-run spread | Reported as unchanged |
| Soft | 20% slower, or 0.5 dB of quality lost | Warning on the run; merge proceeds |
| Hard | **Twice** as slow, or a gate floor breached | **Fails the build** |

Quality is gated **per channel**, not once per file. Each channel of each
fixture has its own floor, derived from that channel's own lowest measurement
across every platform and every commit on record
(`tools/checks/derive_channel_floors.py` is the derivation, and it is a script
rather than a judgement call so a floor move is reviewable as a diff against
evidence). The trend check works the same way: a channel is compared against
its own trailing average, so a front channel slipping is caught even while the
surrounds — which sit far lower by design — have not moved at all.

That is a recent change, and it closed a real hole. One floor per fixture had
to be low enough for the lowest channel to pass, which on the 5.1 fixtures
meant a floor set by the dither-dominated surrounds: 22 dB, against a centre
channel measuring 58 dB. The centre could have collapsed by 36 dB and the gate
would still have gone green. Per channel, the same fixture now fails on a 6 dB
move in **any** channel.

Timings come from shared CI runners, so a single slow run is not evidence of
anything. Every published number is the fastest of several repetitions, and the
trend pages show the run-to-run spread beside it so you can tell a real move
from a noisy one.

## How to read these numbers

A short glossary, in the order you are likely to meet them.

**Frame** — the unit AC-3 codes audio in. One frame is 1536 samples, 32 ms at
48 kHz. Nearly every measurement here is "per frame" so that it does not depend
on how long the test file happens to be.

**x real time** — how much faster than playback. 100x means one minute of audio
is processed in 0.6 seconds. Below 1x means it cannot keep up live.

**ms/frame** — the same number before the division. Lower is better.

**SNR (dB)** — how closely two pieces of audio match, sample by sample. Higher
is better, and every 6 dB is roughly one more bit of accuracy. What the two
pieces *are* decides how to read it: against the original recording it measures
coding loss (where below ~20 dB is usually audible and above ~40 dB usually is
not), while on this page's Decode accuracy card it measures agreement between
two independent decoders given the same bitstream — a different question, with
a different scale, explained in the note above.

**Floor** — the SNR a given channel of a given fixture must stay above. One per
channel, not one per file, and each derived from that channel's own measured
history rather than chosen. The surrounds of a 5.1 fixture have much lower
floors than its front channels because the standard permits decoders to differ
more there, not because less is expected of them.

**Headroom** — how far a channel currently sits above its own floor. This is
the number to watch: the raw SNR says how two decoders compare, headroom says
how close the gate is to firing. A channel with a low SNR and healthy headroom
is behaving exactly as designed.

**MOS-LQO** — predicted listening quality from 1 to 5, where 5 is
indistinguishable from the original. Roughly: **4-5** excellent, **3-4** good,
**2-3** fair, **under 2** poor. A low bitrate legitimately scores lower — the
point is whether it *changes*, not whether it is 5.

**LSD (dB)** — log-spectral distance: how far the decoded *spectrum* has moved,
rather than the waveform. Lower is better. It is the fairer measure for coding
tools that deliberately resynthesise a band instead of reproducing it.

**Leg** — one platform-and-compiler combination (for example `linux-gcc`,
`windows-msvc`). The same code is measured on several, because a change can help
one and hurt another.

**Gate** — a threshold CI enforces. A number below its floor (or above its
ceiling) fails the build rather than merely being recorded.

<style>
.pq-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); gap: 0.9rem; margin: 1.4rem 0 0.4rem; }
.pq-card { border: 1px solid var(--md-default-fg-color--lightest); border-radius: 6px; padding: 0.85rem 1rem; }
.pq-card h3 { margin: 0 0 0.15rem; font-size: 0.78rem; font-weight: 600; text-transform: uppercase; letter-spacing: 0.04em; color: var(--md-default-fg-color--light); }
.pq-value { font-size: 1.85rem; font-weight: 700; line-height: 1.15; }
.pq-unit { font-size: 0.95rem; font-weight: 500; color: var(--md-default-fg-color--light); }
.pq-note { font-size: 0.78rem; color: var(--md-default-fg-color--light); margin-top: 0.3rem; }
.pq-badge { display: inline-block; font-size: 0.7rem; font-weight: 700; padding: 0.05rem 0.4rem; border-radius: 3px; vertical-align: middle; margin-left: 0.35rem; }
.pq-ok { background: rgba(67, 160, 71, 0.16); color: #2e7d32; }
.pq-watch { background: rgba(251, 140, 0, 0.18); color: #ef6c00; }
.pq-unknown { background: var(--md-default-fg-color--lightest); color: var(--md-default-fg-color--light); }
.pq-loading, .pq-asof { color: var(--md-default-fg-color--light); font-size: 0.82rem; }
[data-md-color-scheme="slate"] .pq-ok { color: #81c784; }
[data-md-color-scheme="slate"] .pq-watch { color: #ffb74d; }
</style>

<script>
(function () {
  // Self-contained, like every other trend page here - see the note at the top
  // of performance-trend.md's script on why these pages deliberately do not
  // share a docs/javascripts asset.
  const REPO = "iainchesworthlabs/ac3forge";
  const HISTORY_BRANCH = "quality-history";

  // Every badge on this page is decided by a threshold the project ALREADY
  // enforces somewhere else, never by a number invented for a status card.
  // That rule exists because the first draft of this page invented three of
  // them and lit up "watch" on a completely healthy build - a card that cries
  // wolf teaches people to ignore it, and there is no shortage of real gates.
  //
  // So: real time is 1x by definition, the SNR gate is whatever
  // threshold_db each record carries, and the memory line is the 4 KiB
  // steady-state retention that append_memory_history.py warns at (see
  // performance-trend.md - it warns above 4 KiB and hard-fails above 1 MiB).
  // MOS gets no badge at all, because no absolute MOS gate exists to borrow.
  const MEMORY_RETENTION_WARN_BYTES = 4 * 1024;

  function rawUrl(file) {
    return `https://raw.githubusercontent.com/${REPO}/${HISTORY_BRANCH}/${file}`;
  }

  function parseJsonl(text) {
    return text.split("\n").filter((l) => l.trim().length > 0).map((l) => JSON.parse(l));
  }

  // The window first, the full history second - tools/ci/append_quality_history.py
  // writes a <branch>.recent.jsonl once the history outgrows its window, and
  // nothing at all before that. Absent is the normal case, not an error.
  async function fetchHistory(stem) {
    for (const file of [`${stem}.recent.jsonl`, `${stem}.jsonl`]) {
      try {
        const resp = await fetch(rawUrl(file));
        if (!resp.ok) continue;
        return parseJsonl(await resp.text());
      } catch (e) {
        // fall through to the next candidate
      }
    }
    return [];
  }

  // Only the newest commit's rows. These files are appended to in merge order,
  // so the last commit SHA to appear is the newest - taken from the end rather
  // than by sorting on commit_date, which would tie whenever two records share
  // a second.
  function newestCommitRows(records) {
    if (records.length === 0) return { rows: [], commit: null, date: null };
    const commit = records[records.length - 1].commit;
    const rows = records.filter((r) => r.commit === commit);
    return { rows, commit, date: rows[0] ? rows[0].commit_date : null };
  }

  // badge may be null, meaning NO badge - not an unknown one. A card with a
  // real number on it must never be labelled "no data" just because nothing
  // gates that number.
  function card(title, value, unit, note, badge) {
    const chip = badge
      ? `<span class="pq-badge ${badge.cls}">${badge.label}</span>`
      : "";
    return `<div class="pq-card">
      <h3>${title}${chip}</h3>
      <div class="pq-value">${value}<span class="pq-unit">${unit ? " " + unit : ""}</span></div>
      <div class="pq-note">${note}</div>
    </div>`;
  }

  const OK = { cls: "pq-ok", label: "ok" };
  const WATCH = { cls: "pq-watch", label: "watch" };
  const NONE = { cls: "pq-unknown", label: "no data" };

  function speedCard(rows) {
    // The SLOWEST workload, not the average: "is it fast enough" is answered by
    // the worst case, and an average over nine workloads would hide one of them
    // falling below real time behind eight that did not.
    const timed = rows.filter((r) => typeof r.ms_per_frame === "number" && r.ms_per_frame > 0);
    if (timed.length === 0) return card("Encode speed", "&mdash;", "", "No measurement recorded.", NONE);
    const worst = timed.reduce((a, b) => (a.ms_per_frame > b.ms_per_frame ? a : b));
    const budget = worst.real_time_budget_ms_per_frame || 32;
    const times = budget / worst.ms_per_frame;
    // 1x is the line that means something - below it the codec cannot keep up
    // with playback. Anything above is headroom, and how much headroom is
    // "enough" is a judgement no threshold here should be making.
    return card("Encode speed", times.toFixed(0) + "&times;", "real time",
      `Slowest of ${timed.length} workloads (<code>${worst.config}</code>) at ` +
      `${worst.ms_per_frame.toFixed(3)} ms/frame against a ${budget} ms frame. ` +
      `Real time is 1&times;.`,
      times >= 1 ? OK : WATCH);
  }

  function qualityCard(rows) {
    // Least headroom over the gate, again worst-case: one leg sitting on its
    // floor matters more than four sitting well above it.
    const scored = rows.filter((r) => typeof r.worst_db === "number" &&
                                      typeof r.threshold_db === "number");
    if (scored.length === 0) return card("Decode accuracy", "&mdash;", "", "No measurement recorded.", NONE);

    // Each check is gated per CHANNEL, so the number that matters is the
    // smallest per-channel margin, which is NOT the same thing as the lowest
    // SNR. A 58 dB centre channel 6 dB above a 52 dB floor is closer to
    // failing than a 22 dB surround 6 dB above a 16 dB floor - and the
    // surround is the one this card used to report, every single time,
    // because it reported the lowest number rather than the tightest one.
    //
    // tightest_headroom_db/thresholds_db arrived with per-channel floors;
    // records written before that carry only worst_db and a scalar
    // threshold_db, and fall back to exactly the old computation rather than
    // being dropped from the comparison.
    const headroomOf = (r) => typeof r.tightest_headroom_db === "number"
      ? r.tightest_headroom_db
      : r.worst_db - r.threshold_db;
    const tight = scored.reduce((a, b) => (headroomOf(a) < headroomOf(b) ? a : b));
    const headroom = headroomOf(tight);

    // Name the channel when we know it. "Ls 22.8 dB, 6.8 dB above its 16 dB
    // floor" tells a reader which channel to go and look at; "22.7 dB SNR"
    // on its own invites the reading that the codec is 22 dB accurate, which
    // it is not - see "What the SNR number is measuring" on this page.
    const thresholds = Array.isArray(tight.thresholds_db) ? tight.thresholds_db : null;
    let value, note;
    if (thresholds) {
      const i = typeof tight.tightest_channel === "number" ? tight.tightest_channel : 0;
      const name = (tight.channel_labels || [])[i] || `ch${i}`;
      value = (tight.channels_db ? tight.channels_db[i] : tight.worst_db).toFixed(1);
      note = `Tightest channel of ${scored.length} checks: ${name} on ` +
             `<code>${tight.leg}</code>, ${headroom.toFixed(1)} dB above its own ` +
             `${thresholds[i]} dB floor. Every channel is gated separately.`;
    } else {
      value = tight.worst_db.toFixed(1);
      note = `Tightest of ${scored.length} checks (<code>${tight.leg}</code>), ` +
             `${headroom.toFixed(1)} dB above its ${tight.threshold_db.toFixed(0)} dB gate.`;
    }
    return card("Decode accuracy", value, "dB SNR", note, headroom >= 0 ? OK : WATCH);
  }

  function listeningCard(rows) {
    const scored = rows.filter((r) => typeof r.mos_lqo === "number");
    if (scored.length === 0) return card("Predicted listening quality", "&mdash;", "", "No measurement recorded.", NONE);
    const values = scored.map((r) => r.mos_lqo);
    const best = Math.max(...values);
    const worstRow = scored.reduce((a, b) => (a.mos_lqo < b.mos_lqo ? a : b));
    const legs = new Set(scored.map((r) => r.leg)).size;
    // NO badge, deliberately - not an unknown one. A low-bitrate leg is
    // SUPPOSED to score lower, so any absolute floor here would flag the
    // encoder for doing its job. A MOS change is judged on the trend pages,
    // against that leg's own past, which is the only comparison that means
    // anything.
    return card("Predicted listening quality",
      worstRow.mos_lqo.toFixed(2) + "&ndash;" + best.toFixed(2), "MOS-LQO",
      `${scored.length} measurements across ${legs} legs. Lowest is ` +
      `<code>${worstRow.leg}</code> at ${worstRow.bitrate_kbps} kbps. ` +
      `5 is indistinguishable from the original; a lower bitrate scores ` +
      `lower by design.`,
      null);
  }

  function memoryCard(rows) {
    const scored = rows.filter((r) => typeof r.bytes_per_frame === "number");
    if (scored.length === 0) return card("Memory per frame", "&mdash;", "", "No measurement recorded.", NONE);
    const worst = scored.reduce((a, b) => (a.bytes_per_frame > b.bytes_per_frame ? a : b));
    // steady_live_growth is the bytes still held live after ~200 steady-state
    // frames. NOT "> 0 means a leak": append_memory_history.py warns above
    // 4 KiB and hard-fails above 1 MiB, so a couple of KiB of retained working
    // set is expected and this card says so rather than calling it growth.
    const retained = scored.reduce((a, b) =>
      ((a.steady_live_growth || 0) > (b.steady_live_growth || 0) ? a : b));
    const held = retained.steady_live_growth || 0;
    return card("Memory per frame", (worst.bytes_per_frame / 1024).toFixed(0), "KB",
      `Heaviest of ${scored.length} workloads (<code>${worst.config}</code>), ` +
      `${worst.allocs_per_frame} allocations. Most retained after 200 ` +
      `steady-state frames: ${(held / 1024).toFixed(1)} KiB ` +
      `(<code>${retained.config}</code>), against a 4 KiB warning line.`,
      held <= MEMORY_RETENTION_WARN_BYTES ? OK : WATCH);
  }

  async function render() {
    const target = document.getElementById("pq-status");
    const [perf, quality, external, memory] = await Promise.all([
      fetchHistory("performance-main"),
      fetchHistory("main"),
      fetchHistory("external-comparison-main"),
      fetchHistory("memory-main"),
    ]);

    if (perf.length === 0 && quality.length === 0 && external.length === 0 && memory.length === 0) {
      target.innerHTML = `<p class="pq-loading">Could not reach the measurement history on
        <code>${HISTORY_BRANCH}</code>. The detail pages linked below fetch it the same way,
        so they will be empty too &mdash; this is a network or availability problem, not a
        missing measurement.</p>`;
      return;
    }

    const p = newestCommitRows(perf);
    const q = newestCommitRows(quality);
    const e = newestCommitRows(external);
    const m = newestCommitRows(memory);

    target.innerHTML = [speedCard(p.rows), qualityCard(q.rows),
                        listeningCard(e.rows), memoryCard(m.rows)].join("");

    // Whichever series has a date - they are all written by the same run, but
    // any one of them can be absent on a given commit.
    const stamp = [p, q, e, m].map((x) => x.date).filter(Boolean).sort().pop();
    const sha = [p, q, e, m].map((x) => x.commit).filter(Boolean)[0];
    if (stamp && sha) {
      document.getElementById("pq-asof").innerHTML =
        `Latest measurement on <code>main</code>: ` +
        `<a href="https://github.com/${REPO}/commit/${sha}">${sha.slice(0, 8)}</a>, ` +
        `${new Date(stamp).toLocaleString()}. Updated on every merge.`;
    }
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", render);
  } else {
    render();
  }
})();
</script>
