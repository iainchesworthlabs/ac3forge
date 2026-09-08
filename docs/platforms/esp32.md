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
| Fits internal SRAM | Yes, without PSRAM: 202,860 bytes free against a 171,558-byte peak |
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

| Part | Usable RAM | Clock | FPU | Suitable |
|---|---|---|---|---|
| ESP32-S3 | 341,760 DIRAM | 240 MHz | single + PIE SIMD | Yes — the target here |
| ESP32-P4 | 768 KB L2MEM | 400 MHz | single + SIMD | Yes, but no integrated Wi-Fi |
| ESP32 (LX6) | ~320 KB | 240 MHz | single, no SIMD | Workable, slower |
| ESP32-S2 | 320 KB | 240 MHz | none | No |
| ESP32-C3, C6 | 400/512 KB | 160 MHz | none | No |

The parts without an FPU have enough RAM for the working set; software floating
point is what rules them out. The P4 would fit the working set without float32 at
all, but its FPU is also single-precision, so `double` remains software-emulated
there.

## Not done

- **PIE SIMD.** `src/internal/arch/` has no `f32x4`, so the float path runs
  scalar. The ESP32-S3's 128-bit extensions and `esp-dsp`'s published float32
  kernel costs suggest this is worth doing; confirm against a measurement from
  hardware before starting, since that is the claim it rests on.
- **AC-3's `decoder.cpp` is still `double`.** E-AC-3 was converted; AC-3 works
  but keeps both transform instantiations compiled.
- **Audio output.** The probe decodes a built-in fixture. Nothing reaches I2S.

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
