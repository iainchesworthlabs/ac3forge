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
