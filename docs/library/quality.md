# Measuring quality: `ac3::quality`

`ac3/quality/distortion.hpp` and `ac3/quality/perceptual.hpp` are the encoder's measuring
instruments. They answer two questions an encoder has to be able to answer before it can choose
anything: **how much error will the decoder reconstruct**, and **how much of that error can the
signal hide**.

Nothing here is on by default. `EncoderConfig::search` and its E-AC-3 counterpart,
`eac3::FrameConfig::search`, are the only things in the library that read them, and both are
`kNone` unless a caller asks — see [Encoding AC-3 § Decision
search](encoding-ac3.md#decision-search) and [Encoding E-AC-3](encoding-eac3.md)'s own `search`
row, which is narrower (CBR only, `kDistortion` only, one axis rather than two — see its own
entry for why).

## Why this exists

A/52 §7.2.2's bit allocation is itself a masking model, but a deliberately blind one. The
routine has to run identically inside every decoder from transmitted values alone, so its
masking curve is built from the exponents and nothing else: `psd[bin]` is `3072 - (exp << 7)`.
Two bands with the same energy get identical treatment whether one is a held violin note and the
other is cymbal wash.

That is correct for a decoder and limiting for an encoder, which is the only side that can tell
the difference. Until this module existed, the encoder had no way to price the difference either
— its one in-loop quality number was the composite SNR offset the fit search maximises, and that
number is only comparable between candidates that produce the *same* masking curve. Change
`dbpbcod`, or an exponent strategy, or a delta segment, and the curve moves under the number.
Both recorded attempts at a per-frame search failed on exactly that.

## `distortion.hpp` — what the decoder will reconstruct

`accumulate_block()` takes the three things the encoder already holds at the point of choosing —
the fixed-point coefficients, the decoded exponents (§8.2.10's mirror rule makes them the
decoder's), and the bit allocation pointers — and produces signal and noise power per band. No
decode is involved: §7.3's quantizers are deterministic functions of those three.

```cpp
ac3::quality::BandNoise measured;
for (int block = 0; block < ac3::kBlocksPerFrame; ++block) {
    ac3::quality::accumulate_block(fixed, exponents, bap, start, end, measured);
}
const double snr = ac3::quality::snr_db(measured);
```

The banding is A/52 Table 7.13's own 50 bands, not a separate critical-band partition, so a
measurement lands in the same bands the allocator works in and the same bands a §7.2.2.6 delta
segment can correct.

| Symbol | What it is |
|---|---|
| `BandNoise` | Signal and reconstruction-noise power per band, in normalised coefficient power. Accumulates across blocks. |
| `accumulate_block` | Adds one (stream, block). `fixed` is indexed from the stream's own first coded bin; `exps`/`bap` from bin 0, exactly as `compute_bit_allocation` takes them. |
| `reconstruction_error` | One bin, for diagnostics and for the equivalence test. |
| `snr_db`, `band_snr_db` | Ratios in dB, saturating at `kMaxSnrDb` rather than returning infinities. |
| `Criterion` | What a search minimises: `kNone`, `kDistortion`, `kPerceptual`. |

`accumulate_block` evaluates `dequantize_mantissa(quantize_mantissa(m, bap), bap)` in closed form
rather than calling the pair, which is what makes a per-candidate measurement affordable inside
the frame loop. That is a second copy of the §7.3 quantizer arithmetic — the shape of bug
`ac3/verify/mirror.hpp` exists to catch — so it is pinned rather than trusted:
`tests/quality/test_distortion.cpp` sweeps every `bap` over the quantizer's decision boundaries
and the full mantissa range, at every exponent a coefficient can carry, and requires **bit-exact**
agreement.

!!! note "More bits is not always less error"
    A/52's symmetric quantizers (bap 1–5) are not nested grids: 3 levels reconstruct at ±2/3,
    5 levels at ±0.8 and ±0.4, and nothing in the second set lies near 2/3. A mantissa at 0.62 is
    therefore served *better* by bap 1 than by bap 2. What holds is the aggregate — expected
    squared error falls with every step of `bap` — and that is what makes the measurement useful.
    A search that assumed the pointwise version would be assuming its way past a real property of
    the format.

## `perceptual.hpp` — what the signal can hide

`PerceptualModel` produces, per band, the noise power that band can mask. It is Johnston's
perceptual-entropy formulation with the tonality estimate taken from MPEG-1 psychoacoustic
model 2:

1. **Band energy** over the same 50 bands.
2. **Tonality** from inter-block spectral unpredictability — this block's magnitude spectrum
   against a linear extrapolation of the previous two, normalised. Steady tones extrapolate well
   and score near 1; noise and transients do not.
3. **Spreading** across bands by Schroeder's function over Bark distances.
4. **A signal-to-mask requirement** interpolated by tonality between the tone-masking-noise limit
   (14.5 + *z* dB) and the noise-masking-tone limit (5.5 dB).
5. **An absolute-threshold floor** (Terhardt), optional and capped.
6. **Perceptual entropy** per block — an estimate of the bits needed to code it transparently.

```cpp
ac3::quality::PerceptualModel model(ac3::SampleRate::k48000, channels);
ac3::quality::BlockAnalysis analysis;
model.analyse(channel, coefficients, endmant, analysis);   // once per block, in block order

const auto nmr = ac3::quality::noise_to_mask(measured, threshold);
// nmr.mean_db > 0 means the reconstruction noise is above what the signal can hide.
```

`noise_to_mask` averages the per-band *ratios* rather than dividing the sums. That is deliberate:
the composite SNR offset's failure mode is a loud band's slack paying for a quiet band's excess,
and a replacement measure that did the same thing would be no replacement.

### What is approximate, and why it is said out loud

MPEG-1's unpredictability measure is computed on a complex FFT spectrum, using magnitude **and**
phase. This encoder has an MDCT, which is real; its coefficients carry phase as sign, in a form
that does not linearly extrapolate. What is implemented is the magnitude half of that measure, on
a three-bin smoothed magnitude — smoothed because a stationary sinusoid's MDCT magnitude is
modulated block to block by time-domain aliasing, and an unsmoothed per-bin magnitude would call
a held note unpredictable. The published model is not being claimed; a documented reduction of it
is, and the tests pin the behaviour that matters: a held tone scores above 0.7, white noise below
0.35 on average, and a tone burst with no history below 0.5 despite an identical spectrum.

The absolute threshold is a switch rather than a constant, and it is capped at 60 dB SPL by
default. Terhardt's curve reaches 160 dB SPL by 20 kHz, which uncapped would let the top bands be
discarded outright on any material — and this project has a recorded case of exactly that kind of
change measuring well for the wrong reason (see `encoder.cpp`'s `chbwcod` comment).

The SPL calibration is measured, not assumed. A full-scale sine through this project's own
analysis window and MDCT produces a block energy of 0.25, and `kFullScaleBlockEnergy` is asserted
against that in `tests/quality/test_perceptual.cpp`, so a change to the transform's scaling cannot
quietly move the threshold. The convention that full scale is 96 dB SPL is a `PerceptualConfig`
field, because it is a convention rather than a measurement.

## Validation

`EncoderConfig::search` is where these two measures meet the AC-3 encoder - see
[Encoding AC-3 § Decision search](encoding-ac3.md#decision-search) for what it chooses between and
the measured before/after table. In short: `kDistortion` is a real, repeatable win at 448 kbit/s
and above; at 192 kbit/s its own criterion still improves but external metrics (log-spectral
distance, ViSQOL) show it trading SNR against per-band spectral shape. `kPerceptual`
currently loses at every rate tested - a real finding about the model's calibration on real
material, not a claim this page is hiding.

`eac3::FrameConfig::search` (roadmap EQ13) is the E-AC-3 side, narrower on purpose: CBR only,
`kDistortion` only, and one axis (`dbpbcod`) rather than the two AC-3's search has, because
E-AC-3 has no per-frame `fgaincod` to search yet. Measured on real CC0 stereo material, its effect
is negligible at every rate tried - see [Encoding E-AC-3](encoding-eac3.md)'s own `search` row and
ROADMAP.md's EQ13/EQ8 entries for the numbers and why a single-axis search over an already-tuned
default has little left to find.

Reproducing the numbers needs material this project does not check in (the committed fixtures are
narrow-band synthesized noise, exactly the trap `encoder.cpp`'s `chbwcod` comment warns about) and
`visqol-python`, which CI does not install by default:

```
pip install visqol-python
python tools/ci/quality_race.py ac3            # this project's own scoring machinery
```

then encode real programme material (a CC0/CC-BY source, not the fixtures) with
`search=distortion` / `search=perceptual` against `search` omitted, decode with FFmpeg, and score
with `quality_race.aligned_snr`/`spectral_scores`/`perceptual_score` the way this validation did.

## References

- J. D. Johnston, "Transform Coding of Audio Signals Using Perceptual Noise Criteria",
  *IEEE Journal on Selected Areas in Communications* **6**(2), February 1988, pp. 314–323.
- ISO/IEC 11172-3:1993, Annex D.2 — MPEG-1 psychoacoustic model 2 (the unpredictability measure
  and its mapping to tonality).
- M. R. Schroeder, B. S. Atal, J. L. Hall, "Optimizing digital speech coders by exploiting masking
  properties of the human ear", *JASA* **66**(6), 1979, pp. 1647–1652.
- E. Zwicker, E. Terhardt, "Analytical expressions for critical-band rate and critical bandwidth
  as a function of frequency", *JASA* **68**(5), 1980, pp. 1523–1525.
- ATSC A/52:2018 §7.2.2 (bit allocation), §7.2.2.6 (delta bit allocation), §7.3 (quantization).
