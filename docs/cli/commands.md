# Commands

The command list from the usage text — copied from a build of `ac3cli`, not retyped:

```text
ac3forge — clean-room AC-3 / E-AC-3 (ATSC A/52) encoder/decoder

Usage:
  ac3cli --version    print version and git provenance, then exit
  ac3cli help [<command>|exit-codes]   this list, or one command's own help
  ac3cli silence      <out.ac3> [seconds] [bitrate_kbps]
  ac3cli sine         <out.ac3> [seconds] [bitrate_kbps] [freq_hz] [amp_pct] [layout]
  ac3cli orbit        <out.ac3> [seconds] [bitrate_kbps] [orbit_seconds]
  ac3cli atmos        <out.ec3> [seconds] [bitrate_kbps] [objects] [orbit_seconds] [mode]
  ac3cli atmos-path   <out.ec3> <paths.txt> [seconds] [bitrate_kbps] [objects] (objects driven by an authored keyframe file instead of the built-in orbit)
  ac3cli atmos-encode <in.wav> <out.ec3> [bitrate_kbps] [objects] [paths.txt] (every source channel as an object; optional: authored per-object motion from a keyframe file (same format as atmos-path), objects it doesn't mention keep their default placement)
  ac3cli atmos-adm    <in.adm.wav> <out.ec3> [bitrate_kbps] [programme_id] (UNAVAILABLE HERE)
  ac3cli record       <out.ac3|out.ec3> [seconds] [bitrate_kbps] [device_index] (capture straight to a file; layout=/codec=/container= decide its shape)
  ac3cli live         <out.ac3|out.ec3> <capture_device> [seconds] [bitrate_kbps] [monitor_device] [passthrough_device] [mode] (capture -> encode -> live monitor and/or passthrough)
  ac3cli encode       <in.wav> <out.ac3> [bitrate_kbps] [layout] [in2.wav] (in2.wav: layout 1+1's Ch2, when Ch1 is a separate mono file; or use src=/map= for more than one source)
  ac3cli eac3-silence <out.ec3> [seconds] [bitrate_kbps] [layout]
  ac3cli eac3-sine    <out.ec3> [seconds] [bitrate_kbps] [freq_hz] [amp_pct] [layout]
  ac3cli eac3-encode  <in.wav> <out.ec3> [bitrate_kbps] [tools] [layout] [vbr] [in2.wav] (in2.wav: layout 1+1's Ch2, when Ch1 is a separate mono file; or use src=/map= for more than one source)
  ac3cli decode       <in.ac3|in.ec3> <out.wav> [objects_dir] (AC-3 or E-AC-3; bsid decides. objects_dir (E-AC-3 Atmos only): export each JOC-reconstructed object as its own object_NN.wav there)
  ac3cli levels       <in.wav|in.ac3|in.ec3>                  (per-channel peak/RMS report)
  ac3cli loudness     <in.wav>                                (BS.1770-4 loudness -> dialnorm)
  ac3cli qc           <in.ac3|in.ec3> [preset=<name>|all]     (bitstream-aware loudness QC: measured loudness vs. embedded dialnorm/compr, optional preset gate)
  ac3cli spdif        <in.ac3> <out.wav>                      (IEC 61937 wrap as playable PCM16 WAV)
  ac3cli mkv          <in.ac3|in.ec3> <out.mkv>               (wrap as a playable Matroska file)
  ac3cli mp4          <in.ac3|in.ec3> <out.mp4>               (wrap as a playable MP4 with a spec-correct dac3/dec3 box)
  ac3cli fmp4         <in.ac3|in.ec3> <out_dir> [frames_per_fragment] (fragmented MP4/CMAF + HLS/DASH manifests, ready for a packager)
  ac3cli ts           <in.ac3|in.ec3> <out.ts>                (wrap as an MPEG-2 Transport Stream (DVB profile))
  ac3cli devices                                              (input and loopback capture endpoints)
  ac3cli outputs                                              (render endpoints + AC-3/E-AC-3 passthrough support)
  ac3cli play         <in.ac3|in.ec3> [device_index]          (exclusive-mode IEC 61937 passthrough; bsid decides AC-3 vs E-AC-3)
  ac3cli monitor      <in.ac3|in.ec3> [device_index]          (decode and play on an ordinary (non-bitstreamed) output)
  ac3cli help         [<command>|exit-codes]                  (one command's own arguments and grammars, not the whole manual)
  ac3cli man                                                  (the generated groff man page, on stdout)
  ac3cli completions  <bash|zsh|fish|powershell>              (the generated completion script for that shell, on stdout)
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
| `mkv` | Wraps AC-3 or E-AC-3 as Matroska, reading format/packet boundaries/sample rate/channel count from the bitstream itself so the container can't be told the wrong ones |
| `mp4` | Wraps AC-3 or E-AC-3 as a single-file MP4/ISOBMFF, writing a spec-correct `dac3`/`dec3` sample-entry box (fscod/bsid/bsmod/acmod/lfeon, plus the Atmos complexity-index extension for JOC content) read straight off the bitstream |
| `ts` | Wraps AC-3 or E-AC-3 as an MPEG-2 Transport Stream (PAT + PMT + one PES-wrapped audio PID), identified per the DVB profile — `stream_type` 0x06 plus the `AC3_descriptor`/`Enhanced_AC3_descriptor` ETSI EN 300 468 Annex D defines, not ATSC's |
| `fmp4` | Writes fragmented MP4/CMAF — an init segment plus one media segment per fragment — alongside an HLS media+master playlist pair and a DASH MPD, all pointing at the same segments, ready for a real HLS/DASH origin or packager. `[frames_per_fragment]` defaults to 48 access units per fragment, about 1.5 s at 48 kHz. Atmos content signals `CHANNELS="<N>/JOC"` in the HLS playlists automatically |

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
| `record` | Captures from a device straight to a file, metering live. `layout=`/`codec=` choose the shape (any layout up to 7.1.4, AC-3 or E-AC-3), `container=` the wrapper (`raw`, `mkv`, `ts`, `spdif`), `watchdog=` how long a silent device is tolerated |
| `play` | Exclusive-mode IEC 61937 passthrough of an existing file — `bsid` decides AC-3 vs. E-AC-3 |
| `monitor` | Decodes an existing file and plays it on an ordinary, non-bitstreamed output — the shared-mode preview path. For an Atmos-mode stream, this plays the 5.1 **bed** and reports the object count found: the decoder reads TS 103 420's object layer (OAMD/JOC) but this path does not render or export objects, so this is what a legacy decoder hears, not unmixed objects — use `decode` with `objects_dir` for the object audio itself. |
| `live` | Capture → encode → optional live monitor and/or IEC 61937 passthrough, running continuously, still writing the file `record` always has. Everything `record` takes, plus a second clock-conformed capture device (`capture2=`), an object-slot budget and `map=` binding (`objects=`, `mode=atmos`), and a parallel 5.1 AC-3 leg for an AC-3-only receiver (`downmix=`) |

`live`'s device arguments: `monitor_device`/`passthrough_device` take `-2` (default, leaves that
leg off), `-1` (the default render endpoint), or an index from `outputs`. Either or both legs may
run alongside the file `live` always writes.

`live mode` (also shared with `atmos`): `channels` (default) encodes the captured channels onto
`layout=` — stereo by default, anything up to 7.1.4 — placing them by direction the way `encode`
places a file's; `atmos` pans capture channels into a 5.1 bed as objects, moving each one every
frame the same way `atmos`'s synthetic orbit does — the hook a real live position source drops
into once one exists. `layout=`/`codec=` describe a channel session only; `mode=atmos` always
encodes the TS 103 420 shape and refuses either.

**Take shape and durability.** `record` and `live` both take `layout=`, `codec=`,
`container=raw|mkv|ts|spdif` and `watchdog=<seconds>` (see
[Options & grammars](metadata-options.md#recordlive-options-record-live-container-layout-codec-watchdog)).
All four containers are written **incrementally, as the take runs**, through the same
`RecordingSink` the GUI's own takes go through — so a take costs one frame of memory rather than
the whole session, and a crash an hour in leaves an hour of playable file. A capture device that
stops delivering audio ends the session as a failure (exit `5`) rather than leaving it looking
healthy with nothing coming in; whatever was already written stays on disk.

**Objects on a live session.** `live mode=atmos` allocates its object slots once, at session
start — `objects=<N>` sets the budget, `map=` binds capture channels to slots with the same
`obj`/`objm`/`none` grammar the Format tab's assignment table uses (see
[Options & grammars](metadata-options.md#objects-and-map-modeatmos)). A slot with nothing bound to
it is carried silent rather than changing the object count a decoder read from the first access
unit.

**An AC-3-only receiver.** When the session's stream needs E-AC-3 but the chosen
`passthrough_device` only bitstreams plain AC-3, `live` sends it a parallel 5.1 AC-3 encode of the
bed the main plan already computed, while the file and the monitor still carry the full stream.
`downmix=off` restores the plain refusal.

`live capture2=<index>`: the `capture_device` positional stays the session's clock master, paced
exactly as it always has been; `capture2=` adds a second, independently-clocked device (see
[Options & grammars](metadata-options.md#capture2) for the full grammar) whose
stream is resampled to track the master, with the measured drift printed at session end.

### Self-description & scripting

| Command | What it does |
|---|---|
| `help` | With no argument, the whole usage listing. With a command name, just that command's row and the option grammars it actually uses — not the ~130 lines of prose an argument error used to print. `help exit-codes` prints the exit-code table below. |
| `man` | A section-1 groff man page on stdout, generated from the same command table. `cmake --build` writes it to `ac3cli.1` and `cmake --install` puts it under `share/man/man1`. |
| `completions` | A completion script for `bash`, `zsh`, `fish` or `powershell` on stdout, generated from the same table plus the option list. Installed under the conventional per-shell directories; the Homebrew formula places the bash and fish halves through Homebrew's own helpers. |

`ac3cli <command> --help` (or `-h`) is `ac3cli help <command>` spelled the other way round, and
works even when the rest of the command line would not have satisfied that command.

Every command also takes `quiet` (no status output at all — errors and, for a `-` output, the
payload) and `verbose` (the stderr progress line whatever the run's length). See
[Options & grammars](metadata-options.md#common-options-every-command-quiet-verbose).

### Exit codes

| Code | Meaning |
|---|---|
| `0` | Success |
| `1` | Usage — a bad or missing argument, an unknown command or option, or a configuration the encoder cannot express |
| `2` | Input — unreadable, absent, or not a valid stream |
| `3` | Output — the destination could not be created, written or finalized |
| `4` | Unavailable here — this build or machine cannot run the command at all |
| `5` | Runtime — the run started and then failed (a capture dropout, a measurement with nothing to measure) |
| `6` | A QC gate failed |
| `7` | Internal — an exception escaped a command |

`ac3cli help exit-codes` prints the same table with a sentence on each; see
[Options & grammars](metadata-options.md#exit-codes) for the full descriptions and a worked
`qc` example.

## Next

[Options & grammars](metadata-options.md) — the options encoding commands take after their
positional arguments (and which commands ignore which), plus the full `layout`, `tools` and
`vbr` grammars.
