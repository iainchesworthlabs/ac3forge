# Using ac3::forge

The public API is the headers under `src/forge/include/ac3/`. Link `ac3::forge`; link any of
`matroska::matroska`, `mp4::mp4` and `mpegts::mpegts` as well if you want a container writer,
`ac3::signing` if you want to apply the EMDF object-signing tag (see [Object signing](signing.md)),
`ac3iab::ac3iab` if you want to read a SMPTE ST 2098-2 Immersive Audio Bitstream (it links nothing
from `ac3::forge` and knows nothing about AC-3 — roadmap IM1 phase 1), or
`ac3adm::ac3adm` if you want to read or write a professional ADM BWF master — it does not need
`ac3::forge` linked alongside it on its own (`ac3::admbridge` is the module that needs both, for
mapping an ADM object graph onto/from `ac3::oba::AtmosEncoder`/`ac3::Eac3Decoder`). Unlike every
other module here, `ac3adm::ac3adm` is opt-in: it is only built with
`-DAC3FORGE_BUILD_ADM=ON` (default off), and needs several Boost header libraries pulled in via
`-DVCPKG_MANIFEST_FEATURES=adm` — see [ADM / BW64 reading](adm.md) for why. Unlike every other
module here, it and `ac3::admbridge` are **shared-only** even in an installed package — see the
note below.

**In-tree** (this repo `add_subdirectory`'d into a larger build, or as a git submodule):

```cmake
target_link_libraries(your_target PRIVATE ac3::forge)
```

`ac3::forge` resolves to whichever of the static or shared build the enclosing project's
`BUILD_SHARED_LIBS` asks for.

**Installed package**, from an `ac3forge-dev-*` package (see
[docs/releasing.md](../releasing.md#what-gets-published)) or a local `cmake --install`:

```cmake
find_package(ac3forge REQUIRED)
target_link_libraries(your_target PRIVATE ac3::forge_static)   # or ac3::forge_shared
```

An installed package has no ambient `BUILD_SHARED_LIBS` default to resolve against, so it
exports both variants explicitly rather than a bare `ac3::forge` — pick the one you want.
The package has nothing for a consumer to find: no `find_dependency()` calls, no system or
third-party library to resolve, static or shared. The codec is not dependency-free, though —
`ac3::forge` uses {fmt} for formatting (`cmake/Fmt.cmake`, and this repo's own `vcpkg.json`;
it stands in for `<format>`, which NDK r26's libc++ does not implement). That link is PRIVATE
and wrapped in `$<BUILD_INTERFACE:...>`, so it is absorbed at build time and never reaches the
export graph, which is what leaves the installed package with nothing to declare.
`ac3adm::ac3adm`/`ac3::admbridge` go further than that: they PRIVATE-embed the third-party
libbw64/libadm (Apache-2.0, FetchContent'd — see [ADM / BW64 reading](adm.md)), neither of which
this project installs or exports in its own right, so the installed package only ever exports
their **shared** variant (`ac3adm::ac3adm_shared`/`ac3::admbridge_shared`, plus the bare
`ac3adm::ac3adm`/`ac3::admbridge` alias — there is no `_static` counterpart here, unlike every
other module on this page) regardless of `AC3FORGE_INSTALL_BOTH_LINKAGES`. A self-contained
`.so` absorbs libbw64/libadm at its own build step; a static archive would leave a downstream
consumer with genuinely unresolved symbols into a library this package doesn't ship. `ac3adm`
still needs Boost at build time (see the note above) — that requirement doesn't go away just
because the *installed* artifact is self-contained.

`ac3::signing` follows this exact same shape — mandatory, not gated by an
`AC3FORGE_BUILD_<NAME>` switch, same as `ac3::forge` itself — so it resolves the identical way in
both cases: the bare `ac3::signing` alias in-tree, and explicit `ac3::signing_static`/
`ac3::signing_shared` from an installed package.

**vcpkg.** A port lives in this repo at
[`packaging/vcpkg-port/ac3forge/`](https://github.com/iainchesworthlabs/ac3forge/tree/main/packaging/vcpkg-port/ac3forge) and is pending
submission to the curated `microsoft/vcpkg` registry (see
[docs/releasing.md](../releasing.md#vcpkg-port)) — until that lands, point vcpkg at it directly
with `--overlay-ports`/`VCPKG_OVERLAY_PORTS` (works from any clone of this repo, no waiting on
the upstream PR):

```bash
vcpkg install ac3forge --overlay-ports=/path/to/ac3forge/packaging/vcpkg-port
```

```cmake
find_package(ac3forge CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE ac3::forge)
```

The three container writers and the C API are the port's `matroska`/`mp4`/`mpegts`/`capi`
features — none on by default (a curated-registry port's `default-features` may only cover
behaviors, not additional public APIs/targets/binaries, and each of these four is exactly that) —
opt in with `vcpkg install ac3forge[matroska,mp4,mpegts,capi]` (all four) or `ac3forge[mp4]`
(just `mp4`) to get `matroska::matroska`/`mp4::mp4`/`mpegts::mpegts`/`ac3::forge_c` (the C API,
see [C API](c-api.md)) available. `ac3adm::ac3adm`/`ac3::admbridge` have no vcpkg feature — out
of scope for this port for now, even though upstream now installs/exports both (shared-only, see
the note above). Once merged into `microsoft/vcpkg`, the same two snippets work with a plain
`vcpkg install ac3forge` — no `--overlay-ports` needed.

**Conan.** A recipe lives in this repo at
[`packaging/conan/`](https://github.com/iainchesworthlabs/ac3forge/tree/main/packaging/conan)
and is pending submission to ConanCenter (see
[docs/releasing.md](../releasing.md#conan-recipe)) — until that lands, `conan create
packaging/conan --version <tag>` from a clone of this repo builds it straight into your local
Conan cache, after which a consumer's `conanfile.txt`/`conanfile.py` `requires = "ac3forge/<tag>"`
resolves it the same way a published package would. Same scope and features as the
vcpkg port above (`matroska`/`mp4`/`mpegts` on by default — `-o ac3forge/*:matroska=False` etc.
to drop one — plus `capi`, off by default like the vcpkg port's own feature, `-o
ac3forge/*:capi=True` to opt in), and the same two `find_package`/`target_link_libraries`
snippets: the recipe installs `ac3forge`'s own CMake package config rather than generating a
second one, so a Conan consumer's CMakeLists.txt looks identical to a vcpkg or plain-installed one.

**pkg-config.** Every installed component above also gets its own `.pc` file
(`${libdir}/pkgconfig/<name>.pc` — `ac3forge`, `ac3signing`, `matroska`, `mp4`, `mpegts`,
`ac3iab`, `ac3adm`, `admbridge`, `ac3forge_c`), for a non-CMake consumer:

```bash
pkg-config --cflags --libs ac3forge
```

Picks whichever linkage was actually installed (the shared name when
`AC3FORGE_INSTALL_BOTH_LINKAGES`/`BUILD_SHARED_LIBS` selected it, else the `_static`-suffixed
one — matching what's genuinely on disk), and chains `Requires:` for a component that PUBLIC-
links another (`ac3signing` requires `ac3forge`; `admbridge` requires both `ac3forge` and
`ac3adm`). The `prefix=` line resolves relative to wherever the `.pc` file itself ends up
(`pkg-config`'s own `${pcfiledir}`), so it works the same whether that's a real system install or
an unpacked `ac3forge-dev-*` archive.

Live audio — capture, monitor playback, IEC 61937 passthrough — is `ac3::audio`
(`src/audio/`), a separate target `ac3cli`/`ac3gui` link alongside `ac3::forge` for their own
live-audio commands. It is **not** part of the distributed package: it isn't installed, isn't
exported, and `find_package(ac3forge)` says nothing about it. A consumer wanting live capture
on their own platform provides their own audio I/O and feeds the resulting PCM to the codec API
below directly — `ac3::audio` exists to serve this project's own CLI/GUI, not as something a
third party is expected to link.

Nearly every code block in this section is an excerpt from a program in
[`examples/`](https://github.com/iainchesworthlabs/ac3forge/tree/main/examples) — see
[Example programs](examples.md) for the full list. What the build compiles and `ctest` runs is
the programs, not the excerpts: an example cannot stop working silently, but an excerpt is
re-synced by hand and can drift. Each page's "Full program" link is the canonical form.

## In this section

- [Example programs](examples.md) — every `examples/` program, what it shows, and which page discusses it.
- [Encoding AC-3](encoding-ac3.md) — `ac3::FrameEncoder` and `EncoderConfig`.
- [Encoding E-AC-3](encoding-eac3.md) — `ac3::eac3::FrameEncoder` and wide layouts via `ac3::eac3::AccessUnitEncoder`.
- [Decoding](decoding.md) — scanning a stream with `ac3::io::scan` and decoding it.
- [Spatial & Atmos objects](spatial-and-atmos.md) — the plain-AC-3 object layer and `ac3::oba::AtmosEncoder`.
- [A worked scene — station broadcast](station-broadcast.md) — a complete 115-second authored Atmos scene built on the object APIs.
- [Channel plans & routing](channel-plans-and-routing.md) — custom channel selections and multi-source assignment.
- [Metadata](metadata.md) — loudness, DRC and downmix metadata.
- [Muxing & sinks](muxing-and-sinks.md) — `matroska::mux`, `mp4::mux`, fMP4/CMAF + HLS/DASH
  (`mp4::fragment`, `mp4/hls.hpp`, `mp4/dash.hpp`), metering, the IEC 61937/passthrough/monitor
  sinks, and capture.
- [File I/O](file-io.md) — reading and writing WAV.
- [ADM / BW64 reading](adm.md) — `ac3adm::ac3adm`, a standalone BW64/RF64 + Audio Definition Model
  parser (opt-in, `-DAC3FORGE_BUILD_ADM=ON`).
- [ADM → Atmos bridging](adm-bridge.md) — `ac3::admbridge`, mapping the parsed ADM graph onto
  `ac3::oba::AtmosEncoder` (same opt-in flag).
- [Measuring quality](quality.md) — `ac3::quality`, the decoded-domain distortion measure and the
  tonality/masking model the encoder's decision search is judged on.
- [Object signing](signing.md) — `ac3::signing`, the EMDF protection tag.
- [Header map](header-map.md) — the headers a caller normally reaches for, and what lives in each.
- [C API](c-api.md) — `ac3::forge_c`, a stable, minimal C-callable surface over encode/decode for
  bindings and embedding (roadmap item F1).
- [Python bindings](python-api.md) — the `ac3forge` PyPI package, pybind11-direct over
  `ac3::FrameEncoder`/`FrameDecoder`/`Eac3Decoder`/`oba::AtmosEncoder` and
  `eac3::FrameEncoder`/`AccessUnitEncoder`.

## Conventions

These hold across the whole API.

**Errors are `std::expected`.** Nothing throws for a stream-level or configuration problem.
`FrameError` covers encoding, `DecodeError` decoding, `ScanError` scanning, `WavError` file
I/O, `MuxError` muxing. `DecodeError`, `ScanError`, `WavError` and `MuxError` each have a
`describe()` returning a `std::string_view`; `FrameError` does not.

**Audio is `float`, nominally in [-1, 1).** Internally the transform runs in `double`.

**Channels are passed as `std::span<const std::span<const float>>`.** The inner spans must
outlive the call. Build the outer vector once and refill the buffers underneath it — a fresh
vector of spans per frame is a pure waste.

**Channel order is A/52 Table 5.8, not WAV order.** That is `L, C, R, SL, SR` with LFE last,
against WAVE_FORMAT_EXTENSIBLE's `FL, FR, FC, LFE, BL, BR`. `ac3::io::ac3_layout_for` and
`ac3::io::wav_channel_order` give you the permutation both ways; use them rather than writing
it out again.

**Encoders are stateful and per-stream.** They carry MDCT overlap, the 44.1 kHz rate
accumulator, and the DRC and heavy-compression controllers, all of which smooth across frames.
One encoder per stream, fed in order. The decoders are stateful the same way (overlap-add and
dither state). No encoder or decoder instance is safe for concurrent calls on the same
instance — the headers note that per-frame scratch and history members are reused across
calls — but separate instances share nothing and are independent.

**Each `encode_frame` call takes exactly one frame of PCM per channel.** For AC-3 that is always
`ac3::kSamplesPerFrame` (1536); for E-AC-3 it is `FrameEncoder::samples_per_frame()`, which is
1536 unless `FrameConfig::numblkscod` shortens the syncframe (256, 512 or 768 — see
[Encoding E-AC-3](encoding-eac3.md)). Short-changing it is a programming error, not a runtime
one.

**Every class with non-trivial state hides it behind a pimpl.** `struct Impl;
std::unique_ptr<Impl> impl_;` is the only private member on `FrameEncoder`
(both codecs), `FrameDecoder`, `Eac3Decoder`, `oba::AtmosEncoder`,
`eac3::AccessUnitEncoder`, `meta::RangeController`/`HeavyCompressor`,
`meta::LoudnessMeter`, `analysis::LevelMeter`, `iec61937::Eac3BurstPacker` and
the three `io::Wav*` classes that started the pattern — adding a buffer or
growing a scratch array changes only `Impl`, defined in the `.cpp`, so it is
never an ABI break for a caller linking `ac3::forge_shared`. The five plain
config aggregates (`EncoderConfig`, `DecoderConfig`, `AtmosConfig`,
`FrameConfig`, `AccessUnitConfig`) are the deliberate exception: callers build
them with designated initializers, so they stay ordinary value types rather
than opaque handles, and that ergonomics is worth more than hiding four or
five `double`s. Their layout is what `SameMajorVersion` actually has to
promise once 1.0 ships: a config struct's fields are frozen at the release
that adopts full-version `SOVERSION`, and a field added afterward needs either
a major version bump or an additive extension point (a reserved trailing
field, or a new sibling struct referenced by pointer) rather than an in-place
insert, which would silently shift every later field's offset for anyone who
has not recompiled. The `verify::*Trace*`/`FrameSyntax*` pointers a few of
them carry (`EncoderConfig::trace`, `DecoderConfig::trace`/`eac3_trace`/
`syntax`, `FrameConfig::trace`) are non-owning observers into internal
instrumentation headers, not part of the frozen public surface themselves —
adding, removing or retyping one of those pointers is not a promise this
convention covers.
