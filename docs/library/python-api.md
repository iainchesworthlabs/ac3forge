# Python bindings

Roadmap **F2**: a pybind11 module (`python/src/ac3forge_ext/bindings.cpp`) bound straight onto
`ac3::FrameEncoder`, `ac3::FrameDecoder`, `ac3::Eac3Decoder`, `ac3::eac3::FrameEncoder`,
`ac3::eac3::AccessUnitEncoder` and `ac3::oba::AtmosEncoder` — pybind11-direct, not layered on a
separate C API. Install from PyPI:

```bash
pip install ac3forge
```

or, from a source checkout of this repository, build against the same CMake tree everything else
here uses:

```bash
pip install ./python
```

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

`ac3.EncoderConfig` mirrors `ac3::EncoderConfig` field for field (`encoder/encoder.hpp`) —
construct it with keyword arguments for whichever fields you want to change from their C++
defaults; an unrecognised keyword raises `TypeError` rather than being silently ignored:

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

Roadmap **AP6**. Every `encode_frame`/`encode_access_unit` call above (AC-3, E-AC-3, and
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

Roadmap **AP6**: `ac3.scan()` wraps `ac3::io::scan` (`ac3/io/elementary.hpp`) — reading an
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

Roadmap AP12. `ac3.verify.FrameTrace`/`Eac3AccessUnitTrace` are caller-owned handles that
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

Roadmap **AP6**: `ac3.eac3.FrameEncoder`/`AccessUnitEncoder` wrap `ac3::eac3::FrameEncoder`/
`AccessUnitEncoder` directly (pybind11-direct, like everything else in this binding) — a real
submodule rather than a flat `Eac3FrameEncoder` name, since `ac3::FrameEncoder` and
`ac3::eac3::FrameEncoder` share a name across C++ namespaces (roadmap AP2); `ac3.FrameEncoder`
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
metadata groups, `vbr`/ABR (`EQ12`) and `search` (`EQ7`/`EQ13`'s per-frame bit-allocation codes
search) — real gaps, not design decisions, unlike the C API's own documented trim.

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

All three derive from `ac3.Ac3Error(RuntimeError)`. `ac3.FrameError` has no C++-side `describe()`
(see [docs/library/index.md](index.md#conventions)'s own note — some codec-level failures never
got a text description on the C++ side either), so an `Ac3EncodeError`'s message is the
enumerator's own name; `ac3.DecodeError`/`ac3.ScanError` both do (`ac3.describe`, overloaded for
either), so an `Ac3DecodeError`/`Ac3ScanError`'s message is real spec-level text.

A wrong-length or wrong-count channel array (not `ac3.SAMPLES_PER_FRAME` samples, or not
`channel_count` of them) raises a plain `ValueError` instead — that is a Python-level usage
error, not a codec-level failure the C++ side can report at all (short-changing `encode_frame` is
documented as "a programming error, not a runtime one"). `decode_frame_into`/
`decode_access_unit_into`'s `out` gets the same treatment for its own shape: too few buffers, a
buffer that's too short, or one that isn't C-contiguous or writeable all raise `ValueError`; the
wrong dtype raises `TypeError`. See [Zero-copy numpy](#zero-copy-numpy-and-buffer-reuse) above.

## Containers, metering, QC and signing

Roadmap AP6's completeness pass added four submodules, each pybind11-direct over the same C++
classes every other binding here wraps:

- **`ac3forge.containers`** — the three container writers and the batch read side, bytes in /
  bytes out: `mux_matroska`/`mux_mp4`/`mux_mpegts` over `MatroskaTrack`/`Mp4Track`/`TsTrack`
  (kwargs constructors, the same convention every config class here uses), and
  `demux_matroska`/`demux_mp4`/`demux_mpegts` bringing frames back out. `Mp4Track.codec_config`
  takes the `dac3`/`dec3` payload `ac3forge.build_codec_config_box(stream)` produces — built
  straight off the bitstream, never off whatever a source container declared, exactly like
  `ac3cli mp4`. The incremental `Reader`/`Writer` classes and the fragmented-MP4/HLS/DASH
  surface stay C++-only for now — a boundary, not a silent gap.
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

## What isn't exposed

`FrameEncoder`/`AtmosEncoder`'s self-check `trace` hook (`ac3::verify::FrameTrace`) and
`AtmosEncoder`'s `bed()`/`parameters()` introspection accessors are internal verification
tooling, not part of this binding's surface — the DECODE-side `trace`/`eac3_trace` on
`DecoderConfig` above is a different thing (roadmap AP12's research export, not the
encoder/decoder mirror self-check `ac3::verify::MirrorEncoder`/`Eac3MirrorEncoder` drive
in-repo) and is exposed. `DecodedAccessUnit`/`DecodedSubstream`'s full Table E2.5 channel-map
machinery (`chanmap`, `location_map()`, `layout`) is likewise not exposed beyond the convenience
`channel_labels` list above — deliberately unsupported for now, the same "say so and say why"
convention `CONTRIBUTING.md` asks of the C++ side itself, not a silent gap.

`ac3.eac3.FrameConfig`'s `trace` hook is the same ENCODE-side omission as `FrameEncoder`'s above -
`ac3.verify.Eac3AccessUnitTrace` is decode-only from Python too, same as its AC-3 counterpart. Its
`mixmdate`/`infomdat` metadata groups, `vbr`/ABR and `search` are unmirrored too, but those **are**
gaps rather than decisions (see [Encoding E-AC-3](#encoding-e-ac-3) above) — `AccessUnitConfig` is
also missing `additional` (further independent programmes, I1-I7).

There is no `Eac3Decoder.decode_substream_into` — only the two forms that assemble a full
programme (`FrameDecoder.decode_frame_into`, `Eac3Decoder.decode_access_unit_into`) have a
caller-buffer form, because that is the only pair `ac3::FrameDecoder`/`ac3::Eac3Decoder`
themselves expose one for (see [Zero-copy numpy](#zero-copy-numpy-and-buffer-reuse) above); a
single substream's own PCM is always freshly allocated.

`ac3::oba::ObjectScene` (the object-scene timeline behind `ac3cli atmos-path` and the GUI's
export - see [Spatial & Atmos objects](spatial-and-atmos.md#the-scene-ac3obaobjectscene)) is not
here either, and that is a decision rather than an omission - but no longer the shape-instability
one it used to be. `SceneCursor` existed precisely because the seam a live position source would
plug into wasn't finished; roadmap `UX4`'s OSC wire form
([`ac3/oba/scene_osc.hpp`](spatial-and-atmos.md#the-osc-wire-form)) has since landed as a sibling
header, and it changed nothing about `scene.hpp`: no method on `ObjectScene`/`SceneCursor` gained
or lost a parameter, nothing was added to either class. The shape has settled. What is left is a
plain "not done yet": this surface is a candidate for the coming API freeze, where an
experimental type would be a lasting commitment, and exposing half of it - the serialisation
without the type, say - would still be worse than exposing none, because a caller would get a
scene it could load and not evaluate. Read and write the JSON form from the host language and
hand the resulting placements to the encoder entry points above until it is exposed properly.

---

See also: [Encoding AC-3](encoding-ac3.md), [Decoding](decoding.md),
[Spatial & Atmos objects](spatial-and-atmos.md) — the C++ APIs these bindings wrap;
[docs/releasing.md](../releasing.md#publishing-to-pypi) — how a tagged release reaches PyPI.
