# AC-3: `ac3::FrameEncoder`

`ac3/encoder/encoder.hpp`. One call, one syncframe.

```cpp
// Heap-allocated: FrameEncoder carries several KB of MDCT scratch/history
// state (PREfast's C6262).
auto encoder = std::make_unique<ac3::FrameEncoder>(ac3::EncoderConfig{
    .bitrate_kbps = 448,
    .acmod = ac3::Acmod::k3_2,  // L, C, R, SL, SR
    .lfe = true,
});

// Table 5.8 order, LFE last, exactly kSamplesPerFrame (1536) samples each.
std::vector<std::vector<float>> pcm(6, std::vector<float>(ac3::kSamplesPerFrame));
// encode_frame takes a span of spans, so the views must outlive the call.
// Build them once and refill the buffers underneath each frame.
const std::vector<std::span<const float>> views{pcm.begin(), pcm.end()};

std::vector<std::byte> stream;
for (int frame = 0; frame < 31; ++frame) {  // 48000 / 1536, near enough
    fill_with_audio(pcm, frame, 48000.0);

    const auto encoded = encoder->encode_frame(views);
    if (!encoded) {
        fmt::printf("encode failed: %d\n", std::to_underlying(encoded.error()));
        return 1;
    }
    write(stream, *encoded);  // one complete syncframe
}
```

Full program: [`examples/encode_ac3.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/encode_ac3.cpp).

## `EncoderConfig`

| Field | Default | Notes |
|---|---|---|
| `sample_rate` | `k48000` | Also `k44100`, `k32000`. |
| `bitrate_kbps` | 192 | Must be one of the 19 Table 5.18 rates; `ac3::is_valid_bitrate` checks. |
| `dialnorm` | 31 | 1–31 (§5.4.2.8). 31 means "no attenuation", which is a claim about your content. |
| `chbwcod` | -1 | Coded bandwidth, 0–60. -1 derives it from the bit rate and the content — see below. |
| `acmod` | `k2_0` | Table 5.8, including `kDualMono` (1+1) — see below. |
| `dialnorm2` | none | `std::optional<int>`. Ch2's own dialnorm (§5.4.2.16); required when `acmod` is `kDualMono`, meaningless otherwise. |
| `lfe` | `false` | Adds one channel, coded last. |
| `coupling` | `false` | §7.4. Needs ≥ 2 full-bandwidth channels. |
| `cplbegf`, `cplendf` | -1, -1 | Sub-band indices; -1 lets the encoder choose from the per-channel rate. |
| `fgaincod` | -1 | §7.2.2.4 fast gain, Table 7.11. -1 takes the encoder's rate-dependent choice; 0–7 pins it. |
| `fast_mdct` | `true` | The §7.9.4 fast N/4-FFT forward MDCT instead of the direct §8.2.3.2 evaluation (~25× on the long-transform kernel, identical streams to within ~3e-12 coefficient error). `false` forces the direct reference form, kept as the validation oracle — the CLI spells that `fast-mdct=off`. |
| `dither` | `true` | §7.3.4 `dithflag`, decided per channel per block from the content — see below. `false` pins it at 0 in every block, which is what a bit-for-bit comparison against a second decoder needs, since dither values are decoder-defined; the CLI spells that `dither=off`. |
| `drc` | none | `std::optional<meta::Profile>`. Absent leaves `dynrnge` clear in every block. |
| `heavy` | none | `std::optional<meta::HeavyConfig>`. Independent of `drc`. |
| `drc2` | none | `std::optional<meta::Profile>`. Ch2's own DRC, meaningful only under `kDualMono` — no fallback to `drc` when unset (see below). |
| `heavy2` | none | `std::optional<meta::HeavyConfig>`. Ch2's own heavy compression, same rule as `drc2`. |
| `cmixlev`, `surmixlev` | −4.5 dB, −6 dB | Tables 5.9/5.10. Always define the §7.8 downmix, whatever `acmod` is. |
| `search` | `kNone` | Per-frame search over §7.2.2's transmitted bit allocation parameters, judged on the error the decoder will reconstruct — see below. `kDistortion` or `kPerceptual` turn it on; the CLI spells it `search=distortion` / `search=perceptual`. |

Coupling is what makes 5.1 viable below 448 kbit/s: above the coupling frequency the
full-bandwidth channels stop carrying their own coefficients and share one coupling channel
plus per-band coordinates.

### Coded bandwidth

`chbwcod` decides where the coded spectrum stops. At -1 — the default, and the same default
`ac3::eac3::FrameConfig` now takes — the encoder answers from two things.

The **bit rate sets a ceiling**, on the curve AC-3 has used since 0.7.0: roughly two thirds of
the per-channel kbit/s, clamped to 24–60. Below about 90 kbit/s per channel the bits the top of
the band costs are bits the rest of the spectrum needed, and no amount of content up there
changes that. Measured on real programme material, AC-3 5.1 at 192 kbit/s scores MOS 3.145 at
`chbwcod` 24 (13.6 kHz) falling to 2.411 at 59 (23.4 kHz).

The **content decides under it**, per frame, from that frame's own transform: the highest band
whose §7.2.2.3 banded PSD stands above Table 7.15's absolute hearing threshold. That is
deliberately not an energy test. Every natural signal carries a vanishing fraction of its energy
at the top of the band — a solo piano recording measures 3.5e-8 above 14.7 kHz, half a decade
*less* than the synthetic `reference_51.wav` fixture's 7e-5 — so a rule that narrows while the
discarded energy is small narrows until it is plainly audible, and reports a waveform-SNR win
the whole way down. The hearing threshold asks the different question: would the allocator have
put a bit there at all.

Narrowing is capped at two codes per frame, so a quiet passage cannot pump the band edge;
widening is immediate, so a transient's high band is not a frame late.

Above **128 kbit/s per channel** the content half is skipped and the ceiling stands alone.
Narrowing buys bits, and bits are only worth buying while the rest of the spectrum is short of
them; past that rate the SNR-offset search already has more room than it can spend, so dropping
a band returns nothing and can only lose what was in it. Measured over the corpus, the mean
change from the old rate-only rule is +1.80 dB SNR / +0.013 MOS at 96 kbit/s per channel and
+0.12 dB / −0.010 MOS at 224, turning at 128 — earliest on material whose high band is
noise-like and so has no harmonic structure to mask its absence.

Setting `chbwcod` to 0–60 pins it and skips both halves.

### Block switching

Automatic — no config field toggles it. A §8.2.2 transient detector runs per full-bandwidth
channel per block, and a channel that switches anywhere in the frame is left out of coupling for
that frame. `chincpl` is written per channel, so §8.2.4.1's exclusion costs only the switching
channel its share of the tool rather than turning it off for everyone: the others still share a
coupling channel, and the excluded one carries its own high band and says so with its own
`chbwcod`. Below two members there is nothing left to share and coupling is dropped for the
frame, which is also what settles 2/0 — excluding either channel there leaves one, so a transient
in either still turns the tool off, and §5.4.3.19's `nrematbd` (one value for the pair) stays
well defined. The LFE never switches.

The coded order follows membership: the shared channel's mantissas ride immediately after the
first channel that is actually IN coupling, which is not necessarily channel 0.

### Delta bit allocation

Automatic too (§7.2.2.6) — no config field. For each exponent run the encoder builds the masking
curve a second time from the real pre-quantization coefficient magnitudes and compares it against
the curve the transmitted exponents alone imply — which is all a decoder's allocator ever sees.
Any band where the two diverge by at least one Table 5.17 step (128 units, 6 dB) gets a
transmitted correction. The decision is per channel; the LFE is excluded, because §5.4.3.49's
`deltbae` loop stops short of it — no bitstream field exists to carry an LFE correction at all.
The coupling channel is in scope like any full-bandwidth channel on both generations, and
`cpldeltbae` is emitted as new information whenever it has segments to send. Coupling does not
suppress the tool on E-AC-3 any more; two narrowings apply there instead. An AHT stream carries
no corrections, on measured grounds rather than structural ones — see `docs/index.md`. And both
encoders treat delta as a pure quality refinement: when its side-information cost would make an
otherwise-fittable frame fail to fit, the corrections are dropped and the frame re-measured
rather than refused. E-AC-3 goes further and fits the frame both ways, keeping the corrections
only when they do not cost the frame composite SNR offset — they are re-sent once, in block 0,
and retained by the other five blocks rather than repeated (§5.4.3.47's `deltbaie == 0` means
retain outside block 0), since one exponent set covers the whole frame there.

### Decision search

Off by default (`EncoderConfig::search`). With it on, the encoder stops taking §7.2.2's bit
allocation parameters as given and chooses them per frame, from the error a decoder will actually
reconstruct — measured by [`ac3::quality`](quality.md) without decoding anything.

The candidates are `dbpbcod` {2, 3} × `fgaincod` {1, 2, 4}, six in all. The no-search default -
`dbpbcod` 3 at whatever `fgaincod_for` (above) computes for the frame's rate - is scored
explicitly alongside them, so turning the search on can never silently discard that curve's own
measured win just because none of the six fixed candidates happen to match it. The other four
`BitAllocCodes` fields are left fixed on measured evidence: `floorcod` is inert (the floor never
binds at any rate on any material tried, so all eight values encode identically) and
`sdcycod`/`fdcycod`/`sgaincod` move the result by tenths. `fgaincod`'s own rate curve is a smooth
average; 1 measured worth +2 dB at 448 and +7 dB at 640 kbit/s and *regressing* at 192 is a
sharper local optimum than a smooth curve can express, which is what the fixed candidates are
for.

Two criteria, because they are different questions rather than two points on one scale:

- **`kDistortion`** minimises the reconstruction noise power. Honest and cheap, and still a
  waveform criterion — it prices a decibel in a band nobody can hear the same as a decibel in one
  they can.
- **`kPerceptual`** minimises the noise-to-mask ratio against the tonality/masking model, which
  costs the psychoacoustic analysis on top. That analysis runs once per frame rather than once per
  candidate: it describes the *signal*, and no choice of codes changes that.

The incumbent for that comparison is the PREVIOUS frame's winner, not the fixed defaults - a
candidate has to beat it by 0.05 dB to displace it. Comparing every frame against a fixed baseline
instead of the running choice was tried first and measurably cost stability: on real programme
material it switched on about a fifth of all frames, most of them a single frame reverting the
next, each one moving the masking curve for 32 ms. Carrying the winner forward gives "stay" a
standing zero-cost option, which is what turns the margin into real hysteresis.

The delta-bit-allocation on/off race below is decided the same way when the search is on: on
measured error rather than on which pass reached the higher composite SNR offset.

### Measured, not assumed

Validated on CC0/CC-BY programme material (Bach piano, Blender's *Sintel* film mix) rather than
the checked-in band-limited fixtures, decoded through FFmpeg and scored against the original by
SNR, log-spectral distance and ViSQOL MOS-LQO (`tools/ci/quality_race.py`'s own scoring, reused
rather than re-invented). Measured against the no-search baseline as it stood before `fgaincod_for`
existed (a fixed `fgaincod` 4) - `fgaincod_for`'s own rate-adaptive curve landed in the same PR
cycle as this table, and re-measuring against it is a follow-up, not done here:

| Material | Rate | Criterion | ΔSNR | ΔLSD | ΔMOS |
|---|---|---|---|---|---|
| film (stereo) | 192 kbit/s | `kDistortion` | +0.02 dB | +0.38 (worse) | −0.05 |
| film (stereo) | 448 kbit/s | `kDistortion` | **+0.82 dB** | **−0.03 (better)** | **+0.02** |
| piano (stereo) | 192 kbit/s | `kDistortion` | +0.50 dB | +1.22 (worse) | −0.01 |
| piano (stereo) | 448 kbit/s | `kDistortion` | **+0.36 dB** | **−0.01 (better)** | flat |
| piano (stereo) | 192 kbit/s | `kPerceptual` | −0.40 dB | +1.35 (worse) | −0.01 |
| piano (stereo) | 448 kbit/s | `kPerceptual` | −0.54 dB | flat | flat |
| film (stereo) | 192 kbit/s | `kPerceptual` | −2.18 dB | +0.46 (worse) | −0.06 |
| film (stereo) | 448 kbit/s | `kPerceptual` | −1.30 dB | flat | flat |

`kDistortion` is a genuine, repeatable win at 448 kbit/s and above on every material and metric
measured - the "structural unlock" this search exists to deliver. At the tighter 192 kbit/s budget
its own criterion still improves (that is what it optimises: `kDistortion`'s score is the MEAN of
each stream's own noise-to-signal ratio, not one ratio of power pooled across streams - a pooled
ratio was tried first and let a loud rematrixed channel's outcome dominate a quiet one's the same
way the composite SNR offset already does, which is exactly the failure this measure exists to
avoid), but the improvement is small enough that redistributing bits away from `dbpbcod`'s
quiet-band floor costs more in per-band spectral shape (LSD) and ViSQOL's opinion than the SNR
gains back - a real tradeoff the composite SNR offset could never have surfaced, not a bug in the
measurement. `kPerceptual` currently loses outright at every rate tested: its own objective
correctly discounts already-masked headroom, which gives it much thinner decision margins than raw
distortion, and on real stereo material with rematrixing active those thin margins are landing on
the wrong side of what external metrics prefer. Both stay off by default; `kDistortion` is the one
with evidence to turn on, and only where the material and rate resemble what was measured here.

5.1 is not in the table above: the mirror self-check proves the search's mechanism correct at
`Acmod::k3_2` + LFE (same candidates, same settlement, same re-settle-on-mismatch path
`tests/quality/test_search.cpp` exercises for stereo), but external-metric validation on real 5.1
material hit a measurement-harness alignment problem this session ran out of time to resolve, not
an encoder defect. Left for a follow-up.

See `docs/library/quality.md` for
the model and the reproduction commands.

Frame sizes are unaffected — every one of these codes is a fixed-width field, so no candidate can
cost or save a byte. E-AC-3 has the same search, through `eac3::FrameConfig::search`: Annex E's
`bamode = 1` — which this encoder writes — states the allocation parameters in the frame's own
`baie` element, so the codes are per-frame there too. Its second axis is not free the way AC-3's
is: `baie` carries no `fgaincod` at all, so a non-default fast gain has to open the per-block
`fgaincode` element (roadmap EQ7/EQ13), which is why the candidates are scored after a refit
against their own side-info cost. See [Encoding E-AC-3](encoding-eac3.md).

### Dither substitution

Content-decided by default (§7.3.4), through `EncoderConfig::dither` — `false` pins `dithflag`
at 0 unconditionally, which is what `tools/checks/verify_gold_reference.sh` needs, since real
dither values are decoder-defined and two spec-correct decoders diverge in the dithered bins by
design. The CLI spells that `dither=off`. `dithflag` is one bit per full-bandwidth channel
per block and is transmitted whichever way it reads, so the decision costs nothing; what it
decides is what the decoder puts in the bins the allocator gave no bits to. Per channel per
block, the encoder sums the real coefficient energy over those bins and compares it against the
energy the dither would replace it with (a uniform ±0.707 draw at the bin's own exponent scale,
§7.3.4's own recommended scaling), and sets the flag only when the first is at least as large as
the second — cover a hole, never paper over silence. Digital silence therefore always reads
clear, and a block-switched channel is excluded outright: two interleaved half transforms share
one coefficient set there, so a zero-bit slot is really two half-block bins and filling it
smears noise across the transient. The LFE has no `dithflag` at all (§5.4.3.2's loop is over
full-bandwidth channels), and a coupled channel is judged over both regions it receives — its own
spectrum and the shared coupling channel's band, whose zero-bit bins the decoder dithers per
*receiving* channel.

### Rematrixing

Also automatic, and 2/0 only (§7.5.3) — no config field. Per Table 7.25 band, per block, the
encoder compares the minimum power of the L/R pair against the minimum power of their sum and
difference and codes whichever pair is smaller (`rematflg` per band) — the spec's minimum-power
decision. Strongly correlated stereo then puts nearly everything in the sum channel, and the
difference channel's bits go where they are actually needed.

### Dual mono (`acmod` 0, "1+1")

Not a channel layout — two independent, single-channel programmes sharing one syncframe (a
second language track, a commentary track), each levelled and compressed on its own. Set
`acmod = ac3::Acmod::kDualMono` and `dialnorm2`; `channels[0]` is Ch1, `channels[1]` is Ch2
(`fullbw_channel_count(kDualMono)` is 2, same as stereo, but the two channels are never
downmixed, coupled or rematrixed together — coupling silently stays off even if `coupling` is
set, since averaging two unrelated programmes together would leak one into the other). The
compression fields split the same way `dialnorm`/`dialnorm2` do: `drc` and `heavy` control Ch1
only, and Ch2's compressor/range controller is built **solely** from `drc2`/`heavy2` — there is
no fallback to `drc`/`heavy` when they are unset, so configuring only `drc`/`heavy` sends Ch2
out uncompressed. Each programme then carries its own words: `dynrng`/`compr` for Ch1,
`dynrng2`/`compr2` for Ch2. By convention 1+1 carries no LFE — two unrelated mono programmes
have no shared soundfield for a subwoofer to sit in, and the plan layer never builds one — but
the encoder itself does not reject `lfe = true` here, so a caller assembling an `EncoderConfig`
by hand has to honour the convention itself.

## Latency

`ac3/latency.hpp`. The first question an engine or conferencing integrator asks, answered as a
number rather than a shrug:

```cpp
const auto budget = encoder->latency();
std::printf("%d samples, %.2f ms
",
            budget.total_samples(),
            ac3::latency_ms(budget, encoder->config().sample_rate));
// 1792 samples, 37.33 ms
```

Everything below is **algorithmic** delay — what the coding scheme costs on an infinitely fast
machine. Compute time is a separate question, answered by
[the performance trend](../performance-trend.md); transport, device buffers and resampling are
yours to add.

| Term | AC-3 | Where it comes from |
|---|---|---|
| `frame_samples` | 1536 | `encode_frame` takes exactly one frame of PCM per channel, so nothing leaves the encoder until a whole frame has gone in. |
| `transform_samples` | 256 | The MDCT/IMDCT time-domain-alias-cancellation overlap. |
| `lookahead_samples` | 0 | The encoder never needs input beyond the frame it is coding. |
| `holdback_samples` | 0 | The §3.7 hold-back is an Annex E tool; AC-3 does not have it. |
| **`total_samples()`** | **1792** | 37.33 ms at 48 kHz, 40.63 ms at 44.1 kHz, 56.00 ms at 32 kHz. |

Two of those deserve more than a table row.

**The transform term is a shift, not a wait.** Block *b*'s analysis window spans input samples
`[256b - 256, 256b + 256)`, and a decoder cannot finish any 256-sample segment until it holds
both blocks whose windows cover it. So the frame the encoder emits reconstructs input samples
`[-256, 1280)`: the frame's last block-worth of input is still in the encoder's overlap history
and only reaches the wire in the next frame. Put the other way round, **decoded output sample
`k` is input sample `k - 256`** — and that is a claim you can check, not a description of intent.
[`tests/decoder/test_latency.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/tests/decoder/test_latency.cpp)
puts an impulse into silence at a known absolute position, encodes, decodes, and finds it again
256 samples later; a decaying tone burst through the same chain gives the same 256 by
cross-correlation, which is immune to a quantizer having moved the peak.

**Lookahead is zero because of how `blksw` is decided.** `TransientDetector::detect()` reads
only the 256 *new* samples of the block it decides — §8.2.2 defines the decision on the
analysis window's second half and nothing else — so the block-switch decision never waits for
audio the frame does not already contain. A psychoacoustic encoder that deferred that decision
by a block would add 256 samples here.

`total_samples()` is a bound, not an average: a sample entering the encoder is delayed by
between `transform_samples` (the last sample of a frame) and `total_samples() - 1` (the first),
and never more. The decoder adds nothing of its own — `ac3::FrameDecoder::latency_samples()` is
zero, structurally, because `decode_frame` returns a frame's full PCM from the call that
supplies its bytes.

The same budget is on the C API as `ac3forge_encoder_latency()` /
`ac3forge_encoder_latency_samples()` (see [the C API](c-api.md)) and in Python as
`FrameEncoder.latency` / `.latency_samples` (see [the Python API](python-api.md)).

---

See also: [Metadata](metadata.md) — `dialnorm`, `drc`, `heavy` and the mix-level fields above
are all configured via the metadata layer; [Encoding E-AC-3](encoding-eac3.md) — same shape,
different container, and one tool that changes the latency budget.
