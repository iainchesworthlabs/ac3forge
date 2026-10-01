# ICL Forge Rust bindings

Roadmap item AP9: the first non-Python binding over `iclforge_c/iclforge.h`. Two crates:

- **`iclforge-sys`** — raw, `bindgen`-generated FFI declarations. Nothing here is hand-written;
  `build.rs` regenerates them from the header at every build, against the exact library it also
  compiled from source, so header drift is caught the moment it happens rather than discovered by
  a consumer months later.
- **`iclforge`** — a safe, idiomatic wrapper over it: `Result`/`Option` instead of status codes
  and out-parameters, RAII handles, slices instead of raw pointers.

## Why this lives in-tree

AP10 (an out-of-tree GStreamer element) is out-of-tree for a reason specific to it — it sits
inside FFmpeg's own build under GPL framing concerns. None of that applies here: this is new code
with no host-project entanglement, and bindgen regenerated against a header at a *different*
commit than the library it links is exactly the drift this binding exists to catch — which only
works if both come from the same checkout on every PR.

The precedent already in this repo is `apps/android`: a real Gradle/NDK build living in-tree,
never wired into the root `CMakeLists.txt`, built and tested by its own CI job
(`_build.yml`'s `build-android`). This crate follows the same shape for Cargo: committed, owned,
covered by CI (`build-rust`), but never `add_subdirectory()`'d from the root — Cargo stays out of
the CMake configure the same way Gradle does.

## Building

You need, on `PATH`: **CMake** (the same minimum this project already requires — see the root
`CMakeLists.txt`), a **C++23 compiler**, and **libclang** (bindgen's own dependency — set
`LIBCLANG_PATH` if it isn't auto-detected; on Windows it ships next to `clang.exe` in an LLVM
install). No vcpkg toolchain file is needed: `build.rs` configures with `ICLFORGE_BUILD_CAPI` on,
the CLI, GUI, tests, examples, fuzzers, ADM bridge, container writers and Hearth off, and the rest at
their defaults, AC-4 included (a trimmed set like the one `python/pyproject.toml` uses for its own
extension-module build), and the one dependency that survives that ({fmt}) resolves via
`find_package(CONFIG)` with a `FetchContent` fallback (`cmake/Fmt.cmake`).

```bash
cargo build --workspace
cargo test --workspace
cargo run --example encode_decode_ac3 -p iclforge
cargo run --example encode_decode_eac3 -p iclforge
```

The first build compiles the codec core from source (a few minutes); after that, `build.rs`'s
CMake step is incremental like any other CMake build.

### How linking works

`build.rs` builds `forge_c_shared` specifically (not `forge_c_static`): `src/capi/CMakeLists.txt`
says plainly why that variant exists — "a binding or embedder reaching for a C ABI wants exactly
one library to load" — it statically embeds the whole codec core, so this crate links exactly one
library instead of independently rediscovering `forge_c_static`'s transitive static dependencies
the way `find_package()`-based CMake consumer would for free.

That means a runtime shared library, not just a link-time archive. `build.rs` copies the built
`iclforge_c.dll`/`libiclforge_c.so`/`.dylib` next to the crate's own build output (`target/
<profile>/`, plus `deps/` and `examples/` on Windows, where DLL search actually looks) and, on
Unix, adds an rpath pointing at the same directory — so `cargo test`/`cargo run --example` work
with no `LD_LIBRARY_PATH`/`PATH` juggling. This is a **local-dev/CI convenience, not a deployment
story**: an application embedding this crate is responsible for shipping/locating the shared
library at its own runtime, same as any crate wrapping a dynamically-linked C library.

## What's covered

AC-3, E-AC-3 and AC-4 encode and decode — solid and tested (synthesized audio, several
frames, per CONTRIBUTING.md's validation discipline), not six surfaces half-covered. The first
pass was AC-3 and single-substream E-AC-3:

- `iclforge::ac3` — `Encoder`/`EncoderConfig`, `Decoder`/`DecodedFrame`.
- `iclforge::eac3` — `Eac3Encoder`/`Eac3FrameConfig`, `Eac3Decoder`/`DecodedSubstream`.
- `iclforge::ac4` — `Encoder`/`EncoderConfig`, `Decoder`/`DecodedFrame`, objects included (see
  Coverage below).

Every config type follows the C header's own `_config_init` growth convention: construct with
`Default::default()` (which calls the raw `iclforge_*_config_init()` FFI function first) and
override only the fields you need — never a hand-written Rust-side guess at what the C++ defaults
are. See `iclforge::EncoderConfig::default()`'s doc comment for the mechanics.

### Coverage

The first pass covered AC-3 and single-substream E-AC-3 encode/decode; AP9's completeness pass
closed everything that list deferred:

- `eac3::AccessUnitEncoder`/`AccessUnit` and `Eac3Decoder::decode_access_unit`/
  `DecodedAccessUnit` — wide layouts (7.1/5.1.2/5.1.4/7.1.4) built from several substreams,
  round-tripped in `tests/completeness.rs` through a real 5.1.2 encode and rendered decode.
- `atmos::AtmosEncoder`/`AtmosConfig`/`ObjectPlacement` — Atmos/JOC object encode, with the
  OAMD/JOC accessors (`has_object_metadata`, `dynamic_object`, `object_audio`, …) on both
  `DecodedSubstream` and `DecodedAccessUnit` completing the round trip.
- `stream::split_frames`/`split_access_units`/`stream_bsid` and `stream::scan`/`ScannedStream`
  — the framing/scan helpers AP5 added to the C API after this crate's first pass. The spans
  those report are (offset, length) pairs into the caller's buffer, which Rust states directly:
  the returned slices borrow the input, so the borrow checker enforces the C header's "must
  stay valid and unmodified" clause instead of documentation asking for it.
- `meter::LoudnessMeter` (both constructors — acmod/lfe and the BS.1770-5 chanmap form) and
  `meter::dialnorm_from_lkfs`.
- `iclforge::ac4::Decoder`/`Encoder` (roadmap plan phases I4 and I4b) — a distinct codec (ETSI TS 103 190),
  wrapping `iclforge_ac4_*` unconditionally, with no Cargo feature of its own: the header declares
  those functions whether or not the library was built with AC-4 (`ICLFORGE_BUILD_AC4`, default ON
  and left at its default by `build.rs`; with it off the fallible ones return
  `ICLFORGE_ERROR_UNSUPPORTED`), so this module sits on the same footing `atmos` already did.
  Channel-based and channel-based-immersive encode/decode (mono, stereo, 5.0, 5.1, 5.0.4, 5.1.4),
  presentations, concealment, loudness metadata and the `Toc`/`sync_frame` container helpers are
  wrapped. `tests/ac4_roundtrip.rs` round-trips stereo and 5.1 (checking the 5.1 speaker order),
  refuses a corrupted frame and an invalid channel count, and checks the `Toc` and `sync_frame`
  helpers; no test yet reads back the presentation list, the concealment report or the loudness
  metadata. The encoder's object substream (A-JOC or direct-coded objects with their metadata and
  its updates), the I-frame lists and the experimental flags that need no nested group, with the
  decoder's update ramps, are in `tests/ac4_objects.rs`. The encoder's own subset — no
  loudness/DRC/downmix/dialogue-enhancement metadata groups, no several substreams or
  presentations, no EMDF payloads, no `drc_gains` or `three_zero` flags — mirrors the C API's
  identical cut for the same reason.

Still deliberately out: the caller-buffer `_into` decode forms (a realtime-embedder
convenience whose Rust ergonomics want `&mut [f32]` scratch the value forms already avoid
allocating twice for), the level meter (`iclforge_level_meter_t`) and the QC gates, and a few
scanned-stream, frame-metadata and latency accessors — recorded, not silently missing; the full
list is [What is not wrapped](https://iainchesworthlabs.github.io/ac3forge/library/rust-api/#what-is-not-wrapped).

## Header defects found while building this

The point of AP9, per the roadmap entry, isn't the crate — the C API had never crossed a real FFI
boundary before (Python is pybind11-direct C++, WASM is Embind, Android is JNI, all three
compiling the same C++23 source this binding instead links as a black box). Four things surfaced:

1. **`iclforge_object_placement_t` had no `_init()`, unlike every sibling config struct.**
   `iclforge::oba::ObjectPlacement` default-member-initializes `gain = 1.0`; the C struct's own doc
   comment says "gain: linear, default 1.0" — but nothing prevented a caller from
   zero-initializing it and silently getting a muted object (`gain = 0.0`), with no
   `iclforge_object_placement_init()` to catch the trap the way every other config struct's own
   `_init()` does. **Fixed with the binding**: `iclforge_object_placement_init()` added (purely
   additive); the default position it sets is room-centre (0.5, 0.5, 0.0), not the origin.
2. **Four accessors were missing the pointer-lifetime documentation their sibling has.**
   `iclforge_decoded_frame_channel_samples()` documents "valid until `frame` is destroyed";
   `iclforge_decoded_substream_channel_samples()`, `iclforge_decoded_access_unit_channel_samples()`,
   `iclforge_decoded_substream_object_audio()` and `iclforge_decoded_access_unit_object_audio()`
   didn't say so. **Fixed with the binding** (doc-only). This crate's own wrappers tie every such
   slice's lifetime to `&self` regardless, so a wrong assumption here would have shown up as a
   Rust borrow-checker error in this crate, never as a use-after-free in a caller's.
3. **bindgen types C enums `i32` on MSVC and `u32` on the Unix targets** — so any Rust code
   that bakes one platform's answer into its own types breaks the moment the other platform
   builds. This crate's first Windows build (the AP9 completeness pass, which also widened the
   CI matrix to all three desktop OSes) found exactly that in its own `Error::Other(u32)`:
   constructed from and converted back to the raw status with `as` casts now, so the stored
   value is the same bit pattern under either representation. Not a header defect — a
   portability fact every bindgen consumer of this header inherits, recorded here for the next
   binding's author.
4. **Reported, not changed: no enum in the header says whether it may gain new values in a
   future minor release.** This matters specifically for a strongly-typed binding — a Rust `enum`
   can't safely represent an FFI discriminant it doesn't recognize. `iclforge::Error` is
   deliberately open (`Error::Other(u32)`) for exactly this reason rather than a closed set that
   would have to panic or silently misreport on a value from a newer library. Not a header change
   — a design-philosophy question worth recording, not a bug.

## What .NET, Node N-API and an Android AAR would need

The same header, so the same three questions this crate had to answer apply directly:

- **Linking**: link `forge_c_shared` (not `forge_c_static`), for the reason above — none of
  these ecosystems have a native equivalent of CMake's exported-target transitive-dependency
  resolution either.
- **The `_config_init` convention**: every wrapper needs to call the raw init function first and
  override selectively, exactly like this crate's `EncoderConfig::default()` — a naive
  zero-initialized struct is wrong for several fields (`dialnorm` 0 is invalid; §5.4.2.8 reserves
  it), and the `iclforge_object_placement_init()` gap above shows what happens when a struct is
  missed.
- **Open vs. closed enums**: `iclforge_status_t` and friends should map to whatever each
  ecosystem's idiomatic "this might be one of these, or something newer" shape is (Node: a string
  union plus a fallback; .NET: `int` plus named constants rather than a strict `enum`; Android/
  Kotlin: a sealed class with an `Unknown(Int)` case) — a closed native enum is the wrong default
  for any of them, for the same reason it was for Rust.
