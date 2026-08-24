# Commands

The command list from the usage text — copied from a build of `ac3cli`, not retyped:

```text
ac3forge — clean-room AC-3 / E-AC-3 (ATSC A/52) encoder/decoder

Usage:
  ac3cli --version    print version and git provenance, then exit
  ac3cli silence      <out.ac3> [seconds] [bitrate_kbps]
  ac3cli sine         <out.ac3> [seconds] [bitrate_kbps] [freq_hz] [amp_pct] [layout]
  ac3cli orbit        <out.ac3> [seconds] [bitrate_kbps] [orbit_seconds]
  ac3cli atmos        <out.ec3> [seconds] [bitrate_kbps] [objects] [orbit_seconds] [mode]
  ac3cli atmos-path   <out.ec3> <paths.txt> [seconds] [bitrate_kbps] [objects] (objects driven by an authored keyframe file instead of the built-in orbit)
  ac3cli atmos-encode <in.wav> <out.ec3> [bitrate_kbps] [objects] [paths.txt] (every source channel as an object; optional: authored per-object motion from a keyframe file (same format as atmos-path), objects it doesn't mention keep their default placement)
  ac3cli atmos-adm    <in.adm.wav> <out.ec3> [bitrate_kbps] [programme_id] (UNAVAILABLE HERE)
  ac3cli record       <out.ac3> [seconds] [bitrate_kbps] [device_index]
  ac3cli live         <out.ac3|out.ec3> <capture_device> [seconds] [bitrate_kbps] [monitor_device] [passthrough_device] [mode] (capture -> encode -> live monitor and/or passthrough)
  ac3cli encode       <in.wav> <out.ac3> [bitrate_kbps] [layout] [in2.wav] (in2.wav: layout 1+1's Ch2, when Ch1 is a separate mono file; or use src=/map= for more than one source)
  ac3cli eac3-silence <out.ec3> [seconds] [bitrate_kbps] [layout]
  ac3cli eac3-sine    <out.ec3> [seconds] [bitrate_kbps] [freq_hz] [amp_pct] [layout]
  ac3cli eac3-encode  <in.wav> <out.ec3> [bitrate_kbps] [tools] [layout] [vbr] [in2.wav] (in2.wav: layout 1+1's Ch2, when Ch1 is a separate mono file; or use src=/map= for more than one source)
  ac3cli decode       <in.ac3|in.ec3> <out.wav> [objects_dir] (AC-3 or E-AC-3; bsid decides. objects_dir (E-AC-3 Atmos only): export each JOC-reconstructed object as its own object_NN.wav there)
  ac3cli probe        <in.ac3|in.ec3> [json=1] [detail=frames|blocks] (what the stream declares: layout, substreams, rates, metadata ranges, object layer, tool usage and per-frame CRC - as a table, or as a documented JSON contract)
  ac3cli levels       <in.wav|in.ac3|in.ec3>                  (per-channel peak/RMS report)
  ac3cli loudness     <in.wav>                                (BS.1770-4 loudness -> dialnorm)
  ac3cli qc           <in.ac3|in.ec3> [preset=<name>|all]     (bitstream-aware loudness QC: measured loudness vs. embedded dialnorm/compr, optional preset gate)
  ac3cli spdif        <in.ac3> <out.wav>                      (IEC 61937 wrap as playable PCM16 WAV)
  ac3cli unspdif      <in.wav|in.raw|-> <out.ac3|out.ec3|->   (the inverse: recover the elementary stream from IEC 61937 bursts, as captured from an S/PDIF or HDMI input or written by 'spdif'. '-' pipes either end)
  ac3cli mkv          <in.ac3|in.ec3> <out.mkv>               (wrap as a playable Matroska file)
  ac3cli mp4          <in.ac3|in.ec3> <out.mp4>               (wrap as a playable MP4 with a spec-correct dac3/dec3 box)
  ac3cli fmp4         <in.ac3|in.ec3> <out_dir> [frames_per_fragment] (fragmented MP4/CMAF + HLS/DASH manifests, ready for a packager)
  ac3cli ts           <in.ac3|in.ec3> <out.ts>                (wrap as an MPEG-2 Transport Stream (DVB profile))
  ac3cli devices                                              (input and loopback capture endpoints)
  ac3cli outputs                                              (render endpoints + AC-3/E-AC-3 passthrough support)
  ac3cli play         <in.ac3|in.ec3> [device_index]          (exclusive-mode IEC 61937 passthrough; bsid decides AC-3 vs E-AC-3)
  ac3cli monitor      <in.ac3|in.ec3> [device_index]          (decode and play on an ordinary (non-bitstreamed) output)
```

## By category

### Synthesis — generate a stream from nothing

No source file needed; useful for smoke-testing a build or a receiver without recording
anything first.

| Command | What it writes |
|---|---|
| `silence` | Silent AC-3 |
| `sine` | A tone, one per speaker, AC-3. Append `c` to `[layout]` (e.g. `stereoc`) to turn on channel coupling. |
| `orbit` | AC-3 with a synthetic panned source circling the room (exercises the [spatial layer](../library/spatial-and-atmos.md) — plain bed panning, no object metadata) |
| `atmos` | E-AC-3 with synthetic orbiting Atmos objects — a 5.1 bed plus JOC + OAMD side data (TS 103 420) |
| `atmos-path` | Same, but object motion comes from an authored keyframe file instead of the built-in orbit |

The keyframe file `atmos-path` reads (and `atmos-encode`'s optional `[paths.txt]`, below) is
plain text, one keyframe per line as whitespace-separated columns
`object_index time_s x y z gain lfe_send`; `#` starts a comment and blank lines are skipped.
It is the same grammar the GUI's timeline exports — see
[GUI → Objects & motion](../gui/objects-and-motion.md).

```bash
ac3cli eac3-sine out.ec3 5 384 1000 50 714
```

Five seconds at 50% amplitude, 384 kbps, one tone per coded channel — 14 of them for a 7.1.4
layout, though only 12 reach speakers: the two bed channels the dependent substreams replace
carry tones a full decoder never renders. (The 1000 Hz argument applies only to a one- or
two-channel layout; anything wider gets a distinct spread of per-channel frequencies instead,
so a misrouted channel is identifiable by ear.)

### File encoding — real audio in, a stream out

| Command | What it does |
|---|---|
| `encode` | WAV → AC-3. Without `[layout]`, follows the source channel count (1→mono, 2→stereo, 3–6→5.1); a wider source is refused, since no AC-3 coding mode is wider than 3/2 + LFE. |
| `eac3-encode` | WAV → E-AC-3, with the Annex E `tools:` token and an optional `vbr:` token available (see [Options & grammars](metadata-options.md)). Without `[layout]`, follows the source channel count (1→mono, 2→stereo, 3–6→5.1, 8→7.1, 10→5.1.4, 12→7.1.4). |
| `atmos-encode` | WAV → E-AC-3 Atmos, every source channel becomes its own object; optional `[paths.txt]` drives per-object motion from an authored keyframe file the same way `atmos-path` does, keyed by WAV channel index — an object it doesn't mention keeps its default (fanned-out) placement |

```bash
ac3cli encode in.wav out.ac3 448 couple
```

448 kbps, channel coupling on, layout inferred from the WAV's channel count.

`1+1` (dual mono — two independent programmes, never inferred from a channel count, so it always
has to be named explicitly) takes its two channels either as one two-channel file or as two mono
ones:

```bash
ac3cli encode both.wav out.ac3 192 1+1                    # Ch1/Ch2 = channels 0/1 of both.wav
ac3cli encode narration_en.wav out.ac3 192 1+1 narration_fr.wav  # Ch1, Ch2 as separate files
```

See [Options & grammars](metadata-options.md) for `dialnorm2=` — Ch2's own dialnorm, alongside
the usual `dialnorm=`.

`encode`, `eac3-encode` and `atmos-encode` all take `-` in place of `<in.wav>` or the output path
to mean stdin or stdout, so a pipeline never has to touch a temporary file:

```bash
ac3cli encode - - 448 couple < in.wav > out.ac3
```

The status text these commands normally print (frame count, routing, per-channel levels,
`dialnorm=auto`'s measurement line) goes to stderr instead of stdout whenever the output side is
`-`, so it never ends up inside the piped stream — `src=`/`map=` multi-source runs included.

### ADM ingest — real professional master files (opt-in, roadmap B1)

**Only *runnable* in a build with `-DAC3FORGE_BUILD_ADM=ON`** — but always *listed*, the same
"a command a build cannot run is shown, not hidden" treatment the live-audio commands below get
(see that section's own note): a default build's usage block at the top of this page shows this
row as `UNAVAILABLE HERE` instead of the description below, and running it prints a clear reason
(`ac3cli atmos-adm ...` → `error: 'atmos-adm' is unavailable on this platform: this build was not
configured with -DAC3FORGE_BUILD_ADM=ON ...`) rather than "unknown command". Every command besides
this one builds and works identically whether that flag is on or off — this is the one exception,
because it is the one command that needs `ac3adm::ac3adm`/`ac3::admbridge`, this project's sole
opt-in, Boost-requiring module (default **off** — see
[ADM / BW64 reading](../library/adm.md#why-opt-in)). What the row looks like in a build configured
with the flag on (the usage block at the top of this page is copied from a *default* build, where
this row instead reads `UNAVAILABLE HERE`):

```text
  ac3cli atmos-adm    <in.adm.wav> <out.ec3> [bitrate_kbps] [programme_id] (a real ADM BWF master (BS.2076-2 ADM XML + BW64/RF64, roadmap B1) straight to DD+ JOC E-AC-3; every bed/object channel the resolved audioProgramme names becomes an AtmosEncoder object, driven by the file's own authored automation - no keyframe file needed. Only in builds with -DAC3FORGE_BUILD_ADM=ON)
```

| Command | What it does |
|---|---|
| `atmos-adm` | A real ADM BWF master (professional delivery format Netflix's and Apple's own Atmos ingest pipelines require) straight to DD+ JOC E-AC-3 — no WAV, no hand-authored keyframe file: [`ac3::admbridge::build`](../library/adm-bridge.md) classifies every channel as a bed speaker feed or a dynamic object and builds its own `ac3::oba::ObjectPath` straight from the file's authored BS.2076-2 §10.3 position/gain automation, driven frame by frame the same way `atmos-encode` drives an authored `[paths.txt]` |

```bash
ac3cli atmos-adm master.wav out.ec3 448
```

448 kbps, the file's lowest-ID `audioProgramme` (BS.2076-2 §5.8's own default-selection rule).
Pass a fourth argument to pick a different one by ID:

```bash
ac3cli atmos-adm master.wav out.ec3 448 APR_1002
```

`dialnorm=` works the same as every other encoding command (see
[Options & grammars](metadata-options.md)); `dialnorm=auto` does not — an ADM document's bed/object
channels have no single fixed layout to measure loudness against the way `atmos-encode`'s WAV
input does, so `atmos-adm` refuses it with a clear error rather than silently keeping the default.

Every failure — a container/XML parse error (`ac3adm::AdmError`) or a graph-resolution error
(`ac3::admbridge::BridgeError`, e.g. no `audioProgramme`, an unresolved reference, an unsupported
pack type) — prints a real diagnosis via that error's own `describe()`, never an opaque crash or a
bare non-zero exit.

See [ADM / BW64 reading](../library/adm.md) and [ADM → Atmos bridging](../library/adm-bridge.md)
for the parser and the mapping layer this command drives, and
[`examples/encode_adm.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/encode_adm.cpp)
for the same pipeline as a minimal, standalone, self-fixturing program.

### Decoding & inspection

| Command | What it does |
|---|---|
| `decode` | AC-3 or E-AC-3 → WAV; `bsid` in the stream decides which decoder runs. For an Atmos E-AC-3 stream, reports the object count found and, with `objects_dir`, exports each JOC-reconstructed object as its own `object_NN.wav` there |
| `probe` | What a stream *declares*, without rendering its audio: bsid, sample rate, layout, substream map, counts, duration, bit rate, metadata ranges, EMDF/OAMD/JOC, authenticity, per-frame CRC and coding-tool usage. Human table by default, or the `ac3forge.probe/1` JSON document with `json=1` |
| `levels` | Per-channel peak/RMS report — takes a WAV or an encoded stream |
| `loudness` | BS.1770-4 gated loudness on a WAV, reported as the `dialnorm` it implies |
| `qc` | Bitstream-aware loudness QC: decodes an already-encoded AC-3/E-AC-3 stream, measures it with the real BS.1770-4/EBU Tech 3342 meter, and compares the result against the stream's own embedded `dialnorm`/`compr` and, optionally, a named delivery-spec gate |

```bash
ac3cli decode out.ec3 out.wav
```

`decode` takes `-` for either path too, the same convention the encoding commands above use:

```bash
ac3cli decode - - < out.ac3 > out.wav
```

It decodes on the fast (FFT) inverse-transform path by default; `mode=reference` or
`fast-imdct=off` selects the spec's direct evaluation instead — see
[Validation → Performance and reference modes](../verification.md#performance-and-reference-modes)
for what each mode is for, and [Options & grammars](metadata-options.md) for the token rules.

For an Atmos stream, add `objects_dir` to also export each object's reconstructed audio:

```bash
ac3cli decode atmos.ec3 bed.wav objects/
```

#### `probe` — what the stream says about itself

Every other inspection here goes through the audio: `levels` and `qc` decode the whole
programme to measure it, and `decode` reports an object count on its way past. `probe` asks
the other question — what the bitstream *declares* — and answers it without reconstructing a
single sample:

```bash
ac3cli probe programme.ec3
```

```text
file            programme.ec3
codec           E-AC-3 (bsid 16)
sample rate     48000 Hz
bsmod           0 (complete main)
layout          3/2 + LFE (acmod 7, lfeon 1)
renders         6 channel(s): L C R Ls Rs LFE
blocks          6 per syncframe (numblkscod 3)
substreams      1 per access unit
                independent id 0, 3/2 + LFE, 63 syncframe(s), -
access units    63 (63 syncframe(s)), 112896 bytes
duration        2.016 s
bit rate        448.0 kbit/s measured
rate control    constant (1792 .. 1792 bytes per access unit)
dialnorm        -31 dB
compr           absent
dynrng          absent
EMDF            payload id(s) 11 (OAMD), 14 (JOC)
object audio    5 object(s): bed LFE only, 4 dynamic, in 63 frame(s)
complexity      5
JOC             present
authenticity    no tag
CRC             63 of 63 syncframe(s) valid
tools           378 block(s) parsed
  delta ba      372 of 378 block(s)
  skip field    63 of 378 block(s)
  exponents     reuse 1890, D15 378, D25 0, D45 0
```

It reads a stream in two tiers, and the distinction is the point. The **header tier** —
syncinfo plus the whole of bsi — answers for every syncframe whether or not its audio is
readable, so a stream this decoder refuses is still described in full. The **parse tier** runs
the real decoder with the inverse transform switched off, which is where the `dynrng` words,
the EMDF payload ids, the object layer and the per-block tool usage come from. A syncframe the
parse tier declines is counted and reported; the header tier's answers for it stand.

That is not a hypothetical: the committed DEE-encoded E-AC-3 baseline is exactly such a stream.
`ac3cli decode` on it fails with `decode failed (code 5)` and stops. `probe` reports its layout,
rate, duration, substream map and CRC state, says that 76 of its 79 syncframes were refused and
why, and tells you it uses AHT — which is the first thing a bug report about it would need.

**Exit code** is 0 only when every syncframe passed its CRC *and* the parser accepted it, so
`probe` works as a pipeline gate without anything having to read its output:

```bash
ac3cli probe delivery.ec3 || echo "stream is not clean"
```

Memory is flat in the length of the stream: input is pulled through a fixed window rather than
loaded, and the per-frame dump is written as the walk produces it. Probing a two-hour file costs
what probing a two-second one does. `-` in place of the path reads the stream from stdin, so
`probe` sits in a pipe.

##### Per-frame and per-block detail

`detail=frames` adds one entry per access unit — byte offset, size, timestamp, and each
syncframe's own header, CRC state and object layer. `detail=blocks` adds every block's coding
tools and exponent strategies underneath: the C++ counterpart of `tools/references/eac3_parse.py`,
and what a codec bug report actually needs.

```bash
ac3cli probe programme.ec3 detail=blocks
```

```text
access unit 0 @ 0 (768 bytes, t=0.0000s)
  independent id 0 @ 0: 768 bytes, 2/0 stereo, crc ok, dialnorm -31 dB
    blk 0: cpl+dither+remat             exp [D45 D45 cpl:D45]
    blk 1: cpl+dither                   exp [D15 D15 cpl:D15]
    blk 2: cpl+dither                   exp [reuse reuse cpl:reuse]
```

##### JSON output (`json=1`)

`json=1` emits the same walk as a JSON document instead. The schema is a **stable contract** —
sibling tooling is built on it (an HLS/DASH manifest check comparing a `dec3` box against the
real substream map is the natural next consumer), so the rules below are commitments, not
description.

```bash
ac3cli probe programme.ec3 json=1
```

**Versioning.** The top-level `schema` member names the contract: `"ac3forge.probe/1"`. Within
one version, members are only ever *added*; an existing member never changes its type, its units
or its meaning, and never disappears. A member that does not apply to a given stream is present
and `null` (or `false`/`[]`), never omitted — a consumer must not have to tell "no such key"
apart from "no such thing". A change that would break any of that changes the version.

**Ordering.** `access_units` is written *before* `stream`, because the summary is only complete
once every unit has been walked and the per-frame dump has to stream. JSON member order carries
no meaning, so this costs a consumer nothing — but do not build anything that depends on the
opposite order.

**Units.** `dialnorm_db`/`dialnorm2_db` are reported in **dB** (negative), not as the
transmitted 1..31 code — the field means −1..−31 dB LKFS and that is what a delivery spec is
written in. `compr`, `compr2`, `dynrng` and `dynrng2` are the raw 8-bit words, because their
meaning is a non-linear gain curve (§7.7) and a *range* of gains is not a well-defined thing to
report. `bitrate_kbps` is measured over the whole stream; `nominal_bitrate_kbps` is AC-3's
declared Table 5.18 rate and is `null` for E-AC-3, which has no such field.

Top level:

| Member | Type | Meaning |
|---|---|---|
| `schema` | string | `"ac3forge.probe/1"` |
| `generator` | string | The `ac3cli` version that wrote it |
| `file` | string | The input path as given |
| `access_units` | array | Present only with `detail=` — see below |
| `stream` | object | The summary |

`stream`:

| Member | Type | Meaning |
|---|---|---|
| `codec` | `"ac3"` or `"eac3"` | Which generation, from `bsid` |
| `bsid`, `bsmod` | int | §5.4.1.3 / §5.4.2.1 as transmitted; `bsmod_label` names it (Table 5.7) |
| `sample_rate_hz` | int | 48000/44100/32000, or Annex E's 24000/22050/16000 |
| `reduced_rate` | bool | The rate came from `fscod2` (§E2.3.1.3) |
| `acmod`, `lfeon` | int, bool | As transmitted; `layout_label` names the pair |
| `numblkscod`, `blocks_per_syncframe` | int | §E2.3.1.4; always 6 for AC-3 |
| `coded_channels` | int | What the independent substream itself codes |
| `rendered_channels` | int | What the program renders, every dependent's `chanmap` unioned in (§E3.8.2) |
| `layout` | array of string | Table E2.5 locations, in order. Empty for 1+1 dual mono, which has no layout |
| `substreams` | array | One entry per `(stream_type, substream_id)` identity, with its own `bsid`/`bsmod`/`acmod`/`lfeon`/`numblkscod`/`chanmap` and the `syncframes` that carried it |
| `substreams_per_access_unit` | int | Substreams in the first access unit |
| `access_units`, `syncframes`, `bytes` | int | Extent |
| `duration_seconds` | float | From the coded block counts, not a container timestamp |
| `bitrate_kbps` | float | Measured over the whole stream |
| `nominal_bitrate_kbps` | int or null | AC-3's declared rate; `null` for E-AC-3 |
| `variable_bitrate` | bool | Access units differ in size |
| `access_unit_bytes` | `{min, max}` | The spread behind that flag |
| `metadata` | object | `dialnorm_db`, `dialnorm2_db`, `compr`, `compr2`, `dynrng`, `dynrng2`, each `{present, min, max}` with `min`/`max` `null` when `present` is false |
| `objects` | object | `complexity_index` (TS 103 420 §8.3.2.2, from `addbsi`), `oamd`, `joc`, `emdf_payload_ids`, and the program the first OAMD payload described: `total`, `dynamic`, `bed`, `bed_mask`, `lfe`, plus the `frames` that carried one |
| `authenticity` | `{present, tagged_syncframes}` | Whether frames carry an authenticity tag. Answered **without a key** — where the tag lives is fixed by the container, and only whether it *matches* needs the key (that is `decode ... verify-objects`) |
| `integrity` | object | `crc_valid`, `crc_failures`, `parse_failures`, `first_parse_error` |
| `tools` | object | `blocks` parsed, then how many of them used `coupling`, `enhanced_coupling`, `spectral_extension`, `block_switch`, `dither`, `rematrixing`, `delta_bit_alloc`, `skip_field`; `aht_syncframes` and `transient_prenoise_syncframes` are counted in frames, since Table E1.3 decides them per frame; `exponent_strategy` totals `reuse`/`D15`/`D25`/`D45` over every coded stream of every block |

`access_units[]` (with `detail=`): `index`, `byte_offset`, `bytes`, `start_seconds`, and
`syncframes[]` — each with its own `byte_offset`, `bytes`, `stream_type`, `substream_id`,
`bsid`, `bsmod`, `acmod`, `lfeon`, `numblkscod`, `dialnorm_db`, `compr`, `chanmap`, `crc_valid`,
`authenticity_tag`, `parse_error` and `objects`. With `detail=blocks` each syncframe also carries
`frame_tools` (Table E1.3's frame-level gates, `aht_streams`, `snroffststr`,
`per_block_exp_strategy`) and `blocks[]` — per block: `parsed`, `coupling`,
`enhanced_coupling`, `spectral_extension`, `block_switch` and `dither` (per-channel bit masks),
`rematrixing`, `delta_bit_alloc`, `skip_field`, `skip_bytes`, `exponent_strategy` (one entry per
coded channel, LFE last) and `coupling_exponent_strategy`.


`qc` is `loudness`'s bitstream-aware counterpart: `loudness` measures a *source* WAV before encoding, `qc` measures what a stream actually *delivers* after encoding and decoding it back, and checks that against what the stream's own metadata claims:

```bash
ac3cli qc programme.ec3
```

```text
qc: programme.ec3 (E-AC-3, 3/2 + LFE, 48000 Hz, 938 access unit(s), 30.02 s)
measured (BS.1770-4 gated / EBU Tech 3342 / BS.1770-4 Annex 2):
  integrated loudness    -22.87 LKFS
  loudness range          4.31 LU
  true peak               -1.62 dBTP
embedded metadata:
  dialnorm              24  (claims dialogue at -24.00 LKFS)
  compr                absent
dialnorm check:
  claimed                -24.00 LKFS  (from dialnorm 24)
  delta                   +1.13 dB    (measured - claimed; positive = measured is louder)
  measurement-derived dialnorm would be 23, not 24
```

Add `preset=<name>` (or `preset=all`) to gate that same measurement against a named delivery spec instead of just reporting it — see [Options & grammars](metadata-options.md#qc-options-qc-preset) for the exact preset numbers and the primary source cited for each, and this page's own exit-code note below.

`qc`'s exit code is 0 only when the file decodes cleanly **and** (if a preset was given) every requested gate passes — non-zero otherwise, which is what makes it usable as an actual CI/pipeline QC step: `ac3cli qc out.ec3 preset=ebu-r128-s2 || echo "loudness QC failed"`. With no `preset=` at all it only ever measures and reports (no verdict to fail), so a plain `ac3cli qc <file>` exits non-zero solely on a genuine decode error.

### Containers

| Command | What it does |
|---|---|
| `spdif` | Wraps AC-3 or E-AC-3 as IEC 61937 bursts inside a playable PCM16 WAV — `bsid` in the stream decides which, and the E-AC-3 carrier runs at four times the content sample rate. For feeding a receiver through an ordinary audio path |
| `unspdif` | The inverse of `spdif`: reads IEC 61937 bursts back and writes the AC-3 or E-AC-3 elementary stream inside them. Takes the WAV `spdif` writes, a capture of an S/PDIF or HDMI input, or a bare dump of carrier bytes with no RIFF header at all — the data type in `Pc` decides AC-3 vs. E-AC-3, and both 16-bit word orders are read. Nothing is re-encoded: the output is what the source sent, byte for byte. `-` works on either end — a capture tool piped straight in, the stream piped straight out — with the report going to stderr, same convention as `encode`/`decode` |
| `mkv` | Wraps AC-3 or E-AC-3 as Matroska, reading format/packet boundaries/sample rate/channel count from the bitstream itself so the container can't be told the wrong ones |
| `mp4` | Wraps AC-3 or E-AC-3 as a single-file MP4/ISOBMFF, writing a spec-correct `dac3`/`dec3` sample-entry box (fscod/bsid/bsmod/acmod/lfeon, plus the Atmos complexity-index extension for JOC content) read straight off the bitstream |
| `ts` | Wraps AC-3 or E-AC-3 as an MPEG-2 Transport Stream (PAT + PMT + one PES-wrapped audio PID), identified per the DVB profile — `stream_type` 0x06 plus the `AC3_descriptor`/`Enhanced_AC3_descriptor` ETSI EN 300 468 Annex D defines, not ATSC's |
| `fmp4` | Writes fragmented MP4/CMAF — an init segment plus one media segment per fragment — alongside an HLS media+master playlist pair and a DASH MPD, all pointing at the same segments, ready for a real HLS/DASH origin or packager. `[frames_per_fragment]` defaults to 48 access units per fragment, about 1.5 s at 48 kHz. Atmos content signals itself automatically and completely: `CHANNELS="<N>/JOC"` in the HLS playlists, the two `EC3_ExtensionType`/`EC3_ExtensionComplexityIndex` supplemental descriptors ETSI TS 103 420 clause D.2 defines in the MPD, and the `ceao` compatibility brand its Annex E requires on the segments. Every representation also states its channel configuration, on the Dolby scheme TS 102 366 clause I.1.2.1 defines |

### Live & hardware

Needs the platform's capture/passthrough backend — see the per-OS Platform notes pages
([Windows](../platforms/windows.md), [Linux](../platforms/linux.md),
[Raspberry Pi](../platforms/raspberry-pi.md), [macOS](../platforms/macos.md),
[Android](../platforms/android.md)) for what's actually confirmed against real hardware on
each OS.

| Command | What it does |
|---|---|
| `devices` | Lists capture endpoints (microphones, playback-device loopbacks) |
| `outputs` | Lists render endpoints and whether each supports AC-3/E-AC-3 passthrough |
| `record` | Captures from a device straight to an AC-3 file, metering live — `container=mkv` writes straight to Matroska instead, `container=fmp4` a folder of CMAF segments and manifests. If the endpoint turns out to be bitstreaming IEC 61937 rather than delivering PCM (an HDMI/S/PDIF capture card, or a loopback of a player set to bitstream), `record` recognises that within about a quarter of a second and writes the **elementary stream** instead of encoding the bursts as if they were audio — see [passthrough capture](#passthrough-capture) below |
| `play` | Exclusive-mode IEC 61937 passthrough of an existing file — `bsid` decides AC-3 vs. E-AC-3 |
| `monitor` | Decodes an existing file and plays it on an ordinary, non-bitstreamed output — the shared-mode preview path. For an Atmos-mode stream, this plays the 5.1 **bed** and reports the object count found: the decoder reads TS 103 420's object layer (OAMD/JOC) but this path does not render or export objects, so this is what a legacy decoder hears, not unmixed objects — use `decode` with `objects_dir` for the object audio itself. |
| `live` | Capture → encode → optional live monitor and/or IEC 61937 passthrough, running continuously, still writing the file `record` always has; optionally a second, clock-conformed capture device via `capture2=`, or straight to Matroska via `container=mkv` or to a live fragmented-MP4/CMAF origin via `container=fmp4` |

`live`'s device arguments: `monitor_device`/`passthrough_device` take `-2` (default, leaves that
leg off), `-1` (the default render endpoint), or an index from `outputs`. Either or both legs may
run alongside the file `live` always writes.

`live mode` (also shared with `atmos`): `channels` (default) carries stereo straight through;
`atmos` pans every captured channel into a 5.1 bed as its own object, moving it every frame the
same way `atmos`'s synthetic orbit does — the hook a real live position source drops into once
one exists.

`live container=fmp4`: the output path names a **folder**, written as the session runs rather than
at the end — `init.mp4` first, then a `segment*.m4s` for each fragment as it closes, with
`audio.m3u8`/`master.m3u8`/`manifest.mpd` rewritten each time. While the session is running those
manifests are live-shaped (no `#EXT-X-ENDLIST`, a `type="dynamic"` MPD with an
`availabilityStartTime`), so the folder is a servable origin mid-session; a clean stop flushes the
trailing partial fragment and closes both to their VOD/static forms. `fmp4-window=<n>` lists only
the last *n* segments, for an origin that deletes segments behind itself. Nothing is held in
memory beyond one fragment, unlike `container=mkv`/`raw`, which still accumulate the take. See
[Options & grammars](metadata-options.md#recordlive-options-record-live-container) for the full
grammar.

`live capture2=<index>`: the `capture_device` positional stays the session's clock master, paced
exactly as it always has been; `capture2=` adds a second, independently-clocked device (see
[Options & grammars](metadata-options.md#live-options-live-capture2) for the full grammar) whose
stream is resampled to track the master, with the measured drift printed at session end.

### Passthrough capture

A capture endpoint fed IEC 61937 hands the bursts over as ordinary PCM. Nothing in any capture
API says "this is Dolby Digital", so a recorder that takes the samples at face value encodes
noise. `record` and `live` both look for the burst framing — a `Pa`/`Pb` preamble every
repetition period with a `0x0B77` syncframe behind it — over roughly the first quarter-second of
each session, and act on what they find:

- **`record`** switches to writing the elementary stream. Nothing is re-encoded, the `bitrate`
  argument stops applying, and the output is bit-identical to what the source sent. The carrier
  already gone past is kept, so the recording starts at the first burst rather than a
  quarter-second into it. A device running at a rate AC-3 cannot encode at — 192 kHz is exactly
  the E-AC-3 carrier's 4× — is no longer refused outright: that rate is now checked only once
  the bitstream question has been answered no.
- **`live`** stops with an error naming `record` and `unspdif`. A live session mixes, resamples
  a second device into lockstep, meters, monitors and can pan objects, none of which mean
  anything applied to burst data; switching modes mid-session would produce a file whose first
  quarter-second is a different thing from the rest.

For a capture already saved to disk, `unspdif` does the same job offline.

None of this has been confirmed against a real HDMI or S/PDIF capture device — see
[Windows](../platforms/windows.md#audio-backend-wasapi) for exactly what is and is not verified
against hardware. What is verified is the framing itself, both ways, against FFmpeg's `spdif`
muxer as an independent oracle.

## Next

[Options & grammars](metadata-options.md) — the options encoding commands take after their
positional arguments (and which commands ignore which), plus the full `layout`, `tools` and
`vbr` grammars.
