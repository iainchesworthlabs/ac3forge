# ESP32-C3

The fixed-point tier's target: an Espressif ESP32-C3, RISC-V at 160 MHz with **no floating-point
unit at all**. `ac3::forge_minimal` decodes here the same way it does on
[the Cortex-M3 leg](cortex-m3.md)'s fixed-point build — `-DAC3FORGE_DECODE_SCALAR=fixed`, Q7.24
integers under a per-block exponent — because that is what a part with no FPU wants
([the plan](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/arithmetic-tiers.md)).
It shares the same `esp-idf/ac3forge/` component and manifest as [ESP32-S3](esp32-s3.md); only the
target and the arithmetic tier differ.

## Status

| | |
|---|---|
| Decode | **Correct, and that is all this row claims.** `apps/baremetal/platform/esp32c3/` is a probe target CI runs under `qemu-riscv32`: twelve of the fourteen fixtures decode, every one producing PCM **identical to the x86 host's and the Cortex-M3 leg's** — three architectures, three compilers, one pinned set of hashes (`tests/golden/fixed-probe-pcm-hashes.json`) |
| Memory | Peaks at 212,221 bytes of heap across the twelve fixtures that fit. The two 7.1.4 rows do not: they need 238,094 and 244,502 bytes where the part reports 249,180 free in a heap whose largest block is 114,688 |
| Speed | **Unmeasured.** QEMU is not cycle-accurate and no C3 board has run this. On the [Cortex-M3 leg](../../performance-trend.md#instructions-per-frame-fixed-point-tier) an E-AC-3 5.1 frame is 4.8 M integer instructions against 12.9 M soft-float, AC-3 5.1 3.8 M, AC-3 2/0 1.2 M and mono 0.61 M — against 5.12 M cycles per frame at 160 MHz, but instructions are not cycles and no leg models this part's 16 KB flash cache. The [ESP32-C6](esp32-c6.md), a 160 MHz RISC-V core with no FPU in the same tier, has been timed on a board: AC-3 5.1 at 0.64x a frame, E-AC-3 5.1 at 0.75x, stereo 0.20x and 0.32x, with no network |
| Encode | Not validated here at all: both encoders are floating-point, which on a part with no FPU means software floating point |
| Real silicon | None. Correctness is established under `qemu-riscv32` emulation |
| CI | The `esp32c3` fixed-point leg alongside `build-esp32s3` in `.github/workflows/_build.yml`, under QEMU |

## Why this part, and not another ESP32 variant

Whether a part is viable comes down to floating point, not RAM. Espressif measure a cosine at
~2,377 cycles on an ESP32-C3 against 121 on an ESP32-S3
([Floating-Point Units on Espressif SoCs](https://developer.espressif.com/blog/2025/10/cores_with_fpu/)).

| Part | Usable RAM | Clock | FPU | Vector unit | Viable |
|---|---|---|---|---|---|
| **ESP32-S3** | 341,760 DIRAM | 240 MHz | single | PIE, integer-only; 128-bit float load/store | **Yes** — [the primary target](esp32-s3.md) |
| **ESP32-P4** | 768 KB L2MEM | 400 MHz (360 on pre-production v1.x silicon) | single | PIE, integer-only; no wide float load | **Not as S3 replacement** — complementary **best** sink module, real time on every fixture, no network yet ([ESP32-P4](esp32-p4.md), [sink tiers](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/esp32-sink-tiers.md)) |
| ESP32 (LX6) | ~320 KB | 240 MHz | single | none | Plausible, slower |
| ESP32-S2 | 320 KB | 240 MHz | **none** | none | No — soft-float everything |
| **ESP32-C3**/C6 | 400/512 KB | 160 MHz | **none** | none | **Yes, in the fixed-point tier** — this page and [ESP32-C6](esp32-c6.md) |

Every part with an FPU has a single-precision one, so `double` is soft-float across the whole
family and `decode_scalar_t` earns its keep on all of them.

### Why not the ESP32-P4

Assessed and declined on 2026-09-08 **as a replacement for the ESP32-S3 Wi-Fi Sendspin
sink**. It is dual-core RISC-V at 400 MHz with 768 KB of SRAM, and holds the peak heap without
the float32 work — so it reads as the answer if the S3 misses real time. Three things were
checked and two settle that closer.

**Its vector extension has no floating point.** The P4 is `RV32IMAFC` plus `Xhwlp` and `Xesppie`,
vendor extensions no other implementation carries — not the ratified RISC-V Vector extension.
Across the 360 instructions in ESP-IDF's own decoder test for it, the only data types are `s8`,
`s16`, `s32`, `u8`, `u16`, `u32`. No `f32` anywhere. Espressif's own code agrees: in `esp-dsp`
every `_arp4` file using a PIE instruction sits under `fixed/`, and the float32 kernels contain
exactly one `esp.` instruction each — `esp.lp.setup`, the hardware loop — with scalar `fmadd.s`
arithmetic. So an `f32x4` has nothing to compile to there either. (The S3 has the same
integer-only PIE limit and still decodes in scalar float.)

**It has no radio, and the plan it would *replace* is a Wi-Fi plan.** No Wi-Fi and no Bluetooth;
it needs a companion ESP32-C6 or -H2 (or Ethernet). That disqualifies it as a drop-in for the
S3 node. The S3 probe has since cleared every fixture in real time, so the “rescue the decode”
motivation is gone for that product shape.

What the P4 would buy is clock — 12.8 M cycles per frame against the S3's 7.68 M, **1.67×**,
per-core in both cases — plus wider TDM (up to 16×32-bit on one controller) and more internal
SRAM.

**Reopened 2026-09-21 as a complementary “best” module** on a shared dual-ES9080 PCB (C6 =
good ≤5.1 / one DAC; S3 = better ≤7.1.4 without enhanced coupling / both DACs @ 16-bit; P4 =
best ≤9.1.6 with full tools desired / both DACs @ 32-bit). See
[`planning/esp32-sink-tiers.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/esp32-sink-tiers.md).
Phase P1 of that plan is done: [ESP32-P4](esp32-p4.md) is real time on every fixture, no network
yet, on a board — with a chip-revision trap specific to pre-production silicon worth reading
before flashing one.

## Building

Uses the same `esp-idf/ac3forge/` component and `EXTRA_COMPONENT_DIRS` setup as
[ESP32-S3 → The ESP-IDF component](esp32-s3.md#the-esp-idf-component). `apps/baremetal/platform/esp32c3/`
is the probe target:

```bash
. $IDF_PATH/export.sh
python "$IDF_PATH/tools/idf_tools.py" install qemu-riscv32   # once
cd apps/baremetal/platform/esp32c3
idf.py set-target esp32c3
idf.py build                                  # -DAC3FORGE_DECODE_SCALAR=fixed by default
idf.py qemu
```

`tools/checks/run_esp32c3_probe.sh` drives that leg. Its distinguishing gate is not a footprint
ceiling but the PCM itself: the tier's arithmetic is integer, so the probe's per-fixture hashes
are the same on RISC-V as on the x86 host and the Cortex-M3 leg, and the runner holds all three to
one pinned set (`tests/golden/fixed-probe-pcm-hashes.json`). `--scalar=float` builds the same part
with the S3's tier, which is what the two arithmetics are compared with.

Any other project built for this target against the component gets the fixed-point tier too:
when a project leaves `AC3FORGE_DECODE_SCALAR` unset, the component sets it from the part, and a
part with no FPU gets `fixed`
([ESP32-S3 → The ESP-IDF component](esp32-s3.md#the-esp-idf-component)).

## Where to go next

- [ESP32-S3](esp32-s3.md) — the primary, hardware-verified target this component was built for.
- [ESP32-C6](esp32-c6.md) — the same tier on a board, with and without WiFi running.
- [Cortex-M3 (QEMU reference)](cortex-m3.md) — the same fixed-point tier's reference leg.
- [Bare metal overview](index.md) — how the pages in this section relate.
