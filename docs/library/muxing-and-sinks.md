# Muxing & sinks

## Muxing: `matroska::mux`

`matroska/matroska.hpp`, library `matroska::matroska`. It links nothing from `ac3::forge` and
takes frames as opaque bytes. Pairing it with `ac3::io::scan` is what keeps the track header
honest.

```cpp
// One Matroska frame per access unit. For E-AC-3 an access unit is the
// independent substream plus its dependents, which is exactly what scan
// groups — a player must receive them together.
std::vector<std::vector<std::byte>> frames;
for (const auto unit : scanned->access_units) {
    frames.emplace_back(unit.begin(), unit.end());
}

const matroska::AudioTrack track{
    .codec_id = std::string{scanned->kind == ac3::io::StreamKind::kAc3
                                ? matroska::kCodecAc3
                                : matroska::kCodecEac3},
    .sample_rate = ac3::sample_rate_hz(scanned->sample_rate),
    .channels = scanned->channels,
    .samples_per_frame = ac3::kSamplesPerFrame,
};

const auto file = matroska::mux(track, frames);
```

Full program: [`examples/mux_mkv.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/mux_mkv.cpp).

`mux` returns the whole file as bytes and does no file I/O, which keeps it testable without a
disk. It writes one audio track, one SimpleBlock per frame, clusters closed on a time budget,
and Info with TimestampScale and Duration. No SeekHead, no Cues, no chapters, no tags — those
matter for seeking in large files, not for playing back what this project produces.

### Incremental muxing: `matroska::Writer`

Same header. The incremental counterpart to `mux`, for a session whose length is not known up
front — a live capture, where `mux` cannot help: it needs every frame before it can compute
anything. `Writer::create(track, options)` validates the track the same way `mux` does.
`header()` holds the EBML header through Tracks, written exactly once; each `push(frame)`
buffers into the current cluster and returns the bytes of whichever cluster just closed (empty
on most calls); `finalize()` flushes the last partial cluster. Segment is written with EBML's
reserved unknown-size pattern and Duration is omitted — the standard streamed-Matroska shape,
which real players already handle. No more than one cluster's worth of frames is ever held, so
a caller streaming the returned bytes to disk keeps memory bounded for a session of any length.
This is what the GUI's live session records through.

### Demuxing: `matroska::demux`, `matroska::Reader`

`matroska/reader.hpp`, same library. The read side of the two above, and codec-blind in exactly
the same way: it walks EBML, finds a track, and hands each frame back as opaque bytes. The one
place it names a codec is auto-selection, which takes the first audio `TrackEntry` whose
`CodecID` is `A_EAC3` or `A_AC3`; `ReadOptions::track_number` names any other track explicitly
and accepts whatever `CodecID` it carries.

Two shapes, mirroring the write side. `demux` is the batch one, and it is zero-copy — the frames
it returns are spans into the buffer you passed it, the way `ac3::io::scan` already hands back
access units:

```cpp
const auto out = matroska::demux(file_bytes);
if (!out) {
    std::println(stderr, "{}", matroska::describe(out.error()));
    return 1;
}
// out->frames are views into file_bytes, which must outlive them.
const auto scanned = ac3::io::scan(/* the elementary stream you write them to */);
```

`Reader` is the incremental one — `matroska::Writer`'s mirror image, for a file too big to hold.
Frames arrive through a callback rather than a return value, so nothing accumulates: peak memory
is one chunk plus one frame, never the file.

```cpp
matroska::Reader reader{};
const auto on_frame = [&](std::span<const std::byte> frame) { sink.push(frame); };
for (auto chunk = read_next_chunk(); !chunk.empty(); chunk = read_next_chunk()) {
    if (!reader.push(chunk, on_frame)) { /* ... */ }
}
if (!reader.finish(on_frame)) { /* ... */ }
```

The span handed to the callback is valid for that call only — it points into the reader's own
buffer, which the next `push` reuses. Copy it there if you need to keep it. This is what
`ac3cli demux` runs on, which is why a multi-gigabyte rip never lands in memory.

What it reads beyond what this project writes, because a file from a disc rip or another muxer
has it: all three lacing forms (Xiph, EBML, fixed-size), `BlockGroup`-wrapped `Block`s as well
as `SimpleBlock`, several tracks, 32-bit as well as 64-bit `SamplingFrequency`, and clusters
left at EBML's unknown size rather than only the Segment. A file truncated mid-cluster returns
every whole frame before the cut rather than an error — that is how a live recording ends.

**Untrusted input.** Every length in an EBML file is self-declared, and a container arrives from
a rip, a capture or a download rather than from this project's own writer. `ReadOptions` bounds
the element size the reader will hold (16 MiB by default; anything larger that it does not need
is skipped without ever being buffered), the frames one laced block may carry, the number of
`TrackEntry` elements, and how deep masters may nest — the walker is iterative, so nothing an
input declares can exhaust the call stack. `fuzz/fuzz_matroska_demux.cpp` drives both entry
points with arbitrary bytes under ASan/UBSan.

## Muxing: `mp4::mux`

`mp4/mp4.hpp`, library `mp4::mp4`. Same shape as `matroska::matroska`: it links nothing from
`ac3::forge` and takes frames as opaque bytes. The one place MP4 needs codec-specific bytes that
Matroska's plain CodecID string does not is the sample entry's `dac3`/`dec3` configuration box
(ETSI TS 102 366 Annex F) — so `mp4::AudioTrack::codec_config` carries that box's payload as
opaque bytes too, built by `ac3::io::build_codec_config_box` (`ac3/io/dec3.hpp`) straight off
whatever `ac3::io::scan` read out of the bitstream, fscod/bsid/bsmod/acmod/lfeon and, when the
stream carries Dolby Atmos objects, the `flag_ec3_extension_type_a`/`complexity_index_type_a`
extension (TS 103 420 §8.3.1/§8.3.2.2) alike.

```cpp
// One MP4 sample per access unit. For E-AC-3 an access unit is the
// independent substream plus its dependents, which is exactly what scan
// groups — a player must receive them together.
std::vector<std::vector<std::byte>> frames;
frames.reserve(scanned->access_units.size());
for (const auto unit : scanned->access_units) {
    frames.emplace_back(unit.begin(), unit.end());
}

const mp4::AudioTrack track{
    .codec_id = std::string{scanned->kind == ac3::io::StreamKind::kAc3 ? mp4::kCodecAc3
                                                                        : mp4::kCodecEac3},
    .sample_rate = ac3::sample_rate_hz(scanned->sample_rate),
    .channels = scanned->channels,
    .samples_per_frame = ac3::kSamplesPerFrame,
    // The dac3/dec3 sample-entry box, built from the same scan result -
    // see ac3/io/dec3.hpp for why this lives in ac3::io rather than in
    // mp4::mp4 itself.
    .codec_config = ac3::io::build_codec_config_box(*scanned),
};

const auto file = mp4::mux(track, frames);
```

Full program: [`examples/mux_mp4.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/mux_mp4.cpp).

`mux` returns the whole file as bytes and does no file I/O, the same as `matroska::mux`. It
writes `ftyp`/`moov`/`mdat` for one audio track, one sample per chunk, `stts`/`stsz`/`stco` built
straight off the frame sizes handed in. No edit lists, no multiple tracks — those matter for
large-file seeking and multi-track muxing, not for playing back what this project produces.

Getting the `dec3`/`dac3` box right from the spec is the point: FFmpeg's MKV→MP4 remux path used
to silently drop or mis-signal the Atmos extension
([jellyfin-ffmpeg#584](https://github.com/jellyfin/jellyfin-ffmpeg/issues/584), upstream
[FFmpeg trac #9996](https://trac.ffmpeg.org/ticket/9996), since fixed) — building it from
`ac3::io::scan`'s own read of the bitstream, rather than by copying another tool's output, is
what this module avoided that bug by construction rather than by patching it after the fact, and
still does for any FFmpeg build older than the fix.

### Demuxing: `mp4::demux`, `mp4::Reader`

`mp4/reader.hpp`, same library. The read side of both writers above, and the same shape the
Matroska reader has: `demux` is batch and zero-copy (samples are spans into your buffer),
`Reader` is incremental (samples arrive through a callback, peak memory is one chunk plus one
sample).

It reads both layouts, from either writer and from a real muxer: a plain `moov`/`mdat` file,
walking `stsc`/`stsz`(or `stz2`)/`stco`(or `co64`) to turn the sample table into byte ranges, and
a fragmented one, taking `mvex`/`trex`'s defaults plus every `moof`/`traf`/`tfhd`/`trun` that
follows. A 64-bit `largesize` box header and an `mdat` declared to run to end-of-file both read
normally, though neither writer here emits them.

**`moov` before `mdat`.** `demux` can reach any offset, so it reads a file whose sample table sits
either side of the media data. `Reader` cannot — locating a sample means seeking backwards, and a
stream has nowhere to go back to — so a `moov`-last file reports `kMoovAfterMdat` rather than
silently returning nothing. That is the layout a muxer leaves behind when it never rewrote the
file for "faststart"; `mux()` and `fragment()` both write `moov` first, as does any web-optimised
file.

**The `dec3`/`dac3` box comes back parsed.** `ReadTrack::codec_config` is a `CodecConfig`, the read
twin of [`ac3::io::build_codec_config_box`](#muxing-mp4mux): `fscod`, `bsid`, `bsmod`, `acmod`,
`lfeon`, `bit_rate_code` or `data_rate_kbps`, `num_ind_sub`/`num_dep_sub`/`chan_loc`, and —
crucially — TS 103 420's `flag_ec3_extension_type_a`/`complexity_index_type_a` as an
`optional<int>`. That last field is the Atmos/JOC marker an FFmpeg remux is known to drop, and
reading it back is what makes the repair case possible: demux a file, keep the complexity index,
re-mux it with the signalling intact. The values are reported as raw syntax numbers rather than
`ac3::` enums, because this module has no dependency on the codec library and no business
deciding what `fscod` 0 means. `payload` keeps the bytes verbatim, so a caller remuxing into
another container can hand them straight back.

A `dec3` box that stops before the Atmos extension leaves `oba_complexity_index` empty rather
than reporting a confident zero — the extension is a trailing addition, and a box written before
TS 103 420 simply has nothing to say about it.

**Untrusted input.** An MP4's sample table is an *index*, which is a wider attack surface than
Matroska's in-line framing: `stsc` names chunks, `stco` names absolute file offsets and `stsz`
names sizes, all self-declared and all resolved against each other before a byte of audio is
touched. `ReadOptions` bounds the box size the reader will hold, the sample and chunk counts
(`max_samples` defaults to about 35 hours of access units), and the nesting depth; the walk is
iterative. A chunk offset pointing past the end of the file drops that sample rather than
failing the file — a truncated download is ordinary, and the samples that *are* present are all
real. `fuzz/fuzz_mp4_demux.cpp` drives both entry points with arbitrary bytes.

## Muxing: `mpegts::mux`

`mpegts/mpegts.hpp`, library `mpegts::mpegts`. Same shape as `matroska::mux` above — it links
nothing from `ac3::forge` beyond the AC-3/E-AC-3 choice it is told, and takes access units as
opaque bytes.

```cpp
// One PES-wrapped access unit per TS access unit. For E-AC-3 an access
// unit is the independent substream plus its dependents, which is
// exactly what scan groups — a player must receive them together.
std::vector<std::vector<std::byte>> frames;
frames.reserve(scanned->access_units.size());
for (const auto unit : scanned->access_units) {
    frames.emplace_back(unit.begin(), unit.end());
}

const mpegts::AudioTrack track{
    .codec = scanned->kind == ac3::io::StreamKind::kAc3 ? mpegts::AudioCodec::kAc3
                                                         : mpegts::AudioCodec::kEac3,
    .sample_rate = ac3::sample_rate_hz(scanned->sample_rate),
    .channels = scanned->channels,
    .samples_per_frame = ac3::kSamplesPerFrame,
    // What the PMT descriptor says about the service. Every field is a plain
    // A/52 value ac3::io::scan already read off the bitstream.
    .service = {.bsmod = scanned->bsmod,
                .bsmod_present = scanned->bsmod_present,
                .acmod = static_cast<int>(scanned->acmod),
                .lfe = scanned->lfe,
                .channels = scanned->channels,
                .bsid = scanned->bsid,
                .dsurmod = scanned->dsurmod,
                .bit_rate_code = scanned->bit_rate_code,
                .sample_rate_code = static_cast<int>(scanned->sample_rate),
                .mix_metadata = scanned->mix_metadata,
                .independent_substreams = scanned->independent_substreams},
};

const auto file = mpegts::mux(track, frames,
                              {.profile = mpegts::BroadcastProfile::kAtsc});
```

Full program: [`examples/mux_ts.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/mux_ts.cpp).

`mux` returns the whole 188-byte-aligned Transport Stream as bytes, no file I/O, same testability
reasoning as `matroska::mux`. It writes a single program — one PAT, one PMT (repeated
periodically so a receiver tuning in mid-stream doesn't wait for byte zero), and one PES-wrapped
elementary stream carrying PCR every access unit. No video, no other elementary streams, no PID
remapping: a general-purpose multiplexer is out of scope, this is enough for a player or
`ffprobe` to recognize one AC-3/E-AC-3 programme.

**Broadcast profile.** Two standards register AC-3/E-AC-3 for MPEG-TS carriage — ATSC and DVB —
with different, non-interoperable signalling, so a stream is written to satisfy one of them,
never a bit of each. `MuxOptions::profile` picks which:

| | `kDvb` (default) | `kAtsc` |
|---|---|---|
| AC-3 `stream_type` | `0x06`, PES private data | `0x81` (A/52 Annex A §A4.1) |
| E-AC-3 `stream_type` | `0x06` | `0x87` (A/52 Annex G §G3.1) |
| AC-3 descriptor | `AC3_descriptor`, tag `0x6A` (EN 300 468 Table D.6) | `AC-3_audio_stream_descriptor`, tag `0x81` (A/52 Table A4.1) |
| E-AC-3 descriptor | `enhanced_AC-3_descriptor`, tag `0x7A` (Table D.7) | `E-AC-3_audio_descriptor`, tag `0xCC` (A/52 Table G.1) |
| What identifies the stream | the descriptor tag — DVB registers no `stream_type` of its own | the `stream_type` — ATSC treats the descriptor as configuration detail |

Both descriptors describe the same service in different bit layouts, so a caller supplies the
underlying A/52 field values once, as `mpegts::ServiceInfo`, and the module maps them onto
whichever registry's tables the profile calls for — EN 300 468 Tables D.1–D.8, A/52 Tables
A4.2–A4.6 and G.2–G.6. That mapping is descriptor syntax, which is this module's job; reading
those values off the bitstream is `ac3::io::scan`'s, which is why `ServiceInfo` is plain
integers and `mpegts::` still links nothing from `ac3::forge`.

`ac3::io::ScannedStream` supplies every one of them: `bsmod` (with `bsmod_present`, since
Annex E only carries it inside `infomdate`), `acmod`, `lfe`, the rendered `channels`, `bsid`,
`dsurmod`, `bit_rate_code`, `mix_metadata` for `mixinfoexists`, and `independent_substreams`
with `associated_substreams` for the `substream1`–`3` fields. Two values are *not* in any
bitstream, because they describe how services in a multiplex relate rather than what one stream
contains — `mainid` and `asvc` — and those stay unset unless the caller supplies them
(`ac3cli ts ... mainid=3`, `asvc=0x0A`). An unset optional field is omitted rather than
zero-filled: a receiver already handles an absent one, where an invented main-service number
links the wrong services.

Two places where the standards' own tables cannot express something this project can read, and
the field is omitted rather than approximated: A/52 Table G.5 reserves complete-main and
emergency as *substream* service types, and Table G.6 reserves 1+1 as a substream channel mode,
so an ATSC `substream1`–`3` field for such a substream is left out with its flag clear.

### Demuxing: `mpegts::demux`, `mpegts::Reader`

`mpegts/reader.hpp`, same library. The read side of `mpegts::mux`/`Writer`, codec-blind in the
same sense: it locks to the packet grid, follows PAT to PMT to an elementary PID, reassembles
PES, and hands the payloads back as opaque bytes.

**What comes back is not the same shape as the sibling readers.** A Matroska `SimpleBlock` and an
MP4 sample each hold exactly one access unit, so `matroska::demux`/`mp4::demux` hand back access
units. A PES packet makes no such promise — it may carry one, several, or (with the unbounded
`PES_packet_length` form broadcast uses) a run ending only when the next one starts. So this
reader hands back **PES payloads**, and what they concatenate to is the elementary stream:

```cpp
const auto out = mpegts::demux(file_bytes);
if (!out) {
    fmt::println(stderr, "{}", mpegts::describe(out.error()));
    return 1;
}
std::vector<std::byte> elementary_stream;
for (const auto& payload : out->payloads) {
    elementary_stream.insert(elementary_stream.end(), payload.begin(), payload.end());
}
const auto scanned = ac3::io::scan(elementary_stream);
```

This is exactly what `ac3::io::scan` wants, and re-framing PES payloads into access units is its
job, not this module's — doing it here would mean this container-blind module knowing what an
AC-3 syncframe is.

**All three signalling forms**, one more than the writer. `mux` chooses between DVB and ATSC
through `MuxOptions::profile` (see above), and commits to one of them wholly. A reader has no
such luxury: a third family of files names the codec through neither, using a
`registration_descriptor`'s `'AC-3'`/`'EAC3'` `format_identifier` instead. All three are
recognised on read, reported as `ReadStream::signalling` (`CodecSignalling::kAtscStreamType` /
`kDvbDescriptor` / `kRegistrationDescriptor`) so a caller remuxing back out knows which it was.

**Three packet grids**, detected rather than assumed: 188 bytes (ISO/IEC 13818-1's own), 192
(M2TS — a Blu-ray/AVCHD rip, each packet prefixed by a 4-byte arrival timestamp), and 204 (a
capture that kept its Reed-Solomon parity). The grid is found by where the `0x47` sync byte
repeats at a consistent stride, several packets in a row — a stray `0x47` in payload cannot fake
that — which also means a capture that starts mid-packet (the normal way a transport stream is
acquired: wherever the tuner happened to be) still locks on.

```cpp
mpegts::Reader reader{};
const auto on_payload = [&](std::span<const std::byte> payload) {
    elementary_stream.insert(elementary_stream.end(), payload.begin(), payload.end());
};
for (auto chunk = read_next_chunk(); !chunk.empty(); chunk = read_next_chunk()) {
    if (!reader.push(chunk, on_payload)) { /* ... */ }
}
if (!reader.finish(on_payload)) { /* ... */ }
```

`Reader::finish` takes the callback — unlike the Matroska and MP4 readers' — because it can
genuinely still emit: the unbounded PES form ends only at the next
`payload_unit_start_indicator` or at end of input, so the last payload of a capture is only
complete here.

**Untrusted input, and more so than the sibling formats.** A transport stream is designed to be
tuned into mid-flight and to survive bit errors, so "malformed" is the ordinary case here, not
the exceptional one. Every PSI section's CRC-32 (the non-reflected CRC-32/MPEG-2 variant,
self-checked against the standard test vector) is verified before the PAT/PMT it carries is
believed — a bit-damaged PMT is thrown away rather than locking onto a wrong PID for the rest of
the file. `ReadOptions` bounds the PES and PSI section sizes the reader will assemble (the
unbounded PES form has no ceiling of its own otherwise) and how far it will search for the packet
grid. `fuzz/fuzz_mpegts_demux.cpp` drives both entry points with arbitrary bytes — this is also
the container reader most likely to find a genuine hang rather than a crash, since the sync
search, section reassembly and PES reassembly are all loops a hostile stream can try to stall.

## Fragmented MP4/CMAF + HLS/DASH: `mp4::fragment`, `mp4/hls.hpp`, `mp4/dash.hpp`

ROADMAP.md's A2, the streaming-delivery follow-up `mp4::mux`'s own header deliberately left for
later: `mp4::fragment` lays out the same track and frames as `mux`, but as a fragmented movie
(ISO/IEC 14496-12 §8.8's `moof`/`mfhd`/`traf`/`tfhd`/`tfdt`/`trun`) split into CMAF-shaped pieces
(ISO/IEC 23000-19) — an initialization segment (`ftyp`+`moov`, whose one `trak` carries
`mvex`/`trex` instead of a populated sample table, since a fragmented track's own `stbl`
describes zero samples) plus one or more media segments (`styp`+`moof`+`mdat`, one per fragment).
Same batch shape as `mux`: every frame is known up front, so real durations/timestamps are
filled in throughout, including the track's total duration in `mvhd`/`tkhd`/`mdhd`.
[`mp4::FragmentWriter`](#incremental-fragmenting-mp4fragmentwriter) below is the incremental form
for a live session, and that total duration is the one thing the two disagree about.

```cpp
const auto fragmented =
    mp4::fragment(track, frames, mp4::FragmentOptions{.frames_per_fragment = 8});
```

`FragmentedOutput::init_segment` and `::media_segments` are exactly the files a packager or CDN
origin wants (`init.mp4` plus `segment1.m4s`, `segment2.m4s`, ...) — see `ac3cli fmp4`, which
writes them out that way alongside the manifests below.

`mp4/hls.hpp` and `mp4/dash.hpp` build HLS/DASH signaling for those same segments — one CMAF
segment format, two manifest flavors, the entire point of CMAF:

```cpp
const auto media_playlist =
    mp4::build_hls_media_playlist(track, fragmented->media_segments, mp4::HlsOptions{});
const auto master_playlist = mp4::build_hls_master_playlist(
    track, fragmented->media_segments, "audio.m3u8", mp4::HlsOptions{});
const auto dash_snippet = mp4::build_dash_adaptation_set(track, fragmented->media_segments);
const auto mpd = mp4::build_dash_mpd(track, fragmented->media_segments, dash_snippet);
```

A master playlist can carry more than one audio rendition in the same `#EXT-X-MEDIA` group,
which is what an Atmos asset needs (see the paired-rendition note below):

```cpp
const std::array<mp4::HlsRendition, 2> renditions{
    mp4::HlsRendition{.track = joc_track,
                      .segments = joc.media_segments,
                      .media_playlist_uri = "audio.m3u8",
                      .name = "Dolby Atmos",
                      .channels_attribute = "12/JOC",
                      .is_default = true},
    mp4::HlsRendition{.track = bed_track,
                      .segments = bed.media_segments,
                      .media_playlist_uri = "bed51/audio.m3u8",
                      .name = "5.1"}};
const auto master_playlist = mp4::build_hls_master_playlist(renditions);
```

Full program: [`examples/mux_fmp4.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/mux_fmp4.cpp).

Both manifest flavors get the `CODECS`/`codecs` attribute right: `mp4::hls_codec_string` (and
`build_dash_adaptation_set` internally) use the bare `ac-3`/`ec-3` sample-entry fourcc unmodified
as the RFC 6381 `'Codecs'` parameter — neither AC-3 nor E-AC-3 registers any of the
dot-separated profile/level fields RFC 6381 §3 makes room for (unlike e.g. `avc1.640028`), which
is confirmed against every real HLS manifest example
[Apple's HLS Authoring Specification for Apple
Devices](https://developer.apple.com/documentation/http-live-streaming/hls-authoring-specification-for-apple-devices)
shows. Dolby Digital Plus with Atmos objects additionally needs `CHANNELS="<N>/JOC"` on the HLS
media rendition instead of a plain channel count, where N is the decodable object count
(`ac3::io::ScannedStream::oba_complexity_index`, TS 103 420 §8.3.2's `complexity_index_type_a`)
— reiterated, with a worked example (`CHANNELS="12/JOC"`), by [Dolby's own Online Delivery Kit
documentation](https://ott.dolby.com/OnDelKits/DDP/Dolby_Digital_Plus_Online_Delivery_Kit_v1.5/Documentation/Content_Creation/SDM/help_files/topics/hls_c_hls_signal_atmos_ddp.html)
and shown verbatim in a real manifest (`CODECS="avc1.64001f,ec-3"` / `CHANNELS="12/JOC"`) by
[AWS MediaLive's own HLS+Atmos
documentation](https://docs.aws.amazon.com/medialive/latest/ug/feature-dolbyatmos.html). `mp4::`
itself never reads that TS 103 420 object-layer syntax — `HlsOptions::channels_attribute` is
opaque to it, the same way `AudioTrack::codec_config` is; `ac3cli fmp4` is the caller that
already has `oba_complexity_index` (it read it to build the `dec3` box) and supplies the string.

**The paired 5.1 rendition.** Apple's authoring specification also asks that an Atmos rendition
be accompanied by an equivalent 5.1 bitstream carrying `CHANNELS="6"` *in the same
`#EXT-X-MEDIA` group*, so a client that cannot render the object layer selects the bed rather
than the asset failing to play. Because JOC's bed already *is* the full mix, that companion
needs no re-encode: [`ac3::io::strip_objects`](decoding.md#object-layer-strip) removes the
object layer from the same stream and leaves bit-identical bed audio. `ac3cli fmp4 …
fallback-51` writes both — the Atmos rendition where it always was, the stripped one under
`bed51/`, and one master playlist listing both.

The DASH snippet describes exact per-segment durations with a `SegmentTemplate`/`SegmentTimeline`
(ISO/IEC 23009-1 §5.3.9.6) built from each segment's own duration, rather than one nominal
`duration` attribute assumed constant — segments are constant-duration except (as usual) a
possibly shorter final one, and a flat nominal duration is exactly what let a real player
(FFmpeg's own `dash` demuxer, while writing this module) compute one too many segments from
`mediaPresentationDuration` and request a segment number past the end.

### Atmos/JOC signalling: `ceao`, and the DASH descriptors

`mp4/dash.hpp` used to say there was no established DASH convention to point at for JOC, unlike
HLS's `CHANNELS="<N>/JOC"`. There is: DASH-IF IOP Part 8 v5.0.0 §5.3.2 names, for E-AC-3
carrying JOC, the two SupplementalProperty descriptors
[ETSI TS 103 420](https://www.etsi.org/deliver/etsi_ts/103400_103499/103420/01.02.01_60/ts_103420v010201p.pdf)
clause D.2 defines — `tag:dolby.com,2018:dash:EC3_ExtensionType:2018`, whose value "shall be the
three character string JOC" (§D.2.2.1), and
`tag:dolby.com,2018:dash:EC3_ExtensionComplexityIndex:2018`, whose value "shall be decimal
representation of the eight-bit element `complexity_index_type_a` in the EC3SpecificBox"
(§D.2.2.2). §5.3.3 adds that such a track "shall be constrained according to the CMAF specific
requirements as provided in ETSI TS 103 420 Annex E", where §E.5 requires the `ceao` compatibility
brand. `DashOptions::joc_complexity_index` writes the first pair;
`FragmentOptions::object_audio_brand` adds `ceao` to the `ftyp` and every `styp` alongside the
`iso6`/`cmfc` a fragmented CMAF track already declares (added, not substituted — §E.2 requires
ISO/IEC 23000-19 conformance on top of the profile).

The same §5.3.2 offers two AudioChannelConfiguration schemes for E-AC-3. With
`DashOptions::dolby_channel_configuration` empty, the Representation carries
`urn:mpeg:mpegB:cicp:ChannelConfiguration` with the track's channel count — what TS 103 420
§D.2.3's own example MPD writes. Set it to the four hex digits TS 102 366 clause I.1.2.1 defines
(the 16-bit channel-assignment word, left channel in the most significant bit, so 5.1 is `F801`)
and it carries the Dolby scheme instead. `ac3::io::dash_channel_configuration` is the one place
that word is derived, beside `build_codec_config_box` and for the same reason: which locations a
stream carries is `acmod`/`lfeon`/`chanmap` syntax, and a manifest writer has no business
re-deriving AC-3 semantics. `ac3cli fmp4`, the GUI and the live paths all supply it.

`FragmentOptions::object_audio_brand` and `DashOptions::joc_complexity_index` are caller-supplied
for the same reason `HlsOptions::channels_attribute` is — `mp4::` never reads TS 103 420's object
layer, and the caller that scanned `oba_complexity_index` off the bitstream to build the `dec3`
box already has it.

### Incremental fragmenting: `mp4::FragmentWriter`

Same header as `fragment`. The live counterpart, and `matroska::Writer`/`mpegts::Writer`'s
sibling: `create(track, options)` validates exactly what `fragment` validates and leaves
`init_segment()` ready to write once; each `push(frame)` buffers into the current fragment and
returns the media segment that just *closed* (so one comes back every
`frames_per_fragment`-th call, `std::nullopt` otherwise); `finalize()` flushes the trailing
partial fragment. `tfdt` comes from a running decode time held on the writer, which is the only
per-fragment state `fragment`'s own loop carries. Nothing beyond one fragment's frames and the
playlist window is ever held.

**The contract is byte-equality with the batch form**, the same one `mpegts::Writer` holds itself
to: for the same track, options and frames, the media segments this hands back are byte for byte
the ones `fragment` would have built. The initialization segment differs in exactly one respect —
`mvhd`/`tkhd`/`mdhd` carry duration 0, since a live session does not know its total (ISO/IEC
14496-12 §8.8.2 provides `mehd` for the fragmented movie that *does*). That is the same
concession `matroska::Writer` makes with EBML's unknown-size Segment and its omitted Duration.
Both halves are asserted in `tests/containers/test_fmp4.cpp`, the init segment by patching the
three duration fields back and then requiring full byte equality.

```cpp
auto writer = mp4::FragmentWriter::create(
    track, mp4::FragmentOptions{.playlist_window_segments = 20});
write("init.mp4", writer->init_segment());
for (const auto& frame : frames) {
    const auto closed = writer->push(frame);          // std::optional<MediaSegment>
    if (*closed) {
        write(std::format("segment{}.m4s", (*closed)->sequence_number), (*closed)->bytes);
        // Rebuild the manifests from the rolling window each time a segment closes.
        write("audio.m3u8", mp4::build_hls_media_playlist(track, writer->window(),
                                                          mp4::HlsOptions{.vod = false}));
        write("manifest.mpd",
              mp4::build_dash_mpd(track, writer->window(),
                                  mp4::build_dash_adaptation_set(track, writer->window()),
                                  mp4::MpdOptions{.is_static = false,
                                                  .availability_start_time = now_iso8601()}));
    }
}
```

`window()` hands back `SegmentInfo` — a `MediaSegment`'s bookkeeping without its bytes, so a
rolling window of hundreds of segments costs nothing to keep.
`FragmentOptions::playlist_window_segments` bounds it (0, the default, keeps every segment). All
three manifest builders take `SegmentInfo` spans, with `MediaSegment` overloads for batch
callers.

Live manifests differ from VOD ones only in what they omit and where they start.
`HlsOptions::vod = false` drops `#EXT-X-PLAYLIST-TYPE:VOD` and `#EXT-X-ENDLIST`, leaving
`#EXT-X-MEDIA-SEQUENCE` — always the first *listed* segment's number — to tell a player that
segments have rolled off the front (RFC 8216 §6.2.2). On the DASH side `MpdOptions::is_static =
false` writes `type="dynamic"` with `availabilityStartTime`, `minimumUpdatePeriod` and
`timeShiftBufferDepth` and no `mediaPresentationDuration` — the attribute set TS 103 420 §D.2.3's
own example MPD carries — and the SegmentTemplate's `@startNumber` and the SegmentTimeline's
first `<S t="…">` both come from the window rather than being assumed to be the start of the
track. `mp4::` has no clock (no file I/O, no time), so the caller supplies the timestamp strings;
that is also what keeps the manifests deterministic under test.

This is what `ac3cli record`/`ac3cli live` with `container=fmp4` and the GUI's live session with
**fragmented MP4/CMAF** selected write through: the directory is a servable live origin while the
session runs, and a closed VOD one afterwards.

### External validation

`mp4::fragment`'s ISOBMFF output and the HLS media playlist round-trip cleanly through FFmpeg's
own strict decode (`ffmpeg -v error -xerror -err_detect crccheck+bitstream+buffer+explode`) —
both the fragmented file (init segment concatenated with every media segment) and `audio.m3u8`
read back the exact original frame count and duration. The same holds for what `FragmentWriter`
streams, and for the DASH MPD: see [Validation](../verification.md#where-the-oracles-dont-reach)
for exactly what was and was not checked externally.

## Muxer errors

Every muxing entry point above returns `std::expected`, against a per-module `MuxError` enum
with its own `describe()`:

| Enum | Values |
|---|---|
| `matroska::MuxError` | `kNoFrames`; `kInvalidTrack` (zero/negative channels or sample rate, or an empty codec id); `kFrameTooLarge` (a single frame beyond what one SimpleBlock can carry). |
| `mp4::MuxError` | `kNoFrames`; `kInvalidTrack` (here: an unrecognised codec id — only `ac-3`/`ec-3` are legal — or no `codec_config` payload, besides the zero-channel/rate cases); `kFileTooLarge` — `mdat` would need a 64-bit chunk offset (`co64`), which this module doesn't write, so whole-file offsets are 32-bit; `kInvalidOptions` (e.g. `FragmentOptions::frames_per_fragment == 0`). `mp4::FragmentWriter::create` returns the same two refusals as `fragment`, but never `kNoFrames`: a live writer stopped before its first frame simply has nothing to flush. |
| `mpegts::MuxError` | `kNoFrames` and `kInvalidTrack` as above; `kInvalidOptions` (PID collisions); `kFrameTooLarge` — one access unit too large for a PES packet's 16-bit length field. |

## Demuxer errors

Each module's `demux`/`Reader` returns `std::expected` against its own `DemuxError`, which has
its own `describe()` overload beside `MuxError`'s:

| Enum | Values |
|---|---|
| `mp4::DemuxError` | `kNotIsobmff`; `kTruncated`; `kMalformed` (a box, sample table or fragment layout that cannot be parsed); `kNoAudioTrack`; `kLimitExceeded`; `kMoovAfterMdat` (`Reader` only — the sample table follows the data it indexes; use `demux`). |
| `matroska::DemuxError` | `kNotMatroska` (no EBML header where one has to be); `kTruncated` (the input ends before any track was described — a cut *after* one is not an error, see above); `kMalformed` (a vint, element or block layout that cannot be parsed, including a lace whose declared sizes overrun its block); `kNoAudioTrack` (Tracks held nothing selectable, or the requested `track_number` is absent); `kLimitExceeded` (an element size or nesting depth beyond `ReadOptions`). |
| `mpegts::DemuxError` | `kNotTransportStream` (no 188/192/204-byte sync grid found within `ReadOptions::max_sync_search_bytes`); `kNoProgramme` (no PAT, or no PMT for the programme it named — including one whose CRC failed); `kNoAudioStream` (the PMT held no AC-3/E-AC-3 elementary stream under any of the three signalling forms); `kMalformed` (a PES or section layout that cannot be parsed); `kLimitExceeded` (a PES packet or PSI section beyond `ReadOptions`). |

## Bitstream sinks (`ac3::audio`)

The pieces below are audio-hardware-facing rather than example-driven, so there's no compiled
`examples/` program to excerpt — this is reference prose pointing at the relevant header, plus
the platform and hardware-verification caveats [Validation](../verification.md) states about
each. All of them are gated by `ac3::audio::audio_backend()`
(`ac3/audio/audio_backend.hpp`), which reports whether capture, monitor playback and
passthrough are available on this build's platform, and why not when they aren't — this backs
the CLI's `UNAVAILABLE HERE` messaging for `devices`, `record`, `monitor`, `live`, `outputs`
and `play`.

### `ac3::iec61937` — S/PDIF burst packing and de-framing

`ac3/iec61937/iec61937.hpp`. Packs AC-3 or E-AC-3 elementary-stream frames into IEC 61937 burst
framing — the wrapper a compressed bitstream needs over PCM-shaped hardware/interfaces (S/PDIF,
HDMI) so a receiver recognizes it as AC-3/E-AC-3 rather than treating it as noisy PCM. AC-3
burst packing is byte-exact against FFmpeg's `spdif` muxer. E-AC-3 packing (`Eac3BurstPacker`)
— data type 0x15, the 24576-byte/4x-carrier-rate burst, multi-syncframe accumulation, `Pd` in
bytes not bits — is independently verified against both FFmpeg's `spdif_header_eac3` and
Microsoft's own IEC 61937 documentation (both fetched live and cross-checked against each
other, not recalled), plus round-trip and real-audio unit tests. This header only produces the
framed bytes; getting them onto real hardware is `PassthroughSink`, below.

**De-framing** (the other direction) is `BurstReader`, `unwrap_stream` and
`PassthroughDetector`. `BurstReader` is a streaming `Pa`/`Pb`/`Pc`/`Pd` parser: data types 0x01
and 0x15, both 16-bit word orders, the stuffing between bursts, `Pd`'s two different units, and
E-AC-3's 4× carrier with its multi-syncframe bursts. Feed it carrier bytes in whatever chunks
the source produces and take elementary-stream bytes out; it holds one burst plus the caller's
chunk and nothing more, so a two-hour capture costs what a two-second one does. `unwrap_stream`
is the batch form, mirroring `wrap_stream`.

The input is by definition untrusted — a burst carrier comes off a wire or out of a capture
device — so nothing taken from `Pd` is believed past its data type's repetition period, and a
preamble not backed by a `0x0B77` syncframe is treated as a false match to resync past rather
than a fatal error. `fuzz/fuzz_iec61937_unwrap.cpp` keeps that honest.

This is also what closes the loop on the wrap side: bursts written by this project *and* by
FFmpeg's `spdif` muxer read back byte-exactly to the streams that went in, AC-3 and E-AC-3,
little-endian and big-endian carriers alike. Backs `ac3cli unspdif`.

`PassthroughDetector` answers the capture-side question — is this endpoint delivering PCM, or
somebody's bursts? — from the same interleaved float frames `ac3::audio::Capture` delivers,
using `carrier_from_capture` to recover the PCM16 words exactly (every backend converts int16
to float by dividing by 32768, so nothing is lost). `ac3cli record` uses it to write the
elementary stream instead of encoding noise; `ac3cli live` uses it to stop rather than encode a
whole session of it.

### `ac3::audio::PassthroughSink` — exclusive-mode passthrough

`ac3/audio/passthrough.hpp`. Exclusive-mode/direct bitstream output, AC-3 or E-AC-3 — WASAPI on
Windows, ALSA on Linux, CoreAudio on macOS — the path an AV receiver needs to see the raw
compressed bitstream rather than decoded PCM.

Stated plainly, because this project's docs don't soften verification gaps: **no desktop
platform's passthrough — AC-3 and E-AC-3 alike — has been confirmed against real bitstreaming
hardware.** The one platform with that confirmation is Android, whose backend has locked a real
AV receiver onto real Atmos output over HDMI — see [Android](../platforms/android.md), and each
platform page for its own status. On [Windows](../platforms/windows.md#audio-backend-wasapi)
specifically: the development machine has no S/PDIF or HDMI endpoint behind a real
Dolby-capable AV receiver, and WASAPI's `IsFormatSupported`
correctly rejects both Dolby IEC 61937 subtypes everywhere it has been tried. What *is* verified
there: the exclusive-mode path itself works (a Realtek endpoint accepts an exclusive-mode PCM
format), and the burst framing it carries is verified as described above under
`ac3::iec61937`. But no bitstream-capable receiver has been confirmed to lock onto output
from this sink specifically — the one receiver-locking check that has been done used a different
code path (bursts played as a PCM16 WAV through a passthrough output), not `PassthroughSink`
itself, and that check has only been tried for AC-3, not E-AC-3.

### `ac3::audio::sink_capabilities` — reading what a sink says it accepts

`ac3/audio/sink_capabilities.hpp` (roadmap UX9). `read_sink_capabilities(device_id)` reads a
render endpoint's own advertised capabilities — CEA-861 Short Audio Descriptors, the part of
EDID (over HDMI) or ELD (ALSA's own EDID-Like Data, which carries the same SADs) that says which
codecs, how many channels and which sample rates a sink accepts — rather than
`enumerate_render_devices()`'s own live-probe answer (open the device and try). `ac3cli play`
uses it, EDID first and the probe as the documented fallback, to decide whether a source format
needs the automatic AC-3/PCM fallback described in
[Commands → Following the sink](../cli/commands.md#following-the-sink).

Real on exactly one backend today: ALSA, reading the HD-audio kernel driver's own
`/proc/asound/<card>/eld#<dev>.<port>` text interface (already decoded from the raw CEA-861
bytes, so there is no byte layout for this project to get wrong — only the driver's own field
names to read). **Not verified against real HDMI/ELD hardware** — the development environment
this shipped from has no Linux box with a bitstream-capable receiver attached; see
[Linux](../platforms/linux.md) for the honest status. Every other backend (Windows, macOS,
Android, PipeWire, and Linux without ALSA) reports `kNoBackend` rather than guessing: none has a
documented user-mode API for reading a sink's raw SADs (Windows' WASAPI and macOS' CoreAudio
both expose negotiated-format questions, the same kind `enumerate_render_devices()` already
answers, not the sink's own raw descriptor; PipeWire's node properties might carry enough to
reach the same ALSA ELD file, but no confirmed, version-stable property name was found to code
against without a live daemon to verify it on).

### `ac3::audio::MonitorSink` — shared-mode monitor playback

`ac3/audio/monitor.hpp`. The non-exclusive counterpart to `PassthroughSink`: shared-mode PCM
playback — WASAPI, ALSA or CoreAudio, resampled and mixed like any other app — that decodes what is being
encoded and plays it back on an ordinary output, for previewing a decode without a
bitstream-capable receiver. Backs `ac3cli monitor` and `live`'s monitor leg.

Unlike passthrough, **this one is confirmed against real hardware.** It has actually played
decoded AC-3 and E-AC-3 (including an Atmos stream's 5.1 bed) through real Windows (Realtek)
hardware in real time, and a live microphone capture → encode → monitor session has run
end-to-end. Building this path against real hardware surfaced two genuine bugs that neither
unit tests nor silent/synthetic input would have caught — see
[Windows](../platforms/windows.md#audio-backend-wasapi) for the details, and
`src/audio/src/backend/windows/monitor.cpp` for the fixes.

## Capture: `ac3::audio`

`ac3/audio/capture.hpp`, `ring_buffer.hpp`. Live input/loopback capture — WASAPI on Windows,
ALSA on Linux, CoreAudio on macOS — through the lock-free SPSC ring in `ring_buffer.hpp`, which
sits between the audio callback and whatever consumes the samples (an encoder, a monitor sink,
or both). On macOS capture is input-only: no loopback endpoint is ever enumerated, and
`start()` refuses `DeviceKind::kLoopback` outright rather than silently opening a microphone.
This is what backs `ac3cli record`/`live` and the GUI's live-session tab.

## Metering: `ac3::analysis`

`ac3/analysis/levels.hpp`. Peak/RMS metering with console ballistics, plus the Gerzon energy
vector computed over the BS.775 ring — the metering `ac3cli` and the GUI share so their two
displays never disagree about what a signal contains. One `LevelMeter` instance drives both: the
moving display (`levels()`, ballistic) and the exact end-of-run report (`summary()`,
unweighted), fed by the same pass over the samples.

```cpp
ac3::analysis::LevelMeter meter{acmod, lfe, 48000};
meter.process(decoded_views);   // once per frame, planar A/52 order
```

```cpp
const auto& stats = meter.summary()[static_cast<std::size_t>(ch)];  // exact, not ballistic
fmt::printf("peak %.1f dBFS  rms %.1f dBFS\n", stats.peak_db(), stats.rms_db());

const auto energy = ac3::analysis::energy_vector(meter.levels(), acmod);
```

Full program: [`examples/level_metering.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/level_metering.cpp)
— decodes a 5.1 stream and reports both the per-channel peak/RMS and the soundfield's energy
vector.

This is a separate concern from the BS.1770 integrated-loudness measurement in
`ac3::meta::LoudnessMeter` (see [Metadata](metadata.md)): one is instantaneous display
metering, the other the gated whole-programme measurement `dialnorm` is derived from.
`energy_vector` is computed from the integrated RMS of the full-bandwidth channels only — the
LFE has no direction to contribute, and a subwoofer's level would otherwise swamp the sum.

---

See also: [Decoding](decoding.md) — `ac3::io::scan` is what feeds both `matroska::mux` and the
sinks above their access units; [Header map](header-map.md) — every header referenced on this
page in one table.
