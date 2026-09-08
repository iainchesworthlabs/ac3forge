# ESP32-S3

The minimum-footprint decoder profile (roadmap PF7) on an Espressif ESP32-S3: a
240 MHz dual-core Xtensa LX7 with a single-precision FPU, 128-bit PIE SIMD
extensions, 512 KB of internal SRAM and hardware I2S.

This is the second bare-metal target. The first, `arm-none-eabi` on QEMU's
`mps2-an385`, is a Cortex-M3 with no FPU at all — the least forgiving target and
the right one to prove correctness on, and one nobody ships. An ESP32-S3 is a
part that ends up in products, and the first target where "does this decode in
real time?" is a question worth asking rather than a foregone no.

## Status

| | |
|---|---|
| AC-3 5.1 decode | **Correct.** Six frames, all six channel levels exact against `apps/baremetal/fixture.hpp` |
| E-AC-3 5.1 decode | **Correct.** Same, including AHT and spectral extension |
| Fits internal SRAM | **Yes**, no PSRAM: 202,860 bytes free against a 171,558-byte peak |
| Real time | **Not yet measured.** See [Timing](#timing-and-why-qemus-numbers-are-not-it) |
| CI | `build-esp32s3` in `.github/workflows/_build.yml`, under QEMU |

## Building

ESP-IDF owns the top-level build, the way Gradle does for
[Android](android.md) — so there is no ac3forge preset for this target and no
entry in `cmake/toolchains/`. The project reaches back into this repo from the
other direction: `apps/baremetal/platform/esp32s3/components/ac3forge/` pre-seeds
the root's `option()`s and `add_subdirectory()`s the repo root, which is exactly
the shape `apps/android/app/src/main/cpp/CMakeLists.txt` uses.

Re-listing `src/forge/minimal.cmake`'s source list in an
`idf_component_register(SRCS ...)` was the alternative and was rejected: two
copies of a source list drift, and the drift is silent until a link error on a
target nobody builds locally.

```bash
. $IDF_PATH/export.sh
cd apps/baremetal/platform/esp32s3
idf.py set-target esp32s3
idf.py build
idf.py qemu                    # no board needed
idf.py -p <PORT> flash monitor # a real board
```

Verified against **ESP-IDF v6.1.0**, which ships Xtensa **GCC 15.2.0** and
defaults to `-std=gnu++26`. The library's C++23 use — `std::expected`,
`std::unreachable`, `std::byteswap` — compiles cleanly under `-fno-exceptions
-fno-rtti`, as does `constexpr std::vector`. The libstdc++ problem in
[esp-idf#18172](https://github.com/espressif/esp-idf/issues/18172) is a GCC 14.2
one, so IDF 6.1 is a *safer* choice here than 5.5, not a riskier one.

## What the port needed from the library

### A `thread_local` that made the library unlinkable on any RTOS

`eac3_tools.cpp`'s enhanced-coupling scratch was a 32 KB `thread_local`. On a
single-threaded bare-metal target that costs 32 KB once. On FreeRTOS it is fatal,
and not as a sizing problem:

```c
/* freertos/FreeRTOS-Kernel/portable/xtensa/port.c */
const uint32_t tls_area_size = ALIGNUP(16, tls_data_size + tls_bss_size);
uxStackPointer = STACKPTR_ALIGN_DOWN(16, uxStackPointer - tls_area_size);
```

FreeRTOS carves each task's thread-local area out of **that task's own stack**,
and sizes it from the linked image's `.tdata + .tbss` — the same for every task
in the system, whether or not it has ever heard of this decoder. ESP-IDF's own
IPC task has a 1 KB stack, so it could not be created at all: the application
died in an assert inside `esp_ipc_init()`, before `app_main`, having never
decoded a frame.

Keeping only a `unique_ptr` in TLS takes every task's area from 32 KB to one
pointer. The `arm-none-eabi` leg got that for free: `.tbss` 32,784 → 24 bytes,
and once `tls.cpp`'s block was resized to match, the image fell 22%.

### float32 for the decode path

The LX7's FPU is single-precision, so a `double` coefficient buys precision
nothing downstream can use. The memory was the binding constraint first: the
per-block `coeffs` store is 100,352 bytes and `aht_coeffs_` 86,016, against
160,764 bytes of free internal SRAM.

`src/internal/profile/{minimal,full}/`'s seam carries `decode_scalar_t` — `float`
in the minimum-footprint profile, `double` everywhere else — and four decoder
buffers follow it. See [Building](../building.md#minimum-footprint-decoder-profile)
for what the profile changes.

## Configuration that is not obvious

Both live in `sdkconfig.defaults` with their reasoning, but they cost enough time
to be worth repeating:

- **`CONFIG_ESP_MAIN_TASK_STACK_SIZE=32768`.** IDF's default is 3,584 bytes,
  which is sensible for an application that wires up peripherals and waits on
  queues, and far too small for a codec. The overflow did not report itself as a
  stack overflow — it surfaced as a `LoadProhibited` panic on the *other core's*
  idle task, inside the task watchdog's own bookkeeping, because the overflow
  corrupted a neighbouring structure.
- **`CONFIG_ESP_TASK_WDT_INIT=n`.** The probe is a batch computation that runs
  the CPU flat out and never yields, which is what the watchdog exists to catch.
  A decoder in a real product should keep the watchdog and give the decode its
  own task with a bounded per-frame budget.

**PSRAM stays off**, despite the development board having 8 MB. QEMU cannot
emulate S3 PSRAM ([espressif/qemu#129](https://github.com/espressif/qemu/issues/129)),
so a build that needs it cannot run in CI; and the internal-SRAM budget is a
number this build should have to meet rather than one it can hide from.

## Timing, and why QEMU's numbers are not it

The probe reports `decode_us`, `us_per_frame` and `realtime_permille` per codec.
Under `idf.py qemu` **those numbers are worthless**: QEMU is not a cycle-accurate
emulator, and it reports `cpu_mhz=40` against its own boot log's 160 MHz. They
are printed for shape, not for truth.

The real-time answer needs an **ESP32-S3-DevKitC-1-N16R8** and `idf.py -p <PORT>
flash monitor`. Nothing else is needed — the measurement is already wired.

## Other ESP32 variants

The dividing line is the FPU, not the RAM.

| Part | Usable RAM | Clock | FPU | Vector unit | Viable |
|---|---|---|---|---|---|
| **ESP32-S3** | 341,760 DIRAM | 240 MHz | single | PIE, integer-only; 128-bit float load/store | **Yes** — the target here |
| **ESP32-P4** | 768 KB L2MEM | 400 MHz | single | PIE, integer-only; no wide float load | Fits trivially, costs the radio — [below](#the-esp32-p4-and-why-it-is-not-the-next-target) |
| ESP32 (LX6) | ~320 KB | 240 MHz | single | none | Plausible, slower |
| ESP32-S2 | 320 KB | 240 MHz | **none** | none | No — soft-float everything |
| ESP32-C3/C6 | 400/512 KB | 160 MHz | **none** | none | No — same, slower |

Every part above that has an FPU at all has a single-precision one, so `double`
is soft-float across the whole family and `decode_scalar_t` earns its keep on
all of them rather than only here.

### The ESP32-P4, and why it is not the next target

Assessed 2026-09-08. The P4 is dual-core RISC-V at 400 MHz with 768 KB of
SRAM, and it holds the 171,558-byte peak heap without the float32 work that
this port needed. It looks like the answer if the S3 turns out not to be real
time. Three things were checked before writing any of it. Two came back
against.

**Its vector extension is vendor-specific, and it has no floating point at
all.** The P4 is `RV32IMAFC` plus two custom extensions: `Xhwlp` (hardware
loop) and `Xesppie` (the vector unit). RISC-V reserves the `X` prefix for
vendor extensions no other implementation carries, so this is not the ratified
RISC-V Vector extension, and kernels written against it would serve the P4
alone — the same single-target bargain as the Xtensa work. Espressif document
"PIE" for both parts, which makes it easy to assume otherwise.

The floating-point half is the part that decides it. ESP-IDF carries an
exhaustive decoder test for the extension in
`components/esp_gdbstub/test_gdbstub_host/rv_decode/xesppie.S`; across its 360
instructions the only data-type suffixes that appear are `s8`, `s16`, `s32`,
`u8`, `u16` and `u32`. `esp-dl`'s own `esp32p4-pie-simd` notes say the same in
one line — *"datatype: s8, s16, s32 (signed); u8, u16 (unsigned)"*. There is
no `f32` anywhere in it.

Espressif's own code agrees. In `esp-dsp`, every `_arp4` file that uses a PIE
vector instruction sits under a `fixed/` directory. The float32 kernels —
`dsps_dotprod_f32_arp4.S`, `dsps_fft2r_fc32_arp4.S`, `dsps_fft4r_fc32_arp4.S`,
`dsps_biquad_f32_arp4.S` — contain exactly one `esp.` instruction each, and it
is `esp.lp.setup`, the hardware loop. Their arithmetic is scalar `fmadd.s` on
scalar `flw` loads. `esp-gmf`'s PIE-accelerated FFT for the part is
`fft_pie_radix2_dit_s16.S`: int16. An FFT in fixed point is where a float
vector unit would show up first if there were one to use.

So a `f32x4` has nothing to compile to on a P4. The decode path is float32 by
`decode_scalar_t`, and would stay scalar there.

**For this workload the P4 is behind the S3, not ahead of it.** The S3's
float32 dot product, `dsps_dotprod_f32_aes3.S`, opens with `EE.LDF.128.IP` —
a 128-bit load landing four floats in four FPU registers — and then runs four
independent scalar `madd.s` into four accumulators. That is load bandwidth
plus instruction-level parallelism rather than a four-wide float ALU, and it
is worth having. The P4's equivalent has no wide float load; it loads one
float at a time. Whatever the S3's float path is eventually worth, the P4 does
not inherit it.

What the P4 does buy over the S3 is clock and memory. A frame is 1536 samples,
32 ms at 48 kHz, which is 7.68 M cycles of budget at 240 MHz against 12.8 M at
400 MHz: **1.67×**, and it is per-core in both cases. The memory advantage is
already spent — this port fits internal SRAM on the S3 with 202,860 bytes free
against a 171,558-byte peak.

**It has no radio, and the plan it would serve is a Wi-Fi plan.** The P4 has
neither Wi-Fi nor Bluetooth and needs a companion ESP32-C6 or -H2 for either,
making any networked build a two-chip design.
[`docs/family/topology.md`](../family/topology.md) puts a network transport in
front of the decoder, and its Phase 5 exit is *"an ESP32-S3 decoding E-AC-3
from a network origin in real time"*; the bandwidth argument for carrying a
compressed stream at all is stated there as the difference between an ESP32-S3
receiving Atmos over Wi-Fi and one receiving no surround. ESPHome nodes are
Wi-Fi devices. A part that has to be paired with a second chip to reach the
network is working against that, and it is a product-shape question rather
than a technical one.

**Whether the S3 needs rescuing is still unmeasured**, which is the only one of
the three that could reopen this. [Timing](#timing-and-why-qemus-numbers-are-not-it)
has what that costs: one board. Until that number exists, the P4's headroom is
headroom nobody has shown is needed, bought by giving up the radio.

**What would change the answer.** A measurement from S3 silicon showing the
decode short of real time by less than about 1.67× — close enough that clock
alone closes it, since no SIMD gain is coming from the P4. Short by more than
that, and neither part is the answer and the fix is in the decoder. Comfortably
real time, and the question does not arise.

## Not done

- **PIE SIMD.** `src/internal/arch/` has no `f32x4`, so the float path runs
  scalar. Worth being precise about what the S3 offers here, because the name
  oversells it: PIE's vector ALU is integer-only, and what
  `esp-dsp`'s float32 kernels actually use is `EE.LDF.128.IP` — a 128-bit load
  filling four FPU registers — feeding four independent scalar `madd.s` into
  four accumulators. The gain available is load bandwidth and instruction-level
  parallelism, not a four-wide float multiply. That is still worth having, and
  it still needs a measurement from real silicon first rather than the
  assumption that it is.
- **AC-3's `decoder.cpp` is still `double`.** E-AC-3 was converted; AC-3 works
  but keeps both transform instantiations compiled.
- **Audio output.** The probe decodes a baked-in fixture. Nothing reaches I2S.

## ESPHome

Not built, but the pathway is confirmed and each step exists:

1. **ac3forge as an ESP-IDF component.** Already true —
   `apps/baremetal/platform/esp32s3/components/ac3forge/` is one, though it lives
   inside the probe app and would want relocating somewhere reusable first.
2. **An ESPHome external component**, `components/ac3_decoder/{__init__.py,
   *.cpp}` in a git repo, referenced from YAML via `external_components:`.
3. **Pulling the library in**, with `add_idf_component(name=..., repo=..., ref=...)`
   from that component's `to_code()` — the same mechanism ESPHome's own `mqtt` and
   `usb_host` components use for IDF 6.0's registry-hosted dependencies.

The frameworks line up: ESPHome's `speaker` media_player platform is ESP-IDF-only
already. The obstacle is not integration, it is that real-time decode is unproven
— an ESPHome node that cannot keep up is worse than no node.
