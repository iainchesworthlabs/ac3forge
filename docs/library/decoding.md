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

## Where access unit *i* starts: `ac3::io::access_unit_timing`

Same header. `ScannedStream::access_unit_samples` records how many samples each access unit
codes, parallel to `access_units`. That is always 1536 for AC-3 (§5.3.1: six blocks of 256, no
other option), but E-AC-3's `numblkscod` lets an independent substream code 1, 2, 3 or 6 blocks
(§E2.3.1.4), so an E-AC-3 access unit is 256, 512, 768 or 1536 samples long — and a stream may
mix lengths.

```cpp
const auto at = ac3::io::access_unit_timing(*scanned, index);
at->start_sample;                     // samples from the start of the stream
at->duration_samples;
at->start_seconds();
at->start_in_timescale(90'000);       // MPEG-2 systems clock
at->duration_in_timescale(90'000);
```

Every value is computed from the **absolute** sample position rather than by summing per-frame
increments. A frame duration is very often not a whole number of ticks in whatever timescale a
container uses (1536 samples at 44.1 kHz is 34.83 ms), so a running sum drifts; computing from
the absolute position keeps the error under one tick however long the stream runs. `duration_in_timescale`
is the difference between this unit's converted start and the next one's, not the duration
converted on its own — otherwise a run of durations would not add up to the start times they
are supposed to.

Three lookups go with it:

| Call | Answers |
|---|---|
| `stream_duration_samples` / `stream_duration_seconds` | How long the whole thing is |
| `access_unit_at_sample` / `access_unit_at_seconds` | Which unit covers a position — i.e. where a frame-aligned cut lands. A position inside a unit names that whole unit; a cut is never a split |
| `uniform_access_unit_samples` | The one length every unit shares, or nothing when they differ |

That last one is exactly the question a fixed-duration container track can answer and a variable
one cannot: `mp4::AudioTrack`, `mpegts::AudioTrack` and `matroska::AudioTrack` each hold a single
`samples_per_frame`, so a stream it returns nothing for cannot be described to them without
per-sample durations they do not model. `ac3cli`'s `mkv`/`mp4`/`fmp4`/`ts` take the figure from
here and refuse such a stream rather than muxing it to a silently wrong timeline.

## Changing metadata without re-encoding: `ac3::io::metadata_edit`

`ac3/io/metadata_edit.hpp`. `dialnorm`, `compr`, `bsmod` and `dsurmod` are delivery decisions —
what a receiver is told the dialogue level is, how hard to compress on an RF output, what kind of
service this is, whether the surrounds were matrixed. All four live in `bsi`, ahead of the first
`audblk`, and none of them changes a coded coefficient. Re-encoding a programme to correct one
costs a whole generation of lossy coding for nothing.

```cpp
std::vector<std::byte> stream = /* an AC-3 or E-AC-3 elementary stream */;
const auto summary = ac3::io::edit_stream_metadata(stream, {.dialnorm = 24, .bsmod = 2});
// summary->syncframes visited, summary->changed actually different afterwards
```

`read_frame_metadata` reports what one syncframe carries — including E-AC-3's whole `mixmdate`
group and AC-3's two `bsi` downmix levels — and which of the rewritable fields it transmits at
all.

The CRCs are the part that is not obvious. `crc2` is an ordinary trailing CRC. `crc1` is not:
A/52 §7.10.1 puts it **before** the region it protects and requires the register to read zero
once the first 5/8 of the syncframe has been shifted through, so it has to be *solved* rather
than computed — `ac3::solve_leading_crc` (`ac3/core/crc16.hpp`) does that with a GF(2)
polynomial inverse, and is the same function the encoder itself uses. `restamp_crc` is public
for a caller doing its own bsi surgery (`ac3::signing::sign_atmos_frame` is the in-project
precedent) so nobody has to reimplement that solve.

Stated as limits rather than left to be discovered:

- **Only fields already on the wire can change.** `compr` lives behind `compre`, and E-AC-3's
  `bsmod`/`dsurmod` behind `infomdate`; a frame that did not transmit one has no bits to
  overwrite, and inserting them would move every bit after it and re-frame the syncframe — which
  is a re-encode by another name. That is `kFieldAbsent`, and the answer is to encode (or
  transcode) with the field enabled. This project's own E-AC-3 encoder never sets `infomdate`,
  so its streams carry no `bsmod`/`dsurmod` to rewrite.
- A **dependent** E-AC-3 substream reports no `compr` whatever its `compre` bit says: §E3.8.5
  repurposes that bit to mark the last dependent of the programme. Its eight bits are still
  skipped correctly; they are simply not a `compr` word.
- `strmtyp 2` (a convertible substream, §E2.3.1.1) is refused outright, matching
  `ac3::plan::validate`'s own stance.

A field named in an edit that **no** syncframe in the stream carries fails before anything is
written, so the stream is either fully rewritten or left byte-for-byte alone — a metadata option
that silently did nothing is indistinguishable from one that does not work.

`ScannedStream` also carries the raw syntax values a container writer needs but cannot
re-derive: `bsid`, `bsmod` (with `bsmod_present`, since Annex E carries it only inside
`infomdate`), `bit_rate_code`, `dsurmod`, `mix_metadata`, `oba_complexity_index`, and — for
E-AC-3 — `independent_substreams` plus a `SubstreamService` for substreams 1–3. Those feed
`ac3::io::build_codec_config_box`'s `dac3`/`dec3` payload and the MPEG-TS PMT descriptors of
both broadcast profiles (see [Muxing & sinks](muxing-and-sinks.md#muxing-mpegtsmux)).
`independent_substreams` is an *observation* of which substream ids appear; it deliberately does
not change how `scan` groups access units, which stays one-programme (ROADMAP.md's DC5).

## Object-layer strip

`ac3/io/object_strip.hpp`. The inverse of the object encoder, at the bitstream level: it takes
the EMDF/JOC object layer out of a Dolby Digital Plus stream without decoding anything.

```cpp
const auto stripped = ac3::io::strip_objects(joc_stream);
if (!stripped) {
    // describe(stripped.error()) says why
}
// stripped->bytes is a plain DD+ 5.1 stream; the bed decodes identically.
```

A DD+ JOC stream is an ordinary 5.1 E-AC-3 stream that happens to carry an EMDF container —
OAMD positions plus JOC side information — inside the per-block skip fields the standard already
requires a decoder to step over. The bed underneath is the **full mix**, every object already
panned into it, which is exactly why an Atmos-unaware decoder can play the stream at all. So the
5.1 rendition of a JOC stream does not need re-encoding; it needs the container taken out:

- every exponent and mantissa is copied bit for bit, so the result decodes to sample-identical
  PCM (the tests assert exactly that, and FFmpeg's own decode of both streams agrees);
- the skip fields go entirely, along with TS 103 420 §8.3.1's `addbsi` object-audio marker, so
  nothing downstream — a `dec3` box's Atmos extension, an HLS `CHANNELS="<N>/JOC"` attribute —
  still claims an object layer;
- `frmsiz` is re-derived for the shorter frame, the auxdata padding is re-laid, and `crc2` is
  re-stamped. Unlike an AC-3 syncframe an E-AC-3 one has no `crc1` (Annex E dropped it, leaving
  `syncinfo` as the syncword alone), and `crc2` is the frame's last field covering everything
  before it, so re-stamping is a plain forward recompute.

A rewritten frame is sized to the content it actually holds — §E2.3.1.3 makes `frmsiz` an
arbitrary per-frame word count, unlike AC-3's index into Table 5.18. So the output is smaller
than the input, which is the point for a delivery rendition, but a constant-rate input does not
stay constant-rate: alongside the container, whatever auxdata padding the encoder used to hit
its target rate goes too. A frame with nothing to remove is copied byte for byte.

The container is **removed, not emptied**: an EMDF container with no payloads would still signal
an object layer for a stream that no longer has one, and this project's rule is that a stream
carries objects or omits the container entirely (see [Atmos & JOC](../concepts/atmos-joc.md)).

Frames with no object layer pass through byte for byte — including frames of a bitstream shape
this build cannot rewrite, since a frame with neither the `addbsi` marker nor a skip field has
nothing to strip whatever its shape. A frame that *does* carry an object layer in a shape the
frame walker (`ac3/emdf/frame_layout.hpp`) does not map is refused with `kUnsupportedFrame`
rather than passed through, because passing it through would hand back a stream still carrying
the objects this function promises to remove. An AC-3 stream is refused outright
(`kNotEac3`): Annex E is where substreams and skip fields live.

This is the inverse of `ac3::signing`'s in-place EMDF rewrite and, like it, needs no key —
taking a container out is not authenticating one. Both share one bit-accurate frame walk
(`ac3::emdf::walk_frame`) so the two cannot drift apart.

`ac3cli strip-objects in.ec3 out.ec3` is the command-line front end, and `ac3cli fmp4 …
fallback-51` uses it to write the paired 5.1 HLS rendition Apple's authoring requirements ask
for beside an Atmos one.

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
| `joc_domain` | `joc::Domain::kQmf` | Which domain JOC object reconstruction (above) runs §6.6.6's matrix in: `kQmf` is §7.1's 64-band complex filterbank, what the clause describes and what a licensed decoder runs; `kMdctBand` is the cheaper 512-sample-MDCT approximation this project used before the filterbank existed, correct only against a matrix estimated the same way (`AtmosConfig::joc_domain` on the encode side). The two have different algorithmic delay — `joc::reconstruction_delay(domain)` — so a caller comparing `object_audio` against a known source has to shift by it. |
| `trace` | `nullptr` | AC-3 self-check (`FrameDecoder`): where the decoder records what it derived per block, for `ac3::verify` to diff against the encoder's own model. See below. |
| `eac3_trace` | `nullptr` | The E-AC-3 counterpart (`Eac3Decoder`), one whole access unit rather than one frame. See below. |

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

## The object layer

An Atmos-in-DD+ stream carries OAMD and JOC payloads in an EMDF container tucked into a block
skip field. `Eac3Decoder` reads it whenever it is there: `DecodedSubstream::object_metadata`
is the decoded programme (`std::nullopt` for plain E-AC-3, and equally for a container that
was found but could not be read — a failure there never fails the surrounding frame, which is
the whole point of EMDF), and `object_audio` is JOC's reconstructed per-object waveforms.

`object_indices` says what each `object_audio` entry *is*: an index into the payload's own
object order (bed channels, then ISF, then dynamic objects), which is what
`oba::joc_object_indices()` computes for the programme. For a dynamic-object-only programme —
what this project's own encoder writes — entry *i* is `object_metadata->objects[i]`; for a bed
programme it names the bed channel instead, which `oba::bed_labels()` turns into a speaker
label. `ac3cli decode <in> <out.wav> <objects_dir>` writes one WAV per entry.

What the parsers read is deliberately much wider than what the encoder writes, because real
streams are wider. On the OAMD side: any number of metadata update blocks at any sample offset
and ramp duration; object size, zone constraints, elevation gating, snap, screen reference,
distance, explicit priority and Table 18's gain-reuse; positions coded differentially against
the previous block; inactive objects; several bed instances, standard or non-standard;
programmes carrying an intermediate spatial format; the `trim_element` and the
`extended_object_element`. An `oa_element` with an id this decoder does not know is skipped by
its own `oa_element_size` and named in `DecodedProgram::skipped_elements`, rather than costing
the payload — which is exactly what that size field is for. On the JOC side: all five of
Table 47's downmix configurations, any clip gain, and per-object band count, quantizer, sparse
mode, interpolation slope and data-point count. The EMDF reader parses the whole of
§H.2.1.3's payload configuration and reports it on `DecodedPayload::config` rather than
insisting on the one shape TS 103 420 Table 56 mandates — real Dolby streams do not restrict
themselves to it even for their own object payloads.

What is still refused, and why each one has to be:

| Refused | Reason |
|---|---|
| `oa_md_version_bits` other than 0 | §5.6.0.1 defines no field layout for another version, so every offset after it would be a guess. |
| `intermediate_spatial_format_idx` 6 or 7 | Table 11b reserves them and gives them no object count, so the bed/ISF/dynamic split cannot be worked out. |
| `joc_dmx_config_idx` 5–7 | Table 48 gives them no channel count, so `joc_data` has no loop bound. |
| `joc_ext_config_idx` ≠ 0 | Table 49 reserves every value and §6.2.1 gives `joc_ext_data()` no syntax and no length — there is nothing to read and nothing to skip. |
| `emdf_version` ≠ 0 | §H.2.2.2 defines the container's fields only for version 0. |
| `protection_length_primary` = `0b00` | Table H.2.5 reserves it, so it names no width to skip. |

Two values are read but deliberately not *applied*. `joc_clipgain` (§6.3.3.2) is computed onto
`FrameParameters::clip_gain` and left there: no clause in TS 103 420 says where in the decode
chain the gain belongs, and the published equation renders ambiguously enough that a real DEE
stream's own value lands outside the range the same clause states. And Table 47's two "90 degree
phase shift" downmix configurations reconstruct like their unshifted siblings — the shift is a
property of how the downmix was *built*, §6.6.6 says nothing about undoing it before matrixing,
and there is no Hilbert filterbank here to undo it with.

Reconstruction needs the downmix JOC asks for. Table 47's 7-channel configurations want Lb/Rb
from a dependent substream, which `decode_substream` does not have in hand, so those parse but
leave `object_audio` empty; the metadata still decodes and is still reported.

## The mirror self-check

`ac3/verify/mirror.hpp` and `ac3/verify/eac3_mirror.hpp`. Both decoders can record what they
derived from the wire, so it can be diffed against the encoder's own model of the same frame —
per block, per coded stream, and for E-AC-3 per substream. It exists because a desync is
invisible at the field that causes it: every mantissa's *width* comes out of that model, so the
moment the two sides disagree each goes on reading confidently at its own idea of where it is,
and the failure surfaces some blocks later as whatever §7.10.2 guard the misaligned bits happen
to trip first.

```cpp
// The driver most callers want: a drop-in for eac3::AccessUnitEncoder that also
// decodes every access unit it emits and diffs the two models.
ac3::verify::Eac3MirrorEncoder encoder{config};
const auto checked = encoder.encode_access_unit(channels);
if (checked && !checked->ok()) {
    std::puts(encoder.last_report().c_str());
    // "frame 12 substream 1 block 3 channel 1: bap[87] encoder=5 decoder=4"
}
```

`ac3::verify::MirrorEncoder` is the AC-3 sibling, over `FrameEncoder`. What each compares is in
its own header; the E-AC-3 side adds what Annex E adds — per-substream and per-block bit offsets
across an independent substream and its dependents, AHT gain mode and per-bin gains, and the
coupling, enhanced-coupling and spectral-extension coordinates. `ac3cli eac3-encode … verify`
is the same check over a whole file.

Both trace pointers are null by default and cost one branch per block when they are: attaching
one never changes what the encoder emits, which is what makes a checked build the same encoder
as the shipped one. The decoder fills its side **incrementally**, so a frame it ends up refusing
still leaves behind everything it read before the refusal — the case the comparison is most
useful in, since the mismatch typically names an earlier block than the refusal does.

Transient pre-noise processing's hold-back (above) is invisible to the check: the trace is
written while a frame is *parsed*, not when its audio is released, so a held-back frame is
compared in the call that decoded it like any other.

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

`split_access_units` is the E-AC-3 sibling for `Eac3Decoder::decode_access_unit`, delimiting by
independent-substream boundaries the same way. Losing an entire access unit (rather than one
frame within it) is not recoverable the same way — `decode_access_unit` needs every substream
of a unit in the one call — so a transport that can drop whole units needs its own
redundancy/retransmission above this layer; this API only guarantees that *finding* the next
good boundary never depends on the previous one having decoded cleanly.

## Latency, from the decoder's side

The chain's whole budget is on [the AC-3 encoding page](encoding-ac3.md#latency). What matters
here is the part a decoder controls, which is smaller than it looks:

| | Adds |
|---|---|
| `ac3::FrameDecoder::latency_samples()` | **0**, always. |
| `ac3::Eac3Decoder::latency_samples()` | **0**, or one frame (1536) once §3.7 engages. |

`FrameDecoder`'s zero is structural rather than lucky: `decode_frame` returns a frame's full
1536 samples per channel from the same call that supplies that frame's bytes. The IMDCT overlap
those samples came out of is real, but it is already charged as the chain's transform term — the
samples the decoder hands back are simply 256 samples *older* than the newest input the encoder
had consumed, not samples it is still waiting for.

`Eac3Decoder`'s exception is transient pre-noise processing. A §3.7 correction reaches backwards
across a frame boundary, so the decoder returns frame N−1 from the call that supplies frame N;
`decode_substream`/`decode_access_unit` return `std::nullopt` on the one call where nothing is
ready yet, and `flush()` collects whatever is still pending at end of stream. That is a *release*
delay, not a sample-domain shift — the audio comes out in the same place in the stream, one call
later — so a caller that honours the `std::nullopt` convention and calls `flush()` gets exactly
the same samples in exactly the same order either way.

`latency_samples()` reports what has actually happened so far, so it reads 0 until some
substream's frame sets `transproce`. To size buffers *before* a stream starts, ask the encoder
(`eac3::eac3_latency`), which knows from its configuration whether the tool will ever be used.

### Atmos objects lag the bed

One number an object-aware receiver needs and would not guess: a JOC-reconstructed object
waveform lags its original input by **832** samples, not 256. JOC does not code objects — it
codes a matrix that pulls them back out of the *decoded bed*, and TS 103 420 §7.1 puts that
reconstruction in a 64-band complex QMF filterbank rather than the MDCT domain — a critically
sampled real transform relies on time-domain alias cancellation between neighbouring blocks, an
assumption a per-frame matrix breaks (see `ac3/dsp/qmf.hpp`). Analysis plus synthesis costs the
filterbank's own `dsp::kQmfDelay` (576 samples) on top of the bed's 256, for 832 total. The bed
in the same `DecodedAccessUnit` still lags by 256, so **objects and bed are not aligned with each
other** — anything mixing the two has to delay the bed by 576 samples. `oba::AtmosEncoder::latency()`
reports the object path's budget and `bed_latency()` the bed's; the 832 is measured end to end in
[`tests/decoder/test_latency.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/tests/decoder/test_latency.cpp).

## Decoding bytes you do not control

If the stream comes from the network or from a user, read
[Threat model](../threat-model.md) before wiring this up. In short:

- `scan`, `split_frames` and `split_access_units` take the **whole** stream as one span and
  return one span per access unit, so peak memory is O(input) and the library imposes no upper
  bound. Bound the input yourself, or drive `decode_frame`/`decode_substream` one unit at a time.
- Every per-access-unit allocation is bounded by a bitstream field of fixed width — a single
  frame cannot be made to consume an unbounded amount of memory or time — and a hostile `frmsiz`
  is refused (`kTruncated` if it overruns the input, `kInvalidStream` if it is shorter than the
  header it was read from) rather than becoming a short span something reads past.
- A refused frame produces no audio, but it is not a rollback: the decoder's overlap-add, dither
  and JOC state have advanced as far as the parse got, and an `_into` form's spans are left
  unspecified. Construct a fresh decoder if you need a clean state after an error.
- The `_into` forms check their span sizes with `assert`, so a release build does not check them
  at all.

## Testing a decoder of your own

The [conformance vector set](../conformance-vectors.md) published with each release is the
inverse of this page: streams this project produces, with the PCM they were encoded from and a
manifest of what each exercises, for checking an independent implementation.

---

See also: [Encoding AC-3](encoding-ac3.md) and [Encoding E-AC-3](encoding-eac3.md) — what
`decode_frame`/`decode_access_unit` are undoing, and the full latency budget;
[Muxing & sinks](muxing-and-sinks.md) — pairing `ac3::io::scan` with `matroska::mux` is what
keeps a container's track header honest; [Building](../building.md) — the minimum-footprint
decoder profile for set-top and DSP targets.
