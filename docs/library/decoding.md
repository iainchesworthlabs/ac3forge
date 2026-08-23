# Decoding

## Reading a stream: `ac3::io::scan`

`ac3/io/elementary.hpp`. Finds access-unit boundaries in raw bytes and reports what the stream
carries, without being told. This is what a muxer needs, and deriving it from the bitstream
beats asking a caller who can be wrong.

```cpp
// Spans in the result point into `stream`, so it has to outlive them.
const auto scanned = ac3::io::scan(stream);
if (!scanned) {
    fmt::printf("scan failed: %.*s\n",
                static_cast<int>(ac3::io::describe(scanned.error()).size()),
                ac3::io::describe(scanned.error()).data());
    return 1;
}
fmt::printf("%s, %u Hz, %d channels, %zu access units\n",
            scanned->kind == ac3::io::StreamKind::kAc3 ? "AC-3" : "E-AC-3",
            ac3::sample_rate_hz(scanned->sample_rate), scanned->channels,
            scanned->access_units.size());
```

`ScannedStream::channels` is what the stream **renders**, which for E-AC-3 folds in every
dependent substream's `chanmap` — it is not the bed's channel count. `access_units` holds one
span per AC-3 syncframe, or per E-AC-3 independent substream together with the dependents
following it.

Both formats put `bsid` at bit 40 deliberately, so a reader can tell them apart before
committing to a layout. `ac3::stream_bsid` exposes that on its own.

## Decoding

`ac3/decoder/decoder.hpp`. Two classes, one per generation.

```cpp
// AC-3: one syncframe per access unit. For E-AC-3 use ac3::Eac3Decoder and
// decode_access_unit, which applies the §E3.8.2 render across substreams.
ac3::FrameDecoder decoder;
for (const auto unit : scanned->access_units) {
    const auto decoded = decoder.decode_frame(unit);
    if (!decoded) {
        fmt::printf("decode failed: %.*s\n",
                    static_cast<int>(ac3::describe(decoded.error()).size()),
                    ac3::describe(decoded.error()).data());
        return 1;
    }
    samples += decoded->channels.front().size();
}
```

Full program: [`examples/decode_stream.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/decode_stream.cpp).

Both decoders keep overlap-add state, so feed frames in order. `Eac3Decoder` keys its state on
`strmtyp` and `substreamid` together, because a dependent's id lives in its own numbering
space — stepping through syncframes by hand with `decode_substream` gives the same audio as
calling `decode_access_unit`.

`DecodedFrame` reports the metadata separately from applying it, which is the point of the
decoder as a check on the encoder: a test can assert on the `dynrng` words the encoder chose
*and* on the level change they cause, and those are two different claims.

| `DecoderConfig` | Default | Notes |
|---|---|---|
| `drc_scale` | 0.0 | §7.7.1 partial compression. 0 ignores `dynrng`; 1 applies it as encoded. A/52 says a consumer decoder should default to applying it — this one defaults to 0 because a reference that silently rescales its output is not a reference. |
| `heavy_compression` | `false` | §7.7.2: prefer `compr` where it exists, falling back on `dynrng` for syncframes that carry none. |
| `output` | all off | The §7.8 output stage — dialnorm, downmix, operating mode. See below. |
| `concealment` | `kNone` | §7.10: what to do with a frame that will not decode. See below. |

## The output stage

`ac3/decoder/output.hpp`. Everything between "the coded channels have been reconstructed" and
"these are the samples a listener hears": dialnorm normalisation, the §7.8 downmix, and §7.7's two
named operating modes. It is off by default — a decoder configured the way the examples above
configure it emits the coded channels untouched, sample for sample — because the decoders exist
first as a check on the encoder, and a stage that silently re-levelled or re-folded their output
would destroy that.

```cpp
ac3::FrameDecoder decoder{{
    .output = {.target = ac3::DownmixTarget::kLoRo, .mode = ac3::OperatingMode::kLine},
}};
// decoded->channels now holds two channels, Lo then Ro, at the -31 dBFS
// reference. acmod/lfe still describe what was CODED.
```

| `OutputConfig` | Default | Notes |
|---|---|---|
| `target` | `kAsCoded` | `kLoRo` (§7.8.1's plain stereo fold), `kLtRt` (§7.8.2's Dolby Surround compatible fold), `kMono` (§7.8's `output_mode == 1/0` branch), or no fold at all. |
| `mode` | `kCustom` | `kLine` (§7.7.1: dialnorm plus the full transmitted `dynrng`) or `kRf` (§7.7.2: `compr`, falling back on `dynrng` per §7.7.2.1, plus downmix overload protection). Both **override** `drc_scale`/`heavy_compression` rather than composing with them — that is what makes them modes rather than two more switches. |
| `apply_dialnorm` | `false` | §5.4.2.8 normalisation onto the −31 dBFS reference. Both named modes imply it, so this only has to be set for `kCustom`. |
| `mix_lfe` | `false` | §7.8 makes the LFE's contribution optional and this decoder drops it by default. |
| `ltrt_phase_shift` | `true` | Whether Lt/Rt's surround sum is really phase shifted 90°, or only polarity-inverted. |
| `rf_ceiling` | `1.0` | What `kRf` holds the fold under, as a linear sample magnitude. |

The matrix comes from the **stream's own** mix levels, never from constants chosen here. AC-3
carries two coarse levels in bsi (`cmixlev`, `surmixlev`; §5.4.2.4/§5.4.2.5) and E-AC-3 carries a
richer group inside `mixmdate` — separate Lt/Rt and Lo/Ro centre and surround levels plus an LFE
mix level. Both decoders now keep those and report them (`DecodedFrame::cmixlev`/`surmixlev`,
`DecodedSubstream::mix`), distinguishing "absent" from "present, and says the default";
`ac3::mix_levels()` turns either into the coefficients the stage needs, applying §7.8's own
fallbacks where a field is simply not there.

§7.8.1's normalisation — "attenuating all downmix coefficients equally, such that the sum of
coefficients used to create any single output channel never exceeds 1" — means a fold of plain
coefficients can never be louder than the loudest coded sample. That is why there is no soft-clip
anywhere in this path.

The one exception is Lt/Rt *with* its phase shift: a phase shifter preserves energy, not peak, so
that fold can come out louder than its inputs were even though every coefficient is normalised.
That follows from what §7.8.2 asks for rather than from anything decided here —
`ltrt_phase_shift = false` and `kRf` are the two ways to get a bounded output, and the second is
the one that guarantees it.

Lt/Rt's surround sum is genuinely phase shifted, through a 127-tap Hilbert transformer, with the
direct path delayed to match; `OutputStage::latency_samples()` reports the resulting 63 samples of
output delay, and is zero for every other configuration. `ltrt_phase_shift = false` selects the
sign-only matrix a lot of hardware implements instead — no latency, at the cost of the surround
sum no longer being in quadrature.

**RF mode's overload protection.** §7.7.2's `compr` guarantees a ceiling for the *mono* downmix,
not for whichever fold was actually asked for, and §7.8.1's normalisation does not cover the LFE
(§7.8 treats its contribution as an addition, at up to +10 dB). `kRf` closes that: a per-frame
gain, ramped across the frame rather than stepped at its boundary so it cannot click, backed by a
clamp so the ceiling is true and not merely likely. `OutputStage::rf_protection_db()` reports the
attenuation currently being held, so a test can assert the limiter engaged rather than only that
the output stayed under the ceiling — which silence also satisfies.

**Wide E-AC-3 layouts.** §7.8 defines folds *from* the eight AC-3 acmods and says nothing about
the layouts Annex E's `chanmap` can express: a 7.1.4 programme has no §7.8 fold, because §7.8
predates anything that could code one. `OutputStage`'s layout-aware overload therefore reduces a
rendered Table E2.5 layout to the nearest acmod layout first — each extra location seated where it
obviously belongs (a wide left is a left, a rear surround is a surround, a top front left is a
left), at −3 dB where it shares a seat — and then applies §7.8 proper. That reduction is an
extension beyond the spec and is labelled as one in the source; it is an exact identity for every
plain acmod bed, which is the case that must not change.

Verified against FFmpeg's `-ac 2` decode of the same stream: at 3/2 with `cmixlev` −3 dB and
`surmixlev` −6 dB the two agree to 119–121 dB SNR at zero lag, differing only by a single scalar
of 1/2.20711 — exactly §7.8.1's normalisation divisor for those levels (1 + 0.7071 + 0.5), which
this decoder applies and FFmpeg does not.

Not covered: Annex C's karaoke downmix rules for `bsmod` 7. The mode's `cmixlev`/`surmixlev` are
re-purposed as vocal-channel levels there, so it is a different matrix rather than a variation on
this one, and nothing in this project emits a karaoke stream to check it against.

The E-AC-3 decoder reads every Annex E coding tool — standard coupling (§E3.3), enhanced coupling
(§E3.5), spectral extension (§E3.6), the adaptive hybrid transform with GAQ (§E3.4), and transient
pre-noise processing (§3.7) — individually or stacked together, at every channel layout including
7.1.4. That includes Annex E's default coupling band structures: a block that transmits no band
structure of its own falls back to Table E2.12 (standard coupling) or Table E2.13 (enhanced
coupling) and decodes normally. Two syntax corners are still recognised and refused rather than
mis-decoded — enhanced coupling's `ecplangleintrp` (angle interpolation), and a transient
pre-noise correction reaching further back or forward than the one frame of history/lookahead
this decoder buffers — because no stream this project's own encoder produces exercises either.

Transient pre-noise processing has one API consequence worth knowing: once a stream turns it on,
`Eac3Decoder::decode_substream` holds one frame back at a time (a correction can reach into the
previous frame's already-decoded audio), returning `std::nullopt` until the next frame confirms
it. Call `Eac3Decoder::flush()` once at end-of-stream to collect whichever frame is still held
back — a stream that never uses the tool is completely unaffected, every call returns immediately
as before.

`Eac3Decoder::decode_access_unit` builds on the same convention rather than refusing it: an
access unit needs every one of its substreams ready in the same call, and the tool is a
per-substream flag, so one substream turning it on does not have to stall the others. Whichever
substreams already released this call are queued (per substream identity, oldest first) until the
lagging one catches up, so nothing already-decoded is discarded or, worse, silently paired with
the wrong instant in time — a dependent that never uses the tool can keep releasing every call
while an independent that does falls one frame behind, and each call still assembles the correct
pairing once every identity has something waiting. `flush()` drains both caches: whichever frame
`decode_substream` itself is still holding, and whichever substream results are still queued
waiting for a sibling.

Block switching (§8.2.2/§7.9) decodes on both, and is reported back: `DecodedFrame::blksw` /
`DecodedSubstream::blksw` gives, per full-bandwidth channel per block, whether that block used the
short transform — the same tier of diagnostic as `dynrng`, exposing what the encoder decided
rather than only applying it.

Dual mono (`acmod` 0, "1+1") decodes on both: it's two independent single-channel programmes
sharing one syncframe rather than a channel layout, so `DecodedFrame`/`DecodedSubstream` carry a
second `dialnorm2`/`compr2` alongside the usual fields, and each channel's §7.7 gain is applied
from its own words — Ch2 is never affected by Ch1's compression or vice versa.
`Eac3Decoder::decode_access_unit`'s `layout` comes back empty for it (`DecodedAccessUnit::acmod ==
kDualMono`), since there's no Table E2.5 location for "the second programme" to render onto — the
two channels come back in coded order (Ch1, Ch2) instead.

Delta bit allocation (§7.2.2.6) is decoded like any other transmitted parameter: both decoders
carry per-channel state across a syncframe's blocks and apply it to the masking curve before
computing `bap`, on the coupling channel as well as the full-bandwidth ones. The encode side
differs by generation: the AC-3 encoder does emit coupling-channel delta (`cpldeltbae`, whenever
the coupling channel has segments to send), while the E-AC-3 encoder skips delta entirely for
any frame where coupling is active. How corrections are chosen, and when they are dropped, is
covered in [Encoding AC-3](encoding-ac3.md#delta-bit-allocation).

Dither substitution (§7.3.4) decodes on both as well: a bin allocated zero bits (`bap` 0)
reconstructs as a true zero when its channel's `dithflag` is clear, and as a dither sample when
it is set. A coupled channel's shared bap-0 bins are dithered independently per *receiving*
channel, after decoupling — §7.3.4's own uncorrelated-noise requirement — never by dithering the
shared coupling-channel coefficient itself. The generator's state persists across frames, like
the overlap-add state, so a long stream's substituted noise does not repeat every syncframe.

`fscod2` (the Annex E half sample rates — 24, 22.05, 16 kHz) is decoded like any other rate: the
reduced rate reuses the same bit-allocation tables as its double-rate parent (§E2.3.1.4), so
nothing else about decoding changes.

## Recovering from a damaged frame

`ac3::split_frames` delimits syncframes by sync word and declared size alone — it does not
validate a frame's CRC, so it still finds every boundary correctly even when one frame's payload
is corrupt. That means a caller can decode frame by frame, catch the one bad `decode_frame`
call, and keep going rather than losing the rest of the stream over a single damaged frame — the
shape real capture/transport corruption takes, since a torn or bit-flipped frame does not
usually take its neighbours down with it.

```cpp
const auto frames = ac3::split_frames(stream);

ac3::FrameDecoder decoder;
int recovered = 0;
int failed = 0;
for (std::size_t i = 0; i < frames->size(); ++i) {
    const auto decoded = decoder.decode_frame((*frames)[i]);
    if (!decoded) {
        const auto message = ac3::describe(decoded.error());
        fmt::printf("frame %zu: decode failed (%.*s) - skipping\n", i,
                    static_cast<int>(message.size()), message.data());
        ++failed;
        continue;
    }
    ++recovered;
}
```

Full program: [`examples/decode_robustness.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/decode_robustness.cpp)
— corrupts one frame in the middle of an otherwise-good eight-frame stream and confirms the
other seven still decode.

### Concealing it instead: `DecoderConfig::concealment`

Skipping a frame keeps the *rest* of the stream, but it still leaves a hard discontinuity in the
PCM where that frame should have been. §7.10's answer is to substitute something, and
`DecoderConfig::concealment` opts into it:

```cpp
ac3::FrameDecoder decoder{{.concealment = ac3::ConcealmentPolicy::kRepeatFade}};
for (const auto unit : scanned->access_units) {
    const auto decoded = decoder.decode_frame(unit);
    if (!decoded) { /* only at the head of a stream - see below */ }
    if (decoded->concealed) {
        ++concealed;  // decoded->concealed->error says what went wrong
    }
}
```

| `ConcealmentPolicy` | What it does |
|---|---|
| `kNone` | Returns the error. The default, and what every caller got before this existed — a decoder used as a verification tool has to report a damaged frame, not paper over it. |
| `kRepeatFade` | Repeats the last good block, decaying about 20 dB across the frame and on into any further consecutive losses. Keeps the programme's texture across a short dropout. |
| `kMute` | Substitutes silence. The last good block's own overlap tail still plays out through the first block, so this fades rather than cuts. |

Both work in the **overlap-add domain** rather than on finished PCM, which is what keeps them
coherent with the frames either side: the decoders retain the last successfully decoded *block's*
windowed transform output and synthesise the concealed frame's blocks from it through the same
overlap-add a real frame goes through. So a concealed frame leaves the delay state in exactly the
shape the next good frame expects — recovery is a normal decode, not a second artefact — and the
fade at each end is the codec's own window rather than a ramp invented for the purpose.

A concealed frame comes back as a **successful** result carrying `DecodedFrame::concealed` (or
`DecodedSubstream::concealed` / `DecodedAccessUnit::concealed`), which is what lets a test assert
on the concealment itself rather than only on "the decode did not fail". Everything else on it
describes the last frame that *did* decode — a damaged frame's own bsi cannot be trusted — and
`dynrng` reads unity throughout, because no word arrived. A concealed E-AC-3 substream never
carries an object layer: OAMD and JOC describe the frame that went missing, and repeating the
previous frame's positions would put moving objects somewhere they demonstrably are not.

One case still returns the error with concealment on: a failure **before any frame has decoded**.
Concealment reconstructs from what came before it, and at the head of a stream there is nothing to
reconstruct from; inventing something there would be substituting audio rather than concealing a
gap in it.

For E-AC-3 there is a third outcome. Assembling an access unit needs every one of its substreams
in the one call, so a damaged *dependent* used to take the whole programme down with it. With
concealment on it does not: the bed is rendered on its own — real channels, just a narrower layout
than the stream promised — and the result reports `ConcealmentAction::kBedOnly`. A damaged
*independent* substream is a different matter, and is concealed (or refused) like any other frame,
because without it there is no programme at all.

`ac3cli decode` and `ac3cli monitor` expose all of this as `conceal=repeat|mute`, and report how
many frames or access units were concealed.

`split_access_units` is the E-AC-3 sibling for `Eac3Decoder::decode_access_unit`, delimiting by
independent-substream boundaries the same way. Losing an entire access unit — every substream of
it, not one substream within it — is still not recoverable here: a transport that can drop whole
units needs its own redundancy or retransmission above this layer. What this API guarantees is
that *finding* the next good boundary never depends on the previous one having decoded cleanly,
and, with concealment on, that a unit missing only some of its substreams still renders.

---

See also: [Encoding AC-3](encoding-ac3.md) and [Encoding E-AC-3](encoding-eac3.md) — what
`decode_frame`/`decode_access_unit` are undoing; [Muxing & sinks](muxing-and-sinks.md) — pairing
`ac3::io::scan` with `matroska::mux` is what keeps a container's track header honest.
