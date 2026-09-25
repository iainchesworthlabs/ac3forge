# ADM / BW64 reading and writing: `ac3adm::ac3adm`

`ac3adm/ac3adm.hpp`, library `ac3adm::ac3adm`. A standalone BW64/RF64 + Audio Definition Model
(ADM) parser and writer: the professional delivery format Netflix's and Apple's own Atmos ingest
pipelines require. Like `matroska::matroska`, `mp4::mp4` and `mpegts::mpegts`, it links nothing
from `ac3::forge` — it has no idea AC-3, E-AC-3 or the JOC/Atmos object layer exist.

Mapping the graph this module parses onto `ac3::oba::AtmosEncoder` (ADM → encode) or building it
from a decoded `ac3::Eac3Decoder` programme (decode → ADM) is a separate module,
[`ac3::admbridge`](adm-bridge.md); driving the read direction end to end — a real ADM BWF master
straight to a DD+ JOC E-AC-3 stream — is `ac3cli atmos-adm`, and the write direction is
`ac3cli decode ... adm_out` (see [Commands](../forge/cli/commands.md)) and
[`examples/encode_adm.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/encode_adm.cpp). This page and
[`examples/read_adm.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/read_adm.cpp) only demonstrate this module's own read-side API — opening a file and walking the
parsed graph; `encode_adm.cpp` is the one that shows the full read-direction pipeline. See
"Writing" below for `write_bw64()`.

**Opt-in, unlike every other module in this library.** `AC3FORGE_BUILD_ADM` defaults **off**, and
turning it on additionally needs `-DVCPKG_MANIFEST_FEATURES=adm` (see
[`vcpkg.json`](https://github.com/iainchesworthlabs/ac3forge/blob/main/vcpkg.json)) to resolve its Boost dependency:

```bash
cmake --preset config-windows-msvc-debug -DAC3FORGE_BUILD_ADM=ON -DVCPKG_MANIFEST_FEATURES=adm
```

Every other target in this project — `ac3cli`, `ac3gui`, `ac3tests`, every other example — builds
identically whether `AC3FORGE_BUILD_ADM` is on or off; nothing links `ac3adm::ac3adm`
unconditionally. See "Why opt-in" below for the reasoning.

```cpp
const auto document = ac3adm::parse_bw64(fixture_path);
if (!document) {
    fmt::printf("parse_bw64 failed: %.*s\n", static_cast<int>(ac3adm::describe(document.error()).size()),
                ac3adm::describe(document.error()).data());
    return 1;
}
```

```cpp
for (const auto& programme : document->model.programmes) {
    fmt::printf("  programme %s (%s) -> %zu content(s)\n", programme.id.c_str(), programme.name.c_str(),
                programme.content_refs.size());
}
for (const auto& object : document->model.objects) {
    fmt::printf("  object %s (%s), start=%.5fs, %zu track UID ref(s)\n", object.id.c_str(), object.name.c_str(),
                object.start_s, object.track_uid_refs.size());
}
```

Full program: [`examples/read_adm.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/read_adm.cpp) — writes a small but valid BW64 fixture (adapted
from Recommendation ITU-R BS.2076-2's own worked "Car" object example) to a temp file, since a
real ADM BWF master is production audio this project has no license to embed, then parses it
back and prints what it found.

## What gets parsed

- **The container** (Recommendation ITU-R BS.2088-1, Annex 1): `<fmt >`/`<data>` integer PCM
  (8/16/24/32-bit) and IEEE float (32/64-bit — see "PCM formats" below), the `<ds64>` 64-bit
  size table for `RF64`/`BW64`-headed files, `<chna>` (the track-number ↔ ADM-ID join table) and
  `<axml>` (the embedded ADM XML document itself). A plain `RIFF` header is accepted too, for
  the (very common) case of a master that stays under the 4 GB threshold RF64 exists to lift.
- **The ADM object graph** (Recommendation ITU-R BS.2076-2, Annex 1): `audioProgramme` →
  `audioContent` → `audioObject` → `audioPackFormat`/`audioChannelFormat` (with its
  `audioBlockFormat` time-divisions — position, gain, width/height/depth, `channelLock`,
  `jumpPosition`, HOA order/degree/normalization) → `audioStreamFormat`/`audioTrackFormat` →
  `audioTrackUID`. See [`ac3adm/model.hpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/src/ac3adm/include/ac3adm/model.hpp) for exactly which sub-elements are carried and which
  are deliberately out of phase 1's scope (`zoneExclusion`, `objectDivergence`, `screenRef`, the
  Matrix/Binaural-specific sub-elements, and loudness metadata — `ac3::meta::loudness` already
  measures loudness independently).

**`model` always includes BS.2076-2 Annex A's "common definitions".** libadm's own `parseXml()`
starts every document from a copy already populated with the standard's ~940 predefined
pack/channel/stream/track/block formats (one set per standard loudspeaker layout and first- to
third-order HOA component) and merges the file's own content into it, so that a file referencing
a common-definition ID (e.g. a stereo bed's pack format `AP_00010002`) without locally
re-declaring it still resolves. This module keeps that merge rather than filtering it back out:
phase 2 needs exactly this, a pack/channel/stream/track format reference that resolves regardless
of whether the file re-declared it — so `model.pack_formats`/`channel_formats`/`stream_formats`/
`track_formats` are never just "what this one file defined". `model.programmes`/`contents`/
`objects`/`track_uids` are unaffected (the common set defines none of those four).

`AdmDocument` — the `parse_bw64` result — holds all three pieces together: `model` (the graph
above), `chna` (the join table, one `ChnaEntry` per physical-track-to-ADM-ID row), and `audio`
(the decoded PCM, one `std::vector<float>` per channel, same `[-1, 1)` normalization convention
`ac3::io::WavData` uses). `AdmError` covers open/parse failure — `kCannotOpen`, `kNotRiff`,
`kMalformedXml`, `kMalformedAdm`, `kOther`;
see [`ac3adm/ac3adm.hpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/src/ac3adm/include/ac3adm/ac3adm.hpp) for the full list. In practice, the two libraries underneath this
module (see below) report almost everything through one broad exception family each, so most
real failures currently surface as `kCannotOpen` (bad/truncated container), `kMalformedXml` (axml
isn't well-formed XML) or `kMalformedAdm` (well-formed XML that isn't a valid ADM document) — see
[`src/ac3adm/src/adm.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/src/ac3adm/src/adm.cpp)'s own comments for exactly which library exception maps to which `AdmError`.

## Writing

`write_bw64(path, document)` is the read side's mirror image: it turns an `AdmModel` (the same
plain-data graph `parse_bw64` produces) into a libadm `adm::Document` (a new translator,
`build_libadm_document()` in `src/ac3adm/src/adm_model.cpp`, alongside the existing read-side
`build_adm_model()`), serializes it with `adm::writeXml()`, and writes the BW64 container
(`<fmt >`/`<chna>`/`<axml>`/`<data>`) with libbw64's `Bw64Writer` (`bw64::writeFile()`) — the same
two vendored libraries as the read side, in the other direction. Always 24-bit integer PCM
(`ac3adm::kWriteBitDepth`; `EBU Tech 3306`'s own framing). The pinned libbw64 can write IEEE
float, but `write_bw64` has no option that asks for it.

```cpp
ac3adm::AdmDocument document;
// ... populate document.model / document.chna / document.audio ...
const auto written = ac3adm::write_bw64(path, document);
if (!written) {
    fmt::printf("write_bw64 failed: %.*s\n", static_cast<int>(ac3adm::describe(written.error()).size()),
                ac3adm::describe(written.error()).data());
    return 1;
}
```

One asymmetry from the read side: `AdmModel`'s own ID strings (`AudioObject::id`,
`ChnaEntry::uid`, ...) are used only as correlation keys while the object graph is wired together
— they never appear literally in the written file. Real, BS.2076-2-formatted IDs come from
libadm's own `adm::reassignIds()`, called once the whole graph is built; `write_bw64` reads those
back to build `<chna>`'s `AudioId` rows. A caller is therefore free to use any unique, stable
strings for `id`/`uid` fields, not just the `"AO_1001"`-style ones `parse_bw64` itself produces.
`ChnaEntry::track_ref`/`pack_ref` are consequently read-path-only fields — `write_bw64` derives
the real `trackRef`/`packRef` strings itself, so a caller building a document purely to write it
may leave both empty.

`AudioTrackUid::has_bit_depth`/`bit_depth` are not read on write either. Every `audioTrackUID` is
written with `bitDepth` equal to the `<fmt >` chunk's bits per sample (`kWriteBitDepth`), whatever
the model says, because a value carried over from another file (a 16-bit master the model was
parsed from, say) would contradict the `<fmt >` chunk written beside it. The Dolby Atmos Master ADM
Profile expects the two to agree: Dolby Encoding Engine refuses a master whose `audioTrackUID`s
leave `bitDepth` out ("Mismatched track bit depth between ADM and WAV"). `sampleRate` is written
from the model wherever `has_sample_rate` is set. On the read side both attributes are optional
(BS.2076-2 §5.9), and `parse_bw64` reports which were present through `has_sample_rate` and
`has_bit_depth`.

`write_bw64`'s own translator supports exactly the element shapes [ADM → Atmos bridging](adm-bridge.md)'s
write direction (`ac3::admbridge::write()`) produces: `audioProgramme` → `audioContent` →
`audioObject` (no nesting) → `audioPackFormat` (`Objects` or `DirectSpeakers`, no nesting) →
`audioChannelFormat` (cartesian `audioBlockFormat`s only) → `audioStreamFormat` → `audioTrackFormat`
→ `audioTrackUID`. `ac3::admbridge::write()` always populates the full `audioStreamFormat`/
`audioTrackFormat` chain rather than BS.2076-2's plain-PCM shortcut (`audioTrackUID` referencing
`audioPackFormat`/`audioChannelFormat` directly, with no stream/track format at all) — libadm's own
`adm::reassignIds()` zeroes out any `audioChannelFormat` no `audioStreamFormat` references ("get an
Id with the value zero and are thereby marked as ADM elements which should be ignored" -
`adm/utilities/id_assignment.hpp`'s own doc comment), which the shortcut alone triggers; every
channel this writer produced collapsed to the same `AC_00000000` id before this chain was added.
`AdmWriteError::kInvalidDocument` covers every case outside that shape: an unresolved `*_refs`
entry, Matrix/HOA/Binaural pack or channel types, nested references, or a block whose position is
polar rather than cartesian.

## Built on the EBU's own reference implementations

Unlike every other module in this project, `ac3adm::ac3adm` is not a from-scratch, dependency-free
implementation. It is a thin translation layer over two vendored third-party libraries, fetched
via CMake `FetchContent` (see [`src/ac3adm/CMakeLists.txt`](https://github.com/iainchesworthlabs/ac3forge/blob/main/src/ac3adm/CMakeLists.txt)):

- **[libbw64](https://github.com/pwnified/libbw64)** (Apache-2.0, header-only, no dependency of
  its own) — the BW64/RF64 chunk-walking and PCM-decoding layer, including native IEEE-float
  support. Fetched from a maintained fork of the EBU's own `github.com/ebu/libbw64`, pinned to a
  commit rather than a tag or branch — see
  [`src/ac3adm/CMakeLists.txt`](https://github.com/iainchesworthlabs/ac3forge/blob/main/src/ac3adm/CMakeLists.txt)
  for why the EBU's own repository is not what this module fetches, and
  [the threat model](../threat-model.md#adm-xml-and-bw64) for what the pin does and does not
  cover. Patched at populate time by
  [`src/ac3adm/patch_libbw64.cmake`](https://github.com/iainchesworthlabs/ac3forge/blob/main/src/ac3adm/patch_libbw64.cmake)
  for two behaviours this module's own tests need that the pinned commit does not have by
  default (a truncated recording reading as far as it goes; 64-bit float actually reaching the
  decode this fork's own utilities already support) — see that script.
- **[libadm](https://github.com/ebu/libadm)** (Apache-2.0) — the ADM XML object model: parsing,
  schema validation, and the full element graph.

libadm is the EBU/BBC/IRT team's own repository, the same team that authored the underlying ITU-R
Recommendations (BS.2088-1, BS.2076-2) themselves; libbw64's fork carries that team's original
code forward with fixes of its own on top. Using both means this module's own code only has to
translate an already-validated object graph into `ac3adm`'s own types
([`src/ac3adm/src/adm_model.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/src/ac3adm/src/adm_model.cpp)), rather than re-implementing container-walking and XML/schema
validation this project has no comparative advantage in getting exactly right on the first try.
An earlier attempt at exactly that hand-rolled approach is what prompted switching to these
libraries instead.

Neither library's own types appear in `ac3adm::ac3adm`'s public headers
(`ac3adm/ac3adm.hpp`, `ac3adm/model.hpp`) — they are translated into this module's own types at
the boundary and stay an implementation detail, the same way this project keeps every other
vendored dependency (e.g. Catch2 in `tests/`) out of its own public API. One practical reason
beyond the usual "don't leak a dependency's API" one: libadm's own public C++ namespace is `adm`,
which is why this module is `ac3adm` and not simply `adm` — the two would otherwise collide
(`adm::AudioObject`, `adm::TypeDefinition`, `adm::Position`, ... are all defined by both).

## Why opt-in

libadm needs several Boost header libraries (Optional, Variant, Range, Iterator, Functional,
Format, Integer and Rational — the exact list confirmed by grepping libadm 0.14.0's own
`#include <boost/...>` directives, not guessed from its README, which undersells it: Rational
and Integer aren't mentioned there at all, but `adm/utilities/time_conversion.hpp` needs both).
Every other dependency in this project is either in-tree or a single small vcpkg
package (Catch2); pulling in Boost is a materially bigger footprint, so it is deliberately
**opt-in rather than default-on** — `AC3FORGE_BUILD_ADM` defaults `OFF` (unlike
`AC3FORGE_BUILD_MATROSKA`/`AC3FORGE_BUILD_MP4`/`AC3FORGE_BUILD_MPEGTS`, which default `ON`), and
turning it on needs the dedicated `adm` vcpkg feature to resolve those Boost packages. A build
with `AC3FORGE_BUILD_ADM=OFF` (the default) never touches `find_package(Boost)`, never fetches
libbw64/libadm, and behaves identically to a build of this project before this module existed.

`ac3adm::ac3adm` IS part of the installed `find_package(ac3forge)` package (see
[the library overview](index.md)), but **shared-only** — unlike every other module here, there is
no `ac3adm::ac3adm_static` to link against, since a static archive would leave a downstream
consumer with unresolved symbols into libbw64/libadm (neither installed/exported by this project
in its own right); a self-contained `.so` absorbs both at its own build step instead. It is
**not** wired into the Android/Shield NDK build — see `src/ac3adm/CMakeLists.txt`'s own header
comment for both points.

## PCM formats

Integer PCM (8/16/24/32-bit) and IEEE float (32/64-bit) both read, through the vendored libbw64
directly, and both come back as the same `[-1, 1)` floats on `PcmAudio::channels`.
`bits_per_sample` reports the container width and is not, on its own, a statement about which of
the two it was.

This module used to need a second, hand-rolled container walk for float specifically: the EBU's
own libbw64 refuses any `<fmt >` `formatTag` but PCM outright at open time
(`"format unsupported: <tag>"`), and patching a dependency fetched at a pinned tag to teach it a
feature of this project's own was judged a worse standing cost than a second, narrower reader.
The pinned fork (see above) added native `WAVE_FORMAT_IEEE_FLOAT` support upstream of this
module, so that second reader (`float_pcm_bw64.hpp`, retired) is no longer needed — both formats
go through the identical libbw64 read and the identical libadm `<axml>` parse now, so there is
exactly one place either one could come out differently depending on how the samples were
stored, not two.

A file libbw64 opens and then rejects surfaces as `AdmError::kCannotOpen`; its exceptions carry
no type this module could map to anything more specific — see
[`ac3adm.hpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/src/ac3adm/include/ac3adm/ac3adm.hpp)'s
own comment on `AdmError`.

Most real ADM BWF masters are 16- or 24-bit integer (EBU Tech 3306/BS.2088-1 Annex 2 §2's own
PCM-only framing). Float ones exist too.

---

See also: [File I/O](file-io.md) — the plain-WAV reader this module's container-parsing
deliberately does not share an implementation with, despite the family resemblance;
[ADM → Atmos bridging](adm-bridge.md) — `ac3::admbridge`, which maps this graph onto
`ac3::oba::AtmosEncoder`; [Spatial & Atmos objects](spatial-and-atmos.md) — the
`ac3::oba::AtmosEncoder`/`ac3::oba::motion` surface that bridge drives.
