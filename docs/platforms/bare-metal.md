# Bare metal

A board with no operating system, no C++ runtime to speak of, and a few hundred kilobytes of RAM.
The library builds for it as `ac3::forge_minimal`: one static archive, no exceptions, no RTTI, and
none of the direct-form transform tables.

The reference target is `arm-none-eabi` cross-compiled for QEMU's `mps2-an385` machine — a
Cortex-M3 with no floating-point unit, where every floating-point operation is software-emulated.
It is the target CI measures the profile on. The [ESP32-S3](esp32.md) is the second bare-metal
target and the first with hardware floating point.

## Status

| | |
|---|---|
| AC-3 decode | Correct. Mono, stereo and 5.1, every channel level exact against `apps/baremetal/fixture.hpp` |
| E-AC-3 decode | Correct. 5.1, 2/0 and 7.1.4 (a bed and two dependent substreams), including AHT, spectral extension and §7.5.4 rematrixing |
| E-AC-3 §E3.5 enhanced coupling | Correct, on its own fixture |
| Atmos bed and objects | Correct. Objects reconstruct here; the flat newlib heap makes it easier than on the [ESP32-S3](esp32.md#objects) |
| Encode | A separate encode-only profile, `AC3FORGE_MINIMAL_ENCODER` |
| Image size | 303,145 bytes — 255,916 `.text`, 400 `.data`, 46,829 `.bss` |
| Peak heap | 229,630 bytes, the 7.1.4 fixture (210,203 with Atmos objects) |
| Retained after teardown | 12 bytes, one `__cxa_thread_atexit` record; the enhanced-coupling scratch (23,552 bytes while §E3.5 is in use) is handed back between fixtures |
| Allocations per frame | 1 to 35, by fixture — see [the footprint table](../performance-trend.md#minimum-footprint-decoder) |
| Audio output | None. The probe decodes built-in fixtures and prints levels |
| Real silicon | None. Correctness is established under emulation |
| CI | `build-footprint` in `.github/workflows/_build.yml`, on every push |

Decode and encode are separate builds, and mutually exclusive: configure fails if both are asked
for, because neither fits beside the other in the memory this profile targets.

## Building and running it

```bash
# Cross-compile for arm-none-eabi and run the probe on QEMU
tools/checks/run_baremetal_probe.sh

# The same profile natively, no emulator
tools/checks/run_baremetal_probe.sh --host

# Instructions per frame under QEMU -icount, deterministic and gated
tools/checks/run_baremetal_probe.sh --icount
```

`--icount` is the one timing figure this leg can give. QEMU is not cycle-accurate and the probe's
ordinary clock is semihosting's, which reports the host's time; but under `-icount shift=0` the
guest's own clock advances one nanosecond per executed instruction, and a build whose clock reads
the mps2-an385's 25 MHz timer (`AC3FORGE_BAREMETAL_CLOCK=timer`, its own preset and build
directory) follows it. Every microsecond the probe then prints is a thousand Thumb-2 instructions,
identical on every host and every run — `eac3.instructions_per_frame=12948000` — gated per fixture
with the same headroom rule as the other ceilings, and with `--stage-timers` counted per stage. It
is not cycles on any real part; it is a number that moves when the code does, which the host-time
figure never was, and it is what the [ESP32-C3 estimate](esp32.md#other-esp32-variants) rests on.

Both drive the presets, which you can also use directly: `config-arm-none-eabi-minimal` /
`build-arm-none-eabi-minimal`, and `config-linux-gcc-minimal` or `config-linux-llvm-minimal` for
the host. GCC and Clang only.

[Building from source](../building.md#minimum-footprint-decoder-profile) covers what the profile
changes and the gaps it has not closed. The measured figures are in
[the footprint table](../performance-trend.md#minimum-footprint-decoder).

## What you give up

The profile replaces what `src/forge` builds rather than adding to it, and configure fails with a
list if any component needing the full library is still switched on.

| Not compiled | Consequence |
|---|---|
| Under `AC3FORGE_MINIMAL_DECODER`: the encoder, container writers, WAV I/O, analysis and QC layers, the object encoder | Decode only. `src/forge/minimal.cmake` lists what is compiled, with a line on why each file is reachable from a decode. `AC3FORGE_MINIMAL_ENCODER` is the same profile pointed the other way. |
| The direct-form transform tables | 1,900,544 bytes of `.bss` absent from the image rather than merely unused. `DecoderConfig::fast_imdct = false` returns `DecodeError::kUnsupported` here instead of being served quietly by the fast path. |

## The probe

`apps/baremetal/probe.cpp` links the archive, decodes six frames each of nine fixtures, compares
every channel's level against `apps/baremetal/fixture.hpp`, and prints `key=value` lines the
runner gates on: the levels, image size, peak heap, retained bytes and allocations per frame.
`encode_probe.cpp` is its counterpart, checking a byte count and FNV-1a hash against
`encode_fixture.hpp`.

It is not a unit test — the profile requires `AC3FORGE_BUILD_TESTS=OFF`, since nothing under
`tests/` builds against this archive — and it answers three questions a test could not: does the
archive link with everything else absent, does it produce the right audio on a 32-bit soft-float
target, and what did it cost.

Nothing regenerates the fixtures automatically and nothing detects that they have drifted from the
encoder: the probe decodes a committed bitstream and compares it against committed levels, so both
moving together is invisible to it. Regenerate with
`python tools/generators/gen_baremetal_fixture.py --ac3cli <path>`.

## Porting to another part

Two things are target-specific, both under `apps/baremetal/platform/`: a thread-pointer stub
(`baremetal/tls.cpp`, whose static block the linker script bounds with two `ASSERT()`s) and the
linker script and startup for the board. Nothing in `src/` branches on the target.

Whether a part is viable comes down to floating point. Every operation the decoder does in
`double` is software-emulated without an FPU, and `decode_scalar_t` is `float` under this profile
precisely because that is what the parts with hardware floating point actually have. The
[ESP32-S3 page](esp32.md#other-esp32-variants) works that argument through one vendor's range.
