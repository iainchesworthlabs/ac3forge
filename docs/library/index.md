# Using ac3::forge

`ac3::forge` is the C++23 codec library used by Forge, Crucible, and Hearth. It encodes and
decodes AC-3 and E-AC-3, including E-AC-3 streams with Dolby Atmos objects represented through
Joint Object Coding (JOC). It also provides loudness metering, level analysis, and quality
measurement.

Related targets provide container writing, IAB and ADM/BW64 reading, IAMF writing, object
signing, platform audio, and AC-4 decoding. Build and linkage requirements differ by target. [Capabilities](capabilities.md) lists supported formats and limits;
[Development status](development-status.md) is the compact done / partial / not-started companion.
[Validation](../verification.md) describes how output is checked.

Use this page to link the C++ library. Other interfaces are documented under the
[C API](c-api.md), [Python](python-api.md), [Rust](rust-api.md), and
[WebAssembly](../platforms/wasm.md) pages. Packages are listed under
[Releasing](../releasing.md#what-gets-published).

The main public headers are under `src/forge/include/ac3/`.

| CMake target | Purpose |
|---|---|
| `ac3::forge` | AC-3 and E-AC-3 encoding and decoding |
| `matroska::matroska`, `mp4::mp4`, `mpegts::mpegts` | Container writers |
| `ac3::signing` | EMDF object signing; see [Object signing](signing.md) |
| `ac3iab::ac3iab` | SMPTE ST 2098-2 IAB reading; see [IAB](iab.md) |
| `iamf::iamf` | IAMF OBU and ISOBMFF writing; see [IAMF](iamf.md) |
| `ac3adm::ac3adm` | ADM/BW64 reading and writing; opt-in with `AC3FORGE_BUILD_ADM=ON` |
| `ac3::admbridge` | Mapping between ADM objects and the Atmos encoder or decoder |
| `ac4::decoder` | AC-4 decoding; see [AC-4](ac4.md) |
| `ac4::encoder` | AC-4 encoding; see [AC-4](ac4.md#encoding-a-stream) |
| `ac4::ac4` | AC-4 sync frames, table of contents and presentations, which the decoder reads through and the encoder describes its streams with |

`ac3adm::ac3adm` and `ac3::admbridge` need the root dependency manifest's `adm` feature
(`-DVCPKG_MANIFEST_FEATURES=adm`) when building this repository with vcpkg, and are installed as
shared libraries. The packaged `ac3forge` port has no `adm` feature and does not package either
target. Their [ADM](adm.md) and [ADM bridge](adm-bridge.md) pages explain the dependency and
linkage details.

The AC-4 libraries are installed and exported under `ac4::`: `ac4::decoder_static` and
`ac4::decoder_shared`, and `ac4::encoder_static` and `ac4::encoder_shared`, each linking
`ac4::ac4_static` or `ac4::ac4_shared`. The static decoder and encoder call into `ac4::core`, an
archive with no headers that their exported targets name as a link-only dependency;
[AC-4](ac4.md#linking) has the detail.

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
`ac3::forge` and `mp4::mp4` use {fmt} for formatting (`cmake/Fmt.cmake`, and this repo's own
`vcpkg.json`; it stands in for `<format>`, which NDK r26's libc++ does not implement). Both
compile a private copy of it into their own object files (`FMT_HEADER_ONLY`, in its own inline
namespace `fmt::ac3_private`, through the `ac3::fmt_private` target wrapped in
`$<BUILD_INTERFACE:...>`) and link no {fmt} library, so the export graph names none and the
archive and the shared library each hold all of {fmt} that they call. That is what leaves the
installed package with nothing to declare. A consumer needs no {fmt} of its own, and one that has
its own, of any version, never binds to the private copy. It matters most for the static variants.
A shared library takes a linked {fmt} in at its own link step, but an archive is not linked at
all: one that had linked {fmt} would leave every consumer an unresolved `fmt::v12::vprint`, which
only the same major version of {fmt} can supply.

What a static variant does leave to the consumer's link is the C++ runtime. A CMake project links
an installed static `ac3::` target with the C++ driver when it enables the CXX language, so a C
program using `ac3::forge_c_static` needs `project(your_project LANGUAGES C CXX)`; that driver
supplies libm as well. With only C enabled the link goes through the C driver and stops at C++
runtime symbols such as `operator new`, although the exported target records that it holds C++
objects (`IMPORTED_LINK_INTERFACE_LANGUAGES`). A build outside CMake gets them from the `.pc`
files (see pkg-config below), or adds them to the link line itself (`-lstdc++ -lm` with libstdc++,
`-lc++ -lm` with libc++).

`ac3adm::ac3adm`/`ac3::admbridge` are the exception: they PRIVATE-embed the third-party
libbw64/libadm (Apache-2.0, FetchContent'd — see [ADM / BW64 reading](adm.md)), neither of which
this project installs or exports in its own right, so the installed package only ever exports
their **shared** variant (`ac3adm::ac3adm_shared`/`ac3::admbridge_shared`, plus the bare
`ac3adm::ac3adm`/`ac3::admbridge` alias — there is no `_static` counterpart here, unlike every
other module on this page) regardless of `AC3FORGE_INSTALL_BOTH_LINKAGES`. A self-contained
`.so` absorbs libbw64/libadm at its own build step; a static archive would leave a downstream
consumer with unresolved symbols into a library this package doesn't ship. `ac3adm`
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

Every library beside `ac3::forge` is one of the port's features, and none is on by default (a
curated-registry port's `default-features` may only cover behaviors, not additional public
APIs/targets/binaries, and each of these is exactly that):

| Feature | Targets |
|---|---|
| `matroska` | `matroska::matroska` |
| `mp4` | `mp4::mp4` |
| `mpegts` | `mpegts::mpegts` |
| `capi` | `ac3::forge_c`, the C API (see [C API](c-api.md)) |
| `ac4` | `ac4::ac4`, `ac4::decoder` and `ac4::encoder` (see [AC-4](ac4.md)) |
| `iab` | `ac3iab::ac3iab` (see [IAB](iab.md)) |
| `iamf` | `iamf::iamf` (see [IAMF](iamf.md)) |

Opt in with `vcpkg install ac3forge[matroska,mp4,mpegts]` for the three container writers, or any
subset, such as `ac3forge[ac4]` for AC-4 alone. `ac3adm::ac3adm`/`ac3::admbridge` have no vcpkg
feature — out of scope for this port for now, even though upstream now installs/exports both
(shared-only, see the note above). Once merged into `microsoft/vcpkg`, the same two snippets work
with a plain `vcpkg install ac3forge` — no `--overlay-ports` needed.

**Conan.** A recipe lives in this repo at
[`packaging/conan/`](https://github.com/iainchesworthlabs/ac3forge/tree/main/packaging/conan)
and is pending submission to ConanCenter (see
[docs/releasing.md](../releasing.md#conan-recipe)) — until that lands, `conan create
packaging/conan --version <tag>` from a clone of this repo builds it straight into your local
Conan cache, after which a consumer's `conanfile.txt`/`conanfile.py` `requires = "ac3forge/<tag>"`
resolves it the same way a published package would. Same scope as the vcpkg port above, with an
option for each of its features: `capi`, `ac4`, `iab` and `iamf` off by default like the port's
(`-o "ac3forge/*:ac4=True"` and the like to opt in), and `matroska`/`mp4`/`mpegts` on by default,
where the port has them off (`-o "ac3forge/*:matroska=False"` etc. to drop one).
`tools/checks/check_packaging_versions.sh` holds the two recipes to the same components and the
same `AC3FORGE_BUILD_<NAME>` options. The same two `find_package`/`target_link_libraries`
snippets apply: the recipe installs `ac3forge`'s own CMake package config rather than generating
a second one, so a Conan consumer's CMakeLists.txt looks identical to a vcpkg or plain-installed
one.

**pkg-config.** Every installed component above also gets its own `.pc` file
(`${libdir}/pkgconfig/<name>.pc` — `ac3forge`, `ac3signing`, `matroska`, `mp4`, `mpegts`,
`iamf`, `ac3iab`, `ac3adm`, `admbridge`, `ac3forge_c`, `ac4`, `ac4dec`, `ac4enc`, `ac4core`), for
a non-CMake consumer:

```bash
pkg-config --cflags --libs ac3forge
```

Picks whichever linkage was actually installed (the shared name when
`AC3FORGE_INSTALL_BOTH_LINKAGES`/`BUILD_SHARED_LIBS` selected it, else the `_static`-suffixed
one — matching what is actually on disk), and chains `Requires:` for a component that PUBLIC-
links another (`ac3signing` requires `ac3forge`; `admbridge` requires both `ac3forge` and
`ac3adm`; `ac4dec` and `ac4enc` require `ac4`). The `prefix=` line resolves relative to wherever the `.pc` file itself ends up
(`pkg-config`'s own `${pcfiledir}`), so it works the same whether that's a real system install or
an unpacked `ac3forge-dev-*` archive.

A `.pc` that names a static archive lists what the archive needs in `Requires.private` and
`Libs.private`, and pkg-config puts those on the link line only when asked for `--static`. That is
the mode for an install that holds only the static libraries, the shape a vcpkg or Conan package
has:

```bash
cc consumer.c $(pkg-config --static --cflags --libs ac3forge_c)
```

`ac3forge_c.pc` requires `ac3forge` privately, because `libac3forge_c_static.a` calls into
`libac3forge_static.a`, and `ac4dec.pc` and `ac4enc.pc` require `ac4core` privately, because
`libac4dec_static.a` and `libac4enc_static.a` call into `libac4core_static.a`. `ac3forge.pc`, `matroska.pc`, `mp4.pc`,
`mpegts.pc`, `iamf.pc`, `ac3iab.pc`, `ac4.pc` and `ac4core.pc` list the C++ runtime and libm in
`Libs.private`. A C compiler does not link them by itself, and a C++ compiler does. The names are
the ones CMake recorded for the compiler that built the archives: `-lstdc++ -lm` with libstdc++
and `-lc++ -lm` with libc++ on Linux. `ac3signing.pc` gets them through `ac3forge`, and
`ac4dec.pc` and `ac4enc.pc` through `ac4` and `ac4core`. A `.pc` that names a shared library has neither field: the library
records what it needs, and `libac3forge_c.so` holds its own copy of the codec, so it does not pull
in `libac3forge.so`. An install with both linkages, such as the `ac3forge-dev-*` packages, names
the shared libraries, and `--static` does not switch to the archives, so name them yourself:

```bash
cc consumer.c $(pkg-config --cflags --libs-only-L ac3forge_c) \
    -lac3forge_c_static -lac3forge_static -lstdc++ -lm
```

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

- [Capabilities](capabilities.md) — what ships, with spec sections and limitations.
- [Development status](development-status.md) — at-a-glance status across every codec and bitstream feature.
- [Application coverage](application-coverage.md) — which applications expose each broad capability.
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
- [IAB (SMPTE ST 2098-2) reading](iab.md) — `ac3iab::ac3iab`, a standalone Immersive Audio
  Bitstream reader, elementary `.iab` files and MXF Track Files alike (on by default).
- [ADM / BW64 reading](adm.md) — `ac3adm::ac3adm`, a standalone BW64/RF64 + Audio Definition Model
  parser (opt-in, `-DAC3FORGE_BUILD_ADM=ON`).
- [ADM → Atmos bridging](adm-bridge.md) — `ac3::admbridge`, mapping the parsed ADM graph onto
  `ac3::oba::AtmosEncoder` (same opt-in flag).
- [IAMF writing](iamf.md) — `iamf::iamf`, a standalone writer re-wrapping a decoded 7.1.4
  programme as a channel-based IAMF Audio Element in IAMF's own ISO-BMFF encapsulation (on by
  default).
- [AC-4](ac4.md) — `ac4::decoder`, `ac4::encoder` and the inspector both work through,
  `ac4::ac4`: the decoder's controls, the choice of presentation and what the decoder reports; the
  encoder's configuration, substreams and presentations; and linking (on by default).
- [Measuring quality](quality.md) — `ac3::quality`, the decoded-domain distortion measure and the
  tonality/masking model the encoder's decision search is judged on.
- [Object signing](signing.md) — `ac3::signing`, the EMDF protection tag.
- [Header map](header-map.md) — the headers a caller normally reaches for, and what lives in each.
- [API stability](api-stability.md) — the v1.0 freeze plan: header tiers, SemVer and deprecation
  policy, and what's decided versus still deliberately deferred.
- [C API](c-api.md) — `ac3::forge_c`, a stable, minimal C-callable surface over encode/decode for
  bindings and embedding.
- [Rust bindings](rust-api.md) — `ac3forge-sys` (raw, `bindgen`-generated) plus the safe
  `ac3forge` crate, both over the C API.
- [Python bindings](python-api.md) — the `ac3forge` PyPI package, pybind11-direct over
  `ac3::FrameEncoder`/`FrameDecoder`/`Eac3Decoder`/`oba::AtmosEncoder` and
  `eac3::FrameEncoder`/`AccessUnitEncoder`.
- [WebAssembly](../platforms/wasm.md) — the `ac3forge-wasm-decoder` package, built
  from this tree and not yet on the npm registry: a
  push-frame decode API, an AudioWorklet playback pipeline, and an hls.js/MSE bridge over the
  decoder compiled to WASM.

## Conventions

These hold across the whole API.

**Errors are `std::expected`.** Nothing throws for a stream-level or configuration problem.
`FrameError` covers encoding, `DecodeError` decoding, `ScanError` scanning, `WavError` file
I/O, `MuxError` muxing. All five have a `describe()` returning a `std::string_view`.

**The `ac3::` namespace tree is codec-aware; `matroska::`/`mp4::`/`mpegts::`/`ac3adm::`/`ac3iab::`
are codec-blind.** This is the namespace-level face of the header-prefix rule
[CONTRIBUTING.md](https://github.com/iainchesworthlabs/ac3forge/blob/main/CONTRIBUTING.md#repository-layout)
states for directories: everything nested under `ac3::` depends on or extends `ac3::forge`'s own
model, down to `ac3::oba`, `ac3::io`, `ac3::meta`, `ac3::verify`, `ac3::iec61937`,
`ac3::admbridge`, `ac3::audio` and `ac3::signing` — none of those are AC-3/E-AC-3-*specific*, but
all of them know the codec exists. The separate top-level namespaces know nothing about AC-3,
E-AC-3 or Atmos at all, and take frames as opaque bytes.

Within `ac3::`, AC-3 is the base case and lives in the bare namespace; E-AC-3 additions and
overrides live in `ac3::eac3`, nested rather than parallel. `ac3::FrameEncoder` (AC-3) and
`ac3::eac3::FrameEncoder` (E-AC-3) sharing a class name across that boundary is this rule applied
consistently, not an accident — the same split the Python bindings mirror by putting the E-AC-3
encoder in a real `ac3.eac3` submodule rather than a same-module name that would collide.

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
