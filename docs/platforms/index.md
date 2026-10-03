# Platforms

Where ICL Forge runs, and which page holds the detail. Where this page and a platform page
disagree, the platform page is right.

The overview compares products across platform families. The routing table below it is organised
by what you have, followed by one organised by how you want to call the code. The evidence claims
are not all the same strength; [Reading the evidence](#reading-the-evidence) sets out the four
levels that appear.

## Feature support at a glance

This table compares the broad feature set across platform families. **Source** means the feature
is available to code or applications built from this tree; **Limited** identifies a family where
variants differ. The evidence line says whether the claim rests on hardware, a build and test run
in CI, an experimental CI leg, a build alone, or emulation. Select the platform heading for its variant-level table.

--8<-- "docs-snippets/generated/platform-feature-overview.md"

## Product availability at a glance

**Published** means there is a package to download now; **Next release** means packaging is wired
but has not appeared on a tagged release. **Source** and **In development** do not promise a
downloadable application. The [home-page selector](../index.md) routes a product, platform and
variant to its release or build instructions.

--8<-- "docs-snippets/generated/platform-product-overview.md"

## Which page

| You have | Read | What runs there | Where it stands |
|---|---|---|---|
| **A Windows PC** (x64; ARM64 has the CLI only) | [Windows](windows.md) | The library, `forge`, `forge-gui`, Hearth and Crucible | Windows MSVC builds and tests in the merge queue, and the run after a merge to main builds it and clang-cl too. Live capture, decoded monitor playback, `spatial` rendering and exclusive-mode IEC 61937 passthrough (AC-3, E-AC-3 and signed Atmos) are all confirmed on real hardware, an Onkyo TX-RZ740 over HDMI. AC-4 is decoded and encoded; WASAPI has no IEC 61937 format for it, so it reaches an output as decoded PCM. |
| **A Linux PC** (x64 or arm64) | [Linux](linux.md) | The library, `forge`, `forge-gui` and Crucible; Hearth builds and is packaged, and has not been run | The pull-request gate builds and tests Linux GCC; the run after a merge to main adds Linux LLVM (x64) and Linux GCC (arm64). ALSA or PipeWire. `forge-gui` is opt-in at build time (`-DICLFORGE_BUILD_GUI=ON`). Bitstream output has reached a real receiver on [one machine](raspberry-pi.md#live-hdmi-passthrough-to-a-real-receiver). AC-4 bursts go out over ALSA, tested against ALSA's null device; PipeWire has no AC-4 codec, and no receiver is known to accept AC-4. |
| **A Mac** (Apple Silicon or Intel) | [macOS](macos.md) | The library, `forge`, `forge-gui`; Hearth and Crucible compile | The Apple Silicon leg builds and tests in the run after a merge to main, and the Intel leg in the nightly run. No Mac host is available to this project: nothing on macOS has captured or played a sound, and the [Core Audio process tap](macos.md#per-application-capture-the-core-audio-process-tap) has never been created. Core Audio defines no AC-4 format, so AC-4 can reach an output only as decoded PCM. |
| **A Raspberry Pi 4B** | [Raspberry Pi](raspberry-pi.md) | Everything Linux arm64 runs — there is no Pi-specific code | Run for real on a Pi 4B: as recorded on 2026-08-15 the full suite (440 tests then) passed on both compilers, including the hard real-time encode gate, and Atmos went out over HDMI to a powered AVR. The Pi 5 is expected to behave the same and is **not** validated; the Pi 3 is not a supported target. No Pi has run the AC-4 code. |
| **An NVIDIA Shield** (Android TV) | [Android](android.md) | Shield Atmos Demo, a demo app — not `forge`/`forge-gui` ported | Encodes Atmos/JOC live and plays it out the Shield's HDMI passthrough to a receiver, with a controller moving objects; confirmed on real 2017 Shield hardware. Sideload only, never the Play Store. It captures nothing and does nothing with AC-4: the NDK build compiles the AC-4 libraries and the app links none of them. |
| **A browser** | [WebAssembly](wasm.md) | Decode and encode modules over the same library, an Atmos object-authoring page, and an AC-4 module (decode and encode, objects included) that has no demo page | The demo pages are built and [published live](../wasm-demo.md). The `iclforge-wasm-decoder` npm package (AC-3 and E-AC-3 decode, and an `./ac4` wrapper over the AC-4 module) **has never been released to npm** — building it from `js/` is the only way to get it. |
| **An ESP32-S3** (ESP-IDF, FreeRTOS) | [ESP32-S3](bare-metal/esp32-s3.md) | `iclforge::ac3_minimal` as a reusable ESP-IDF component, decode-only or encode-only | The standalone probe decodes AC-3 and E-AC-3 **correctly** — every layout to 7.1.4, every coding tool, the output stage's folds, Atmos objects reconstructed **and placed onto 7.1.4** by their positions — every level exact against baked-in fixtures, inside internal SRAM with no PSRAM. The networked Hearth Sendspin sink requires a board with 8 MB of PSRAM. AC-3 and E-AC-3 encode 2/0 and 5.1 byte-exact with the host. Two [example players](bare-metal/esp32-s3.md#examples) drive I2S, one of them streaming from flash, SD or HTTP. **Real time on the board** for every decode fixture, from 0.07x for AC-3 mono to 0.92x for E-AC-3 7.1.4 folded to stereo, with objects placed onto 7.1.4 at 0.78x, at 240 MHz on an ESP32-S3-DevKitC-1 — see [Timing](bare-metal/esp32-s3.md#timing). **AC-3 2/0 and E-AC-3 2/0 encode in real time** (0.35x, 0.73x) with the encoders in `float` end to end and the search made cheaper; AC-3 5.1 (1.01x) sits at the line, the other rows 1.3x to 1.7x over, what remains being the exponent-run planner and the allocation candidates. AC-4 has not been measured on the S3. CI runs under QEMU, which cannot answer either. [What the part can and cannot do](bare-metal/esp32-s3.md#what-the-part-can-and-cannot-do) is the full table. |
| **An ESP32-C3** (ESP-IDF, FreeRTOS) | [ESP32-C3](bare-metal/esp32-c3.md) | The same `iclforge::ac3_minimal` component - its manifest lists `esp32c3` beside `esp32s3` - decoding in the fixed-point tier, since the part has no floating-point unit at all | **Decode is correct, and that is all this row claims.** `apps/baremetal/platform/esp32c3/` is a probe target CI runs under `qemu-riscv32`: twelve of the fourteen fixtures decode, and every one of them produces PCM **identical to the x86 host's and the Cortex-M3 leg's** - three architectures, three compilers, one pinned set of hashes. The two 7.1.4 rows do not fit in 400 KB of SRAM: they need 238,094 and 244,502 bytes of heap where the part reports 249,180 free in a heap whose largest block is 114,688. **Speed on a C3 is unmeasured** - QEMU is not cycle-accurate and no board has run this. Encode is not validated here at all: both encoders are floating-point, which on this part means software floating point. The AC-4 decoder builds in the fixed-point tier and does not fit: 2.0 peaks at 286,365 bytes. |
| **An ESP32-C6** (ESP-IDF, FreeRTOS) | [ESP32-C6](bare-metal/esp32-c6.md) | The same `iclforge::ac3_minimal` component, whose manifest lists `esp32c6`, decoding in the fixed-point tier: one 160 MHz RISC-V core with no floating-point unit, 512 KB of SRAM shared with WiFi, no PSRAM | **Measured on a board**, with no network and with WiFi connected and a 1,536 kbit/s TCP stream arriving. Decode is correct on all fourteen fixtures, the PCM identical to the host's, the Cortex-M3 leg's and the C3 leg's. With the network up, AC-3 and E-AC-3 5.1 decode in real time (0.82x and 0.96x), as do stereo and mono (0.17x to 0.41x), and E-AC-3 7.1 takes 1.09x; every fixture but 7.1.4 fits the heap. The AC-4 decoder builds in the fixed-point tier and does not fit beside WiFi: 2.0 peaks at 286,365 bytes, where the part has about 236,000 free (285,408 with WiFi's code in flash). QEMU does not emulate the part, so CI builds it and runs nothing. |
| **An ESP32-P4** (ESP-IDF, FreeRTOS) | [ESP32-P4](bare-metal/esp32-p4.md) | The same `iclforge::ac3_minimal` component, decoding in the float tier, with the AC-4 decoder behind `CONFIG_ICLFORGE_AC4`; `hearth_sink` for boards of silicon revision v1.x | **Measured on a board** (a DFRobot FireBeetle 2, chip revision v1.3), with no network, at 360 MHz, the ceiling of that revision. Decode is correct on all fourteen fixtures and real time on every one. The AC-4 decoder is off by default and real time for stereo only; [its section](bare-metal/esp32-p4.md#ac-4) has the plays, the hash agreement with the host and the memory. QEMU does not emulate the part, so CI builds it and runs nothing. |
| **A board with no operating system at all** | [Bare metal](bare-metal/cortex-m3.md) | `iclforge::ac3_minimal`: one static library, decode-only or encode-only, no exceptions, no RTTI | Cross-compiled `arm-none-eabi` and run on QEMU's `mps2-an385` (Cortex-M3, no FPU). A probe decodes six frames each of fourteen fixtures and gates on exact per-channel levels, image size, heap peak, retained bytes and allocation counts. CI runs it twice: in the default arithmetic, and in the [fixed-point tier](bare-metal/cortex-m3.md) (`-DICLFORGE_DECODE_SCALAR=fixed`, Q7.24 integers under a per-block exponent) which is what a part with no FPU wants and costs 0.37x the default build's instructions on the same leg. A third probe (`--ac4`) decodes six AC-4 streams, decode only, in float and in the fixed-point tier, with PCM bit-identical to the x86-64 host's in each. Correctness under emulation; no real silicon. |

**Windows kernel driver.** Crucible uses a kernel driver to provide its silent output device on
Windows. The Crucible archive carries the scripts that install it and not the driver, which is
built from source. It is **test-signed only** today and a Windows machine with default settings
refuses to load it, which is why [Crucible's install page](../crucible/install.md#without-the-driver)
covers running without it. [The null-sink driver on ACX](windows-driver-acx.md) has the detail, and
[the Windows demo page](windows-demo.md) has the phase record of the demo it grew out of.

## Which interface

| You want to call it from | Where that runs | Read |
|---|---|---|
| **A shell** — `forge` | Windows, Linux (including the Pi), macOS | [CLI reference](../forge/cli/index.md), [Forge](../forge/index.md) |
| **A window** — `forge-gui` | Windows, Linux and macOS. Shipped prebuilt on all three (the Windows installer and ZIP, the Linux packages and an x86_64 AppImage, the macOS universal DMG); from source, only the Windows presets default it on (`-DICLFORGE_BUILD_GUI=ON` elsewhere, and Qt is needed either way) | [GUI guide](../forge/gui/index.md) |
| **A desktop app** — Crucible | Windows and Linux; the macOS half compiles but has never been run | [Crucible](../crucible/index.md) |
| **A desktop player** — Hearth (`hearth`) | Windows, where it has been built and run; Linux and macOS, where it builds in CI. The next release packages it for Windows and Linux; the macOS release DMG does not carry it | [Hearth](../hearth/index.md) |
| **A Hearth sink** — `hearth_sink` | ESP32-S3 network player. Two boards as a group. Also runs on an ESP32-C6, stereo only ([setup guide](https://github.com/iainchesworthlabs/iclforge/blob/main/esp-idf/iclforge/examples/hearth_sink/README.md#on-the-esp32-c6)); CI builds the C6 player, which QEMU cannot run. CI also builds the ESP32-P4 sink firmware | [Hearth](../hearth/index.md), [An ESP32-S3 sink](../hearth/sink-esp32-s3.md) |
| **C++** | Every desktop platform above, plus WebAssembly and [bare metal](bare-metal/index.md) | [Library conventions](../library/index.md) |
| **C** | Wherever the C++ library builds | [C API](../library/c-api.md) |
| **Python** | `pip install iclforge` from the first release made after the rename; the pre-release on PyPI (0.10.0b1) is `pip install ac3forge`. Wheels for Windows x64, macOS arm64 and Linux x86_64. The Linux aarch64 and macOS x86_64 wheels build in CI and reach PyPI with the next release | [Python bindings](../library/python-api.md) |
| **Rust** | In-tree at `rust/`, over the C API; not published to crates.io | [Rust bindings](../library/rust-api.md) |
| **An ESP-IDF component** | An ESP32-S3, decode-only or encode-only; ESP32-C3 and ESP32-C6 decode in the fixed-point tier (C3 under QEMU, C6 timed on a board); ESP32-P4 decodes in the float tier, timed on a board at 360 MHz — this chip revision's ceiling, not the part's 400 MHz maximum. The AC-4 decoder is in the component behind `CONFIG_ICLFORGE_AC4` and has been measured on the P4 only. ESP-IDF owns the build, so there is no ICL Forge preset | [ESP32-S3](bare-metal/esp32-s3.md), [ESP32-C3](bare-metal/esp32-c3.md), [ESP32-C6](bare-metal/esp32-c6.md), [ESP32-P4](bare-metal/esp32-p4.md) |
| **An ESPHome component** | The same ESP32-S3 decoder, wrapped for an ESPHome project; not yet a `media_player` or `speaker` source | [ESPHome](bare-metal/esphome.md) |
| **JavaScript** | A browser, through WebAssembly; the npm package is unpublished | [WebAssembly](wasm.md) |
| **Shield Atmos Demo** | An NVIDIA Shield, sideloaded | [Android](android.md) |

## Reading the evidence

Four strengths of claim appear throughout these tables, and the distance between them matters:

- **Confirmed on hardware.** Somebody connected it and it worked: Windows passthrough, the
  Pi 4B driving an AVR, the Shield playing to a receiver.
- **Built and tested in CI.** The code builds and its tests pass on hosted runners. The
  pull-request gate builds Linux GCC, the merge queue adds Windows MSVC, the run after a merge to
  main adds the other legs a merge can break, and the nightly run adds the rest (see
  [CI for many agents](../ci-agentic.md#the-stages)). This is where macOS sits in full and where
  every desktop platform sits for anything that does not touch sound hardware. A leg whose
  failure fails no run is "experimental CI" in the tables.
- **Built, run under emulation.** The [bare-metal profile](bare-metal/cortex-m3.md) decodes correctly on
  QEMU, and so does the ESP32-S3 build. Correct output; nothing about timing on real silicon,
  which emulation cannot answer — the ESP32-S3's timing comes from a board, see
  [Timing](bare-metal/esp32-s3.md#timing), and so does the ESP32-C6's, which QEMU does not
  emulate at all.
- **Written, never exercised.** The macOS process tap, and the macOS half of Crucible. The code
  compiles and one test checks a version gate. Nothing more than that.

A target can sit at different levels for different things, and several do. macOS builds and tests
under the second level while its process tap sits at the fourth. Read the platform page before
depending on any of it.

## Codec detail

[Capabilities and limitations](../library/capabilities.md) lists every coding mode, layout,
sample rate, metadata field and Annex E tool with its spec citation. Desktop and WebAssembly
builds expose the full AC-3 and E-AC-3 codec, and the AC-4 decoder and encoder ([AC-4 decoding
and encoding](../library/ac4.md)). Minimum-footprint and fixed-point builds narrow that surface for
memory, arithmetic and timing constraints; the variant tables above and the individual bare-metal
pages record those limits. The AC-4 decoder is an option of the ESP-IDF component
(`CONFIG_ICLFORGE_AC4`, for parts with an FPU) that has been measured on the ESP32-P4 only.
The [application coverage
matrix](../library/application-coverage.md) shows which broad library capabilities each
application exposes.
