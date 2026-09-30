# Python bindings

A pybind11 module (`python/src/iclforge_ext/bindings.cpp`) bound straight onto
`ac3::FrameEncoder`, `ac3::FrameDecoder`, `ac3::Eac3Decoder`, `ac3::eac3::FrameEncoder`,
`ac3::eac3::AccessUnitEncoder` and `ac3::oba::AtmosEncoder`, and, in the `ac3forge.ac4` submodule,
`ac4::Decoder` and `ac4::Encoder` — pybind11-direct, not layered on a separate C API. Install from
PyPI:

```bash
pip install ac3forge
```

or, from a source checkout of this repository, build against the same CMake tree everything else
here uses:

```bash
pip install ./python
```

A wheel built from a release that predates the AC-4 module has no `ac3forge.ac4`: the wheels for
0.10.0b1 and earlier are of that kind, and a build from a checkout has the module. PyPI carries
0.10.0b1's wheels for Windows x64, Linux x86_64 and macOS on Apple Silicon, for Python 3.10 to 3.14.
`wheels.yml` also builds Linux aarch64 and macOS Intel wheels, which no release has carried, and
PyPI has no source archive, so on those two `pip install ./python` from a checkout is the way in.

The package's own readme — layout, build notes and examples not duplicated here — lives at
[`python/README.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/python/README.md).

`ac3forge.__version__` reports the installed package's own PEP 440 version string, derived from
the nearest `git describe` tag the same way `PROJECT_VERSION_FULL` is on the C++ side (see
[docs/releasing.md](../releasing.md#versioning)) but rendered by `setuptools_scm` rather than
`cmake/GitVersionDerivation.cmake` — independently, on the Python-packaging side. A tag like
`v0.8.0-beta.1` therefore reports as `0.8.0b2.dev1+...` between releases or `0.8.0b1` exactly on
the tagged commit; that is PEP 440's normal rendering of a SemVer prerelease tag, not a bug.

## Encoding AC-3

```python
import ac3forge as ac3

encoder = ac3.FrameEncoder(ac3.EncoderConfig(bitrate_kbps=448, acmod=ac3.Acmod.k3_2, lfe=True))
stream = bytearray()
for frame in range(31):
    channels = [build_channel(tone, frame) for tone in TONES_HZ]  # 6 numpy float32 arrays
    stream += encoder.encode_frame(channels)
```

Full program: [`examples/python/encode_decode_roundtrip.py`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/python/encode_decode_roundtrip.py).

`encode_frame` takes either a 2-D `(n_channels, n_samples)` array or a sequence of 1-D
`float32`-convertible arrays (any array-like `numpy` accepts — a list also works), one per
channel, **AC-3 channel order** (Table 5.8, LFE last — same convention as the C++ API, see
[docs/library/index.md](index.md#conventions)), each exactly `ac3.SAMPLES_PER_FRAME` (1536)
samples. It returns one syncframe as `bytes`. See [Zero-copy numpy and buffer
reuse](#zero-copy-numpy-and-buffer-reuse) below for what "zero-copy" means here and the one
caveat it comes with.

`ac3.EncoderConfig` mirrors the coding and metadata fields of `ac3::EncoderConfig`
(`encoder/encoder.hpp`): `sample_rate`, `bitrate_kbps`, `dialnorm`, `dialnorm2`, `chbwcod`, `acmod`,
`lfe`, `coupling`, `cplbegf`, `cplendf`, `fast_mdct`, `drc`, `heavy`, `drc2`, `heavy2`, `cmixlev`
and `surmixlev`. `fgaincod`, `dither`, `delta_allocation`, `search`, `info` (the bsi fields),
`alternate_bsi` (Annex D) and `trace` are not bound. Construct it with keyword arguments for
whichever fields you want to change from their C++ defaults; an unrecognised keyword raises
`TypeError` rather than being silently ignored:

```python
config = ac3.EncoderConfig(
    acmod=ac3.Acmod.k2_0,
    bitrate_kbps=192,
    drc=ac3.profile_for(ac3.ProfileId.kFilmLight),
    heavy=ac3.HeavyConfig(peak_ceiling_dbfs=-1.0),
)
```

`ac3.profile_for(id)` is `ac3::meta::profile(ProfileId)` — the conventional Dolby DRC curves;
`ac3.Profile(...)` is available directly for a fully custom curve, same shape as the C++
`meta::Profile` struct.

## Decoding

```python
decoder = ac3.FrameDecoder()
for frame_bytes in ac3.split_frames(stream):
    decoded = decoder.decode_frame(frame_bytes)
    print(decoded.channel_labels, decoded.channels[0].shape)
```

`decoded.channels` is a list of `numpy.float32` arrays, one per channel, in decode order — a
read-only *view* onto `decoded`'s own memory rather than a copy (see [Zero-copy
numpy](#zero-copy-numpy-and-buffer-reuse) below). `decoded.channel_labels` is the same channels'
Table 5.8/Table E2.5 names as plain strings (`["L", "C", "R", "Ls", "Rs", "LFE"]`) — not part of
the C++ `DecodedFrame`/`DecodedSubstream` structs themselves, added here purely for convenience.

`ac3.split_frames`/`ac3.split_access_units` wrap the free functions of the same name in
`ac3/decoder/decoder.hpp` — splitting a raw elementary stream (or one already known to be E-AC-3)
into individual syncframes or access units before decoding each one.

## Zero-copy numpy and buffer reuse

Every `encode_frame`/`encode_access_unit` call above (AC-3, E-AC-3, and
`AtmosEncoder.encode_frame`'s `objects`) and every decoded `.channels`/`.object_audio` property
avoids a `memcpy` when it can:

- **Encode input** is read directly out of the array(s) you pass — no intermediate copy — as
  long as they are already `float32` and C-contiguous (`numpy`'s default for a freshly-built
  array). An array that isn't (wrong dtype, a transposed/strided view, a Python `list` of plain
  floats) is converted once, exactly as it always was; this only removes the *second*,
  unconditional copy the pre-AP6 bindings always made on top of that.
- **Decoded PCM** (`.channels`, `.object_audio`) is a read-only `numpy` view directly onto the
  `DecodedFrame`/`DecodedSubstream`/`DecodedAccessUnit` instance's own memory — no allocation, no
  copy. The view keeps that instance alive for as long as the view itself is (via `numpy`'s own
  `base` mechanism), so it stays valid even after you drop your last reference to the decoded
  object. It is non-writeable (`arr.flags.writeable is False`): mutating it would silently
  corrupt the decoder's own state.

**The one caveat**: encoding releases Python's GIL for the actual codec work (so another Python
thread can make progress while it runs), which means an array you are encoding must not be
mutated by another thread until `encode_frame`/`encode_access_unit` returns — the same
"don't touch the buffer mid-call" contract any zero-copy buffer-protocol API has. This does not
apply to the array(s) you get back from decoding; those are plain read access once the call
returns.

For a caller that decodes the same stream shape repeatedly (a realtime embedder, a tight batch
loop) and wants to reuse its own buffers instead of letting each call allocate fresh ones,
`FrameDecoder.decode_frame_into`/`Eac3Decoder.decode_access_unit_into` write PCM straight into
buffers you supply instead:

```python
import numpy as np

decoder = ac3.FrameDecoder()
out = np.zeros((ac3.MAX_AC3_CHANNELS, ac3.SAMPLES_PER_FRAME), dtype=np.float32)
for frame_bytes in ac3.split_frames(stream):
    decoded = decoder.decode_frame_into(frame_bytes, out)  # decoded.channels stays empty
    n = ac3.fullbw_channel_count(decoded.acmod) + (1 if decoded.lfe else 0)
    print(decoded.channel_labels, out[:n])
```

`out` is either a single 2-D `(buffers, ac3.SAMPLES_PER_FRAME)` array or a sequence of 1-D
arrays, each `float32`, C-contiguous and writeable. `buffers` must be **at least**
`ac3.MAX_AC3_CHANNELS` (6) for `decode_frame_into`, or `ac3.eac3.MAX_RENDER_CHANNELS` (16) for
`decode_access_unit_into` — the
real channel count for a given frame is only known once it has been decoded, so both methods ask
for enough buffers up front to cover any layout their decoder can produce; unused trailing
buffers are simply left untouched. Every one of these constraints is checked explicitly and
raises `TypeError`/`ValueError` on mismatch — it does not fall back to silently copying into a
private buffer the decoder would write into instead of yours (which would defeat the entire
point), and it does not rely on the C++ side's own `assert()` (compiled out in release wheels) as
the only guard, the same policy `encode_frame`'s own channel-count check follows (see
[Errors](#errors) below). The returned `DecodedFrame`/`DecodedAccessUnit`'s own `.channels` stays
empty either way — read the PCM back from `out`.

## Scanning a stream

`ac3.scan()` wraps `ac3::io::scan` (`ac3/io/elementary.hpp`) — reading an
elementary stream's shape (channel layout, every programme, every access unit's byte range)
without decoding any audio, the same walk `ac3cli probe`/a muxer's own input stage does:

```python
result = ac3.scan(stream)
print(result.kind, result.acmod, result.lfe, result.channels)
print(f"{len(result.access_units)} access units, {ac3.stream_duration_seconds(result):.2f}s")

decoder = ac3.FrameDecoder()  # or Eac3Decoder, for an E-AC-3/kAc3CoreEac3Extension stream
for unit in result.access_units:
    decoded = decoder.decode_frame(unit)
```

`result.access_units` is `ac3.ScannedStream`'s first (or only) programme's access units, already
split — `Eac3Decoder.decode_access_unit`'s own input shape, or `FrameDecoder.decode_frame`'s for
plain AC-3. `result.kind` is `ac3.StreamKind` — `kAc3`, `kEac3`, or `kAc3CoreEac3Extension` for an
AC-3 core carrying Annex E dependent extensions (§E2.3.1.2); every scalar field describes that
first programme, same as the C++ `ScannedStream`'s own convention. A stream carrying more than one
independent substream (broadcast DD+'s alternate-language/commentary services) reports each as
its own entry in `result.programmes` — a parallel sequence, not more entries in `access_units`,
since two programmes are never one spliced timeline.

`ac3.access_unit_timing(result, index)` and `ac3.stream_duration_samples`/`stream_duration_seconds`/
`access_unit_at_sample`/`access_unit_at_seconds`/`uniform_access_unit_samples` mirror
`ac3::io::access_unit_timing` and its neighbours — all free functions taking a `ScannedStream`,
matching the C++ shape, useful for a container muxer computing where to cut. `ac3.read_frame_header`
(`ac3::io::read_frame_header`) reads one syncframe's header — everything `scan()` reports about
the first frame, without walking the rest of the stream.

A malformed stream raises `ac3.Ac3ScanError` (`.error: ac3.ScanError`) — same exception-translation
convention as encode/decode failures, see [Errors](#errors) below.

## Research trace export

`ac3.verify.FrameTrace`/`Eac3AccessUnitTrace` are caller-owned handles that
`DecoderConfig(trace=...)`/`(eac3_trace=...)` fills, per block per stream, as a real decode runs —
exponents, bit allocation pointers, the §7.2.2.6 masking curve and the composite SNR offset.
`ac3.verify.trace_to_csv`/`trace_to_json_lines` turn one of those into text, one tidy row per
(frame, substream, block, stream, kind, index, value):

```python
trace = ac3.verify.FrameTrace()
decoder = ac3.FrameDecoder(ac3.DecoderConfig(trace=trace))

csv_text = ac3.verify.trace_csv_header()
for frame in range(FRAME_COUNT):
    decoder.decode_frame(frame_bytes[frame])
    # decode_frame refills `trace` from scratch each call - read it back out
    # once per frame rather than accumulated across the loop.
    csv_text += ac3.verify.trace_to_csv(trace, frame_index=frame)
```

Full program: [`examples/python/trace_export.py`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/python/trace_export.py).

`kind` distinguishes the per-*bin* `exponent`/`bap` curves from the per-*band* `mask` curve and
the per-stream `snr_offset` scalar — different index spaces, named rather than forced together.
Load the CSV/JSON Lines text with `pandas.read_csv`/`read_json(lines=True)` and call
`.to_parquet()` from there for Parquet; this binding has no Parquet writer of its own; see
`ac3/verify/trace_export.hpp` for why.

## Encoding E-AC-3

`ac3.eac3.FrameEncoder`/`AccessUnitEncoder` wrap `ac3::eac3::FrameEncoder`/
`AccessUnitEncoder` directly (pybind11-direct, like everything else in this binding) — a real
submodule rather than a flat `Eac3FrameEncoder` name, since `ac3::FrameEncoder` and
`ac3::eac3::FrameEncoder` share a name across C++ namespaces; `ac3.FrameEncoder`
(AC-3) and `ac3.eac3.FrameEncoder` (E-AC-3) keep that collision out of the Python surface too.

```python
config = ac3.eac3.FrameConfig(bitrate_kbps=192, acmod=ac3.Acmod.k2_0)
encoder = ac3.eac3.FrameEncoder(config)
frame = encoder.encode_frame(channels)  # channels: encoder.channel_count arrays, AC-3 order
```

`ac3.eac3.FrameConfig` mirrors `ac3::eac3::FrameConfig`'s core surface — sample rate (including
the three `fscod2` reduced rates), bitrate, `numblkscod`, `acmod`/`lfe`, the Annex E tools
(`auto_tools` and the individual `coupling`/`spx`/`aht` flags it overrides), substream identity
(`strmtyp`/`substreamid`/`chanmap`/`last_dependent`), and `drc`/`heavy`/`drc2`/`heavy2` (the same
`ac3.Profile`/`ac3.HeavyConfig` types the AC-3 side uses). Not mirrored: the `mixmdate`/`infomdat`
metadata groups (`mixing`, `info`), `vbr`/ABR (`EQ12`), `search` (`EQ7`/`EQ13`'s per-frame
bit-allocation codes search), `fgaincod` and `delta_allocation`. None of these is left out for a
reason of design, as the C API's own documented trim is.

### Wide layouts: `ac3.eac3.AccessUnitEncoder`

Anything past 5.1 needs an independent substream plus dependents that widen it —
`ac3.eac3.access_unit_config_for_layout()` is the named-layout convenience: it builds a whole
`AccessUnitConfig` from a `LayoutId`, without hand-building a dependent's `chanmap`:

```python
config = ac3.eac3.access_unit_config_for_layout(ac3.eac3.LayoutId.k71, bitrate_kbps=448)
encoder = ac3.eac3.AccessUnitEncoder(config)
unit = encoder.encode_access_unit(channels)  # channels: encoder.channel_count arrays
stream += unit.bytes
```

`ac3.eac3.LayoutId` names the same eight layouts `ac3::plan::LayoutId` does (`kMono`, `kStereo`,
`kDualMono`, `k51`, `k71`, `k512`, `k514`, `k714`); `dependent_bitrate_kbps` (default half of
`bitrate_kbps`, applied to every dependent) overrides the per-dependent rate. Building an
`AccessUnitConfig` by hand works too — `independent`/`dependents` are plain
`ac3.eac3.FrameConfig`/`list[ac3.eac3.FrameConfig]` fields — for a layout `access_unit_config_for_layout`
doesn't name, or full control over each substream's own fields.

`unit` is an `ac3.eac3.AccessUnit`: `.bytes` is the whole access unit, `.substream_bytes` is each
substream's byte length (independent first, summing to `len(unit.bytes)`).

Full program: [`examples/python/encode_eac3.py`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/python/encode_eac3.py).

## E-AC-3 and Atmos objects

`ac3.Eac3Decoder.decode_substream`/`decode_access_unit` return `None` exactly when a frame's PCM
is being held back for transient pre-noise processing (§3.7, see the C++ `Eac3Decoder` header) —
call `.flush()` once at end-of-stream to collect anything still pending.

```python
encoder = ac3.AtmosEncoder(ac3.AtmosConfig(bitrate_kbps=448), objects=2)
placements = [
    ac3.ObjectPlacement(position=ac3.Position(x=0.2, y=0.5, z=0.0)),
    ac3.ObjectPlacement(position=ac3.Position(x=0.8, y=0.5, z=0.5)),
]

decoder = ac3.Eac3Decoder()
unit = encoder.encode_frame([object0_pcm, object1_pcm], placements)
decoded = decoder.decode_access_unit(unit)
if decoded is not None and decoded.object_metadata is not None:
    for obj, audio in zip(decoded.object_metadata.objects, decoded.object_audio):
        print(obj.position.x, obj.position.y, obj.position.z, obj.gain_db, audio.shape)
```

`decoded.object_metadata` is the decoded OAMD (`ac3.DecodedProgram`: `.program` plus
`.objects`, a list of `ac3.DynamicObject`); `decoded.object_audio` is JOC's reconstructed
per-object audio, index-parallel to `object_metadata.objects` — same pairing convention as the
C++ `DecodedSubstream`/`DecodedAccessUnit` structs.

## Errors

Every fallible call translates the C++ side's `std::expected` error branch into a Python
exception rather than a Result-like return — idiomatic for a Python API, even though the C++
core itself never throws (see [CONTRIBUTING.md](https://github.com/iainchesworthlabs/ac3forge/blob/main/CONTRIBUTING.md)'s
"no exceptions for stream-level failure" rule, which is a C++-core policy this binding layer
does not need to import wholesale).

| Exception | Raised by | `.error` |
|---|---|---|
| `ac3.Ac3EncodeError` | `FrameEncoder.encode_frame`, `AtmosEncoder.encode_frame`, `ac3.eac3.FrameEncoder.encode_frame`, `ac3.eac3.AccessUnitEncoder.encode_access_unit` | `ac3.FrameError` |
| `ac3.Ac3DecodeError` | `FrameDecoder.decode_frame`/`decode_frame_into`, `Eac3Decoder.decode_substream`/`decode_access_unit`/`decode_access_unit_into`, `ac3.split_frames`/`split_access_units`/`stream_bsid` | `ac3.DecodeError` |
| `ac3.Ac3ScanError` | `ac3.scan`, `ac3.read_frame_header` | `ac3.ScanError` |

All three derive from `ac3.Ac3Error(RuntimeError)`. Each message is the C++ `describe()` text of
the error after a prefix such as `ac3forge encode failed:`; `ac3.describe` is overloaded for
`FrameError`, `DecodeError` and `ScanError`, so the same text is available without an exception.

A wrong-length or wrong-count channel array (not `ac3.SAMPLES_PER_FRAME` samples, or not
`channel_count` of them) raises a plain `ValueError` instead — that is a Python-level usage
error, not a codec-level failure the C++ side can report at all (short-changing `encode_frame` is
documented as "a programming error, not a runtime one"). `decode_frame_into`/
`decode_access_unit_into`'s `out` gets the same treatment for its own shape: too few buffers, a
buffer that's too short, or one that isn't C-contiguous or writeable all raise `ValueError`; the
wrong dtype raises `TypeError`. See [Zero-copy numpy](#zero-copy-numpy-and-buffer-reuse) above.

## Latency

`FrameEncoder.latency`, `AtmosEncoder.latency` (with `AtmosEncoder.bed_latency`, the 5.1 bed a
legacy decoder hears), `eac3.FrameEncoder.latency` and `eac3.AccessUnitEncoder.latency` return an
`ac3.LatencyBudget`: the `frame_samples`, `transform_samples`, `lookahead_samples` and
`holdback_samples` terms of [Encoding AC-3](encoding-ac3.md#latency), their `total_samples`, and
`milliseconds(sample_rate)`. Each of those encoders also has a `latency_samples` total, and
`FrameDecoder`, `Eac3Decoder` and `ac4.Decoder` have `latency_samples` for the delay they add.
`ac3.BLOCKS_PER_FRAME` (6) and `ac3.TRANSFORM_DELAY_SAMPLES` (256) sit beside `SAMPLES_PER_FRAME`.

## Enums and result types

Every enum keeps its C++ enumerator names (`ac3.Acmod.k3_2`, `ac3.SampleRate.k48000`), and the
result types are read-only.

| Names | Where they appear |
|---|---|
| `Acmod`, `SampleRate`, `StreamType`, `StreamKind`, `ProfileId`, `CentreMixLevel`, `SurroundMixLevel` | Configs and results throughout. `ac3.sample_rate_hz(rate)` gives a `SampleRate` in hertz, `ac3.fullbw_channel_count(acmod)` the full-bandwidth channel count and `ac3.profile_name(id)` a profile's name |
| `FrameError`, `DecodeError`, `ScanError` | The `.error` of the exceptions in [Errors](#errors) |
| `FrameHeader`, `ScannedStream`, `ScannedProgramme`, `SubstreamService`, `AccessUnitTiming` | Results of `ac3.read_frame_header`, `ac3.scan` (`programmes` holds `ScannedProgramme`s, `associated_substreams` `SubstreamService`s) and `ac3.access_unit_timing` (`start_sample`, `duration_samples`, `start_seconds`, `duration_seconds`, `start_in_timescale(timescale)`, `duration_in_timescale(timescale)`) |
| `Program`, `DecodedProgram`, `DynamicObject`, `Position`, `ObjectPlacement` | The OAMD side of [E-AC-3 and Atmos objects](#e-ac-3-and-atmos-objects): `Program` (`dynamic_only`, `lfe`, `bed`, `dynamic_objects`) is what `AtmosEncoder.program` and `DecodedProgram.program` return |
| `eac3.LayoutId`, `eac3.FrameMetadata` | [Encoding E-AC-3](#encoding-e-ac-3). `eac3.FrameEncoder.encode_frame(channels, metadata=None, aux=b"")` takes the §7.7 words as an `eac3.FrameMetadata` (`dynrng`, `compr`, `dynrng2`, `compr2`) and an EMDF `aux` payload |
| `meta.QcPresetId`, `QcLoudnessLimit`, `QcPreset`, `QcVerdict` | `meta.qc_preset(id)` returns a `QcPreset` (`target_lkfs`, `tolerance_lu`, `max_true_peak_dbtp`, `loudness_limit` of `kBand` or `kCeiling`, `source`); `meta.evaluate_qc_gate` returns a `QcVerdict` (`loudness_delta_lu`, `loudness_pass`, `true_peak_margin_dbtp`, `true_peak_pass`, `passed`) |
| `signing.VerifySummary` | `signing.verify_atmos_stream`'s counts: `valid`, `mismatch`, `no_container` and `all_valid` |
| `containers.TsCodec`, `TsProfile` | `TsTrack.codec` (`kAc3`, `kEac3`, `kAc4`) and `mux_mpegts`'s `profile` (`kDvb`, `kAtsc`) |
| `ac4.Speaker`, `ObjectKind`, `DownmixTarget`, `DrcMode`, `AssociatedType`, `ConcealmentPolicy`, `ConcealmentAction`, `DecodingMode`, `CodecMode`, `RateMode`, `BedChannel`, `ObjectCoding`, `AjocDownmix`, `AdditionalPair`, `DecodeError`, `EncodeError` | The enums of `ac4::` behind the [AC-4](ac4.md) controls, under their C++ enumerator names |
| `ac4.OutputConfig`, `PresentationChoice`, `DecoderConfig`, `Concealment`, `PresentationInfo`, `LoudnessInfo`, `EncodedFrame` | `OutputConfig` has `output_level_dbfs`, `drc`, `headphones`, `dialogue_enhancement_db`, `downmix`, `mix_lfe`, `dialogue_gain_db` and `associated_gain_db`; `PresentationChoice` `presentation_id`, `index`, `language`, `associated`, `associated_type` and `headphones`; `DecoderConfig` `output`, `concealment`, `presentation`, `level` and `decoding`; `Concealment` the `error` and `action` of a frame the decoder made in place of one that did not decode; `PresentationInfo` `index`, `presentation_id`, `md_compat`, `enabled`, `alternative`, `pre_virtualized`, `name`, `language`, `decodable`, `selectable` and `speakers`; `LoudnessInfo` `dialnorm_dbfs`, `integrated_lkfs`, `true_peak_dbtp` and `loudness_range_lu`, each `None` until the stream sends it; `EncodedFrame` `data`, `samples` and `iframe`. A `DecodedFrame` also has `sample_rate_hz`, `sequence_counter`, `presentation_index`, `presentation_id` and `concealed` |

## Containers, metering, QC and signing

Three more submodules, each pybind11-direct over the same C++ classes every other binding here
wraps, and a context manager:

- **`ac3forge.containers`** — the three container writers and the batch read side, bytes in /
  bytes out: `mux_matroska`/`mux_mp4`/`mux_mpegts` over `MatroskaTrack`/`Mp4Track`/`TsTrack`
  (kwargs constructors, the same convention every config class here uses), and
  `demux_matroska`/`demux_mp4`/`demux_mpegts` bringing frames back out. `Mp4Track.codec_config`
  takes the `dac3`/`dec3` payload `ac3forge.build_codec_config_box(stream)` produces — built
  straight off the bitstream, never off whatever a source container declared, exactly like
  `ac3cli mp4`. AC-4 goes through in part: `containers.TsCodec.kAc4` for MPEG-TS (DVB only), and
  `Mp4Track(codec_id="ac-4", codec_config=...)` with the `dac4` payload of
  `ac4.Encoder.toc.build_dac4()`; `demux_mp4` and `demux_mpegts` read AC-4 tracks back. `Mp4Track`
  has no `timescale` and `mux_mp4` no sync-sample list, so every sample is marked a sync sample and
  a frame rate whose frames alternate in length (29.97, 59.94 and 119.88 fps) has no
  `samples_per_frame` to give; `ac3cli` or the C++ `mp4::mux` writes those. The incremental
  `Reader`/`Writer` classes and the fragmented-MP4/HLS/DASH surface are C++-only, by design.
- **`ac3forge.meta`** — `LoudnessMeter` (BS.1770; every gated measurement is `None` until it
  can mean anything), the cited `qc_preset()` table, and `evaluate_qc_gate()` — `ac3cli qc`'s
  own machinery, callable from a notebook.
- **`ac3forge.signing`** — `SigningKey` (base64 or raw, the single decode every front end
  shares), `sign_atmos_stream` (returns a signed copy — Python bytes are immutable),
  `has_authenticity_tag` and `verify_atmos_stream`.
- **`Eac3Decoder` is a context manager** — `with ac3forge.Eac3Decoder() as d:` drains the §3.7
  hold-back on scope exit (discarding it; call `flush()` yourself to keep it).

The hand-written stubs in `__init__.pyi` cover all of it, and wheels.yml's `stubtest` step
holds them to the compiled module on every push.

## AC-4

`ac3forge.ac4` is one of the extension's optional submodules, with `containers` and `signing`: it
exists only in a build that also built the libraries behind it (`AC3FORGE_BUILD_AC4`), and the
wheel build turns them all on. It is pybind11-direct on `ac4::Decoder`/`ac4::Encoder` (ETSI TS
103 190-1 V1.4.1, TS 103 190-2 V1.3.1), not layered on the C API, and binds a deliberate subset of
both C++ headers:
decoder output config, presentation selection, concealment, decoded PCM/speakers/objects (with the
metadata updates within a frame) and loudness metadata; encoder config (the core fields,
`iframes` and `fragment_starts`, the `experimental` flags that need no nested group, and one
object substream), `encode`/`flush`, and a minimal `Toc` wrapper for container muxing. Left out on
both sides: the syntax trace, DRC/dialogue-enhancement/downmix detail beyond `LoudnessInfo`, the
loudness, DRC, downmix and dialogue configuration groups, substream/presentation configuration
lists, EMDF payloads, and the `drc_gains` and `three_zero` experimental flags.

The encoder writes channel-based and channel-based-immersive content (mono, stereo, 5.0, 5.1,
5.0.4, 5.1.4) and one object substream of A-JOC or direct-coded objects ([Encoding
objects](#encoding-objects) below). `DecodedFrame.objects` reads whatever object audio a stream
carries.

```python
ac4 = ac3.ac4

config = ac4.EncoderConfig(channels=6, bitrate_kbps=256)  # 5.1: L R C LFE Ls Rs
encoder = ac4.Encoder.create(config)
decoder = ac4.Decoder()

for frame in encoder.encode(channels):  # 6 arrays of any equal length
    decoded = decoder.decode(frame.data)  # None: no output yet, not an error
    if decoded is not None:
        print(decoded.speakers, decoded.channels[0].shape)
```

`Encoder.create` raises `Ac4EncodeError` with the first rule a configuration breaks
(`ac4::Encoder::refusal_reason`, also `Encoder.refusal_reason(config)`) — every enumerator here keeps its C++ name verbatim
(`ac4.DrcMode.kDefault`, not `.Default`), same as the rest of this binding's enums. `encoder.encode`
takes a 2-D array or a sequence of 1-D arrays, one per `EncoderConfig.channels`, any equal length
— the encoder buffers input to its own frame length internally, unlike `FrameEncoder.encode_frame`'s
fixed `SAMPLES_PER_FRAME`. `encoder.flush()` pads to the end of the last frame and returns whatever
the delay still held; `encoder.toc` reads back a `Toc` snapshot whose `build_dac4()`/
`dac4_refusal()`/`media_timing()`/`samples_per_frame()` feed a container muxer the same way the
C API's `ac3forge_ac4_toc_t` accessors do, and `ac4.sync_frame(raw_frame, crc)` wraps a raw frame
for a `.ac4` file or MPEG-2 TS. `encoder.codec_mode` is what `kAuto` chose, and
`encoder.delay_samples` and `encoder.decoder_delay_samples` say where an input sample lands in the
decoded output.

`decoder.decode(frame_bytes)` returns `None` when the frame has no output yet — not an error —
and otherwise a `DecodedFrame` whose `.channels`/`.objects[].samples` are read-only `numpy` views
with the same zero-copy convention as [Zero-copy numpy](#zero-copy-numpy-and-buffer-reuse) above;
both `encoder.encode()` and `decoder.decode()` release the GIL for the underlying call.
`decoder.presentations` reads the last frame's table of contents, `decoder.metadata_loudness`
reads the selected presentation's loudness fields, `decoder.set_output()` and
`decoder.set_presentation()` change the output processing and the chosen presentation from the next
frame, and `decoder.refusal_reason`, `decoder.latency_samples` and `decoder.reset()` are the
C++ decoder's own.

### Encoding objects

`EncoderConfig.objects` takes an `ObjectsConfig`: a list of `ObjectConfig` (one input channel
each: a bed object from a loudspeaker, a dynamic object, or the LFE, with an `ObjectProperties` in
force from the first sample), how they are coded (`coding`: A-JOC, the default, or direct-coded;
`downmix`, `downmix_signals`, `decorrelation`, `parameter_bands` and the rest of
`ac4::ObjectsConfig`), and the object substream is experimental, so `experimental.objects` has to
be set as well. `ObjectProperties` is the class `DecodedObject.properties` returns, and the
Encoder takes the same fields: `x`, `y`, `z`, `gain_db`, `priority`, `width_x`/`width_y`/`width_z`,
`zone_mask`, `screen_factor`, `depth_exponent`, `distance`, `divergence`, `headphone_render_mode`
and the rest, each with the range and step its docstring gives. A new one starts from the C++
struct's defaults: priority 1, depth exponent 1, the room's centre.

```python
objects = ac4.ObjectsConfig(
    objects=[
        ac4.ObjectConfig(properties=ac4.ObjectProperties(x=0.1, y=0.2, gain_db=-3.0)),
        ac4.ObjectConfig(lfe=True),
        ac4.ObjectConfig(bed=ac4.BedChannel.kLeft),
    ],
)
config = ac4.EncoderConfig(
    bitrate_kbps=256, experimental=ac4.Experimental(objects=True), objects=objects
)
encoder = ac4.Encoder.create(config)

# One channel of PCM per object, and the changes to the objects' metadata with the input they
# belong to: from input sample 5000 of this call, object 0 moves over 1024 samples.
move = ac4.ObjectMetadataUpdate(
    object=0, sample=5000, ramp_samples=1024, properties=ac4.ObjectProperties(x=0.75, gain_db=-12.0)
)
frames = encoder.encode(pcm, updates=[move])
```

The limits are the encoder's: 1 to 64 objects, at most one the LFE and at least one that is not;
`frame_rate_index` 13 (the 2 048-sample frame) only; an A-JOC downmix of 1 to 11 signals, no more
than there are full-band objects, or a static 5.0 or 5.1 bed; direct-coded objects are dynamic
objects and the LFE with no bed objects. `Encoder.refusal_reason(config)` names the rule a
configuration breaks. With `objects` set, `codec_mode` is the object substream's, `channels` is
ignored, and `config.objects` gives a copy of the `ObjectsConfig`: assign a whole list to change
it. The decoder reports the objects in its own order, not the encoder's: the LFE first, then the
bed objects, then the dynamic objects, each group in the order the configuration lists it, and
`DecodedObject.updates` lists the block updates within the frame (`ObjectUpdate`: the output
sample each takes effect at, `ramp_samples` and `properties`).

### Exceptions

Every AC-4 failure raises a subclass of `ac3.Ac4Error`, which derives from `ValueError`: AC-4's
failures were plain `ValueError`s before they had types, and `except ValueError` still catches
every one. `Decoder.decode()` raises `Ac4DecodeError` for a frame that will not decode, unless
`DecoderConfig.concealment` supplies a frame in its place; `Encoder.create()`, `encode()` and
`flush()` raise `Ac4EncodeError` for a configuration or input the encoder refuses. Each carries the
C++ enumerator as `.error` (`ac4.DecodeError`, `ac4.EncodeError`) and `ac4::describe()` of it, or
for `create()` the refusal reason, as its message. They sit beside `Ac3Error` and are not under
it, since `Ac3Error` derives from `RuntimeError`. An argument the binding cannot read (a channel
that is not a 1-D array) stays a plain `ValueError`.

Test coverage: `python/tests/test_ac4_roundtrip.py` (stereo and 5.1 round trips checked by
correlation, presentation/delay agreement with the encoder, the channel-count refusal above, and
`ac4.sync_frame`'s sync word); `test_ac4_objects.py` (an A-JOC scene and a direct-coded one, each
configured by keywords and by attributes with the two streams the same bytes, and read back within
what each field's code can hold, with its own tone and a metadata update at the sample its input
sample comes out; the limits, the I-frame lists and the experimental flags); `test_ac4_errors.py`
(the hierarchy, the exports, and each exception's `.error`).

## What isn't exposed

`FrameEncoder`/`AtmosEncoder`'s self-check `trace` hook (`ac3::verify::FrameTrace`) and
`AtmosEncoder`'s `bed()`/`parameters()` introspection accessors are internal verification
tooling, not part of this binding's surface — the DECODE-side `trace`/`eac3_trace` on
`DecoderConfig` above is a different thing (the research export, not the
encoder/decoder mirror self-check `ac3::verify::MirrorEncoder`/`Eac3MirrorEncoder` drive
in-repo) and is exposed. `DecodedAccessUnit`/`DecodedSubstream`'s full Table E2.5 channel-map
machinery (`chanmap`, `location_map()`, `layout`) is likewise not exposed beyond the convenience
`channel_labels` list above — deliberately unsupported, and said so here, the "say so and say why"
convention `CONTRIBUTING.md` asks of the C++ side itself.

`ac3.DecoderConfig` binds `drc_scale` and `heavy_compression`, and the research `trace` and
`eac3_trace` handles. The §7.8 output stage (`output`: dialnorm normalisation, the downmix, the
operating modes), `concealment`, `programme`, `drc_boost_scale`, `fast_imdct`, `fast_mdct`,
`joc_domain`, `syntax`, `skip_reconstruction`, `skip_object_reconstruction` and `diagnostics` are
not bound: the Python decoders return the coded channels as decoded, and a fold, concealment or
the choice of one programme among several happens on the arrays or in C++.

`ac3.eac3.FrameConfig`'s `trace` hook is the same ENCODE-side omission as `FrameEncoder`'s above -
`ac3.verify.Eac3AccessUnitTrace` is decode-only from Python too, same as its AC-3 counterpart. Its
`mixmdate`/`infomdat` metadata groups, `vbr`/ABR and `search` are unmirrored too, with no reason of
design behind it (see [Encoding E-AC-3](#encoding-e-ac-3) above); `AccessUnitConfig` also lacks
`additional` (further independent programmes, I1-I7).

There is no `Eac3Decoder.decode_substream_into` — only the two forms that assemble a full
programme (`FrameDecoder.decode_frame_into`, `Eac3Decoder.decode_access_unit_into`) have a
caller-buffer form, because that is the only pair `ac3::FrameDecoder`/`ac3::Eac3Decoder`
themselves expose one for (see [Zero-copy numpy](#zero-copy-numpy-and-buffer-reuse) above); a
single substream's own PCM is always freshly allocated.

`ac3::oba::ObjectScene` (the object-scene timeline behind `ac3cli atmos-path` and the GUI's
export - see [Spatial & Atmos objects](spatial-and-atmos.md#the-scene-ac3obaobjectscene)) is not
here either. Its shape has settled: `SceneCursor` is the seam a live position source plugs into,
and the OSC wire form ([`ac3/oba/scene_osc.hpp`](spatial-and-atmos.md#the-osc-wire-form)), a
sibling header, changed nothing about `scene.hpp`. It is left out because this surface is a
candidate for the coming API freeze, where an experimental type would be a lasting commitment, and
exposing half of it - the serialisation without the type, say - would be worse than exposing none,
because a caller would get a scene it could load and not evaluate. Read and write the JSON form
from the host language and hand the resulting placements to the encoder entry points above.

---

See also: [Encoding AC-3](encoding-ac3.md), [Decoding](decoding.md),
[Spatial & Atmos objects](spatial-and-atmos.md) — the C++ APIs these bindings wrap;
[docs/releasing.md](../releasing.md#publishing-to-pypi) — how a tagged release reaches PyPI.
