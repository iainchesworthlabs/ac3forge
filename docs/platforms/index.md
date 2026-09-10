# Platforms

Where ac3forge runs, and which page holds the detail. Where this page and a platform page
disagree, the platform page is right.

The first table below is organised by what you have, the second by how you want to call it. The
last column of the first says what each target has been *shown* to do; those claims are not all
the same strength, and [Reading the last column](#reading-the-last-column) sets out the four
levels that appear.

## Which page

| You have | Read | What runs there | Where it stands |
|---|---|---|---|
| **A Windows PC** (x64) | [Windows](windows.md) | The library, `ac3cli`, `ac3gui`, and Crucible | Required, green CI on MSVC and clang-cl. Live capture, decoded monitor playback and `spatial` rendering are confirmed on real hardware; exclusive-mode IEC 61937 passthrough has never been accepted by a real device, and no receiver has been cabled to a Windows machine. |
| **A Linux PC** (x64 or arm64) | [Linux](linux.md) | The library, `ac3cli`, `ac3gui`, and Crucible | Required, green CI on GCC and Clang, both arches. ALSA or PipeWire. `ac3gui` is opt-in at build time (`-DAC3FORGE_BUILD_GUI=ON`). Bitstream output has reached a real receiver on [one machine](raspberry-pi.md#live-hdmi-passthrough-to-a-real-receiver). |
| **A Mac** (Apple Silicon or Intel) | [macOS](macos.md) | The library, `ac3cli`, `ac3gui`; Crucible compiles | Two required CI legs, neither experimental. No Mac host is available to this project: nothing on macOS has captured or played a sound, and the [Core Audio process tap](macos.md#per-application-capture-the-core-audio-process-tap) has never been created. |
| **A Raspberry Pi 4B** | [Raspberry Pi](raspberry-pi.md) | Everything Linux arm64 runs — there is no Pi-specific code | Run for real on a Pi 4B: 440/440 tests on both compilers, including the hard real-time encode gate, and Atmos out over HDMI to a powered AVR. The Pi 5 is expected to behave the same and is **not** validated; the Pi 3 is not a supported target. |
| **An NVIDIA Shield** (Android TV) | [Android](android.md) | Shield Atmos Demo, a demo app — not `ac3cli`/`ac3gui` ported | Encodes Atmos/JOC live and plays it out the Shield's HDMI passthrough to a receiver, with a controller moving objects; confirmed on real 2017 Shield hardware. Sideload only, never the Play Store. It captures nothing. |
| **A browser** | [WebAssembly](wasm.md) | Decode and encode modules over the same library, and an Atmos object-authoring page | The demo pages are built and [published live](../wasm-demo.md). The `ac3forge-wasm-decoder` npm package is decode-only and **has never been released to npm** — building it from `js/` is the only way to get it. |
| **An ESP32-S3** (ESP-IDF, FreeRTOS) | [ESP32-S3](esp32.md) | `ac3::forge_minimal` as a reusable ESP-IDF component, decode-only or encode-only | AC-3 and E-AC-3 decode **correctly** — every layout to 7.1.4, every coding tool, the output stage's folds, Atmos objects reconstructed **and placed onto 7.1.4** by their positions — every level exact against baked-in fixtures, inside internal SRAM with no PSRAM; AC-3 and E-AC-3 encode 2/0 and 5.1 byte-exact with the host. Two [example players](esp32.md#examples) drive I2S, one of them streaming from flash, SD or HTTP. **Real time on the board** for every decode fixture: a 5.1 E-AC-3 frame in 11.0 ms of its 32, 7.1.4 in 28.5 and objects placed onto 7.1.4 in 25.1, at 240 MHz on an ESP32-S3-DevKitC-1 — see [Timing](esp32.md#timing). **AC-3 2/0 and E-AC-3 2/0 encode in real time** (0.35x, 0.73x) with the encoders in `float` end to end and the search made cheaper; AC-3 5.1 (1.01x) sits at the line, the other rows 1.3x to 1.7x over, what remains being the exponent-run planner and the allocation candidates. CI runs under QEMU, which cannot answer either. [What the part can and cannot do](esp32.md#what-the-part-can-and-cannot-do) is the full table. |
| **An ESP32-C3** (ESP-IDF, FreeRTOS) | [ESP32 variants](esp32.md#other-esp32-variants) | The same `ac3::forge_minimal` component - its manifest lists `esp32c3` beside `esp32s3` - decoding in the fixed-point tier, since the part has no floating-point unit at all | **Decode is correct, and that is all this row claims.** `apps/baremetal/platform/esp32c3/` is a probe target CI runs under `qemu-riscv32`: eleven of the twelve fixtures decode, and every one of them produces PCM **identical to the x86 host's and the Cortex-M3 leg's** - three architectures, three compilers, one pinned set of hashes. 7.1.4 is the twelfth and does not fit in 400 KB of SRAM: it needs 238,094 bytes of heap where the part reports 249,180 free in a heap whose largest block is 114,688. **Speed on a C3 is unmeasured** - QEMU is not cycle-accurate and no board has run this. Encode is not validated here at all: both encoders are floating-point, which on this part means software floating point. |
| **A board with no operating system at all** | [Bare metal](bare-metal.md) | `ac3::forge_minimal`: one static library, decode-only or encode-only, no exceptions, no RTTI | Cross-compiled `arm-none-eabi` and run on QEMU's `mps2-an385` (Cortex-M3, no FPU). A probe decodes six frames each of twelve fixtures and gates on exact per-channel levels, image size, heap peak, retained bytes and allocation counts. CI runs it twice: in the default arithmetic, and in the [fixed-point tier](bare-metal.md) (`-DAC3FORGE_DECODE_SCALAR=fixed`, Q7.24 integers under a per-block exponent) which is what a part with no FPU wants and costs 0.37x the default build's instructions on the same leg. Correctness under emulation; no real silicon. |

**The Windows kernel driver is a component, not a target**, so it is not in that table.
Crucible's silent output device on Windows is a kernel driver, which is the only way Windows lets
anyone create an audio endpoint. You do not target it; Crucible installs it. It is
**test-signed only** today and a Windows machine with default settings refuses to load it, which
is why [Crucible's install page](../crucible/install.md#without-the-driver) covers running
without it. [The null-sink driver on ACX](windows-driver-acx.md) has the detail, and
[the Windows demo page](windows-demo.md) has the phase record of the demo it grew out of.

## Which interface

| You want to call it from | Where that runs | Read |
|---|---|---|
| **A shell** — `ac3cli` | Windows, Linux (including the Pi), macOS | [CLI reference](../cli/index.md), [Forge](../forge/index.md) |
| **A window** — `ac3gui` | Windows, Linux and macOS. Shipped prebuilt for Windows and macOS; from source, only the Windows presets default it on (`-DAC3FORGE_BUILD_GUI=ON` elsewhere, and Qt is needed either way) | [GUI guide](../gui/index.md) |
| **A desktop app** — Crucible | Windows and Linux; the macOS half compiles but has never been run | [Crucible](../crucible/index.md) |
| **C++** | Every desktop platform above, plus WebAssembly and [bare metal](bare-metal.md) | [Library conventions](../library/index.md) |
| **C** | Wherever the C++ library builds | [C API](../library/c-api.md) |
| **Python** | `pip install ac3forge` — wheels for Windows x64, macOS arm64 and x86_64, Linux x86_64 and aarch64 | [Python bindings](../library/python-api.md) |
| **Rust** | In-tree at `rust/`, over the C API; not published to crates.io | [Rust bindings](../library/rust-api.md) |
| **An ESP-IDF component** | An ESP32-S3, decode-only or encode-only; ESP-IDF owns the build, so there is no ac3forge preset | [ESP32-S3](esp32.md) |
| **JavaScript** | A browser, through WebAssembly; the npm package is unpublished | [WebAssembly](wasm.md) |
| **You do not** — Shield Atmos Demo is the whole surface | An NVIDIA Shield, sideloaded | [Android](android.md) |

## Reading the last column

Four strengths of claim appear in that column, and the distance between them matters:

- **Confirmed on real hardware.** Somebody connected it and it worked: Windows passthrough, the
  Pi 4B driving an AVR, the Shield playing to a receiver.
- **Required and green in CI.** The code builds and its tests pass on every push, on real
  runners. This is where macOS sits in full and where every desktop platform sits for anything
  that does not touch sound hardware.
- **Built, run under emulation.** The [bare-metal profile](bare-metal.md) decodes correctly on
  QEMU, and so does the ESP32-S3 build. Correct output; nothing about timing on real silicon,
  which emulation cannot answer — the ESP32-S3's timing comes from a board, see
  [Timing](esp32.md#timing).
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
