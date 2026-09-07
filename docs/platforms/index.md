# Platforms

You have a machine, or a chip, or a browser tab, and you want to know whether AC3Forge runs on
it and which page to read next. That is what this page is for. It routes; the page it sends you
to is the one that holds the detail, and where the two ever disagree, the platform page is right.

Two tables, deliberately coarse. The first is by **what you have**, the second by **how you want
to call it**. Neither uses ticks: what each target has been shown to do differs enough that a
tick would flatten the part worth knowing.

## Which page

| You have | Read | What runs there | Where it stands |
|---|---|---|---|
| **A Windows PC** (x64) | [Windows](windows.md) | The library, `ac3cli`, `ac3gui`, and Crucible | Required, green CI on MSVC and clang-cl. Capture, IEC 61937 passthrough and monitor playback over WASAPI, confirmed against real hardware. |
| **A Linux PC** (x64 or arm64) | [Linux](linux.md) | The library, `ac3cli`, `ac3gui`, and Crucible | Required, green CI on GCC and Clang, both arches. ALSA or PipeWire. `ac3gui` is opt-in at build time (`-DAC3FORGE_BUILD_GUI=ON`). Bitstream output has reached a real receiver on [one machine](raspberry-pi.md#live-hdmi-passthrough-to-a-real-receiver). |
| **A Mac** (Apple Silicon or Intel) | [macOS](macos.md) | The library, `ac3cli`, `ac3gui`; Crucible compiles | Two required CI legs, neither experimental. No Mac host is available to this project: nothing on macOS has captured or played a sound, and the [Core Audio process tap](macos.md#per-application-capture-the-core-audio-process-tap) has never been created. |
| **A Raspberry Pi 4B** | [Raspberry Pi](raspberry-pi.md) | Everything Linux arm64 runs — there is no Pi-specific code | Run for real on a Pi 4B: 440/440 tests on both compilers, including the hard real-time encode gate, and Atmos out over HDMI to a powered AVR. The Pi 5 is expected to behave the same and is **not** validated; the Pi 3 is not a supported target. |
| **An NVIDIA Shield** (Android TV) | [Android](android.md) | Shield Atmos Demo, a demo app — not `ac3cli`/`ac3gui` ported | Encodes Atmos/JOC live and plays it out the Shield's HDMI passthrough to a receiver, with a controller moving objects; confirmed on real 2017 Shield hardware. Sideload only, never the Play Store. It captures nothing. |
| **A browser** | [WebAssembly](wasm.md) | Decode and encode modules over the same library, and an Atmos object-authoring page | The demo pages are built and [published live](../wasm-demo.md). The `ac3forge-wasm-decoder` npm package is decode-only and **has never been released to npm** — building it from `js/` is the only way to get it. |
| **A board with no operating system** | [Minimum-footprint decoder profile](../building.md#minimum-footprint-decoder-profile) | `ac3::forge_minimal`: one decode-only static library, no exceptions, no RTTI | Cross-compiled `arm-none-eabi` and run on QEMU's `mps2-an385` (Cortex-M3, no FPU). A probe decodes six frames each of AC-3 and E-AC-3 and gates on exact per-channel levels, image size, heap peak and allocation counts. Correctness under emulation; no real silicon. |

**The Windows kernel driver is not in that table**, because it is a component rather than
somewhere you run anything. Crucible's silent output device on Windows is a kernel driver — the
only way Windows lets anyone make an audio endpoint — and it has a page of its own, [the
null-sink driver on ACX](windows-driver-acx.md), with the phase record of the demo it grew out
of on [the Windows demo page](windows-demo.md). You do not target it; Crucible installs it. It
is **test-signed only** today and a Windows machine with default settings refuses to load it,
which is why [Crucible's install page](../crucible/install.md#without-the-driver) has a section
on running without it.

## Which interface

| You want to call it from | Where that runs | Read |
|---|---|---|
| **A shell** — `ac3cli` | Windows, Linux (including the Pi), macOS | [CLI reference](../cli/index.md), [Forge](../forge/index.md) |
| **A window** — `ac3gui` | Windows, Linux and macOS. Shipped prebuilt for Windows and macOS; from source, only the Windows presets default it on (`-DAC3FORGE_BUILD_GUI=ON` elsewhere, and Qt is needed either way) | [GUI guide](../gui/index.md) |
| **A desktop app** — Crucible | Windows and Linux; the macOS half compiles but has never been run | [Crucible](../crucible/index.md) |
| **C++** | Every desktop platform above, plus WebAssembly and bare metal | [Library conventions](../library/index.md) |
| **C** | Wherever the C++ library builds | [C API](../library/c-api.md) |
| **Python** | `pip install ac3forge` — wheels for Windows x64, macOS arm64 and x86_64, Linux x86_64 and aarch64 | [Python bindings](../library/python-api.md) |
| **Rust** | In-tree at `rust/`, over the C API; not published to crates.io | [Rust bindings](../library/rust-api.md) |
| **JavaScript** | A browser, through WebAssembly; the npm package is unpublished | [WebAssembly](wasm.md) |
| **You do not** — Shield Atmos Demo is the whole surface | An NVIDIA Shield, sideloaded | [Android](android.md) |

## Reading the last column

The rightmost column of the first table says what each target has been *shown* to do, and those
are not the same strength of claim. Four levels appear, and the distance between them matters
more than the platform list does:

- **Confirmed on real hardware.** Somebody connected it and it worked: Windows passthrough, the
  Pi 4B driving an AVR, the Shield playing to a receiver.
- **Required and green in CI.** The code builds and its tests pass on every push, on real
  runners. This is where macOS sits in full and where every desktop platform sits for anything
  that does not touch sound hardware.
- **Built, run under emulation.** The bare-metal profile decodes correctly on QEMU. Correct
  output; nothing about timing on real silicon.
- **Written, never exercised.** The macOS process tap, and the macOS half of Crucible. The code
  compiles and one test checks a version gate. Nothing more than that.

A target can sit at different levels for different things, and several do. macOS builds and tests
under the second level while its process tap sits at the fourth. Read the platform page before
depending on any of it.

## What the codec does, on any of them

Picking a platform settles where the code runs, not what it can encode or decode. That question
is the same on every row above and is answered in one place:
[Capabilities and limitations](../library/capabilities.md), which lists every coding mode,
layout, sample rate, metadata field and Annex E tool with its spec citation, and says where each
runs out. Two limits there are worth knowing before you choose anything — objects decode as a
5.1 bed in Dolby's own decoder without a signing key, and Linux bitstream output has reached a
real receiver on one machine.
