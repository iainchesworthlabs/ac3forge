# ESP32-S3

The minimum-footprint decoder profile on an Espressif ESP32-S3: a
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
| Atmos, bed | Correct. Its own fixture, decoded bed-only; costs 61 allocations/frame and nothing extra in peak heap |
| Atmos, objects | Does not fit. Decodes correctly on a host; see [Objects](#objects-and-what-it-took-to-fit-them) |
| Fits internal SRAM | Yes, without PSRAM: 280,792 bytes free against a 179,064-byte peak — see [Memory](#how-much-memory-there-actually-is) |
| Retained after teardown | 34,232 bytes of `thread_local` enhanced-coupling scratch, held for the life of the decoding task — see [Building](../building.md#gaps) |
| Real time | Not measured. See [Timing](#timing) |
| CI | `build-esp32s3` in `.github/workflows/_build.yml`, under QEMU |

## Building

ESP-IDF owns the top-level build, as Gradle does for [Android](android.md), so
there is no ac3forge preset for this target and no entry in `cmake/toolchains/`.
The project reaches into this repo from the other direction:
`apps/baremetal/platform/esp32s3/CMakeLists.txt` points `EXTRA_COMPONENT_DIRS`
at `esp-idf/`, and the component there pre-seeds the root's `option()`s and
`add_subdirectory()`s the repo root, the same shape
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

`src/forge/src/internal/scalar/{float32,float64}/`'s seam carries
`decode_scalar_t` — `float` under the minimum-footprint profile, `double` by
default elsewhere, and selectable in any build with
`-DAC3FORGE_DECODE_SCALAR=float`. Four decoder buffers follow it. Which profile
a build is and which scalar its decoder carries are two independent CMake axes.
See [Building](../building.md#minimum-footprint-decoder-profile) for what the
profile changes, and its Gaps section for the measured accuracy cost.

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
768 KB of SRAM, and it holds the 179,064-byte peak heap without the float32
work this port needed, so it reads as the answer if the S3 turns out not to
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
is worth having — from hand-written assembly rather than from the arch seam,
which [Not done](#not-done) measures. The P4's equivalent has no wide float
load; it loads one float at a time. Whatever the S3's float path is eventually
worth, the P4 does not inherit it.

What the P4 does buy over the S3 is clock and memory. A frame is 1536 samples,
32 ms at 48 kHz, which is 7.68 M cycles of budget at 240 MHz against 12.8 M at
400 MHz: **1.67×**, and it is per-core in both cases. The memory advantage is
already spent: this port fits internal SRAM on the S3 with 280,792 bytes free
against a 179,064-byte peak.

**It has no radio, and the plan it would serve is a Wi-Fi plan.** The P4 has
neither Wi-Fi nor Bluetooth and needs a companion ESP32-C6 or -H2 for either,
making any networked build a two-chip design.
The [source/transport/sink plan](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/topology.md), which is
kept in the repository rather than published here, puts a network transport in
front of the decoder, and its Phase 5 exit is *"an ESP32-S3 decoding E-AC-3
from a network origin in real time"*; the bandwidth argument for carrying a
compressed stream at all is stated there as the difference between an ESP32-S3
receiving Atmos over Wi-Fi and one receiving no surround. ESPHome nodes are
Wi-Fi devices. A part that has to be paired with a second chip to reach the
network works against all of that. This was a product-shape question rather
than a technical one, and it was decided on
2026-09-08: the radio is disqualifying on its own, whatever the S3 measures.

**Whether the S3 needs rescuing was the third thing checked, and it turns out
not to bear on this.** It is still unmeasured — [Timing](#timing)
has what that costs, which is one board. The decision does not wait on it. The
radio disqualifies the P4 on its own, so a decode that misses real time on the
S3 gets fixed in the decoder rather than by changing part: 46–87 heap
allocations per frame remain PF7's other open gap, and the float path has a
hand-written-kernel option this page now sizes. `src/forge/src/internal/arch/` does
carry an `f32x4` since PF7's SIMD step, but it resolves to `generic/` here and
buys this part nothing. Those are the levers, and they apply to every target
at once instead of to one that cannot reach the network.

The measurement is still worth taking, for the S3's own sake and for
[the topology plan](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/topology.md)'s Phase 5. It is no longer a question about
the P4.

## Not done

- **A vectorised float32 path on this part.** `src/forge/src/internal/arch/` carries an
  `f32x4` since PF7's SIMD step, so the float32 IMDCT's twiddle stages go four
  lanes at a time on SSE2 and NEON. On an S3 that type resolves to `generic/`
  and compiles to four scalar operations, because PIE's vector ALU is
  integer-only — the name oversells it, which is worth being precise about.

  What `esp-dsp`'s float32 kernels use instead is `EE.LDF.128.IP`, a 128-bit
  load filling four FPU registers, feeding four independent scalar `madd.s`
  into four accumulators: load bandwidth and instruction-level parallelism
  rather than a four-wide float multiply.

  **That is not reachable from the arch seam**, which was measured rather than
  assumed. An `f32x4` whose `load`/`store` are `EE.LDF.128.IP`/`EE.STF.128.IP`
  through inline asm, compiled over `imdct256_pair_windowed`'s post-twiddle at
  `-O2` under ESP-IDF v6.1's Xtensa GCC 15.2.0:

  | Form | Instructions | Spills |
  |---|---|---|
  | Plain scalar, what `generic/` emits today | **68** | — |
  | PIE loads, `asm volatile` | 73 | 22 `ssi` |
  | PIE loads, non-volatile with memory operands | 99 | 33 `ssi` + 22 `lsi` |

  The six wide accesses do replace twenty-four narrow ones and are then
  swamped. `EE.LDF.128.IP` writes a *consecutive quad* of `f` registers, and
  GCC's Xtensa port has no way to model that as a single value, so it spills
  every asm block's outputs and loses the hardware `loop` along with them.
  `esp-dsp` does not meet this because its kernels are assembly end to end and
  allocate their own registers.

  So the shape that could capture it is a hand-written Xtensa kernel tier,
  like `src/forge/src/internal/avx2/` rather than like `src/forge/src/internal/arch/`: whole
  twiddle stages in assembly, selected at build time. Reaching `esp-dsp`'s
  figures also means `madd.s`, a deliberate fused multiply-add of exactly the
  kind `-ffp-contract=off` forbids project-wide, so that tier would have to
  carry its own bit-exactness argument rather than inherit the seam's. It
  still wants a measurement from real silicon before any of it.
- **AC-3's `decoder.cpp` is still `double`.** E-AC-3 was converted; AC-3 works
  but keeps both transform instantiations compiled.
- **Audio output from the probe.** The footprint probe decodes built-in fixtures and
  reports levels; it drives no peripheral. Sound out of this part goes through the
  separate I2S example described in [Audio output](#audio-output) below.

Adding fixtures does not move the internal-SRAM figure. The enhanced-coupling and 2/0 streams
added 13,824 bytes and DIRAM stayed at 134,676: `fixture.hpp` is `constexpr` data, and on this
part it lands in **Flash Data** (112,524 bytes) rather than DIRAM. The `arm-none-eabi` image
ceiling is the one fixture size spends against; here it costs flash, of which the app partition
has 66% free.

## Objects, and what it took to fit them

`src/forge/src/oba/joc.cpp` and `oamd.cpp` are both in `src/forge/minimal.cmake`'s source list and
link into every build of this profile, so object decode always compiled here. It did not fit. An
`atmos-encode` fixture (six objects, JOC over a 5.1 downmix, 448 kbit/s) decoded correctly on the
`arm-none-eabi` leg and peaked at **449,826 bytes of heap** against 280,792 free — and worse,
`oba::joc::ReconstructionState` was a single 147,504-byte allocation, larger than the 116,736-byte
contiguous block a decode leaves free, so it failed on contiguity before any budget was consulted.

Three changes, each measured on its own rather than stacked in arithmetic:

| | Peak heap | Largest single allocation |
|---|---|---|
| As found (`Domain::kQmf`, `double`, arrays at `kMaxObjects`) | 449,826 | 147,504 |
| `Domain::kMdctBand` | 386,770 | 147,504 |
| + `ReconstructionState` in float32 | 301,522 | 73,776 |
| + per-object scratches sized to the stream | **267,754** | 43,008 |

The largest allocation is now the E-AC-3 decoder's own AHT buffer rather than anything JOC owns,
which removes the contiguity blocker: 43,008 fits the 116,736-byte free run easily, where 147,504
never could.

### It fits, and what it took to stop the order mattering

267,754 against 280,792 bytes of free internal SRAM looks like 13,038 spare. On this part it was
not, and the reason is worth keeping because a total-free figure is not an allocation budget here.

Measured on the ESP32-S3 itself, same build, same fixture, only the order changed:

| | Peak heap | Result |
|---|---|---|
| Objects after the enhanced-coupling fixture | 267,754 | **failed** — `out_of_memory bytes=6144` |
| Objects first, on a clean heap | 233,522 | passed |

The 34,232 bytes between them are `eac3_tools.cpp`'s enhanced-coupling scratch — a 32,768-byte
spectrum buffer and a 1,440-byte bin-angle vector, both `thread_local` so that §E3.5 neither
allocates per call nor puts 32 KB on the stack. On a hosted platform they go at thread exit. Here
the only thread never exits, so they stayed resident and object reconstruction had nowhere to go.

`ac3::eac3::release_ecpl_scratch()` hands them back, and the next call rebuilds what it needs. The
probe calls it between fixtures, so the rows sit in the order they belong rather than the order
that happens to pass:

| | Peak heap | Retained at exit |
|---|---|---|
| Before | 267,754 | 34,232 |
| After | **233,546** | **24** |

24 bytes is two `__cxa_thread_atexit` registration records. Both legs report the same figures.

That leaves **47,246 bytes spare** against free SRAM rather than 13,038, and it is why object
decode is gated in CI on both bare-metal legs instead of documented as almost fitting.

The `arm-none-eabi` leg could not have found this. Its newlib heap is flat, so 267,754 of 280,792
packs there and the same build passed. This part's heap is regioned — 280,792 free against a
largest block of 217,088 — and that is the number that decides.

### The bed plays, though

None of the above stops an Atmos stream being played on this part. Its bed is ordinary E-AC-3 5.1
and the objects are side data; only reconstructing them is expensive.
`DecoderConfig::skip_object_reconstruction` decodes the bed and never allocates
`ReconstructionState` at all, and `apps/baremetal/fixture.hpp` carries an Atmos fixture decoded
that way:

| | Full decode | Bed only |
|---|---|---|
| Peak heap | 449,826 | **179,064 — unchanged from a plain decode** |
| Allocations/frame | 80 | 61 |
| Bed channels correct | yes | yes, identically |

The flag costs the bed nothing: `tests/oba/test_atmos.cpp` asserts the rendered channels are
bit-for-bit what a full decode produces, since skipping reconstruction touches no coefficient the
bed is built from. `object_metadata` still arrives — it is parsed out of a block's skip field and
costs nothing to keep, and a renderer picking a speaker layout still wants what the stream
declared.

Without the flag an Atmos stream does not degrade on this part, it fails: the allocation is
attempted, and the decode stops partway through in `operator new`.

## The ESP-IDF component

`esp-idf/ac3forge/` is the profile packaged as a component, so a project outside
this repository can build against it without vendoring the source list:

```cmake
set(EXTRA_COMPONENT_DIRS "/path/to/ac3forge/esp-idf")
set(AC3FORGE_ESP_PROFILE "decoder")   # or "encoder"
```

The two profiles are mutually exclusive — `AC3FORGE_MINIMAL_DECODER` and
`AC3FORGE_MINIMAL_ENCODER` fail configure together, because neither fits beside
the other in internal SRAM. Build one, tear it down, rebuild for the other if a
target needs both in sequence.

`idf_component.yml` carries the registry metadata and names `esp32s3` as the
only target, which is a measurement rather than a shrug at the rest: the S3 is
the part this was ported to and the one CI exercises. **Nothing publishes the
component** — there is no upload step in any workflow, deliberately, since
publishing to a registry is a distribution decision rather than a build one.

## Audio output

`esp-idf/ac3forge/examples/i2s_player/` decodes the AC-3 5.1 fixture, folds it
to stereo through the decoder's own §7.8 output stage, and writes it to an I2S
DAC at 48 kHz, 16-bit, on a loop. Three GPIOs, set under `ac3forge I2S player`
in `idf.py menuconfig`, defaulting to BCLK 5, WS 6, DOUT 7 — chosen to avoid the
strapping pins, the USB pair and the console UART. The example's README names
the DAC shapes it is written for (a MAX98357A, a PCM5102), and no MCLK pin is
configured, so a DAC that needs one has to have it added.

It is a smaller build than the footprint probe — 94,383 bytes of DIRAM against
134,676 — because it reaches only the AC-3 path: no Annex E decoder, no QMF
bank, no object reconstruction. An E-AC-3 or Atmos player is a bigger build.

Unlike the probe under QEMU, the I2S peripheral is a real clock: the DMA drains
at 48,000 frames a second whatever the CPU does, so the example's
`realtime_permille` and `worst_frame_us` are the timing figures this port has
otherwise had no way to take. What the [Timing](#timing) section says about QEMU
still holds for the probe.

## ESPHome

Not built. The first of the three steps it needs is now done:
[the ESP-IDF component](#the-esp-idf-component) above is reusable and reachable
through `EXTRA_COMPONENT_DIRS`. Two remain:

1. **An ESPHome external component**, `components/ac3_decoder/{__init__.py,
   *.cpp}` in a git repo, referenced from YAML via `external_components:`.
2. **Pulling the library in**, with `add_idf_component(name=..., repo=..., ref=...)`
   from that component's `to_code()` — the mechanism ESPHome's own `mqtt` and
   `usb_host` components use for IDF 6.0's registry-hosted dependencies. That
   mechanism wants a registry-hosted dependency, and nothing publishes this
   component, so an ESPHome build would reach it by git reference instead.

ESPHome's `speaker` media_player platform is ESP-IDF-only, so the frameworks are
compatible.
