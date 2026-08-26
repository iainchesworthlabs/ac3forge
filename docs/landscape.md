# Landscape

How ac3forge's encoder compares to FFmpeg's and Dolby's own (DEE, the Dolby
Encoding Engine) at matched bitrates, release by release. This is the
headline number — see [Tool comparison trend](tool-comparison-trend.md) for
the commit-level, per-Annex-E-tool detail this page deliberately doesn't
show.

The comparison is necessarily one number per (leg, tool): neither FFmpeg's
nor DEE's own E-AC-3/AC-3 encoder exposes which internal coding tools it
used, so there's no apples-to-apples way to isolate "just coupling" or "just
spectral extension" against them the way this project can against its own
history. What's shown is `landscape` — this project's `auto`-tools E-AC-3
encode (the set the encoder picks from the per-channel rate, which is the
like-for-like answer to FFmpeg's and DEE's own automatic choices), or AC-3's
unconditionally-automatic encode — since that's the number a real user of
either tool actually gets, not an internal detail.

<div id="landscape-app">
  <p class="landscape-status">Loading landscape data…</p>
</div>

<style>
#landscape-app { margin: 1.5em 0; }
.landscape-status { color: var(--md-default-fg-color--light); font-style: italic; }
.landscape-baseline { font-size: 0.85em; color: var(--md-default-fg-color--light); margin-bottom: 1em; }
.landscape-table-wrap { overflow-x: auto; }
#landscape-app table { width: 100%; border-collapse: collapse; font-size: 0.9em; }
#landscape-app th, #landscape-app td { padding: 0.4em 0.7em; text-align: left; border-bottom: 1px solid var(--md-default-fg-color--lightest); white-space: nowrap; }
#landscape-app th { vertical-align: bottom; }
/* One rule per metric group, so the three blocks read as blocks rather than
   as nine loose columns. */
.landscape-group-start { border-left: 1px solid var(--md-default-fg-color--lighter); }
.landscape-release { text-decoration: none; font-weight: 600; }
.landscape-release:hover { text-decoration: underline; }
.landscape-prerelease { font-weight: 400; font-style: italic; color: var(--md-default-fg-color--light); }
.landscape-delta-up { color: #2e7d32; font-weight: 600; }
.landscape-delta-down { color: #c62828; font-weight: 600; }
.landscape-na { color: var(--md-default-fg-color--light); font-style: italic; }
/* Alternated per release (not per row) so a release's 1-3 leg rows read as
   one band, and skimming down the table shows release boundaries at a
   glance rather than a flat wall of same-looking rows. */
.landscape-stripe-b { background: color-mix(in srgb, var(--md-default-fg-color) 6%, transparent); }
</style>

<script>
(function () {
  const REPO = "iainchesworthlabs/ac3forge";
  const HISTORY_BRANCH = "quality-history";
  // Releases only ever happen on main, which since the 2026-08 move to
  // trunk-based development is the only branch there is - tagging IS the
  // release decision (releasing.md). One track to fetch here, not two.
  const BRANCH = "main";

  const root = document.getElementById("landscape-app");

  function rawUrl(branch, file) {
    return `https://raw.githubusercontent.com/${REPO}/${branch}/${file}`;
  }

  function parseJsonl(text) {
    return text.split("\n").filter((l) => l.trim().length > 0).map((l) => JSON.parse(l));
  }

  async function fetchTrack() {
    try {
      const resp = await fetch(rawUrl(HISTORY_BRANCH, `external-comparison-${BRANCH}.jsonl`));
      if (!resp.ok) return [];
      return parseJsonl(await resp.text());
    } catch (e) {
      return [];
    }
  }

  async function fetchManifest() {
    try {
      const resp = await fetch(rawUrl(BRANCH, "tests/golden/external-baseline/manifest.json"));
      if (!resp.ok) return null;
      return await resp.json();
    } catch (e) {
      return null;
    }
  }

  // Same client-side, best-effort tag->release join as quality-trend.md -
  // see that page's identical function for the full reasoning.
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
            shaMap[sha].date = rel.published_at || null;
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

  // Every vs_* value in the history file is ours-minus-theirs. For SNR and
  // MOS that means higher is better; for LSD - a distance - it is the other
  // way round, so the colour is chosen here rather than baked into the sign.
  function deltaCell(value, opts) {
    const { lowerIsBetter = false, unit = " dB", digits = 2 } = opts || {};
    if (value === undefined || value === null) return '<span class="landscape-na">n/a</span>';
    const better = lowerIsBetter ? value <= 0 : value >= 0;
    const cls = better ? "landscape-delta-up" : "landscape-delta-down";
    return `<span class="${cls}">${value >= 0 ? "+" : ""}${value.toFixed(digits)}${unit}</span>`;
  }

  function valueCell(value, unit, digits) {
    if (value === undefined || value === null) return '<span class="landscape-na">-</span>';
    return `${value.toFixed(digits)}${unit}`;
  }

  function buildBaselineInfo(manifest) {
    if (!manifest) {
      return '<p class="landscape-baseline">Current baseline metadata unavailable (manifest fetch failed or rate-limited).</p>';
    }
    return `<p class="landscape-baseline">Current baseline (v${manifest.baseline_version}, generated ${manifest.generated_date}):
      FFmpeg <code>${manifest.tools.ffmpeg.version}</code> ·
      DEE <code>${manifest.tools.dee.version}</code> —
      regenerated locally and reviewed by hand, see
      <a href="https://github.com/${REPO}/blob/main/tools/generators/gen_external_baseline.py">tools/generators/gen_external_baseline.py</a>.</p>`;
  }

  function buildTable(records, releasesBySha) {
    // Only landscape rows, and only commits carrying a release tag - this
    // page's entire reason for existing is the release-over-release view,
    // not a running commit-by-commit line (that's tool-comparison-trend.md).
    const rows = records.filter((r) => r.variant === "landscape" && releasesBySha[r.commit]);
    if (rows.length === 0) {
      return '<p class="landscape-status">No tagged-release rows yet - this fills in as releases are cut on main after this page landed.</p>';
    }
    let prevTag = null;
    let groupIndex = -1;
    const trs = rows
      .slice()
      .sort((a, b) => {
        const ra = releasesBySha[a.commit], rb = releasesBySha[b.commit];
        const da = ra.date || a.commit_date, db = rb.date || b.commit_date;
        if (da !== db) return db.localeCompare(da);
        return a.leg.localeCompare(b.leg);
      })
      .map((r) => {
        const release = releasesBySha[r.commit];
        if (release.tag !== prevTag) {
          groupIndex++;
          prevTag = release.tag;
        }
        const stripeCls = groupIndex % 2 === 1 ? " class=\"landscape-stripe-b\"" : "";
        return `<tr${stripeCls}>
          <td><a class="landscape-release" href="${release.url}">🏷 ${release.name}</a>${release.prerelease ? ' <span class="landscape-prerelease">(prerelease)</span>' : ""}</td>
          <td>${(release.date || r.commit_date).slice(0, 10)}</td>
          <td><a href="${commitUrl(r.commit)}">${shortSha(r.commit)}</a></td>
          <td>${r.leg}</td>
          <td class="landscape-group-start">${valueCell(r.snr_db, " dB", 2)}</td>
          <td>${deltaCell(r.vs_ffmpeg_snr_db)}</td>
          <td>${deltaCell(r.vs_dee_snr_db)}</td>
          <td class="landscape-group-start">${valueCell(r.lsd_db, " dB", 2)}</td>
          <td>${deltaCell(r.vs_ffmpeg_lsd_db, { lowerIsBetter: true })}</td>
          <td>${deltaCell(r.vs_dee_lsd_db, { lowerIsBetter: true })}</td>
          <td class="landscape-group-start">${valueCell(r.mos_lqo, "", 2)}</td>
          <td>${deltaCell(r.vs_ffmpeg_mos_lqo, { unit: "" })}</td>
          <td>${deltaCell(r.vs_dee_mos_lqo, { unit: "" })}</td>
          <td class="landscape-group-start">${r.baseline_version !== undefined ? "v" + r.baseline_version : "-"}</td>
        </tr>`;
      })
      .join("");
    return `<div class="landscape-table-wrap"><table>
      <thead>
        <tr>
          <th rowspan="2">Release</th><th rowspan="2">Date</th><th rowspan="2">Commit</th><th rowspan="2">Leg</th>
          <th colspan="3" class="landscape-group-start">SNR — waveform (higher better)</th>
          <th colspan="3" class="landscape-group-start">LSD — envelope (lower better)</th>
          <th colspan="3" class="landscape-group-start">MOS — perceptual (higher better)</th>
          <th rowspan="2" class="landscape-group-start">Baseline</th>
        </tr>
        <tr>
          <th class="landscape-group-start">ac3forge</th><th>vs FFmpeg</th><th>vs DEE</th>
          <th class="landscape-group-start">ac3forge</th><th>vs FFmpeg</th><th>vs DEE</th>
          <th class="landscape-group-start">ac3forge</th><th>vs FFmpeg</th><th>vs DEE</th>
        </tr>
      </thead>
      <tbody>${trs}</tbody>
    </table></div>`;
  }

  Promise.all([fetchTrack(), fetchReleaseShaMap(), fetchManifest()]).then(([records, releasesBySha, manifest]) => {
    root.innerHTML = `
      ${buildBaselineInfo(manifest)}
      ${buildTable(records, releasesBySha)}
    `;
  });
})();
</script>

## Spectrograms

A visual supplement to the table above — one image per leg, each stacking
the original source against ac3forge's own decode and, where the baseline
has a trustworthy score for it (see **n/a** below), FFmpeg's and DEE's own
decodes of the same material at the same bitrate. All four panels are present
on every leg from baseline version 2 onward; before that the two 5.1 legs had
no DEE panel, because DEE had no trustworthy score for them.

<div class="landscape-spectrograms">
  <figure>
    <img src="https://raw.githubusercontent.com/iainchesworthlabs/ac3forge/quality-history/spectrograms/ac3-51-448.png" alt="ac3-51-448 spectrogram: original vs ac3forge vs FFmpeg vs DEE" loading="lazy">
    <figcaption>ac3-51-448 (AC-3, 5.1 @ 448 kbit/s) — synthetic fixture.</figcaption>
  </figure>
  <figure>
    <img src="https://raw.githubusercontent.com/iainchesworthlabs/ac3forge/quality-history/spectrograms/eac3-stereo-192.png" alt="eac3-stereo-192 spectrogram: original vs ac3forge vs FFmpeg vs DEE" loading="lazy">
    <figcaption>eac3-stereo-192 (E-AC-3, stereo @ 192 kbit/s) — synthetic fixture; 96 kbit/s per channel, above both Annex E crossovers, so <code>auto</code> selects no tools here.</figcaption>
  </figure>
  <figure>
    <img src="https://raw.githubusercontent.com/iainchesworthlabs/ac3forge/quality-history/spectrograms/eac3-51-256.png" alt="eac3-51-256 spectrogram: original vs ac3forge vs FFmpeg vs DEE" loading="lazy">
    <figcaption>eac3-51-256 (E-AC-3, 5.1 @ 256 kbit/s) — synthetic fixture.</figcaption>
  </figure>
  <figure>
    <img src="https://raw.githubusercontent.com/iainchesworthlabs/ac3forge/quality-history/spectrograms/eac3-stereo-96.png" alt="eac3-stereo-96 spectrogram: original vs ac3forge vs FFmpeg vs DEE" loading="lazy">
    <figcaption>eac3-stereo-96 (E-AC-3, stereo @ 96 kbit/s) — synthetic fixture; 48 kbit/s per channel, where spectral extension runs and coupling does not.</figcaption>
  </figure>
  <figure>
    <img src="https://raw.githubusercontent.com/iainchesworthlabs/ac3forge/quality-history/spectrograms/eac3-stereo-64.png" alt="eac3-stereo-64 spectrogram: original vs ac3forge vs FFmpeg vs DEE" loading="lazy">
    <figcaption>eac3-stereo-64 (E-AC-3, stereo @ 64 kbit/s) — synthetic fixture; 32 kbit/s per channel, where both coupling and spectral extension run.</figcaption>
  </figure>
  <figure>
    <img src="https://raw.githubusercontent.com/iainchesworthlabs/ac3forge/quality-history/spectrograms/ac3-music-stereo-192.png" alt="ac3-music-stereo-192 spectrogram: original vs ac3forge vs FFmpeg vs DEE" loading="lazy">
    <figcaption>ac3-music-stereo-192 (AC-3, stereo @ 192 kbit/s) — 30 s of real orchestral music. Compare the top octave against the synthetic legs above: this one rolls off, they do not.</figcaption>
  </figure>
  <figure>
    <img src="https://raw.githubusercontent.com/iainchesworthlabs/ac3forge/quality-history/spectrograms/eac3-music-stereo-96.png" alt="eac3-music-stereo-96 spectrogram: original vs ac3forge vs FFmpeg vs DEE" loading="lazy">
    <figcaption>eac3-music-stereo-96 (E-AC-3, stereo @ 96 kbit/s) — real music at the spectral-extension crossover.</figcaption>
  </figure>
  <figure>
    <img src="https://raw.githubusercontent.com/iainchesworthlabs/ac3forge/quality-history/spectrograms/eac3-speech-stereo-64.png" alt="eac3-speech-stereo-64 spectrogram: original vs ac3forge vs FFmpeg vs DEE" loading="lazy">
    <figcaption>eac3-speech-stereo-64 (E-AC-3, stereo @ 64 kbit/s) — 30 s of real connected speech, at the rate where both Annex E tools run.</figcaption>
  </figure>
</div>

<style>
.landscape-spectrograms { display: flex; flex-direction: column; gap: 1.5em; margin: 1.5em 0; }
.landscape-spectrograms figure { margin: 0; }
.landscape-spectrograms img { width: 100%; height: auto; border: 1px solid var(--md-default-fg-color--lightest); border-radius: 0.2em; }
.landscape-spectrograms figcaption { font-size: 0.85em; color: var(--md-default-fg-color--light); margin-top: 0.4em; }
</style>

These are **not** tied to any specific release row above — there is only a
single "latest" image per leg, regenerated on every push to `main` (i.e.
every release promotion, same cadence as a row landing in the table), never
one per historical release. If the image looks newer than the table row
you're comparing it against, it is: the images have no history of their own,
only a current snapshot. They come from the same `quality-history` branch
mechanism as the table's own numbers (see "Where the data lives" below) —
generated in CI by `tools/ci/quality_race.py`'s `render_spectrograms()`
(`trend --spectrogram-dir`), decoding this build's own encode plus the
committed `tests/golden/external-baseline/` FFmpeg/DEE bitstreams — never
invoking FFmpeg's or DEE's own encoder, same boundary as the numbers.

## Reading it

Each row is one (tagged release, leg) result — a release cuts one commit on
`main`, and that commit contributes one row per leg. Alternating row shading
marks where one release's rows end and the next begins, so a release's leg
rows read as one band rather than blending into the wall of numbers below.

There are eight legs as of baseline version 2. The first three go back to the
first baseline and are unchanged, so their series are continuous:

| Leg | Codec | Layout | Rate | Material |
| --- | --- | --- | --- | --- |
| `ac3-51-448` | AC-3 | 5.1 | 448 kbit/s | synthetic |
| `eac3-stereo-192` | E-AC-3 | stereo | 192 kbit/s | synthetic |
| `eac3-51-256` | E-AC-3 | 5.1 | 256 kbit/s | synthetic |
| `eac3-stereo-96` | E-AC-3 | stereo | 96 kbit/s | synthetic |
| `eac3-stereo-64` | E-AC-3 | stereo | 64 kbit/s | synthetic |
| `ac3-music-stereo-192` | AC-3 | stereo | 192 kbit/s | music |
| `eac3-music-stereo-96` | E-AC-3 | stereo | 96 kbit/s | music |
| `eac3-speech-stereo-64` | E-AC-3 | stereo | 64 kbit/s | speech |

The five added at version 2 close two different gaps.

**Rates where the Annex E tools actually run.** ac3forge's `auto` enables
coupling below 12 + 14n kbit/s per channel and spectral extension below 56.
The only stereo leg sat at 192 kbit/s — 96 per channel, above both — so
`auto` chose no tools at all there, and this page had never once compared
this project's Annex E work against FFmpeg's or DEE's at a rate where it
exists. 96 kbit/s stereo is 48 per channel (spectral extension only) and 64
is 32 per channel (both tools).

**Material that is not band-limited.** The synthetic fixtures are 2.5–3 s of
`sin()`, pseudo-random noise and FIR smoothing, which leaves a flat noise
plateau across the whole top octave — a shape no real programme material has,
and one that has already produced a measured, fake 2.1 dB "win" when the
encoder's bandwidth default was tuned against it. The three programme legs
use 30 s CC0 recordings of real speech and music instead. The synthetic legs
stay: their series are the history this page exists to show, and breaking
that continuity to swap the material would throw it away. See
[tools/generators/README.md](https://github.com/iainchesworthlabs/ac3forge/blob/main/tools/generators/README.md)
for the sources, licences and measured spectra.

Three metrics are shown side by side, each with its own **vs FFmpeg** /
**vs DEE** delta against that tool's number for the same leg at the baseline
version shown. Green always means ac3forge came out better, which is a
*higher* number for SNR and MOS and a *lower* one for LSD — the stored
deltas are all plainly ours-minus-theirs, and only the colouring knows which
way each metric points.

No one of the three is the headline, and that is deliberate. E-AC-3's
coupling and spectral extension trade waveform fidelity for banded envelope
fidelity **on purpose** — that is what they are for — so waveform SNR alone
reports a working tool as a straight loss, while LSD alone rewards one that
has thrown the waveform away. Reading all three together is the only way the
comparison says something true about the encoder rather than about the
metric. (The per-tool detail behind that trade is in
[Tool comparison trend](tool-comparison-trend.md).)

**n/a** on a `vs DEE` or `vs FFmpeg` cell means that side of the comparison
has no real number for that leg and metric — not that the comparison came out
even. **-** in an ac3forge cell means the metric was not scored for that row
at all: LSD is a measure of what the Annex E tools trade away, so it is scored
on the E-AC-3 legs only and the AC-3 rows leave it blank.

Two long-standing sources of **n/a** are gone as of baseline version 2:

- Both 5.1 legs used to have no DEE number, because the installed DEE build
  drops the surround-left channel when 5.1 arrives as one discrete
  6-channel file. Feeding it one mono WAV per channel instead
  (`--input-format wav_list`) does not, so the `vs DEE` cells on `ac3-51-448`
  and `eac3-51-256` carry real numbers now.
- Every MOS cell was empty, in every row ever published, because CI did not
  install `visqol-python` — so both sides of the delta were missing. It is
  installed now, and baseline version 2 was generated with it, so both sides
  exist.

Releases from before those two changes keep their `n/a` cells; nothing is
back-filled, because the numbers were never measured.

The baseline itself — FFmpeg's and DEE's actual encoded output — is
regenerated locally, occasionally, and reviewed by hand as a normal PR (see
`tools/generators/gen_external_baseline.py`'s own docstring); it is never re-run
automatically, and never runs in CI. The **Baseline** column names which
version of it a given release's numbers were compared against, so a jump in
that column marks where the external side of the comparison changed, not
ac3forge's own encoder.

**MOS** is [ViSQOL](https://github.com/google/visqol)'s MOS-LQO (Mean
Opinion Score - Listening Quality Objective) in audio mode, a perceptual-
quality prediction from 1 (bad) to a ceiling around 4.75 — see [Tool
comparison trend](tool-comparison-trend.md#reading-it) for why ViSQOL over
PEAQ. Of the three metrics it is the only one that tries to answer "which
sounds better", which is why it is worth having beside the two that answer
narrower questions exactly.

Unlike SNR and LSD, which span the whole fixture, MOS is scored on a fixed
4 s window taken from the middle of it (`MOS_WINDOW_S` in
`tools/ci/quality_race.py`). ViSQOL's patch matching is super-linear —
3.9 s of compute for 3 s of audio, 127.8 s for 30 s — so scoring the
programme legs whole would cost more than the rest of the CI job put
together. The window is the same deterministic span every run, so the
release-to-release deltas this page is read for are unaffected; only the
absolute value carries a small constant offset (~0.02 MOS against a much
longer window, measured).

## Listening test

Everything above this section is a waveform or model measure. SNR and LSD are
distances; MOS-LQO is a *prediction* of what a panel would say, from a model
trained on panels — which is closer to "how it sounds" than a distance is, and
still not a listener. This section is where a real one goes.

**No session has been run yet.** The apparatus is in the repository
([`tools/listening/`](https://github.com/iainchesworthlabs/ac3forge/tree/main/tools/listening));
the listening is human time that has not been spent. The results table below
is empty and says so rather than carrying placeholder numbers, and
`README.md`'s and [Validation](verification.md)'s "no listening test has been
run" sentences stay as they are until it is not.

### Protocol

**Systems under test.** The same three legs as the table above, and the same
conditions:

| Leg | Codec | Layout | Rate | Arms |
|---|---|---|---|---|
| `ac3-51-448` | AC-3 | 5.1 | 448 kbit/s | ac3forge, FFmpeg |
| `eac3-stereo-192` | E-AC-3 | stereo | 192 kbit/s | ac3forge, FFmpeg, DEE |
| `eac3-51-256` | E-AC-3 | 5.1 | 256 kbit/s | ac3forge, FFmpeg |

The 5.1 legs have no DEE arm, for the reason their `vs DEE` cells are already
**n/a** above: that DEE build drops the Ls channel on discrete 6-channel
input, so its 5.1 output is marked unverified in the baseline manifest. A
stimulus nobody should draw a conclusion from is worse than a missing one.

Each leg also carries BS.1534-3's hidden reference and its two low-pass
anchors, 3.5 kHz (mandatory) and 7 kHz (recommended).

**One decoder for everything.** Every stimulus, including ac3forge's own
encode, is decoded by FFmpeg. This is deliberately the opposite of what the
trend legs do. A listening test compares *encoders*; if each encoder's output
went through its own decoder, the panel would be scoring encoder-and-decoder
pairs and no result could be attributed to either. One decoder makes the
decoder a constant, and FFmpeg is the one all three encoders have in common.
Neither FFmpeg's nor DEE's *encoder* is ever run — the external arms are the
committed `tests/golden/external-baseline/` bitstreams, the same boundary the
numbers above observe.

**Alignment and level.** Codec delay is removed by cross-correlation and every
condition trimmed to a common length, so switching between them mid-item does
not click. Levels are deliberately *not* re-normalized per condition: a level
change introduced by the apparatus would be scored as an artifact of the
encoder.

**Method.** BS.1534-3 MUSHRA where a panel can be staffed — all of a leg's
conditions presented together against a labelled reference, scored 0–100, with
BS.1534-3 post-screening (a listener who scored the hidden reference below 90
on more than 15% of trials is excluded, reported by name with their numbers).
Forced-choice A/B/X otherwise, which works with one listener and answers a
narrower question — not "how much worse", only "could this listener tell them
apart at all" — with an exact one-sided binomial p-value against the 0.5
guessing rate. 24 trials per pair puts the p &lt; 0.05 threshold at 17 correct.

**Two limits of the synthetic fixtures**, both detected and reported by the
stimulus generator rather than left to be discovered mid-session:

- **The anchors do not work on the 5.1 legs.** A low-pass anchor only anchors
  the scale if removing the band above its cutoff is audible.
  `reference_51.wav` carries 0.059% of its total energy above 3.5 kHz (−32.3
  dB) and 0.031% above 7 kHz, so both anchors are near-transparent copies of
  the reference there — a panel would correctly score them near 100, which
  leaves the bottom of the scale undefined and the session unscalable against
  any other panel's. `reference_stereo.wav` is fine (−5.0 dB and −11.5 dB).
- **The items are 1.9 seconds long.** BS.1534-3 asks for excerpts of about 10
  s. The synthetic fixtures are 2.5–3.0 s before alignment trims them, and
  they are `sin()` and filtered noise, not programme.

Both are recorded per session in `session.json` alongside the seed, render
mode and baseline version, so a session's own limits travel with its numbers.

Both were what roadmap VX7 was for, and **VX7 has landed**: two 30 s CC0
fixtures, full-band speech and music, are committed as
`tests/golden/audio/programme_{speech,music}_stereo.flac` and carry three legs
of their own in the external-baseline manifest — `ac3-music-stereo-192`,
`eac3-music-stereo-96` and `eac3-speech-stereo-64`. `gen_listening_stimuli.py`
walks every leg in that manifest, so those three already get a stimulus set
built with no change to the generator; the two limits above are properties of
the synthetic legs specifically, and a MUSHRA session on the programme stereo
legs (with ABX still the right method for the synthetic 5.1 ones) is the
session worth running. What has not happened is the running of it: that is
human listening time, and roadmap VX9 stays open until it is spent.

**Monitoring.** `--render native` presents the 5.1 legs as 5.1 and needs a 5.1
setup; `--render stereo` renders them through FFmpeg's own downmix (which
applies the stream's `cmixlev`/`surmixlev`) for a headphone session. Which was
used is part of the result — the two are not the same experiment.

**Decoder complaints ride through to the table.** FFmpeg reports two
out-of-range exponents decoding DEE's own committed stereo stream. A concealed
error is a real artifact a listener hears, but it is that decoder reading that
stream, not DEE's encoder being worse, and any row scored from a flagged
stimulus carries the flag.

### Results

No session has been run, so there are no numbers here. When one is,
`score_listening_test.py --markdown-out` produces the table that goes in this
space: one row per (leg, condition) with the mean score and a 95% confidence
interval for MUSHRA, or one row per (leg, system) with proportion correct, a
95% Wilson interval and an exact binomial p for ABX.

### Reproducing the stimulus set

After building `ac3cli`:

```bash
AC3CLI=build/config-linux-llvm/bin/ac3cli python3 tools/listening/gen_listening_stimuli.py --out listening-session
```

[`tools/listening/README.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/tools/listening/README.md)
is the operator's sequence, including what a session needs from a person.

## Where the data lives

Same `quality-history` branch mechanism as
[Quality trend](quality-trend.md#where-the-data-lives) and
[Tool comparison trend](tool-comparison-trend.md#where-the-data-lives) -
this page reads `external-comparison-main.jsonl` specifically (releases only
ever happen on `main`) and filters it down to commits the GitHub API reports
as tagged, joined client-side the same way quality-trend.md's own release
badges are.
