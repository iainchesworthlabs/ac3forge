# Generators

Almost everything in this directory produces a **committed artefact**: a
fixture, a table, or a baseline. Each script is run by hand, its output is
reviewed as a normal PR diff, and the output is what the rest of the project
actually depends on - so a change here is a change to the ground truth every
measurement sits on, not an implementation detail. Three do not fit that:
`gen_dee_gold.py` and `gen_ac4_baseline.py --gold-set` write local sets that are
never committed, and `gen_conformance_vectors.py` builds the bundle a release
publishes. CI runs that last one (`.github/workflows/_ci-linux.yml`) and none of
the others.

Some of them extract tables from specification text (AC-3, E-AC-3 and JOC in
`gen_aht_tables.py`, `gen_bitalloc_tables.py` and `gen_joc_tables.py`; AC-4 in
`gen_ac4_tables.py` and `gen_ac4_reference_tables.py`), some produce golden
vectors, and the rest produce fixtures: the **fixture corpus** described below,
the Dolby Encoding Engine's baselines, and the streams the device and
bare-metal probes decode.

## The fixture corpus

`tests/golden/audio/` is versioned as a single corpus, described by
`tests/golden/audio/corpus.json` and enforced by `tools/checks/check_corpus.py`.
The manifest carries, per fixture: channels, sample rate, bit depth, duration,
SHA-256, and — for the programme fixtures — the upstream source, its own
SHA-256, its licence, and the exact excerpt window taken from it.

`corpus_version` (in `gen_programme_fixtures.py`, copied into the manifest) is
bumped by hand whenever a fixture's bytes change. That matters because a
regenerated fixture is close to invisible: it still decodes, still has the
right duration and channel count, still produces a plausible SNR — and every
series in [Landscape](../../docs/landscape.md),
[Quality trend](../../docs/quality-trend.md) and
[Tool comparison trend](../../docs/tool-comparison-trend.md) simply acquires a
step in it that reads as an encoder change. `check_corpus.py` turns that into
a failing check instead.

| Fixture | Kind | Layout | Length | Produced by |
| --- | --- | --- | --- | --- |
| `reference_51.wav` | synthetic | 5.1, 16-bit | 2.50 s | `gen_gold_reference_wav.py` |
| `reference_stereo.wav` | synthetic | stereo, 16-bit | 3.00 s | `gen_stereo_reference_wav.py` |
| `programme_speech_stereo.flac` | speech | stereo, 16-bit | 30.00 s | `gen_programme_fixtures.py` |
| `programme_music_stereo.flac` | music | stereo, 16-bit | 30.00 s | `gen_programme_fixtures.py` |
| `reference_objects.wav` | synthetic | five mono objects, one per channel, 16-bit | 2.00 s | `gen_object_scene_wav.py` |
| `reference_objects.paths` | object placements | text, in `atmos-path`'s keyframe format | - | `gen_object_scene_wav.py` |
| `reference_51_eac3_448k_cplbndstrce0.ec3` | bitstream | — | — | FFmpeg (captured, see below) |

The last of those is not audio material and no generator here produces it: it
is a real FFmpeg-encoded E-AC-3 stream, committed so
`tools/checks/verify_gold_reference.sh` can check this project's decoder
against a third-party bitstream that sets `cplbndstrce == 0` — Annex E's
default coupling band structure, which nothing this project's own encoder
emits ever does, and which a real decoder bug hid behind until FFmpeg's output
was first tried against it. Its bytes matter for the same reason the audio
fixtures' do, so it is hashed in the manifest too, with no audio parameters to
check. `check_corpus.py` also fails on any file in `tests/golden/audio/` that
the manifest does not list, so a new fixture cannot arrive unregistered.

### Synthetic and programme material are both kept, on purpose

The synthetic pair is built from `sin()`, pseudo-random noise and boxcar-FIR
smoothing. That has a specific, measurable consequence: a boxcar FIR rolls off
far too slowly to stop white noise, so both fixtures carry a **flat noise
plateau from 12 kHz all the way to Nyquist**, at roughly the level of the
content below it. Real programme material has nothing of the kind — it rolls
off monotonically.

Mean power per band, in dB relative to each fixture's own 200 Hz – 2 kHz mean,
measured on the four files as they ship:

| Fixture | 4–8k | 8–12k | 12–14.7k | 14.7–16k | 16–18k | 18–20k | 20–24k |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `reference_51` | −39.3 | −43.5 | −46.1 | −45.6 | −47.0 | −47.6 | −47.7 |
| `reference_stereo` | −10.5 | −35.4 | −38.6 | −37.5 | −39.9 | −40.8 | −40.2 |
| `programme_music` | −27.9 | −43.3 | −75.9 | −85.6 | −90.2 | −91.3 | −91.3 |
| `programme_speech` | −19.6 | −35.4 | −44.6 | −60.1 | −87.8 | −89.0 | −89.1 |

This project has already paid for that difference once. `EncoderConfig`'s
bandwidth default was swept against the synthetic fixtures and narrowing
looked like a **2.1 dB SNR win** at 448 kbit/s — because discarding the top
9 kHz of a flat noise plateau costs almost nothing, while doing the same to
real material throws away real energy. The comment recording that trap is in
[`src/forge/src/encoder/encoder.cpp`](../../src/forge/src/encoder/encoder.cpp)
above `chbwcod`.

The synthetic fixtures are **not** retired, for a different reason: the
published trend series are only comparable to each other because the material
under them never moved, and those series go back to the first baseline.
Swapping the material would throw away the history the pages exist to show.
So the programme fixtures were added as their own legs alongside them.

Two caveats worth knowing before tuning anything against these files:

- The programme fixtures' own flat tail above ~17 kHz is **16-bit
  quantisation noise**, not the synthetic plateau returning. It sits at −90 dB
  where the synthetic plateau sits at −47 dB. In the 24-bit sources those
  bands measure −96/−110/−118 dB (music) and −91/−93/−94 dB (speech).
- `programme_speech_stereo.flac`'s source has a filter cliff at about 16 kHz.
  It is full-band in the sense that matters — a natural, monotonic rolloff
  rather than a plateau — but it is not evidence about anything above 16 kHz.
  Use the music fixture for that; it rolls off cleanly to 24 kHz.

Both programme fixtures are stereo. There is no redistributable native 5.1
programme source, and a matrix upmix of a stereo recording would put derived,
correlated content in the surrounds and say more about the upmix than about
the encoder — so the 5.1 legs stay synthetic.

### Licences and provenance

Both programme fixtures are **CC0 1.0 Universal** (public-domain dedication).
No attribution is required; it is recorded here anyway because provenance is
the point.

| Fixture | Source | Licence |
| --- | --- | --- |
| `programme_speech_stereo.flac` | ["Sally Mann at VMFA 2024-12-05"](https://commons.wikimedia.org/wiki/File:Sally_Mann_at_VMFA_2024-12-05.flac), Wikimedia Commons. 48 kHz, 24-bit stereo FLAC, 31.25 s of unscripted connected speech recorded in a room. | [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) |
| `programme_music_stereo.flac` | Mendelssohn, Symphony No. 4 "Italian", IV. Saltarello (Presto), from [Musopen's Kickstarter recordings](https://archive.org/details/MusopenKickstarterRecordingsLossless). 48 kHz, 24-bit stereo ALAC, 367.5 s. | [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) |

The source files themselves are **not committed** — they are 60 MB and 2.2 MB,
against 3.1 MB for both trimmed 30 s excerpts together. `corpus.json` records
each source's URL and SHA-256 so the excerpt can be reproduced exactly, and
`gen_programme_fixtures.py` refuses to build from a source whose hash does not
match.

The fixtures are committed as FLAC rather than WAV for the same size reason
(5.8 MB of WAV each). Consumers go through `quality_race.py`'s
`materialise_fixture()`, which decodes to a cached WAV under `build/` on
demand, so nothing else in the tree has to learn about FLAC.

## Running them

All of these are run from the repo root.

| Script | Produces | Notes |
| --- | --- | --- |
| `gen_gold_reference_wav.py` | `tests/golden/audio/reference_51.wav` | stdlib only |
| `gen_stereo_reference_wav.py` | `tests/golden/audio/reference_stereo.wav` | stdlib only |
| `gen_object_scene_wav.py` | `tests/golden/audio/reference_objects.wav` and `.paths`, the fixed five-object scene of the object-reconstruction quality leg | stdlib only |
| `gen_gui_resample_test_wav.py` | the GUI's 44.1 kHz resample fixture | stdlib only |
| `gen_programme_fixtures.py` | both programme fixtures + `corpus.json` | needs `--source-dir` and `ffmpeg` |
| `gen_external_baseline.py` | `tests/golden/external-baseline/` | needs **Dolby DEE**, `ffmpeg`, a built `ac3cli` |
| `gen_object_fixture.py` | `tests/golden/object-fixture/dee_joc_514.ec3`, DEE's DD+ JOC stream of a synthetic 5.1.4 bed, and the bed | needs **Dolby DEE**; local only |
| `gen_dee_gold.py` | a local set of DEE's AC-3, E-AC-3, E-AC-3 JOC and TrueHD streams, never committed | needs **Dolby DEE**, `ffmpeg`; `--cli` records a built `ac3cli`'s reading |
| `gen_dee_tpn_fixture.py` | `tests/golden/external-baseline/eac3-transient-stereo-128/` | needs the local DEE golden-master set (`--gold`) |
| `gen_ac4_baseline.py` | `tests/golden/external-baseline/ac4-*/dee.ac4` with `ac4-manifest.json`, and each stream's syntax digest under `tests/golden/ac4dec/`; with `--gold-set DIR` the local AC-4 gold set instead, never committed | needs **Dolby DEE**; `--cli` records a built `ac3cli`'s reading of the gold set |
| `gen_ac4_presentation_sources.py` | `tests/golden/ac4dec/presentations/sources/`, the six encoder-made substreams the presentation tests multiplex | needs a built `ac3cli` (`--cli`) |
| `gen_aht_tables.py`, `gen_bitalloc_tables.py`, `gen_joc_tables.py` | `aht_tables.hpp` and `bitalloc_tables.hpp` in `src/forge/include/ac3/core/`, `joc_tables.hpp` in `src/forge/include/ac3/oba/` | read spec text (and JOC's tables attachment), not committed |
| `gen_ac4_tables.py` | `src/ac4core/src/tables/`: the Huffman codebooks, the scale factor band tables, and the noise, QMF and intermediate spatial format tables | reads the spec texts and both tables attachments from `--spec-dir` (default `spec/`), stdlib only; runs its checks before it writes |
| `gen_ac4_reference_tables.py` | `tools/references/ac4_tables.py`, the Python transcription's tables, written separately from the C++ ones | the same sources, stdlib only |
| `gen_ac4_qmf_twiddles.py` | `src/ac4core/src/tables/qmf_twiddles.hpp`, the cosines the AC-4 QMF banks' twiddle factors are built from | stdlib only; `--check` compares the committed header |
| `gen_qmf_prototype.py` | `src/forge/src/dsp/qmf_prototype.hpp`, the 64-band complex QMF prototype filter of the JOC reconstruction | needs numpy; deterministic, takes a couple of minutes |
| `gen_mdct_goldens.py` | `tests/golden/mdct_goldens.hpp`, filterbank golden vectors | needs numpy and the A/52 text |
| `gen_baremetal_fixture.py` | `apps/baremetal/fixture.hpp`, the AC-3 and E-AC-3 streams the minimum-footprint decoder probe decodes and their per-channel levels | needs a built `ac3cli` (`--ac3cli`) |
| `gen_baremetal_ac4_fixture.py` | `apps/baremetal/ac4_fixture.hpp`, the committed AC-4 streams the bare-metal AC-4 probe decodes and their per-channel levels | needs a built `ac3cli` (`--ac3cli`); the E-AC-3 probe's `fixture.hpp` is `gen_baremetal_fixture.py`'s |
| `gen_device_streams.py` | `esp-idf/ac3forge/examples/hearth_sink/www/`, the stream set the ESP32 player's HTTP source is pointed at, and its `streams.json` | needs numpy and a built `ac3cli` (`--ac3cli`) |
| `gen_pseudo_locale.py` | `apps/gui/translations/ac3gui_xx.ts`, the GUI's pseudo-locale fixture | stdlib only; run after `ac3gui_lupdate` (see [Localisation](../../docs/forge/gui/localisation.md)) |
| `gen_conformance_vectors.py` | the conformance vector bundle a release publishes (`--out`), not committed | needs a built `ac3cli` (`--cli`); CI runs it |

Regenerating a programme fixture:

```bash
python tools/generators/gen_programme_fixtures.py --source-dir /path/to/sources
```

It prints the exact URL for any source it cannot find, and verifies each
source's SHA-256 before touching anything. Bump `CORPUS_VERSION` in the same
change if the output bytes move, then re-run `python
tools/checks/check_corpus.py`.

The generators that run Dolby DEE - `gen_external_baseline.py`, `gen_dee_gold.py`,
`gen_ac4_baseline.py` and `gen_object_fixture.py` - must **never** run in CI: DEE
is licensed commercial software, and each refuses to start if `GITHUB_ACTIONS` is
set. `gen_external_baseline.py` also invokes FFmpeg's encoder. See its module
docstring for the leg list, the DEE input-path constraint, and how the manifest
it writes is consumed.

`gen_dee_gold.py` writes a set that is not committed: it makes DEE's golden
masters for AC-3, E-AC-3, E-AC-3 JOC and TrueHD while DEE's licence runs (it ends
on 2026-11-06), a set of about 1,100 streams kept on a local disk with a
manifest, each rebuildable from the committed programme fixtures.
`gen_ac4_baseline.py --gold-set` does the same for AC-4; without `--gold-set` it
writes the short streams under `tests/golden/external-baseline/` that CI reads.
