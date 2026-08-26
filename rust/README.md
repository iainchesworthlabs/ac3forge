# ac3forge Rust bindings

Roadmap item AP9: the first non-Python binding over `ac3forge_c/ac3forge.h`. Two crates:

- **`ac3forge-sys`** — raw, `bindgen`-generated FFI declarations. Nothing here is hand-written;
  `build.rs` regenerates them from the header at every build, against the exact library it also
  compiled from source, so header drift is caught the moment it happens rather than discovered by
  a consumer months later.
- **`ac3forge`** — a safe, idiomatic wrapper over it: `Result`/`Option` instead of status codes
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
install). No vcpkg toolchain file is needed: `build.rs` configures with everything except
`AC3FORGE_BUILD_CAPI` turned off (the same trimmed set `python/pyproject.toml` uses for its own
extension-module build), and the one dependency that survives that ({fmt}) resolves via
`find_package(CONFIG)` with a `FetchContent` fallback (`cmake/Fmt.cmake`).

```bash
cargo build --workspace
cargo test --workspace
cargo run --example encode_decode_ac3 -p ac3forge
cargo run --example encode_decode_eac3 -p ac3forge
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
`ac3forge_c.dll`/`libac3forge_c.so`/`.dylib` next to the crate's own build output (`target/
<profile>/`, plus `deps/` and `examples/` on Windows, where DLL search actually looks) and, on
Unix, adds an rpath pointing at the same directory — so `cargo test`/`cargo run --example` work
with no `LD_LIBRARY_PATH`/`PATH` juggling. This is a **local-dev/CI convenience, not a deployment
story**: an application embedding this crate is responsible for shipping/locating the shared
library at its own runtime, same as any crate wrapping a dynamically-linked C library.

## What's covered

AC-3 and E-AC-3 encode and decode, single substream — solid and tested (real synthesized audio,
several frames, per CONTRIBUTING.md's validation discipline), not six surfaces half-covered:

- `ac3forge::ac3` — `Encoder`/`EncoderConfig`, `Decoder`/`DecodedFrame`.
- `ac3forge::eac3` — `Eac3Encoder`/`Eac3FrameConfig`, `Eac3Decoder`/`DecodedSubstream`.

Every config type follows the C header's own `_config_init` growth convention: construct with
`Default::default()` (which calls the raw `ac3forge_*_config_init()` FFI function first) and
override only the fields you need — never a hand-written Rust-side guess at what the C++ defaults
are. See `ac3::EncoderConfig::default()`'s doc comment for the mechanics.

### Explicitly not covered yet

Recorded here rather than silently missing:

- `ac3forge_eac3_access_unit_encoder_t` — wide layouts (7.1/5.1.2/5.1.4/7.1.4) built from several
  substreams.
- `ac3forge_atmos_encoder_t` — Atmos/JOC object encode.
- The OAMD/JOC object-audio decode accessors on `DecodedSubstream` (`has_object_metadata`,
  `dynamic_object`, `object_audio`, …).
- `ac3forge_split_frames`/`split_access_units`/`stream_bsid` — stream-framing helpers.
- AP5's own remaining gap (`scan`, caller-buffer `_into` decode forms, metering) — not in the C
  API yet at all; `ac3forge-sys` picks up new declarations automatically the moment it lands,
  no design change needed here.
- Windows/macOS CI legs — `build-rust` is Linux-only for this first pass (see `_build.yml`),
  matching `build-android`/`build-wasm` each being single-OS jobs too. The dynamic-linking
  approach above needs a real CI run to validate before widening the matrix.

## Header defects found while building this

The point of AP9, per the roadmap entry, isn't the crate — the C API had never crossed a real FFI
boundary before (Python is pybind11-direct C++, WASM is Embind, Android is JNI, all three
compiling the same C++23 source this binding instead links as a black box). Three things surfaced:

1. **`ac3forge_object_placement_t` had no `_init()`, unlike every sibling config struct.**
   `ac3::oba::ObjectPlacement` default-member-initializes `gain = 1.0`; the C struct's own doc
   comment says "gain: linear, default 1.0" — but nothing prevented a caller from
   zero-initializing it and silently getting a muted object (`gain = 0.0`), with no
   `ac3forge_object_placement_init()` to catch the trap the way every other config struct's own
   `_init()` does. **Fixed in this PR**: `ac3forge_object_placement_init()` added (purely
   additive), which also corrected this README's own first draft — the real default position is
   room-centre (0.5, 0.5, 0.0), not the origin.
2. **Four accessors were missing the pointer-lifetime documentation their sibling has.**
   `ac3forge_decoded_frame_channel_samples()` documents "valid until `frame` is destroyed";
   `ac3forge_decoded_substream_channel_samples()`, `ac3forge_decoded_access_unit_channel_samples()`,
   `ac3forge_decoded_substream_object_audio()` and `ac3forge_decoded_access_unit_object_audio()`
   didn't say so. **Fixed in this PR** (doc-only). This crate's own wrappers ties every such
   slice's lifetime to `&self` regardless, so a wrong assumption here would have shown up as a
   Rust borrow-checker error in this crate, never as a use-after-free in a caller's.
3. **Reported, not changed: no enum in the header says whether it may gain new values in a
   future minor release.** This matters specifically for a strongly-typed binding — a Rust `enum`
   can't safely represent an FFI discriminant it doesn't recognize. `ac3forge::Error` is
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
  it), and the `ac3forge_object_placement_init()` gap above shows what happens when a struct is
  missed.
- **Open vs. closed enums**: `ac3forge_status_t` and friends should map to whatever each
  ecosystem's idiomatic "this might be one of these, or something newer" shape is (Node: a string
  union plus a fallback; .NET: `int` plus named constants rather than a strict `enum`; Android/
  Kotlin: a sealed class with an `Unknown(Int)` case) — a closed native enum is the wrong default
  for any of them, for the same reason it was for Rust.
