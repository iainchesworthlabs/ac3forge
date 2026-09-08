# ESP32-S3

The minimum-footprint decoder profile (roadmap PF7) on an Espressif ESP32-S3: a
240 MHz dual-core Xtensa LX7 with a single-precision FPU, 128-bit PIE SIMD
extensions, 512 KB of internal SRAM and hardware I2S.

It is the second bare-metal target. The first is `arm-none-eabi` on QEMU's
`mps2-an385`, a Cortex-M3 with no FPU, where every floating-point operation is
software-emulated. The ESP32-S3 has hardware single-precision floating point, so
it is the first target where real-time decode is worth measuring.

## Status

| | |
|---|---|
| AC-3 5.1 decode | Correct. Six frames, all six channel levels exact against `apps/baremetal/fixture.hpp` |
| E-AC-3 5.1 decode | Correct. Same, including AHT and spectral extension |
| E-AC-3 §E3.5 enhanced coupling | Correct. Its own fixture, since `tools=all` does not select it; costs 126 allocations/frame against 86 |
| E-AC-3 2/0, §7.5.4 rematrixing | Correct. A layout no 5.1 stream reaches whatever its tools are |
| JOC / Atmos objects | Does not fit. Decodes correctly; see [Objects](#objects-do-not-fit-in-internal-sram) |
| Fits internal SRAM | Yes, without PSRAM: 280,792 bytes free against a 179,064-byte peak — see [Memory](#how-much-memory-there-actually-is) |
| Retained after teardown | 34,232 bytes of `thread_local` enhanced-coupling scratch, held for the life of the decoding task — see [Building](../building.md#gaps) |
| Real time | Not measured. See [Timing](#timing) |
| CI | `build-esp32s3` in `.github/workflows/_build.yml`, under QEMU |

## Building

ESP-IDF owns the top-level build, as Gradle does for [Android](android.md), so
there is no ac3forge preset for this target and no entry in `cmake/toolchains/`.
The project reaches into this repo from the other direction:
`apps/baremetal/platform/esp32s3/components/ac3forge/` pre-seeds the root's
`option()`s and `add_subdirectory()`s the repo root, the same shape
`apps/android/app/src/main/cpp/CMakeLists.txt` uses.

The alternative was re-listing `src/forge/minimal.cmake`'s source list in an
`idf_component_register(SRCS ...)`. That was rejected because two copies of a
source list drift, and the drift surfaces as a link error rather than as a
diff.

```bash
. $IDF_PATH/export.sh
cd apps/baremetal/platform/esp32s3
idf.py set-target esp32s3
idf.py build
idf.py qemu                    # no board needed
idf.py -p <PORT> flash monitor # a real board
```

Verified against ESP-IDF v6.1.0, which ships Xtensa GCC 15.2.0 and defaults to
`-std=gnu++26`. The library's C++23 use — `std::expected`, `std::unreachable`,
`std::byteswap` — compiles under `-fno-exceptions -fno-rtti`, as does
`constexpr std::vector`. The libstdc++ problem in
[esp-idf#18172](https://github.com/espressif/esp-idf/issues/18172) is specific to
GCC 14.2, so it does not apply to IDF 6.1.

## What the port needed from the library

### A `thread_local` that made the library unlinkable on any RTOS

`eac3_tools.cpp`'s enhanced-coupling scratch was a 32 KB `thread_local`. On a
single-threaded bare-metal target that costs 32 KB once. On FreeRTOS it prevents
the application starting:

```c
/* freertos/FreeRTOS-Kernel/portable/xtensa/port.c */
const uint32_t tls_area_size = ALIGNUP(16, tls_data_size + tls_bss_size);
uxStackPointer = STACKPTR_ALIGN_DOWN(16, uxStackPointer - tls_area_size);
```

FreeRTOS carves each task's thread-local area out of that task's own stack, and
sizes it from the linked image's `.tdata + .tbss` — the same size for every task
in the system, including tasks that never call into the decoder. ESP-IDF's IPC
task has a 1 KB stack, so it could not be created, and the application failed an
assert inside `esp_ipc_init()` during startup, before `app_main`.

Keeping only a `unique_ptr` in TLS takes every task's area from 32 KB to one
pointer. The `arm-none-eabi` leg benefits too: `.tbss` fell from 32,784 bytes to
24, and resizing `tls.cpp`'s block to match reduced the image by 22%.

### float32 for the decode path

The LX7's FPU is single-precision, so `double` coefficients are wider than
anything downstream can use. Memory was the binding constraint: the per-block
`coeffs` store is 100,352 bytes and `aht_coeffs_` 86,016, against 160,764 bytes
of free internal SRAM at the time.

`src/internal/profile/{minimal,full}/`'s seam carries `decode_scalar_t` — `float`
in the minimum-footprint profile, `double` in every other build — and four
decoder buffers follow it. See
[Building](../building.md#minimum-footprint-decoder-profile) for what the profile
changes, and its Gaps section for the measured accuracy cost.

## Configuration

Both settings are in `sdkconfig.defaults` with their reasoning. They are
repeated here because neither failure mode points at its cause.

- **`CONFIG_ESP_MAIN_TASK_STACK_SIZE=32768`.** IDF's default is 3,584 bytes,
  which suits an application that configures peripherals and waits on queues but
  is too small for a codec. The overflow does not report as a stack overflow: it
  surfaces as a `LoadProhibited` panic on the other core's idle task, inside the
  task watchdog's bookkeeping, because the overflow corrupts a neighbouring
  structure.
- **`CONFIG_ESP_TASK_WDT_INIT=n`.** The probe is a batch computation that runs
  the CPU continuously without yielding, which is what the watchdog exists to
  catch. A decoder in a product should keep the watchdog and give the decode its
  own task with a bounded per-frame budget.

PSRAM is off, although the development board has 8 MB. QEMU cannot emulate S3
PSRAM ([espressif/qemu#129](https://github.com/espressif/qemu/issues/129)), so a
build that requires it cannot run in CI, and keeping it off means the
internal-SRAM budget is enforced rather than avoided.

## How much memory there actually is

The datasheet says 512 KB of internal SRAM, `idf.py size` says 341,760, and the allocator
says 280,792. All three are true and they answer different questions.

| | Bytes | |
|---|---|---|
| Physical SRAM | 524,288 | the datasheet's 512 KB |
| − data cache | 32,768 | `CONFIG_ESP32S3_DATA_CACHE_SIZE`, carved out of SRAM2 |
| − instruction cache | 16,384 | `CONFIG_ESP32S3_INSTRUCTION_CACHE_SIZE`, already the minimum |
| = DRAM-addressable window | 491,520 | `SOC_DRAM_LOW`…`SOC_DRAM_HIGH` |
| DIRAM pool `idf.py size` reports | 341,760 | after ROM reservations and the non-doubly-mapped region |
| **free at runtime, this app** | 280,792 | what `heap_caps_get_free_size` returns |

`idf.py size`'s "remain" is a **linker estimate** — it was 207,084 where the allocator reports
280,792, 73,708 bytes pessimistic. A footprint budget quoted from it is a budget nobody checked.
`app_main` prints the runtime figures now, before and after the decode.

### Contiguity, not just total

| | Free | Largest block |
|---|---|---|
| Before the decode | 280,792 | 217,088 |
| After it | 245,248 | 116,736 |

The total falls 35,544 (the retained scratch, mostly). The largest contiguous run falls
100,352. That gap is the number that decides whether a large allocation succeeds, and the probe
cannot see it — its allocator hooks count bytes, not runs.

### There is no IRAM to reclaim here

ESP-IDF donates whatever IRAM an application does not fill to the heap as 32-bit-access-only
memory, which `malloc` and `operator new` never return. That would suit this decoder well: its
large buffers are `float` and `double` arrays and never byte-addressed. Measured, the pool is
0 bytes — `idf.py size` reports IRAM as 16,384 of 16,384 used, so there is nothing left to
donate. Reclaiming it is not an option that exists on this build.

What is left is the caches. The instruction cache is already at its 16 KB minimum; the data
cache could go 32 KB → 16 KB and return 16,384 bytes. It is not free: `fixture.hpp` lives in
Flash Data, so a smaller data cache directly slows fixture reads. For a product streaming from
I2S rather than decoding flash-resident fixtures, the trade may look different.

### Stack

The decode runs on the main task. `uxTaskGetStackHighWaterMark` leaves 14,000 bytes free of
the 32,768 `sdkconfig.defaults` sets, so the decode uses about 18,800. That file's own comment
told an integrator to measure this; now something does, and the runner holds a floor under it.

## Timing

The probe reports `decode_us`, `us_per_frame` and `realtime_permille` per codec.

Under `idf.py qemu` these figures do not describe the hardware. QEMU is not a
cycle-accurate emulator, and it reports `cpu_mhz=40` against its own boot log's
160 MHz. Treat the QEMU leg as a correctness and footprint check only.

Measuring real-time performance requires an ESP32-S3-DevKitC-1-N16R8 and
`idf.py -p <PORT> flash monitor`. The instrumentation is already in place.

## Other ESP32 variants

Whether the part has an FPU decides this; the RAM does not. Espressif publish the
split, and measure a cosine at ~2,377 cycles on an ESP32-C3 against 121 on an
ESP32-S3 ([Floating-Point Units on Espressif
SoCs](https://developer.espressif.com/blog/2025/10/cores_with_fpu/)).

| Part | Usable RAM | Clock | FPU | Vector unit | Viable |
|---|---|---|---|---|---|
| **ESP32-S3** | 341,760 DIRAM | 240 MHz | single | PIE, integer-only; 128-bit float load/store | **Yes** — the target here |
| **ESP32-P4** | 768 KB L2MEM | 400 MHz | single | PIE, integer-only; no wide float load | **No** — fits trivially, costs the radio; [below](#the-esp32-p4-and-why-it-is-not-a-target) |
| ESP32 (LX6) | ~320 KB | 240 MHz | single | none | Plausible, slower |
| ESP32-S2 | 320 KB | 240 MHz | **none** | none | No — soft-float everything |
| ESP32-C3/C6 | 400/512 KB | 160 MHz | **none** | none | No — same, slower |

Every part above that has an FPU at all has a single-precision one, so `double`
is soft-float across the whole family and `decode_scalar_t` earns its keep on
all of them rather than only here.

### The ESP32-P4, and why it is not a target

Assessed 2026-09-08 and declined. The P4 is dual-core RISC-V at 400 MHz with
768 KB of SRAM, and it holds the 171,558-byte peak heap without the float32
work this port needed — so it reads as the answer if the S3 turns out not to
be real time. Three things were checked before writing any of it, and two of
them settle it.

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

**For this workload the S3 has the better float path of the two.** The S3's
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
network works against all of that. This was a product-shape question rather
than a technical one, and it was put to the project owner and decided on
2026-09-08: the radio is disqualifying on its own, whatever the S3 measures.

**Whether the S3 needs rescuing was the third thing checked, and it turns out
not to bear on this.** It is still unmeasured — [Timing](#timing)
has what that costs, which is one board. The decision does not wait on it. The
radio disqualifies the P4 on its own, so a decode that misses real time on the
S3 gets fixed in the decoder rather than by changing part: the float path is
still scalar, `src/internal/arch/` has no `f32x4` yet, and 46–87 heap
allocations per frame remain PF7's other open gap. Those are the levers, and
they apply to every target at once instead of to one that cannot reach the
network.

The measurement is still worth taking, for the S3's own sake and for
[topology](../family/topology.md)'s Phase 5. It is no longer a question about
the P4.

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
- **Audio output.** The probe decodes built-in fixtures. Nothing reaches I2S.

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

It decodes correctly — all six bed channels exact — and its allocation churn is 80 per frame,
lower than the plain E-AC-3 fixture's 86. What it costs is memory:

| | Bytes |
|---|---|
| `oba::joc::ReconstructionState` | 147,504 |
| `ReconstructionState::QmfState` (`Domain::kQmf` is the default) | 34,360 |
| QMF analysis bank (5 × 5,120) | 25,600 |
| QMF synthesis banks (2 live × 12,800) | 25,600 |
| **JOC state** | 233,064 |
| **Probe peak heap, whole run** | 449,826 (against 179,064 without it) |

The allocator has 280,792 bytes free, so the peak overshoots by 169,034. Not a ceiling to
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

Not built. The pathway is three steps, each of which exists:

1. **ac3forge as an ESP-IDF component.**
   `apps/baremetal/platform/esp32s3/components/ac3forge/` is one, though it sits
   inside the probe app and would need relocating somewhere reusable.
2. **An ESPHome external component**, `components/ac3_decoder/{__init__.py,
   *.cpp}` in a git repo, referenced from YAML via `external_components:`.
3. **Pulling the library in**, with `add_idf_component(name=..., repo=..., ref=...)`
   from that component's `to_code()` — the mechanism ESPHome's own `mqtt` and
   `usb_host` components use for IDF 6.0's registry-hosted dependencies.

ESPHome's `speaker` media_player platform is ESP-IDF-only, so the frameworks are
compatible. The open question is real-time decode, which is unmeasured.
