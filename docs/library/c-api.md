# C API

A stable, minimal C-callable surface over `ac3::forge`'s encode/decode core —
AC-3, E-AC-3 and Atmos (OAMD + JOC) — and over the AC-4 decoder and encoder ([AC-4](#ac-4)), for
bindings and embedding by callers that cannot or do not want to link C++23. The whole surface is
one header,
[`ac3forge_c/ac3forge.h`](https://github.com/iainchesworthlabs/ac3forge/blob/main/src/capi/include/ac3forge_c/ac3forge.h),
plain C11 with no C++ type crossing it anywhere — only opaque handles and POD structs. It is a
separate library from `ac3::forge`: link `ac3::forge_c` instead, not both.

[`examples/capi_encode_decode.c`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/capi_encode_decode.c)
is a complete, buildable program (compiled as C, not C++, so the build itself proves the header
is C-usable) — the excerpts below are drawn from it. `tests/capi/test_capi.cpp` covers the
rest of the surface, including Atmos encode/decode and the error paths, from Catch2.

```cmake
target_link_libraries(your_target PRIVATE ac3::forge_c)
```

`ac3::forge_c` resolves to whichever of the static or shared build the enclosing project's
`BUILD_SHARED_LIBS` asks for, same as `ac3::forge`; an installed package exports both variants
explicitly as `ac3::forge_c_static`/`ac3::forge_c_shared` — see [Using ac3::forge](index.md) for
the equivalent `ac3::forge` linking recipe. Unlike `ac3::forge`, **both** `ac3forge_c` variants
statically embed the codec core, and the AC-4 libraries where `AC3FORGE_BUILD_AC4` is on,
regardless of `BUILD_SHARED_LIBS`: a binding or embedder reaching
for a C ABI wants exactly one library to `dlopen`/`ctypes`/`ffi.dlopen`, not a second
`libac3forge.so` to also track down and ship — see `src/capi/CMakeLists.txt`'s header comment. On
Linux the shared library exports the C API and nothing else, so a program that links it beside
`libac3forge.so` still calls the C++ API in `libac3forge.so`; the copy of the codec inside
`libac3forge_c.so` serves the C API alone.

Built by default (`-DAC3FORGE_BUILD_CAPI=OFF` to skip it); it needs nothing `ac3::forge` itself
doesn't.

Linking `ac3::forge_c_static` from a C project takes one more step than the snippet above,
because the archive holds C++ objects: enable the CXX language beside C
(`project(your_project LANGUAGES C CXX)`), so that CMake links with the C++ driver, which supplies
the C++ runtime and libm. With only C enabled the link goes through the C driver and stops at C++
runtime symbols such as `operator new`. `ac3::forge_c_shared` carries its own runtime dependency
and links from a C-only project as it is. Neither variant needs {fmt}, and
[Using ac3::forge](index.md) says why.

A build that finds libraries through pkg-config runs
`pkg-config --static --cflags --libs ac3forge_c` for a static-only install. The line it prints
names `libac3forge_static.a` and the C++ runtime along with `libac3forge_c_static.a`; the
pkg-config paragraph of [Using ac3::forge](index.md) has the details.

## Conventions

**Every fallible function returns `ac3forge_status_t`.** `AC3FORGE_OK` is always zero, so
`if (ac3forge_xxx(...) != AC3FORGE_OK)` and the shorter `if (status)` are equally correct.
`ac3forge_status_message()` gives a short human-readable description for logging.

**Every handle is opaque and owned.** `ac3forge_encoder_t`, `ac3forge_decoded_frame_t`, and every
other `..._t` here are forward-declared structs — only pointers to them cross the header. Each has
a matching `_destroy` function; passing `NULL` to one is a no-op, matching `free()`. A function
producing a variable-length or structured result (a decoded frame, an encoded frame's bytes, a
list of OAMD objects) writes an owned handle through an out-parameter rather than filling a
caller-supplied buffer, so nothing here requires the caller to predict a size up front — the
pointee is left untouched on failure. Read it through the type's accessor functions, then destroy
it.

**No exception ever crosses this boundary.** `ac3::FrameError`/`ac3::DecodeError` map one-for-one
onto `ac3forge_status_t` codes (`AC3FORGE_ERROR_ENCODE_*`/`AC3FORGE_ERROR_DECODE_*`), and
`ac4::DecodeError`/`ac4::EncodeError` onto `AC3FORGE_ERROR_AC4_DECODE_*`/`AC3FORGE_ERROR_AC4_ENCODE_*`; an actual
C++ exception — realistically only `std::bad_alloc` for a codec core that never throws on its own
— is caught inside the library and reported as `AC3FORGE_ERROR_OUT_OF_MEMORY` or
`AC3FORGE_ERROR_INTERNAL` instead of propagating into a (possibly non-C++) caller frame.

**No ABI-compatibility promise before v1.0.** Same pre-1.0 stance as the rest of the project (see
see [API stability](api-stability.md)): a rebuild against a newer `ac3forge` may need a recompile, not merely a relink.
`ac3forge_version()` reports what was actually linked at runtime.

## Encoding

```c
ac3forge_encoder_config_t encoder_config;
ac3forge_encoder_config_init(&encoder_config);   // same defaults as EncoderConfig{}
encoder_config.bitrate_kbps = 192;
encoder_config.acmod = AC3FORGE_ACMOD_2_0;        // L, R

ac3forge_encoder_t* encoder = NULL;
ac3forge_status_t status = ac3forge_encoder_create(&encoder_config, &encoder);
```

`ac3forge_encoder_encode_frame` takes `ac3forge_encoder_channel_count(encoder)` channel pointers,
each exactly `AC3FORGE_SAMPLES_PER_FRAME` (1536) samples, and writes one complete syncframe into an
owned `ac3forge_bytes_t`:

```c
const float* channels[2] = {left, right};
ac3forge_bytes_t* encoded = NULL;
status = ac3forge_encoder_encode_frame(encoder, channels, 2, AC3FORGE_SAMPLES_PER_FRAME, &encoded);
/* ac3forge_bytes_data(encoded) / ac3forge_bytes_size(encoded), then ac3forge_bytes_destroy(encoded) */
```

`EncoderConfig`'s DRC field is exposed through the five named `ac3forge_drc_profile_t` presets
(`AC3FORGE_DRC_FILM_STANDARD`, `..._SPEECH`, …) — the same presets `ac3cli --drc` accepts — rather
than the full custom curve struct, which stays a C++-only tuning knob; see [Metadata](metadata.md)
for what each preset means.

## E-AC-3 encoding (multiple substreams, Annex E tools)

`ac3forge_eac3_encoder_t` and `ac3forge_eac3_access_unit_encoder_t` are the C counterparts to
`ac3::eac3::FrameEncoder` and `AccessUnitEncoder` — see [Encoding E-AC-3](encoding-eac3.md) for what
each field actually does. `ac3forge_eac3_frame_config_t` mirrors `FrameConfig`'s core surface —
sample rate (including the three `fscod2` reduced rates), bitrate, `acmod`/`lfe`, the Annex E tools
(`auto_tools` and the individual `coupling`/`spx`/`aht` flags it overrides), and substream identity
(`strmtyp`/`substreamid`/`chanmap`):

```c
ac3forge_eac3_frame_config_t config;
ac3forge_eac3_frame_config_init(&config);   // same defaults as FrameConfig{}
config.bitrate_kbps = 192;
config.acmod = AC3FORGE_ACMOD_2_0;           // L, R

ac3forge_eac3_encoder_t* encoder = NULL;
ac3forge_status_t status = ac3forge_eac3_encoder_create(&config, &encoder);
```

`ac3forge_eac3_encoder_encode_frame` takes `ac3forge_eac3_encoder_channel_count(encoder)` channel
pointers, each `ac3forge_eac3_encoder_samples_per_frame(encoder)` samples (always
`AC3FORGE_SAMPLES_PER_FRAME`, since `numblkscod` is not exposed), an optional
`ac3forge_eac3_frame_metadata_t*` (`NULL` measures the §7.7 words internally),
and an optional EMDF aux payload:

```c
const float* channels[2] = {left, right};
ac3forge_bytes_t* encoded = NULL;
status = ac3forge_eac3_encoder_encode_frame(encoder, channels, 2, AC3FORGE_SAMPLES_PER_FRAME,
                                             NULL, NULL, 0, &encoded);
```

Widening past 5.1 needs `ac3forge_eac3_access_unit_encoder_t`, built from one independent config
plus an array of dependent configs (at most 8) — `AC3FORGE_CHANMAP_71_REAR`/`_512_HEIGHT`/`_TOP_QUAD`
name the Table E2.5 combinations a dependent needs for 7.1/5.1.2/5.1.4:

```c
ac3forge_eac3_frame_config_t independent, dependent;
ac3forge_eac3_frame_config_init(&independent);
independent.bitrate_kbps = 448;
independent.acmod = AC3FORGE_ACMOD_3_2;
independent.lfe = 1;

ac3forge_eac3_frame_config_init(&dependent);
dependent.bitrate_kbps = 192;
dependent.acmod = AC3FORGE_ACMOD_2_0;
dependent.has_chanmap = 1;
dependent.chanmap = AC3FORGE_CHANMAP_512_HEIGHT;   // Vhl, Vhr -> 5.1.2

ac3forge_eac3_access_unit_encoder_t* au_encoder = NULL;
status = ac3forge_eac3_access_unit_encoder_create(&independent, &dependent, 1, &au_encoder);
```

`ac3forge_eac3_access_unit_encoder_encode` takes every substream's channels in transmission order
(the independent's first, LFE last, then each dependent's in the order its `chanmap` names them)
and writes an owned `ac3forge_eac3_access_unit_t` — `..._data`/`..._size` for the concatenated
bytes, `..._substream_count`/`..._substream_bytes` for the per-substream boundaries `crc2`
recomputation or demuxing needs. Full program:
[`examples/capi_encode_eac3.c`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/capi_encode_eac3.c).

## Atmos encoding

`ac3forge_atmos_encoder_t` encodes mono object signals into a legacy-playable 5.1 E-AC-3 bed
with OAMD and JOC. Create it with a fixed object count, then provide one 1536-sample signal and
one room placement per object for each frame:

```c
ac3forge_atmos_config_t config;
ac3forge_atmos_config_init(&config);

ac3forge_atmos_encoder_t* encoder = NULL;
status = ac3forge_atmos_encoder_create(&config, 1, &encoder);

ac3forge_object_placement_t placement;
ac3forge_object_placement_init(&placement);
placement.x = 0.75;  /* right side of the room */
const float* objects[1] = {mono_object};

ac3forge_bytes_t* unit = NULL;
status = ac3forge_atmos_encoder_encode_frame(
    encoder, objects, 1, AC3FORGE_SAMPLES_PER_FRAME, &placement, 1, &unit);
/* write ac3forge_bytes_data(unit), ac3forge_bytes_size(unit) */
ac3forge_bytes_destroy(unit);
ac3forge_atmos_encoder_destroy(encoder);
```

Positions use the room-anchored coordinates described under
[Spatial & Atmos objects](spatial-and-atmos.md): `x` and `y` are in `[0,1]`, and `z` is in
`[-1,1]`. The C API emits unsigned object containers. Object signing remains the separate
`ac3::signing` C++ library or the CLI workflow documented under [Object signing](signing.md).

## Decoding

`ac3forge_decoder_t` (AC-3) and `ac3forge_eac3_decoder_t` (E-AC-3/Atmos) mirror `FrameDecoder` and
`Eac3Decoder` — `ac3forge_decoder_decode_frame`/`ac3forge_eac3_decoder_decode_substream`/
`ac3forge_eac3_decoder_decode_access_unit` return an owned `ac3forge_decoded_frame_t`/
`ac3forge_decoded_substream_t`/`ac3forge_decoded_access_unit_t`, read through accessors and then
destroyed:

```c
ac3forge_decoded_frame_t* decoded = NULL;
status = ac3forge_decoder_decode_frame(decoder, data, size, &decoded);
int channels = (int)ac3forge_decoded_frame_channel_count(decoded);
const float* left = ac3forge_decoded_frame_channel_samples(decoded, 0);
ac3forge_decoded_frame_destroy(decoded);
```

`decode_substream`/`decode_access_unit` keep the C++ API's `std::optional`-via-return convention
for transient pre-noise processing's held-back frame (see [Decoding](decoding.md)): a return of
`AC3FORGE_OK` with the out-parameter left `NULL` means the frame's PCM is being held, not an
error. Call `ac3forge_eac3_decoder_flush()` at end of stream to collect it.

### Caller-buffer decode (no per-call allocation)

`ac3forge_decoder_decode_frame_into`/`ac3forge_eac3_decoder_decode_access_unit_into` are the C
mirrors of `FrameDecoder::decode_frame_into`/`Eac3Decoder::decode_access_unit_into` — the
memory-usage programme's forms for a realtime embedder or the WASM demo that cannot allocate on
the decode path. The PCM lands in caller-owned planar storage instead of an owned handle's own
allocation; the returned handle still carries every other field:

```c
float* channels[AC3FORGE_DECODER_MAX_CHANNELS];
float storage[AC3FORGE_DECODER_MAX_CHANNELS][AC3FORGE_SAMPLES_PER_FRAME];
for (size_t i = 0; i < AC3FORGE_DECODER_MAX_CHANNELS; i++) channels[i] = storage[i];

ac3forge_decoded_frame_t* decoded = NULL;
status = ac3forge_decoder_decode_frame_into(decoder, data, size, channels,
                                             AC3FORGE_DECODER_MAX_CHANNELS,
                                             AC3FORGE_SAMPLES_PER_FRAME, &decoded);
/* decoded's own channel_count() is 0 - the samples are already in `storage` */
```

The caller must always supply the documented maximum span count
(`AC3FORGE_DECODER_MAX_CHANNELS` = 6, `AC3FORGE_EAC3_DECODER_MAX_CHANNELS` = 16), each exactly
`AC3FORGE_SAMPLES_PER_FRAME` samples, since how many this particular frame actually codes is not
known until its header is parsed; a span this frame does not need is left untouched, not zeroed.

The E-AC-3 form keeps §3.7's hold-back semantics exactly: `AC3FORGE_OK` with the out-parameter
`NULL` means the same held-back frame the value form reports, and the caller's spans are left
completely untouched for that call too — a held-back frame's PCM is decoded and buffered
internally either way, and only copied out (to the caller's spans, this time) at the call that
releases it. `ac3forge_eac3_decoder_flush()` is still the only release path at end of stream, and
still returns library-owned data even for a decoder driven entirely through this form — flush's
own per-substream results were never assembled into one programme to begin with, so there is
nothing for a `flush_into` to write through a caller's spans (see its own header comment).

## Object audio (OAMD + JOC)

A decoded E-AC-3 substream or access unit that carries an Atmos object container exposes it
through `ac3forge_decoded_substream_has_object_metadata()`/
`ac3forge_decoded_access_unit_has_object_metadata()` and a parallel set of accessors — program
shape (`program_dynamic_only`/`program_lfe`/`program_bed`), each dynamic object's room-anchored
position and gain (`..._dynamic_object`), and JOC's reconstructed per-object audio
(`..._object_audio`/`..._object_audio_count`). Those audio entries are index-parallel to the
dynamic objects for the dynamic-object-only programme this project's own encoder writes; for a
bed programme they are its bed channels instead, and the C++ surface
(`DecodedSubstream::object_indices`, `ac3::oba::joc_object_indices`) is what says which. See
[Spatial & Atmos objects](spatial-and-atmos.md) for what the position/gain values mean and how
`ac3forge_atmos_encoder_t` (the C counterpart to `ac3::oba::AtmosEncoder`) produces them.

## Stream scan

`ac3forge_split_frames`/`ac3forge_split_access_units`/`ac3forge_stream_bsid` only delimit a
stream. `ac3forge_scan` — the C mirror of `ac3::io::scan`/`ScannedStream` — actually reads what
it contains: sample rate, layout, every programme it carries (§E2.3.1.2 allows up to eight for
E-AC-3), and the raw bsid/bsmod/bit-rate and DVB/ATSC service fields a container muxer's own
descriptors want, all without decoding any audio:

```c
ac3forge_scanned_stream_t* scanned = NULL;
status = ac3forge_scan(stream, stream_size, &scanned);

ac3forge_acmod_t acmod = ac3forge_scanned_stream_acmod(scanned);
int channels = ac3forge_scanned_stream_channels(scanned);  /* RENDERED, dependents folded in */

for (size_t i = 0; i < ac3forge_scanned_stream_access_unit_count(scanned); i++) {
    ac3forge_span_t unit = ac3forge_scanned_stream_access_unit(scanned, i);
    /* stream + unit.offset, unit.length -> that access unit's bytes */
}
ac3forge_scanned_stream_destroy(scanned);
```

Access-unit spans are offset/length pairs into the buffer passed to `ac3forge_scan` — same
convention as `ac3forge_split_frames`'s result, and the same lifetime requirement (keep that
buffer alive and unmodified for as long as the scan result is in use). A second programme's own
access units, and per-programme detail (substream id, folded channel count, its own bsmod), are
reached through the `ac3forge_scanned_stream_programme_*` accessors rather than the top-level
ones, which always describe the first (or only) programme — see `ac3::io::ScannedStream`'s own
comment on why a second programme's units are never appended to the first's list.

Timing helpers mirror `ac3::io::access_unit_timing`/`stream_duration_samples`/
`access_unit_at_sample`/`uniform_access_unit_samples` — the access unit covering a given sample
or second, the stream's total duration, and whether every access unit shares one length (E-AC-3's
`numblkscod` lets it vary; every AC-3 stream trivially agrees). Not mirrored: `AccessUnitTiming`'s
own `start_seconds`/`start_in_timescale` convenience methods, one line of arithmetic
(`start_sample / sample_rate`, or `* timescale` first) a caller can write directly against the
`ac3forge_scanned_stream_access_unit_timing` out-parameters instead.

## Loudness, level and QC metering

Three independent handle families mirror the library's own independent measurement types — there
is no single bundled "QC report" struct in `ac3::forge` itself to mirror, only in the CLI/GUI
application layer, which composes the same three the way a caller of this API would:

- **`ac3forge_loudness_meter_t`** mirrors `ac3::meta::LoudnessMeter` — BS.1770-4/5 integrated,
  momentary and short-term loudness, EBU Tech 3342 loudness range, and true peak.
  `ac3forge_loudness_meter_create` takes the same `acmod`/`lfe` weighting Annex 1 uses;
  `ac3forge_loudness_meter_create_for_chanmap` takes a Table E2.5 chanmap word instead, for
  BS.1770-5 Annex 3's extended algorithm over a wide rendered layout an acmod cannot name. Feed it
  incrementally with `ac3forge_loudness_meter_push` (any length per call, unlike `encode_frame`'s
  fixed frame size); every measurement is a `has_*`/value accessor pair, `std::optional`'s usual
  C mirror, since each has its own "not enough audio yet" threshold. `ac3forge_dialnorm_from_lkfs`
  is the §5.4.2.8 conversion the encoder's own dialnorm field needs from a measured result.
- **`ac3forge_level_meter_t`** mirrors `ac3::analysis::LevelMeter` — unweighted peak/RMS/clip
  ballistics per channel, the front-end meter both `ac3cli`/`ac3gui` already share one
  implementation for. `ac3forge_level_meter_level` is the live ballistic view (`peak_db`/
  `hold_db`/`rms_db`/`clipped`); `ac3forge_level_meter_summary` is the exact, unweighted
  whole-run statistic a file report wants instead. Not mirrored: `process_interleaved` (planar
  spans only, matching every other buffer convention in this header), and the presentational
  `channel_name`/`layout_name`/`channel_azimuth_deg`/`energy_vector` helpers — string/geometry
  convenience over the same acmod a caller already has on hand.
- **`ac3forge_qc_preset`/`ac3forge_qc_preset_name`/`ac3forge_parse_qc_preset`/
  `ac3forge_evaluate_qc_gate`** mirror `ac3::meta::qc` — the five named delivery-loudness gates
  (`ebu-r128-s2`, `atsc-a85`, `atsc-a85-streaming`, `netflix`, `apple-music-atmos`) `ac3cli qc`
  already checks a measurement against, each citing the document/clause/date its numbers were
  read out of. `ac3forge_evaluate_qc_gate` takes a loudness meter's own `has_integrated_lkfs`/
  `integrated_lkfs`/`has_true_peak_dbtp`/`true_peak_dbtp` straight through; a measurement that was
  itself unavailable leaves that half of the verdict at its not-passing default rather than a
  false pass, matching `ac3::meta::QcVerdict`'s own convention.

## AC-4

`ac3forge_ac4_decoder_t` and `ac3forge_ac4_encoder_t` mirror `ac4::Decoder`/`ac4::Encoder`
(ETSI TS 103 190-1 V1.4.1, TS 103 190-2 V1.3.1) behind the same opaque-handle, `_config_init()`
and out-parameter conventions as the rest of this header — see
[`ac3forge_c/ac3forge.h`](https://github.com/iainchesworthlabs/ac3forge/blob/main/src/capi/include/ac3forge_c/ac3forge.h)'s
own AC-4 section for the full surface. The section is declared whether or not this library was
configured with `AC3FORGE_BUILD_AC4` (on by default): built without it, every fallible function
returns `AC3FORGE_ERROR_UNSUPPORTED` (4), a `_create()` leaves its out-parameter `NULL`, and
`ac3forge_c/version.h`'s `AC3FORGE_HAS_AC4`, which `#cmakedefine`s that option, says which library
a program was built against. Two new status code ranges: `AC3FORGE_ERROR_AC4_DECODE_*` at 60–64
(`TRUNCATED`, `INVALID_TOC`, `INVALID_STREAM`, `UNSUPPORTED`, `MISSING_IFRAME`) and
`AC3FORGE_ERROR_AC4_ENCODE_*` at 80–81 (`INVALID_CONFIG`, `INVALID_INPUT`).

The encoder writes channel-based and channel-based-immersive content (mono, stereo, 5.0, 5.1,
5.0.4, 5.1.4) and, given an objects configuration, one object substream of A-JOC or direct-coded
objects ([Encoding objects](#encoding-objects) below). The decoder's object accessors read
whatever object audio a stream carries.

```c
ac3forge_ac4_encoder_config_t config;
ac3forge_ac4_encoder_config_init(&config);  // channels 2, 48000 Hz, frame_rate_index 13, 192 kbps
config.channels = 6;                         // 5.1: L R C LFE Ls Rs
config.bitrate_kbps = 256;

ac3forge_ac4_encoder_t* encoder = NULL;
ac3forge_status_t status = ac3forge_ac4_encoder_create(&config, &encoder);
```

`ac3forge_ac4_encoder_encode` takes `config.channels` planar spans of any equal length — the
encoder buffers input to its own frame length internally, unlike
`ac3forge_encoder_encode_frame`'s fixed `AC3FORGE_SAMPLES_PER_FRAME` — and writes the frames that
input completed into an owned array:

```c
const float* channels[6] = {l, r, c, lfe, ls, rs};
ac3forge_ac4_encoded_frame_t** frames = NULL;
size_t count = 0;
status = ac3forge_ac4_encoder_encode(encoder, channels, 6, samples_per_channel, &frames, &count);
/* ac3forge_ac4_encoded_frame_data(frames[i]) / ..._size(frames[i]): one raw AC-4 frame each */
ac3forge_ac4_encoded_frame_array_destroy(frames, count);
```

`ac3forge_ac4_encoder_flush` pads to the end of the last frame and returns whatever the delay
still held. `ac3forge_ac4_encoder_toc` reads back an owned `ac3forge_ac4_toc_t` snapshot for a
container muxer: `ac3forge_ac4_build_dac4` writes the `dac4` box payload (empty, with a reason
from `ac3forge_ac4_dac4_refusal`, where the table of contents holds something it cannot describe
whole), and `ac3forge_ac4_media_timing`/`ac3forge_ac4_samples_per_frame` give an ISOBMFF track's
timing. `ac3forge_ac4_sync_frame` wraps a raw frame with Annex G.3.1's sync word and an optional
CRC for a raw `.ac4` file or MPEG-2 TS.

`ac3forge_ac4_encoder_config_t` also carries `iframes` and `fragment_starts` (a pointer and a count
each: frames, counted from 0, that must be I-frames, and where an MP4's fragments start in samples
of the decoded output) and `experimental`, the flags of `ac4::EncoderConfig::Experimental` that
need no nested group: `aspx_balance`, `aspx_varvar`, `aspx_interleave`, `coding_configs`,
`seven_x`, `acpl`, `back_pair`, `ajcc` and `objects`. The arrays are read while
`ac3forge_ac4_encoder_create()` runs and not after. `ac3forge_ac4_encoder_refusal_reason()` takes
a configuration and returns the first rule `ac3forge_ac4_encoder_create()` refuses it for, as
`ac4::Encoder::refusal_reason()` does, or an empty string.

### Encoding objects

An objects configuration makes the stream one object substream. Each object is one input channel
of PCM and one `ac3forge_ac4_object_config_t`: a bed object from a loudspeaker
(`ac3forge_ac4_bed_channel_t`), a dynamic object, or the LFE, with the metadata in force from the
first sample as an `ac3forge_ac4_object_properties_t`, the struct the decoder already returns:
position, gain, priority, size (a width in each axis), zone mask, screen factor, depth exponent,
distance, divergence and headphone render mode, with the range and step of each in the header.
`ac3forge_ac4_object_properties_init()` fills it with the defaults; a zero-initialised struct has
a depth exponent no code holds and the encoder refuses it. The objects are coded as A-JOC (the
default: a computed downmix of `downmix_signals` signals, or a static 5.0 or 5.1 bed) or as
direct-coded object substreams (`coding`), and the object substream is experimental, so
`experimental.objects` has to be set beside the configuration:

```c
ac3forge_ac4_object_config_t objects[3];
for (int i = 0; i < 3; ++i) ac3forge_ac4_object_config_init(&objects[i]);  // dynamic, room centre
objects[0].properties.x = 0.1;
objects[0].properties.y = 0.2;
objects[0].properties.gain_db = -3.0;
objects[1].lfe = 1;
objects[2].has_bed = 1;
objects[2].bed = AC3FORGE_AC4_BED_LEFT;

ac3forge_ac4_objects_config_t scene;
ac3forge_ac4_objects_config_init(&scene);  // A-JOC over a computed downmix
scene.objects = objects;
scene.object_count = 3;

ac3forge_ac4_encoder_config_t config;
ac3forge_ac4_encoder_config_init(&config);
config.bitrate_kbps = 256;
config.experimental.objects = 1;
config.objects = &scene;
```

`ac3forge_ac4_encoder_encode_objects` takes one PCM array per object and the changes to their
metadata, in the order they are wanted or any other: an `ac3forge_ac4_object_metadata_update_t`
moves an object to new properties from an input sample of that call (0 to any later one) over
`ramp_samples` (0 to 2 047, or 2 048). The decoder reports the update at the output sample its
input sample comes out at, to within 32 samples.

```c
ac3forge_ac4_object_metadata_update_t move;
ac3forge_ac4_object_metadata_update_init(&move);
move.object = 0;
move.sample = 5000;
move.ramp_samples = 1024;
move.properties.x = 0.75;
move.properties.gain_db = -12.0;
status = ac3forge_ac4_encoder_encode_objects(encoder, pcm, 3, samples, &move, 1, &frames, &count);
```

The limits are the encoder's. There are 1 to `AC3FORGE_AC4_MAX_OBJECTS` (64) objects, at most one
the LFE and at least one that is not. The object substream is at `frame_rate_index` 13 (the
2 048-sample frame) and no other; an A-JOC downmix is 1 to `AC3FORGE_AC4_MAX_DOWNMIX_SIGNALS` (11)
signals, no more than there are full-band objects, or a static bed; direct-coded objects are
dynamic objects and the LFE, with no bed objects; the codec mode is `AUTO`, `SIMPLE` or `ASPX`.
An update for an object the configuration lacks, before the input's first sample, or with a
property off its range fails with `AC3FORGE_ERROR_AC4_ENCODE_INVALID_INPUT`, and
`ac3forge_ac4_encoder_refusal_reason()` names the rule a configuration breaks.

### Decoding

`ac3forge_ac4_decoder_config_t` holds `output` (`ac3forge_ac4_output_config_t`: the output level,
DRC mode, headphones, dialogue enhancement, downmix layout, LFE mixing and the two mix gains of
[AC-4](ac4.md#the-controls)), `presentation` (`ac3forge_ac4_presentation_choice_t`),
`concealment`, `level` and `decoding`; the syntax trace is not mirrored. Each struct has an
`_init()`:

```c
ac3forge_ac4_decoder_config_t config;
ac3forge_ac4_decoder_config_init(&config);
config.output.has_output_level_dbfs = 1;
config.output.output_level_dbfs = -24.0;      /* Lout: the level the stream's dialnorm is taken to */
config.output.downmix = AC3FORGE_AC4_DOWNMIX_STEREO;
config.presentation.language = "en";          /* read during the call, not kept */

ac3forge_ac4_decoder_t* decoder = NULL;
ac3forge_status_t status = ac3forge_ac4_decoder_create(&config, &decoder);

ac3forge_ac4_decoded_frame_t* decoded = NULL;
status = ac3forge_ac4_decoder_decode(decoder, frame, frame_size, &decoded);
if (status == AC3FORGE_OK && decoded != NULL) {
    size_t samples = ac3forge_ac4_decoded_frame_samples_per_channel(decoded);
    for (size_t ch = 0; ch < ac3forge_ac4_decoded_frame_channel_count(decoded); ++ch) {
        const float* pcm = ac3forge_ac4_decoded_frame_channel_samples(decoded, ch);
        ac3forge_ac4_speaker_t speaker = ac3forge_ac4_decoded_frame_speaker(decoded, ch);
        /* `samples` floats in `pcm`, for `speaker` */
    }
    ac3forge_ac4_decoded_frame_destroy(decoded);
} else if (status != AC3FORGE_OK) {
    /* ac3forge_ac4_decoder_refusal_reason(decoder) says why, in words */
}
```

`ac3forge_ac4_decoder_decode` takes one `raw_ac4_frame`, an MP4 sample or the payload of a sync
frame (the C API has no counterpart of `ac4::SyncFrameSplitter`), and writes
an owned `ac3forge_ac4_decoded_frame_t*`, left `NULL` (with `AC3FORGE_OK`) when the frame has no
output yet rather than as an error — the same `std::optional`-via-out-parameter convention as the
AC-3/E-AC-3 decoders above. Planar PCM comes back through
`ac3forge_ac4_decoded_frame_channel_samples`/`_speaker`; AC-4's frame length varies by frame rate,
so `ac3forge_ac4_decoded_frame_samples_per_channel()` reports each frame's length rather than a
fixed constant. `ac3forge_ac4_decoder_set_output`/`_set_presentation` change the output processing
and the chosen presentation from the next frame, needing no I-frame.
`ac3forge_ac4_decoder_presentation_*` reads the last frame's table of contents, and
`ac3forge_ac4_decoder_metadata_loudness` reads the selected presentation's loudness fields. A
presentation with object audio hands its objects over through `ac3forge_ac4_decoded_frame_object_*`
— kind, LFE, speaker, samples, the `ac3forge_ac4_object_properties_t` in force at the frame's
first sample, and the updates within the frame (`ac3forge_ac4_decoded_frame_object_update_count`
and `_update`: the output sample each takes effect at, the ramp a renderer takes to reach it and
the properties). The objects come in the decoder's order, not the encoder's: the LFE first, then
the bed objects, then the dynamic objects, each group in the order the configuration lists it.

The AC-4 cases of `tests/capi/test_capi.cpp` cover the rest. An A-JOC scene and a direct-coded one are encoded
through the C API and through `ac4::Encoder` itself, and the two streams are the same bytes; the
C API's decoder reads each object back within what each field's code can hold, with its own tone
and a metadata update at the sample its input sample comes out. Its other cases hold the limits
and refusals, the I-frame lists and each experimental flag to the encoder, and the null-safety
convention on every accessor, `ac3forge_ac4_encoder_encode_objects`'s argument checks and
`ac3forge_ac4_sync_frame`'s CRC byte. See [AC-4](ac4.md) for the C++ library these functions
mirror.

## What is deliberately out of scope

The self-check/mirror tracing (`ac3::verify::FrameTrace`) is a C++-oriented encoder-implementer
diagnostic, not part of this consumer-facing surface — see [Header map](header-map.md). The full
custom `ac3::meta::Profile` DRC curve (attack/release timing, boost ratios) is likewise a C++-only
tuning knob; the C API exposes only the five named presets (above). Internal kernel-level
benchmarking entry points such as `ac3::oba::band_energy` are excluded outright — their own C++
doc comments already say no caller outside the library should need them directly.

`ac3forge_eac3_frame_config_t` likewise trims `ac3::eac3::FrameConfig`: the `mixmdate`/`infomdat`
metadata groups, `dialnorm2`/`drc`/`heavy` (dual mono and DRC would reuse the same presets the AC-3
encoder already exposes, but the broader Table E1.2 metadata surface those two groups sit inside is
deferred), `vbr` (CBR only), `numblkscod` (six-block syncframes only), `search`/`dither` (both
decision knobs stay at their defaults — content-decided dither on, no per-frame bit-allocation
codes search), `chbwcod`/`fgaincod` (auto-from-bitrate only, unlike the AC-3 struct's own
`chbwcod`), `oba_complexity_index` (the TS 103 420 object-count marker, which
`ac3forge_atmos_encoder_t` sets for the streams it builds) and `last_dependent` (§E3.8.5's
end-of-programme marker — part of the substream identity
`ac3forge_eac3_access_unit_encoder_t` assigns itself, and readable back through
`ac3forge_decoded_substream_last_dependent()`) are not mirrored — a config built from
`ac3forge_eac3_frame_config_init()` and read back always agrees with a default `FrameConfig{}` on
every field this struct doesn't carry.

The AC-4 surface is a subset in the same way. `ac3forge_ac4_encoder_config_t` describes one
substream in one presentation: the loudness, DRC, downmix and dialogue-enhancement metadata groups,
several substreams and presentations, EMDF payloads, the syntax trace and the `drc_gains` and
`three_zero` experimental flags are not mirrored, so a configuration left at its defaults writes
the shape DEE's streams have for its channel count. The decoder side leaves out the syntax trace,
`Decoder::parse()` and its `FrameReport`, `decode_by_block()`, `select_presentation()` and the
metadata beyond the loudness values (the DRC, dialogue enhancement and downmix information of
`Decoder::metadata()`). The inspector's own functions (`ac4::scan`, `SyncFrameSplitter`,
`parse_raw_frame`, `rfc6381_codec_string`, `cmaf_refusal` and the manifest helpers) are C++ only;
`ac3forge_ac4_encoder_toc()` and the functions that take its result cover what a container muxer
needs from the encoder's own stream.

`ac3::oba::ObjectScene` (the object-scene timeline behind `ac3cli atmos-path` and the GUI's
export - see [Spatial & Atmos objects](spatial-and-atmos.md#the-scene-ac3obaobjectscene)) is not
here either. Its shape has settled: `SceneCursor` is the seam a live position source plugs into,
and the OSC wire form ([`ac3/oba/scene_osc.hpp`](spatial-and-atmos.md#the-osc-wire-form)), a
sibling header, changed nothing about `scene.hpp`. It is left out because this surface is a
candidate for the coming API freeze, where an experimental type would be a lasting commitment, and
exposing half of it - the serialisation without the type, say - would be worse than exposing none,
because a caller would get a scene it could load and not evaluate. Read and write the JSON form
from the host language and hand the resulting placements to the encoder entry points above.
