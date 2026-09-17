# Bare metal

Five pages, one profile: `ac3::forge_minimal`, the minimum-footprint build of the codec — one
static library, no exceptions, no RTTI, decode-only or encode-only, and none of the direct-form
transform tables. What differs between the pages is the part it targets and, on parts with no
floating-point unit, the arithmetic tier it decodes in
([the plan](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/arithmetic-tiers.md)).

The variant table keeps codec support, Hearth support, distribution and evidence separate.

--8<-- "docs-snippets/generated/platform-bare-metal.md"

Whether a part is viable at all comes down to floating point, not RAM — the comparison across the
wider ESP32 family, and why the ESP32-P4 was assessed and declined, is on
[ESP32-C3 → Why this part](esp32-c3.md#why-this-part-and-not-another-esp32-variant).

## Which page

- **Building the library itself for a part with no operating system?** Start at
  [Cortex-M3](cortex-m3.md) — it's the reference leg CI measures the profile on, and the page
  that explains what the profile gives up.
- **Have an ESP32-S3 board?** [ESP32-S3](esp32-s3.md) is real time on real hardware, with two
  example players (`i2s_player`, `hearth_sink`) that drive I2S.
- **Have an ESP32-C3 (or another part with no FPU)?** [ESP32-C3](esp32-c3.md) covers the
  fixed-point tier and what has and hasn't been measured on it.
- **Have an ESP32-C6?** [ESP32-C6](esp32-c6.md) has the fixed-point and float tiers timed on a
  board, with and without WiFi and a stream arriving, and which streams fit.
- **Building with ESPHome instead of raw ESP-IDF?** [ESPHome](esphome.md) is the external
  component, and where it stops short of a `media_player` or `speaker` source today.

## Where to go next

- [Platforms](../index.md) is one level up — the full routing table across every target, not only
  the bare-metal ones.
- [Capabilities](../../library/capabilities.md) — the complete codec surface; the table above
  records where minimum-footprint targets narrow it.
