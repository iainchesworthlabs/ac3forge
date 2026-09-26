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
   `oa_element` carrying a `trim_element`, `joc_dmx_config_idx` 3 with a nonzero `joc_clipgain_x_bits`
   (4, though `joc_clipgain_y_bits` is 0, which makes the computed `joc_clipgain` exactly unity —
   see `oba::joc::parse_payload`'s own comment; this fixture exercises the nonzero-field parse path
   but not a non-unity gain) and sparse coding, and an EMDF container mixing `payload_frame_aligned`
   0 and 1 across its
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
  , driving `live mode=atmos positions=osc:<port>` and the GUI live room — is
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
   `tools/ci/fuzz_eac3_encoder_space.py` (E-AC-3) draw random legal encoder
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
Unlike the trend pages beside it, this table is a point measurement rather than a gated series:
nothing on the `quality-history` branch carries an ac3forge-against-FFmpeg comparison, so no CI
run reproduces these four numbers or would notice them drifting.
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
failing anything. The LFE had 60 dB of slack. That gated one channel and left a rounding error
on the other five, and it was blind to precisely the
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
. Two things are now settled.

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
instruments can see it, which is the more useful thing to report.

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
[Options & grammars](forge/cli/metadata-options.md#command-specific-notes) for the full token
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
spectral extension; 62–89 dB for AHT, which recodes mantissas rather than scaling or
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
 is built around: it classifies every case it draws by which of these rows it lands
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
project ships no key for ([Atmos & JOC](concepts/atmos-joc.md#two-limitations)), so it
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
synthetic, and the hashes are per-toolchain. On the first, the CC0
speech and music fixtures are committed (`tests/golden/audio/programme_{speech,music}_stereo.flac`),
but the vector generator was never pointed at them: `tools/generators/gen_conformance_vectors.py`
still synthesizes its own sources and marks the spot where those files would join the set. Wiring
this bundle to that material is outstanding work. On the second —
encoded output is not bit-identical across compilers or architectures, so a bundle regenerated
elsewhere differs from the published one for the same correct streams. VX11 closed without
explaining the 6.02 dB arm64 offset (both hypotheses it proposed were falsified by direct
measurement); VX12, gating byte-identical encodes across every leg, is the one still open.
Regenerating with the toolchain the manifest names reproduces every hash exactly, and the
generator asserts that with `--check-determinism` rather than assuming it.

See [Conformance vectors](conformance-vectors.md).

## AC-4

`ac4::` is a bitstream inspector, not a decoder: it parses the sync frame, table of
contents, presentation and substream-group framing (ETSI TS 103 190-1/-2) — channel-coded,
A-JOC-coded, direct-coded-object and OAMD alike — and reports `audio_data`/`metadata()` payloads
as byte ranges without decoding them. That narrower scope changes which of this page's usual
checks apply.

**Where real AC-4 streams come from.** Nothing open encodes AC-4 — the same gap this page states
for AC-3/E-AC-3, just with no third-party corpus to fall back on either, since neither ATSC nor
ETSI publish AC-4 conformance vectors. The substitute is the same tool this project already
treats as a licensed, local-only, never-in-CI oracle for the AC-3/E-AC-3 "Committed" tier: Dolby
Encoding Engine 6.5.4, whose install here also carries `dee_ac4_encoder.exe` (2.0, 5.1 and 5.1.4
channel-based-immersive; 7.1 input is written as 5.1) and
`dee_ac4ajoc_encoder.exe`/`dee_ac4ims_encoder.exe` (A-JOC and
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
1/2/3 above, though, because no stream this project can encode reaches this path:
`dee_ac4ajoc_encoder.exe` accepts only an Atmos ADM BWF mezzanine as input, and this project has no
tooling that produces one DEE
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
branch but excluded for `bed_dyn_obj_assignment()`'s, a spec difference this project's
first draft assumed away). If `dee_ac4ajoc_encoder.exe`'s provenance gate or `dee_ac4ims_encoder.exe`'s
behavior changes, that would upgrade this to a tier 2/3 check. `oamd_common_data()` (§6.2.8.1) is
transcribed at the one TOC-level site that reaches it, `ac4_substream_info_ajoc()`'s own
`b_oamd_common_data_present` flag — bed render info, trim and headphone metadata, plus the
declared-length `add_data` tail a nested element that reads past its own byte budget fails against,
the same synthetic-vector evidence as the rest of this section. The OAMD substream DATA payload
itself (`oamd_substream()`, §6.2.2.4, which embeds a second, independent `oamd_common_data()` of its
own) stays out of scope, reported as a byte range like every other non-audio substream. Chromium's
public A-JOC test file sets `b_oamd_common_data_present` in every frame; re-checking this reading
against it is still open.

**The `bitstream_version <= 1` legacy path**
(`ac4_toc()`/`ac4_presentation_info()` as TS 103 190-1 alone defines them) is transcribed and
checked against the published spec text (including a page-rendered visual check of Table 4/5,
not just the PDF's extracted text) but never against a real stream — every DEE 6.5.4 encode
observed writes `bitstream_version == 2`, so no sample exercises this branch. It is exactly the
kind of gap tier 3 above exists to narrow and tier 1/2 cannot: two transcriptions can share a
misreading neither catches.

**EMDF-only presentations** (`presentation_config` 6) have no real stream either: no DEE encode
writes one. `ac4::` used to stop reading such a presentation before the `n_add_emdf_substreams`
loop that TS 103 190-1 §4.2.3.2 and TS 103 190-2 §6.2.1.3 place after the config-6 branch, on both
TOC paths. `tools/references/ac4_parse.py` did the same on the `bitstream_version` 2 path. Every
later presentation, the substream groups and `substream_index_table()` were then read from the
wrong bit, and on that path the two transcriptions agreed: the shared-misreading case described
above. `tests/ac4/test_ac4.cpp` now builds an EMDF-only presentation ahead of an ordinary one on
both paths, and the same frames, built again with a separate Python bit writer, parse the same way
in `tools/references/ac4_parse.py`.

### The decoder's syntax

`src/ac4dec` is the start of an AC-4 decoder written from the same two standards. It reads every
syntax element of a frame's substreams, and decodes the audio of some of them (see "The decoder's
output" below): the presentation substream,
channel-coded audio substreams in the Part 1 channel elements (ASF spectral data, stereo processing,
companding, A-SPX and A-CPL data, and `metadata()` with its DRC and dialogue enhancement), a
channel-coded substream's HSF extension substream where one resolves to a distinct, readable
substream (the additional scale factor bands, spectral data and noise fill above 24 kHz a 96 kHz or
192 kHz substream carries), and EMDF payload substreams. It refuses, with a named reason, the speech
spectral frontend, the immersive and 22.2 channel elements, object substreams, and a 96/192 kHz
substream whose HSF extension substream could not be resolved.

With no reference output to compare against, the syntax is transcribed twice, separately, from the
text: in C++ in the decoder, and in Python in `tools/references/ac4_syntax.py`, which takes its table
of contents from `ac4_parse.py`. Each writes a trace of every element it reads, and the traces are
compared. A reading both transcriptions share passes this check, so the places where the text is
ambiguous or defective, the reading taken for each and the evidence for it are recorded in
`src/ac4dec/ERRATA.md`.

**The trace.** One record per syntax element (an entry with a bit count in a syntax table of either
part), holding the bit offset from the start of its substream, the width and the value:

| Element | Width | Value |
|---|---|---|
| A fixed-width or computed-width field | its width | the bits, MSB first |
| An element listed with a variable width, such as `aspx_int_class` | the bits read | the code read |
| `variable_bits(n)` | every bit, continuation flags included | the decoded value, modulo 2^64 |
| A Huffman codeword | its length | its index in the codebook, before `cb_off` |
| `quad_sign_bits`, `pair_sign_bits` | one bit per nonzero line | the bits |
| `ext_code` | the whole escape | the magnitude, 2^(N_ext+4) + ext_val |
| `add_data`, `extensions_bits`, `drc2_bits` | the width, in 65535-bit records when longer | its last 64 bits |
| A byte of a run, such as `emdf_payload_byte` or `presentation_name` | 8 | the byte |

`byte_align`, `fill_bits`, bits skipped by a size, zero-width elements and any element that runs past
the end of its substream are not recorded. A loop records each read, and a field read and then
extended (`audio_size_value` and its `variable_bits(7)`) is two records.

**Digests, in CI.** For each frame and substream with records, one line: frame, substream, kind
(`presentation`, `audio` or `emdf_payloads`), record count, the bit where the last record ends, and
zlib's CRC-32 over the records packed as `struct.pack('<IHQ', offset, width, value)`.
`tests/golden/ac4dec/` holds the Python parser's digests of the committed DEE streams: SIMPLE, ASPX,
ASPX_ACPL_2 and ASPX_ACPL_3 at 2.0 and 5.1, one tone per channel at 2.0 and 5.1, DRC curves with an
Lt/Rt downmix, and immersive stereo at three frame rates. The three 5.1.4 streams, one tone per channel
in each immersive codec mode DEE writes, have none until both transcriptions read the immersive
element (phase D9). `tests/ac4dec/test_ac4dec_syntax.cpp` requires
the decoder to produce the same lines, to read every substream to its exact end and to refuse nothing,
and `tools/checks/test_ac4_syntax_digests.py` requires the Python parser to reproduce the same files, so
neither transcription can change alone.

**The committed streams can be scored.** Every committed stream but `ac4-stereo-64` is made with DEE's
loudness measured and not corrected (`gen_ac4_baseline.py`'s baseline version 3): DEE's default
normalises to −24 LKFS and runs a true-peak limiter, which changes the audio in a way a gain fit does
not undo. `ac4-manifest.json` records each stream's source, rebuilt by the generator from the committed
programme fixtures, with its SHA-256, so a decode can be scored against the exact source DEE encoded.
The generator also makes a larger local set, never committed. Phase G0 made every layout and rate DEE
writes, from 2.0 at 48 kbps to 5.1.4 at 768, immersive stereo at every frame rate, and DRC, downmix,
loudness and I-frame settings, each with MediaInfo's frame-by-frame trace beside it. Phase G1 added
the streams the phases still to come test against, since DEE's licence ends on 2026-11-06: sweeps,
noise and transients at every 2.0, 5.1 and 5.1.4 rate, film and speech at 5.1.4, 7.1 input, immersive
stereo at every rate and frame rate and in gapless parts, metadata at 2.0, 5.1, 5.1.4 and immersive
stereo, substreams for presentations, 60 s programmes, and E-AC-3 and E-AC-3 JOC from the same
sources, each with DEE's MP4 of it and what `ac3cli` made of it. DEE writes no AC-4 from objects: its
object encoders take only an Atmos master, and refuse every master this project writes as "not
authored with Dolby tools" (`planning/ac4.md`, phase G0).

### The decoder's output

The decoder turns a mono, stereo, 3.0, 5.X or 7.X substream in any of Part 1's codec modes (SIMPLE,
ASPX and the three A-CPL modes), at `frame_rate_index` 13 and, through the sample rate converter of
the next section, at every other index, into PCM: the audio spectral frontend, stereo
and multichannel processing, the inverse transform with block switching and frame alignment (Part 1
clauses 5.1, 5.3, 5.5 and 5.6), then the QMF domain (5.7): the analysis bank, companding, A-SPX, A-CPL
and the synthesis bank. Every codec
mode passes through the QMF banks, SIMPLE included, as Part 1 Figure 9 draws the chain, so the decoder
has one delay, 1,313 samples at index 13: `d_pcm`'s 352, the banks' 577 and six QMF slots of history.
The LFE passes through the banks with the other channels and nothing else touches it there. The
transforms, the QMF banks, A-SPX's tables and high frequency generator, and A-CPL's decorrelators,
transient ducker, interpolation and dequantisation tables are in `src/ac4core`, the core the decoder
shares with the encoder. Six checks stand in for the reference output neither part defines:

- **Each transform against its formula** (`tests/ac4core/test_ac4core_dsp.cpp`): the FFT against the
  DFT; the inverse MDCT against a verbatim transcription of Pseudocodes 60 to 63 and against the cosine
  sum they come to, at every transform length of clause 5.5.3; the forward MDCT against its own sum; the
  KBD windows against numpy's Kaiser window, cumulated; the QMF analysis and synthesis banks against
  Pseudocodes 65 and 66 as printed; all to 1e-12. Blocks windowed and transformed by an analysis written
  in the test from the same windows reconstruct their input to 1e-12 across every block transition Part
  1 Table 187 allows, within a frame and across frames, and the QMF pair gives back its input 577
  samples later to 78 dB, a property of its window, `QWIN`.
- **A-SPX's parts on known input** (`tests/ac4core/test_ac4core_aspx.cpp`,
  `tests/ac4dec/test_ac4dec_aspx.cpp`): the subband group, patch and limiter tables of DEE's two 2.0
  configurations, worked through Pseudocodes 67 to 74 by hand, and their invariants over all 2,811
  configurations a stream can select; the linear prediction finding a two-slot recursion; pre-flattening
  an envelope that is a cubic in dB; Table 195's chirp factors; and, on hand-built A-SPX data, the paths
  DEE's streams do not take: frequency and time interleaved waveform coding, a balanced pair, the tone
  generator's phase, the noise generator's index across intervals, an interval running past its frame,
  and companding's gains.
- **A-CPL's parts on known input** (`tests/ac4core/test_ac4core_acpl.cpp`,
  `tests/ac4dec/test_ac4dec_acpl.cpp`): each of the three decorrelators, in each of Table 198's regions,
  has the impulse response of its difference equation to 1e-12, run through 32 or 16 slots at a time,
  and a magnitude response flat to 1e-9; the transient ducker leaves a steady signal alone and ducks a
  decaying one by the gain Pseudocode 112 gives, worked by hand. The dequantisation tables hold their
  printed entries and the structure they share: the fine alpha and beta tables step through one
  sequence, each fine beta row is a multiple of the last to the table's seven decimals, and the coarse
  tables are the fine ones at even indices. Differential decoding runs along frequency, along time and
  across frames, and a value outside its table refuses the frame; interpolation is worked smooth and
  steep, with one and two parameter sets; ASPX_ACPL_3 makes its centre of gamma5 and gamma6.
- **The multichannel matrices against the printed tables, and the elements DEE does not write**
  (`tests/ac4dec/test_ac4dec_multichannel.cpp`, `test_ac4dec_constructed.cpp`): the matrices of Part 1
  Tables 178 and 179 and clause 5.3.3.4 equal the tables' printed entries, 300 of them for Table 179
  alone, held in the test as a transcription of their own. DEE's 5.1 streams use one form of the 5.X
  element, `coding_config` 0 with `2ch_mode` 0. The others, the 3.0 element and the 7.X element in its
  three channel modes are read from streams built with the encoder's writer, one tone per channel, whose
  tracks are the channels through the inverse of the printed matrix: every `coding_config`, both
  `2ch_mode`s, every `chel_matsel`, stereo processing on and off, `b_use_sap_add_ch`, SIMPLE and ASPX.
  Each reads with the writer's trace, record for record, and puts each tone back on its channel, 60 dB
  over the other tones there. On the ASPX streams, one aspx_data element sent loud fills its own
  channels' high band alone, and `b_compand_on` changes only the channel Table 212 gives it. The A-CPL
  modes are built the same way, with A-CPL's syntax written by the encoder's writer: the channel pair in
  ASPX_ACPL_1 and 2, the 5.X element in all three, and the 7.X element in ASPX_ACPL_1 and 2 in its
  three channel modes, with both of Table 202's pairings. Their parameters send each module's downmix
  wholly to one of its two outputs, and ASPX_ACPL_1's residuals carry the other below `acpl_qmf_band`,
  so each tone comes back on its channel and the channels A-CPL leaves out stay 60 dB under -20 dBFS. A
  pair in ASPX_ACPL_2 with beta 1.4 puts the decorrelated part in L and R with opposite signs: it cancels
  in their sum to 0.1 dB and is the tone 1.4 times over in their difference. Twenty of the streams are
  committed, with the Python parser's digests, which both transcriptions reproduce.
- **DEE's streams against their sources** (`tools/checks/score_ac4_decode.py`): the decoded output is
  aligned with the source by cross-correlation and fitted with a gain per channel. Every leg must lag
  its source by the same 4,385 samples (DEE's encoder's 3,072 and this decoder's 1,313; DEE's immersive
  stereo encoder runs a frame shorter, 2,337), sit within 0.2 dB of unity gain, and meet floors pinned
  at the first measurement: per-channel SNR over the whole band for SIMPLE and below the A-SPX crossover
  for ASPX; above the crossover, each frame's energy in each A-SPX subband group against the source's;
  log-spectral distance and ViSQOL. Each tone must land on its own channel. FFmpeg Validate runs it on
  the nine committed legs, six of them 5.1, three ASPX and two A-CPL; locally it runs over phase G0's 99
  legs at index 13: 2.0 from 48 to 768 kbps, 5.1 from 96 to 768 and immersive stereo from 64 to 320,
  every 2.0 channel within 0.17 dB of unity and every 5.1 channel but the LFE within 0.11 dB in SIMPLE
  and ASPX. DEE low-passes the
  LFE before it codes it: from
  the source to the decoded LFE the level runs 0.25 to 0.31 dB under unity to 100 Hz and falls 12 dB by
  120 to 160 Hz, with the phase of a filter near 120 Hz, so the LFE's level is checked from 20 to 100 Hz
  within 0.5 dB and its SNR against the source, -2.2 dB on music, is only pinned. Where the source has
  content above the crossover, the A-SPX tiles' energy sits 0.5 to 1.0 dB below the source's on average
  on 2.0 music at 48 kbps and speech at 48, 64 and 128. That is with the pre-flattening phase D4 reads
  (`src/ac4dec/ERRATA.md`, "Pre-flattening's direction"). As printed, the patch's slope doubles and
  those tiles sat 0.9 to 2.3 dB below; film's 5.1 centre sat 4.6 dB below in the top group of its first
  patch at 256 kbps, all of it lost to the limiter, and 10.9 dB below at 192.
  The immersive stereo legs, made from 5.1, are compared with the source's Lo/Ro downmix, which they
  correlate with at 0.977 to 0.984. DEE's 2.0 streams carry the same audio from 256 kbps up, the rest of
  each frame being fill, so those rates decode to the same samples.
  DEE's 5.1 streams at 96 kbps (ASPX_ACPL_3) and at 128 and 144 (ASPX_ACPL_2) code a pair of downmixes,
  and A-CPL makes the surrounds of them, and the centre in ASPX_ACPL_3, so they are scored as A-CPL
  rebuilds them. The coded downmixes, recovered from the output, meet the source's as waveforms below
  the crossover: in ASPX_ACPL_2 (L + Ls / sqrt 2) / 2 and its mirror, which the upmix keeps exactly, at
  21 to 23 dB SNR on music, 17 to 20 on film and 48 on tones, and in ASPX_ACPL_3 the Lo/Ro downmix,
  which it keeps as closely as gamma's quantisation allows, at 22 to 25 dB on music and film; C in
  ASPX_ACPL_2 and the LFE are scored as they are. Per A-CPL parameter band and pair, (L, Ls) and (R,
  Rs), the output's level difference and correlation are held to the source's: over 2,048-sample frames
  their mean distances run 1.8 to 4.0 dB and 0.18 to 0.47, pinned band by band. Applying each frame's
  parameters a frame early or late takes the level difference's distance on music at 128 kbps from 2.6
  dB to 3.8 and 3.6 (`src/ac4dec/ERRATA.md`, "When A-CPL's parameters apply"). On the tone legs each tone is at least 15
  dB over the others in its channel in ASPX_ACPL_2 and 8 dB in ASPX_ACPL_3: the parameters are per band,
  and a tone 44 Hz from a band's edge, L's at 331 Hz, reaches the next band, whose parameters serve
  another channel's tone.
- **librempeg on the same streams**: its output is 736 samples earlier than this decoder's on SIMPLE
  and ASPX streams alike, the 352 of frame alignment and the 384 of history it does not delay by, and
  on SIMPLE streams the two agree to 83 to 90 dB SNR at unity gain, on each of 5.1's six channels as
  on stereo's two. Below an ASPX stream's crossover they agree to 83 dB where companding is off and to
  33 to 35 dB where it is on, where this decoder's output is 0.5 to 0.9 dB closer to the source. Above
  the crossover they part: on DEE's 2.0 music and speech at 48 and 64 kbps, librempeg's A-SPX tiles sit
  2.3 to 7.5 dB below the source's on average, and 10.1 dB on music at 64, where this decoder's sit 0.5
  to 0.9 dB below, and 5.5. Over the whole band of DEE's 5.1 ASPX streams the two agree to 67 to 75 dB,
  and to 48 dB on film's centre, whose band above the crossover carries the dialogue. On the tone legs,
  where A-SPX adds noise alone, the two agree to 30 dB, noise included: they index the noise table
  alike. On DEE's 5.1 streams in ASPX_ACPL_2 librempeg puts out the coded pair as L and R and leaves Ls
  and Rs silent, so it is no reference for A-CPL.

The level check settled one question the text leaves open in phase D2: the pseudocode as printed,
without the factor of two its informative example mentions, decodes DEE's streams at unity gain with
full scale at 2^15. Phase D3 settled two more: A-SPX's envelopes read at that scale, and companding
measures its levels against full scale 1.0. Those and the other readings reconstruction takes are in
`src/ac4dec/ERRATA.md`, under "Reconstruction" and "The QMF domain". None of the streams here, from DEE
or anyone else, sets `b_snf_data_exists`, so the noise fill is decoded from the text alone.

**Locally, over the census.** With `AC4DEC_GOLDEN_DIR` and `AC4DEC_STREAM_DIR` set, the same test
compares the decoder with the Python parser's digests of any other set of streams. Over the 107 DEE
streams of the local census (50,728 frames of 2.0, 5.1, 5.1.4 and immersive stereo) every digest
agrees, and every substream is read to its exact end or, for 5.1.4's audio, refused at the immersive
element. The same holds for the public channel-based streams other encoders wrote: DASH-IF's Dolby
test vectors (2.0 and 5.1 at 25 and 29.97 fps), CTA WAVE's `ca4s` sets (2.0 at 30 fps) and Chromium's
channel-based and immersive-stereo test files, 6,670 frames in all, taken out of their MP4 and CMAF
segments with `ac3cli demux` and kept out of the tree. Chromium's A-JOC file is refused at the same
table of contents by both. `AC4DEC_TRACE_DIR` writes the decoder's full trace, one record per line as
`frame substream bit_offset width value name`, the shape `ac4_syntax.py trace` prints.

**Where no stream reaches.** No stream of DEE's reaches most of the syntax: noise fill, VARVAR framing,
time-interleaved A-SPX, the mono, 3.0 and 7.X elements, ASPX_ACPL_1 and A-CPL in a channel pair,
transmitted DRC gains, dialogue enhancement methods 1 to 3 and alternative presentations among it. The
constructed streams above reach the 3.0 and 7.X elements in the SIMPLE and ASPX modes, and every A-CPL
mode. `tools/checks/ac4_syntax_differential.py` reads streams made
for this through both transcriptions: DEE frames with one substream altered (a random tail from a
random bit, a few flipped bits, or a random codec mode), tables of contents for the channel modes no
encoder here writes over random payloads, and, with `--inputs`, a corpus `fuzz_ac4_decode` grew, which
reaches syntax random bits rarely do. Where both read a substream to its end their traces must agree
record for record, and where either stops they must agree up to that point; a table of contents the two
read differently is reported apart. It found a limit on `variable_bits()` groups in the decoder that
the text does not set, and `fuzz_ac4_decode` found an escape code the decoder did not bound.
Comparing the two transcriptions' notes found that they had framed ASPX_ACPL_1's residuals and
`b_use_sap_add_ch`'s parameters differently, each against the channel mapping of Part 1 clause 5.3.4;
both now follow that mapping. The check shows that the two transcriptions read these paths alike,
which a shared misreading still passes.

### The decoder's output processing

Phase D6 adds the sample rate converter for every frame rate but index 13, the output processing a
system configures through `ac4::OutputConfig` (the output level and DRC, dialogue enhancement and the
downmix), and what the decoder does at I-frames, at a change of source and with a frame that does not
decode. Where the text leaves a choice open, the reading is in `src/ac4dec/ERRATA.md`, under "Output
processing" and in "A change of source" and "What an I-frame does not restore".

- **The sample rate converter** (`tests/ac4core/test_ac4core_resampler.cpp`): over 100,000 frames at
  each of the decoder's three ratios and the encoder's inverses the output count is exact, frame by
  frame in Part 2 Table 47's sequence at the 1000/1001 rates and from any starting frame, and a
  converter whose phase jumps goes on converting at the new phase's counts. Tones in the passband come
  out flat to 0.001 dB with everything else 100 dB under them, and converting down, a tone between the
  two Nyquist frequencies comes out 100 dB down. DEE's immersive stereo at 23.976, 24, 25 and 29.97 fps
  decodes at Table 47's counts, and `score_ac4_decode.py` scores it as it scores index 13: each rate lags
  its source by a constant, within 1.3 samples of DEE's half frame plus the decoder's delay.
- **The output level and DRC** (`tests/ac4dec/test_ac4dec_drc.cpp`): Table 162's profiles and the
  curves DEE transmits are the compression curves the text defines; stepped tones at steady state
  follow each profile's static curve within 0.5 dB, and a step in level moves the gain at the attack and
  release time constants. Table 161 chooses the mode for the output level. The output level gain
  equals 2^((Lout - dialnorm) / 6) to 0.01 dB on streams the encoder writes at dialnorms from -31 to
  -17. Transmitted gains apply by channel group, band and subframe on constructed data, and DEE's 5.1
  stream compresses within its own curves in each mode it configures.
- **Dialogue enhancement** (`tests/ac4dec/test_ac4dec_de.cpp`): at 0 dB the output is the output with
  the tool bypassed, sample for sample; at the stream's cap the channel-independent method, its mid and
  side form and the cross-channel method apply the gains their parameters give to 0.01 dB on known
  input, and a frame's matrix moves to the next slot by slot. DEE's speech comes out raised at its cap
  and unchanged at 0 dB.
- **The downmix** (`tests/ac4dec/test_ac4dec_downmix.cpp`): Tables 149 and 149a give the mix gains,
  5.1's downmixes are Table 218's with the stream's gains, the LFE and the loudness corrections, 7.X
  folds to 5.X by Table 219 for each additional pair, and 3.0, stereo and mono take Table 217, the sum
  and the 0.707 upmix; the gains hold from the frame that sends them until another does. DEE's 5.1 tones
  come out of each downmix at the stream's gains to 0.01 dB.
- **The gains on DEE's streams** (`tools/checks/gain_ac4_decode.py`): each stream decoded as coded and
  again at output levels of -31, -24 and -17 dBFS with DRC off gives the coded output times
  2^((Lout - dialnorm) / 6) to 0.01 dB, with what is left beside that gain 100 dB down; each 5.1
  stream's two-channel, Lo/Ro, Lt/Rt and mono outputs are clause 6.2.17's matrix, with the stream's own
  values read from its syntax trace, applied to its coded output, to 80 dB. Both hold to the output's
  rounding. CI runs the committed streams, dialnorms from -26 to -16 dBFS; locally the 115 legs of the
  gold set, dialnorms from -24 (the ATSC A/85 preset) to -16, with DEE's own Lo/Ro and Lt/Rt gains,
  each preferred downmix method and Lt/Rt's Pro Logic II form. DEE's immersive stereo at 24 and 25 fps
  sends a dialnorm of -24 dBFS in its last frame, so the checks stop before it.
- **Start-up, splices and damaged frames** (`tests/ac4dec/test_ac4dec_decoder.cpp`): decoded from each
  of their I-frames, the committed streams give the whole stream's output from the frame after the
  I-frame, whose own audio overlaps a frame the decoder never had: to under -100 dBFS in SIMPLE, to -50
  dBFS in ASPX, whose noise and tone generators run at another phase, and in A-CPL within -54 dBFS by
  the fourth frame, as its decorrelators settle. Spliced at an I-frame, marked 0 or with the counter
  jumping, two streams come out as each decodes alone, the first up to the joint and the second from the
  frame after it, overlapping across the joint frame; spliced between I-frames, the output resumes at
  the next I-frame as the second stream decoded alone. At 29.97 fps the counts follow the new counter's
  phase across a jump and the old sequence across a 0. Under either concealment policy each of three
  damaged frames in a row comes out at its length with the damage reported, and the output is the
  undamaged decode's up to the lost audio and again from the second good frame after it; muted frames
  are silent, repeated ones fade by more than 20 dB a frame, and a frame whose table of contents does
  not read keeps the stream's counter and the converter's counts.

### The encoder

`src/ac4enc` writes AC-4 from the same two standards: mono, stereo, 5.0 or 5.1 at 48 kHz at every
frame rate of Part 1 Table 83 or at 44.1 kHz at `frame_rate_index` 13, at a constant, average or
variable rate, with I-frames where a caller asks for them, the loudness values, DRC's decoder modes,
the stereo downmix's values and dialogue enhancement, in the SIMPLE codec mode or the ASPX mode, with A-SPX
above a crossover: below 96 kbps a channel in mono and stereo, with companding below 64, and below
76.8 kbps a channel in 5.X, as DEE's 5.1 streams switch between 320 and 384 kbps. Below 33.6 kbps a
channel in 5.X it writes ASPX_ACPL_2, and below 22.4 ASPX_ACPL_3, as DEE's 5.1 streams are at 128
and 96 kbps: a downmix coded in the ASPX way and A-CPL's parameters, from which the decoder rebuilds
the channels. 5.0 and 5.1 take the 5.X element in the one form DEE's streams have (`coding_config` 0
and `2ch_mode` 0: L and R a pair, Ls and Rs a pair, C alone, the LFE); its other coding
configurations, chosen frame by frame by the bits they save, 7.0 and 7.1 in the 7.X element,
ASPX_ACPL_1 and A-CPL in stereo are experimental options. It shares
`src/ac4core`'s transforms, windows, codebooks, QMF banks and A-SPX tables and high frequency
generator with the decoder, and writes the syntax through a transcription of the tables of its own.
`ac3cli ac4-encode` writes it raw or in MP4. Five checks stand behind it (`planning/ac4.md`, the
encoder's ladder, and phase E5's exit):

- **Three transcriptions agree.** The encoder records each element it writes in the shape the decoder
  records what it reads. The encoder's tests and the fuzz target `fuzz_ac4_encode` require the
  decoder to read every frame to the end of every substream with the encoder's trace, record for
  record; the fuzz target also decodes every frame and encodes the input a second time, which must
  give the same bytes. The encoder-space harness (`tools/ci/fuzz_ac4_encoder_space.py`) draws
  configurations and adversarial PCM, and compares the encoder's trace, the decoder's and
  `tools/references/ac4_syntax.py`'s through `ac3cli`'s `syntax-trace=` option; it draws mono to
  5.1 and the 7.X layouts, the codec mode the rate picks, SIMPLE or ASPX forced or an A-CPL mode
  forced, the experimental tools, every frame rate, the rate modes, the I-frame options and each
  metadata option, and its `--check-envelope` holds each A-CPL mode's least rate. A refusal of a rate
  as too low for the frame rate and metadata counts only for frames under 400 bytes, and only if the
  same case at 400 bytes a frame encodes.
  The fuzz target reaches every A-CPL mode too. FFmpeg Validate runs it for 120 seconds on each pull request, and the nightly fuzz workflow
  for 900. The A-SPX writer's tests read every interval class, balance, sinusoids and both kinds of
  interleaving back through the decoder's parser, and hold the encoder's reading of the interval
  borders, envelope resolutions and noise borders to the parser's. The encoder undoes the three, four
  and five channel matrices as the 2 x 2 steps they cascade, in a transcription of Tables 178 and 179
  and clause 5.3.3.4 of its own, which `test_ac4enc_multichannel.cpp` holds to every printed matrix
  for every `chel_matsel`, with the steps' parameters chosen or drawn at random.
- **One tone per channel.** Encoded and decoded, each channel's tone comes back at unity gain on its
  own channel, 60 dB or more over every other tone there, the LFE's 47 Hz included: 5.0 and 5.1 in
  SIMPLE and ASPX, and 7.0 and 7.1 in each of the three 7.X layouts (`test_ac4enc_encoder.cpp`, and
  through `ac3cli` in the WAV order `decode` writes). Noise above the crossover in one channel comes
  back in that channel alone, so each `aspx_data` element carries the channels Part 1 Table 213 gives
  it. In the A-CPL modes, whose parameters rebuild the channels band by band, a tone at the centre of
  each channel's own parameter band comes back within 0.5 dB, 40 dB over every other tone, in
  ASPX_ACPL_1 to 3 and in stereo. A pair's level difference and correlation come back with it: for one
  noise at a 6 dB difference within 1 dB and above 0.9, and for two independent noises within 1 dB and
  under 0.3.
- **Readers outside the project.** FFmpeg's raw AC-4 demuxer finds every frame at the size written,
  and its mov demuxer reads the MP4 track (the harness and the codec matrix). Locally,
  `tools/checks/check_ac4_encode_readers.py` holds MediaInfo's frame-by-frame trace to the
  configuration, field by field, CRC included, and has DEE's MP4 muxer mux the raw output: the `dac4`
  it writes is the encoder's, byte for byte, for mono and stereo at both sample rates, 5.0 and 5.1,
  the experimental coding configurations and the 7.X layouts 3/4/0 and 5/2/0, and every substream
  field MediaInfo shows holds the value the encoder's syntax trace wrote. For 3/2/2 the muxer
  leaves out channel group 4, which Part 2 Table A.27 and Pseudocode E.3 both give its top front
  pair, and MediaInfo's summary names that pair Tfc (`src/ac4enc/ERRATA.md`). MediaInfo and the
  muxer read the A-CPL streams as configured as well, ASPX_ACPL_1 and A-CPL in stereo included.
  librempeg decodes the 5.X element's ASPX_ACPL_2 and ASPX_ACPL_3 streams' coded channels to within
  69 dB of the decoder's recovered downmixes, and stereo ASPX_ACPL_2's to 45 dB, and leaves the
  channels A-CPL rebuilds silent, as it does DEE's; it refuses the ASPX_ACPL_1 streams, whose
  residuals and side send fewer bands than the channels they pair with, and reads D5's constructed
  ones, which send as many.
- **Decoded against the sources** (`tools/checks/score_ac4_encode.py`, in FFmpeg Validate): the
  programme fixtures, one tone per channel, a sweep, noise, castanet-like bursts and a panned source,
  mono, stereo, 5.0 and 5.1 (music and film mixes), 48 and 44.1 kHz, 24 to 384 kbps, encoded and
  decoded by the decoder. Every leg lags its source by 4,385 samples, sits within 0.2 dB of unity
  where its SNR is 20 dB or more, and meets SNR, log-spectral distance and ViSQOL floors pinned at
  the first measurement; in the ASPX legs SNR and gain are measured below the crossover of the
  `aspx_data` element that carries each channel, and above it each A-SPX tile's energy against the
  source's, as the decoder's scorer measures DEE's streams. A 5.1 leg's LFE is scored over its whole
  band: it is coded to 140.6 Hz, its first three scale factor bands, as DEE codes it, so what the
  source's 120 Hz low-pass leaves above that counts as noise. librempeg decodes every one of these
  streams, its output 736 samples earlier than the decoder's. In SIMPLE, and below the crossover of
  the ASPX streams without companding, it agrees with the decoder to 83 dB or better, 5.0 and 5.1
  included (82.5 to 90 dB on every channel, the LFE's too); with companding, to 35 to 36 dB, as on
  DEE's companded streams. On the experimental options it does not agree. On the coding
  configurations' streams its channels sit 2 to 42 dB under the decoder's; on D4's constructed
  streams it reads `coding_config` 0 with `2ch_mode` 1 as the decoder does, to 81 dB or better, and
  parts from it on every three, four and five channel matrix and on the 3.0 element's pair, 6 to 19
  dB down, where its pairs of the 5.X element match. It refuses the 5/2/0 and 3/2/2 layouts ("Not
  yet implemented"), and writes a 3/4/0 stream's L on every channel. Above the crossover its A-SPX
  band strays further from the source than the decoder's, as it does on DEE's streams: on the
  castanet-like bursts at 64 kbps it arrives a block of 1,024 samples after each burst and trails
  it, 27 dB from the source's energy per loud block, where the decoder's output is 1.7 dB from it.
  The A-CPL legs, 5.1 at 96 and 128 kbps, 5.0 at 112, 5.1 in ASPX_ACPL_1 at 160 and stereo in both
  modes, are scored as `score_ac4_decode.py` scores DEE's: the coded downmixes, recovered from the
  output, as waveforms below the crossover, each parameter band's level difference and correlation
  against the source's, and on the tones the routing margin.
- **Frame rates, rates and metadata** (phase E5). At every frame rate each frame decodes to the
  samples Part 2 clause 5.11 gives it, over a second in `test_ac4enc_frame_rates.cpp`, and over
  100 000 frames at every rate, across 98 wraps of `sequence_counter`, in a test run on demand; the
  decoded output lags the input by `delay_samples()` and `decoder_delay_samples()`, found by
  correlation, to within a sample. `score_ac4_encode.py`'s frame-rate legs hold music and speech in
  stereo and music in 5.1 at the other frame rates within pinned allowances of the same source at
  index 13 on the log-spectral distance and ViSQOL: to 60 fps within 0.17 dB and 0.012, and at 100 to
  120 fps music at 128 kbps 0.63 to 0.87 dB over and 0.09 to 0.18 under, where the frames' fixed side
  information takes five times its share of the rate. An average-rate stream never needs more than
  the input buffer it signals: `test_ac4enc_rates.cpp` starts a decoder at every frame and runs its
  buffer. Through the decoder's output processing (`gain_ac4_decode.py --encoder`, in CI) the output
  level, each downmix and dialogue enhancement's gains on the encoder's streams equal their formulas
  to 0.01 dB. MediaInfo reads every loudness, DRC, downmix and dialogue enhancement field of 23
  further configurations as the encoder wrote it, and the table of contents' `wait_frames`, frame
  rate and I-frames as configured; it reads a DRC gainset no further than `drc_gain_val`, and with
  `de_ms_proc_flag` twice the parameters Part 1 Table 78 reads. The harness found I-frames at the
  least rate a configuration takes that the encoder could not write: their A-CPL values, a VARFIX
  interval and per-frame metadata cost more than the frame `create()` checked. A frame that holds
  nothing more now sends each as a stream starts it, and `create()` sizes it with the costlier
  interval, which takes stereo at 48 kHz in the ASPX mode from 8 kbps to 9.
- **The race against DEE** (`score_ac4_encode.py --gold`, locally): phase G0's 2.0 legs of music,
  speech and tones from 48 to 768 kbps, encoded again here in the mode DEE writes at each rate, and
  both decoded by the decoder. In ASPX, from 48 to 144 kbps, ViSQOL is within 0.03 of DEE's or above
  it from 64 kbps up, 0.05 above on music at 64 kbps and 0.04 at 96, and 0.06 and 0.09 under DEE's on
  music and speech at 48 kbps. Below the crossover the encoder's SNR is 1.4 to 3.6 dB under DEE's at 48
  and 64 kbps and on music at 96, where its rate loop spends bits where ViSQOL marks the noise, and 0.6
  to 8.2 dB over it on speech at 96 kbps and at 128 and 144; above it the A-SPX tiles sit within 0.35
  dB of DEE's distance from the source or closer. The decoder's pre-flattening, which phase D4 turned
  round, raised both encoders' speech at 48 kbps, DEE's by more; before it, this encoder led DEE there
  by 0.02. In SIMPLE, at 192 kbps the encoder's SNR is 5.6 dB above DEE's on music, 14.9 dB on speech
  and 32 dB on the tones, its log-spectral distance is lower on each (1.04 against 1.17 dB on music),
  and ViSQOL is within 0.01 of DEE's. DEE's 2.0 audio stops changing from 256 kbps; the
  encoder's goes on improving with the rate, to 74 dB on music at 768 kbps, where the QMF banks'
  reconstruction bounds it. At 5.1, G0's film, music and tones from 192 to 768 kbps, ASPX to 320
  as DEE writes them: below the crossover the encoder's SNR is 7.3 to 11.6 dB under DEE's at 192
  kbps and 1.9 to 3.7 under at 256, since DEE keeps 21 to 27 dB below 2 kHz and lets the band from 8
  kHz fall to 3 dB and under, where this encoder spreads its noise across the band; from 288 kbps it
  is within 1.6 dB of DEE's, and from 320 above it, by 3.2 to 3.8 dB at 384 and 15 to 19 at 768.
  ViSQOL is at or above DEE's at 192 and 256 kbps, and from 288 up to 0.05 under it on film and
  0.02 on music, within 0.01 at 768. The log-spectral distance is lower on every leg, and the A-SPX
  tiles within 0.07 dB of DEE's distance from the source or closer. At 96, 128 and 144 kbps, in DEE's
  A-CPL modes, each band's level difference lands 0.02 to 0.13 dB nearer the source's than DEE's on
  music and film, the correlation within 0.007 of DEE's distance, the log-spectral distance lower on
  every leg, and ViSQOL 0.01 to 0.06 above DEE's but for film at 128 kbps, 0.12 under, where the
  coded centre's band below 2 kHz trails DEE's by 9.5 dB of SNR; the tones route 1.2 to 2.9 dB more
  cleanly than DEE's. The coded downmixes' SNR trails DEE's by 3.4 to 9.5 dB, the same spread of
  noise across the band as at 192 kbps. Its scores are pinned.

The race changed the encoder before it was pinned. Its first version trailed DEE by 11.7 dB of SNR
on music at 192 kbps, and removed the top octave of speech and music: its rate loop spent a frame's
bits beyond the masking thresholds in proportion to each band's signal, and Terhardt's threshold in
quiet, taken with a full-scale sine at 96 dB SPL, zeroed content DEE keeps. The loop now spends those
bits where the noise is loudest first, and the model has no threshold in quiet. The ASPX race changed
it again: below 64 kbps a channel no frame can hold its bands at their masking thresholds, and
raising every band's noise over its threshold together left music at 48 kbps with 6 dB of SNR in
every band, where DEE keeps 19 dB in the bass. Such frames now pull every band toward one level of
noise and cap each band's noise at 0.7 to 2 times its energy, the tightest cap the frame holds, since
ViSQOL marks the holes a looser cap leaves more than the noise a tighter one spreads. At 5.1 the race
showed DEE splitting a fifth of its frames into two blocks of 1,024 samples, where this encoder's
transient detector splits under one in a hundred; splitting on a 3 or 6 dB rise as well moved
ViSQOL by 0.02 at most, up on some legs and down on others, and was left out. The A-CPL race
changed the estimate. Estimating each band from its subbands' whole spectrum, the encoder's
ASPX_ACPL_3 routed the 5.1 tones at -3.7 dB, a channel carrying another's tone louder than its own:
the L and Ls tones, within 44 Hz of subband 1's edges, reach into it through the QMF prototype's
transition band, and the centre's prediction there came out 0.5 and 0.2 where C alone is in Lo. A
subband's own band lies in half of its spectrum, so the estimate now takes a DFT of each subband's
slots and reads the bins of its own band, all of them where its neighbours' components carry the
band; the prediction is 1 and 0 there (DEE's 0.9 and 0), and the routing 9.7 dB. The readings the
writer takes, and those it shares with the decoder, are in `src/ac4enc/ERRATA.md`.

### IEC 61937

AC-4's burst types (IEC 61937-14, phase D11) have no oracle: nothing else here writes or reads
them, and no receiver found accepts AC-4. They are checked against the standard's text.
`tests/iec61937/test_iec61937_ac4.cpp` transcribes Part 14's repetition periods, burst sequences,
`Pc` codes and maximum lengths a second time, row by row as printed, and holds the library's
tables to that transcription and both to the arithmetic the standard implies: five bursts of a
sequence span five frames exactly, each burst starts at the IEC 60958 frame nearest its frame's
exact start, and each maximum length is the period less the preamble and the two IEC 60958 frames
of spacing between bursts. A stream packed and read back returns every frame unchanged at every
frame rate of every type, with each burst's period, measured from the carrier as a receiver would
measure it, its place in its sequence and its `Pc` fields as the tables give them; DEE's streams
at four frame rates do the same. For the extension role, a loopback test
(`tests/hearth/test_group.cpp`) sends DEE's 2.0 stream at 48 kHz through `_ac3forge_player@v1`
to a test sink, whose output equals the local decode, rendered the same way, sample for sample.
The two readings Part 14 leaves open, which frame starts a burst sequence and whether `Pd` counts
bits or bytes, are given in `src/forge/src/iec61937/iec61937.cpp`.

## What untrusted input is checked against

Correctness and robustness are different questions, and this page answers only the first. What
happens when the bytes are hostile rather than merely wrong — the trust boundary, the
memory-safety posture, the per-access-unit resource limits, and the gaps — is
[Threat model](threat-model.md).

## What's confirmed against real hardware, and what isn't

The codec itself is platform-independent; only capture, monitor playback and IEC 61937
passthrough touch sound hardware, and how far each is verified differs by platform and by sink —
covered where it's most relevant rather than repeated here:

- [Windows](platforms/windows.md#audio-backend-wasapi) — `MonitorSink` and exclusive-mode
  passthrough bitstreaming (AC-3, E-AC-3 and signed Atmos) are both confirmed against real
  hardware, an Onkyo TX-RZ740 over HDMI.
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
- [AC-4 passthrough](library/muxing-and-sinks.md) — no receiver found accepts AC-4, so no AC-4
  burst has reached hardware; ALSA's path runs against ALSA's `null` device, and Windows, PipeWire
  and macOS cannot send AC-4 at all.
- [Atmos & JOC](concepts/atmos-joc.md#two-limitations) — Dolby's own decoder gates object
  decoding on a keyed authenticity tag; the signer ships in-tree (`ac3::signing`) but this
  project ships no key for it, so its streams are unsigned unless an operator supplies one.
  Objects sharing a direction also can't be perfectly separated. Neither is a conformance gap.
