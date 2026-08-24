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

---

See also: [Encoding AC-3](encoding-ac3.md) and [Encoding E-AC-3](encoding-eac3.md) — what
`decode_frame`/`decode_access_unit` are undoing; [Muxing & sinks](muxing-and-sinks.md) — pairing
`ac3::io::scan` with `matroska::mux` is what keeps a container's track header honest.
