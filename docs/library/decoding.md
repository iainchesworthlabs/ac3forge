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

`split_access_units` is the E-AC-3 sibling for `Eac3Decoder::decode_access_unit`, delimiting by
independent-substream boundaries the same way. Losing an entire access unit (rather than one
frame within it) is not recoverable the same way — `decode_access_unit` needs every substream
of a unit in the one call — so a transport that can drop whole units needs its own
redundancy/retransmission above this layer; this API only guarantees that *finding* the next
good boundary never depends on the previous one having decoded cleanly.

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
`decode_frame`/`decode_access_unit` are undoing; [Muxing & sinks](muxing-and-sinks.md) — pairing
`ac3::io::scan` with `matroska::mux` is what keeps a container's track header honest.
