# Bare metal

Five targets, one profile: `ac3::forge_minimal`, the minimum-footprint build of the codec — one
static library, no exceptions, no RTTI, decode-only or encode-only, and none of the direct-form
transform tables. What differs between the targets is the part and, on parts with no
floating-point unit, the arithmetic tier the decoder runs in
([the plan](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/arithmetic-tiers.md)).

AC-4 is built from its own libraries (`src/ac4`, `src/ac4core`, `src/ac4dec`), the decoder only:
no bare-metal build has the AC-4 encoder. The [Cortex-M3](cortex-m3.md#status) leg probes it, and the
[ESP32-P4](esp32-p4.md#ac-4) decodes it behind `CONFIG_AC3FORGE_AC4`. It is not built for the
ESP32-S3, the ESP32-C6 or the ESP32-C3 yet: those are phases D14c and D14d of
[`planning/ac4.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/ac4.md#d14-ac-4-on-the-esp32s).
No ESP32 sink takes AC-4 in a Sendspin group either.

The variant table keeps codec support, Hearth support, distribution and evidence separate.

--8<-- "docs-snippets/generated/platform-bare-metal.md"

Whether a part is viable at all comes down to floating point, not RAM — the comparison across the
wider ESP32 family, why the ESP32-P4 was declined as an S3 *replacement*, and the complementary
C6 / S3 / P4 sink modules on one dual-ES9080 PCB, are on
[ESP32-C3 → Why this part](esp32-c3.md#why-this-part-and-not-another-esp32-variant) and
[`planning/esp32-sink-tiers.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/esp32-sink-tiers.md).

## Which page

- **Building the library itself for a part with no operating system?** Start at
  [Cortex-M3](cortex-m3.md) — it's the reference leg CI measures the profile on, and the page
  that explains what the profile gives up.
- **Have an ESP32-S3 board?** [ESP32-S3](esp32-s3.md) is real time on real hardware, with two
  example players (`i2s_player`, `hearth_sink`) that drive I2S, and the Sendspin sink. AC-3 and
  E-AC-3 only.
- **Have an ESP32-C3 (or another part with no FPU)?** [ESP32-C3](esp32-c3.md) covers the
  fixed-point tier and what has and hasn't been measured on it. AC-3 and E-AC-3 only.
- **Have an ESP32-C6?** [ESP32-C6](esp32-c6.md) has the fixed-point and float tiers timed on a
  board, with and without WiFi and a stream arriving, and which streams fit. AC-3 and E-AC-3
  only.
- **Have an ESP32-P4?** [ESP32-P4](esp32-p4.md) is the "best" tier of the shared sink family. Its
  probe is real time on every fixture with no network, and `hearth_sink` plays a paired Sendspin
  stream over Wi-Fi through the board's onboard ESP32-C6. If the board is pre-production silicon
  like the one this was measured on, the page also has a chip-revision trap worth reading before
  flashing anything. It decodes AC-4 behind a switch, from an HTTP source: 2.0 in SIMPLE and
  A-SPX modes in real time, wider layouts slower ([AC-4](esp32-p4.md#ac-4)).
- **Building with ESPHome instead of raw ESP-IDF?** [ESPHome](esphome.md) is the external
  component. It decodes AC-3 only, and it is not a `media_player` or `speaker` source.

## Where to go next

- [Platforms](../index.md) is one level up — the full routing table across every target, not only
  the bare-metal ones.
- [Capabilities](../../library/capabilities.md) — the complete codec surface; the table above
  records where minimum-footprint targets narrow it.
