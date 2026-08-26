# C API

Roadmap item F1: a stable, minimal C-callable surface over `ac3::forge`'s encode/decode core —
AC-3, E-AC-3 and Atmos (OAMD + JOC) — for bindings and embedding by callers that cannot or do not
want to link C++23. The whole surface is one header,
[`ac3forge_c/ac3forge.h`](https://github.com/iainchesworthlabs/ac3forge/blob/main/src/capi/include/ac3forge_c/ac3forge.h),
plain C11 with no C++ type crossing it anywhere — only opaque handles and POD structs. It is a
separate library from `ac3::forge`: link `ac3::forge_c` instead, not both.

[`examples/capi_encode_decode.c`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/capi_encode_decode.c)
is a complete, buildable program (compiled as C, not C++, so the build itself proves the header
is genuinely C-usable) — the excerpts below are drawn from it. `tests/capi/test_capi.cpp` covers the
rest of the surface, including E-AC-3/Atmos decode and the error paths, from Catch2.

```cmake
target_link_libraries(your_target PRIVATE ac3::forge_c)
```

`ac3::forge_c` resolves to whichever of the static or shared build the enclosing project's
`BUILD_SHARED_LIBS` asks for, same as `ac3::forge`; an installed package exports both variants
explicitly as `ac3::forge_c_static`/`ac3::forge_c_shared` — see [Using ac3::forge](index.md) for
the equivalent `ac3::forge` linking recipe. Unlike `ac3::forge`, **both** `ac3forge_c` variants
statically embed the codec core regardless of `BUILD_SHARED_LIBS`: a binding or embedder reaching
for a C ABI wants exactly one library to `dlopen`/`ctypes`/`ffi.dlopen`, not a second
`libac3forge.so` to also track down and ship — see `src/capi/CMakeLists.txt`'s header comment.

Built by default (`-DAC3FORGE_BUILD_CAPI=OFF` to skip it); it needs nothing `ac3::forge` itself
doesn't.

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
onto `ac3forge_status_t` codes (`AC3FORGE_ERROR_ENCODE_*`/`AC3FORGE_ERROR_DECODE_*`); an actual
C++ exception — realistically only `std::bad_alloc` for a codec core that never throws on its own
— is caught inside the library and reported as `AC3FORGE_ERROR_OUT_OF_MEMORY` or
`AC3FORGE_ERROR_INTERNAL` instead of propagating into a (possibly non-C++) caller frame.

**No ABI-compatibility promise before v1.0.** Same pre-1.0 stance as the rest of the project (see
roadmap item AP1): a rebuild against a newer `ac3forge` may need a recompile, not merely a relink.
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
pointers, each `ac3forge_eac3_encoder_samples_per_frame(encoder)` samples (`AC3FORGE_SAMPLES_PER_FRAME`
today), an optional `ac3forge_eac3_frame_metadata_t*` (`NULL` measures the §7.7 words internally),
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

`ac3::oba::ObjectScene` (the object-scene timeline behind `ac3cli atmos-path` and the GUI's
export - see [Spatial & Atmos objects](spatial-and-atmos.md#the-scene-ac3obaobjectscene)) is not
here either, and that is a decision rather than an omission: the shape of the type is expected to
move when a live position source lands (roadmap `UX4`, which is why `SceneCursor` exists), and
this surface is a candidate for the coming API freeze, where an experimental type would be a
lasting commitment. Exposing half of it - the serialisation without the type, say - would be
worse than exposing none, because a caller would get a scene it could load and not evaluate.
Read and write the JSON form from the host language and hand the resulting placements to the
encoder entry points above until the type settles.
