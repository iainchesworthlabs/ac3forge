# Metadata

`ac3/meta/`. Everything here is optional; leaving it out produces a stream bit-identical to
one from before the metadata layer existed. An AV receiver reads exactly these bits to set
level, compress dynamics and fold down to fewer speakers than the stream carries.

`dialnorm` cannot be derived from the frame being encoded — BS.1770 gating is defined over the
whole programme — so measure first and configure second:

```cpp
// BS.1770 Annex 1's basic algorithm: weights follow its Table 3 - unity
// front, +1.5 dB surrounds, LFE excluded outright - keyed on the Table 5.8
// acmod, whose CODED order (L, C, R, Ls, Rs, LFE) push() then expects. That
// is not WAV order (FL, FR, FC, LFE, BL, BR) for anything wider than
// stereo; ac3::io::ac3_layout_for()'s wav_index is the permutation between
// them.
ac3::meta::LoudnessMeter meter{ac3::SampleRate::k48000, kAcmod, kLfe};
for (int frame = 0; frame < kFrames; ++frame) {
    fill(pcm, frame);
    meter.push(views);
}
```

A Table 5.8 acmod cannot name a layout wider than 5.1, so a rendered E-AC-3 program - 7.1, 5.1.2,
5.1.4, 7.1.4 - has channels Table 3 has no weight for. The second constructor takes an
`eac3::chanmap::Layout` instead and applies **ITU-R BS.1770-5 (11/2023) Annex 3**'s extended
algorithm for advanced sound systems, whose Table 4 weights a channel by its position: 1.41
(+1.5 dB) between 60° and 120° azimuth below 30° elevation, 1.00 everywhere else, LFE-type
channels excluded. Everything else about the measurement - filtering, gating, true peak - is
unchanged, which is Annex 3's own wording.

```cpp
// Straight from the decoder: decode_access_unit reports the assembled
// program's Table E2.5 layout, and push() takes its channels in that order.
const auto unit = decoder.decode_access_unit(access_unit);
ac3::meta::LoudnessMeter meter{unit->sample_rate, unit->layout};
meter.push(views);

// The per-location weight on its own, if you need it: nullopt for LFE/LFE2,
// which Annex 3 drops from the sum rather than weighting zero.
const auto g = ac3::meta::position_weight(ac3::eac3::chanmap::Location::kLrs);  // 1.0 - M+135
```

Reasoning from channel names gets two of these wrong: a 7.1 layout's rear pair (`Lrs`/`Rrs`,
M±135) is **not** surround-weighted, and neither is any height channel (`Vhl`, `Vhr`, `Lts`,
`Rts`, `Vhc`, `Ts`) — Table 4's elevation row does not cover the upper layer at all. The wides
(`Lw`/`Rw`, M±060) *are*, sitting on the inclusive 60° edge. For any layout whose full-bandwidth
channels are all Table 5.8's own, the two constructors agree channel for channel.

```cpp
// nullopt until at least one 400 ms block has passed the absolute gate:
// silence has no meaningful loudness, and inventing one would put a wrong
// dialnorm on the stream.
const auto lkfs = meter.integrated_lkfs();
const int dialnorm = lkfs ? ac3::meta::dialnorm_from_lkfs(*lkfs) : 31;
```

The same pass also gives the rest of an R128 meter, off the same K-weighted, channel-summed
signal `integrated_lkfs()` already builds:

```cpp
// Momentary (400 ms) and short-term (3 s) loudness: the same un-gated block
// power integrated_lkfs() gates internally, read directly instead. nullopt
// until the respective window has elapsed.
const auto momentary = meter.momentary_lkfs();
const auto short_term = meter.short_term_lkfs();

// EBU Tech 3342 §3.1 Loudness Range: the 95th minus 10th percentile of
// short-term loudness values, gated at -70 LUFS absolute and -20 LU relative
// to their own gated mean - a different relative gate to integrated
// loudness's -10 LU, and over a different (short-term) population.
const auto lra = meter.loudness_range();

// ITU-R BS.1770-4 Annex 2: the highest sample found in a 4x-oversampled
// reconstruction of every pushed channel, LFE included - true peak is about
// physical overload headroom, not perceived loudness, so it is the one
// measure here that does not drop the LFE channel or apply the surround
// weighting.
const auto true_peak = meter.true_peak_dbtp();
```

```cpp
// Heap-allocated: FrameEncoder carries several KB of MDCT scratch/history
// state (PREfast's C6262).
auto encoder = std::make_unique<ac3::FrameEncoder>(ac3::EncoderConfig{
    .bitrate_kbps = 448,
    .dialnorm = dialnorm,
    .acmod = kAcmod,
    .lfe = kLfe,
    // §7.7.1. A/52 fixes the wire format and the intent but never the
    // curve, so the profile is this project's reading of it.
    .drc = ac3::meta::profile(ac3::meta::ProfileId::kFilmStandard),
    // §7.7.2, independent of drc: the two answer different questions, so a
    // stream may carry either, both or neither.
    .heavy = ac3::meta::HeavyConfig{.dialogue_target_dbfs = -20.0,
                                    .peak_ceiling_dbfs = -0.5},
    // Tables 5.9 / 5.10. These always define the §7.8 downmix, whatever
    // acmod is, so the heavy-compression peak detector consults them too.
    .cmixlev = ac3::meta::CentreMixLevel::kMinus4_5dB,
    .surmixlev = ac3::meta::SurroundMixLevel::kMinus6dB,
});
```

Full program: [`examples/metadata.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/metadata.cpp).

Note `meta::Profile` is the curve and `meta::ProfileId` the name of one; `meta::profile(id)`
converts. The five ids are `kFilmStandard`, `kFilmLight`, `kMusicStandard`, `kMusicLight` and
`kSpeech`.

The K-weighting in `ac3/meta/loudness.hpp` is designed analytically rather than tabulated, so
44.1 and 32 kHz work too. Calibration: a 1 kHz tone at −20 dBFS reads −19.99 LKFS, matching
FFmpeg's `ebur128` to 0.01 LU.

`ac3/meta/mixing.hpp` holds the downmix levels — `CentreMixLevel` and `SurroundMixLevel` for
AC-3, `MixMetadata` for the E-AC-3 `mixmdate` group. In `MixMetadata`, an absent
`lfemixlevcod` means LFE mixing is *disabled*, which per §E2.3.1.10 is a decision in its own
right and not the same as sending code 31.

`ac3/meta/qc.hpp` is the QC-side counterpart: named loudness/true-peak delivery
gates a decoded stream's measurement can be checked against, mirroring `meta::Profile`/
`meta::ProfileId`'s own shape:

```cpp
const auto preset = ac3::meta::qc_preset(ac3::meta::QcPresetId::kEbuR128S2);
const auto verdict = ac3::meta::evaluate_qc_gate(preset, meter.integrated_lkfs(),
                                                 meter.true_peak_dbtp());
if (!verdict.pass()) { /* ... */ }
```

`QcPresetId` names five delivery specs, each carrying a loudness limit, a true-peak ceiling
(dBTP, always a one-sided limit) and the document version and date it was read out of — see
`qc_preset()`'s own comment for the exact clause cited per preset:

| `QcPresetId` | CLI name | Loudness | Max true peak | Source (version, date) |
|---|---|---|---|---|
| `kEbuR128S2` | `ebu-r128-s2` | −23.0 LUFS ±1.0 LU | −1.0 dBTP | EBU R 128 s2 v3 + EBU R 128 v5 (November 2023) |
| `kAtscA85` | `atsc-a85` | −24.0 LKFS ±2.0 dB | −2.0 dBTP | ATSC A/85:2026-07 §6 (8 July 2026) |
| `kAtscA85Streaming` | `atsc-a85-streaming` | −25.0 LKFS ±2.0 LU (the −23…−27 band) | −2.0 dBTP | ATSC A/85:2026-07 Annex L.5 (8 July 2026) |
| `kNetflix` | `netflix` | −27.0 LKFS ±2.0 LU | −2.0 dBTP | Netflix Sound Mix Specs v1.6 / Atmos Home Mix v2.3 |
| `kAppleMusicAtmos` | `apple-music-atmos` | ≤ −18.0 LKFS (ceiling) | −1.0 dBTP | Apple Immersive Audio Source Profile (BS.1770-4) |

`QcPreset::loudness_limit` says which kind of limit the loudness figure is. Most specs state a
target with a symmetric tolerance (`QcLoudnessLimit::kBand`); Apple's states only a level not to
exceed (`kCeiling`), where a quieter master is compliant however quiet it is — gating that as a
±band would fail material the specification accepts. `QcPreset::source` carries the document,
version and date, so a report can name the edition it judged against.

`evaluate_qc_gate()` is the pure comparison:
`QcVerdict::loudness_delta_lu`/`true_peak_margin_dbtp` report signed distance from the target/
ceiling, and `pass()` is both halves together. `ac3cli qc` is this same API driven end to end over
a real, already-encoded file — see [`examples/qc_report.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/qc_report.cpp)
for the library-only version: encode with a dialnorm that does not match the real level, decode,
measure, and see the mismatch and the gate verdicts it produces.

---

See also: [Encoding AC-3](encoding-ac3.md) and [Encoding E-AC-3](encoding-eac3.md) — where
`dialnorm`, `drc`, `heavy` and the mix-level fields are actually consumed.
