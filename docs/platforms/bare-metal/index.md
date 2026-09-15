# Bare metal

Five pages, one profile: `ac3::forge_minimal`, the minimum-footprint build of the codec — one
static library, no exceptions, no RTTI, decode-only or encode-only, and none of the direct-form
transform tables. What differs between the pages is the part it targets and, on parts with no
floating-point unit, the arithmetic tier it decodes in
([the plan](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/arithmetic-tiers.md)).

| | [Cortex-M3](cortex-m3.md) | [ESP32-S3](esp32-s3.md) | [ESP32-C3](esp32-c3.md) | [ESP32-C6](esp32-c6.md) | [ESPHome](esphome.md) |
|---|---|---|---|---|---|
| What it is | The reference target: `arm-none-eabi` cross-compiled for QEMU's `mps2-an385` | Espressif's dual-core Xtensa part, hardware single-precision FPU | Espressif's RISC-V part, **no FPU at all** | Espressif's RISC-V part with WiFi 6, no FPU, 512 KB of SRAM | An external component wrapping the ESP32-S3 decoder for ESPHome projects |
| Arithmetic | Default (software `double`) and the fixed-point tier, both gated in CI | Default: the profile's scalar in `float`, hand-tuned against the ROM's software `double` | Fixed-point tier only — the arithmetic a part with no FPU wants | Fixed-point tier by default; float measured beside it | Whatever the wrapped ESP32-S3 component uses |
| Decode | Correct, every fixture | **Correct, and real time on a board** — every fixture, including Atmos objects placed onto 7.1.4 | Correct on 12 of 14 fixtures under emulation; the two 7.1.4 rows don't fit the heap | Correct on every fixture on a board. With WiFi up, stereo and mono in real time and no 5.1 stream | N/A — plumbing only, not a decoder of its own |
| Encode | Correct, six rows, separate profile | Correct; several rows real time on a board, the widest still over | Not validated — both encoders are floating-point | Not validated — both encoders are floating-point | N/A |
| Real silicon | None — QEMU only | **Yes.** Every timing and memory figure on this page set is from a board | None — `qemu-riscv32` only | **Yes**, with and without a network running | Inherits the ESP32-S3 component's evidence |
| CI | `build-footprint`, every push | `build-esp32s3`, under QEMU | The fixed-point `esp32c3` leg, under `qemu-riscv32` | Built beside the C3 leg; QEMU does not emulate the part | `esphome config` against the manifest, on every change |

Whether a part is viable at all comes down to floating point, not RAM — the comparison across the
wider ESP32 family, and why the ESP32-P4 was assessed and declined, is on
[ESP32-C3 → Why this part](esp32-c3.md#why-this-part-and-not-another-esp32-variant).

## Which page

- **Building the library itself for a part with no operating system?** Start at
  [Cortex-M3](cortex-m3.md) — it's the reference leg CI measures the profile on, and the page
  that explains what the profile gives up.
- **Have an ESP32-S3 board?** [ESP32-S3](esp32-s3.md) is real time on real hardware, with two
  example players (`i2s_player`, `stream_player`) that drive I2S.
- **Have an ESP32-C3 (or another part with no FPU)?** [ESP32-C3](esp32-c3.md) covers the
  fixed-point tier and what has and hasn't been measured on it.
- **Have an ESP32-C6?** [ESP32-C6](esp32-c6.md) has the fixed-point and float tiers timed on a
  board, with and without WiFi and a stream arriving, and which streams fit.
- **Building with ESPHome instead of raw ESP-IDF?** [ESPHome](esphome.md) is the external
  component, and where it stops short of a `media_player` or `speaker` source today.

## Where to go next

- [Platforms](../index.md) is one level up — the full routing table across every target, not only
  the bare-metal ones.
- [Capabilities](../../library/capabilities.md) — what the codec does, the same everywhere; this
  section is about where it runs, not what it can encode or decode.
