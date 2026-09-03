# Validation

Quality is measured, not asserted, and coverage has known edges. This page is both: how output
is checked, and exactly where checking runs out.

## Six independent checks

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

   One DEE-produced stream is **committed** rather than only diffed against:
   `tests/golden/object-fixture/dee_joc_514.ec3`, a DD+ JOC encode of a synthetic 5.1.4 tone bed
   (`tools/generators/gen_object_fixture.py`, local-only — DEE is licensed and never runs in CI).
   It is the only Atmos stream here this project's own encoder did not make, and every part of
   the object layer it exercises was refused outright before it existed: a bed programme with a
   twelve-channel assignment and `b_bed_chan_distribute` set, `object_gain_idx` 3, a second
   `oa_element` carrying a `trim_element`, `joc_dmx_config_idx` 3 with a nonzero clip gain and
   sparse coding, and an EMDF container mixing `payload_frame_aligned` 0 and 1 across its
   payloads. Decoding it also caught a real audio-layer bug — `audblk` reads `cplfgaincod` and
   `cplfsnroffst` ahead of the per-channel lists when the block couples, and the decoder skipped
   both, which no stream this project produces could have exposed.

   What that fixture asserts is not just "it parses". Each of the ten channels of the source bed
   carries a different tone, so identifying each reconstructed JOC object by which tone dominates
   it is an independent check on both the reconstruction and the *order* the bed's channels
   occupy — the order TS 103 420 §5.6.1.1.4 states backwards
   (`tests/oba/test_dee_joc_fixture.cpp`).

   Two limits are worth stating: DEE's `atmos_mezz` (ADM BWF) input refuses a master this project
   authors, gating on content provenance rather than syntax, so the fixture is channel-based
   immersive and carries no dynamic objects — object size, zone constraints and snap are covered
   by the in-repo encode round trip instead. And retail Atmos discs, whatever they would exercise,
   are not redistributable and are not used here.
5. **Fuzzing, in both directions.** Into the decoder: the libFuzzer harnesses under `fuzz/` drive
   the codec's untrusted-input entry points looking for crashes and undefined behaviour
   (ASan+UBSan), and two differential harnesses decode each mutated stream with both this
   project's decoder and FFmpeg's and diff the PCM. CI runs both: the `Fuzz Regress` job replays
   the checked-in seed and regression corpora on every push and PR, and the `Fuzz Differential`
   job adds a bounded mutation budget on pushes.

   The object and metadata layer is driven directly rather than through the decoder: separate
   harnesses over `emdf::parse_container`, `oba::parse_payload`, `oba::joc::parse_payload`,
   `signing::verify_atmos_stream`/`verify_atmos_frame` and (opt-in) `ac3adm::parse_bw64`, each
   seeded from the real payloads inside this project's own Atmos streams. A sixth,
   `oba::parse_osc_packet` — the OSC 1.0 wire form a live session's object positions arrive over
   (roadmap UX4), driving `live mode=atmos positions=osc:<port>` and the GUI live room — is
   covered the same direct way by `fuzz_osc_parse`, part of `fuzz/run.sh`'s default target list
   and so covered by CI exactly as the five above are; its own seeds are hand-built OSC packets
   (`fuzz/seeds/fuzz_osc_parse/`) rather than extracted from an Atmos stream, since there is no
   bitstream to extract them from. See [Threat model](threat-model.md#trust-boundary). That
   matters because
   the indirect route was mostly closed: both decoders check their CRC words before reading the
   frame behind them, so a mutation landing in a skip field died at the checksum. The two decode
   harnesses now carry a custom mutator that re-stamps crc1 and crc2 after mutating — crc1
   through the GF(2) polynomial inverse it has to be solved with — while leaving one mutation in
   four unrepaired so the rejection path itself stays reachable.

   Out of the encoder: `tools/ci/fuzz_encoder_space.py` (AC-3) and
   `tools/ci/fuzz_eac3_encoder_space.py` (E-AC-3, roadmap VX1) draw random legal encoder
   configurations crossed with adversarial PCM — transients, silence↔loud transitions inside one
   frame, spectral jumps between blocks, dense harmonics, clipping — and hold every stream they
   produce against both decoders. This is the one check here that varies the *input material*
   rather than the option list; it exists because an encoder defect that produced streams both
   decoders reject needed a specific input shape to reach, and so escaped every other check on
   this page. The E-AC-3 half additionally covers the Annex E tool tokens, the `fscod2` half
   rates, VBR, the layouts that need dependent substreams and Atmos object counts, and classifies
   each case by which oracle can actually read it (the table below). Bounded on every pull
   request, deeper nightly. See
   [fuzz/README.md](https://github.com/iainchesworthlabs/ac3forge/blob/main/fuzz/README.md).

6. **The encoder/decoder mirror self-check** (`ac3::verify`, opt-in). Not an oracle: it compares
   this project against itself. What it compares is the *model* rather than the audio. An encoder
   carries a picture of the decoder it is writing for — the exponents that decoder will
   reconstruct, the bit allocation it will derive, the delta correction it is holding, the AHT
   gains and coupling/spectral-extension coordinates it will apply — and every mantissa field's
   width comes out of that picture. `MirrorEncoder` (AC-3) and `Eac3MirrorEncoder` (E-AC-3)
   decode every frame the encoder just emitted and diff the two pictures per block, per coded
   stream, per substream, starting from the bit offset at each block boundary.

   What that adds over a round trip is the case where the two sides differ but the audio
   survives it — an AHT gain one side recovered differently, a coordinate quantized against a
   different band structure, a delta correction one side is still holding — which a decode-and-
   compare passes and an SNR gate does not notice, and which a third-party decoder would
   nevertheless render differently. It also localises an outright desync: the AC-3 half fired
   four frames before the `deltbaie` bug produced its own §7.10.2 symptom, in the right file
   rather than two blocks downstream in the wrong one. What it cannot see is a misreading the two
   sides make *identically* — anything decided in code they share (`compute_bit_allocation`,
   `group_bands`, `decode_coordinate`) is shared by construction, and only checks 2–4 above reach
   that. Off by default at the cost of one branch per block; `ac3cli eac3-encode … verify` turns
   it on for a whole encode, and `tools/ci/run_codec_matrix.sh` runs it across the tool matrix on
   the sanitizer leg.

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
waveform-distance number, and something SNR alone cannot claim. See `perceptual_score()` in
`tools/ci/quality_race.py`.

That column used to be empty everywhere it was published: CI deliberately did not install
`visqol-python`, so every row on the `quality-history` branch carried `mos_lqo: null` and every
MOS cell rendered `n/a`. It is installed now, hash-pinned like every other Python dependency, so
the trend pages carry real MOS numbers. It stays optional for a *local* run — not installed
still shows `-`, never a failure — which is why the one-off snapshot above has no MOS column.

Both the table above and everything the trend pages plot come from the **fixture corpus** in
`tests/golden/audio/`, which is versioned and hash-checked as a unit
(`tools/checks/check_corpus.py`). Two of those fixtures are synthesized from `sin()`,
pseudo-random noise and FIR smoothing, and two are 30 s CC0 recordings of real speech and music.
Both kinds are kept, and the distinction matters when reading any number on this page: the
synthetic pair carries a flat noise plateau across its whole top octave that no real material
has, and tuning the encoder's bandwidth against it once produced a measured 2.1 dB "win" that was
an artefact of the fixture. See [tools/generators/README.md](https://github.com/iainchesworthlabs/ac3forge/blob/main/tools/generators/README.md)
for the measured spectra, the licences, and which fixture is evidence about what.

That is a one-off snapshot. [Quality trend](quality-trend.md) tracks the same gold-reference SNR
by commit, on every push to `main`, so a regression shows up as a trend line
rather than only in that run's CI log.

The object layer has its own series, [Object quality trend](object-quality-trend.md): a fixed
five-object Atmos scene encoded and decoded by each build, one delay-compensated SNR/LSD/leakage
row per object per rate. It is a self-consistency series throughout — see "Where the oracles
don't reach" below for why no other kind is available.

### One floor per channel, not one per file

Every SNR gate here is stated **per channel**. That is worth spelling out, because
the alternative looks equivalent and is not.

A 5.1 fixture's six channels do not sit anywhere near each other. On
`ac3-51-448/dee.ac3` — Dolby's own encoder, decoded by this project and by FFmpeg and the
two decodes compared — the measured agreement is:

| L | R | C | LFE | Ls | Rs |
|---|---|---|---|---|---|
| 57.5 dB | 63.8 dB | 58.1 dB | 82.2 dB | 22.8 dB | 22.7 dB |

The surrounds are 35 dB below the front channels, and legitimately so — though the
mechanism is not the obvious one. §7.3.4 leaves the *values* a decoder substitutes for
zero-bit bins unspecified ("any reasonably random sequence"), so two spec-correct
decoders are required to disagree in those bins; the question is where they fall.
Measured with `ac3cli decode … bap-census=`, the surrounds' own basebands are almost
fully coded on this fixture — **1.8–2.2%** zero-bit bins, against **80–90%** for the
front channels. What is heavily zero-bit is the **coupling channel**, at **59.7%**, and
§7.3.4 dither for coupled bins is applied per *receiving* channel after decoupling. So
the same absolute dither lands in every coupled channel, and it dominates whichever ones
are quietest: Ls and Rs sit at −33 and −29 dBFS where L and R sit at −13. The low
surround agreement here is a signal-level effect on a shared error, not a sparser
allocation — the reading this page carried before the census existed to check it.

A single floor for this fixture therefore had to clear 22.7 dB, and it was set at 22.
Which meant the centre channel was gated at 22 dB while measuring 58.1 — it could have
lost 36 dB, more than the entire dynamic range of the surround channels, without
failing anything. The LFE had 60 dB of slack. That is not a gate; it is a gate on one
channel and a rounding error on the other five, and it was blind to precisely the
per-channel syntax defects the third-party fixtures were added to catch (one of the
five found there, `firstcplcos[ch]`, is per channel by nature).

Each channel now carries its own floor, derived as `floor(min_observed − 1.0)` from
that channel's lowest value across every CI leg and every recorded commit —
`tools/checks/derive_channel_floors.py` is the derivation, kept as a script so a floor
move is reviewable against evidence rather than asserted.

The 1.0 dB covers commit-to-commit noise and nothing else, because `min_observed` has
already absorbed everything else. Measured over 520 (check, leg, channel) series, one
leg's own number moves by a median of **0.000 dB** across the whole recorded history,
and 495 of them stay under 0.5 dB; the 25 that do not are a single real step change on
one check, not noise. A new leg landing below a floor is not a false alarm under this
policy — it is platform behaviour nobody has reviewed, and stopping to look at it is the
right outcome rather than something to pre-authorise with margin.

One pair of floors went *down* in the change: this fixture's Ls/Rs, from 22 to 21,
because 22 was never derived for those channels — it was the one floor the whole
fixture had to share. Its other four channels gained 34–59 dB of gate. The check went
from catching only a total surround collapse to catching a 1 dB move in any channel,
and `tools/checks/test_compare_wav.py` holds that property down with a test that fails
if the single-floor form is ever restored.

### Why arm64 and x86-64 disagree

The legs split into two groups on the high-SNR channels, ~6.02 dB apart, and this was
carried for a long time as an unexplained effect attributed to "arm64 and macOS" legs
(roadmap VX11). Two things are now settled.

**It is architecture, not OS or compiler.** `macos-llvm` (arm64) sits with the arm64
group; `macos-llvm-x64` sits with the x86-64 group. Same OS, same Homebrew LLVM, opposite
sides. That rules out the "macOS libm" reading directly.

**It is one bit of arithmetic, not a codec error.** 20·log₁₀2 is 6.02 dB whether the bit
is an AC-3 exponent step or a floating-point rounding bit, so the number alone cannot
tell them apart — but the prediction can. A systematic exponent error would be
level-independent and shift every channel equally. Rounding is only visible where the
measurement is already rounding-dominated. Sorting all 52 (check, channel) pairs by
their x86-64 SNR gives a step, not a gradient:

| x86-64 SNR of the channel | arm64 difference |
|---|---|
| 18.3 – 66.7 dB (32 pairs) | **0.00 – 0.11 dB** |
| 67.8 – 88.3 dB (20 pairs) | **5.85 – 6.05 dB** |

Nothing lands in between. Below ~67 dB the disagreement between the two decoders is
dominated by real coding differences and by §7.3.4 dither, and one extra rounding bit is
buried in it. Above ~67 dB the two decoders agree so closely that arithmetic is all
that is left to disagree about, and the bit becomes the whole signal. That is also why
the LFE is the only channel to split on the fixed third-party fixtures: at 88 dB it is
the one channel there whose comparison is rounding-limited.

The practical consequence is the floors above. `min_observed` is a minimum **across
legs**, so wherever this split appears the minimum is already the arm64 value — the low
side. The first version of these floors subtracted a further 6.02 dB on top of that,
counting the same margin twice and costing about 5 dB of sensitivity on every channel.
Removing the double-count is what took the headroom to 1.0 dB.

**The encoder is bit-exact across architectures; the gap is entirely decode-side.** The
cross-platform hash gate had never pinned `aarch64-neon` — it printed `[unpinned]` and passed,
so every arm64 run had compared its encoder's output to nothing. Pinned now from the real arm64
CI legs (PR #503, run 33635430769), and all three streams come back **byte-identical** to
`x86_64-sse2`. So the same bitstream goes in on both architectures and different PCM comes out:
whatever the last-bit difference is, it is in the decode path, not the encode path.

That also settles a discrepancy that looked like it needed two mechanisms. On the fixed
third-party fixtures only the LFE splits, but on this project's own gold-reference streams
*every* channel does — which invited the inference that the arm64 encoder must produce a
different bitstream. It does not. The gold-reference streams are encoded `dither=off`
(`nodither` for E-AC-3), so no channel's comparison is dither-limited and **all** of them sit in
the rounding-limited regime above 67 dB, where the last-bit difference is the whole remaining
signal. The third-party fixtures carry dither, which dominates every channel except the LFE.
One mechanism, two fixture populations.

What is **not** yet answered is why arm64 is the *worse* of the two — it agrees with FFmpeg's
decode less closely than x86-64 does, consistently, by one bit. The search space is now much
smaller: the encoder is excluded by the hashes above, the reference side is excluded by the
FFmpeg kernel test, and contraction and libm were excluded before that. What remains is the
decode path on real arm64 silicon, which is also the one thing no emulated run has reproduced.

The same reasoning now applies to the *trend* check as well as the gate:
`tools/ci/append_quality_history.py` compares each channel against its own trailing
average, where it previously watched only the worst channel — which, on these fixtures,
was the same dither-dominated surround every single run.

### What would make these numbers excellent

1. ~~**The 6.02 dB headroom is set by something unexplained.**~~ **Closed.** It was not
   an exponent step and it was not unexplained once the split was sorted by level — see
   "Why arm64 and x86-64 disagree" above. The headroom it was forcing turned out to be a
   double-count on top of an already-cross-platform minimum, and the floors are now
   derived at 1.0 dB, catching a 1 dB per-channel regression where they previously
   needed 6. The follow-on question — why arm64 is the *less* accurate of the two —
   remains open and is tracked separately.

2. **Spec-permitted dither divergence is still inside the measurement.** The surrounds
   score ~22 dB not because either decoder is wrong but because §7.3.4 lets them differ
   in the zero-bit bins. A comparison that excluded those bins — masking on the `bap`
   values the decoder already records in `ac3::verify::FrameTrace` (`DecoderConfig::trace`,
   exported by `ac3/verify/trace_export.hpp`) — would measure only the bins that were
   actually coded, and the surrounds would be expected to join the front channels in the
   50–90 dB band. That needs the comparison moved into the MDCT domain, with block
   alignment and the coupling-region indirection (a coupled channel's bap-0 decision
   lives on the coupling stream, not its own) handled correctly; it is a real piece of
   work, not a flag. It would also produce a new metric on a new scale, so it belongs
   beside the current series rather than replacing it.

Neither gap is a defect in the codec. Both are limits on how sharply the current
instruments can see it, which is the more useful thing to be honest about.

## Performance and reference modes

Both transform hot spots — the forward MDCT (§8.2.3.2) and the inverse transform's step-3
complex sum (§7.9.4) — exist in two evaluations: the spec's own direct form, and a fast path
through a shared FFT kernel. The direct forms are the *reference*: they are what the standard
states, and every fast path is validated against its direct counterpart by the test suite (max
peak-normalized relative error 1.3e-13 forward, 7.8e-14 inverse, 1.7e-15 for `dft512`
against its own O(N²) summation; end-to-end agreement 331 dB direct-vs-fast for encode, and
232.1 dB (AC-3) / 208.2 dB (E-AC-3, every Annex E tool) / 217.9 dB (E-AC-3, enhanced coupling)
for a decode over 180 seconds of real material). The fast paths are the default, because that
evidence was reviewed and accepted before each default flipped.

The kernel itself is radix-4 with a trailing radix-2 stage where log2(P) is odd, specialised at
compile time for the three sizes the codec uses (P = 64, 128, 512), with the first stage's
unit twiddles eliminated and the digit-reversal permutation folded into each caller's own
input-producing loop rather than run as a pass — 1.6–1.75× the throughput of the generic
radix-2 core it replaced, at the same tolerances.

Both evaluations are gated end to end, not only at the transform level. The `linux-gcc` leg
runs `tools/checks/verify_gold_reference.sh` twice — once as it stands, once with
`TRANSFORM_MODE=reference` — so the direct forms face the same FFmpeg-oracle SNR floors on the
same real streams as the fast paths, and `tools/ci/run_codec_matrix.sh` carries `fast-mdct=off`
and `fast-imdct=off` rows through the sanitizers. Without that, a change to a fast path could
take its own reference with it and nothing outside the transform unit tests would notice.

One nearby switch is deliberately **not** part of this pair: `joc-domain=qmf|mdct`, which selects
where JOC's reconstruction matrix is estimated and applied. The two transforms above are the same
answer computed two ways; the two JOC domains are different answers about 5 dB apart, so folding
them into a speed preference would make `mode=performance` quietly pick the worse one. The default
is already the domain TS 103 420 §6.6.6 states, so `mode=reference` has nothing to add either. See
[Atmos & JOC](concepts/atmos-joc.md#which-domain-the-matrix-lives-in).

`ac3cli` exposes the pair as one intent-level switch: `mode=reference` runs every transform in
the command on the direct evaluations — for regenerating fixtures, comparing sample-for-sample
against an external decoder, or isolating a suspected transform defect — and `mode=performance`
(the default state) names the fast paths. The per-transform escape hatches `fast-mdct=off` and
`fast-imdct=off` adjust one half at a time; see
[Options & grammars](cli/metadata-options.md#command-specific-notes) for the full token
semantics. At the library level the same pair is `EncoderConfig::fast_mdct` /
`eac3::FrameConfig::fast_mdct` for the forward transform and `DecoderConfig::fast_imdct` for the
inverse.

Encoded output never depends on `DecoderConfig`. An enhanced-coupling encode does run an
inverse transform of its own — `ecpl_channel_spectrum` reconstructs the spectrum the decoder
will hold — and that one follows `eac3::FrameConfig::fast_mdct`, which makes that field the
encoder's fast-transform switch in both directions and keeps `mode=reference` direct end to
end. It is byte-identical either way on the encode corpus at the tolerances above, so it is a
speed choice, not an output one.

One decode-side case runs the FORWARD transform too: JOC's own bed analysis under
`joc-domain=mdct` (PF8) has to re-express the decoded bed in the same 256-bin MDCT domain the
transmitted matrix was estimated in before it can apply §6.6.6's per-band combination — the one
place a decode ever needs the fold `mode=`/`fast-mdct=off` otherwise only reach on the encode
side. `DecoderConfig::fast_mdct` carries it (`oba::joc::reconstruct`'s own `fast_mdct` parameter,
threaded from `decode`/`monitor`/`live`), defaulted ON by the same evidence gate as every other
fast path here: 1.3e-13 worst relative error at the transform level (the same forward kernel
`EncoderConfig::fast_mdct` already validates), full `oba::joc::reconstruct` output agreeing
321-325 dB SNR against the direct form over three real encoded-and-decoded objects
(`tests/oba/test_atmos.cpp`), and the bed analysis kernel itself — isolated from object
synthesis, which this switch does not touch — measured 11.0x, 238 to 2628 microseconds per
block's five-channel analysis on a release build (`ac3kernelbench`'s
`joc_reconstruct_mdct_4obj`/`_direct`): a fixed ~2.4 ms saved per frame regardless of object
count, ~2.2 s over a 30 s `kMdctBand` decode. It has no effect under the default
`joc-domain=qmf`, whose filterbank has only the one evaluation, and — like
`DecoderConfig::fast_imdct` — never reaches an encoder.

## Test suite

The Catch2 suites (`ac3tests` plus the `ac3perf` throughput suite) plus one `ctest` entry per
example program, run per platform. The GUI's Qt Quick Test harness (`ac3gui_qmltests`) adds one
entry per `tst_*.qml` suite under `apps/gui/tests/qml/` (21 today) on a GUI-enabled build, and the
ALSA backend's `tests/backend/alsa/` adds 15 on a Linux build with libasound present — or
`tests/backend/pipewire/`'s 5, on a build that selected PipeWire instead; `ctest` runs whatever
the configuration registered:

```bash
ctest --preset test-windows-msvc-debug
```

`examples/CMakeLists.txt` registers 21 example programs as their own `ctest` cases, plus
`read_adm`/`encode_adm` under `AC3FORGE_BUILD_ADM` and the two C-API examples under
`AC3FORGE_BUILD_CAPI`. The ones that touch the filesystem (`wav_roundtrip`, `read_adm`,
`encode_adm`) write scratch files under a name unique to that run, not a fixed name in the OS
temp directory — two checkouts running `ctest` at once would otherwise read and delete each
other's fixture.

## Third-party bitstreams

**There are no free AC-3 or E-AC-3 conformance bitstreams.** ATSC A/52 and ETSI TS 102 366 are
both freely downloadable *documents*, but neither body publishes conformance *vectors* for these
codecs the way MPEG does for its own, and Dolby's verification material ships under licence with
its professional tools. Everything below is the substitute, and it is worth being explicit that
it is one: a corpus of real third-party encoder output with no normative expected decode
attached to it, not a conformance suite.

Two tiers, both gated in CI:

- **Committed** — `tests/golden/external-baseline/` holds 14 streams from Dolby Encoding Engine
  6.5.4 and FFmpeg 8.0.1 across 8 codec/layout/bitrate legs (`manifest.json`, `baseline_version`
  2), each encoded from this repository's own source WAVs (see
  `tools/generators/gen_external_baseline.py`) and each carrying the DEE, FFmpeg and
  ours-at-baseline-time scores it was measured at. `tools/checks/verify_gold_reference.sh` gates
  on a six-stream subset of that: it decodes all six with `ac3cli` on every gold-reference leg and
  diffs each against FFmpeg's own decode, with per-fixture floors quoted beside the measured
  numbers in the script. The other eight are not gated:
  `tools/ci/append_external_comparison_history.py` walks every leg in the manifest for the
  [tool-comparison trend](tool-comparison-trend.md). They also seed the
  decoder fuzzers, so mutation starts from third-party structure rather than only from this
  project's own encoder output.
- **Fetched** — `tools/checks/verify_fate_interop.py` pulls eight SHA-256-pinned samples from
  FFmpeg's FATE archive and holds each against FFmpeg's own decode. These are excerpts of
  commercially mastered programme material, encoded years ago by whatever encoder the mastering
  house used, and they exercise choices neither this project's encoder nor FFmpeg's makes:
  spectral extension at 128 and 256 kbit/s, 1536 kbit/s, a director's-commentary track, dither
  in use, the 3/1 acmod nothing in this tree can encode, and an A/52 Annex E §E2.3.1.2
  legacy-core delivery (below). Fetched at run time and never committed — they are film
  excerpts, and pinning by hash is what keeps an upstream change from quietly moving the
  numbers. Runs nightly in the `Interop` workflow.

Wiring up the first tier found **five separate Annex E decoder defects** in a single sitting, on
syntax that no stream this project can encode is able to reach — the three AHT-in-use flags read
unconditionally, `cplfgaincod`/`cplfsnroffst` not read at all, the three band-structure default
tables applied in the wrong blocks, the `first*` per-frame coordinate states approximated as
"block 0", and a missing coupling-state reset. Four of the six fixtures did not decode at all
before that. It is the clearest evidence on this page for why a self-consistent round trip, an
independent transcription and a second decoder driven by the same encoder are all still not the
same thing as reading somebody else's bitstream.

One arrangement fetched third-party structure led to being **added** rather than recorded or
gated: `the_great_wall_7.1.eac3` is not a plain E-AC-3 elementary stream. Each 4608-byte access
unit is an AC-3 core syncframe (bsid 6, 3/2+LFE, no E-AC-3 header at all) followed by a
2304-byte E-AC-3 DEPENDENT substream (chanmap 0x1A00: Ls/Rs replaced, Lrs/Rrs added) - a
legacy-core-plus-extension delivery, and A/52 Annex E sanctions it explicitly: §E2.3.1.2 states
"If an AC-3 bit stream is present in the E-AC-3 bit stream, then the AC-3 bit stream shall be
processed as an independent substream assigned substream ID 0", and §E3.8.2's combining rule
(bed locations, then each dependent's chanmap overwriting and extending them) does not care
whether that independent substream happens to be AC-3 syntax or Annex E syntax - only that
dependents "shall immediately follow the independent substream with which they are associated"
and agree with it on sample rate and block count, both of which this arrangement satisfies (an
AC-3 syncframe is always six audblks, matching Annex E's `numblkscod` 3).

Before this, `ac3::io::scan()` and `ac3cli decode` both dispatched on the first frame's bsid
alone: an AC-3 frame sent the stream down the AC-3 path, which read the core cleanly and then
refused the following bsid-16 dependent as "valid AC-3 this decoder does not implement (bsid >
8)". `ac3::split_access_units` had the same gap from the other direction - it read `strmtyp`
out of byte 2's top two bits unconditionally, which in an AC-3 syncframe are crc1's, not a
stream-type field, so a core's own checksum could accidentally look like `kIndependent` or
`kDependent` regardless of what actually followed it.

Both are fixed: `ac3::io::StreamKind` gained `kAc3CoreEac3Extension`, `ac3::io::scan()`
recognises the alternating bsid pattern as one access unit per core-plus-dependents group, and
`ac3::has_eac3_extension_substreams()` lets `ac3cli decode` route such a stream to
`Eac3Decoder` even though its first frame is AC-3. There, `Eac3Decoder::decode_substream` reads
an AC-3 frame through a private `ac3::FrameDecoder` and presents the result as substream
(independent, 0), and `decode_access_unit_core`'s existing §E3.8.2 combining - unchanged - lays
the dependent's channels over it exactly as it would a normal Annex E bed. Measured against
FFmpeg's own decode of the real FATE sample: 41.69 dB on the worst of the eight rendered
channels, in the same range as every other spectral-extension-free sample in this corpus. No
codec-config box is defined for the arrangement (`build_codec_config_box` returns nothing for
it), so container muxing refuses it explicitly rather than emit a `dac3`/`dec3` box that
contradicts its own `mdat`; `ac3cli decode` remains the way to read one.

One more divergence was found and fixed rather than recorded:

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

Two divergences are recorded rather than resolved:

- **FFmpeg fails frame 0 of DEE's stereo E-AC-3 stream.** Exactly one frame, from cold, with
  `exponent 25 is out-of-range`; the other 93 read cleanly, and FFmpeg conceals the failure by
  repeating block 0 across blocks 1-4 rather than dropping the frame. Whole-file, that costs it
  a lot: against the source WAV FFmpeg's decode scores **14.30 dB** where `ac3cli`'s scores
  **33.72 dB** — and `ac3cli` lands within 0.6 dB of its own score on FFmpeg's encode of the same
  source at the same rate, so the gap is FFmpeg's concealment, not DEE's encoding. The gate here
  compares whole files, so that one fixture has no usable FFmpeg reference and is scored against
  the source WAV instead. (`manifest.json`'s 33.32 dB for the same leg is *not* in conflict with
  this: `quality_race.py`'s `score_fixed` skips the first 0.2 s, which is exactly where the
  failing frame sits — see `tools/generators/gen_external_baseline.py`'s module docstring.)
- **`the_great_wall_7.1.eac3`'s OAMD payload does not decode.** FFmpeg reports the file as
  "Dolby Digital Plus + Dolby Atmos", and its arrangement is the real Annex E structure
  described above, but `ac3::oba::parse_payload` refuses several `object_element` fields
  (`num_obj_info_blocks`, `sample_offset_code`, `b_object_not_active` among them) to exactly the
  shape this project's own `AtmosEncoder` emits, and Dolby's commercial encoder does not produce
  that same shape. This is a pre-existing, generic scope limit of the OAMD parser - equally true
  of the same payload riding in an ordinary Annex E independent substream - not something the
  legacy-core support above introduced or could fix on its own, so only the eight rendered audio
  channels are gated.

## Where the oracles don't reach

FFmpeg and the in-repo decoder are complementary, not redundant, and neither covers everything
alone. FFmpeg reads Annex E coupling, spectral extension and AHT (98+ dB SNR for coupling and
spectral extension; 62–89 dB for AHT, which genuinely recodes mantissas rather than scaling or
synthesizing around already-decoded content, so a wider margin from bit-exact is expected there)
— but it refuses any substream whose `substreamid != 0` (`ff_ac3_parse_header`), which rules out
both the second *dependent* substream 7.1.4 needs and the second *independent* one a
multi-programme stream carries. The in-repo decoder reads every Annex E
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
| E-AC-3 with a second *independent* substream (two programmes) | no — and it poisons the first programme too | yes |
| E-AC-3 with JOC objects (Atmos) | 5.1 bed only | yes, including the objects |

Every "no" in that column is a cell where a generated stream has to be checked some other way,
which is what [`tools/ci/fuzz_eac3_encoder_space.py`](https://github.com/iainchesworthlabs/ac3forge/blob/main/tools/ci/fuzz_eac3_encoder_space.py)
(roadmap VX1) is built around: it classifies every case it draws by which of these rows it lands
on, and checks the *framing* of the ones FFmpeg cannot decode — which needs no decode at all. Two
things do it: a walk over the four fields that fix E-AC-3's framing (syncword, `strmtyp`,
`substreamid`, `frmsiz`, all at fixed bit offsets), which shares nothing with the encoder and
works at every layout; and `ffprobe`'s own syncframe walk, where FFmpeg can be trusted to do one.
It is not asked about a two-dependent layout, because it demonstrably cannot: on one 7.1.4 stream
it reported 19 access units where 18 were written, splitting one at an offset that is not a
syncframe boundary — it had lost sync inside the very substream this table's "no" row is about.

None of this closes the gap — the paragraphs below still stand, and a misreading shared by this
project's encoder and its decoder would survive a framing check as easily as it survives the
round trip — but it is more than nothing, and it is where a bit-offset defect shows up first: a
syncframe written at the wrong offset is a syncframe whose `frmsiz` no longer lands the next
syncword where it promised. The harness's `--check-oracles` re-measures this table's own claims
against the installed FFmpeg, so a row that stops being true is reported rather than quietly
assumed.

**7.1.4 has no external oracle at all.** For that one layout, encoder and decoder are checked
against each other and nothing else — the round trip below, plus the mirror self-check, which
diffs both dependent substreams' own models block by block rather than only the assembled audio:

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

**A second independent substream has no external oracle either, and it is worse than 7.1.4's
gap — FFmpeg decodes *nothing at all*, not even the main programme.** `ff_ac3_parse_header`'s
`substreamid != 0` check does not distinguish `strmtyp`, so §E2.3.1.2's I1 is rejected exactly as
a second dependent substream is. What makes this case worse is where the refusal lands: the raw
E-AC-3 demuxer hands the decoder one frame period as a single packet, I0 and I1 concatenated, so
the second programme's presence fails the whole packet. Measured against ffmpeg 8.0.1 on a
125-access-unit two-programme stream (5.1 main plus a mono commentary):

```
$ ac3cli eac3-encode main51.wav two.ec3 448 none 51 off       programme2=commentary.wav programme2-layout=mono programme2-bitrate=96
$ ffmpeg -v error -f eac3 -i two.ec3 -f null -
[dec:eac3] Error submitting packet to decoder: Error number -84085770 occurred
    Last message repeated 124 times
[dec:eac3] Decode error rate 1 exceeds maximum 0.666667
```

Every one of the 125 packets is refused and the output file is zero bytes, even though those
packets carry a main programme FFmpeg reads perfectly well on its own — splitting the stream by
programme first and handing FFmpeg only I0's access units strict-decodes clean, while I1's alone
give `invalid frame type` / `unable to determine channel mode`. So FFmpeg remains usable as an
oracle on each programme's frames, but only after the stream has been demultiplexed by programme,
which is what `ac3::split_access_units(stream, programme)` does.

That demultiplexing is what the container path already performs — a track carries one programme,
so `ac3cli mkv`/`mp4` write the first programme's access units (and warn about the rest) — and
FFmpeg strict-decodes the *result* cleanly. `tools/ci/run_codec_matrix.sh` therefore skips the
FFmpeg check on the raw two-programme stream, the same way it does for 7.1.4, but keeps it on the
muxed file: that check is a direct guard on the access-unit boundaries, since a programme's unit
has to end at the next independent substream of *any* programme rather than at its own next
frame, or each span swallows the other programme's frame and FFmpeg refuses the container too.

**Enhanced coupling and transient pre-noise processing have no external oracle at all — not even
the partial one 7.1.4 gets.** FFmpeg's own Annex E parser was never written to read either
tool's syntax, so it doesn't reject these streams the way it does a second dependent substream —
it has no model of the bits at all, which makes `-xerror` unusable as a check here rather than
merely unavailable. `tools/ci/quality_race.py`'s CI gate (`decode_scores_ours`) scores both through
this project's own decoder instead, the same self-consistency posture 7.1.4 falls back to, with
one weaker guarantee than 7.1.4 has: a defect both the encoder and decoder agree on — a
misreading of the spec shared by both sides rather than a one-sided bug — is not caught by
either the CI gate or the round-trip unit tests in `tests/decoder/test_eac3_decoder.cpp`.

The E-AC-3 mirror self-check (#6 above) narrows that, and is worth being exact about what it
narrows. It compares the encoder's and the decoder's *models* of each block — bit offsets,
exponents, `bap`, delta, AHT gain mode and gains, and the coupling, enhanced-coupling and
spectral-extension coordinates — for every substream of an access unit. The emit side and the
parse side are separate implementations of the same Annex E text, so a misreading in one of them
is caught there even when the audio round-trips cleanly and the SNR gate is happy: an
`ecplchaos` index fitted against a different band structure than the one transmitted, an AHT
gain the decoder recovers differently from the one the encoder chose, a `spxblnd` that
persisted on one side and not the other. What it still cannot see is a misreading the two sides
make *identically*, which for anything decided in code they share (`compute_bit_allocation`,
`group_bands`, `coupling::decode_coordinate`) is by construction. That residue is real, and only
an external oracle or an independent transcription of the same spec text closes it — neither of
which exists for `ecpl` or `tpn`. `tools/ci/run_codec_matrix.sh` runs the check over both tools
on the sanitizer leg.

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
project's own encoder/decoder round trip, the mirror self-check over that round trip (all three
rates, with and without the Annex E tools, in `tools/ci/run_codec_matrix.sh` and
`tests/verify/test_eac3_selfcheck.cpp`), and the independent Python parser
(`tools/references/eac3_parse.py`) — the last of which is the only one of the three written from
the spec separately from the codec.

**Object decode has no external oracle at all, and for once that is not FFmpeg's gap alone.**
FFmpeg implements no JOC reconstruction: it reads these streams correctly and renders the 5.1
bed, which is the designed fallback, but it never produces objects to compare against. Dolby's
own decoder does implement reconstruction — and gates it on a keyed authenticity tag this
project ships no key for ([Atmos & JOC](concepts/atmos-joc.md#two-honest-limitations)), so it
plays them as the bed too. Nothing outside this repository can currently produce an independent
object decode of an ac3forge stream, which makes this the one layer where even the partial
oracle 7.1.4 gets is unavailable. What covers it instead is a self-consistency series with real
resolution: [Object quality trend](object-quality-trend.md) scores each of a fixed scene's five
objects, per commit, at two rates. The same caveat as `ecpl`/`tpn` applies with full force — a
defect the encoder and decoder share is invisible to it.

**Containers and manifests are checked externally where a reader exists, and only there.**
`mp4::fragment`'s and `mp4::FragmentWriter`'s CMAF output both pass FFmpeg 8.0.1's strict decode
(`ffmpeg -v error -xerror -err_detect crccheck+bitstream+buffer+explode`) over the init segment
concatenated with every media segment, and both the HLS media playlist and the DASH MPD read back
through FFmpeg's own `hls` and `dash` demuxers at the exact original access-unit count —
confirmed on a session written segment-by-segment by the streaming writer, not only on the batch
form. The `ceao` compatibility brand is present in the `ftyp` and every `styp` of an
object-audio track and does not disturb that decode.

What has **not** been checked against anything external is the *meaning* of the DASH signalling.
`EC3_ExtensionType`/`EC3_ExtensionComplexityIndex` and the Dolby
`audio_channel_configuration:2011` `@value` are transcribed from ETSI TS 103 420 clause D.2 and
TS 102 366 clause I.1.2.1 (via DASH-IF IOP Part 8 v5.0.0 §5.3.2–5.3.3) and asserted against those
clause texts in `tests/containers/test_fmp4.cpp`, including the element order ISO/IEC 23009-1's
`RepresentationBaseType` sequence requires — but FFmpeg's DASH demuxer ignores supplemental
descriptors entirely, so it confirms only that the manifest still parses and plays, not that a
JOC-aware player would read the right complexity index from it. No MPD schema validator and no
real DASH player has been run against these manifests. The same gap applies to the HLS
`CHANNELS="<N>/JOC"` attribute, which predates this work.

The incremental writers are held to a stronger in-repo standard instead: `mp4::FragmentWriter`'s
media segments are asserted byte-identical to `mp4::fragment`'s over the same frames, and its
initialization segment byte-identical once the three duration fields a live session cannot know
are patched back — the same equality contract `mpegts::Writer` has against `mpegts::mux`. That
makes the batch form's own external validation carry over to the streamed one by construction
rather than by re-measuring it.

**`compr` in E-AC-3 has no external oracle.** FFmpeg's Annex E header parser reads `compre` and
then skips the word, so `-heavy_compr` changes nothing on an E-AC-3 stream however good the
metadata is. It is covered bit-by-bit instead
([tests/meta/test_drc.cpp](https://github.com/iainchesworthlabs/ac3forge/blob/main/tests/meta/test_drc.cpp),
[tools/references/eac3_parse.py](https://github.com/iainchesworthlabs/ac3forge/blob/main/tools/references/eac3_parse.py)).

## Going the other way: published conformance vectors

Everything above consumes someone else's streams as an oracle. Every release also publishes a
set this project produces — 60 streams covering each coding tool, layout and sample rate the
encoder can emit, each with the source PCM it was encoded from, the expected decode hashes and
per-channel levels, and a manifest saying what each vector exercises. It ships as
`ac3forge-conformance-vectors-<version>.tar.gz`, signed and attested like every other release
asset.

The `ffmpeg` field on each vector is derived from the table above rather than typed in, so the
published set cannot drift out of step with what this page says the oracles reach: `full` where
FFmpeg decodes the audio, `header_only` for the `fscod2` rates, `none` for 7.1.4 and for enhanced
coupling / transient pre-noise processing.

Two limits are worth stating on this page rather than only in the bundle: the source material is
synthetic, and the hashes are per-toolchain. On the first — roadmap VX7 has landed and the CC0
speech and music fixtures are committed (`tests/golden/audio/programme_{speech,music}_stereo.flac`),
but the vector generator was never pointed at them: `tools/generators/gen_conformance_vectors.py`
still synthesizes its own sources and marks the spot where those files would join the set. Wiring
this bundle to that material is outstanding work, not a pending roadmap item. On the second —
encoded output is not bit-identical across compilers or architectures, so a bundle regenerated
elsewhere differs from the published one for the same correct streams. VX11 closed without
explaining the 6.02 dB arm64 offset (both hypotheses it proposed were falsified by direct
measurement); VX12, gating byte-identical encodes across every leg, is the one still open.
Regenerating with the toolchain the manifest names reproduces every hash exactly, and the
generator asserts that with `--check-determinism` rather than assuming it.

See [Conformance vectors](conformance-vectors.md).

## AC-4

`ac4::` (roadmap IM4) is a bitstream inspector, not a decoder: it parses the sync frame, table of
contents, presentation and substream-group framing (ETSI TS 103 190-1/-2) — channel-coded,
A-JOC-coded, direct-coded-object and OAMD alike — and reports `audio_data`/`metadata()` payloads
as byte ranges without decoding them. That narrower scope changes which of this page's usual
checks apply.

**Where real AC-4 streams come from.** Nothing open encodes AC-4 — the same gap this page states
for AC-3/E-AC-3, just with no third-party corpus to fall back on either, since neither ATSC nor
ETSI publish AC-4 conformance vectors. The substitute is the same tool this project already
treats as a licensed, local-only, never-in-CI oracle for the AC-3/E-AC-3 "Committed" tier: Dolby
Encoding Engine 6.5.4, whose install here also carries `dee_ac4_encoder.exe` (2.0/5.1/7.1 and
5.1.4 channel-based-immersive) and `dee_ac4ajoc_encoder.exe`/`dee_ac4ims_encoder.exe` (A-JOC and
object-based encodes this parser does not read — see below). `tools/generators/gen_ac4_baseline.py`
generates `tests/golden/external-baseline/ac4-*/dee.ac4` from it, the same local-generation,
committed-output pattern `gen_external_baseline.py` uses.

**What is actually verified**, in the same "how much does this prove" ordering CONTRIBUTING.md's
Oracles list uses:

1. **Annex G's CRC-16**, over every sync frame of the committed DEE fixtures — not a
   self-consistency check, since the polynomial, initial state and no-reflection/no-final-XOR are
   transcribed from the standard and computed independently of whatever produced the bytes. A
   frame this project did not write passing this check is real evidence the sync-frame layer
   (`sync_word`, `frame_size`, `crc_word`) is read correctly.
2. **MediaInfo**, bundled with the same DEE install, reads the committed fixtures through its own
   `dlb_ac4lib`-based AC-4 support (channel count, channel layout, bitstream_version, presentation
   and substream-group counts). `tests/ac4/test_ac4.cpp` asserts `ac4::parse_raw_frame`'s fields
   against exactly what MediaInfo reports for the same file — an independent second reader, not
   just a self-consistent round trip.
3. **`tools/references/ac4_parse.py`**, an independent Python transcription of the same clauses,
   used the same way `eac3_parse.py` is: to catch a transcription slip a self-consistent parse
   cannot. It is what caught three of this parser's own bugs during development — a missing
   `emdf_reserved()` call, `substream_index_table()`'s `b_more_bits`-before-`substream_size` field
   order, and a Table 56 extended `channel_mode` prefix code that read a fixed-width chunk
   regardless of which 7-bit prefix it followed, silently misreading every 9.x/22.2 layout — each
   found by disagreeing with what the real DEE fixture actually contained, not by inspection.

**What is not verified, and why.** Dolby's own AC-4 *decoder* — the Reference Player's
`dlbac4dec` GStreamer element, the same install's `dlbac3dec` already used as an AC-3/E-AC-3
oracle — parses a real DEE-encoded frame's framing cleanly (`dlbac4parse` reports correct
`audio/x-ac4-raw` caps, no CRC or sync errors) but returns zero PCM samples for every frame in
testing, with and without explicit `out-ch-config`/`out-cplx-level`/`main-assoc-mode` overrides.
Whether that is a license/entitlement gap specific to AC-4 decode (as opposed to AC-3/E-AC-3
decode, confirmed working on the same install) or something else was not resolved. It does not
block this parser's own scope, since parse-and-inspect never claims to decode audio content
either — but it does mean **no tool available to this project can currently decode AC-4 audio**,
so nothing here can be checked against rendered PCM the way AC-3/E-AC-3's SNR gates are. If that
gap closes later, it would upgrade tier 2 above (framing-only) to an audio-content check.

**A-JOC / direct-coded-object / OAMD substream groups** (`b_channel_coded == 0`, TS 103 190-2
clause 6.3.2.8-6.3.2.12 — `ac4_substream_info_ajoc()`, `ac4_substream_info_obj()`,
`bed_dyn_obj_assignment()`, `oamd_substream_info()`) are parsed the same way the channel-coded
path is, as of the IM4 follow-up that added them. Their verification story is narrower than tier
1/2/3 above, though, because no real stream reaches this path: `dee_ac4ajoc_encoder.exe` accepts
only an Atmos ADM BWF mezzanine as input, and this project has no tooling that produces one DEE
accepts (the same "gates on content provenance, not syntax" limit this page already states for
the AC-3/E-AC-3 JOC side); `dee_ac4ims_encoder.exe` — the other locally available object-adjacent
encoder, despite its "immersive stereo" name — was confirmed to stay channel-coded regardless of
input. What stands in for a real fixture is a set of **synthetic, hand-built bitstreams**
(`tests/ac4/test_ac4.cpp`), each assembled by a from-scratch `BitWriter` sharing no code with
either `ac4::` or `tools/references/ac4_parse.py`, field-traced against the spec text (including
Table 64/65's array-position-to-bit-index mapping, cross-checked against §6.3.2.10.8's own worked
EXAMPLE 2/3 values) rather than against an external reader. This is weaker evidence than tiers
2-3 — a shared misreading of the spec between this parser and its own test vectors cannot be
ruled out the way MediaInfo or DEE's own output rules it out for the channel-coded path — but it
is stronger than self-consistency alone: the vectors caught two real bugs during construction (an
array-index formula that was reversed for one of the two flag-array widths, and this parser's own
handling of LFE at the same array position - included for `ac4_substream_info_obj()`'s std-flags
branch but excluded for `bed_dyn_obj_assignment()`'s, a genuine spec difference this project's
first draft assumed away). If `dee_ac4ajoc_encoder.exe`'s provenance gate or `dee_ac4ims_encoder.exe`'s
behavior changes, that would upgrade this to a tier 2/3 check. `oamd_common_data()` (§6.2.8.1,
reachable only via `ac4_substream_info_ajoc()`'s own `b_oamd_common_data_present` flag) remains
out of scope either way — a large separate metadata structure (bed assignment, DRC, target-device
categories, dialogue enhancement) a stream setting that flag is refused cleanly
(`Error::kOamdCommonDataPresent`) rather than misparsed.

**The `bitstream_version <= 1` legacy path**
(`ac4_toc()`/`ac4_presentation_info()` as TS 103 190-1 alone defines them) is transcribed and
checked against the published spec text (including a page-rendered visual check of Table 4/5,
not just the PDF's extracted text) but never against a real stream — every DEE 6.5.4 encode
observed writes `bitstream_version == 2`, so no sample exercises this branch. It is exactly the
kind of gap tier 3 above exists to narrow and tier 1/2 cannot: two transcriptions can share a
misreading neither catches.

## What untrusted input is checked against

Correctness and robustness are different questions, and this page answers only the first. What
happens when the bytes are hostile rather than merely wrong — the trust boundary, the
memory-safety posture, the per-access-unit resource limits, and the gaps — is
[Threat model](threat-model.md).

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
  `vc4hdmi` HDMI outputs, an inspected arm64 `.deb`, and [live HDMI passthrough to a real
  Atmos-capable AVR](platforms/raspberry-pi.md#live-hdmi-passthrough-to-a-real-receiver) — every
  stream shape tried, including signed Atmos with height channels, locked correctly at zero
  underruns.
- [Android (Shield Atmos Demo)](platforms/android.md#what-has-and-has-not-been-verified) — the
  most thoroughly hardware-verified platform in the project: real E-AC-3/Atmos passthrough over
  HDMI to a real AV receiver, with object audio confirmed reconstructable (not just the panned
  bed). Verification specific to this one Android app on this one Shield + receiver pair, not a
  general claim about Android as a platform.
- [Atmos & JOC](concepts/atmos-joc.md#two-honest-limitations) — Dolby's own decoder gates object
  decoding on a keyed authenticity tag; the signer ships in-tree (`ac3::signing`) but this
  project ships no key for it, so its streams are unsigned unless an operator supplies one.
  Objects sharing a direction also can't be perfectly separated. Neither is a conformance gap.
