# ESP32-S3

The minimum-footprint codec profile on an Espressif ESP32-S3: a 240 MHz dual-core Xtensa LX7 with
a single-precision FPU, 512 KB of internal SRAM and hardware I2S. It decodes AC-3 and E-AC-3
— including Atmos objects — and encodes both, in internal SRAM with no PSRAM.

It is the second bare-metal target. The first is [`arm-none-eabi` on QEMU](bare-metal.md), a
Cortex-M3 with no FPU. The S3 has hardware single-precision floating point, which is what makes
the float32 path worth having and real-time decode worth measuring.

## Status

| | |
|---|---|
| AC-3 decode | Correct. Mono, stereo and 5.1, every channel level exact against `apps/baremetal/fixture.hpp` |
| E-AC-3 decode | Correct. 5.1 and 2/0, including AHT, spectral extension and §7.5.4 rematrixing |
| E-AC-3 §E3.5 enhanced coupling | Correct, on its own fixture. Costs 12 allocations per frame, level with plain E-AC-3 |
| Atmos bed | Correct, decoded bed-only via `DecoderConfig::skip_object_reconstruction`. 23 allocations per frame |
| Atmos objects | **Correct, reconstructed on target.** 41 allocations per frame — see [Objects](#objects) |
| Encode | AC-3 and E-AC-3, six frames of synthesised 5.1 through each encoder, byte count and FNV-1a hash checked against `apps/baremetal/encode_fixture.hpp` |
| Fits internal SRAM | Yes, without PSRAM. 236,391-byte peak heap against 280,792 free — see [Memory](#memory) |
| Retained after teardown | 24 bytes, two `__cxa_thread_atexit` records |
| Audio output | Two examples drive real peripherals — see [Examples](#examples) |
| Real time | **Not measured.** No board has been timed; QEMU cannot answer it — see [Timing](#timing) |
| CI | `build-esp32s3` in `.github/workflows/_build.yml`, under QEMU |

Decode and encode are separate builds. They are mutually exclusive, and configure fails if both
are asked for, because neither fits beside the other in this memory.

## Building

ESP-IDF owns the top-level build, as Gradle does for [Android](android.md), so there is no
ac3forge preset for this target and no entry in `cmake/toolchains/`.

### The ESP-IDF component

`esp-idf/ac3forge/` is the profile packaged as a component. A project outside this repository
builds against it in two lines, without vendoring the source list:

```cmake
set(EXTRA_COMPONENT_DIRS "/path/to/ac3forge/esp-idf")
set(AC3FORGE_ESP_PROFILE "decoder")   # or "encoder"
```

The component pre-seeds the root's `option()`s and `add_subdirectory()`s the repo root, the same
shape `apps/android/app/src/main/cpp/CMakeLists.txt` uses. Re-listing
`src/forge/minimal.cmake`'s sources in an `idf_component_register(SRCS ...)` was rejected: two
copies of a source list drift, and the drift surfaces as a link error rather than a diff.

`idf_component.yml` carries registry metadata and names `esp32s3` as its only target. **Nothing
publishes it** — there is no upload step in any workflow, deliberately, since publishing to a
registry is a distribution decision rather than a build one.

### The probes

`apps/baremetal/platform/esp32s3/` is the footprint harness, and points `EXTRA_COMPONENT_DIRS` at
`esp-idf/`:

```bash
. $IDF_PATH/export.sh
cd apps/baremetal/platform/esp32s3
idf.py set-target esp32s3
idf.py build
idf.py qemu                    # no board needed
idf.py -p <PORT> flash monitor # a real board
```

`tools/checks/run_esp32s3_probe.sh` drives that under QEMU and gates on the results;
`--encoder` runs the encode direction instead.

Verified against ESP-IDF v6.1.0, which ships Xtensa GCC 15.2.0 and defaults to `-std=gnu++26`.
The library's C++23 use — `std::expected`, `std::unreachable`, `std::byteswap`, `constexpr
std::vector` — compiles under `-fno-exceptions -fno-rtti`. The libstdc++ problem in
[esp-idf#18172](https://github.com/espressif/esp-idf/issues/18172) is specific to GCC 14.2 and
does not apply.

## Examples

Both live under `esp-idf/ac3forge/examples/` and are built by CI.

### I2S player

`i2s_player` decodes the AC-3 5.1 fixture linked into its own image, folds it to stereo through
the decoder's §7.8 output stage, and writes it to an I2S DAC at 48 kHz, 16-bit, on a loop. It
proves the codec works; it is not how anything real gets its audio.

Three GPIOs under `ac3forge I2S player` in `idf.py menuconfig`, defaulting to BCLK 5, WS 6,
DOUT 7 — chosen to avoid the strapping pins, the USB pair and the console UART. No MCLK is
configured, so a DAC needing one has to have it added. Written against a MAX98357A and a PCM5102.

### Streaming player

`stream_player` decodes AC-3 out of a flash partition without ever holding more than 16 KB of the
stream in memory. Where bytes come from and where audio goes are directories CMake picks, not
flags the player branches on — the player itself names neither a partition nor I2S:

| Source | Sink |
|---|---|
| `partition` — flash (default) | `i2s` — stereo DAC (default) |
| `sd` — SD card over SDMMC | `tdm` — up to eight channels on one data line |
| `http` — an HTTP body over WiFi | `null` — counts frames; what CI runs |

Chosen under *ac3forge stream player* in `idf.py menuconfig`.

It exists to exercise the incremental input path. `ac3::split_frames` takes a span over a whole
stream, which nothing streaming can produce; `ac3::io::AccessUnitAccumulator` applies the same
boundary rule over a caller-owned buffer, allocating nothing. It hands the decoder access units
rather than syncframes, because `decode_access_unit_into` wants an independent substream together
with the dependents that extend it (§E3.8.2).

Only `partition` runs without hardware, so it is the default and the one CI drives end to end.
`sd` and `http` are compiled and no further — QEMU has no SD host and no network. `tdm` has never
run on hardware either; what is tested is `main/interleave.hpp`, on the host
(`tests/io/test_interleave.cpp`), because planar-to-interleaved indexing with slot padding is
where the bugs are. A 5.1 programme on an 8-slot bus leaves two slots that must be written as
zeros rather than skipped: the DMA buffer is reused, so whatever the previous frame left is what
the DAC clocks out.

CI compares the sink's per-channel RMS against the host's answer for the same file through the
same configuration (`ac3cli decode … downmix=loro drcmode=line`). A `result=pass` alone would be
satisfied by a stream decoding to silence.

## Memory

The datasheet says 512 KB, `idf.py size` says 341,760, and the allocator says 280,792. All three
are true and answer different questions.

| | Bytes | |
|---|---|---|
| Physical SRAM | 524,288 | the datasheet's 512 KB |
| − data cache | 32,768 | `CONFIG_ESP32S3_DATA_CACHE_SIZE`, carved out of SRAM2 |
| − instruction cache | 16,384 | already the minimum |
| = DRAM-addressable window | 491,520 | `SOC_DRAM_LOW`…`SOC_DRAM_HIGH` |
| DIRAM pool `idf.py size` reports | 341,760 | after ROM reservations |
| **free at runtime** | **280,792** | what `heap_caps_get_free_size` returns |

`idf.py size`'s "remain" is a linker estimate — 206,956 where the allocator reports 280,792. A
footprint budget quoted from it is a budget nobody checked.

Measured, decode direction: the image uses 134,804 bytes of DIRAM and the decode peaks at
236,391 bytes of heap. The encode image is smaller, 110,900. `app_main` prints the runtime
figures before and after.

### Contiguity

| | Free | Largest block |
|---|---|---|
| Before the decode | 280,792 | 217,088 |
| After it | 245,248 | 116,736 |

The total is not an allocation budget on this part: the heap is regioned, and the largest
contiguous run is what decides whether a large allocation succeeds. That distinction is what made
object reconstruction fail here while the same build passed on `arm-none-eabi`, whose newlib heap
is flat.

### IRAM and the caches

ESP-IDF donates unfilled IRAM to the heap as 32-bit-access-only memory, which `malloc` never
returns. Measured, that pool is 0 bytes — `idf.py size` reports IRAM as 16,384 of 16,384 used, so
there is nothing to donate. The instruction cache is already at its 16 KB minimum; the data cache
could go 32 KB → 16 KB and return 16,384 bytes, at the cost of slower reads from flash-resident
fixtures.

### Stack

The decode runs on the main task. `uxTaskGetStackHighWaterMark` leaves 11,280 bytes free of the
32,768 `sdkconfig.defaults` sets, so the decode uses about 21,500; the encode direction leaves
23,040. The runner holds a floor of 8,192 under it. That margin is the one to watch — it was
14,000 before object reconstruction ran here.

## Timing

The probe reports `decode_us`, `us_per_frame` and `realtime_permille` per codec, and both example
players report per-frame timing of their own.

**No board has been timed.** Under `idf.py qemu` these figures do not describe hardware: QEMU is
not cycle-accurate and reports `cpu_mhz=40` against its own boot log's 160 MHz. Treat the QEMU leg
as a correctness and footprint check only. Under the streaming player's `null` sink the figure
means less again, since nothing paces the loop.

An I2S sink would answer it: the DMA drains at exactly 48,000 frames a second whatever the CPU
does, so back-pressure paces the loop and a decode that cannot keep up is audible. That needs an
ESP32-S3-DevKitC-1-N16R8 and `idf.py -p <PORT> flash monitor`. The instrumentation is in place;
the measurement has not been taken.

## Objects

`joc.cpp` and `oamd.cpp` are in `src/forge/minimal.cmake`'s source list and link into every build
of this profile, so object decode always compiled here. For a long time it did not fit: an
`atmos-encode` fixture (six objects, JOC over a 5.1 downmix, 448 kbit/s) peaked at 449,826 bytes
against 280,792 free, and `ReconstructionState` was a single 147,504-byte allocation — larger
than the 116,736-byte contiguous run a decode leaves free, so it failed on contiguity before any
budget was consulted.

Four changes, each measured on its own:

| | Peak heap | Largest single allocation |
|---|---|---|
| As found (`Domain::kQmf`, `double`, arrays at `kMaxObjects`) | 449,826 | 147,504 |
| `Domain::kMdctBand` | 386,770 | 147,504 |
| + `ReconstructionState` in float32 | 301,522 | 73,776 |
| + per-object scratches sized to the stream | 267,754 | 43,008 |
| + handing back the enhanced-coupling scratch | **233,546** | 43,008 |

The largest allocation is now the E-AC-3 decoder's own AHT buffer rather than anything JOC owns.

That last row is the one that is easy to miss. 267,754 against 280,792 free looks like 13,038
spare, but the order of fixtures decided the result: objects run after an enhanced-coupling decode
failed with `out_of_memory bytes=6144`, while objects on a clean heap passed. The difference is
`eac3_tools.cpp`'s 32,768-byte spectrum scratch and 1,440-byte bin-angle vector, both
`thread_local` so §E3.5 neither allocates per call nor puts 32 KB on the stack. On a hosted
platform they are released at thread exit; here the only thread never exits.
`ac3::eac3::release_ecpl_scratch()` hands them back, and the probe calls it between fixtures.
Retained at exit went from 34,232 bytes to 24.

**The bed does not need any of this.** An Atmos bed is ordinary E-AC-3 5.1 and the objects are
side data, so `DecoderConfig::skip_object_reconstruction` decodes the bed without allocating
`ReconstructionState` at all — 23 allocations per frame against 41. `tests/oba/test_atmos.cpp`
asserts the rendered channels are bit-for-bit what a full decode produces. `object_metadata`
still arrives, parsed out of a block's skip field.

## Configuration

`apps/baremetal/platform/esp32s3/sdkconfig.defaults` carries the settings and their reasoning. Two
are repeated here because neither failure mode points at its cause:

- **`CONFIG_ESP_MAIN_TASK_STACK_SIZE=32768`.** IDF's default is 3,584 bytes, which suits an
  application that configures peripherals and waits on queues and is far too small for a codec.
  The overflow does not report as a stack overflow: it surfaces as a `LoadProhibited` panic on the
  other core's idle task, because it corrupts a neighbouring structure. An integrator sizing a
  real decoding task should measure with `uxTaskGetStackHighWaterMark()` rather than copy this.
- **`CONFIG_ESP_TASK_WDT_INIT=n`.** The probe is a batch computation that runs the CPU flat out
  without yielding, which is what the watchdog exists to catch. A decoder in a product should keep
  the watchdog and give the decode its own task with a bounded per-frame budget.

PSRAM is off, although the development board has 8 MB. QEMU cannot emulate S3 PSRAM
([espressif/qemu#129](https://github.com/espressif/qemu/issues/129)), so a build requiring it
cannot run in CI, and keeping it off means the internal-SRAM budget is enforced rather than
avoided. Turn it on for a board build that needs the headroom; not to make a footprint number go
away.

## What the port required from the library

**A `thread_local` that made the library unlinkable on any RTOS.** `eac3_tools.cpp`'s
enhanced-coupling scratch was a 32 KB `thread_local`. FreeRTOS carves each task's thread-local
area out of that task's own stack and sizes it from the linked image's `.tdata + .tbss` — the same
size for every task, including tasks that never call the decoder. ESP-IDF's IPC task has a 1 KB
stack, so it could not be created and the application failed an assert inside `esp_ipc_init()`
before `app_main`. Keeping only a `unique_ptr` in TLS took every task's area from 32 KB to one
pointer, and `.tbss` from 32,784 bytes to 24.

**float32 for the decode path.** The LX7's FPU is single-precision, so `double` coefficients are
wider than anything downstream can use, and memory was the binding constraint: the per-block
`coeffs` store is 100,352 bytes and `aht_coeffs_` 86,016.
`src/forge/src/internal/scalar/{float32,float64}/` carries `decode_scalar_t` — `float` under the
minimum-footprint profile, `double` by default elsewhere, and selectable in any build with
`-DAC3FORGE_DECODE_SCALAR=float`. Which profile a build is and which scalar its decoder carries
are independent CMake axes.

## Open work

- **Real-time measurement on hardware.** The single largest gap. See [Timing](#timing).
- **Heap traffic in the decode loop.** PF7 asks for zero; the steady state is 1–41 allocations per
  frame depending on fixture, from per-block geometry vectors and the `std::vector` members of the
  returned `DecodedFrame`. Reaching zero means those becoming fixed-capacity, which changes public
  types. The runner gates at 100 so the distance from zero cannot grow quietly.
- **A vectorised float32 path.** `src/forge/src/internal/arch/` carries an `f32x4`, but it
  resolves to `generic/` here and compiles to four scalar operations: PIE's vector ALU is
  integer-only. What `esp-dsp` uses instead is `EE.LDF.128.IP`, a 128-bit load filling four FPU
  registers feeding four scalar `madd.s` — load bandwidth and instruction-level parallelism rather
  than a four-wide multiply. That is not reachable from the arch seam, measured rather than
  assumed: `EE.LDF.128.IP` writes a consecutive quad of `f` registers, which GCC's Xtensa port
  cannot model as one value, so it spills every asm block's outputs (68 instructions scalar
  against 73 with 22 spills). Capturing it needs a hand-written assembly kernel tier, like
  `src/forge/src/internal/avx2/`, which would also need `madd.s` — a fused multiply-add of exactly
  the kind `-ffp-contract=off` forbids project-wide — and so its own bit-exactness argument.
- **AC-3's `decoder.cpp` is still `double`.** E-AC-3 was converted; AC-3 works but keeps both
  transform instantiations compiled.

## Other ESP32 variants

Whether the part has an FPU decides this; RAM does not. Espressif measure a cosine at ~2,377
cycles on an ESP32-C3 against 121 on an ESP32-S3
([Floating-Point Units on Espressif SoCs](https://developer.espressif.com/blog/2025/10/cores_with_fpu/)).

| Part | Usable RAM | Clock | FPU | Vector unit | Viable |
|---|---|---|---|---|---|
| **ESP32-S3** | 341,760 DIRAM | 240 MHz | single | PIE, integer-only; 128-bit float load/store | **Yes** — the target here |
| **ESP32-P4** | 768 KB L2MEM | 400 MHz | single | PIE, integer-only; no wide float load | **No** — see below |
| ESP32 (LX6) | ~320 KB | 240 MHz | single | none | Plausible, slower |
| ESP32-S2 | 320 KB | 240 MHz | **none** | none | No — soft-float everything |
| ESP32-C3/C6 | 400/512 KB | 160 MHz | **none** | none | No — same, slower |

Every part with an FPU has a single-precision one, so `double` is soft-float across the family and
`decode_scalar_t` earns its keep on all of them.

### Why not the ESP32-P4

Assessed and declined on 2026-09-08. It is dual-core RISC-V at 400 MHz with 768 KB of SRAM, and
holds the peak heap without the float32 work — so it reads as the answer if the S3 misses real
time. Three things were checked and two settle it.

**Its vector extension has no floating point.** The P4 is `RV32IMAFC` plus `Xhwlp` and `Xesppie`,
vendor extensions no other implementation carries — not the ratified RISC-V Vector extension.
Across the 360 instructions in ESP-IDF's own decoder test for it, the only data types are `s8`,
`s16`, `s32`, `u8`, `u16`, `u32`. No `f32` anywhere. Espressif's own code agrees: in `esp-dsp`
every `_arp4` file using a PIE instruction sits under `fixed/`, and the float32 kernels contain
exactly one `esp.` instruction each — `esp.lp.setup`, the hardware loop — with scalar `fmadd.s`
arithmetic. So an `f32x4` has nothing to compile to there either.

**It has no radio, and the plan it would serve is a Wi-Fi plan.** No Wi-Fi and no Bluetooth; it
needs a companion ESP32-C6 or -H2, making any networked build a two-chip design. That was a
product-shape question and it is disqualifying on its own, whatever the S3 measures.

What the P4 would buy is clock — 12.8 M cycles per frame against the S3's 7.68 M, **1.67×**,
per-core in both cases. The memory advantage is already spent, since this port fits internal SRAM.

## ESPHome

Not built. The first of the three steps it needs is done: [the component](#the-esp-idf-component)
is reusable and reachable through `EXTRA_COMPONENT_DIRS`. Two remain:

1. **An ESPHome external component**, `components/ac3_decoder/{__init__.py, *.cpp}` in a git repo,
   referenced from YAML via `external_components:`.
2. **Pulling the library in**, with `add_idf_component(name=..., repo=..., ref=...)` from that
   component's `to_code()` — the mechanism ESPHome's own `mqtt` and `usb_host` components use.
   That wants a registry-hosted dependency and nothing publishes this component, so an ESPHome
   build would reach it by git reference instead.

ESPHome's `speaker` media_player platform is ESP-IDF-only, so the frameworks are compatible.
