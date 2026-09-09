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
    The API is not stable — releases so far are 0.x betas, and the
    [changelog](https://github.com/iainchesworthlabs/ac3forge/blob/main/CHANGELOG.md) records what
    each contains.

    The library, `ac3cli` and `ac3gui` are required and green in CI on Windows (MSVC and
    clang-cl), Linux (GCC and Clang, x64 and arm64) and macOS (Homebrew LLVM, arm64 and Intel).
    Crucible is built and tested on both Windows legs, both Linux Clang legs against PipeWire and
    both macOS legs, and packaged from the Windows MSVC leg and both Linux Clang legs. No hosted
    runner can put any of it near sound, so no macOS code has captured or played anything.

    [Building from source](building.md#verified-configuration) has the exact toolchain versions
    and what each CI leg covers; [Validation](verification.md) has what the output is checked
    against and where that checking runs out.

## What is here

Three things, built on one codec.

### The library

`ac3::forge` and its siblings: the codec itself. It turns PCM — or mono sources placed and
moved in 3D space — into AC-3, E-AC-3, or E-AC-3 with Joint Object Coding elementary streams,
and reads those streams back; loudness metering, level analysis and the QC gates are part of it.
Around it sit standalone MKV, MP4/CMAF and MPEG-TS muxers, an IAB reader, an ADM/BW64 reader and
bridge, an IAMF writer, an AC-4 inspector, the shared platform audio backends and EMDF
object signing, with C, Python and Rust bindings and a WebAssembly build over the same code. It
ships separately from the applications: the `ac3forge-dev-*` archives and, on Linux, the
`libac3forge0` runtime package with `libac3forge-dev` (DEB) or `ac3forge-devel` (RPM), from each
[release](https://github.com/iainchesworthlabs/ac3forge/releases), or `pip install ac3forge` for
the [Python bindings](library/python-api.md). What it can and cannot do is on
[Capabilities](library/capabilities.md); how to link and call it is on
[the library page](library/index.md).

### Forge

[Forge](forge/index.md) is the tooling over the library: `ac3cli`, the forty-one-command
front end for encoding, decoding, muxing, inspection, QC and live capture, and `ac3gui`, the Qt
Quick workbench with a plan view for placing objects and channel-level metering. The two ship
together in one download in every generator and registry; the Forge page says how to get them,
and the [CLI reference](cli/index.md) and [GUI guide](gui/index.md) cover each in full.

### Crucible

[Crucible](crucible/index.md) is the desktop application that makes every application playing
sound an Atmos object the listener places in a room, and streams the result live over HDMI, or
decoded to whatever the endpoint takes. It runs on Windows, where it installs its own silent
virtual output device so the sound has somewhere to go, and on Linux, where it taps each
application through PipeWire. A macOS platform half compiles on the two macOS CI legs and the
window's suites run over it there; nobody has launched it on a Mac, and its Core Audio tap has
never captured anything.
[Install and first run](crucible/install.md) is the fastest way in.

## Where it runs

| Target | What runs there | Strongest evidence |
|---|---|---|
| [Windows](platforms/windows.md) x64 | Library, `ac3cli`, `ac3gui`, Crucible | Passthrough and capture confirmed on real hardware |
| [Linux](platforms/linux.md) x64 / arm64 | Library, `ac3cli`, `ac3gui`, Crucible | Bitstream out to a real receiver, on [one machine](platforms/raspberry-pi.md) |
| [Raspberry Pi 4B](platforms/raspberry-pi.md) | Everything Linux arm64 runs | Atmos over HDMI to a powered AVR, on the board |
| [macOS](platforms/macos.md) arm64 / Intel | Library, `ac3cli`, `ac3gui`; Crucible compiles | Green CI. Nothing has captured or played a sound |
| [Android](platforms/android.md) (Shield) | Shield Atmos Demo only | Live objects out HDMI to a receiver, on the device |
| [WebAssembly](platforms/wasm.md) | Decode and encode in a page | Demos published and running |
| [ESP32-S3](platforms/esp32.md) | Decode or encode, incl. Atmos objects | Correct under QEMU. Two example players drive I2S. Real time not measured |
| [Bare metal](platforms/bare-metal.md) | Decode or encode, `arm-none-eabi` | Correct under QEMU. No real silicon |

Picking a platform settles where the code runs, not what it can encode or decode — that is the
same everywhere and is listed in [Capabilities](library/capabilities.md).
[Platforms](platforms/index.md) has the full routing table and explains how much weight each
evidence level carries.

## Also in the tree

Two demonstrations of the library, neither of them a product of its own: the
[Shield Atmos Demo](platforms/android.md), an Android TV app that streams controller-driven
Atmos object motion out an NVIDIA Shield's HDMI passthrough to a receiver, and the browser demos
that [decode](wasm-demo.md) and [encode](wasm-encode-demo.md) in a page over the library
compiled to WebAssembly.

## Where to go next

- **Getting started** — [Quick start](quickstart.md): clone to first encode.
- **Platforms** — [which page applies to what you have](platforms/index.md): a PC, a Mac, a
  Raspberry Pi, a Shield, a browser, an ESP32-S3 or a board with no operating system, each with
  what has been shown to work on it.
- **Concepts** — [Overview](concepts/index.md): AC-3, E-AC-3 and the Atmos/JOC object layer
  explained.
- **Capabilities** — [what the library encodes and decodes](library/capabilities.md): every
  coding mode, layout, sample rate, metadata field and Annex E tool, with spec citations.
- **Validation** — [how output is checked](verification.md): quality numbers, oracle coverage,
  and exactly where it runs out.
- **Library** — [what it is and how to link it](library/index.md): the public C++ API, with
  [compiled examples](library/examples.md).
- **Forge** — [what it is and how to get it](forge/index.md), then the
  [CLI reference](cli/index.md) for `ac3cli` and the [GUI guide](gui/index.md) for `ac3gui`.
- **Crucible** — [what it is](crucible/index.md): every application on the desk as an Atmos
  object, and [how to install it](crucible/install.md).
