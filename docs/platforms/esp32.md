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
| E-AC-3 §E3.5 enhanced coupling | **Correct.** Its own fixture, since `tools=all` does not select it; costs 126 allocations/frame against 86 |
| E-AC-3 2/0, §7.5.4 rematrixing | **Correct.** A layout no 5.1 stream reaches whatever its tools are |
| JOC / Atmos objects | **Does not fit.** Decodes correctly; see [Objects](#objects-do-not-fit-in-internal-sram) |
| Fits internal SRAM | **Yes**, no PSRAM: 280,792 bytes free against a 179,064-byte peak — see [Memory](#how-much-memory-there-actually-is) |
| Retained after teardown | 34,232 bytes of `thread_local` enhanced-coupling scratch, held for the life of the decoding task — see [Building](../building.md#gaps) |
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

## How much memory there actually is

The datasheet says **512 KB** of internal SRAM, `idf.py size` says **341,760**, and the allocator
says **280,792**. All three are true and they answer different questions.

| | Bytes | |
|---|---|---|
| Physical SRAM | 524,288 | the datasheet's 512 KB |
| − data cache | 32,768 | `CONFIG_ESP32S3_DATA_CACHE_SIZE`, carved out of SRAM2 |
| − instruction cache | 16,384 | `CONFIG_ESP32S3_INSTRUCTION_CACHE_SIZE`, already the minimum |
| = DRAM-addressable window | 491,520 | `SOC_DRAM_LOW`…`SOC_DRAM_HIGH` |
| DIRAM pool `idf.py size` reports | 341,760 | after ROM reservations and the non-doubly-mapped region |
| **free at runtime, this app** | **280,792** | what `heap_caps_get_free_size` returns |

`idf.py size`'s "remain" is a **linker estimate** — it was 207,084 where the allocator reports
280,792, 73,708 bytes pessimistic. A footprint budget quoted from it is a budget nobody checked.
`app_main` prints the runtime figures now, before and after the decode.

### Contiguity, not just total

| | Free | Largest block |
|---|---|---|
| Before the decode | 280,792 | 217,088 |
| After it | 245,248 | **116,736** |

The total falls 35,544 (the retained scratch, mostly). The largest **contiguous run** falls
100,352. That gap is the number that decides whether a large allocation succeeds, and the probe
cannot see it — its allocator hooks count bytes, not runs.

### There is no IRAM to reclaim here

ESP-IDF donates whatever IRAM an application does not fill to the heap as **32-bit-access-only**
memory, which `malloc` and `operator new` never return. That would suit this decoder well: its
large buffers are `float` and `double` arrays and never byte-addressed. Measured, the pool is
**0 bytes** — `idf.py size` reports IRAM as 16,384 of 16,384 used, so there is nothing left to
donate. Reclaiming it is not an option that exists on this build.

What is left is the caches. The instruction cache is already at its 16 KB minimum; the data
cache could go 32 KB → 16 KB and return 16,384 bytes. It is not free: `fixture.hpp` lives in
Flash Data, so a smaller data cache directly slows fixture reads. For a product streaming from
I2S rather than decoding flash-resident fixtures, the trade may look different.

### Stack

The decode runs on the main task. `uxTaskGetStackHighWaterMark` leaves **14,000 bytes free** of
the 32,768 `sdkconfig.defaults` sets, so the decode uses about 18,800. That file's own comment
told an integrator to measure this; now something does, and the runner holds a floor under it.

## Timing, and why QEMU's numbers are not it

The probe reports `decode_us`, `us_per_frame` and `realtime_permille` per codec.
Under `idf.py qemu` **those numbers are worthless**: QEMU is not a cycle-accurate
emulator, and it reports `cpu_mhz=40` against its own boot log's 160 MHz. They
are printed for shape, not for truth.

The real-time answer needs an **ESP32-S3-DevKitC-1-N16R8** and `idf.py -p <PORT>
flash monitor`. Nothing else is needed — the measurement is already wired.

## Other ESP32 variants

The dividing line is the FPU, not the RAM.

| Part | Usable RAM | Clock | FPU | Viable |
|---|---|---|---|---|
| **ESP32-S3** | 341,760 DIRAM | 240 MHz | single + PIE SIMD | **Yes** — the target here |
| **ESP32-P4** | 768 KB L2MEM | 400 MHz | single + AI ext | Best on paper; no integrated Wi-Fi |
| ESP32 (LX6) | ~320 KB | 240 MHz | single, no SIMD | Plausible, slower |
| ESP32-S2 | 320 KB | 240 MHz | **none** | No — soft-float everything |
| ESP32-C3/C6 | 400/512 KB | 160 MHz | **none** | No — same, slower |

The P4 would fit the working set without float32 at all, but it is still
single-precision, so `double` is still soft-float there too.

## Not done

- **PIE SIMD.** `src/internal/arch/` has no `f32x4`, so the float path runs
  scalar. The ESP32-S3's 128-bit extensions and `esp-dsp`'s published float32
  kernel costs are the reason to expect this is worth doing — but it should be
  driven by a measurement from real silicon, not by the assumption that it is.
- **AC-3's `decoder.cpp` is still `double`.** E-AC-3 was converted; AC-3 works
  but keeps both transform instantiations compiled.
- **Audio output.** The probe decodes baked-in fixtures. Nothing reaches I2S.

Adding fixtures does not move the internal-SRAM figure. The enhanced-coupling and 2/0 streams
added 13,824 bytes and DIRAM stayed at 134,676: `fixture.hpp` is `constexpr` data, and on this
part it lands in **Flash Data** (112,524 bytes) rather than DIRAM. The `arm-none-eabi` image
ceiling is the one fixture size spends against; here it costs flash, of which the app partition
has 66% free.

## Objects do not fit in internal SRAM

`src/forge/src/oba/joc.cpp` and `oamd.cpp` are both in `src/forge/minimal.cmake`'s source list
and link into every build of this profile, so the question was never whether object decode
compiles here. It was measured rather than argued: an `atmos-encode` fixture (six objects, JOC
over a 5.1 downmix, 448 kbit/s) was added to the probe, run on the `arm-none-eabi` leg, and
then removed.

It **decodes correctly** — all six bed channels exact — and its allocation churn is 80 per frame,
lower than the plain E-AC-3 fixture's 86. What it costs is memory:

| | Bytes |
|---|---|
| `oba::joc::ReconstructionState` | 147,504 |
| `ReconstructionState::QmfState` (`Domain::kQmf` is the default) | 34,360 |
| QMF analysis bank (5 × 5,120) | 25,600 |
| QMF synthesis banks (2 live × 12,800) | 25,600 |
| **JOC state** | **233,064** |
| **Probe peak heap, whole run** | **449,826** (against 179,064 without it) |

The allocator has 280,792 bytes free, so the peak overshoots by **169,034**. Not a ceiling to
raise: the probe would die in `operator new` the way the port originally did at `bytes=86016`.

**Contiguity rules it out a second time, independently.** `ReconstructionState` is one 147,504-byte
allocation, and the largest free block after any other decode is 116,736 (see
[Memory](#how-much-memory-there-actually-is)). It would fail on the single allocation even if the
budget allowed it.

All 233,064 bytes are `double`. `decode_scalar_t`'s float32 seam reaches both decoders' coefficient
stores but not JOC's reconstruction or the QMF bank — [Building](../building.md#gaps) records that
as a known non-gap. Three changes stack, and the arithmetic below is calculation from measured
sizes rather than a second measurement:

| | Peak | |
|---|---|---|
| As measured | 449,826 | |
| `Domain::kMdctBand` instead of `kQmf` | 364,266 | a config flag; `QmfState` and both banks stop existing |
| + float32 the JOC path | 290,514 | also brings the biggest allocation to ~73,752, under the 116,736 block |
| + size the object arrays to the stream | 259,794 | `kMaxObjects` is 16; the fixture carried 6 |

That last row fits, with about 21,000 bytes spare. So object decode here is reachable, and it needs
all three — the float32 conversion is load-bearing twice over, once for the total and once for the
contiguity.

Adding more fixtures would not have found any of this, which is why the fixture was measured and
removed rather than committed.

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
