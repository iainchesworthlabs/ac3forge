# Validation

Quality is measured, not asserted, and coverage has known edges. This page is both: how output
is checked, and exactly where checking runs out.

## Five independent checks

In rough order of strength:

1. **The in-repo decoder.** Fully normative and shares the encoder's core, so a round trip
   exercises the bit-allocation model in both directions. It reaches float32-precision PCM
   parity with FFmpeg's decoder on identical streams: max sample difference 7.9e-6 (≈ −102 dBFS)
   for AC-3, 1.4e-5 for E-AC-3. It also reads FFmpeg's own encoder output, Dolby Encoding
   Engine's, and pinned commercial-encoder excerpts from FFmpeg's FATE archive — see
   [Third-party bitstreams](#third-party-bitstreams) for what that corpus is, and for the five
   decoder defects wiring it up exposed.
2. **FFmpeg as an external oracle.** Every stream this project produces is strict-decoded with
   `-xerror -err_detect crccheck+bitstream+buffer+explode`, which fails on a CRC error, a
   bitstream violation or a buffer problem rather than concealing it. Automated and required in
   CI.
3. **Independent Python transcriptions.** `tools/` holds second implementations of the spec
   pseudocode, written from the standard separately from the C++: the §7.2.2 bit allocation, the
   Tables 7.29/7.30 DRC lookups, MDCT goldens. Agreement between two transcriptions of the same
   text is weaker evidence than a decoder, but it catches transcription slips that a
   self-consistent round trip cannot.
4. **Dolby's own tooling as a syntax oracle.** The Reference Player and the Dolby Media Encoder
   were diffed field-for-field against this encoder's output during the object work. That found
   several real bugs — the EMDF container belonging in a skip field rather than the aux field,
   `codecdatae=0`, a dynamic-object-only programme with the LFE as an object but not a JOC
   output, and metadata flag arrays transmitted index-0-first.
5. **Fuzzing, in both directions.** Into the decoder: the libFuzzer harnesses under `fuzz/` drive
   the codec's untrusted-input entry points looking for crashes and undefined behaviour
   (ASan+UBSan), and two differential harnesses decode each mutated stream with both this
   project's decoder and FFmpeg's and diff the PCM. CI runs both: the `Fuzz Regress` job replays
   the checked-in seed and regression corpora on every push and PR, and the `Fuzz Differential`
   job adds a bounded mutation budget on pushes.

   Out of the encoder: `tools/ci/fuzz_encoder_space.py` draws random legal encoder configurations
   crossed with adversarial PCM — transients, silence↔loud transitions inside one frame, spectral
   jumps between blocks, dense harmonics, clipping — and holds every stream it produces against
   both decoders. This is the one check here that varies the *input material* rather than the
   option list; it exists because an encoder defect that produced streams both decoders reject
   needed a specific input shape to reach, and so escaped every other check on this page. Bounded
   on every pull request, deeper nightly. See
   [fuzz/README.md](https://github.com/iainchesworthlabs/ac3forge/blob/main/fuzz/README.md).

Contributor-facing detail on which oracle to reach for and how — including the exact FFmpeg
flags and the CI jobs that run them — is in [Oracles](https://github.com/iainchesworthlabs/ac3forge/blob/main/CONTRIBUTING.md#oracles).

## Quality

`tools/ci/quality_race.py` synthesizes stereo programme material, encodes it with both ac3forge
and FFmpeg at matched bit rates, decodes both with FFmpeg as a neutral referee, aligns by
cross-correlation, and reports SNR against the original:

| Bit rate | ac3forge | FFmpeg | Difference |
|---|---|---|---|
| 192 kbps | 41.23 dB | 40.98 dB | +0.25 |
| 256 kbps | 44.00 dB | 42.85 dB | +1.15 |
| 320 kbps | 45.09 dB | 44.15 dB | +0.94 |
| 448 kbps | 51.05 dB | 47.60 dB | +3.46 |

Measured with FFmpeg 8.0.1 on 2026-08-09; reproduce with `python tools/ci/quality_race.py ac3`.
SNR on synthetic material is a narrow metric — it says the waveform is closer, not that it
sounds better, and no *subjective* listening test has been run. `quality_race.py`'s tables (and
[Tool comparison trend](tool-comparison-trend.md)/[Landscape](landscape.md)) also carry an
objective perceptual-quality prediction alongside SNR, [ViSQOL](https://github.com/google/visqol)'s
MOS-LQO — narrower than a real listening panel, but closer to "how it would sound" than a
waveform-distance number, and something SNR alone cannot claim. It's an optional column
(`visqol-python` not installed shows `-`, never a failure), so it isn't in the snapshot table
above; see `perceptual_score()` in `tools/ci/quality_race.py`.

That is a one-off snapshot. [Quality trend](quality-trend.md) tracks the same gold-reference SNR
by commit, on every push to `develop` and `main`, so a regression shows up as a trend line
rather than only in that run's CI log.

## Performance and reference modes

Both transform hot spots — the forward MDCT (§8.2.3.2) and the inverse transform's step-3
complex sum (§7.9.4) — exist in two evaluations: the spec's own direct form, and a fast path
through a shared radix-2 FFT core. The direct forms are the *reference*: they are what the
standard states, and every fast path is validated against its direct counterpart by the test
suite (max peak-normalized relative error ~3e-12 forward, 7.8e-14 inverse; end-to-end agreement
331 dB direct-vs-fast for encode, 214.9/284.7 dB SNR for AC-3/E-AC-3 decode over 180 seconds of
real material). The fast paths are the default, because that evidence was reviewed and accepted
before each default flipped.

Both evaluations are gated end to end, not only at the transform level. The `linux-gcc` leg
runs `tools/checks/verify_gold_reference.sh` twice — once as it stands, once with
`TRANSFORM_MODE=reference` — so the direct forms face the same FFmpeg-oracle SNR floors on the
same real streams as the fast paths, and `tools/ci/run_codec_matrix.sh` carries `fast-mdct=off`
and `fast-imdct=off` rows through the sanitizers. Without that, a change to a fast path could
take its own reference with it and nothing outside the transform unit tests would notice.

`ac3cli` exposes the pair as one intent-level switch: `mode=reference` runs every transform in
the command on the direct evaluations — for regenerating fixtures, comparing sample-for-sample
against an external decoder, or isolating a suspected transform defect — and `mode=performance`
(the default state) names the fast paths. The per-transform escape hatches `fast-mdct=off` and
`fast-imdct=off` adjust one half at a time; see
[Options & grammars](cli/metadata-options.md#command-specific-notes) for the full token
semantics. At the library level the same pair is `EncoderConfig::fast_mdct` /
`eac3::FrameConfig::fast_mdct` and `DecoderConfig::fast_imdct`. Encoded output never depends on
the decode-side switch: the encoder's own internal inverse-transform uses are pinned to the
direct form regardless of any mode.

## Test suite

The Catch2 suites (`ac3tests` plus the `ac3perf` throughput suite) plus one `ctest` entry per
example program, run per platform. The GUI's Qt Quick Test harness (`ac3gui_qmltests`) adds one
entry on a GUI-enabled build, and the ALSA backend's `tests/backend/alsa/` adds 14 on a Linux
build with libasound present; `ctest` runs whatever the configuration registered:

```bash
ctest --preset test-windows-msvc-debug
```

## Third-party bitstreams

**There are no free AC-3 or E-AC-3 conformance bitstreams.** ATSC A/52 and ETSI TS 102 366 are
both freely downloadable *documents*, but neither body publishes conformance *vectors* for these
codecs the way MPEG does for its own, and Dolby's verification material ships under licence with
its professional tools. Everything below is the substitute, and it is worth being explicit that
it is one: a corpus of real third-party encoder output with no normative expected decode
attached to it, not a conformance suite.

Two tiers, both gated in CI:

- **Committed** — `tests/golden/external-baseline/` holds six streams from Dolby Encoding Engine
  6.5.4 and FFmpeg 8.0.1, each encoded from this repository's own source WAVs (see
  `tools/generators/gen_external_baseline.py`). `tools/checks/verify_gold_reference.sh` decodes
  all six with `ac3cli` on every gold-reference leg and diffs each against FFmpeg's own decode,
  with per-fixture floors quoted beside the measured numbers in the script. They also seed the
  decoder fuzzers, so mutation starts from third-party structure rather than only from this
  project's own encoder output.
- **Fetched** — `tools/checks/verify_fate_interop.py` pulls seven SHA-256-pinned samples from
  FFmpeg's FATE archive and holds each against FFmpeg's own decode. These are excerpts of
  commercially mastered programme material, encoded years ago by whatever encoder the mastering
  house used, and they exercise choices neither this project's encoder nor FFmpeg's makes:
  spectral extension at 128 and 256 kbit/s, 1536 kbit/s, a director's-commentary track, dither
  in use, and the 3/1 acmod nothing in this tree can encode. Fetched at run time and never
  committed — they are film excerpts, and pinning by hash is what keeps an upstream change from
  quietly moving the numbers. Runs nightly in the `Interop` workflow.

Wiring up the first tier found **five separate Annex E decoder defects** in a single sitting, on
syntax that no stream this project can encode is able to reach — the three AHT-in-use flags read
unconditionally, `cplfgaincod`/`cplfsnroffst` not read at all, the three band-structure default
tables applied in the wrong blocks, the `first*` per-frame coordinate states approximated as
"block 0", and a missing coupling-state reset. Four of the six fixtures did not decode at all
before that. It is the clearest evidence on this page for why a self-consistent round trip, an
independent transcription and a second decoder driven by the same encoder are all still not the
same thing as reading somebody else's bitstream.

One divergence is recorded rather than resolved:

- **FFmpeg mis-decodes DEE's stereo E-AC-3 stream.** It reports `exponent 25 is out-of-range`
  and `error decoding the audio block` on frame after frame; measured against the source WAV,
  FFmpeg's decode scores 14.3 dB where `ac3cli`'s scores 33.7 dB — and `ac3cli` lands within
  0.6 dB of its own score on FFmpeg's encode of the same source at the same rate. So that one
  fixture has no FFmpeg oracle and is scored against the source WAV instead.

A second was found and fixed rather than recorded:

- **`wav_channel_order` used to write acmods 2/1 and 3/1 in bitstream order** (L C R S), on the
  stated grounds that no WAV convention claims a mono-surround slot, while FFmpeg mapped 3/1 onto
  `WAVEFORMATEXTENSIBLE`'s FL/FR/FC/BC and wrote L R C S. That premise was wrong:
  `WAVE_FORMAT_EXTENSIBLE` does define `SPEAKER_BACK_CENTER` (`0x100`), which is exactly FFmpeg's
  mono-surround slot for both 2/1 and 3/1. `wav_channel_order` now places every acmod by WAV
  speaker position rather than bitstream order — the practical effect is C swapping with R and
  the LFE moving up to fourth, on top of the mono-surround fix — and the FATE sample that
  exercises 3/1 (`millers_crossing_4.0.ac3`) went from decode-and-parse-only to a compared sample
  once the two decoders' channel orders agreed: channel 0 (L) at 48.93 dB, and a near-silent
  surround channel gated on absolute difference at a −46.0 dBFS floor (measured −55.31 dBFS).

## Where the oracles don't reach

FFmpeg and the in-repo decoder are complementary, not redundant, and neither covers everything
alone. FFmpeg reads Annex E coupling, spectral extension and AHT (98+ dB SNR for coupling and
spectral extension; 62–89 dB for AHT, which genuinely recodes mantissas rather than scaling or
synthesizing around already-decoded content, so a wider margin from bit-exact is expected there)
— but it refuses a *second* dependent substream (`ff_ac3_parse_header` rejects
`substreamid != 0`), which is exactly what 7.1.4 needs. The in-repo decoder reads every Annex E
tool combination at every layout, 7.1.4 included, so it backstops FFmpeg's one gap — but a stream
only the in-repo decoder can read is checked against itself, not against anything external.

| Stream | FFmpeg | In-repo decoder |
|---|---|---|
| AC-3, any supported mode | yes | yes |
| E-AC-3 up to 5.1.4 (one dependent), no Annex E tools | yes | yes |
| E-AC-3 7.1.4 (two dependents) | no | yes |
| E-AC-3 with cpl / spx / aht | yes | yes |
| E-AC-3 7.1.4 with Annex E tools | no | yes |
| E-AC-3 with enhanced coupling (`ecpl`) or transient pre-noise processing (`tpn`) | no | yes |
| E-AC-3 `fscod2` half rates (24/22.05/16 kHz) | header only | yes |

**7.1.4 has no external oracle at all.** For that one layout, encoder and decoder are checked
against each other and nothing else:

```
$ ac3cli eac3-sine out.ec3 1 384 1000 50 714
$ ffmpeg -v error -i out.ec3 -f null -
[dec:eac3] Error submitting packet to decoder: Error number -84085770 occurred
$ ac3cli decode out.ec3 out.wav
decoded 32 E-AC-3 access units (3 substreams each) -> out.wav
  12 channels, 48000 Hz: L R C LFE Lrs Rrs Ls Rs Vhl Vhr Lts Rts
```

Fourteen channels are coded and twelve are rendered: per §E3.8.2 the dependent's Ls and Rs
replace the bed's rather than adding to them.

**Enhanced coupling and transient pre-noise processing have no external oracle at all — not even
the partial one 7.1.4 gets.** FFmpeg's own Annex E parser was never written to read either
tool's syntax, so it doesn't reject these streams the way it does a second dependent substream —
it has no model of the bits at all, which makes `-xerror` unusable as a check here rather than
merely unavailable. `tools/ci/quality_race.py`'s CI gate (`decode_scores_ours`) scores both through
this project's own decoder instead, the same self-consistency posture 7.1.4 falls back to, with
one weaker guarantee than 7.1.4 has: a defect both the encoder and decoder agree on — a
misreading of the spec shared by both sides rather than a one-sided bug — would not be caught by
either the CI gate or the round-trip unit tests in `tests/decoder/test_eac3_decoder.cpp`.

**`fscod2` audio content has no external decode oracle at all — not even Dolby's own.**
`ffprobe` walks every syncframe of a reduced-rate stream correctly (frame count, exact byte size,
exact spacing, and `sample_rate` all confirmed against all three rates), so the framing and
header are cross-checked externally. But actually decoding the audio is refused by both
real-world implementations available here: FFmpeg's E-AC-3 decoder (`Not yet implemented in
FFmpeg, patches welcome`) and, more surprisingly, Dolby's own Reference Player — `dlbac3parse`
reports `No valid frames found before end of stream` on a stream `ffprobe` reads frame-by-frame
without complaint, using the same pipeline (`tools/ci/quality_race.py`'s `dolby_decode`) that decodes
a normal-rate stream from this encoder without issue. `fscod2` appears to be a coding tool whose
own reference implementation does not support it. So the coded audio is verified only by this
project's own encoder/decoder round trip and the independent Python parser
(`tools/references/eac3_parse.py`).

**`compr` in E-AC-3 has no external oracle.** FFmpeg's Annex E header parser reads `compre` and
then skips the word, so `-heavy_compr` changes nothing on an E-AC-3 stream however good the
metadata is. It is covered bit-by-bit instead
([tests/meta/test_drc.cpp](https://github.com/iainchesworthlabs/ac3forge/blob/main/tests/meta/test_drc.cpp),
[tools/references/eac3_parse.py](https://github.com/iainchesworthlabs/ac3forge/blob/main/tools/references/eac3_parse.py)).

## What's confirmed against real hardware, and what isn't

The codec itself is platform-independent; only capture, monitor playback and IEC 61937
passthrough touch sound hardware, and how far each is verified differs by platform and by sink —
covered where it's most relevant rather than repeated here:

- [Windows](platforms/windows.md#audio-backend-wasapi) — `MonitorSink` is confirmed against real
  hardware; exclusive-mode passthrough bitstreaming has not been.
- [Linux](platforms/linux.md#what-has-and-has-not-been-verified) — the ALSA backend is verified
  headless only; no real S/PDIF or HDMI output has been tried.
- [macOS](platforms/macos.md#audio-backend-coreaudio) — the CoreAudio backend is CI-verified
  only: its device-free logic runs under `ac3tests` on hosted runners, but no real Mac hardware
  has ever run it.
- [Raspberry Pi](platforms/raspberry-pi.md#verified-configuration) — real-hardware validation on
  a Pi 4B: the full suite on both compilers, ALSA device enumeration against the Pi's real
  `vc4hdmi` HDMI outputs, and an inspected arm64 `.deb` — still with no downstream receiver in
  the loop.
- [Android (Shield Atmos Demo)](platforms/android.md#what-has-and-has-not-been-verified) — the
  most thoroughly hardware-verified platform in the project: real E-AC-3/Atmos passthrough over
  HDMI to a real AV receiver, with object audio confirmed reconstructable (not just the panned
  bed). Verification specific to this one Android app on this one Shield + receiver pair, not a
  general claim about Android as a platform.
- [Atmos & JOC](concepts/atmos-joc.md#two-honest-limitations) — Dolby's own decoder gates object
  decoding on a keyed authenticity tag; the signer ships in-tree (`ac3::signing`) but this
  project ships no key for it, so its streams are unsigned unless an operator supplies one.
  Objects sharing a direction also can't be perfectly separated. Neither is a conformance gap.
