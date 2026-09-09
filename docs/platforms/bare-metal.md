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
| E-AC-3 decode | Correct. 5.1 and 2/0, including AHT, spectral extension and §7.5.4 rematrixing |
| E-AC-3 §E3.5 enhanced coupling | Correct, on its own fixture |
| Atmos bed and objects | Correct. Objects reconstruct here; the flat newlib heap makes it easier than on the [ESP32-S3](esp32.md#objects) |
| Encode | A separate encode-only profile, `AC3FORGE_MINIMAL_ENCODER` |
| Image size | 318,001 bytes — 221,556 `.text`, 400 `.data`, 96,045 `.bss` |
| Peak heap | 233,195 bytes, the Atmos fixture decoded with its objects |
| Retained after teardown | 12 bytes, one `__cxa_thread_atexit` record; the enhanced-coupling scratch (23,552 bytes while §E3.5 is in use) is handed back between fixtures |
| Allocations per frame | 1 to 41, by fixture — see [the footprint table](../performance-trend.md#minimum-footprint-decoder) |
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
```

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

`apps/baremetal/probe.cpp` links the archive, decodes six frames each of eight fixtures, compares
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
