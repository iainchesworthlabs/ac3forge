# API stability and the road to v1.0

What "stable" will mean once this project
tags `v1.0.0`, and what has to be true first. Every release to date has been a `0.x.y` prerelease
— nothing has promised compatibility across two tags yet, deliberately (see
[Versioning](../releasing.md#versioning)). This page is where that promise gets defined before it
starts being made, and tracks the mechanical pieces that exist versus the ones still open.

## Header tiers

`docs/library/header-map.md` lists the headers a caller normally reaches for, but stops short of
saying which of them a `v1.0.0` compatibility promise actually covers. It doesn't cover all of
them equally: a chunk of what's under `src/ac3/include/iclforge/ac3/` is bitstream/DSP machinery the
encoder and decoder share internally, installed today because splitting the install set is more
CMake complexity than the problem has earned so far, not because a caller is expected to include
it directly. Four tiers, assigned per header below:

- **Public** — the frozen surface once `v1.0.0` ships. Breaking one of these needs a major
  version bump afterward.
- **Internal** — installed (`iclforge_ac3_objects` needs them visible, and a split install tree isn't
  worth building yet) but never covered by a compatibility promise, frozen or not. Free to
  change, rename, or disappear in any release.
- **Diagnostic** — instrumentation and self-check machinery: real public symbols, but explicitly
  out of scope for both bindings already (`docs/library/header-map.md` already says as much for
  `verify/*`, and [Using the libraries](index.md)'s pimpl note says the same for the `trace`
  pointers a few config structs carry). Formalizing that existing intent as its own tier rather
  than leaving it implicit.
- **In-tree only** — never installed or exported at all (`iclforge::audio`); already stated per-header
  in `header-map.md` and unaffected by this page.

| Header(s) | Tier |
|---|---|
| `iclforge/ac3/core/tables.hpp` | Public — `SampleRate`, `Acmod` and the frame constants appear directly in public function signatures everywhere. |
| `iclforge/ac3/core/eac3_tables.hpp` | Public — `chanmap` and `ChannelPlan` are likewise part of `plan.hpp`'s own public surface. |
| `iclforge/base/layout.hpp`, `downmix_target.hpp` | Public — `Location`, `Layout` and `DownmixTarget` appear in public signatures (`plan.hpp`, the output stage, the renderer) and keep the tier of the headers they left. |
| `iclforge/base/bitreader.hpp`, `bitwriter.hpp` | Internal — bitstream I/O primitives, never called directly by a caller using the encoder/decoder API. |
| `iclforge/ac3/core/mdct.hpp`, `window.hpp` | Internal — transform internals. |
| `iclforge/ac3/core/bitalloc.hpp`, `exponents.hpp`, `mantissas.hpp` | Internal — §7.1–7.3 coding internals shared by encoder and decoder. |
| `iclforge/ac3/core/coupling.hpp`, `eac3_tools.hpp` | Internal — coding-tool internals shared by encoder and decoder, selected via `plan::Tools`/content-adaptive search on the encode side and driven by the bitstream on the decode side; not instantiated directly by a caller. |
| `iclforge/ac3/core/crc16.hpp`, `fft.hpp`, `aht_tables.hpp`, `bitalloc_tables.hpp`, `oba/joc_tables.hpp` | Internal — already undiscussed in `header-map.md`'s own intro; this just makes the tier explicit. |
| `iclforge/ac3/encoder/encoder.hpp`, `eac3_frame.hpp`, `silent_frame.hpp`, `plan.hpp`, `assignment.hpp` | Public. |
| `iclforge/ac3/encoder/transient.hpp`, `bandwidth.hpp` | Internal — coding-tool implementations selected via `plan::Tools`/content-adaptive search (`bandwidth.hpp` chooses `chbwcod`), not instantiated directly by a caller. |
| `iclforge/ac3/decoder/decoder.hpp`, `output.hpp`, `diagnostics.hpp` | Public — `DecoderConfig` carries `diagnostics.hpp`'s sink in its own fields. |
| `iclforge/render/pcm_block.hpp` | Public — `PcmBlock` and `BlockSink` are the by-block decode API of `decoder.hpp`, which includes it; the header sits with the renderer, whose tier below is not this one. |
| `iclforge/ac3/decoder/syntax_trace.hpp` | Diagnostic. |
| `iclforge/ac3/decoder/transient_prenoise.hpp` | Internal — applied automatically by `Eac3Decoder`; a caller observes its buffering effect, never calls it. |
| `iclforge/ac3/decoder/serving.hpp` | **Experimental** — the policy that fits the renderer's layout to the decoder's configuration, shared by the ESP32 player and Hearth; it has the tier of the render headers it serves. |
| `iclforge/ac3/io/elementary.hpp`, `stream_accumulator.hpp`, `metadata_edit.hpp`, `probe.hpp`, `object_strip.hpp`, `dec3.hpp`, `wav.hpp` | Public. |
| `iclforge/ac3/meta/bsi.hpp`, `drc.hpp`, `loudness.hpp`, `mixing.hpp`, `qc.hpp` | Public. |
| `iclforge/render/spatial.hpp` | Public. |
| `iclforge/render/layout.hpp`, `render.hpp`, `routing.hpp`, `trim_delay.hpp`, `identify.hpp`, `float_biquad.hpp` | **Experimental** — the output layout, renderer and speaker management the ESP32 player and Hearth share ([Hearth reference-player plan](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/hearth-reference-player.md)), outside the `v1.0.0` freeze while Hearth's phases settle their shape; see [Experimental modules](#experimental-modules). |
| `iclforge/ac3/oba/atmos.hpp`, `joc.hpp`, `oamd.hpp`, `motion.hpp`, `scene.hpp`, `scene_osc.hpp` | Public — `iclforge::oba::joc` included, now that AP2 folded it into `iclforge::oba` proper. |
| `iclforge/objects/joc_domain.hpp` | Public — `joc::Domain` is selected through `AtmosConfig` and `reconstruct()`, as it was in `joc.hpp`. |
| `iclforge/objects/emdf.hpp` | Public. |
| `iclforge/ac3/emdf/frame_layout.hpp` | Internal — the bit-accurate frame walk `strip_objects` and `iclforge::signing` share; a caller uses them, not it. |
| `iclforge/objects/placement.hpp` | Public — `ObjectPlacement` is what `AtmosEncoder::encode_frame` takes, as it was when `atmos.hpp` declared it. |
| `iclforge/iec61937/iec61937.hpp` | Public. |
| `iclforge/dsp/qmf.hpp` | Public — `oba::joc::Domain::kQmf` is selected through public `AtmosConfig`. |
| `iclforge/dsp/biquad.hpp`, `resampler.hpp` | Public — `dsp::resample`/`resample_planar` is a documented multi-source-rate-conversion utility, not purely an implementation detail (see `header-map.md`). |
| `iclforge/ac3/analysis/levels.hpp` | Public. |
| `iclforge/ac3/quality/distortion.hpp`, `perceptual.hpp` | Public. |
| `iclforge/ac3/latency.hpp` | Public. |
| `iclforge/ac3/verify/mirror.hpp`, `selfcheck.hpp`, `eac3_mirror.hpp`, `eac3_selfcheck.hpp`, `trace_export.hpp`, `bap_census.hpp` | Diagnostic. |
| `ac3/audio/*` | In-tree only (unchanged). |
| `matroska/`, `mp4/`, `mpegts/` mux + demux headers | Public — each is its own installed target with its own `SOVERSION`. |
| `iclforge/adm/ac3adm.hpp`, `model.hpp` | Public within its own opt-in module (`-DICLFORGE_BUILD_ADM=ON`); see [Experimental modules](#experimental-modules) for why this is not the same as "frozen." |
| `iclforge/admbridge/bridge.hpp`, `iab_bridge.hpp`, `coordinates.hpp` | Public, same opt-in caveat. |
| `iclforge/iab/ac3iab.hpp`, `model.hpp`, `mxf.hpp` | **Experimental** — see below; not part of the `v1.0.0` freeze despite being installed and default-on today. |
| `iclforge/iamf/iamf.hpp` | **Experimental** — a new module, which starts there (see below); installed and default-on. |
| `iclforge/ac4/ac4.hpp`, `iclforge/ac4dec/decoder.hpp`, `iclforge/ac4enc/encoder.hpp` | **Experimental** — the AC-4 inspector, decoder and encoder, installed and default-on; see below. |
| `iclforge/ac4/syntax.hpp` | Diagnostic — the AC-4 syntax trace the decoder and encoder write, as `iclforge/ac3/decoder/syntax_trace.hpp` is for AC-3 and E-AC-3. |
| `iclforge_c/iclforge.h` | Public — its own narrower promise, see [C API](c-api.md). |
| `iclforge/signing/signing_key.hpp`, `emdf_atmos_signer.hpp` | Public. |

**The `detail` namespace convention is unaffected by a header's tier.** Five public headers
(`silent_frame.hpp`, `meta/drc.hpp`, `core/tables.hpp`, `core/window.hpp`, `core/crc16.hpp`)
declare a `namespace detail` alongside their public surface, rather than splitting a private
helper into its own file. That split already means what this page needs it to mean: nothing
inside `namespace detail`, in any header at any tier, is covered by any compatibility promise —
codifying an existing convention as policy, not introducing a new one.

## SemVer and deprecation policy

Standard SemVer once `v1.0.0` ships: a patch release fixes behavior without changing any Public
surface; a minor release adds to it; a major release is the only place a Public break is
permitted, and per-header ABI compatibility only holds within a major version (see
[SOVERSION](#soversion) below). Before `v1.0.0`, none of that holds — every `0.x` tag may break
anything, and has (this is what "all releases are prereleases" means to the vcpkg registry
reviewer's maturity rule).

`ICLFORGE_DEPRECATED` (and each module's own equivalent — `ICLFORGE_MATROSKA_DEPRECATED`,
`ICLFORGE_ADM_DEPRECATED`, and so on, all `generate_export_header()` output) exists in every generated
export header already, but `DEFINE_NO_DEPRECATED` is passed everywhere it's generated, which
disables it unconditionally. That's correct for right now: there is no stable symbol yet for
anything to deprecate *from*. The policy going forward:

- **Before `v1.0.0`:** `DEFINE_NO_DEPRECATED` stays. A pre-1.0 break is just a break, documented
  in `CHANGELOG.md` like any other change — a deprecation cycle promises a grace period this
  project isn't promising yet.
- **At and after `v1.0.0`:** drop `DEFINE_NO_DEPRECATED` from every `generate_export_header()`
  call (eighteen libraries — `base`, `dsp`, `objects`, `render`, `iec61937`, `ac3`, `capi`,
  `matroska`, `mp4`, `mpegts`, `adm`, `iab`, `admbridge`, `signing`, `iamf`, `ac4`, `ac4dec`,
  `ac4enc`; `iclforge::ac3_minimal` has no `SOVERSION` promise to protect and can keep it).
  A symbol scheduled for removal gets `ICLFORGE_DEPRECATED` (or its module's equivalent) in the
  same minor release its replacement ships, stays for at least one further minor release, and is
  only removed in a major release. No symbol needs this yet, so no macro use is being added by
  this page — the mechanism is verified present and the trigger condition is written down instead.

## SOVERSION

**Deferred**, not decided against. `src/ac3/CMakeLists.txt` (and the other twelve libraries)
pin `SOVERSION` to the full `PROJECT_VERSION` today, with the comment already explaining why:
pre-1.0, no ABI-compatibility promise holds across any two releases, so there is no meaningful
"compatible" range narrower than an exact match. Flipping every library's `SOVERSION` to just the
major component (`SOVERSION "${PROJECT_VERSION_MAJOR}"`) is the literal moment this project starts
promising that a `libiclforge_ac3.so.1` built at `1.3.0` loads fine against a binary linked at
`1.0.0` — that promise should ship *with* `v1.0.0`, once the ABI gate (`AP4`, already merged and
running `abidiff` + `check_abi_symbols.py` advisory on every build) is promoted from advisory to
required. Flipping the version scheme first and promoting the gate later would let a real ABI
break through in the gap between the two; the gate's own promotion switch (flip one
`ABI_ENFORCE` from `'false'` to `'true'`) is designed to be the last step specifically so this
can't happen. Until then, the sequencing is:

1. This page and its Public/Internal/Diagnostic split are in place.
2. The `v1.0.0` release candidate is cut against the Public tier only, mechanically checked by
   promoting `AP4`'s `abi-gate` to required.
3. `SOVERSION` flips to major-only in the same PR that flips `abi-gate`'s `ABI_ENFORCE` to
   `'true'`, so both changes are reviewed and tagged together.

## Inline namespace for future ABI tagging

**Deferred, deliberately not introduced now.** An inline namespace (`namespace ac3 { inline
namespace v1 { ... } }`) is the standard way to let two ABI-incompatible major versions of a
library coexist in one process without a linker collision — ELF/COFF spell the versioned symbol
as `iclforge::v1::FrameEncoder`, and a consumer's ordinary `iclforge::ac3::FrameEncoder` resolves through the
`inline` transparently. Introducing it now, before there is a `v1` to distinguish from anything,
would touch every symbol in the tree for a distinction that does not exist yet and cannot be
tested (there is no `iclforge::v2` to link against it). The plan is to introduce `inline namespace v1`
in the same release that ships `SOVERSION` major-only — the two changes protect the same promise
(two versions, once compatibility is real, must be distinguishable both at the linker level and
at the C++ symbol level) and share the same "not yet meaningful" argument for staying out of
`v1.0.0`'s prerequisites.

## C API compile-time version

`iclforge_c/iclforge.h` exposes the runtime `iclforge_version()` — what actually got linked.
`iclforge_c/version.h` (generated from `version.h.in` by `src/capi/CMakeLists.txt`, included from
`iclforge.h`) adds `ICLFORGE_C_VERSION_MAJOR`/`MINOR`/`PATCH` and a combined `ICLFORGE_C_VERSION`
integer, usable in `#if` — the SDK version a translation unit compiled against, which a caller may
need to gate on before it can even call `iclforge_version()` to check the other one. It also
carries `ICLFORGE_HAS_AC4`, defined when the library was built with the AC-4 surface.

## C config struct growth

No `iclforge_*_config_t` struct carries a reserved field or a size sentinel today, and none is
being retrofitted by this page — every existing struct is passed by value/stack-allocated (see
`iclforge_encoder_config_init()`'s own header comment), so a caller who has not recompiled has a
fixed, compiled-in `sizeof()` no runtime check can work around. The policy mirrors the C++ config
aggregates' own (`docs/library/index.md`'s pimpl note): a field added to an existing struct after
`v1.0.0` needs either a major version bump, or an additive sibling (`iclforge_encoder_config_v2_t`
plus a `_v2` creation function, the same shape a new field needing a different type
would already require). A `struct_size`-sentinel scheme (`vkStructureType`/`pNext`-style
extensibility) was considered and declined: it adds a branch to every function taking a config
struct for a growth path the C++ side doesn't need either, and after `v1.0.0` a plain major bump
already covers the same case without it.

## Experimental modules

Not every installed, default-on module is part of the `v1.0.0` freeze. `iclforge::iab` (the
SMPTE ST 2098-2 IAB reader) is real, tested, and default-built (`ICLFORGE_BUILD_IAB`).
`forge atmos-iab` consumes it through the opt-in `iclforge::admbridge` module, while the GUI does
not. Its own model is still being built out (`AudioDataDLC`'s Annex B coder is read by identity
only, not decoded — see [Header map](header-map.md)). It is **Experimental**: installed,
versioned, and functional, but explicitly outside the compatibility promise `v1.0.0` makes for
the Public tier above until this page deliberately promotes it. The same designation applies to
`iclforge::mlp` when the TrueHD/MLP branch lands (itself already
scoped as "an explicitly experimental module" gated behind its own `ICLFORGE_BUILD_MLP` — this
page's policy just confirms that plan rather than overriding it; real interoperability, `IM6`,
stays separately blocked on MLP/FBA source material that isn't public). A new module defaults to
Experimental from its first merge, and only leaves that tier through a deliberate, documented
decision on this page, the same way `iclforge::iab` will. `iclforge::render` is one: it began in
the ESP32 player and serves the desktop player as well, it has been a library of its own since the
re-layout, and its speaker management is new with it.

The AC-4 inspector, decoder and encoder (`iclforge::ac4`, `iclforge::ac4dec`, `iclforge::ac4enc`,
[AC-4](ac4.md)) are Experimental too. Their API has the form
[planning/ac4.md](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/ac4.md) set, and
the immersive layouts, core decoding and objects came as fields after the ones channel-based
streams needed. The shared core, `iclforge::ac4core`, has no headers and so no tier. `iclforge::iamf`, the
IAMF writer, is Experimental as a new module.

`iclforge::adm` and `iclforge::admbridge` are a different case: also opt-in
(`-DICLFORGE_BUILD_ADM=ON`), but consumed for real by the ADM→Atmos bridging path and stable in
shape since `IM2`/`IM7`. They're Public, not Experimental — opt-in build gating and API maturity
are independent axes, and conflating "off by default" with "not yet stable" would understate how
settled `iclforge::adm`'s reader actually is.

## Release criteria

`v1.0.0` ships once:

1. Every header above is tagged Public, Internal, Diagnostic or Experimental, and nothing new has
   been added to `ac3/` without that decision being made at merge time (a review checklist item,
   not a CI gate — there's no mechanical way to detect "this header needs a tier").
2. `AP4`'s `abi-gate` is required, not advisory, and `SOVERSION`/the inline namespace have flipped
   together (see [SOVERSION](#soversion)).
3. `CHANGELOG.md`'s `## [Unreleased]` → the release's own `### Known gaps` section contains
   nothing that reads as "the API isn't finished" — a platform-verification gap (exclusive-mode
   WASAPI/PipeWire/CoreAudio passthrough against real hardware, say) is an acceptable Known gap
   for `v1.0.0` the same way it has been for every prerelease; an incomplete or about-to-move
   Public header is not. Distinguishing the two is a judgment call made when that release's Known
   gaps section is actually drafted, not a rule this page can fully anticipate today.
4. `AP5`/`AP6` (C API and Python completeness) are either finished or their remaining gaps are
   themselves re-classified as Experimental/Known-gap rather than silently left off the Public
   surface both bindings claim to mirror.

## Cadence and governance

There is no fixed release cadence (`docs/releasing.md`'s process is tag-triggered, not
calendar-triggered) and this page does not introduce one — a fixed cadence pressures a maintainer
to ship on schedule rather than when the criteria above are actually met, which is the wrong
trade for the first release that claims compatibility. What this page does commit to: `v1.0.0` is
a deliberate, single-maintainer decision made against the checklist above, not an automatic
consequence of enough `0.x` releases accumulating. Until that decision is made, every tag stays a
prerelease — the same maturity signal the vcpkg registry reviewer's own rule was reading in the
first place, now backed by a written criteria list instead of an implicit "not yet."
