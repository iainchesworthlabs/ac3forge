# Roadmap

Ideas under consideration — a candidate list, not a commitment.

Organised by theme, and within each theme by where things stand: what's **shipped**, what's **in
progress**, and what's still a **candidate**. Each item keeps a stable ID (`EQ1`, `IO4`) so pull
requests and discussions can reference it, but the ID is a label, not the point — read for the
state of the work, not the numbering. Sizes are rough guesses: **S** (an afternoon), **M** (a
day or two), **L** (a focused week), **XL** (several PRs). The full record for an item —
measurements, file paths, dead ends — sits behind its "Full record" toggle; nothing has been
cut, just moved out of the way of a first read.

## Overview

| Theme | Shipped | In progress | Considering |
|---|---|---|---|
| EQ — Encoder decision quality | 8 | 4 | 1 |
| DC — Decoder and consumer output | 10 | 0 | 0 |
| IO — Streams in and out | 12 | 0 | 0 |
| IM — Immersive and other formats | 3 | 3 | 1 |
| VX — Verification and oracles | 19 | 2 | 1 |
| PF — Performance and portability | 8 | 0 | 0 |
| AP — Library surface, bindings and v1.0 | 7 | 3 | 2 |
| UX — Applications | 7 | 1 | 2 |
| DR — Distribution, release engineering and hardware | 5 | 1 | 3 |

## Where this starts from

Rebuilt at v0.9.0-beta.1 (2026-08-22) from the 2026-08-15 roadmap, most of which had merged by
then; the handful of items still open carried forward under new IDs — old references still
resolve, see the bottom of this file.

The capability tables live in `docs/index.md`; this file is organised around where the remaining
room is, which the tree mostly names itself:

- The E-AC-3 encoder never got the decision-quality passes the AC-3 encoder got in 0.7.0: one
  exponent set per frame, one SNR offset for every channel, dither hard-off, every coupling
  decision static. It is the one landscape leg still behind both FFmpeg and Dolby's encoder.
- Both decoders parse every syntax element but have no consumer output stage: dialnorm is
  reported and never applied, there is no downmix, no concealment, and most metadata is
  discarded on the way through.
- The three container modules are write-only and the CLI has no inspector, so a real `.mkv`,
  `.mp4` or `.ts` cannot be read without FFmpeg — the one tool the project otherwise treats as an
  oracle rather than a dependency.
- Three public specifications have become usable since the last draft: SMPTE opened its whole
  catalogue on 2026-06-17 (so ST 2098-2, the Atmos master bitstream, is now a free PDF), IAMF has
  a v2.0.0 working-group-approved draft with object-based elements, and AC-4's texts are free.
- The verification estate is broad, but E-AC-3 has neither the mirror self-check nor the
  input-space fuzzer that AC-3 has, and the perceptual column has never carried a number in CI.
- The v1.0 freeze has its mechanical pieces and none of its decisions.

## EQ. Encoder decision quality

The 0.7.0 AC-3 work (dbpbcod, LFE exponent refresh, LFE fine offset, delta bit allocation
weighed against its cost) moved the AC-3 5.1/448 leg to +0.9 dB over FFmpeg. E-AC-3 got none of
it: at stereo/192 it is −0.8 dB vs FFmpeg and −1.3 dB vs DEE on SNR, with a spectral distance of
1.95 against FFmpeg's 0.83. Most items here are "do for E-AC-3 what AC-3 already does", then let
both encoders decide from content rather than from the bit rate.

### Shipped

**EQ1 (L)** — E-AC-3 per-channel exponent strategies. LSD 1.54 → 0.95 dB at equal SNR on the
new transient leg (192 kbit/s stereo); stationary legs a wash (−0.22 to +0.35 dB SNR).
<details markdown="1">
<summary>Full record</summary>

The encoder plans exponent runs per stream per frame and writes them in either of Annex E's
two forms — a Table E2.10 code per channel (`expstre` 0) or per-block strategies (`expstre`
1). Table E2.10 turned out to enumerate all 32 run layouts with §8.2.8's own span rule
attached, so the two forms differ only in what strategies they can state; the planner
(`src/forge/src/encoder/exp_strategy.hpp`) weighs each exponent set's bits against the
mantissa precision it buys back, bounded by what the allocator actually gives each bin, and a
proposal is only taken if the encoder's own allocator agrees it costs the frame fewer bits.
Two conformance bugs came out of it, both on paths no stream had ever exercised: `deltbaie` 0
means RETAIN, not "no delta", so a run change needs an explicit `'10'`; and the §E2.2.3 AHT
flags exist only where a stream has one exponent region. The stationary legs are a wash, which
is EQ13's ceiling showing: the only in-loop criterion is bits, so a plan that trades bits for
precision cannot be recognised as a win.
</details>

**EQ3 (S)** — `bamode=1` for E-AC-3. +1.2 to +3.0 dB SNR over `bamode=0`'s value of 2 across
every rate and layout swept, MOS up everywhere too.
<details markdown="1">
<summary>Full record</summary>

`baie` plus the eleven parameter bits in block 0 and one bit in each of the other five, 17 a
frame, buying `dbpbcod` 3. Swept 0–3 at 96/128/192 stereo and 192/256/384/640 5.1: 3 wins every
cell. `floorcod` swept 0/4/7 and left at Table E1.4's 7 (inert on SNR, best LSD). Both
reference AC-3 encoders emit exactly this set, `{2,1,1,3,7}`.
</details>

**EQ4 (S)** — Adaptive `dithflag`, both encoders. Free in bits — the flag is transmitted
either way; trades waveform SNR for perceptual quality per §7.3.4.
<details markdown="1">
<summary>Full record</summary>

Per channel per block: dither where the zero-bit bins hold at least as much energy as the
dither that would replace them, never over digital silence, never on a block-switched channel
(Dolby's own encoder writes `dithflag` as exactly `!blksw`), and — E-AC-3 only — never in a
frame using spectral extension, whose copy-source reconstruction the encoder cannot mirror.
See the pull request's table.
</details>

**EQ5 (M)** — E-AC-3 delta bit allocation under coupling and on AHT streams. Wins on a modest
minority of coupled frames at low-to-mid bitrates; excluded on AHT streams on measured grounds.
<details markdown="1">
<summary>Full record</summary>

No longer skipped for a coupled frame: the coupling channel carries its own `cpldeltbae` like
any full-bandwidth channel, transmitted on the same per-run, per-block terms `D3` already
established for AC-3 — no reuse code, a wanted correction pays its full segment cost again on
every block it applies to. AHT streams stay excluded, on measured grounds: the comparison was
put on the AHT axis and still lost on every AHT-carrying point, because the DCT's own job is
to concentrate six blocks into one coefficient, and that concentration is what the comparison
was reading as quantization error. Whether a correction earns its side info is a closed-loop
decision against the rate fit (fit with and without, keep the higher composite SNR offset,
the E-AC-3 half of what `D3` established for AC-3) — at that real, repeated cost it wins on a
modest minority of coupled frames at low-to-mid bitrates, never enough to make coupling the
reason it's skipped. Also fixed in the same pass: the decoder never read `cpldeltbae` at all
(unreachable before, since delta was off in every coupled frame), which desynchronised the
first coupled frame that carried one once it became reachable.
</details>

**EQ6 (L)** — Content-adaptive coupling. Up to +3.1 dB E-AC-3 stereo and +12.3 dB AC-3
anti-phase stereo; almost every row flat or improved.
<details markdown="1">
<summary>Full record</summary>

(a): AC-3's `chincpl` is per channel now — a block-switched channel is excluded and the rest
still couple, rather than the whole frame losing coupling — coordinates resend only when the
quantized value actually changes, and 2/0 carries a measured `phsflg` (up to +12 dB where the
coupling sum would otherwise cancel an out-of-phase pair). Found and fixed along the way: coded
order follows the FIRST COUPLED channel, not channel 0, once membership is partial — a
structural desync invisible to every size/CRC/exponent-range check, only visible in whole-file
SNR. (b) coherence-driven `cplbndstrc` was implemented and measured against the fixed frequency
template — a wash on SNR and LSD across every rate/layout tried, so it was dropped rather than
shipped for its own sake; the fixed template stands. (c) `ecplangleintrp` — §3.5.5.3's linear
interpolation between band centres — decodes on both sides now; the encoder decides per frame
by actually reconstructing both ways with the fitted band values and keeping whichever is
closer to the real content, which measurably fires (real material chooses it on a meaningful
fraction of frames, not a rare edge case). Measured with `tools/ci/quality_race.py`: almost
every row flat or improved, up to +3.1 dB E-AC-3 stereo and +12.3 dB AC-3 anti-phase stereo. One
tradeoff: AC-3 5.1 coupled loses up to 0.4 dB SNR (192 kbit/s) from coordinate resends genuinely
competing with mantissas for the same tight budget where they previously didn't get the chance
to — the same dynamic EQ5's closed-loop delta decision exists to solve, not yet extended to
coordinates. See the PR body for the full table.
</details>

**EQ9 (L)** — Closed-loop `auto` tool decisions, driven by the frame's own MDCT coefficients
instead of rate alone. +0.11 MOS and +0.36 dB SNR over the rate-only policy, better in 19 of 36
cells, no regressions.
<details markdown="1">
<summary>Full record</summary>

`auto` chose cpl/spx/aht from the rate alone; two measures taken from the frame's own MDCT
coefficients now decide with it — the coupling region's fit against the decoder's own rank-one
reconstruction, and the energy share above the extension frequency. Re-measured on real
programme material (six excerpts of a 5.1 theatrical mix, 32–96 kbit/s per channel, ViSQOL
MOS-LQO beside SNR), the committed fixtures' own landscape numbers unchanged. The extension
ceiling now moves with content (110 kbit/s per channel where the top end is empty, 55 where it
is not) instead of sitting at a fixed 56 measured as SNR on fixtures with nothing above
8.1 kHz. Band edges themselves are still rate-only — `EQ6`/`EQ13`.
</details>

**EQ10 (M)** — Enhanced coupling and transient pre-noise processing: measured, and labelled
rather than made `auto`-worthy. Enhanced coupling wins up to +0.54 MOS-LQO but stays out of
`auto` because FFmpeg misreads its syntax as corrupt; transient pre-noise measures 6.5–24 dB
worse than doing nothing and is not adopted.
<details markdown="1">
<summary>Full record</summary>

Two different reasons. Enhanced coupling is the better-sounding of the two coupling
reconstructions on real material at every (layout, rate) point tried, worth +0.54 MOS-LQO at
96 kbit/s stereo through +0.16 at 384 kbit/s 5.1; every trend row calls it a loss because every
trend row is SNR. It stays out of `auto` because FFmpeg misreads §E3.5's syntax as a corrupt
frame, and `auto` has to stay decodable. Transient pre-noise does not pay at all: over exactly
the samples it touches it measures 6.5–24 dB worse than leaving the audio alone, at every
bitrate, and the gap widens with rate because the substitution's error is a property of the
material (flat at 20.7–22.5 dB) while the coder's own error keeps falling. Block switching gets
there first. Both documented in `docs/concepts/ac3-eac3.md` and
`docs/library/encoding-eac3.md`.
</details>

**EQ12 (M)** — E-AC-3 VBR characterisation and an average-rate mode. Curve published in
`docs/concepts/ac3-eac3.md`; `eac3::AbrConfig` ships as `avg:kbps[,win:frames]` on the CLI.
<details markdown="1">
<summary>Full record</summary>

`quality_race.py vbr` sweeps `VbrConfig::quality` and scores CBR and FFmpeg CBR at the rate
each point actually measured; the curve is published in `docs/concepts/ac3-eac3.md`'s "E-AC-3
rate control: what VBR and ABR are worth" section. Average-rate mode is `eac3::AbrConfig`
(`avg:kbps[,win:frames]` on the CLI): one composite SNR offset held across frames and steered
by an integral controller, over a sliding-window bit reservoir that caps any window's pooled
budget.
</details>

### In progress

**EQ7 (M)** — Content-adaptive bandwidth and rate-dependent `fgaincod`. Partly addressed:
both encoders now take the per-channel-rate curve as a ceiling and put the frame's own
spectrum under it, band by band against Table 7.15's hearing threshold, up to 128 kbit/s per
channel. `fgaincod` follows a measured line from 7 at 38 kbit/s per channel to 0 at 128,
replacing §8.2.12's fixed 4, on AC-3 by default (`encoder.cpp`'s `fgaincod_for`).
<details markdown="1">
<summary>Full record</summary>

Decided on ViSQOL, because waveform SNR prefers the narrowest band at every rate on every
material and so distinguishes nothing. The band-limited-fixture trap turned out to understate
itself: real programme material carries *less* energy above 14.7 kHz than `reference_51.wav`
does, so an SNR-led bandwidth rule narrows harder on real audio than on the fixture. The curve
itself now lives in `ac3::rate_adaptive_fgaincod` (`core/bitalloc.hpp`) so both encoders read
one definition of it rather than two.

**The E-AC-3 half is reachable but not default, and the reason is a real asymmetry rather
than an oversight.** AC-3 gets the curve free: §5.4.3.x hangs `fgaincod` off the `snroffst`
element it already sends every block. E-AC-3's `baie` does not carry `fgaincod` at all, so a
non-default code needs Table E1.4's separate per-block `fgaincode` element — and that element
has no persistence rule, so a block that omits it reverts to 0x4 rather than keeping the last
value. Holding a code for the frame therefore means paying in all six blocks: 132 bits a frame
at coupled 5.1, about 1.1% of a 384 kbit/s one, traded out of mantissa precision. What landed
is the mechanism and the measurement path, not a flipped default —
`eac3::FrameConfig::fgaincod` pins the code (`-1`, the default, leaves the implied 0x4 and
writes no element, byte-for-byte as before), and EQ13's E-AC-3 search now moves `fgaincod`
as a real second axis beside `dbpbcod`, scoring each candidate after a refit against *its own*
side-info cost — restricted, on the measurement below, to codes *below* the default.
Round-tripped through `Eac3MirrorEncoder` at every pinned code including coupled 5.1, where
`cplfgaincod` leads the per-channel run and the LFE's closes it — the exact desync the decode
side hit against a real DEE stream. Bandwidth measured locally on sourced CC0/public-domain
material — VX7 still wants it packaged.

**Measured, and the curve loses** (`quality_race.py fgaincod`, VX7's CC0 speech and music,
E-AC-3 stereo, `tools=none`, FFmpeg as the constant decoder, paired per-4 s-window deltas with
standard errors). It does not beat §8.2.12's `0x4` at any rate on either material: ViSQOL
MOS-LQO is never significantly positive, and is significantly *negative* where the curve moves
furthest — −0.240±0.049 (speech) and −0.105±0.036 (music) at 96 kbit/s, −0.020±0.001 at 128 —
flat above. The noise floor here is genuinely zero, because `fgaincod=4` is byte-identical to
the default by construction: that leg measures `+0.000±0.000` in every cell, so every non-zero
figure is signal.

The side-info bill turned out not to be the reason, which is the useful part. Sweeping all
eight codes prices the element empirically at −0.29 to −0.43 dB SNR and ≈0.00 MOS — codes 0–3
pay it and are perceptually indistinguishable from 4 — while the curve's own code at 96 kbit/s
loses about sixty times that in MOS. It fails on the *code*, not the cost. The same sweep says
why: SNR and MOS are **opposed** along this axis on E-AC-3 (speech/96, SNR rises monotonically
25.49 → 27.37 dB from code 4 to 7 while MOS falls 4.619 → 4.127), and the MOS optimum sits
exactly on `0x4` on both materials at both 96 and 192. AC-3's curve asks for codes *above* 0x4
at precisely the low rates where the divergence is largest, so carrying it across whole is
directionally wrong for this codec. **The shipped default was already the right answer**, and
EQ7's E-AC-3 half is closed as measured-and-declined rather than left unfinished.

That result also forced a correction to what EQ13's search does with the axis: an unrestricted
distortion-driven search reliably buys SNR it can see and spends quality it cannot, measured at
−0.396±0.083 MOS (speech) and −0.097±0.033 (music) against the one-axis search at 96 kbit/s.
Only codes below `0x4` — where the two measures agree, worth +3.3 dB SNR at 640 stereo with MOS
flat and +1.17 dB/+0.29 MOS at coupled 5.1/640 — are offered now.

Two caveats on the evidence, recorded rather than glossed: ViSQOL saturates at its ~4.75
ceiling from 192 kbit/s up, so flat MOS in the top half of the table is absence of evidence
rather than evidence of equality, and SNR/LSD are the only live metrics there; and the 5.1
legs are single-window on synthetic material, because no redistributable native 5.1 programme
source exists.
</details>

**EQ8 (M)** — Close the E-AC-3 stereo/192 gap. The coded-bandwidth fix (EQ7) is worth 1.2–2.7
dB SNR at that rate but does not move the *landscape* number; the remaining gap is
bit-allocation efficiency (EQ2/EQ3's territory), not a tool or search problem — three separate
searches ruled out as the lever.
<details markdown="1">
<summary>Full record</summary>

Partly addressed: the coded bandwidth is no longer fixed at 60 there (EQ7), which is worth
1.2–2.7 dB SNR and up to +0.034 MOS on real programme material at that rate and improves the
high-band ratio with it. What did not move is the *landscape* number, because
`reference_stereo.wav` is FIR-smoothed noise flat to Nyquist: there is nothing inaudible up
there to drop, so the leg gains 0.04 dB and the gap to FFmpeg (0.79 dB SNR, LSD 1.97 against
0.83) stands. Two findings for whoever takes the rest. The remaining gap on that material is
bit-allocation efficiency, which is EQ2/EQ3's, not a tool choice: no tool set closes it, and
`auto`'s AHT is already the SNR-best of them. And AHT itself is SNR-positive but
ViSQOL-negative at every rate and both channel counts measured (+0.6 to +1.9 dB SNR against
−0.024 to −0.066 MOS over eight rate points), with the worst high-band ratio of any set — which
looks like EQ1's whole-frame exponent set (`nchregs == 1`) rather than a rate policy, and wants
EQ1 and a listening test (VX9) before `auto` stops choosing it. Needs VX7's material and VX6's
column in CI for any of this to be visible to the trend gate.

Third finding, same direction as the first two: EQ13's per-frame `dbpbcod` search, now wired for
E-AC-3 (see that entry), was measured directly against this gap on VX7's real stereo material -
`search=distortion` against `search=off`, `none` and `auto` tool sets, 96-640 kbit/s including
192. Effect negligible everywhere, most points inside the search's own switch margin. `dbpbcod`
is not the lever either; see EQ13's entry for why. The gap stays bit-allocation efficiency in a
sense none of EQ2, EQ3 or EQ13's dbpbcod axis reach.

Fourth finding, and it closes the "wire the other axis first" caveat the third one left open:
`fgaincod` IS wired now (EQ7), and it does not move this gap either. Two-axis against one-axis
at 192 kbit/s stereo measures +0.007±0.008 dB SNR and +0.000±0.000 MOS on speech, −0.015±0.020
and −0.001±0.000 on music — inert, and correctly so: the curve asks for 2 at that rate, the
refit rejects it, and the search reproduces the one-axis answer exactly. So the prerequisite
this entry was waiting on has been supplied and spent, and the stereo/192 gap is *still* not a
transmitted-bit-allocation-parameter problem in any axis this project can now search.
</details>

**EQ11 (M)** — E-AC-3 short syncframes (`numblkscod` 0–2) and `convsync`. Shipped for
`eac3-encode`; `atmos-encode`'s object metadata over short frames is unstarted.
<details markdown="1">
<summary>Full record</summary>

Done for `eac3-encode`: `FrameConfig::numblkscod` (default 3, the CLI's `numblkscod:N` tools
token), `AccessUnitConfig` refuses substreams that disagree about it, AHT and the hoisted
(Table E2.10) exponent form are unavailable below six blocks exactly as Table E1.3 requires,
and `convsync` cycles across each group of `6 / blocks_per_syncframe` frames. The decoder's
`numblkscod != 3` path — spec-derived, never before driven by a real stream — now is:
round-trip tests decode a real access unit at every code, including one with a dependent
substream, through `tools/ci/run_codec_matrix.sh`'s FFmpeg strict-decode leg as well as this
project's own decoder. **Not done: `atmos-encode`.** OAMD/JOC's object metadata is timed and
interpolated across a full six-block frame; extending that to a shorter one is unstarted, not
merely unexposed — see `docs/cli/metadata-options.md`'s own note. A CLI-reachable crash
surfaced along the way: `auto` tools selection can choose AHT, which a short `numblkscod`
forbids outright, and `run_eac3_encode`/`run_eac3_encode_multi` asserted on the resulting
rejected config instead of reporting it — fixed to the same clean error every other
unexpressable configuration already gets.
</details>

**EQ13 (XL)** — Distortion-measured parameter search and a perceptual model. A real win on
AC-3 from 448 kbit/s up; on E-AC-3 the two-axis search is inert at 192 kbit/s and actively
harmful at 96 (up to −0.396 MOS), now restricted to the codes where SNR and MOS agree. The
perceptual criterion still loses at every rate tested and stays off by default.
<details markdown="1">
<summary>Full record</summary>

PARTIAL: the measure exists and is validated (`ac3::quality`, decoded-domain distortion pinned
bit-exact against §7.3's real quantizer, plus a cited/tested Johnston+MPEG-1-model-2
tonality/masking model), wired into a per-frame `dbpbcod`/`fgaincod` search
(`EncoderConfig::search`) with real hysteresis, and validated on real CC0/CC-BY material (not
fixture SNR - VX6/VX7's own gap, closed locally for this) against FFmpeg's decode by
SNR/LSD/ViSQOL. The distortion criterion is a real win from 448 kbit/s up; at 192 it trades SNR
against per-band shape and currently costs more than it buys. The perceptual criterion
currently loses at every rate tested - its model is validated in isolation but not yet
calibrated well enough to beat the fixed defaults on real material with rematrixing active.
Both stay off by default. What's NOT done: neither criterion drives EQ2/EQ5/EQ7's knobs or
delta segments yet (only the two `BitAllocCodes` fields the encoder's own dead-end comment
named), and the perceptual model needs further calibration before it is worth turning on. See
`docs/library/quality.md` and `docs/library/encoding-ac3.md`'s Decision search section.

**E-AC-3 wiring, attempted and measured (EQ3 landing unblocked it as the note above predicted).**
`eac3::FrameConfig::search` now exists (`ac3cli eac3-encode ... search=distortion`, and
`plan::apply_tools` reaches it the same way `fast-mdct=`/`dither=` already did), CBR only and
`kDistortion` only - narrower than AC-3's, on purpose, not as a placeholder:

- **CBR only.** VBR/ABR's own budget-fitting is a materially bigger unit to wrap in a candidate
  loop than AC-3's `settle()` is - the delta-segment with/without comparison and ABR's stateful
  reservoir both assume one committed `BitAllocCodes` per frame - and untangling that was scoped
  out rather than rushed. `search=distortion` is silently inert under `vbr=`, the same documented
  boundary EQ5 draws around AHT streams rather than a rejected configuration.
- **`kPerceptual` not offered for E-AC-3.** AC-3's own table above already shows it losing at
  every rate tried; wiring `ac3::quality::PerceptualModel` a second time to chase a criterion
  already known not to win was scoped out too. Accepted but inert if set, same reasoning as VBR
  above.
- **~~`dbpbcod` only, not `fgaincod`~~ — two axes now.** This was the entry's own named
  blocker and EQ7's E-AC-3 half has since closed it: `eac3::FrameConfig::fgaincod` exists,
  Table E1.4's per-block `fgaincode` element is written, and the candidate set moves
  `fgaincod` between §8.2.12's implied 0x4 and `ac3::rate_adaptive_fgaincod`'s value beside
  `dbpbcod`'s `{3, 2}`. Unlike AC-3's, the two axes are not equally priced — `baie` carries
  `dbpbcod` for free but not `fgaincod`, which opens an element in all six blocks — so each
  candidate is scored after a **refit against its own side-info cost** rather than against the
  incumbent's, since the two are not competing for the same number of mantissa bits. The
  measurement below predates that and is the one-axis result; re-running it two-axis is what
  now answers the question.
- **Measured, not assumed the answer would be no.** Real CC0 material (VX7's `programme_music_
  stereo`/`programme_speech_stereo`), `search=distortion` against `search=off`, both `none` and
  `auto` tool sets, 96-640 kbit/s: effect negligible everywhere tried, −0.06 to +0.29 dB, most
  points within the search's own 0.05 dB switch margin of zero. This is a real answer, not a
  failed attempt: EQ3's own sweep already found `dbpbcod` 3 wins every cell it swept on average,
  so a per-frame search restricted to `{2, 3}` with no `fgaincod` to move alongside it has very
  little left to find - the AC-3 search's real win comes from moving BOTH axes together, and
  only one is wired here. `EQ8`'s own stereo/192 gap does not move either, confirming that gap
  is not a `dbpbcod` problem - see its own entry.
- **~~What would actually test this properly~~ — done, and it answered against the axis.** The
  prerequisite this bullet asked for (`frmfgaincode`'s per-channel path, EQ7) was wired,
  `tests/quality/test_eac3_search.cpp` extended as predicted, and the sweep run on VX7's real
  material across 96–640 kbit/s with both axes moving. The two-axis search is **inert where it
  matters and harmful where it acts**: at 192 kbit/s it reproduces the one-axis answer exactly
  (+0.007±0.008 dB SNR, +0.000 MOS), and at 96 it costs −0.396±0.083 MOS on speech and
  −0.097±0.033 on music, because the distortion criterion and perceived quality run *opposite*
  ways along this axis on E-AC-3 (EQ7's entry has the code-by-code numbers). The axis is now
  restricted to codes below §8.2.12's `0x4`, the half where the two measures agree — worth
  +3.3 dB SNR at 640 stereo with MOS flat, and +1.17 dB/+0.29 MOS at coupled 5.1/640.

  So this entry's original negative result **stands and is now better explained**: a per-frame
  search over transmitted bit-allocation parameters has little to find on E-AC-3 not because
  one axis was missing, but because `dbpbcod` was already settled by EQ3 and `fgaincod`'s
  SNR-optimal direction is perceptually wrong. What that leaves genuinely open is a search
  driven by a criterion that tracks perception — `kPerceptual`, still uncalibrated — rather
  than more axes under `kDistortion`.

5.1 external-metric harness alignment: still open, not attempted this pass.
</details>

### Considering

**EQ2 (M)** — Per-channel and per-block SNR offsets. Tried and declined: no setting beat
leaving it off across every constant and material swept; two reference encoders (FFmpeg, DEE)
agree with the shipped behaviour. Read the full record before trying again.
<details markdown="1">
<summary>Full record</summary>

The redistribution was implemented for AC-3 (search the composite, score each stream's real
distortion against the allocation that won, move fine steps towards the worse-served streams,
re-search) and swept over both of its constants — a level-discount exponent of 0/0.5/1 crossed
with a gain of −2/−0.7/0/+0.7/+2 steps per dB — on synthesized stereo, synthesized 5.1 and the
committed 5.1 fixture at 192 and 448 kbit/s. No setting beat leaving it off: best case ±0.1 dB
SNR, typically −0.1 to −0.6, ViSQOL MOS flat throughout. The mechanism does move bits; the
channels receiving them (surrounds carrying broadband noise) cannot convert a fraction of a bap
step into anything, while the channels giving them up are high in the table where each step is
worth real dB. Two reference encoders agree: FFmpeg 8.0.1 and Dolby DEE 6.5.4 both write ONE
fine offset for every stream — coupling channel and LFE included — in all 79 frames of their
coupled 5.1/448 streams in `tests/golden/external-baseline/`. The AC-3 LFE's own measured `+4`
(PR #195) stays as the one per-channel departure. On E-AC-3 `snroffststr` 0x1 and 0x2 were both
emitted and FFmpeg refused both, with and without an explicit block-0 `snroffste`, so this
project's reading of Table E1.4's block-level SNR element disagrees with FFmpeg's somewhere,
and no encoder in reach emits either strategy to arbitrate from — the encoder stays on 0x0.
Note that the emitted layout matched `tools/references/eac3_parse.py`, this project's
independent transcription, so the two readings inside the project agree and it is FFmpeg that
differs. The decoder's own 0x2 path was brought into line with that transcription in passing
(it read no `cplfsnroffst` at all). What is left genuinely untried is the per-BLOCK dimension,
which on E-AC-3 needs a bit allocation per block rather than per frame — six times the work in
the rate search's innermost loop — and has its own measurement to justify that.
</details>

## DC. Decoder and consumer output

Both decoders walk every metadata payload correctly and, outside the downmix levels DC1 now
keeps, still discard almost all of it. The output stage and §7.10 concealment landed with
DC1/DC2, so no consumer surface improvises a fold any more; what remains here is the metadata
depth a receiver needs to do anything beyond play one programme at the right level.

### Shipped

**DC1 (L)** — Decoder output stage: dialnorm, §7.8 Lo/Ro, Lt/Rt and mono downmix, LFE mixing,
line/RF operating modes. Lo/Ro agrees with FFmpeg `-ac 2` to 119–121 dB SNR.
<details markdown="1">
<summary>Full record</summary>

Using the stream's own `cmixlev`/`surmixlev` or E-AC-3 `mixmdate`. Shipped as
`ac3::OutputStage` (`ac3/decoder/output.hpp`) on `DecoderConfig::output`, off by default; both
decoders now keep the levels they used to skip. `ac3cli decode|monitor channels=2|1`,
`downmix=loro|ltrt|mono`, `drcmode=line|rf`, `mix-lfe`, `ltrt-phase=off`; `monitor` folds on its
own when the endpoint renders fewer channels than the programme; the WASM demo plays the
library's fold instead of one of its own. Lt/Rt's surround sum is phase shifted through a
127-tap Hilbert transformer with the direct path delayed to match. Lo/Ro differs from FFmpeg's
`-ac 2` only by §7.8.1's own normalisation divisor. Annex C's karaoke downmix (`bsmod` 7) is
deliberately out: it re-purposes `cmixlev`/`surmixlev` as vocal-channel levels, so it is a
different matrix rather than a variation on this one, and nothing here emits a karaoke stream
to check it against.
</details>

**DC2 (M)** — Error concealment (§7.10), opt-in: repeat-fade or mute, both in the overlap-add
domain so the delay state stays coherent.
<details markdown="1">
<summary>Full record</summary>

Via `DecoderConfig::concealment`: `kRepeatFade` or `kMute`, both working in the overlap-add
domain (the decoders retain the last good block's windowed transform output) so the delay
state stays coherent and the fade at each end is the codec's own window. Reported on the
result as `concealed`; an E-AC-3 access unit whose dependent will not decode renders its bed
alone (`kBedOnly`). `ac3cli decode|monitor conceal=repeat|mute`. Tests damage real encoded
frames, which the differential fuzzers — they only compare successful decodes — never did.
</details>

**DC3 (M)** — AC-3 Annex D alternate syntax (bsid 6, `xbsi1`/`xbsi2`) and informational BSI
fields, encode and decode both ways.
<details markdown="1">
<summary>Full record</summary>

`EncoderConfig::alternate_bsi` writes bsid 6 and spends the two 14-bit `timecod` fields §D1
reclaims on `dmixmod` plus separate Lt/Rt and Lo/Ro levels (`xbsi1`) and the Surround EX /
Headphone / A-D converter flags (`xbsi2`); `EncoderConfig::info` (`ac3::meta::BsiInfo`) makes
`bsmod`, `dsurmod`, `langcod`, `audprodie`, `copyrightb`, `origbs` and the time code
configurable. `FrameDecoder` recognises bsid 6 and reports both groups, the informational
fields and `cmixlev`/`surmixlev` on `DecodedFrame`. CLI: `annexd`, `bsmod=`, `dsurmod=`,
`dsurexmod=`, `dheadphonmod=`, `adconvtyp=`, `mixlevel=`, `roomtyp=`, `langcod`, `copyright`,
`origbs=`, `timecode=`, the four `ltrt*`/`loro*` levels; GUI: the Metadata tab's Service &
production card.
</details>

**DC4 (M)** — E-AC-3 `mixmdate` depth and `infomdat`: programme scale factors, the
mixing-parameter block, pan information and per-block mix configuration.
<details markdown="1">
<summary>Full record</summary>

`mixdef` 0x1–0x3, including `mixdata2e`'s per-channel external scales and `mixdata3e`'s speech
enhancement data, plus the whole `infomdat` group. `MixMetadata` carries all of it, the
independent substream writes it, and `Eac3Decoder` reports `mixing`/`info` on
`DecodedSubstream` and `DecodedAccessUnit`. CLI: `pgmscl=`, `pgmscl2=`, `extpgmscl=`,
`mixdef=`, `premixcmp=`, `mixdata=`, `extmix=`, `auxmix=`, `speechmix=`, `paninfo=`,
`paninfo2=`, `blkmixcfg=`, `infomdat`, `sourcefscod`.
</details>

**DC5 (L)** — Multiple independent substreams (I0–I7) — the structural half. FFmpeg is no
oracle here: its demuxer packs I0 and I1 into one packet, so a second programme makes it
refuse the whole stream.
<details markdown="1">
<summary>Full record</summary>

`ac3::io::scan` groups access units by programme and reports the list on `ScannedStream`;
`ac3::split_access_units` gained a programme-selecting overload and a `programme_ids()`
enumerator (and now gates its identity read on each frame's own `bsid`, since byte 2 is `crc1`
in an AC-3 frame); `DecoderConfig::programme` selects one on decode and
`DecodedAccessUnit::programme` says which arrived; `AccessUnitConfig::additional` carries
further programmes, each with its own layout, rate, dialnorm and DRC controllers, and the
`dec3` box declares them all. On the CLI: `programme=<0..7>` on `decode`/`qc`/`levels`, and
`programme2=` plus its layout/bitrate/dialnorm on `eac3-encode`. Labelling the programmes as
services (`bsmod`) and mixing one against another still needs DC3/DC4 — this is the structural
half. FFmpeg is no oracle here at all: its `substreamid != 0` check does not distinguish
`strmtyp`, and because its demuxer packs I0 and I1 into one packet, a second programme makes it
refuse the whole stream (docs/verification.md).
</details>

**DC6 (L)** — Decode third-party Atmos streams. A committed Dolby-Encoding-Engine JOC fixture
exercises the whole path; decoding it found a real `audblk` coupling bug.
<details markdown="1">
<summary>Full record</summary>

OAMD now reads any number of `md_update_info` blocks at any offset and ramp duration, object
size/zone/elevation/snap/screen reference/distance/explicit priority/gain reuse, differential
positions, inactive objects, several bed instances (standard or non-standard), ISF programmes,
alternate object data, the `trim_element` and the `extended_object_element`, and skips an
unknown `oa_element` by its own size rather than abandoning the payload. JOC reads all five
Table 47 downmix configurations, any clip gain, and per-object band count, quantizer, sparse
mode, slope and data-point count; `reconstruct()` implements the whole of §6.6.5. EMDF parses
the whole of §H.2.1.3 rather than Table 56's one shape. `tests/golden/object-fixture/
dee_joc_514.ec3` is a committed Dolby-Encoding-Engine DD+ JOC stream that exercises all of it;
decoding it also found a real `audblk` bug (`cplfgaincod`/`cplfsnroffst` were skipped when the
block couples). JOC reconstruction now covers bed programmes too, so channel-based-immersive
content exports its channels.
</details>

**DC7 (M)** — Object size, spread and zone constraints on the encode side, per TS 103 420
§5.6.1.
<details markdown="1">
<summary>Full record</summary>

`ObjectPlacement`/`Keyframe` carry `size`, `snap`, `zone` and `enable_elevation`,
`build_payload` writes all four, and the ADM bridge maps `width`/`height`/`depth` onto
`ObjectSize` and `channelLock` onto `snap`. `diffuse`, `zoneExclusion` and `objectDivergence`
stay unmapped for reasons `docs/library/adm-bridge.md` now states individually.
</details>

**DC8 (S)** — 24/32-bit integer PCM, `WAVE_FORMAT_EXTENSIBLE` and RF64 in the WAV/ADM readers —
closes the professional-delivery-format gap that used to need an FFmpeg pre-conversion.
<details markdown="1">
<summary>Full record</summary>

`read_wav` and `WavStreamReader` accepted PCM16 and float32 only. The ADM reader had the
mirror-image hole (integer only, float32 refused). Both readers now take 8/16/24/32-bit
integer PCM and 32/64-bit float, either wrapped in `WAVE_FORMAT_EXTENSIBLE`, with RF64/BW64
`ds64` sizes for files past 4 GB; the header walk and the sample conversion moved into one
shared translation unit so the two cannot disagree. The ADM side detects an IEEE-float master
up front and reads it with this module's own container walk, since the vendored libbw64
refuses to open one at all.
</details>

**DC9 (M)** — Stream tools that don't re-encode: `transcode`, in-place metadata rewrite,
frame-aligned `cut`/`cat`.
<details markdown="1">
<summary>Full record</summary>

(a) `ac3cli transcode in.ec3 out.ac3`: decode and re-encode preserving dialnorm, DRC and mix
metadata — the DD+-to-DD path for optical and AC-3-only HDMI sinks. (b) Metadata rewrite in
place (dialnorm, `compr`, `bsmod`, `dsurmod`) with the CRCs re-stamped; with `LoudnessMeter`
this is also a metadata-only `normalize` (A/85 §8). (c) Frame-aligned `cut`/`cat` with
access-unit-aware boundaries, and a public per-AU timestamp helper. Shipped as
`ac3::io::metadata_edit` (crc1 solved through `ac3::solve_leading_crc`'s GF(2) inverse, only
fields already on the wire rewritable) and `ac3::io::access_unit_timing` over a new
`ScannedStream::access_unit_samples`, which the four container commands now take their
`samples_per_frame` from instead of assuming 1536. `transcode` carries dialnorm and the
source's own `compr` word across verbatim and converts the mix metadata between AC-3's two
`bsi` levels and E-AC-3's `mixmdate` group; per-block `dynrng` has no `bsi` field to stamp into
and is regenerated from `drc=`, reported rather than silently dropped. `strmtyp 2` convertible
streams — the spec's own no-re-encode path, refused by `validate()` — stayed out.
</details>

**DC10 (XL)** — QMF-domain JOC. Mean per-object SNR 22.8 → 28.6 dB (MDCT/MDCT vs QMF/QMF);
still unmeasured against a real licensed decoder — needs hardware and a valid signing key.
<details markdown="1">
<summary>Full record</summary>

`ac3::dsp::QmfAnalysis`/`QmfSynthesis` is the 64-band complex filterbank §7.1 calls for —
640-tap prototype designed in-tree for exact perfect reconstruction
(`tools/generators/gen_qmf_prototype.py`), 128-point FFT on the shared radix-2 core.
`joc::Domain` selects where the matrix is estimated (`AtmosConfig::joc_domain`) and applied
(`DecoderConfig::joc_domain`); `kQmf` is the default on both sides and the MDCT-band path stays
as `joc-domain=mdct`. Mean per-object SNR, four placements: 22.8 dB
MDCT-estimated/MDCT-reconstructed, 23.5 dB MDCT-estimated/QMF-reconstructed (what a licensed
decoder was getting), 28.6 dB QMF/QMF; 20.2 → 26.5 dB on moving objects. Encode
0.62 → 0.74 ms/frame of a 32 ms budget, decode 0.88 → 0.70 (cheaper: the MDCT path's inverse
is pinned to the direct form). Object audio now lags the bed by 576 samples rather than 256 —
`joc::reconstruction_delay(domain)`. **Still unmeasured:** how these streams reconstruct
through a real licensed decoder. Every number above is this decoder measuring this encoder,
and a domain fix is precisely the kind of change that cannot self-validate. The Shield/AVR
path is the route, and it needs two things this branch could not supply: the hardware, and a
valid signing key — a licensed decoder will not engage object decoding at all without the
EMDF authenticity tag, so an unsigned stream tests the 5.1 bed and nothing else. UX8 does not
substitute: it renders *this* decoder's reconstructed objects through Dolby's renderer, which
says nothing about how Dolby's own reconstruction reads this matrix.
</details>

## IO. Streams in and out

`mp4.hpp` and `mpegts.hpp` both say "a container writer and nothing more", `matroska::` exposes
`mux()`/`Writer` only, `ac3::iec61937` only wraps, and the CLI has no inspector, no
machine-readable output and a single failure exit code. Users arrive with containers.

### Shipped

**IO1 (M)** — `ac3cli probe` with JSON output: layout, tools in use per block, dialnorm/compr/
DRC presence, authenticity tag, CRC validity. Versioned (`ac3forge.probe/1`) and documented as
a contract.
<details markdown="1">
<summary>Full record</summary>

bsid, sample rate (incl. `fscod2`), layout, `bsmod`, `chanmap`, the substream map,
`numblkscod`, tools in use per block, frame/AU count, duration, bit rate and VBR statistics,
dialnorm/compr/DRC presence, EMDF payloads, OAMD `complexity_index`, object count and bed,
whether an authenticity tag is present, CRC validity. Shipped: `ac3::io::probe` over a
promoted `ac3::io::read_frame_header` (the header tier, which answers for a syncframe whose
audio the decoder refuses) plus the real decoders under a new
`DecoderConfig::skip_reconstruction` (the parse tier). Per-block tool usage and exponent
strategies come from a new `ac3::FrameSyntax` trace; `detail=blocks` dumps them. The JSON
document is versioned `ac3forge.probe/1` and documented as a contract in
docs/cli/commands.md. An HLS/DASH manifest check is NOT part of it and stays open: it is a
consumer of this document rather than part of it, and IO5 already owns the `ceao`/JOC
signalling half of the same question.
</details>

**IO2 (XL)** — Container readers for Matroska, MP4 and MPEG-TS, plus remux and a shared
container-sniffing layer — `decode`/`qc`/`levels`/`play`/`monitor` and both GUI pickers all
take a `.mkv`/`.mp4`/`.ts` directly now.
<details markdown="1">
<summary>Full record</summary>

Matroska (EBML walk, `A_AC3`/`A_EAC3` blocks), MP4 (`ac-3`/`ec-3` sample entries, `stco`/
`stsz`, fragmented `moof`/`trun`), MPEG-TS (PAT/PMT, stream types 0x81/0x87, PES reassembly),
each yielding an elementary stream for `scan`. All three readers done
(`matroska::demux`/`Reader`, `mp4::demux`/`Reader` including the `dec3` parser and fragmented
`moof`/`trun`, `mpegts::demux`/`Reader` reading DVB/ATSC/registration-descriptor signalling and
all three packet grids, plus `ac3cli demux` and a fuzz harness per container). A new
`apps/common/container_input.hpp` (`ac3::apps::sniff_container`/`elementary_stream_from_bytes`)
sniffs a file's own bytes for the three containers `demux` already reads and, when it is one,
batch-demuxes its first AC-3/E-AC-3 track (zero-copy for Matroska/MP4, owned for MPEG-TS's PES
reassembly) into a contiguous elementary stream — compiled straight into both `ac3cli`
(`support.cpp`'s `read_elementary_stream`, now what `decode`/`qc`/`levels`/`play`/`monitor`
call instead of a bare `read_all`) and `ac3gui` (`qc_controller.cpp`/
`object_decode_controller.cpp`, plus the QC/Inspect pickers' `nameFilters` widened to the
container extensions), the same shared-not-duplicated shape
`recording_sink.hpp`/`fmp4_folder_writer.hpp` already use — never in `ac3::forge` itself, which
stays free of a dependency the containers explicitly say they do not need in return. Remux is
`mkv`/`mp4`/`ts` themselves widened the identical way on their OWN input side (so
`ac3cli mp4 broken.mkv fixed.mp4` already works), plus a new `remux <in> <out> [dvb|atsc]`
command that picks the target by `out_path`'s extension for discoverability; the dec3-repair
case falls out for free since `run_mp4`'s `codec_config` was always built from the re-scanned
bitstream (`ac3::io::build_codec_config_box`), never from whatever the source container
declared.
</details>

**IO3 (M)** — IEC 61937 de-framing and capture-side recognition. Round-trips byte-exactly
against this project's own wrapper and FFmpeg's `spdif` muxer; not confirmed against real
capture hardware.
<details markdown="1">
<summary>Full record</summary>

A burst parser (`Pa/Pb/Pc/Pd`, data types 0x01/0x15, E-AC-3's 4× carrier) and `unspdif`, then
capture-side recognition so an HDMI/S/PDIF capture device or a loopback of a bitstreaming
player records the elementary stream rather than PCM. `ac3::iec61937::BurstReader`/
`unwrap_stream`/`PassthroughDetector`, `ac3cli unspdif`, and detection in `record` (switches to
writing the elementary stream) and `live` (stops rather than encode a session of noise).
Round-trips byte-exactly against this project's own wrapper AND FFmpeg's `spdif` muxer, both
data types, both word orders. Fuzzed via `fuzz_iec61937_unwrap`. Not hardware-confirmed: no
capture device has been available, the same gap the passthrough output side has.
</details>

**IO4 (M)** — Streaming fMP4/CMAF fragmenter — `tfdt`, a rolling HLS playlist, a dynamic MPD,
matching Matroska/MPEG-TS's existing incremental writers.
<details markdown="1">
<summary>Full record</summary>

`mp4::fragment` was batch ("a true live fragmenter would need…", `mp4.hpp`); Matroska and
MPEG-TS have had incremental `Writer`s since 0.9.0, so the GUI live session could target both
but not the one container whose native shape is streaming.
</details>

**IO5 (S)** — DASH JOC signalling and the `ceao` compatibility brand, per DASH-IF IOP Part 8
v5.0.0 §5.3.2–5.3.3.
<details markdown="1">
<summary>Full record</summary>

`dash.hpp` used to say there was no established convention to point at; DASH-IF IOP Part 8
v5.0.0 §5.3.2–5.3.3 names the `tag:dolby.com,2018:dash:EC3_ExtensionType:2018` and
`…ExtensionComplexityIndex:2018` supplemental properties (ETSI TS 103 420 D.2), the E-AC-3
`AudioChannelConfiguration`, and `ceao` as a compatibility brand (`fragment.cpp` wrote
`iso6`/`cmfc` only). `ScannedStream::oba_complexity_index` already supplies the value.
</details>

**IO6 (S)** — MPEG-TS ATSC profile beside DVB, with `scan` now exposing the descriptor fields
both registries need.
<details markdown="1">
<summary>Full record</summary>

A/52 Annex A descriptors, E-AC-3 type 0x87 with 0xCC. `MuxOptions::profile` / `ac3cli ts …
atsc`, with `scan` extended to expose `bsmod_present`, `dsurmod`, `mix_metadata`,
`independent_substreams` and a per-substream description for substreams 1–3, so both
registries' `component_type`/`bsid`/`mixinfoexists`/`substream1-3` carry real values.
`mainid`/`asvc` are authoring values no elementary stream carries and come from CLI options.
Per-programme grouping — which dependents belong to which independent substream — is still
DC5's, so a non-zero independent substream is described by its own bed alone.
</details>

**IO7 (M)** — Object-layer strip without re-encoding — a bit-identical-bed 5.1 rendition
alongside the Atmos one, for Apple's HLS multi-rendition requirement.
<details markdown="1">
<summary>Full record</summary>

Drops the EMDF/JOC skip-field payload so a DD+ JOC stream yields a bit-identical-bed DD+ 5.1
rendition, the `CHANNELS="6"` companion Apple's HLS authoring requirements want in the same
`EXT-X-MEDIA` group. `ac3::io::strip_objects` / `ac3cli strip-objects`, with
`ac3cli fmp4 … fallback-51` writing both renditions and `mp4::build_hls_master_playlist` taking
a rendition list. The addbsi object marker comes out with the container, so nothing downstream
still signals an object layer. Scope is the frame shape `ac3::emdf::walk_frame` maps — this
project's own DD+ JOC output; a frame carrying an object layer in another shape is refused
rather than passed through, which DC6 (widening the object parsers to real third-party
content) is the natural place to revisit.
</details>

**IO8 (M)** — CLI scripting ergonomics — a real exit-code scheme, `help <command>`, a
generated man page and shell completions.
<details markdown="1">
<summary>Full record</summary>

`apps/cli/exit_codes.hpp` names eight codes and every `return` site is classified against it;
`apps/cli/usage.{hpp,cpp}` splits the usage block into topic sections a command row selects
from, so `help <command>` / `--help` print one row and its own grammars and an argument error
prints a pointer instead of the manual; `man` and `completions <shell>` render from the same
table and are generated and installed by the build (the Homebrew formula places the bash/fish
halves); `quiet`/`verbose` plus a stderr progress line on long encodes and decodes.
</details>

**IO9 (M)** — CLI live/record parity with the GUI session — any layout up to 7.1.4, all four
containers, a silence watchdog; receiver hot-swap stays GUI-only.
<details markdown="1">
<summary>Full record</summary>

`record`/`live` take `layout=`/`codec=` (any layout up to 7.1.4, AC-3 or E-AC-3),
`container=raw|mkv|ts|spdif` written incrementally through `RecordingSink` itself (moved to
`apps/common/` and compiled into both front ends), and `watchdog=<seconds>` over
`ac3::audio::SilenceWatchdog`; `live mode=atmos` allocates an `objects=<N>` slot budget once
and binds capture channels to it with `map=`; an AC-3-only passthrough endpoint gets the
parallel 5.1 AC-3 leg instead of a refusal (`downmix=off` to keep the refusal); and
`atmos-encode` assembles real objects behind `src=`/`map=`. Receiver hot-swap stays GUI-only —
a command line has nothing to trigger it with.
</details>

**IO10 (M)** — Loudness of the rendered layout, ITU-R BS.1770-5 Annex 3's advanced-sound-system
weighting — cross-checked against ffmpeg's `ebur128` on 5.1.
<details markdown="1">
<summary>Full record</summary>

`LoudnessMeter` gained a second constructor taking an `eac3::chanmap::Layout` and applying
Annex 3's extended algorithm for advanced sound systems, whose Table 4 weights each channel by
position (1.41 between 60° and 120° azimuth below 30° elevation, 1.00 elsewhere, LFE excluded)
instead of by its slot in a Table 5.8 `acmod` — so `Lrs`/`Rrs`, `Vhl`/`Vhr`, `Lts`/`Rts`, `Cs`
and `Lw`/`Rw` all have a weight and 7.1, 5.1.2, 5.1.4 and 7.1.4 can be metered. `qc` takes
`layout=rendered|bed`; `bed` stays the default and now says out loud when a stream's dependent
substreams were left out. Annex 3's Table 5 gives a second check on every weight. BS.1770-5
Annex 4's object-based rendering is IO12's.
</details>

**IO11 (S)** — QC preset refresh — re-cited to the 2026-07 ATSC A/85 revision, new streaming
and Apple Music Atmos presets; three candidates deliberately left out.
<details markdown="1">
<summary>Full record</summary>

`atsc-a85` re-cited to A/85:2026-07 (approved 2026-07-08), which restates −24 LKFS / ±2 dB /
−2 dBTP unchanged; new `atsc-a85-streaming` from that revision's Annex L.5 (a −23…−27 LKFS
band) and `apple-music-atmos` from Apple's Immersive Audio Source Profile (a −18 LKFS
*ceiling*, which is why `QcPreset` gained a band-vs-ceiling kind). Every preset now records its
document version and date. EBU R 128 s4, Netflix's Atmos Home Mix v2.3 and Amazon were checked
and deliberately left out — the first two are numerically identical to presets already present
and the third has no primary source that could be read; `qc.hpp` and
`docs/cli/metadata-options.md` record why for each.
</details>

**IO12 (M)** — Object-based loudness, ITU-R BS.1770-5 Annex 4 — re-renders a dynamic-object-only
programme's objects onto a named layout before metering.
<details markdown="1">
<summary>Full record</summary>

Annex 4 covers object-based audio (and a combination of channel- and object-based audio), in
which each object is weighted by its own OAMD position rather than by a fixed speaker slot.
IO10 implemented Annex 3 (channel-based, advanced sound systems) and left this half out. Annex
4 itself defines no new weighting table: it says to render the object-based (or combined)
audio to a real loudspeaker configuration first and meter *that* through Annexes 1/3, and to
report which configuration and rendering algorithm did the rendering, since two reasonable
choices can legitimately disagree by several LU (its own worked example, Table 6).
`qc ... objects=<layout>` (`71`/`512`/`514`/`714`) re-renders a dynamic-object-only programme's
objects by their own OAMD position onto the named layout, via a new
`ac3::spatial::pan_direction`/`direction_of`/`position_direction` (the height-aware two-ring
azimuth/elevation panner `ac3::plan`'s own layout-to-layout channel renderer already used
internally, promoted out of it rather than duplicated a third time), then meters the result
through the existing Annex 3 `LoudnessMeter`. Scoped to dynamic-object-only programmes — the
only shape `AtmosEncoder` produces, and what Dolby's own reference JOC streams declare: for
that shape the decoded bed already IS the objects' 5.1 VBAP fold (`oba::oamd.hpp`'s own
comment), so this starts every full-bandwidth target channel at silence and sums each object's
own recovered `object_audio` into it rather than adding to a bed that already carries it and
double-counting. A bed-and-objects programme (third-party content whose bed may carry
independent, non-object material this decoder cannot separate back out) is refused with a
pointer to `layout=rendered`/`layout=bed` instead, rather than risk silently doubling or
dropping content.
</details>

## IM. Immersive and other formats

### Shipped

**IM1 (XL)** — IAB (SMPTE ST 2098-2) reader, all three phases complete. Parsed header matches
the DTS `iab-validator`'s own reference JSON exactly on all 10 sampled streams.
<details markdown="1">
<summary>Full record</summary>

The public-spec replacement for the DAMF item. SMPTE made its entire standards catalogue free
on 2026-06-17, so ST 2098-2:2022 — the Immersive Audio Bitstream that Netflix takes inside IMF
(ST 2067-201) — is a plain PDF with the full element syntax (IAFrame, BedDefinition,
ObjectDefinition with per-sub-block position, snap, zone gains and spread, AudioDataDLC/PCM
essence), and DTS publishes an MIT-licensed parser and validator
(`DTSProAudio/iab-validator`) to check against. Phase 1 (L): an `ac3iab::` bitstream reader in
the `ac3adm::` mould, PCM essence first (the DLC coder is the hard part), tested against the
validator. Phase 2: minimal MXF KLV extraction for IAB track files —
`ac3iab::parse_mxf_iab` (`mxf.hpp`), governed by SMPTE ST 2067-201, a separate and much shorter
standard than ST 2098-2 itself; its own §5.5 clip-wraps the whole bitstream as a single KLV, so
extraction needs none of the base MXF standards' Header Metadata object graph or Index Tables.
Phase 3: `atmos-iab`, mapping onto `ac3::admbridge`'s `ObjectPath` layer (`build_iab()`) —
Table 19 `ChannelID` resolved to `ac3::oba::BedLabel`, §10.3.1 MetaID tracking channel identity
across the frame sequence, `iab_position_to_room()` a direct passthrough since §11.1's own axes
already match `oba::Position`'s. Reader and ingest only; rendering stays with Cavern.
`ac3iab::` (`src/ac3iab`) parses the full §7/§8 Preamble+IAFrame segment framing and every
element in §9's Table 4 tree — IAFrame, BedDefinition (+ recursive BedDefinition/BedRemap
children), ObjectDefinition (+ recursive ObjectDefinition/ObjectZoneDefinition19 children) and
AudioDataPCM, all fully decoded (positions/spreads/snap tolerance resolved via §5.4's
DistanceXY/DistanceZ formulas, gains via §5.5, zone gains via their own separate linear
§10.5.14/§10.6.3 formula — the two are easy to conflate and an early draft did). AudioDataDLC
is read by identity only (AudioDataID, the opaque coded residual as a byte span) per this
phase's own scope; Annex B's lossless predictor/entropy coder is undecoded and stays a
documented follow-up. Validated against `DTSProAudio/iab-validator`'s own real sample corpus
(`test/bitstreams/*.iab`, MIT) as an external oracle, not vendored into this repo per the
project's spec-PDF convention: this reader's parsed header
(SampleRate/BitDepth/FrameRate/FrameCount/MaxRendered) matches that tool's own reference JSON
exactly on all 10 streams sampled, and every frame across all ten streams parses without error
(one stream alone carries 720 real AudioDataDLC elements and 240 ObjectDefinitions).
</details>

**IM2 (L)** — JOC → ADM BWF writer, also the practical IAMF bridge (AOM's `iamf-tools` takes
ADM-BWF input directly).
<details markdown="1">
<summary>Full record</summary>

`decode … adm_out` writes a Dolby Atmos Master ADM Profile BW64 (cartesian coordinates,
`audioBlockFormat` automation, `chna`) from `Eac3Decoder`'s object metadata, object audio and
the bed's own LFE, round-tripped through `atmos-adm` (`ac3adm::write_bw64` +
`ac3::admbridge::write()`, both new). Scoped to dynamic-object-only programmes (this project's
own encoder never writes a bed program; a decoded one is warned about and skipped rather than
written incorrectly) and cartesian positions only. Checking a written master against
MediaConch's own EBU-R 143-style profile rules was not attempted — no MediaConch install in
this environment — so that verification is still open if it turns out to matter. The vendored
libbw64/libadm writers were unused before this; both are now driven by `ac3adm`'s new write
side. Inherits the `AC3FORGE_BUILD_ADM` gate.
</details>

**IM7 (M)** — A public object-scene timeline type (`ObjectScene`), shared by the
station-broadcast example, the GUI live room, `atmos-path` and UX4's live seam.
<details markdown="1">
<summary>Full record</summary>

`AtmosEncoder` takes keyframes; the station-broadcast example, the GUI live room, `atmos-path`
and any live source (UX4) each used to re-invent a scene description. `ac3/oba/scene.hpp`.
JSON, not YAML — the codec target takes no third-party dependencies, so the format is parsed
in-tree, and RFC 8259 is small enough to implement completely where YAML 1.2 is not.
`atmos-path`/`atmos-encode`, the examples and the GUI's export are on it, the keyframe grammar
still reads byte-identically, and `SceneCursor` is the live seam UX4 plugs into. The GUI's own
per-frame encode loops still build `ObjectPath`s directly — a follow-up, not a gap in the type.
</details>

### In progress

**IM3 (XL)** — IAMF / Eclipsa Audio interop. Phase 1 (a channel-based OBU/ISOBMFF writer)
shipped; object elements (phase 2) and an OBU reader (phase 3) are unstarted.
<details markdown="1">
<summary>Full record</summary>

(was `B3`). v1.1.0 is final (`libiamf`, BSD-3-Clause-Clear), a v2.0.0 working-group-approved
draft (2026-07-27) adds object-based elements, and AOM published its Open Audio Renderer v1 on
2026-07-30. IAMF's codec list is Opus, AAC-LC, FLAC and LPCM, so E-AC-3 can never be carried
inside it: the bridge is decode → re-wrap, which is what this decoder already produces. Phase
1: an `iamf::` OBU/ISOBMFF writer with `ipcm` substreams for a channel-based 7.1.4 element from
the E-AC-3 decode. Phase 2: object elements once v2.0 is final. Phase 3: an OBU reader
(`ipcm`/FLAC) onto `FrameConfig`/`AtmosEncoder`. `libiamf` and OAR are oracles, not sources. IM2
(shipped above) is the cheaper, indirect route to the same ecosystem — a decoded programme
already reaches AOM's own `iamf-tools` encoder via ADM-BWF, no code in this repo needed — but
that only carries IM2's own scope (dynamic-object-only programmes, cartesian positions) and
depends on a second, external encoder. IM3 writes the IAMF bitstream directly, for any
7.1.4-coded programme this decoder can render, with nothing else in the chain.

Phase 1 done: `iamf::` (`src/iamf`) writes a channel-based Audio Element (7.1.4ch,
loudspeaker_layout 7, 5 coupled + 2 non-coupled `ipcm` substreams in the §3.6.3.3 order) plus a
Mix Presentation carrying both the mandatory Stereo loudness layout and the 7.1.4 one, wrapped
in IAMF's ISO-BMFF encapsulation (`iamf`-branded `ftyp`, an `iamf` sample entry's `iacb` box).
Codec-blind like `mp4::mp4`/`ac3iab::ac3iab` (no `ac3::forge` dependency); default-on
(`AC3FORGE_BUILD_IAMF`), installed/exported the same way. `examples/mux_iamf.cpp` is the
decode → permute → `iamf::mux()` round trip: it encodes a synthetic 7.1.4 E-AC-3 stream,
decodes it with `Eac3Decoder`, and reorders Table E2.5's bit order into §3.6.2's own
L/C/R/Lss/Rss/Lrs/Rrs/Ltf/Rtf/Ltb/Rtb/LFE via `Layout::index_of`. Tested with an independent
OBU/ISOBMFF walker built into `test_iamf.cpp` itself (no `libiamf` dependency added to the
build) — deliberately proven able to fail: a substream/channel-pairing swap was reintroduced
and confirmed to break the PCM round-trip test before being reverted. No Parameter Block OBUs,
no Temporal Delimiter OBU, no trimming, batch API only — see `iamf/iamf.hpp`'s own header for
the full phase-1 boundary. Phases 2 and 3 are unstarted.
</details>

**IM4 (L)** — AC-4 parse-and-inspect. TOC/presentation/substream-group parsing landed for both
channel-coded and A-JOC/object paths; the separable MP4/MPEG-TS/DASH carriage slice remains.
<details markdown="1">
<summary>Full record</summary>

(was `D4`). The blocking question is resolved: nothing open encodes AC-4, but the Dolby
Encoding Engine already licensed and used for the AC-3/E-AC-3 external-baseline tier includes
`dee_ac4_encoder.exe`/`dee_ac4ajoc_encoder.exe`, and the Dolby Reference Player's
`dlbac4parse` GStreamer element frames (though does not yet decode PCM from) real AC-4 output
on this machine — see docs/verification.md's AC-4 section. TOC / presentation / channel-coded
substream-group parsing landed: a standalone `ac4::` library (deliberately not under `ac3::` —
it depends on nothing of `ac3::forge`, including `ac3::emdf`, whose classic Annex H
sync+length+protection framing turned out not to be what AC-4's own `emdf_info()`/
`emdf_payloads_substream()` actually use), an independent Python reference parser
(`tools/references/ac4_parse.py`), real DEE fixtures (`tools/generators/gen_ac4_baseline.py`),
and `ac3cli probe` support (table + `json=1`, both bitstream_version <= 1 legacy and >= 2
extended TOC paths, 7.0.4 through 22.2 channel-based immersive). A-JOC/direct-coded-object/OAMD
substream group parsing (TS 103 190-2 clause 6.3.2.8-6.3.2.12 — `ac4_substream_info_ajoc()`,
`ac4_substream_info_obj()`, `bed_dyn_obj_assignment()`, `oamd_substream_info()`) landed as a
follow-up: no real fixture reaches this path (`dee_ac4ajoc_encoder.exe` gates on Atmos
mezzanine provenance this project's tooling cannot produce; `dee_ac4ims_encoder.exe` stays
channel-coded despite its name), so it is verified against synthetic hand-built vectors instead
— see docs/verification.md's AC-4 section. `oamd_common_data()` (§6.2.8.1) remains explicitly
out of scope, refused cleanly (`Error::kOamdCommonDataPresent`) rather than misparsed. What
remains: the separable carriage slice (`ac-4` sample entry and `dac4` box in `mp4::`, the AC-4
descriptor in `mpegts::`, DASH-IF §5.3.4 signalling).
</details>

**IM5 (L)** — Land the TrueHD/MLP branch as an explicitly experimental module. A substantial
internal codec already exists on a long-lived branch; needs a rebase, its own build target, and
honest output labelling before it can merge.
<details markdown="1">
<summary>Full record</summary>

(was `D1`). `feature/truehd-atmos-support` (pushed; 21 commits, +8,090 lines, 41 commits behind
`develop`) is far past the old `D1` text: a complete internal lossless codec — stream
assembler, PMQ matrix cascade, Huffman tables from WO 96/37048, automatic predictor and matrix
selection, FIFO timing, end-of-stream terminators, the `16ch_channel_meaning` +
`EXTRA_DATA`/EMDF/OAMD Atmos layer — plus `truehd-encode`/`truehd-decode`/`truehd-atmos`
commands, a GUI lossless lab and 79 test cases. It writes the real FBA major-sync around a
block layout that is, per its own design note, a self-consistent packing of the patent's
inventory and not the shipping layout: no real TrueHD decoder reads it. To merge: rebase (the
command table and `src/forge/CMakeLists.txt` both churned), gate it as its own target
(`ac3::mlp`, `AC3FORGE_BUILD_MLP`) rather than compiling eleven files into `ac3::forge` —
keeping it out of AP1's frozen surface and the library-only packages — remove the ten committed
third-party PDFs (the tree's rule is "not redistributed here"), label the output honestly (an
`.mlp` that `ffprobe` detects and then fails on), fix the design doc's drift (`src/lib` links;
"deferred" vs the landed Atmos layer), delete or justify the superseded Rice coder, and add the
CHANGELOG/docs rows the branch lacks. UX10 carries its front ends.
</details>

### Considering

**IM6 (XL, blocked)** — Real TrueHD interoperability. Blocked on sources that aren't public
(the DVD Forum's MLP reference information) and on a clean-room ruling that doesn't exist yet.
<details markdown="1">
<summary>Full record</summary>

Needs sources that are not public: the shipping block-header and block-data layout (the DVD
Forum's MLP reference information, held at Japan's National Diet Library), the FBA deltas
(40-sample access units, the substream hierarchy, the Atmos substream matrices), and a ruling
on whether bit-level analysis of Dolby-produced streams is allowed at all —
`CONTRIBUTING.md`'s clean-room rule covers published standards and FFmpeg-as-oracle, not that.
Stays blocked until those exist; listed so the state is recorded rather than rediscovered.
</details>

## VX. Verification and oracles

Nine required build legs, sanitizers, clang-tidy, PREfast, CodeQL, per-component coverage
floors, a gold-reference gate on every leg, eleven libFuzzer harnesses (one of them opt-in) and
an AC-3 input-space fuzzer already exist. What remains is mostly what the tree names itself.

### Shipped

**VX1 (L)** — E-AC-3 encoder input-space fuzzing, closing `G4`'s own stated AC-3-only gap.
First finding fixed: `eac3-encode` aborted above the half-rate `frmsiz` ceiling.
<details markdown="1">
<summary>Full record</summary>

Random Annex E tool tokens, `fscod2` rates, VBR, the wide layouts with dependents and object
counts, crossed with the existing adversarial PCM; FFmpeg strict decode where it has a reading
and the in-repo decoder where it does not (a new "no oracle" cell class for ecpl/tpn/fscod2);
regression seeds; per-PR and nightly. `tools/ci/fuzz_eac3_encoder_space.py`. The "no oracle"
class turned out to have more in it than the name suggests: `ffprobe` still walks the
syncframes of every cell FFmpeg cannot decode, so those streams are held to the access-unit
count and the exact byte extent of each one rather than to nothing. `--check-oracles`
re-measures the gap table against the installed FFmpeg; `--check-envelope` measures the
per-layout rate floors and §E2.3.1.3's 11-bit `frmsiz` word ceiling, which at the half rates
sits inside Table 5.18's own rate list. First finding, fixed: `eac3-encode` aborted on an
assertion above that ceiling at every layout.
</details>

**VX2 (L)** — E-AC-3 mirror self-check — per-substream, per-block diffs across every tool
including ecpl/tpn/fscod2/7.1.4, where the in-repo round trip used to be the only check.
<details markdown="1">
<summary>Full record</summary>

`DecoderConfig::trace` used to be "AC-3 only (FrameDecoder); Eac3Decoder does not write one".
For ecpl, tpn, fscod2 and 7.1.4 the in-repo round trip was the only check, and
`docs/verification.md` admits a misreading shared by both sides passes it. Per-substream,
per-block diffs of exponents, bap, delta, AHT gains, coupling and spx coordinates, including
dependents and the tpn hold-back — the facility that fired four frames before the `deltbaie`
symptom on AC-3.
</details>

**VX3 (M)** — libFuzzer harnesses for the metadata parsers, plus a CRC-re-stamping custom
mutator so mutated skip-field bytes actually reach the object parsers.
<details markdown="1">
<summary>Full record</summary>

`emdf::parse_container`, `oba::parse_payload`, `joc::parse_payload`,
`signing::verify_atmos_stream`, `ac3adm::parse_bw64`. `fuzz/README.md` says most mutations used
to die at the CRC check before reaching the object parsers at all — the custom mutator fixes
that.
</details>

**VX4 (M)** — Third-party decode interop gates. Found five real Annex E decoder defects, none
reachable by any stream this project can encode; nightly FATE interop over eight pinned
samples.
<details markdown="1">
<summary>Full record</summary>

Both steps done. `verify_gold_reference.sh` decodes all six committed
`tests/golden/external-baseline` bitstreams on every gold-reference leg, five against FFmpeg's
own decode and the sixth against its source WAV (FFmpeg fails frame 0 of DEE's stereo E-AC-3
stream and conceals it, so it is no oracle there), and the six seed the decoder fuzzers. The
nightly `Interop` workflow runs `tools/checks/verify_fate_interop.py` over eight
SHA-256-pinned FATE samples, fetched rather than committed. Running the first step found
**five** real Annex E decoder defects — the AHT-in-use flags, `cplfgaincod`/`cplfsnroffst`, the
three band-structure default tables, the `first*` per-frame states, and the coupling-state
reset — none of which any stream this project can encode could reach.
`the_great_wall_7.1.eac3` turned out to be a real Annex E arrangement rather than a gap: an
AC-3 core standing in as independent substream 0 per §E2.3.1.2, with an E-AC-3 dependent
extending it to 7.1 per §E3.8.2 — `ac3::io::scan` and `ac3cli decode` both recognise it now,
verified to 41.69 dB against FFmpeg's own decode. Two things are recorded rather than fixed:
`wav_channel_order` writes acmods 2/1 and 3/1 in bitstream order where FFmpeg uses WAV's
FL/FR/FC/BC, and that same sample's OAMD payload does not decode (`oba::parse_payload`'s
pre-existing scope is this project's own encoder shape, not Dolby's).
</details>

**VX6 (M)** — A perceptual column that carries numbers — `mos_lqo` is real (not null) in every
row from 2026-08-23 on.
<details markdown="1">
<summary>Full record</summary>

`visqol-python` is hash-pinned in `requirements-ffmpeg-validate` and installed on the
`ffmpeg-validate` leg; `MOS_WINDOW_S` caps ViSQOL's super-linear cost, and the history appender
has a soft MOS regression tier. Baseline v2 carries MOS on the external side, re-scores both
DEE 5.1 legs (the Ls-channel drop is an artefact of DEE's discrete-multichannel input path;
`--input-format wav_list` does not have it, so `UNVERIFIED_DEE_LEGS` is empty), and adds five
legs, four of them at rates where spectral extension and coupling actually run.
</details>

**VX7 (M)** — Real programme material — two 30s CC0 speech/music fixtures, versioned and
hash-enforced, run alongside the synthetic legs.
<details markdown="1">
<summary>Full record</summary>

Two 30 s CC0 fixtures — full-band speech and music, both natively 48 kHz and losslessly
sourced — run as their own landscape and trend legs beside the synthetic ones, which stay for
series continuity, and are available to the other `quality_race.py` modes through
`--material`. `tools/generators` is documented and the fixture corpus is versioned
(`corpus.json`, `CORPUS_VERSION`) and hash-enforced (`tools/checks/check_corpus.py`).
</details>

**VX8 (M)** — An object-reconstruction quality leg — closes the gap where a 15 dB JOC
regression could pass CI unnoticed. Self-consistency only; no external oracle exists for
object decode at all.
<details markdown="1">
<summary>Full record</summary>

Per-object SNR used to be measured exactly once, in a unit test with a 10 dB floor against
18–35 dB measured (`tests/oba/test_atmos.cpp`), so a 15 dB JOC regression passed CI and no
trend page saw it. `quality_race.py`'s `objects` mode now encodes a committed five-object
scene (`tests/golden/audio/reference_objects.wav` plus its placements) with `atmos-encode`,
decodes it back to per-object WAVs, and records SNR/LSD/leakage/MOS per object at 256 and
448 kbit/s on `docs/object-quality-trend.md`. LSD needed an object-specific form (objects are
narrow-band, so the codec legs' banded measure reads 10–38 dB for a healthy reconstruction);
the out-of-band half became a leakage figure, which is the object-specific failure mode. DC10's
own head-to-head MDCT-vs-QMF domain comparison (`tests/oba/test_atmos.cpp`) predates this trend
leg and stays as a permanent regression test alongside it.
</details>

**VX10 (S)** — Reference-mode end-to-end gate — `linux-gcc` now runs the full gold-reference
suite a second time in reference mode.
<details markdown="1">
<summary>Full record</summary>

`verify_gold_reference.sh` takes `TRANSFORM_MODE=reference`, which puts `mode=reference` on
every encode and decode it runs and suffixes its check labels so both runs' trend rows
survive. The codec matrix gained `fast-imdct=off` decode rows for both codecs beside its
existing `fast-mdct=off` encode row.
</details>

**VX11 (S)** — Explain the 6.0 dB arm64 offset. FMA contraction and libm last-bit differences
both ruled out by direct measurement; root cause still unidentified, so a cross-platform
bitstream-hash gate ships instead of an explanation.
<details markdown="1">
<summary>Full record</summary>

`linux-gcc-arm64`, `linux-llvm-arm64` and `macos-llvm` all score exactly 6.0 dB below every
x86 leg on every channel of the gold gate. `docs/building.md` and `ci.yml` blamed Homebrew's
libm, which the glibc/GCC arm64 rows contradict: it is architectural, not a libm-package
difference. FMA contraction was the leading hypothesis for what "architectural" meant — PF5
tested it directly by pinning `-ffp-contract=off` project-wide, on every leg, and **the
hypothesis is falsified**: the arm64 and macOS legs still measure ~61.8 dB against x86's
~67.8 dB, unchanged to within run-to-run noise from the numbers before the flag existed. The
surviving hypothesis — aarch64's own compiled libm producing different last-bit
`std::cos`/`std::sin` in the transform twiddle tables — was investigated for real this time,
and **is also falsified**, by direct measurement rather than argument: every one of the 2,170
`std::cos`/`std::sin` calls the actual twiddle-table constructors make (`mdct.cpp`'s
`Twiddles`/`Twiddles2`/`FastMdctTables`, `fft_kernel.hpp`'s `FftTables` at this codec's real
transform sizes) is bit-identical between native x86-64 (GCC and Clang) and a real aarch64
cross-build (GCC 16, matching `linux-gcc-arm64`'s major version) run under `qemu-user`, which
implements IEEE-754 arithmetic rather than approximating it. Taken further: the actual
gold-reference gate, run end to end against a real `AC3FORGE_SIMD=aarch64`/`aarch64-neon`
cross-build under that same emulation, does not reproduce the gap either — every one of its 32
checks came back bit-identical to x86-64, not the ~61.8 dB every real arm64/macOS CI leg
measures (`generic` on x86-64 matched both, for the same IEEE-754 reason). Along the way,
`docs/building.md`'s own description of `kAnalysisWindow` as libm-derived was itself wrong —
it is a `consteval` construction with no runtime libm call at all — fixed in the same pass. So
the gap is real, reproducible, and does not come from anything this project can build without
the real hardware CI already has: the two most likely remaining candidates (GitHub's
*natively*-packaged aarch64 compiler versus the Debian cross-compiler package used here, or a
genuine real-silicon FP behaviour `qemu-user` does not reproduce) both need the real runners to
test further, which is now recorded in `docs/building.md` rather than guessed at. What ships
instead of an explanation: a cross-platform bitstream-hash gate
(`tools/checks/check_cross_platform_hash.py`, wired into `verify_gold_reference.sh`) pinning a
SHA-256 of the actual encoded bytes per `(kernel, transform mode)` in
`tests/golden/bitstream-hashes.json` — `x86_64-sse2` and `generic` are pinned from the
measurements above; `aarch64-neon` and the macOS kernel are deliberately left for whoever next
has those real CI logs in front of them, since pre-filling them from the qemu measurement would
pin the wrong number by this item's own finding. `docs/building.md` and `ci.yml`'s header both
carry the corrected history in place of the stale libm explanation.

**Update (DR8):** `macos-llvm-x64`, a new x86_64-on-macOS leg added for DR8's universal
binaries, is a genuinely new data point along the OS-vs-architecture axis - same OS and Homebrew
LLVM/Qt stack as `macos-llvm`, x86_64 instead of arm64. Across three real CI runs it measured
67.80/67.82/67.76 dB, matching the x86 baseline every non-arm64 leg reports, not the ~61.8 dB
`macos-llvm` and every other arm64/aarch64 leg reports. That rules out "something about macOS
specifically" as a contributing factor: the same OS and toolchain now sit on both sides of the
split, differing only by CPU architecture, which is now confirmed rather than merely the leading
hypothesis. The underlying mechanism is still open - this narrows what it could be, it doesn't
name it.
</details>

**VX13 (S)** — Promote the fuzz jobs. Found and fixed a real integer-overflow bug in MP4's
box-size walk — the only failure `fuzz-short` had ever actually recorded.
<details markdown="1">
<summary>Full record</summary>

The claimed track record didn't hold up under a real check of `gh run list`/job-level history
back to 2026-08-09: `fuzz-differential` genuinely had zero failures across 218 push runs, but
`fuzz-short` had five, all the same `UndefinedBehaviorSanitizer` report in `fuzz_mp4_demux`,
clustered in the 24 hours before this item was picked up (first at 2026-08-24T10:42Z) rather
than spread across the window — a live regression, not pre-existing flakiness. Root cause:
`mp4::Reader`'s box walk computed `box_end = parse_pos + box.size` with no check that
`box.size` (attacker-controlled up to `UINT64_MAX` via ISOBMFF §4.2's 64-bit largesize escape)
could be added without wrapping past 2^64; a box using it could send `parse_pos` backwards past
the streaming reader's sliding `window_pos`, and the next iteration's
`parse_pos - window_pos` then underflowed to a near-`SIZE_MAX` `std::size_t` that
`read_box_header`'s own bounds check failed to catch for the identical reason, indexing far
past the buffer. Fixed with one bounds check on the addition itself (`src/mp4/src/reader.cpp`),
the same rejection `read_box_header` already gives a size smaller than its own header; the
crash minimised into
`fuzz/regressions/fuzz_mp4_demux/largesize-escape-wraps-parse-pos-overflow`. `fuzz-short` and
`fuzz-differential` both had their `continue-on-error` deleted in the same PR — the fix removes
the only failure `fuzz-short` had ever recorded, so promoting the job stakes exactly what the
investigation found rather than the roadmap's earlier, unverified claim. `fuzz-nightly` now
also runs the two differential harnesses at its deeper budget (they were never in `run.sh`'s
`BASE_TARGETS`, the same reason `fuzz-differential` is its own job), and `fuzz/corpus/`
persists across nightly runs via `actions/cache` (restore by prefix, save under a run-scoped
key — cache entries are immutable, so there is no in-place update) instead of every scheduled
run mutating from an empty corpus, which is what every job log up to this point actually
showed. Making `Fuzz Regress` a required check is a repository-admin ruleset edit
(`.github/branch-protection.md`), not something a PR can do — the one step this item leaves for
a human.
</details>

**VX14 (S)** — Lint and scan the non-C++ code — `ruff`/`shellcheck`/`actionlint`, plus CodeQL
for Python and JS/TS. `java-kotlin` needs a real Gradle build, carried forward as VX21.
<details markdown="1">
<summary>Full record</summary>

A `script-lint` job runs `ruff` over every `.py` file (curated rule set in `ruff.toml`),
`shellcheck` over `git ls-files '*.sh'` and `actionlint` over the workflows, all three
hash-pinned in `requirements/requirements-lint.txt`. `codeql.yml` is now a language matrix
with `python` and `javascript-typescript` alongside `cpp`. `java-kotlin` is NOT included:
CodeQL's Kotlin extractor has no buildless mode, so `build-mode: none` extracted nothing and
ended as a configuration error on a measured dispatch run — enabling it needs JDK 17, the
pinned NDK and Gradle, i.e. a CodeQL step inside `_build.yml`'s existing `build-android` job
rather than a leg of its own. Carried forward as **VX21** below.
</details>

**VX15 (M)** — Coverage floors for `apps/cli` and `python/`, gated at 40/34% against a
measured 54.0/46.5%. GUI C++ coverage stays out of scope.
<details markdown="1">
<summary>Full record</summary>

`coverage_report.sh` gates `apps/cli` at line 40 / branch 34 against a measured 54.0 / 46.5
(re-checked after roadmap `IO2`'s container-reader/probe work landed in the same window), and
prints a per-command breakdown below the gate so a thin command module is visible rather than
averaged away; `wheels.yml`'s `python-coverage` job runs `pytest --cov` against the built
wheel. Getting a number at all needed `ac3cli` to link `ac3::coverage` itself — `--coverage`
is target-scoped at compile time, so linking an instrumented library was giving it the gcov
runtime and no instrumentation. GUI C++ coverage remains out of scope (it needs a Qt kit on
the coverage leg); `commands/containers.cpp` measured 0.0% at the time - since risen to 30.4%
as roadmap `IO2`'s own read-side work added coverage incidentally, but still well under the
aggregate, so the gap is carried forward as **VX22** below.
</details>

**VX16 (S)** — A ThreadSanitizer leg over the audio layer — first run clean.
<details markdown="1">
<summary>Full record</summary>

`config-linux-llvm-tsan` plus a `Linux LLVM TSan` matrix entry, running a `concurrency` ctest
label over `tests/audio/` and `tests/cli/test_cli_live.cpp` (the headless CLI device paths, new
here — nothing tested them before). The label comes from `catch_discover_tests(...
ADD_TAGS_AS_LABELS)`, which turns Catch2 tags into ctest labels; two labels did already exist
(`Performance`, `gui`), so this follows their convention rather than introducing the mechanism.
`tsan.supp` is checked in and near-empty. `ac3membench` is excluded from the leg: its global
`operator new`/`delete` replacements collide with TSan's runtime at link.
</details>

**VX17 (M)** — PR-time performance comparison, merge base vs head on one runner — non-blocking,
posts a delta table to the job summary.
<details markdown="1">
<summary>Full record</summary>

A `performance-compare` job builds `ac3bench`/`ac3kernelbench` on both sides, runs each three
times, and posts a delta table to the job summary using the same soft/hard tiers
`append_performance_history.py` applies — imported from it, not restated. Non-blocking by
design and absent from `CI Status`; the trend-branch append stays push-only.
</details>

**VX18 (M)** — Automated tests for the app tier — a real Playwright harness for the WASM demo,
instrumented Android bridge tests against a hosted emulator.
<details markdown="1">
<summary>Full record</summary>

A headless browser test of the WASM demo (`docs/platforms/wasm.md`: "every functional claim
above is manual verification") and an instrumented test for the Android bridge's device-free
paths. `apps/wasm/tests/` is a small Playwright harness `build-wasm` now runs against every
build: it serves the just-built demo directory and drives the real `WasmDecoder` Embind API to
decode the bundled fixture, asserting the channel count/sample rate/object
count/duration/moving-object-position values that page's own docs previously recorded as
manual-only. `apps/android/app/src/androidTest/` adds
`NativeBridgeInstrumentedTest`/`PassthroughBridgeInstrumentedTest`, run by `build-android` via
`connectedDebugAndroidTest` against a GitHub-hosted x86_64 emulator (KVM acceleration is
x86/x86_64-only on these runners, so the debug build type now also targets x86_64 alongside the
real device's arm64-v8a — release stays arm64-v8a-only) — every case is a "no receiver
attached" contract check (JNI round trip and `AudioTrack`/`AudioFormat` calls fail safely
rather than throwing or hanging), not the real-hardware passthrough path
`docs/platforms/android.md` still covers as manual verification only.
</details>

**VX19 (S)** — A threat model for untrusted input. Writing it found and fixed a real heap
over-read in the WAV parser.
<details markdown="1">
<summary>Full record</summary>

What is untrusted, the memory-safety posture, per-access-unit allocation caps and decode
resource limits — what a media server wants to read before linking a decoder against internet
input. `docs/threat-model.md`, cross-referenced from `SECURITY.md`, README and
`docs/library/decoding.md`. Every enforced limit is tabulated with the field width it comes
from; the three that are not enforced (no cap on stream length, no decode time bound, ADM
parsers unfuzzed) are recorded as gaps with the mitigation on the caller. Writing it found and
fixed one real defect: `parse_wav` read its `fmt `/`data` chunk fields at fixed offsets past a
tag located by searching the whole buffer, with no bound on either — a heap over-read on a
file whose last four bytes read `"fmt "`.
</details>

**VX20 (M)** — Publish conformance vectors — 60 versioned streams (21 AC-3, 35 E-AC-3, 4
Atmos) with expected decode hashes.
<details markdown="1">
<summary>Full record</summary>

A versioned release artifact of streams per tool and layout with expected decode hashes, so
other decoders can test against this project. The complement of VX4.
`tools/generators/gen_conformance_vectors.py` emits 60 vectors (21 AC-3, 35 E-AC-3, 4 Atmos)
with the source PCM, per-vector hashes, decoded per-channel levels and a derived
FFmpeg-readability column; `docs/conformance-vectors.md` is the usage page and the release
workflow attaches the bundle beside the SBOM and attestations. Hashes are per-toolchain until
VX11/VX12; the source material stays synthetic until VX7.
</details>

**VX21 (S)** — CodeQL for `java-kotlin`, wired into the existing Android build job instead of
a standalone leg.
<details markdown="1">
<summary>Full record</summary>

As a step inside `_build.yml`'s existing `build-android` job rather than a leg in
`codeql.yml`. The extractor needs a real Gradle build (measured: `build-mode: none` extracts
nothing from a 100%-Kotlin app and fails as a configuration error), and that job already
provisions JDK 17, the pinned NDK and the signing key material. `init`/`analyze` (the same
pinned `codeql-action` SHA `codeql.yml` uses) now bracket `build-android`'s existing
`assembleDebug` step, `languages: java-kotlin`, `build-mode: manual` — no separate build
invocation, the job's own debug build already is one. Needed `security-events: write` raised
on `build-android`'s job permissions and on both reusable-workflow callers (`ci.yml`'s
`build-and-test`, `release.yml`'s `build-packages`) — a callee job cannot request a permission
its caller did not grant, and neither call site passed it before. Split out of VX14.
</details>

**VX22 (S)** — CLI tests for the container commands, client-side coverage which library-level
tests never touched.
<details markdown="1">
<summary>Full record</summary>

`apps/cli/commands/containers.cpp` (`mkv`, `mp4`, `fmp4`, `ts`) measured 0.0% line coverage
when VX15 first pointed the gate at `apps/`; re-measured at 30.4% after roadmap `IO2`'s
container-reader/`probe` work landed in the same window and incidentally exercised some of it,
but still well under `apps/cli`'s 54.0% aggregate. Unlike `audio_io`/`live_audio` nothing about
these commands needs a device — they are fully testable headless, and the library-level
container tests do not exercise the CLI paths that wrap them.
`tests/cli/test_cli_containers.cpp` adds direct assertions on `mkv`/`mp4`'s own success output
(including the Atmos-complexity annotation), the base `fmp4` DASH/HLS path with no
`fallback-51` companion, `demux`'s per-container status line (sample rate present vs. the
MPEG-TS "PES payloads, no rate" branch), and three refusal branches nothing had ever reached in
either direction: `reject_legacy_core` (an AC-3-core-with-Annex-E-extension fixture, built the
same header-level way `tests/io/test_elementary.cpp`'s own does), the non-uniform-access-unit
refusal (a spliced three-block/six-block E-AC-3 fixture), and `mkv`'s multi-programme warning
(via `eac3-encode`'s `programme2=` tokens). Split out of VX15.
</details>

### In progress

**VX9 (M)** — A listening test. The apparatus (blind stimulus set, scoring, protocol) is built
and merged; the session itself hasn't been run yet, and needs human time.
<details markdown="1">
<summary>Full record</summary>

README and `docs/verification.md` have carried "no listening test has been run" through nine
releases. One documented MUSHRA or ABX session over the landscape legs on VX7's material, with
the protocol and results on `docs/landscape.md`. `tools/listening/` builds the blind stimulus
set (hidden reference, BS.1534-3's two anchors, one arm per encoder, everything decoded by
FFmpeg so the decoder is a constant) and scores the answers back into a table with confidence
intervals, and the protocol is on `docs/landscape.md`. Building it also found the two things
VX7 had to land first: `reference_51.wav` carries 0.059% of its energy above 3.5 kHz, so both
BS.1534 anchors are inaudible on both 5.1 legs and cannot scale a MUSHRA session there, and the
items are 1.9 s against BS.1534-3's ~10 s. VX7 has since landed real speech/music fixtures — a
session over those, rather than the synthetic 5.1 fixture, is the better one to run.
</details>

**VX12 (L)** — Reproducible bitstreams across toolchains. Every bit-cost decision audited and
found safe; one real libm fragility fixed. The higher-impact findings are calibrated constants
that need a re-validation campaign, not a quick fix.
<details markdown="1">
<summary>Full record</summary>

PARTIAL. Audited every discrete,
bitstream-affecting decision in `src/forge/src/encoder/` (and the one shared call it makes into
`bitalloc.cpp`'s delta-segment bucketing) that a floating-point comparison, argmin or threshold
test gates — as opposed to ordinary DSP arithmetic, which is expected to carry tiny
platform-dependent noise without changing any discrete choice.
**What is already safe:** every BIT-COST decision is integer, and consistently so —
`exp_strategy.hpp`'s whole exponent-run DP (`score`/`waste`/`best[]`), `snr_search.hpp`'s
fitting search, both encoders' SNR-offset search and delta on/off race, and E-AC-3's
hoisted-vs-per-block exponent form choice are all `int`/`long long`/`uint32_t` throughout, with
no floating-point comparison anywhere in the decision itself. This is the pattern every fragile
finding below should eventually follow, and several already do.
**What is fragile, ranked by how much of the bitstream one flipped comparison can restructure:**
(1) `transient.cpp`'s block-switch ratio tests (`p1 * kT1 > prev_level1_` and four siblings,
fed by a cascaded biquad IIR filter accumulating over hundreds of samples) — `blksw` cascades
into MDCT type, coupling/AHT eligibility and rematrix bands, in both encoders, unconditionally,
every block. (2) `eac3_frame.cpp`'s `auto_cplbegf`: `coupling.fit < kCouplingMinFit` (0.99) turns
E-AC-3 coupling on or off for the whole frame, and the constant's own comment is explicit that
it is "not a tuning knob with a comfortable margin" — real frames sit right at it by design.
(3) `ecplangleintrp`'s decode-both-ways comparison (`err_interp < err_direct`) and (4) the AHT
stationarity ratio (`peak <= 10.0 * quietest`), both structural path choices fed by deep
reconstruction pipelines. (5) The rematrix decision
(`min(power_sum,power_diff) < min(power_l,power_r)`, shared by both encoders) and (6) the
coupling phase-flip test (`correlation < 0.0`), both bare comparisons with no margin at all,
though bounded impact near their own tie point since both sides are close in value exactly when
the decision barely matters. (7) Nearest-code/VQ argmin searches with a strict `<` and an
early-exit `break` (`quantize_ecplamp`, `aht_vector_quantize`, `fit_ecpl_band`'s chaos-code
search) — inherent to any nearest-neighbour search over a continuous-valued codebook, not really
"fixable" without a different algorithm. Full file-by-file detail, including several more
moderate-priority findings and everything already ruled safe, is preserved in PR history.
**What is fixed:** `coupling.cpp`'s `quantize_coordinate` and `choose_master` both computed the
shift that lands a coordinate in [0.5, 1) as `floor(-std::log2(value))` — a transcendental libm
call whose last-bit behaviour is not required to agree across implementations, at exactly the
one input class (a value on or near a power of two) where that call's true result is itself an
integer, so any rounding at all can land `floor()` on either side of it. Replaced with
`std::ilogb`, which reads the unbiased binary exponent directly out of the IEEE-754
representation — exact, no rounding, identical on every conformant platform by construction.
Proven behaviour-preserving rather than assumed: a new test
(`quantize_coordinate is exact at power-of-two boundaries`, `tests/encoder/test_coupling.cpp`)
pins both boundary cases directly, the full 3,778,270-assertion test suite passes unchanged, and
the gold-reference gate's three self-encoded streams hash byte-identical to their pre-change
values — real coupling coordinates essentially never land exactly on a power of two, so this
closes a real correctness gap without moving anything on real material.
**What is not done, and why:** the higher-impact findings above are not fixed here. Most of them
(the coupling-fit threshold, the AHT stationarity ratio, the transient detector's own ratios)
are constants and comparisons calibrated against measured MOS-LQO/SNR data on real programme
material (see `docs/library/encoding-ac3.md`), not arbitrary — moving them, even by adding a
margin, is a perceptual-tuning change that needs the same kind of re-validation campaign the
original tuning did, which this item's own scope did not include.

**Finding (1) revisited: proven, not just asserted, on today's toolchains.** The transient
detector's IIR state was this audit's own named deepest risk, and the full fixed-point port
stays future work (scoped precisely below) rather than landed here — but the platform-invariance
question underneath it does not have to stay a guess. The reduction that makes it tractable: with
`-ffp-contract=off` already project-wide (this document's own "Floating-point contraction"
section) and no `-ffast-math`/reassociation flag anywhere in this build, every `+`, `-`, `*`, `/`
in `TransientDetector::Biquad::process()` and its coefficient formula is IEEE-754-mandated
correctly-rounded, evaluated in the fixed left-to-right grouping C++'s grammar gives a chain of
same-precedence operators — none of that has room to disagree between conformant toolchains, on
any architecture, after any number of recursive samples. The only two operations in the whole
class not so constrained are the constructor's `std::cos`/`std::sin` calls (IEEE-754 does not
mandate correctly-rounded transcendental functions), so platform-invariance of the entire
recursive filter — coefficients, cascade, and every downstream ratio comparison, for any input
signal, at a tie point or nowhere near one — reduces exactly to platform-invariance of those two
calls at the six `w0 = 2·π·8000/fs` arguments the six A/52 rates produce.

Tested directly (VX11's own cos/sin bit-identity methodology, applied here to this file's own
call sites rather than the twiddle tables VX11 checked): a minimal harness reproducing
`transient.cpp`'s exact constructor and `Biquad::process()`, comparing the raw IEEE-754 bit
pattern (not the decimal printout, which would itself round) of every coefficient at all six
rates plus a hash of a 2000-sample cascaded recursive drive, across `windows-msvc` (cl
19.51.36256, `/fp:precise`), `windows-llvm` (clang-cl 22.1.2, `/clang:-ffp-contract=off`),
`linux-gcc` (g++ 15.2.0 x86-64), `linux-llvm` (clang++ 22.1.2 x86-64) and `linux-gcc-arm64`
(`aarch64-linux-gnu-gcc-16` cross-compiled, run under `qemu-user` — the same
standards-conformant construction VX11 used and already trusts). All five builds produced
bit-identical output at every rate, every coefficient and the cascade hash — zero divergent bits.
That covers all four of this audit's in-scope x86-64 legs plus the emulated-aarch64 leg VX11
already relies on; it does not touch the real native `linux-gcc-arm64`/`linux-llvm-arm64`/
`macos-llvm` legs, whose gap is VX11's own open question, not this one's (this entry's closing
paragraph, unchanged below, says why that stays out of scope here).

One incidental finding from the same probe, recorded rather than left implicit: at `k16000`
(E-AC-3's lowest half-rate), `kCutoffHz`'s fixed 8000 Hz *is* that rate's Nyquist frequency, so
`std::cos(w0)` lands on exactly -1.0 (all five toolchains agree on this too) and the RBJ
highpass's own passband collapses to zero width — `b0` and `b2` both compute to exactly `0.0`,
the filtered signal is identically zero for any input, and `blksw` is therefore always `false` at
this one rate by construction, independent of programme content. Whether that is the intended
behaviour for E-AC-3's lowest rate is a spec/perceptual question, not a reproducibility one, and
stays outside this audit's scope — it is now a documented, pinned test case rather than an
unexamined gap.

Landed alongside this: `tests/encoder/test_transient.cpp`'s coverage widened from `k48000` alone
(every prior test in that file) to all six A/52 rates, in a new
`TEST_CASE("a loud onset trips the detector at every A/52 sample rate")` pinning the exact
onset-trips/does-not-trip sequence at each rate, `k16000`'s always-false case included with the
reasoning above inline. Deliberately booleans, not raw bit patterns: a hard-pinned coefficient
hash gated on all seven CI legs would collide with VX11's still-open arm64/macOS question the
moment it ran there, where a pinned discrete decision does not, since no CI leg — arm64/macOS
included, per the gold-reference gate's own passing SNR numbers on those legs — has ever been
observed to flip one. It is a permanent, all-legs-safe regression pin that catches any future
change to this exact decision (a formula edit, an accidental FMA reintroduction, an
optimiser-flag change) immediately, at the unit-test level, instead of waiting for it to surface
as an unexplained shift somewhere downstream. Full suite and the gold-reference gate re-run after
the addition: 3,780,657 of 3,780,678 assertions passing, the 21 failing ones all inside
`test_cli.cpp` (four test cases, none in `[transient]` or any encoder path) — a live `fgaincod=`
CLI wiring gap from a different, already-merged item (`ed6bab71`), pre-existing on `main` before
this branch, confirmed by direct repro and flagged separately, not touched here — and the three
gold streams hash byte-identical to their pre-change pins (`x86_64-sse2/fast/{ac3,eac3,eac3_cpl}`
in `tests/golden/bitstream-hashes.json`), confirming a test-only change moved nothing. The
cross-toolchain coefficient/cascade comparison itself is not committed as a permanent script,
matching VX11's own precedent (its cos/sin check is documented investigative methodology in this
file, not a standing CI gate) — the exact reproduction is `transient.cpp`'s own constructor and
`Biquad::process()`, unchanged, built and run with the flags and toolchains listed above.

**The fixed-point port, scoped precisely rather than left as "substantially larger, riskier":**
eliminate the runtime transcendental call rather than porting it — A/52 fixes the sample rate to
exactly six values, so all six coefficient sets can be computed once, offline, at full double
precision, correctly rounded to a fixed Q-format, and embedded as literal integer constants; no
`cos`/`sin` runs for this filter again, at compile time or runtime, which removes the one
non-guaranteed operation in the class instead of porting it. Q1.30 signed 32-bit for coefficients
and state (`b0`/`b2` ∈ [0, ~0.5], `b1` ∈ [-1, 0], `a1` ∈ [-2, 2], `a2` ∈ [0, 1) at every rate,
per the probe's own six coefficient sets above — comfortably inside Q1.30's range); an `int64_t`
accumulator for `process()`'s five-term sum (each term at most a 32-by-32-bit product, five sum
with wide headroom below `int64_t`'s range); one rounding rule (round-half-up: add
`1 << (shift-1)` before the final right shift) applied identically everywhere — integer `+`, `-`,
`*` and a fully-specified shift are exact and platform-independent by construction, unlike
float's correctly-rounded-but-still-lossy arithmetic, which is what would actually earn
"platform-invariant by construction" rather than "platform-invariant on every toolchain tried so
far." `kSilenceThreshold`/`kT1`/`kT2`/`kT3` and the peak tree (`peak_abs`, the five ratio
comparisons) port the same way: integer `p1`/`p2`/`p3` from the fixed-point cascade,
integer-scaled thresholds, comparisons unchanged in shape. The validation gate that would
actually justify landing it: an offline bit-exact simulator FIRST (Python/numpy, no C++), diffing
its `blksw` decision against today's double implementation block-by-block across the fullest
real, broadband corpus available — the in-repo gold WAVs are not enough alone (see "Quality
fixtures are band-limited": `reference_51.wav` is 99% energy below 100 Hz, essentially silent
through an 8 kHz highpass), so this needs the kind of real material roadmap VX7's
versioned-corpus item and this project's other quality work already reach for. Any discrepancy at
all is disqualifying — it is exactly the undocumented perceptual-tuning change the constants'
calibration note above warns against, not a bug to tune away. Why this round still does not
attempt it: today's toolchains now PROVE bit-identical (above) — no live divergence has ever been
observed, so the risk this closes is prospective, not a currently-failing gate — and there is no
existing fixed-point kernel anywhere else in this otherwise entirely floating-point codec to
build on, so the offline-simulator validation step is new infrastructure, not a small addition.
Spending this item's remaining budget porting a working, spec-tuned filter against a
theoretical-only risk was judged the wrong trade against the item's own re-validation cost — the
same call the previous round of this audit made; this round narrows what is actually still open
with proof, rather than repeating the same unproven deferral.

Gating byte-identical gold encodes across *every* leg (the roadmap text's other half) is blocked
on roadmap VX11, not on this audit: VX11 found that a real, standards-conformant aarch64 build
does not reproduce the arm64/macOS gap at all, so the true root cause is still unidentified, and
asserting cross-leg byte-equality today would either be vacuously true on the x86 legs (already
covered by VX11's `check_cross_platform_hash.py`) or fail on arm64/macOS for a reason this audit
cannot name yet. A recorded-decisions replay mode is genuinely independent future work, not
attempted here.
</details>

### Considering

**VX5 (M)** — Dolby Reference Player, wider and in CI. The "decodes to garbage" blocker was an
unnormalised-dialnorm scoring mistake, not a decoder defect; widening the crosscheck loop to
ecpl/tpn/7.1.4/E-AC-3 `compr` and running it in CI is still unstarted.
<details markdown="1">
<summary>Full record</summary>

The crosscheck loop runs `none/cpl/spx/aht/all` only; point it at ecpl, tpn, 7.1.4 and E-AC-3
`compr` — the "no external oracle" claims in `docs/verification.md` are about FFmpeg, and the
licensed decoder is already wired up (S, local). Then make the player's path configurable and
run it as a self-skipping job on the self-hosted Windows runner. The "decodes DEE's own stereo
output to garbage" blocker on this item is answered and was never a decode defect: the player
applies dialnorm, DEE writes a measured dialnorm of 12 on that stream, and the resulting 19 dB
attenuation was being charged to the decode by scoring it against an un-normalised source WAV.
Compensate the 19 dB and the same decode scores 32.19 dB. So `dolby_decode` has to normalise
for dialnorm (or the material has to be encoded at dialnorm 31) before any conclusion is drawn
through it - see `gen_external_baseline.py`'s module docstring.
</details>

## PF. Performance and portability

At 2faf352 on linux-gcc: `plain_51` encodes at 0.49 ms/frame (65× real time), `atmos_4obj` at
0.31 ms; a 180-second 5.1 decode takes 0.79 s since the fast IMDCT became the default. PF5 has
since put 128-bit SIMD behind the transform kernels through a CMake-selected architecture
directory; there is still no threading anywhere in the codec core.

### Shipped

**PF1 (M)** — Bench what is not benched — E-AC-3 encode and every decode path now have
ms/frame series and a real-time gate, on real audio.
<details markdown="1">
<summary>Full record</summary>

E-AC-3 encode and every decode path had no ms/frame series and no real-time gate
(`bench_encoder` ran `plain_51` and `atmos_4obj` on a 440 Hz tone; `bench_memory` already had
the workloads); `kernel_bench` benched the direct IMDCT and not the fast one that is now the
default; the decoder had no Tracy zones. Switched the timing benches to `reference_51.wav` —
the project's own real-audio rule applies to timing too.
</details>

**PF2 (S)** — Inline `to_fixed25` and fuse it with exponent extraction — removed the largest
named remainder of the profile (~7% of the fast path). Byte-identical streams.
<details markdown="1">
<summary>Full record</summary>

An out-of-line exported `std::round` call made about 9,100 times per frame, ~33–38 µs and the
largest named remainder of the last profile. Byte-identical streams on the corpus are the
gate.
</details>

**PF3 (M)** — Fast IMDCT in the two remaining direct spots — `ecpl_channel_spectrum` 4.4–6.9×
faster; a 180s enhanced-coupling decode 5.84 → 3.24s. Byte-identical encodes.
<details markdown="1">
<summary>Full record</summary>

`ecpl_channel_spectrum` (three direct 512-point inverses per coupled channel per block, encode
and decode) and JOC object synthesis (`joc.cpp`, one per object per block — a 16-object frame
spends ~1.7 ms there, more than a whole 5.1 encode, and the WASM demo decodes objects in the
browser). Both forward a `fast` flag, the decoder passing `DecoderConfig::fast_imdct` and the
encoder-internal `ecpl` use `eac3::FrameConfig::fast_mdct`; the 40-stream encode corpus is
byte-identical to before. `ecpl_channel_spectrum` 4.4× (74.8 → 17.1 µs, before PF4, 6.9× with
it); a 180-second enhanced-coupling decode 5.84 → 3.24 s, a 30-second 15-object decode
6.47 → 4.83 s. `joc::reconstruct` still runs its bed ANALYSIS (five forward MDCTs per block)
direct, which is now the dominant cost of an object decode — see PF8.
</details>

**PF4 (M)** — FFT core follow-ups (radix-4/split-radix codelets) — a 180s 5.1 AC-3 decode
4.19 → 2.92s. Encodes byte-identical.
<details markdown="1">
<summary>Full record</summary>

The generic iterative radix-2 with an explicit bit-reversal pass (`fft_radix2.hpp`) becomes
fixed-size radix-4/split-radix codelets for P = 64/128/512 with trivial-twiddle elimination.
Done as `fft_kernel.hpp`: compile-time-specialised radix-4 stages with a trailing radix-2
stage where log2(P) is odd, the first stage's unit twiddles gone, and the digit-reversal
folded into each caller's own input-producing loop instead of running as a pass. The kernel
measured standalone is 1.6–1.75× across P = 64/128/512; at the caller level, median of ten
interleaved runs, 1.24–1.86× per fast transform against an 0.90–1.07× spread on the unchanged
ones. `dft512` against its own O(N²) sum improved from 1.9e-15 to 1.7e-15.
</details>

**PF5 (L)** — SIMD kernels through per-architecture directories, plus an AVX2 follow-on tier —
but the two biggest wins found along the way were pure bookkeeping fixes (1.82×/2.90× on
Atmos decode), dwarfing the ~18% from all transform vectorisation combined.
<details markdown="1">
<summary>Full record</summary>

Through CMake-selected per-architecture directories
(`src/forge/src/internal/arch/{generic,x86_64,aarch64}/`, the same mechanism as
`profiling/tracy_{enabled,disabled}` — no `#ifdef`): FFT butterflies, windowing, `to_fixed25`,
`band_energy`, the psd/mask loops. Raspberry Pi, the Shield and WASM are where a 2–3× decode
matters. Done with the directory mechanism, not `std::simd`: libstdc++ has only the
pre-standard `<experimental/simd>` and libc++ and MSVC have neither, so it is unavailable on
four of the six toolchains in the matrix. The kernels live once, written against two 128-bit
types (`f64x2`/`i32x4`) the selected directory supplies; SSE2 and base ARMv8-A Advanced SIMD
are used, both architectural rather than optional, so no `-march=` and no runtime dispatch.
Bit-exactness is the gate rather than a tolerance, held by `tests/core/test_simd_kernels.cpp`
(the seam's primitives, since the kernels built from them are composition and inherit the
guarantee) and by the codec matrix producing byte-identical output across builds.
`-ffp-contract=off` is pinned project-wide as part of this, for the SIMD seam's own
bit-exactness argument (contraction would let the compiler re-fuse a vector op back into an
FMA the intrinsics cannot express) — but that is independent of VX11's question, which this
item's own measurement answered in the negative: see VX11.

Landed alongside PF4 rather than after it, and PF4's own radix-4 restructuring absorbed the FFT
butterfly this item originally scoped: that kernel's win is now algorithmic (fewer operations),
not wider-lane, and PF4's header carries its own correctness argument rather than this seam's.
What is actually vectorised instead: `dct4_scaled`'s pre/post-twiddle loops around PF4's kernel
(adapted to gather from and scatter to its digit-reversed layout — the gather/scatter ends stay
scalar, the arithmetic between them is two-wide), the IMDCT twiddle stages, analysis windowing,
`dft512`'s normalisation, the §7.2.2.2 exponent-to-PSD conversion, and a batched `to_fixed25`.
`band_energy` gets faster transitively, through the MDCT it calls, rather than by any code of
its own. Not done, and left for a later item: the direct-form transforms (reductions —
reassociating them would change the reference path's numbers), a WASM `simd128` directory (no
`emsdk` on this session's machine to verify one against, and WASM reaches `generic` — a complete
and correct scalar implementation — until then), and AVX2/NEON-wider dispatch.

The AVX2 half of that remainder has since landed as a follow-on: a second, AVX2-flagged object
library selected per-process by CPUID (`has_avx2()`, `AC3FORGE_SIMD_TIER` to force either way),
carrying 256-bit windowing and twiddle stages plus batched 4-transform IMDCT/MDCT kernels for
the JOC object loop, the JOC bed analysis and both encoders' per-channel loops. Output stays
byte-identical across tiers. NEON-wider is still not done, and there is no equivalent gap on
aarch64 to close — base ARMv8-A is already the widest guaranteed set there.

Two findings from that work belong here rather than in the SIMD description, because they
changed where this project should look next. Profiling by source line rather than by symbol
found `FrameParameters::at()` re-walking an O(objects) offset list on every coefficient access
(a frame O(objects²), ~44% of a 12-object decode profile) and `aht_bin_gaq_bits` fully
quantising six mantissas per candidate gain to read one integer width off each (~43% of an
E-AC-3 encode profile). Fixing those two — pure bookkeeping, unchanged output — was worth
1.82x/2.90x on a 12-object Atmos decode and 1.70x on `eac3_51_auto`, against roughly 18% from
all the transform vectorisation combined. The transforms are now ~1-6% of any profile and are
not where the remaining cost is; `docs/performance-trend.md` records the method and the trap
(symbol self-time attributes inlined header code to its caller). FMA3 was measured at ~1% and
declined, since it perturbs results and would break the cross-tier byte-identity gate.
</details>

**PF6 (M)** — A latency budget, measured empirically — exposed on every encoder/decoder and
the C API/Python bindings.
<details markdown="1">
<summary>Full record</summary>

Documented end-to-end encoder latency (frame granularity, MDCT/IMDCT overlap, lookahead, the
§3.7 hold-back) in `ac3/latency.hpp`, measured it empirically
(`tests/decoder/test_latency.cpp`: an impulse and a tone burst through a real encode→decode,
located by peak and by cross-correlation), and exposed it -
`latency()`/`latency_samples()` on every encoder, `latency_samples()` on both decoders,
`ac3forge_encoder_latency_samples()` in the C API, `FrameEncoder.latency` in Python. An Atmos
object waveform's own term is `joc::reconstruction_delay(joc_domain)` — 832 samples end to end
with the default QMF reconstruction, not a flat multiple of the MDCT overlap. EQ11's short
syncframes (the low-latency mode this was meant to document) have not landed - the E-AC-3
latency section names the 512-1024-sample figures they would enable and says so.
</details>

**PF7 (L)** — A minimum-footprint decoder profile — 403 KB image, 238 KB peak heap, proven on
a real cross-compiled bare-metal CI leg.
<details markdown="1">
<summary>Full record</summary>

`AC3FORGE_MINIMAL_DECODER` builds a decode-only `ac3::forge_minimal` with no exceptions, no
RTTI and no direct-form transform tables (an explicit 1.81 MiB ROM budget, measured on the
object file), proven on a cross-compiled `arm-none-eabi`/QEMU CI leg (`apps/baremetal`,
`build-footprint`) that decodes real AC-3/E-AC-3 to the host build's own levels in 403 KB of
image and 238 KB of peak heap. Two requirements are recorded as open gaps rather than
half-enforced: zero heap traffic in the decode loop (today: 45-87 allocations/frame) and a
float32-only internal path - see `docs/building.md`'s Gaps section.
</details>

**PF8 (S)** — The decoder's JOC bed analysis was still running direct forward transforms — now
switchable, 11.0× faster on the analysis kernel; only matters under `joc-domain=mdct`, not the
default.
<details markdown="1">
<summary>Full record</summary>

`Eac3Decoder` called `joc::reconstruct` with `fast_mdct = false` hardcoded, so every object
frame under `joc-domain=mdct` ran five direct §8.2.3.2 forward transforms per block, and there
was no decoder-side forward switch to hang a fix on — `DecoderConfig::fast_imdct` names only
the inverses. Decided: add one, default it ON, the same gate every other fast path here already
passed. `DecoderConfig::fast_mdct` now carries `joc::reconstruct`'s own `fast_mdct` parameter
from `decode`/`monitor`/`live` (CLI: `fast-mdct=off`/`mode=reference`, now reaching decode as
well as every encode path they already covered). Evidence: 1.3e-13 worst relative error at the
transform level (the same forward kernel `EncoderConfig::fast_mdct` already validates), full
`joc::reconstruct` output agreeing 321-325 dB SNR against the direct form over three real
encoded-and-decoded objects, and the bed analysis kernel itself — isolated from object
synthesis, which this switch does not touch — 11.0x faster on a release build (238 µs against
2628 µs per block's five-channel analysis, `ac3kernelbench`'s
`joc_reconstruct_mdct_4obj`/`_direct`): a fixed ~2.4 ms saved per frame regardless of object
count, ~2.2 s projected over a 30 s decode. Has no effect under the default `joc-domain=qmf`,
whose filterbank has only the one evaluation, so this only matters for a stream (this
project's own `joc-domain=mdct` request, or a third-party one whose matrix was estimated that
way) that actually reconstructs in the MDCT-band domain.
</details>

## AP. Library surface, bindings and v1.0

### Shipped

**AP2 (M)** — Naming and error-type sweep before the freeze — three real fixes, one deliberate
non-fix (written down as a convention, not left as an inconsistency).
<details markdown="1">
<summary>Full record</summary>

`ac3/oba/joc.hpp`/`joc_tables.hpp` now declare `ac3::oba::joc`, matching the path (source- and
ABI-breaking — every in-repo caller, the two ABI allowlists and
`tools/generators/gen_joc_tables.py` moved with it); `ac3/sinks/` — a directory that only ever
held one file, and that file was never a `Sink` type in the first place, `ac3::audio`'s
`PassthroughSink`/`MonitorSink` are the real ones — is now `ac3/iec61937/`, header path only,
the `ac3::iec61937` namespace was already correctly named (source-breaking for the `#include`
path; ABI unaffected since mangled names carry the namespace, not the directory); `FrameError`
gained `describe()` (`src/forge/src/encoder/silent_frame.cpp`), and the Python binding's
`EncodeFailure` now builds `Ac3EncodeError`'s message from it instead of the enumerator's own
name. `ac3::FrameEncoder`/`ac3::eac3::FrameEncoder` sharing a name across namespaces is left
alone — it is the same base-case-in-the-bare-namespace, extension-in-a-nested-one split the
Python bindings already mirror with a real `ac3.eac3` submodule, not an inconsistency — and is
now written down as a convention in `docs/library/index.md` alongside the codec-vs-codec-blind
namespace split.
</details>

**AP3 (L)** — Pimpl sweep — every exported class with non-trivial state now hides it; five
plain config aggregates stay value types on purpose.
<details markdown="1">
<summary>Full record</summary>

Every `AC3FORGE_EXPORT` class with non-trivial state now hides it behind
`struct Impl; std::unique_ptr<Impl> impl_;`, the same pattern the three WAV classes already
used — `ac3::FrameEncoder` and `ac3::eac3::FrameEncoder` (finished from their half-done
`PlanScratch`/`FrameState`), `FrameDecoder`, `Eac3Decoder`, `oba::AtmosEncoder`,
`eac3::AccessUnitEncoder`, `meta::RangeController`, `meta::HeavyCompressor`,
`meta::LoudnessMeter`, `analysis::LevelMeter` and `iec61937::Eac3BurstPacker`. See
`docs/library/index.md`'s new "pimpl" convention note for the growth-after-1.0 decision and
why `EncoderConfig`'s `verify::FrameTrace*` (and its siblings) are not part of that promise.
</details>

**AP4 (M)** — An ABI gate — `abidiff` plus a checked-in symbol allowlist, advisory until AP1's
freeze. Demonstrated locally: catches a stray accidental export.
<details markdown="1">
<summary>Full record</summary>

`abi-gate` in `ci.yml` builds `config-linux-llvm-shared` at HEAD and at a comparison point (a
`git worktree`, mirroring `performance-compare`) — the PR's own merge base on a pull request,
the last `v*` tag on a push or tag — and runs `abidiff` across every shared library the preset
builds (discovered from the build tree rather than a fixed list) under
`tools/ci/abi-suppressions.ini`, plus `tools/ci/check_abi_symbols.py` — a checked-in
`nm -D --defined-only` allowlist per library under `tools/ci/abi-allowlist/` — advisory
(`ABI_ENFORCE: 'false'`) until `AP1`'s freeze, at which point flipping that one value is the
whole promotion. `examples/capi_encode_decode.c` and `capi_encode_eac3.c` now pin
`C_STANDARD 11`/`C_STANDARD_REQUIRED ON`/`C_EXTENSIONS OFF` on top of the `-Wpedantic` they
already got from `ac3::warnings`, so `ac3forge_c/ac3forge.h` is proven strict-C11-clean on
every desktop leg that already builds them, not a new leg. Demonstrated locally (WSL2): a
stray `AC3FORGE_EXPORT` added to `ac3::internal::resolve_operating_mode` (an internal decoder
helper, never meant to be public) makes `check_abi_symbols.py` fail with `+
ac3::internal::resolve_operating_mode(ac3::DecoderConfig const&) (newly exported, not in
allowlist)`; reverted before merging.
</details>

**AP5 (L)** — C API completeness — an E-AC-3 encoder (plain and wide layouts), `scan`,
caller-buffer decode forms, and loudness/level/QC metering all now mirror the C++ surface.
<details markdown="1">
<summary>Full record</summary>

`ac3forge_eac3_encoder_t`/`ac3forge_eac3_access_unit_encoder_t` cover plain E-AC-3 and
dependent-substream wide layouts (7.1/5.1.2/5.1.4/7.1.4) with the Annex E tools including
`auto`, mirroring `ac3::eac3::FrameEncoder`/`AccessUnitEncoder`. `ac3forge_scan` now reads
layout, every programme and the DVB/ATSC service fields a muxer's descriptors want, without
decoding audio.
`ac3forge_decoder_decode_frame_into`/`ac3forge_eac3_decoder_decode_access_unit_into` decode
into caller-owned planar storage, preserving §3.7's transient pre-noise hold-back exactly (a
held-back frame leaves the caller's spans untouched, same as the value form leaves its result
unpopulated). `ac3forge_loudness_meter_t`/`ac3forge_level_meter_t`/`ac3forge_qc_preset`/
`ac3forge_evaluate_qc_gate` mirror `ac3::meta::LoudnessMeter`/`ac3::analysis::LevelMeter`/
`ac3::meta::qc`. See `docs/library/c-api.md` for what every one of these deliberately trims
(mixmdate/infomdat, VBR, `numblkscod`, `process_interleaved`, the presentational level-meter
string/geometry helpers, `AccessUnitTiming`'s one-line seconds/timescale conversions) and for
the custom DRC profile, still a deliberate omission to revisit at 1.0.
</details>

**AP7 (M)** — Install and export completeness — pkg-config files, `ac3adm`/`admbridge` now
install/export, a `capi` vcpkg/Conan feature, and the licence-identifier drift fixed; CI now
fails on future drift instead of missing it.
<details markdown="1">
<summary>Full record</summary>

No pkg-config files existed; `ac3adm` and `admbridge` were `add_subdirectory`-only although
`docs/releasing.md` prescribes the three-step recipe for a new component; the vcpkg port and
Conan recipe had no `capi` feature (the portfile pinned `AC3FORGE_BUILD_CAPI=OFF`); and the
licence identifier drifted (`pyproject.toml` said `GPL-3.0-only`, every other manifest and the
README said `GPL-3.0-or-later`). Now: a `.pc` file per installed component
(`cmake/PkgConfig.cmake`); `ac3adm`/`ac3::admbridge` install/export via
`find_package(ac3forge)` shared-only (re-exporting the third-party libbw64/libadm they embed
was out of scope, so only the self-contained `.so` variant ships); `capi` vcpkg feature and
Conan option; `pyproject.toml` corrected to `GPL-3.0-or-later`.
`tools/checks/check_packaging_versions.sh` and the ABI gate (`ci.yml`) both extended so
licence/feature/pkg-config drift and a missing `abi-allowlist` entry (found stale for
`ac3iab`, fixed alongside) fail CI instead of going unnoticed.
</details>

**AP11 (S)** — A consumer-facing diagnostic sink — a null-by-default callback for CRC
mismatches and unknown EMDF payloads, usable from the minimal decoder build.
<details markdown="1">
<summary>Full record</summary>

A callback hook (no iostream) for "CRC failed at frame N" or "unknown EMDF payload skipped".
Tracy is profiling, not diagnostics. `DecoderConfig::diagnostics`
(`ac3/decoder/diagnostics.hpp`) — a plain function pointer plus an opaque context, following
`trace`/`syntax`/`concealment`'s own null-by-default pointer convention rather than
`std::function`, so it costs no allocation and needs no exceptions/RTTI, usable from
`AC3FORGE_MINIMAL_DECODER`. Fires for `DiagnosticEvent::kCrcMismatch` at the moment the check
fails — the only signal a caller gets once `ConcealmentPolicy` turns the same call into a
successful, concealed result — and `kUnknownEmdfPayload` when a block's EMDF container carries
a payload id this decoder does not interpret (anything but OAMD/JOC), which previously left no
trace anywhere at all. Null by default on both decoders.
</details>

**AP12 (S)** — Research instrumentation export — per-frame bap/exponent/SNR-offset/masking
curves as CSV/JSON-lines, reachable from Python (Parquet via pandas, not a second writer).
<details markdown="1">
<summary>Full record</summary>

Both codecs carry a trace since `VX2`; the trace (`ac3::verify::FrameTrace`/
`Eac3AccessUnitTrace`) already carried exponents and bap; SNR-offset and the §7.2.2.5 masking
curve did not exist anywhere and were added to `StreamTrace`/`Eac3StreamTrace`, populated
decode-side only via a new internal entry point
(`ac3::internal::compute_bit_allocation_traced`, `src/core/bitalloc_internal.hpp`) that shares
`compute_bit_allocation`'s own implementation rather than changing its exported signature (an
ABI break the gate would have flagged for every consumer, not just this one).
`ac3/verify/trace_export.hpp`'s `append_trace_csv`/`append_trace_json_lines` write one tidy row
per (frame, substream, block, stream, kind, index, value) — bin-indexed `exponent`/`bap` and
band-indexed `mask` named apart by `kind` rather than forced into one fixed-width record, since
the trace never claimed those two index spaces line up. No C++ Parquet writer: reachable from
Python as `ac3.verify.trace_to_csv`/`trace_to_json_lines`, then
`pandas.read_csv`/`read_json(lines=True)`/`.to_parquet()` — Python's own, more complete Parquet
support, not a second one grown here for one research-only export path.
</details>

### In progress

**AP1 (L)** — API freeze → v1.0.0. The tiering, SemVer policy, and release criteria are
written down; `SOVERSION`/ABI-tagging changes are deliberately deferred to the v1.0.0 cut
itself, sequenced with AP4's ABI gate going required.
<details markdown="1">
<summary>Full record</summary>

(was `F5`). Written down in `docs/library/api-stability.md`: every header under `ac3/` gets a
Public/Internal/Diagnostic/Experimental tier (the `ac3/core/` bit-reader, bit-allocation,
exponent, mantissa and FFT internals are Internal; `ac3iab` is Experimental until `IM1`
finishes; the five `namespace detail` headers are unaffected by tier — nothing in `detail` is
ever covered regardless); a SemVer and deprecation policy (`DEFINE_NO_DEPRECATED` stays until
`v1.0.0`, then drops from all nine libraries' `generate_export_header()` calls); a C config
struct growth policy (major bump or an additive `_v2` sibling, no size-sentinel scheme);
release criteria against the standing Known gaps; and a cadence/governance statement answering
the vcpkg reviewer's "all releases are prereleases" maturity note with a written criteria list
instead of an implicit "not yet." Compile-time version macros landed for real:
`AC3FORGE_C_VERSION_MAJOR`/`MINOR`/`PATCH`/`AC3FORGE_C_VERSION` in `ac3forge_c/ac3forge.h`
(generated from a new `version.h.in`), tested against the runtime `ac3forge_version()` in
`tests/capi/test_capi.cpp`. Still open, both deliberately deferred to the `v1.0.0` cut itself
rather than done now (see the page's own reasoning): flipping `SOVERSION` from the full version
to the major component across all nine `CMakeLists.txt`, and introducing `inline namespace v1`
for ABI tagging — both are sequenced together with `AP4`'s ABI gate going from advisory to
required, so a real break can't slip through the gap between promising compatibility and
actually checking for it. `AP5`/`AP6` completion is now one of the page's own release-gate
criteria rather than a separate, unlinked concern.
</details>

**AP6 (L)** — Python completeness. E-AC-3 encoder, `scan`, and zero-copy numpy all landed;
containers, metering, signing and several wheel targets are still missing.
<details markdown="1">
<summary>Full record</summary>

`ac3.eac3.FrameEncoder`/`AccessUnitEncoder` now wrap the E-AC-3 encoder directly
(pybind11-direct, matching every other class here), with
`ac3.eac3.access_unit_config_for_layout` as the named-layout convenience over
`ac3::plan::channel_plan_for` — see `docs/library/python-api.md`'s "Encoding E-AC-3" section
for what's mirrored and what's a real gap there (mixmdate/infomdat, VBR/ABR, `additional`
programmes) rather than a decision. `ac3.scan()`/`ac3.read_frame_header()` wrap
`ac3::io::scan`/`read_frame_header` directly (`ac3.ScannedStream`/`ScannedProgramme`/
`FrameHeader`, plus `access_unit_timing` and its timing-arithmetic neighbours) — see
`docs/library/python-api.md`'s "Scanning a stream" section. Zero-copy numpy: encode input
(AC-3, E-AC-3 and `AtmosEncoder`) accepts either a 2-D `(n_channels, n_samples)` array or a
sequence of 1-D arrays and is read directly out of whichever is passed when it's already
contiguous float32; decoded `.channels`/`.object_audio` are read-only zero-copy views over the
decoded instance's own memory instead of a fresh `memcpy`'d list every property access;
`FrameDecoder.decode_frame_into`/`Eac3Decoder.decode_access_unit_into` write PCM into
caller-supplied buffers (`ac3.MAX_AC3_CHANNELS`/`ac3.eac3.MAX_RENDER_CHANNELS` size them) —
see `docs/library/python-api.md`'s "Zero-copy numpy and buffer reuse" section. No
`decode_substream_into`: `ac3::Eac3Decoder` itself only exposes a caller-buffer form for the
two calls that assemble a full programme, not the single-substream one. Still missing: no
containers (`AC3FORGE_BUILD_MATROSKA/MP4/MPEGTS` are off in `pyproject.toml`), no metering, no
signing, a context manager that flushes `Eac3Decoder`; `stubtest` in CI for the hand-written
`.pyi`; manylinux aarch64 and macOS x86_64/universal wheels — Raspberry Pi is a documented
platform with no wheel.
</details>

**AP9 (L)** — A first non-Python binding over the C API (Rust) — a `bindgen`-generated `-sys`
crate plus a safe wrapper, with real round-trip tests; found and fixed a real header bug along
the way. Wide layouts, Atmos, and non-Linux CI legs are not done.
<details markdown="1">
<summary>Full record</summary>

The C API had no consumer but its own test and example — Python is pybind11-direct, WASM is
Embind, Android is app-specific JNI over C++ — so the ABI had never crossed a real FFI
boundary. It now has: `rust/ac3forge-sys` (`bindgen`-generated against
`src/capi/include/ac3forge_c/ac3forge.h` at build time, so header drift is caught on every PR
rather than discovered later) plus `rust/ac3forge`, a safe wrapper covering AC-3 and E-AC-3
encode/decode (single substream) with real round-trip tests against synthesized audio. In-tree
at `rust/` — never `add_subdirectory()`'d from the root `CMakeLists.txt`, same shape
`apps/android`'s Gradle build already established for a second build system living alongside
CMake — with its own CI job (`build-rust` in `_build.yml`, Linux-only for this first pass).
`ac3forge-sys` links `forge_c_shared` specifically (not `forge_c_static`): that variant exists
precisely so a non-CMake consumer links exactly one library instead of independently
rediscovering the codec core's transitive static dependencies. Three real header findings, the
actual point of this item: `ac3forge_object_placement_t` had no `_init()` unlike every sibling
config struct, so a zero-initialized one silently encoded a muted object (fixed,
`ac3forge_object_placement_init()` added); four decoded-audio accessors were missing the
pointer-lifetime documentation their AC-3 sibling has (fixed, doc-only); and no status/enum in
the header says whether it may gain new values in a future minor release, which the Rust
wrapper answers by treating `ac3forge_status_t` as an open set (`Error::Other(i32)`) rather
than a closed one that would have to panic or misreport on a code from a newer library — see
`rust/README.md`'s own "Header defects found" section for the full detail, including what a
future .NET, Node N-API or Android AAR binding over the same header would need to answer for
itself. **Not done**: `ac3forge_eac3_access_unit_encoder_t` (wide layouts), the Atmos encoder
and OAMD/JOC object-audio decode accessors, the stream-framing helpers
(`split_frames`/`split_access_units`/`stream_bsid`), and Windows/macOS CI legs — recorded, not
silently missing, in the same README. AP5's still-open C API gap (`scan`, caller-buffer `_into`
decode forms, metering) needs no design change here: `bindgen` picks up new declarations the
moment they land.
</details>

### Considering

**AP8 (M)** — A generated API reference and versioned docs. Unstarted.
<details markdown="1">
<summary>Full record</summary>

Doxygen into mkdocs (the header comments are already the reference) and versioned docs
(`mike`: `latest` from `main`, `dev` from `develop` — today a `develop` docs change is
invisible until a release). Note in `header-map.md` that `ac3/audio` is not installed.
</details>

**AP10 (L)** — An out-of-tree GStreamer element or FFmpeg external-encoder wrapper, the way
>5.1 and JOC encode would reach the transcode ecosystem. Needs AP5 (now done); unstarted.
<details markdown="1">
<summary>Full record</summary>

Over the C API, as the way >5.1 and JOC encode reach the transcode ecosystem: FFmpeg's E-AC-3
encoder has been 5.1-max since [trac #3595](https://trac.ffmpeg.org/ticket/3595) (2014) and
[HandBrake #1085](https://github.com/HandBrake/HandBrake/issues/1085) has been open since 2017.
Out of tree, over the C API only, GPL-3 framed (`--enable-gpl --enable-version3`); FFmpeg stays
an oracle for the codec itself.
</details>

## UX. Applications

### Shipped

**UX1 (M)** — A GUI player/monitor for an existing stream, with decode-to-WAV and object
export, and QC/Inspect shortcuts.
<details markdown="1">
<summary>Full record</summary>

The run-chip shortcuts into QC and Inspect that two docs pages each end by saying do not exist
yet. The `MonitorSink` plumbing is already owned by the object-decode controller; only the
file-driven transport and UI were missing.
</details>

**UX2 (M)** — Desktop integration — drag-and-drop, file associations, and the Linux
`.desktop`/AppStream entries that were missing entirely.
<details markdown="1">
<summary>Full record</summary>

Drag-and-drop, `ac3gui <file>`, file associations (the installer's registry keys,
`CFBundleDocumentTypes`, a `.desktop` entry plus AppStream metainfo and MIME XML for the
`.deb`/`.rpm` — `ac3gui` was absent from Linux application menus).
</details>

**UX3 (M)** — Localisation and accessibility foundations — six languages (98/758 messages each,
rest left in English), full RTL support, and `Accessible.role`/`name`/`description` on every
custom control.
<details markdown="1">
<summary>Full record</summary>

`qt_add_translations()` wires `apps/gui/translations/*.ts` through `lupdate`/`lrelease` into a
`:/i18n` resource, `LanguageManager` (`language_manager.{hpp,cpp}`) loads the active language
at startup and on a live Preferences switch, and RTL layout mirroring plus bundled Noto Sans
Arabic/Hebrew faces cover Arabic, Hebrew and Yiddish. The canonical language set — French,
German, Spanish, Arabic, Hebrew, Yiddish — matches the one the sibling CountdownSolver project
already ships, rather than inventing a second one; coverage today is 98 of 758 extracted
messages per language (window chrome, tab names, the Guided wizard's step titles, all of
Preferences), the rest left in English exactly like any real partial translation — see
`docs/gui/localisation.md` for the tracked remainder, and completing it is the natural
follow-on, not a hidden gap. A pseudo-locale QA fixture
(`tools/generators/gen_pseudo_locale.py`, `ac3gui_xx.ts`) mechanically decorates every
extracted string and is loaded only under `AC3GUI_LOCALE=xx` in `tst_localisation_pipeline.qml`,
proving the whole pipeline end to end independent of the six real languages' completeness and
catching any string that bypasses `qsTr()` entirely. CI's "Check translations are up to date"
step reruns `lupdate` and fails on drift — the literal ask behind the item's original wording —
deliberately short of CountdownSolver's own stricter "zero unfinished" gate, since that would
fail today on content nobody has touched yet.

Every custom control and every ad-hoc control in `Main.qml` now carries `Accessible.role`/
`name`/`description`, built from the same live properties the visual state already reads (never
a static duplicate of a label) — the QC preset regression's own lesson, now enforced by
`tst_accessibility.qml` and targeted additions to `tst_main_shell.qml`/`tst_qc_panel.qml`. Two
genuine Qt API surprises found doing this, both worth knowing before touching this again:
`Accessible` has no `disabled` property (Qt's own accessibility bridge reads the item's real
`enabled` state instead), and a `Dialog`/`Popup` root is not itself an `Item` — `Accessible.*`
has to attach to its `contentItem`, not the `Dialog` itself, or it throws a runtime warning on
every window that ever instantiates the dialog, not just when it opens.
</details>

**UX4 (M)** — A real live object-position source (OSC) for `live mode=atmos` and the GUI live
room.
<details markdown="1">
<summary>Full record</summary>

As the roadmap asked for first: `ac3::oba::parse_osc_packet`/`apply()`
(`src/forge/src/oba/scene_osc.cpp`) is a third, pure reader of a scene update beside the JSON
and keyframe-text forms `scene.hpp` already had, and `ac3::audio::LivePositionSource`
(`src/audio`) is the UDP listener and per-object mailbox that drains into an
`ac3::oba::SceneCursor` once per encode frame — the live seam `SceneCursor` was built for
(IM7) and had sat unused until now. `ac3cli live mode=atmos positions=osc:<port>` and the
GUI's live room (a "Drive objects from OSC" toggle on the Live session card, room markers
greyed and undraggable while a network update owns them) both land on it; the CLI's synthetic
orbit and the GUI's manual placement are both unchanged when `positions=` is off. The address
space is `/object/<n>/xyz|gain|lfe|release` (0-based, literal match, no OSC glob patterns) —
see `docs/library/spatial-and-atmos.md`'s "The live half" section. Default bind is loopback;
`positions=osc:any:<port>` opts into every interface. Fuzzed (`fuzz/fuzz_osc_parse.cpp`) and
covered by the ThreadSanitizer leg (`tests/audio/test_live_positions.cpp`, `[concurrency]`).
MIDI and a desktop game controller remain: the `positions=<scheme>:...` grammar is
scheme-prefixed for exactly this (`positions=midi:...`, `positions=gamepad:...` land as new
schemes, not a new token), but neither is implemented — the Shield app stays the only
controller-driven path anywhere in the project until one is.
</details>

**UX5 (L)** — WASM as a reusable streaming decoder (`ac3forge-wasm-decoder`) — push-frame
decode in a Worker over a `SharedArrayBuffer` ring buffer, an hls.js/MSE bridge. npm publishing
gated on a not-yet-provisioned environment.
<details markdown="1">
<summary>Full record</summary>

Shipped as `ac3forge-wasm-decoder` (`js/`): a push-frame `PushDecoder` over
`decode_access_unit_into`'s caller-buffer form (buffers allocated once, reused every call — the
C++ hot path allocates nothing), an `Ac3ForgeDecoderNode` (decode in a Worker, a real
`AudioWorkletNode` draining a `SharedArrayBuffer` ring buffer, so only the ring-buffer read
runs on the audio thread), multichannel output or DC1's `ac3::OutputStage` downmix applied over
a reused copy (never a hand-rolled fold), and an hls.js/MSE bridge (`js/src/fmp4.ts` + a
`MediaSource`/`addSourceBuffer` shim — a passive `BUFFER_APPENDING` listener alone doesn't
work, since hls.js drops the audio track entirely the moment
`addSourceBuffer('audio/mp4;codecs="ec-3"')` throws). `apps/wasm/`'s demo is now a consumer of
the package, not a parallel implementation — its old bespoke whole-file Embind `Decoder` class
is gone; `js/src/decode-file.ts`'s `decodeFile()` is built on `PushDecoder` instead. Published
versioning tracks the same release tag the PyPI package uses; npm publishing itself is gated on
a not-yet-provisioned `npm` GitHub environment (see `docs/releasing.md`). Chrome still cannot
decode EC-3 ([video.js http-streaming #1297](https://github.com/videojs/http-streaming/issues/1297)
is open) — this is a real, if not yet live-stream-soak-tested, answer to that. See
`docs/platforms/wasm.md`'s verification notes for exactly what's been checked (a real local
Emscripten 6.0.6 build, both Playwright specs, `js/`'s own unit tests against a real
ffmpeg-remuxed fMP4 fixture) versus what hasn't (a live hls.js instance against a real HLS
server).
</details>

**UX8 (L)** — A Windows Spatial Sound object sink — confirmed against a real spatial endpoint
(4-object stream, 250 access units, zero underruns); not a measurement of Dolby's own JOC
reconstruction.
<details markdown="1">
<summary>Full record</summary>

`ac3::audio::SpatialObjectSink` (`ISpatialAudioObjectRenderStream`): decoded objects go out as
dynamic objects at their real OAMD positions, the bed's LFE as a static one (never a JOC
output, §6.3.2.2 — the rest of a genuine bed programme's channels are not yet mapped, see the
sink's own header). Confirmed against a real spatial endpoint: `GetMaxDynamicObjectCount` read
0 everywhere on this machine before Windows Sonic for Headphones was enabled on the default
Realtek output, `start()` refused with `kNoSpatialFormat` naming the fix, and after enabling it
a real 4-object stream rendered 250 access units with zero underruns. That confirms the
plumbing end to end against a real OS renderer, not a self-consistency check — but per DC10's
own note, it still is not a measurement of how Dolby's *own* JOC reconstruction reads this
project's matrix, since Windows Spatial Sound renders the PCM and positions it is handed rather
than re-deriving objects from the matrix itself.
</details>

**UX9 (M)** — `play` that follows the sink — EDID-driven format choice plus automatic
transcode-to-passthrough; not yet verified against real EDID/ELD hardware.
<details markdown="1">
<summary>Full record</summary>

Parse EDID short audio descriptors to choose AC-3, E-AC-3 or PCM, and a one-command
transcode-to-passthrough pipeline (DC9's transcode into `PassthroughSink`) for the "no 5.1 PCM
over optical" case. Shipped as `ac3::audio::sink_capabilities` (real on ALSA, reading the
HD-audio driver's own `/proc/asound/.../eld#*` text; every other backend reports `kNoBackend`
honestly rather than guessing, and `play` falls back to `enumerate_render_devices()`'s live
probe there, same as before). `play` tries EDID first, an E-AC-3 source on an AC-3-only sink
gets transcoded to AC-3 automatically (reusing `transcode` through a temp file, so
dialnorm/compr/mix metadata still carry across), and a sink that bitstreams neither format
falls back to decoded PCM (`monitor`'s own §7.8-aware path). `follow=off` restores the plain
refusal `play` always gave. Not verified against real EDID/ELD hardware this round (no Linux
box in the loop) - see `docs/platforms/linux.md`.
</details>

### In progress

**UX6 (XL)** — In-browser encoding. The encode module and a drop-a-WAV demo page shipped
(385×/120×/82× real-time for AC-3/E-AC-3/4-object Atmos, no threads needed); mic capture and an
object-authoring UI are still open.
<details markdown="1">
<summary>Full record</summary>

`docs/platforms/wasm.md` used to call it "a separate, much larger undertaking"; measured
instead of assumed (WSL2/Emscripten 6.0.6, the same toolchain `build-wasm` uses), that turned
out not to be true. A combined AC-3 + E-AC-3 + Atmos/JOC + QC Embind module
(`apps/wasm/encoder_bindings.cpp`) is 390 KB raw / 153 KB gzip, comparable to the decode-only
module's own 372 KB/133 KB — not worth a further module split. Timed under Node/V8,
single-threaded, no SIMD: AC-3 2.0 at 385x real-time, E-AC-3 3/2+LFE at 120x, 4-object Atmos/JOC
at 82x — enough headroom that neither product needs `pthreads`/`SharedArrayBuffer`, and a future
microphone-capture encoder is a plumbing problem, not a CPU one. QC needed no new DSP:
`ac3::meta::LoudnessMeter`/`evaluate_qc_gate` are `ac3cli qc`'s own functions, bound directly.
**Landed so far**: the encode module (full binding surface, including Atmos/JOC even though no
page uses it yet) and the drop-a-WAV page (`apps/wasm/encode/`) — format/sample-rate/bitrate
controls, the five-preset QC verdict table, and a round-trip preview through the existing decode
module — plus `apps/wasm/tests/encode.spec.js` extending VX18(a)'s Playwright harness (asserts a
known tone's encoded byte count, QC verdict and true peak, and a successful round-trip decode).
See [docs/platforms/wasm.md#encode-module](https://github.com/iainchesworthlabs/ac3forge/blob/main/docs/platforms/wasm.md#encode-module)
for the full numbers. **Still open**: capture-a-mic (real-time capture/buffering plumbing —
`getUserMedia`/`AudioWorklet` — the CPU budget is proven, this is UX work); an object-authoring
UI on top of the already-bound `WasmAtmosBedEncoder`; wider channel layouts (7.1, height
channels) than the mono/stereo/5.1 the drop-a-WAV page reorders today. Dialnorm is left at
§5.4.2.8's unmeasured default (31) rather than derived from the QC pass's own measured
loudness — the QC verdict itself measures the source PCM directly so this does not affect it,
but a real decoder's dialnorm normalisation would under-attenuate loud content this page
produces. VX18(a)'s browser-test coverage now spans both demos, but neither demo's visual/
interactive surface (drag-and-drop, the QC table's rendering, the decode side's visualizations)
is CI-tested — see `wasm.md`'s own "Not yet verified" note.
</details>

### Considering

**UX7 (M)** — macOS loopback capture through Core Audio process/system taps. Blocked on a real
Mac and on DR6's code signing; only documentation and an OS-version gate landed without one.
<details markdown="1">
<summary>Full record</summary>

Through `AudioHardwareCreateProcessTap`/`CATapDescription`, macOS 14.2+. Capture is still
input-only there and `start()` still refuses `kLoopback`: the tap itself needs an Objective-C
class, a real-time TCC consent prompt under `SystemAudioCaptureRequests`, and — since that
prompt is keyed to code-signing identity and does not fire unsigned — DR6 resolved first or
nothing to grant it to. Needs a real Mac (DR9) either way; not attempted without one. What
landed without one: the loopback gap's documentation corrected (it previously cited macOS
14.4, not the API's real 14.2) and expanded (`docs/platforms/macos.md`), and a pure,
CI-verified OS-version gate a future implementation should refuse on before ever touching
`CATapDescription` (`ac3::coreaudio::system_audio_tap_api_available()`).
</details>

**UX10 (rides IM5)** — The TrueHD front ends on the IM5 branch — needs its own PR split, a
QML test leg, and docs once IM5 lands.
<details markdown="1">
<summary>Full record</summary>

The lossless-lab dialog, its QML test, the CLI rows get their own PR split, a QML test leg and
`docs/gui`/`docs/cli` pages.
</details>

## DR. Distribution, release engineering and hardware

Where `F4` actually stands on 2026-08-23, per tool: PyPI is live (`ac3forge` 0.9.0b1, published
by the tag run; the `pypi` environment exists). The Homebrew tap `iainchesworthlabs/homebrew-ac3forge`
is live with a Formula and a Cask. The vcpkg port is [microsoft/vcpkg#53470](https://github.com/microsoft/vcpkg/pull/53470),
a draft with changes requested: every technical point was fixed, and the reviewer declined an
exception to the six-month maturity rule — the repository was created 2026-08-09, so not before
about 2027-02. The winget submission is [microsoft/winget-pkgs#419594](https://github.com/microsoft/winget-pkgs/pull/419594),
untouched since 2026-08-18 with `Needs-CLA` and a Defender validation error. ConanCenter was never
submitted. All four staged manifests and the tap now point at v0.9.0-beta.1 (DR1).

### Shipped

**DR1 (S)** — Bump the four package manifests and the tap to the current release — cross-checked
against the release's own published SHA512SUMS, not fabricated.
<details markdown="1">
<summary>Full record</summary>

`vcpkg.json`'s `version-semver` and `portfile.cmake`'s SHA512, the Homebrew formula's and
cask's `url`/`sha256`/`version`, `conandata.yml`'s `sources` entry, and a new
`0.9.0-beta.1/` winget manifest directory all point at the real release tarball/binary hashes
(`sha256sum`/`sha512sum` against the downloaded assets, cross-checked against the release's own
published `SHA512SUMS`, not fabricated); `tools/checks/check_packaging_versions.sh` passes. The
live tap (`iainchesworthlabs/homebrew-ac3forge`) is pushed to match. Still the second cycle in
a row these went stale — DR2 is the fix for that.
</details>

**DR2 (M)** — Automate the post-release manifest bump — a dry-runnable workflow that downloads
real release assets, cross-checks digests, and opens the bump PR; the upstream fork PRs stay
manual since they write to repos this project doesn't own.
<details markdown="1">
<summary>Full record</summary>

`release.yml`'s `github-release` job now renders the release body from CHANGELOG.md's matching
`## [x.y.z]` section (`tools/release/render_release_notes.py`) instead of `--generate-notes`,
and `SHA512SUMS` is generated unconditionally (previously only alongside GPG signing) so the
step below always has something to cross-check against. A new `manifest-bump` job calls
`.github/workflows/manifest-bump.yml` after every real (non-dry-run) release: it downloads the
release's source tarball and, where they exist, `ac3forge-*-Darwin.dmg`/`ac3forge-*-win64.zip`,
computes the digests `tools/release/bump_manifests.py` needs, cross-checks the two platform
assets against the release's own `SHA512SUMS` (never fabricated — the source tarball has no
entry to check against, it isn't a CI-built package), opens a PR bumping all four staged
manifests together, and pushes the Homebrew Formula/Cask to the live
`iainchesworthlabs/homebrew-ac3forge` tap when `HOMEBREW_TAP_TOKEN` is provisioned. That
workflow is also directly `workflow_dispatch`-able with `dry_run: true` (the default), so the
whole download/digest/cross-check pipeline is exercisable against any already-shipped tag
without cutting a release — verified locally against the real v0.9.0-beta.1 release assets
before this shipped. `tools/checks/check_packaging_versions.sh` now carries the latest-tag
advisory too (a `::warning::`, not a gate — merging the bump PR is still a separate, reviewed
step), and `ci.yml`'s `packaging-consistency` job fetches full history so it can see tags at
all. Still manual, because it means writing to a repository this project does not own: the
vcpkg/Conan/winget upstream fork PRs, and Homebrew's local macOS-only `brew audit`/`brew
install`/`brew test` validation. See `docs/releasing.md`'s Post-release section.
</details>

**DR5 (S)** — Fix the docs that shipped work made false — nine separate stale claims corrected.
<details markdown="1">
<summary>Full record</summary>

`docs/releasing.md` and `wheels.yml`'s comments now say PyPI publishing is live, not off until
provisioned; `releasing.md` and `README.md` now call the Homebrew tap published/live rather
than pending/unpublished; `README.md` no longer says macOS builds the CLI only (the GUI leg has
run since 0.8.0-beta.2); `docs/index.md` now says `ac3::admbridge` wires the ADM object/bed
graph onto `ac3::oba::AtmosEncoder`, driven end to end by `ac3cli atmos-adm`, rather than "not
wired up yet"; `docs/cli/metadata-options.md` now says the E-AC-3 decode-time DRC tokens apply
(0.6.0 fixed that) rather than "silently inert"; `docs/gui/format-and-channels.md` now lists
MPEG-TS beside fragmented MP4/CMAF as carrying over to a live session (0.9.0 added
`mpegts::Writer`) rather than falling back to the plain elementary stream; the two pages that
pointed at `run_live` in `apps/cli/main.cpp` (`docs/history.md`, `docs/platforms/windows.md`)
now say `commands/live_audio.cpp`; `docs/library/muxing-and-sinks.md`'s `A1` justification now
says the jellyfin-ffmpeg issue (upstream FFmpeg trac #9996) has since been fixed rather than
presenting it as an open bug. The DR9 contradiction is fixed too: `docs/verification.md`,
`docs/platforms/linux.md`, `docs/platforms/windows.md` and the 0.9.0 CHANGELOG Known gaps
section all now reflect ALSA/Raspberry Pi's real HDMI-to-receiver confirmation instead of
contradicting it.
</details>

**DR7 (S)** — The Windows installer — a real `.exe` now comes out of `cpack`, verified in CI;
still unsigned (DR6).
<details markdown="1">
<summary>Full record</summary>

`.github/workflows/_build.yml`'s `windows-msvc` leg now installs `makensis` via Chocolatey —
the same fresh-install-every-run pattern already used for ffmpeg/Ninja, deliberate per
`docs/ci-self-hosted-runners.md` ("nothing is pre-baked onto the self-hosted image without data
justifying it") — and a new step asserts a real `.exe` came out of `cpack`, failing the leg
outright if it didn't. WiX was considered and rejected: `Packaging.cmake` already carries
NSIS-specific work (the `.ac3`/`.ec3` file-association commands UX2 added) a WiX rewrite would
have to redo for no real benefit, on a leg this fix scopes as an afternoon, not a rewrite.
`cmake/Packaging.cmake`'s `find_program()` gate now warns instead of degrading silently when
`makensis` is absent, for a local dev build that doesn't have it. `docs/releasing.md`'s winget
instructions now default the *next* release bump to `InstallerType: nullsoft`; the
already-published `0.9.0-beta.1` manifest stays `zip`/`portable` since that release genuinely
has no `.exe` to point at — never rewrite a published version to claim an installer it never
shipped. `tools/checks/check_packaging_versions.sh` now fails if a future manifest bump
declares `nullsoft` without matching `InstallerUrl`/dropping `NestedInstallerType`, or vice
versa. The new installer is still unsigned (DR6) and may still trip SmartScreen or the Defender
false positive already blocking DR4's winget resubmission — DR7 fixes the build, not the trust
story; DR4 stays blocked on DR6.
</details>

**DR8 (M)** — Reach: AppImage, Windows ARM64 (CLI-only), and macOS universal binaries — all
three landed and promoted out of experimental.
<details markdown="1">
<summary>Full record</summary>

Restated per sub-item now that all three have landed:

- **AppImage: done.** `ac3gui` ships as a self-contained AppImage (`linuxdeploy` +
  `linuxdeploy-plugin-qt`, built in an `ubuntu:22.04` container deliberately older than every
  other Linux CI leg's `ubuntu:26.04`, so its glibc floor stays lower) alongside the existing
  `.deb`/`.rpm`, so a distro whose own Qt 6/`qml6-module-*` split is too old or cut differently
  no longer blocks running `ac3gui` at all. `linux-appimage` in `.github/workflows/_build.yml`
  builds it on every push and then launches it inside a second container that never had Qt
  installed, proving the claim rather than just the build. Flatpak was decided against outright
  rather than deferred — `ac3gui`'s own reason to exist is `ac3::audio`'s exclusive-mode ALSA/
  PipeWire passthrough, which needs exactly the raw device access a Flatpak sandbox exists to
  take away — see `docs/platforms/linux.md`'s own AppImage section for the full reasoning.
- **Windows ARM64: done, CLI-only.** `windows-msvc-arm64` builds and tests `ac3cli` on real
  ARM64 hardware (no resolvable Qt6 ARM64 Windows kit for the pinned version yet, see
  `docs/platforms/windows.md`'s ARM64 section) and packages a `win-arm64` release archive;
  `experimental: true` until it proves itself green over real runs, the same promotion path
  `macos-llvm` went through.
- **macOS universal binaries: landed.** This item originally called them "a separate decision,
  not a given," skeptical because the Cask was arm64-only and Intel demand looked doubtful —
  that skepticism was written without checking current GitHub runner availability, and it turned
  out to rest on a stale assumption: `macos-15-intel` exists, is free for public repos, and is
  real native Intel hardware, not Rosetta emulation (confirmed against
  `docs.github.com/en/actions/reference/runners/github-hosted-runners`) — so a universal binary
  needed no cross-compilation and the actual blocker was gone.
  `cmake/vcpkg/triplets/x64-macos-llvm.cmake` joins the existing `arm64-macos-llvm.cmake`;
  `macos.llvm.toolchain.cmake` and `FindQt6.cmake` needed zero changes, since both already
  resolved their target architecture generically. A new `macos-llvm-x64` CI leg builds the Intel
  half for real; a `package-macos-universal` job `lipo`-merges it with `macos-llvm`'s arm64
  build (proven via `lipo -info` on the merged `ac3cli`/`ac3gui` binaries and a bundled Qt
  framework binary in the CI log, not assumed from a green checkmark) and packages the result as
  the release's one canonical macOS `.dmg` — neither single-arch leg's own `cpack` output ships
  as the release artifact any more, a deliberate change from before. The Homebrew Cask dropped
  its `depends_on arch: :arm64`. `macos-llvm-x64` went three consecutive clean real runs (two
  `release.yml` dry runs plus its own required PR CI, each including a real gold-reference pass)
  before its `experimental: true` came off, the same promotion bar `macos-llvm` itself was held
  to; see [docs/platforms/macos.md](platforms/macos.md#universal-binaries-dr8) and
  [docs/releasing.md](releasing.md#what-gets-published). Unplanned bonus: `macos-llvm-x64`'s
  real gold-reference numbers (67.80/67.82/67.76 dB) turned out to be new evidence for VX11 —
  see that entry.
</details>

### In progress

**DR9** — Hardware confirmation, per backend. Linux/ALSA confirmed on real hardware; Windows/
WASAPI exclusive and PipeWire remain unconfirmed; CoreAudio is blocked on real Mac hardware.
<details markdown="1">
<summary>Full record</summary>

(was `E3`), restated per backend because the one-line version hid a contradiction:

- **Linux/ALSA: confirmed.** `docs/platforms/raspberry-pi.md` ("Live HDMI passthrough to a
  real receiver", 2026-08-20) records a Pi 4B driving an Atmos-capable AVR: every stream shape
  locked and was identified correctly, including signed Atmos with four height channels, at
  zero underruns. `docs/verification.md`, `docs/platforms/linux.md`, the 0.9.0 Known gaps and
  `docs/platforms/windows.md` all carried stale text contradicting this — fixed with DR5.
- **Windows/WASAPI exclusive: unconfirmed** — only a Realtek analogue endpoint has been tried.
  The receiver exists now: cable the workstation's HDMI (or a USB S/PDIF for the AC-3 half)
  and run the Pi page's stream matrix (S, hardware).
- **PipeWire: unconfirmed** — it has only ever seen "no session" on WSL2. Raspberry Pi OS ships
  PipeWire: build with ALSA off, write down the WirePlumber `iec958` codec rule a user needs
  (the most the library can do about `iec958Codecs`), run the same matrix (M). Plus a PipeWire
  CI leg mirroring `alsa_fallback` — no workflow mentions PipeWire today (S).
- **CoreAudio: blocked** — CI-only, no Mac has ever run it. Also outstanding: a Pi 5 and a
  second Android TV device.
</details>

### Considering

**DR3 (S)** — A vcpkg git registry, so consumers get `vcpkg install ac3forge` without an
overlay-port source clone — keeps the existing draft PR alive as a re-request around 2027-02.
<details markdown="1">
<summary>Full record</summary>

`ports/ac3forge` + `versions/` now, so consumers get `vcpkg install ac3forge` through
`vcpkg-configuration.json` instead of overlay-ports from a source clone; keep #53470 as a
draft to re-request around 2027-02. The reviewer also cited prerelease-only history, so AP1's
timing interacts.
</details>

**DR4 (M)** — winget and ConanCenter submissions. winget needs the CLA signed and the Defender
hit resolved (likely DR6); ConanCenter expects pushback on the recipe's `cmake_find_mode`.
<details markdown="1">
<summary>Full record</summary>

winget: sign the CLA, resolve the Defender hit (unsigned binaries are the likely cause — DR6),
resubmit at the current release. ConanCenter: bump, run the three `conan create` validations
`docs/releasing.md` lists, open the `conan-center-index` PR; expect pushback on the recipe's
`cmake_find_mode = "none"`.
</details>

**DR6 (M, needs accounts)** — Code signing — Developer ID/notarisation for macOS, Authenticode
for Windows. Blocked on certificates, not code; a Known gap in every release since 0.8.0-beta.2.
<details markdown="1">
<summary>Full record</summary>

Developer ID signing and notarisation of `ac3gui.app` and the `.dmg` (a Known gap in every
release since 0.8.0-beta.2; Gatekeeper blocks it), Authenticode for the Windows binaries and
installer. GPG and Sigstore satisfy neither OS. Blocked on the certificates, not on code.
</details>

## Deliberately not on the list

- **Forging Dolby's authenticity tag** — see `docs/concepts/object-signing.md`. The signer ships;
  the key is the operator's.
- **AC-3 VBR** — structurally impossible; the frame size indexes a fixed table.
- **Renderer and room-correction territory** — already covered by
  [Cavern](https://github.com/VoidXH/Cavern). A headphone/binaural preview for the WASM demo and
  Inspect was considered here: with an HRTF set it is a renderer in miniature, so it stays off
  unless that boundary is redrawn on purpose. UX8 (the OS renders) and an example over AOM's Open
  Audio Renderer (an external renderer) are on the right side of the line.
- **A DAMF reader** (was `B2`) — no public specification exists (the Library of Congress format
  registry, fdd000646, says so; PR #217 recorded the finding). IM1 is the replacement; the ADM
  BWF route is served by `B1`. Reopen only if Dolby publishes the format.
- **TrueHD interoperability by black-box analysis of Dolby streams** — not until the clean-room
  rule explicitly allows it (IM6).
- **An external oracle for `fscod2` audio** — FFmpeg and Dolby's own Reference Player both
  refuse it; self-consistency plus the independent Python parser is the ceiling.
- **Perfect separation of co-directional objects** — a property of parametric object coding,
  not of this implementation.
- **Enabling PipeWire `iec958Codecs` on the user's behalf** — session-manager policy; DR9
  documents the rule instead.
- **HOA, Matrix and Binaural ADM pack types** — the clear `kUnsupportedType` refusal stays until
  a design exists (DC7 shipped object extent and channel lock, not these).
- **APT/DNF repositories and Docker images** — not planned. `docs/releasing.md` names where the
  workflows could be copied from if one were ever wanted; the previous roadmap's "ruled out"
  overstated it.

## Retired IDs

Before 2026-08-23 this file used single-letter IDs (`A1`–`G4`). They're retired now — every item
either merged or carried forward under a new ID above — but a reference from an old PR or
discussion still resolves in the ledger below.

<details markdown="1">
<summary>Full ledger (2026-08-15 roadmap → current)</summary>

All merged to `develop` by v0.9.0-beta.1 unless noted; `CHANGELOG.md` has the detail.

| ID | Item | |
|---|---|---|
| A1 | MP4/ISOBMFF muxer with a correct `dec3` box | merged |
| A2 | fMP4/CMAF segmenting, HLS/DASH helpers | merged (IO4, IO5 extend it) |
| A3 | MPEG-TS muxing with AC-3/E-AC-3 descriptors | merged (IO6 extends it) |
| A4 | stdin/stdout streaming in `ac3cli` | merged |
| A5 | Live sessions mux straight to Matroska | merged |
| B1 | ADM BWF reader feeding the JOC encoder (three phases) | merged |
| B2 | DAMF reader | not on the list; IM1 replaces it |
| B3 | IAMF / Eclipsa interop | carried → IM3 |
| C1 | Full R128 metering | merged |
| C2 | `ac3cli qc` | merged (IO10, IO11 extend it) |
| C3 | QC in the GUI | merged |
| C4 | `dialnorm=auto` per programme/source | merged |
| D1 | Resume TrueHD/MLP | carried → IM5, IM6 |
| D2 | Decoder dither substitution | merged (EQ4 is the encoder half) |
| D3 | Delta bit allocation alongside coupling | merged for AC-3 (EQ5 is the E-AC-3 half) |
| D4 | AC-4 bitstream parser/inspector | carried → IM4 |
| E1 | macOS CoreAudio backend | merged |
| E2 | PipeWire backend | merged |
| E3 | Confirm exclusive-mode passthrough on real hardware | carried → DR9 (Linux/ALSA done) |
| E4 | Linux aarch64 CI leg | merged |
| F1 | C API over the encode/decode core | merged (AP5 extends it) |
| F2 | Python bindings on PyPI | merged and published (AP6 extends it) |
| F3 | WASM build plus browser demo | merged (UX5 extends it) |
| F4 | Package-manager presence | carried → DR1–DR5 (PyPI and the tap are live) |
| F5 | API freeze → v1.0.0 | carried → AP1 |
| G1 | Perceptual-quality leg | merged; column populated in CI as of VX6 |
| G2 | Backfill thin test coverage | merged |
| G3 | Differential decoder fuzzing against FFmpeg | merged |
| G4 | Encoder input-space fuzzing | merged, both codecs (AC-3 under G4, E-AC-3 under VX1) |

</details>

---

Rebuilt 2026-08-23 at v0.9.0-beta.1 from a repository survey, the `quality-history` series, the
state of the open external submissions, and the surrounding ecosystem. First drafted 2026-08-15
at v0.5.0-beta.1.
