# Metadata

`ac3/meta/`. Everything here is optional; leaving it out produces a stream bit-identical to
one from before the metadata layer existed. An AV receiver reads exactly these bits to set
level, compress dynamics and fold down to fewer speakers than the stream carries.

`dialnorm` cannot be derived from the frame being encoded — BS.1770 gating is defined over the
whole programme — so measure first and configure second:

```cpp
// Weights follow BS.1770 Table 3: unity front, +1.5 dB surrounds, LFE
// excluded outright.
ac3::meta::LoudnessMeter meter{ac3::SampleRate::k48000, kAcmod, kLfe};
for (int frame = 0; frame < kFrames; ++frame) {
    fill(pcm, frame);
    meter.push(views);
}

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
AC-3's own bsi, `MixMetadata` for the E-AC-3 `mixmdate` group. In `MixMetadata`, an absent
`lfemixlevcod` means LFE mixing is *disabled*, which per §E2.3.1.10 is a decision in its own
right and not the same as sending code 31.

## Programme mixing (`mixmdate`, Table E1.2)

The five levels at the top of `MixMetadata` say how to fold *this* programme down. Everything
below them says how to combine it with a *second* one — the audio-description, commentary or
alternate-language service a receiver mixes against the main programme. §E2.3.1.17's "external
program" is that second stream: a separate bit stream or independent substream being decoded
alongside this one. Table E1.2 gates the whole group on `strmtyp == 0x0`, so only an independent
substream writes it; a dependent stops after the levels.

```cpp
ac3::meta::MixMetadata mix;
// §E2.3.1.13: 0 is mute, 1..63 are -50 dB to +12 dB in 1 dB steps. An absent
// scale factor is 0 dB stated in one bit rather than seven.
mix.pgmscl = 45;                                   // -6 dB on this programme
mix.extpgmscl = 54;                                // +3 dB on the external one
// Table E2.6's mixing option 4: the flexible one, sized by mixdeflen.
mix.mixing.mixdef = ac3::meta::MixDefinition::kExtended;
mix.mixing.external = ac3::meta::ExternalScales{
    .premix = {.premixcmpscl = 0},                 // §E2.3.1.21's recommended value
    .left = 0, .centre = 6, .right = 0,            // Table E2.8 codes
    // std::nullopt is §E2.3.1.31's cleared flag: the external programme has
    // NO such channel, which is not a scale factor of 0 (that is -1 dB).
    .left_surround = std::nullopt,
};
// §E2.3.1.59-61: one 5-bit word per block, each independently optional.
mix.blkmixcfginfo = std::array<std::optional<int>, ac3::kBlocksPerFrame>{3, {}, 7, {}, {}, 31};
config.mixing = mix;
```

`MixDefinition` names Table E2.6's four options: `kNone`, `kPremix` (the five-bit
`premixcmpsel`/`drcsrc`/`premixcmpscl` triple alone), `kReserved` (twelve reserved bits carried
verbatim) and `kExtended`. Under `kExtended`, `mixdeflen` sizes the *whole* element — sub-fields
and the byte-alignment fill together — so the encoder measures the contents before writing the
length, and the decoder places itself from the length rather than from where its field walk
stopped. A stream using a sub-field this build does not model therefore still leaves the reader
at the right offset.

`PanInfo` is §E2.3.1.53-58's placement for a mono or 1+1 programme (`acmod < 0x2` only):
`panmean` indexes 1.5-degree steps clockwise from the centre speaker, 0..239 covering 0..358.5
degrees.

## Bit stream information (`ac3/meta/bsi.hpp`)

The fields a frame carries *about itself*, as opposed to the coding parameters that say how to
decode it. None of them changes an output sample; what they decide is whether a receiver can tell
a complete main programme from an audio-description track, whether a stereo pair is a Dolby
Surround matrix that wants a Pro Logic decoder behind it, and what acoustic level the mix was
judged at.

`BsiInfo` is one struct both codecs fill. AC-3 spreads the group across bsi (§5.4.2); E-AC-3
gathers the same fields into one optional `infomdat` element (Table E1.2, §E2.3.1.62 — which
defers to §5.4.2 for every one of them bar `sourcefscod`). Which fields actually reach the wire
depends on `acmod` and on the codec, exactly as the syntax says, so a caller may set the lot and
let the layout decide:

```cpp
config.info.bsmod = ac3::meta::BitstreamMode::kVisuallyImpaired;   // Table 5.7
config.info.dsurmod = ac3::meta::SurroundMode::kDolbySurround;     // 2/0 only
config.info.audprod = ac3::meta::AudioProduction{
    .mixlevel = 25,                                                // 80 + 25 = 105 dB SPL
    .roomtyp = ac3::meta::RoomType::kLargeRoomXCurve,
    // Annex E's audprodie carries this third field; AC-3's stops at roomtyp
    // and puts adconvtyp in Annex D's xbsi2 instead.
    .adconvtyp = ac3::meta::AdConverterType::kHdcd,
};
config.info.copyrightb = true;
config.info.timecod1 = ac3::meta::TimeCodeCoarse{.hours = 17, .minutes = 43, .eight_seconds = 5};
config.info.timecod2 = ac3::meta::TimeCodeFine{.seconds = 6, .frames = 21, .sixty_fourths = 39};
```

The two time-code halves are two structs rather than one because Table 5.13 makes "the coarse
half only" a legal state in its own right, not a partly filled value. `langcod` is a presence
flag rather than a value: §5.4.2.12 makes it an 8-bit reserved value that shall be 0xFF if
present, the language table it once indexed having been dropped in favour of the signalling
layer's own ISO 639-2 code, so the writer always sends 0xFF.

### Annex D's alternate syntax (`bsid` 6)

AC-3's two 14-bit `timecod` fields have never been applied for their originally anticipated
purpose (§D1), so Annex D reuses them. Setting `EncoderConfig::alternate_bsi` writes `bsid` 6
and spends those same 28 bits on `xbsi1` and `xbsi2`:

```cpp
config.alternate_bsi = ac3::meta::AlternateBsi{
    // xbsi1 (§D2.3.1.1-6): the same five quantities mixmdate carries, which is
    // why it reuses MixMetadata. Annex D has no LFE mix level and none of
    // mixmdate's programme-mixing depth, so nothing past the levels is read.
    .mix = ac3::meta::MixMetadata{
        .dmixmod = ac3::meta::DownmixMode::kLtRt,
        .ltrtcmixlev = ac3::meta::MixLevel::kMinus1_5dB,
        .lorocmixlev = ac3::meta::MixLevel::kMinus4_5dB,
        .ltrtsurmixlev = ac3::meta::MixLevel::kMinus3dB,
        .lorosurmixlev = ac3::meta::MixLevel::kMinus6dB,
    },
    // xbsi2 (§D2.3.1.7-12).
    .extended = ac3::meta::ExtendedBsi{
        .dsurexmod = ac3::meta::SurroundExMode::kSurroundEx,
        .dheadphonmod = ac3::meta::HeadphoneMode::kDolbyHeadphone,
        .adconvtyp = ac3::meta::AdConverterType::kHdcd,
    },
};
```

Table D2.1 pairs the four levels Lt/Rt-then-Lo/Ro where Table E1.2 pairs them
centre-then-surround — the same five quantities in a different order on the wire, which is why
they are written and read against Annex D's own order rather than `mixmdate`'s.

Annex D and a time code cannot both be sent: they occupy the same 28 bits. `FrameEncoder`
refuses the pair with `FrameError::kInvalidBsi`, and `plan::validate` catches it earlier as
`PlanError::kTimecodeNeedsBsid8`. `kInvalidBsi` is also what refuses a value too wide for its
field — a mixing level above 31 needs six bits where §5.4.2.14 has five, and writing it would not
merely record the wrong level, it would push every following field one bit along and the frame
would decode as something else entirely.

§D2.3.1.11's `xbsi2` byte is reserved and encoders shall set it to zero, so the writer always
does; the decoder reports whatever it read, for a third-party stream that does otherwise.
`encinfo` is the one bit here reserved for the encoder's own use (§D2.3.1.12).

### On decode

`DecodedFrame` reports `bsid`, `cmixlev`, `surmixlev`, `info` and (for `bsid` 6)
`alternate_bsi`. `DecodedSubstream` and `DecodedAccessUnit` report `mixing` and `info`, each
`std::nullopt` when the corresponding `mixmdate`/`infomdate` flag was clear. Fields the layout
gives no home to keep their defaults rather than reporting bits that were never on the wire — a
3/2 frame sends no `dsurmod`, so `info.dsurmod` stays "not indicated".

`ac3/meta/qc.hpp` is the QC-side counterpart: named loudness/true-peak delivery
gates a decoded stream's measurement can be checked against, mirroring `meta::Profile`/
`meta::ProfileId`'s own shape:

```cpp
const auto preset = ac3::meta::qc_preset(ac3::meta::QcPresetId::kEbuR128S2);
const auto verdict = ac3::meta::evaluate_qc_gate(preset, meter.integrated_lkfs(),
                                                 meter.true_peak_dbtp());
if (!verdict.pass()) { /* ... */ }
```

`QcPresetId` names three delivery specs — `kEbuR128S2`, `kAtscA85`, `kNetflix` — each carrying a
target integrated loudness, a symmetric tolerance (LU) and a true-peak ceiling (dBTP, a one-sided
limit) read from its own primary source (EBU R 128 s2 + EBU R 128, ATSC A/85:2013 §6, and
Netflix's Sound Mix Specifications & Best Practices, respectively — see `qc_preset()`'s own
comment for the exact clause cited per preset). `evaluate_qc_gate()` is the pure comparison:
`QcVerdict::loudness_delta_lu`/`true_peak_margin_dbtp` report signed distance from the target/
ceiling, and `pass()` is both halves together. `ac3cli qc` is this same API driven end to end over
a real, already-encoded file — see [`examples/qc_report.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/qc_report.cpp)
for the library-only version: encode with a dialnorm that does not match the real level, decode,
measure, and see the mismatch and the gate verdicts it produces.

---

See also: [Encoding AC-3](encoding-ac3.md) and [Encoding E-AC-3](encoding-eac3.md) — where
`dialnorm`, `drc`, `heavy` and the mix-level fields are actually consumed.
