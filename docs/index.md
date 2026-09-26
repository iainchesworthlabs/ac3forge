# ac3forge

AC3Forge is a clean-room C++23 implementation of AC-3, E-AC-3, and Dolby Atmos decoding and
encoding, and of AC-4 decoding and encoding. The repository contains the codec library and three
applications built on it. AC-4 is in the library and in `ac3cli`; the Forge GUI, Hearth, Crucible,
and the C, Python, Rust, and WebAssembly bindings do not use it yet.

## Choose what you need

| Goal | Start here |
|---|---|
| Learn what AC-3, E-AC-3, and Atmos mean | [Concepts](concepts/index.md) |
| Install or use the CLI and GUI | [Forge](forge/index.md) |
| Capture desktop applications and position them in an Atmos scene | [Crucible](crucible/index.md) |
| Play audio through the desktop player or an ESP32 network sink | [Hearth](hearth/index.md) |
| Decode or encode AC-4 | [AC-4 in the library](library/ac4.md) |
| Link the codec from C++, C, Python, Rust, or WebAssembly | [Library](library/index.md) |
| Compare products and features by platform and architecture | [Platforms](platforms/index.md) |

The [Quick start](quickstart.md) covers installation, source builds, and the ESP32-S3 sink.

!!! note "Status"
    Releases are 0.x betas and the API is not stable. The
    [changelog](https://github.com/iainchesworthlabs/ac3forge/blob/main/CHANGELOG.md) records
    shipped changes. Platform pages distinguish CI, emulation, and real-hardware results.

## Performance and quality

The following values are loaded from the append-only measurement history maintained by CI on the
`quality-history` branch.

<div id="ac3f-stats">
  <p class="ac3f-stat-status">Loading the latest measurements from <code>main</code>…</p>
</div>

[Performance and quality](performance-quality.md) explains the measures and links to the full
performance, decoder-accuracy, listening-quality, object-quality, and memory histories.

## Project information

- [Capabilities](library/capabilities.md) — supported formats, coding tools, layouts, and limits.
- [Development status](library/development-status.md) — done / partial / not-started across every codec surface.
- [Application coverage](library/application-coverage.md) — which library features each
  application exposes.
- [Validation](verification.md) — how output is checked and where independent checking ends.
- [Building from source](building.md) — toolchains, presets, options, and platform details.
- [Contributing](contributing.md) — repository structure and contribution requirements.

!!! warning "Standards and trademarks"
    "Dolby", "Dolby Digital", and "Dolby Atmos" are trademarks of Dolby Laboratories. This
    project implements ATSC A/52:2018, ETSI TS 102 366, and ETSI TS 103 420. It is not
    affiliated with, endorsed by, or certified by Dolby Laboratories. Patent requirements
    depend on how and where the formats are used.
