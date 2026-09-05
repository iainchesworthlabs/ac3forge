# ac3forge

AC3Forge is a clean-room AC-3, E-AC-3 and Dolby Atmos codec written from the published
standards in C++23, and the two applications built on it.

Nothing here links FFmpeg or any other codec library. The FFmpeg command-line tools are used
during development as an independent decoder to check output against; the build does not
depend on them.

!!! warning "Standards and trademarks"
    "Dolby", "Dolby Digital" and "Dolby Atmos" are trademarks of Dolby Laboratories. This
    project implements the openly published standards — ATSC A/52:2018 (of which E-AC-3 is
    normative Annex E), ETSI TS 102 366 and ETSI TS 103 420 — and is not affiliated with,
    endorsed by, or certified by Dolby Laboratories. Code and documentation use the technical
    names AC-3 and E-AC-3. Whether the patents reading on these formats matter for your use is
    your problem to assess, not something this project resolves.

!!! note "Status"
    The API is not stable — releases so far are 0.x betas; the
    [changelog](https://github.com/iainchesworthlabs/ac3forge/blob/main/CHANGELOG.md) records what
    each contains. Green and required in CI on Windows (MSVC, clang-cl), Linux (GCC and Clang,
    x64 and arm64) and macOS (Homebrew LLVM) — CLI and GUI alike on every platform — plus an
    ASan+UBSan leg, clang-tidy static analysis, a line/branch
    coverage gate over the library, a per-platform gold-reference *quality* gate, dedicated
    Linux FFmpeg- and ADM-validation legs checking output *correctness*, and a required Android
    build leg. One leg, `windows-msvc-arm64`, is still marked experimental, and still
    packages for release. See [building.md](building.md) for exact toolchain versions and
    what each CI leg covers.

## The three members

### The library

`ac3::forge` and its siblings: the codec itself. It turns PCM — or mono sources placed and
moved in 3D space — into AC-3, E-AC-3, or E-AC-3 with Joint Object Coding elementary streams,
and reads those streams back. Around it sit standalone MKV, MP4/CMAF and MPEG-TS muxers, an IAB
reader, an ADM/BW64 reader and bridge, an IAMF writer, an AC-4 inspector, the family's shared
platform audio backends, loudness metering and QC, and EMDF object signing, with C, Python
and Rust bindings and a WebAssembly build over the same code. What it can and cannot do is on
[Capabilities](library/capabilities.md); how to link and call it is on
[Conventions](library/index.md).

### Forge

[Forge](forge/index.md) is the tooling over the library: `ac3cli`, the thirty-nine-command
front end for encoding, decoding, muxing, inspection, QC and live capture, and `ac3gui`, the Qt
Quick workbench with a plan view for placing objects and channel-level metering. The two ship
together in one download in every generator and registry; the Forge page says how to get them,
and the [CLI reference](cli/index.md) and [GUI guide](gui/index.md) cover each in full.

### Crucible

[Crucible](crucible/index.md) is the desktop application for Windows and Linux that makes every
application playing sound an Atmos object the listener places in a room, and streams the result
live over HDMI, or decoded to whatever the endpoint takes. On Windows it installs its own
silent virtual output device so the sound has somewhere to go; on Linux it taps each
application through PipeWire. [Install and first run](crucible/install.md) is the fastest way
in.

## Also in the tree

Two things sit beside the three members and join none of them: the
[Shield Atmos Demo](platforms/android.md), an Android TV app that streams controller-driven
Atmos object motion out an NVIDIA Shield's HDMI passthrough to a receiver, and the browser demos
that [decode](wasm-demo.md) and [encode](wasm-encode-demo.md) in a page over the library
compiled to WebAssembly. Both demonstrate the library.

## Where to go next

- **Getting started** — [Quick start](quickstart.md): clone to first encode in under ten
  minutes.
- **Concepts** — [Overview](concepts/index.md): AC-3, E-AC-3 and the Atmos/JOC object layer
  explained.
- **Capabilities** — [what the library encodes and decodes](library/capabilities.md): every
  coding mode, layout, sample rate, metadata field and Annex E tool, with spec citations.
- **Validation** — [how output is checked](verification.md): quality numbers, oracle coverage,
  and exactly where it runs out.
- **Library** — [Conventions](library/index.md): the public C++ API, with
  [compiled examples](library/examples.md).
- **Forge** — [what it is and how to get it](forge/index.md), then the
  [CLI reference](cli/index.md) for `ac3cli` and the [GUI guide](gui/index.md) for `ac3gui`.
- **Crucible** — [what it is](crucible/index.md): every application on the desk as an Atmos
  object, and [how to install it](crucible/install.md).
