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

`encode_frame` takes a sequence of 1-D `float32`-convertible arrays (any array-like `numpy`
accepts — a list also works), one per channel, **AC-3 channel order** (Table 5.8, LFE last —
same convention as the C++ API, see [docs/library/index.md](index.md#conventions)), each exactly
`ac3.SAMPLES_PER_FRAME` (1536) samples. It returns one syncframe as `bytes`.

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

`decoded.channels` is a list of `numpy.float32` arrays, one per channel, in decode order.
`decoded.channel_labels` is the same channels' Table 5.8/Table E2.5 names as plain strings
(`["L", "C", "R", "Ls", "Rs", "LFE"]`) — not part of the C++ `DecodedFrame`/`DecodedSubstream`
structs themselves, added here purely for convenience.

`ac3.split_frames`/`ac3.split_access_units` wrap the free functions of the same name in
`ac3/decoder/decoder.hpp` — splitting a raw elementary stream (or one already known to be E-AC-3)
into individual syncframes or access units before decoding each one.

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
| `ac3.Ac3DecodeError` | `FrameDecoder.decode_frame`, `Eac3Decoder.decode_substream`/`decode_access_unit`, `ac3.split_frames`/`split_access_units`/`stream_bsid` | `ac3.DecodeError` |

Both derive from `ac3.Ac3Error(RuntimeError)`. Both `ac3.FrameError` and `ac3.DecodeError` have a
C++-side `describe()` (see [docs/library/index.md](index.md#conventions)), so an `Ac3EncodeError`'s
and an `Ac3DecodeError`'s message is real spec-level text, not the enumerator's own name.

A wrong-length channel array (not `ac3.SAMPLES_PER_FRAME` samples) raises a plain `ValueError`
instead — that is a Python-level usage error, not a codec-level failure the C++ side can report at
all (short-changing `encode_frame` is documented as "a programming error, not a runtime one").

## What isn't exposed

`FrameEncoder`/`AtmosEncoder`'s self-check `trace` hook (`ac3::verify::FrameTrace`) and
`AtmosEncoder`'s `bed()`/`parameters()` introspection accessors are internal verification
tooling, not part of this binding's surface. `DecodedAccessUnit`/`DecodedSubstream`'s full
Table E2.5 channel-map machinery (`chanmap`, `location_map()`, `layout`) is likewise not exposed
beyond the convenience `channel_labels` list above — deliberately unsupported for now, the same
"say so and say why" convention `CONTRIBUTING.md` asks of the C++ side itself, not a silent gap.

`ac3.eac3.FrameConfig`'s `trace` hook is the same omission as `FrameEncoder`'s above. Its
`mixmdate`/`infomdat` metadata groups, `vbr`/ABR and `search` are unmirrored too, but those **are**
gaps rather than decisions (see [Encoding E-AC-3](#encoding-e-ac-3) above) — `AccessUnitConfig` is
also missing `additional` (further independent programmes, I1-I7).

`ac3::oba::ObjectScene` (the object-scene timeline behind `ac3cli atmos-path` and the GUI's
export - see [Spatial & Atmos objects](spatial-and-atmos.md#the-scene-ac3obaobjectscene)) is not
here either, and that is a decision rather than an omission: the shape of the type is expected to
move when a live position source lands (roadmap `UX4`, which is why `SceneCursor` exists), and
this surface is a candidate for the coming API freeze, where an experimental type would be a
lasting commitment. Exposing half of it - the serialisation without the type, say - would be
worse than exposing none, because a caller would get a scene it could load and not evaluate.
Read and write the JSON form from the host language and hand the resulting placements to the
encoder entry points above until the type settles.

---

See also: [Encoding AC-3](encoding-ac3.md), [Decoding](decoding.md),
[Spatial & Atmos objects](spatial-and-atmos.md) — the C++ APIs these bindings wrap;
[docs/releasing.md](../releasing.md#publishing-to-pypi) — how a tagged release reaches PyPI.
