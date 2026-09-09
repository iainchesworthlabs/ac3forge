# Bare metal

A board with no operating system, no C++ runtime to speak of, and a few hundred kilobytes of RAM.
The library builds for it as `ac3::forge_minimal`: one static archive, no exceptions, no RTTI,
and none of the direct-form transform tables. It is decode-only under
`AC3FORGE_MINIMAL_DECODER` and encode-only under `AC3FORGE_MINIMAL_ENCODER`; the two are
mutually exclusive, because neither fits beside the other in the memory this profile targets.

The reference target is `arm-none-eabi` cross-compiled for QEMU's `mps2-an385` machine — a
Cortex-M3 with no floating-point unit, where every floating-point operation is software-emulated.
It is the target CI measures the profile on. The
[ESP32-S3](esp32.md) is the second bare-metal target and the first with hardware floating point.

## Status

| | |
|---|---|
| AC-3 5.1 decode | Correct. Six frames, every channel level exact against `apps/baremetal/fixture.hpp` |
| E-AC-3 5.1 decode | Correct. Same, including AHT and spectral extension |
| E-AC-3 §E3.5 enhanced coupling | Correct. Its own fixture; 12 allocations per frame, level with plain E-AC-3 |
| E-AC-3 2/0, §7.5.4 rematrixing | Correct. The only layout that tool exists in |
| Image size | 320,521 bytes total — 224,076 `.text`, 400 `.data`, 96,045 `.bss` |
| Peak heap | 234,803 bytes, the Atmos fixture decoded with its objects |
| Retained after teardown | 12 bytes once the probe hands back the enhanced-coupling scratch (23,552 bytes while §E3.5 is in use) |
| Encode | A separate encode-only profile, `AC3FORGE_MINIMAL_ENCODER`. The two are mutually exclusive: neither fits beside the other. |
| Audio output | None. The probe decodes built-in fixtures and prints levels. |
| Real silicon | None. Correctness is established under emulation. |
| CI | `build-footprint` in `.github/workflows/_build.yml`, on every push |

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

[Building from source](../building.md#minimum-footprint-decoder-profile) covers what
`AC3FORGE_MINIMAL_DECODER=ON` changes, what the probe checks and what it does not, and the gaps
the profile has not closed. The measured figures and how they moved are in
[the footprint table](../performance-trend.md#minimum-footprint-decoder).

## What you give up

The profile replaces what `src/forge` builds rather than adding to it, and configure fails with a
list if any component needing the full library is still switched on.

| Not compiled | Consequence |
|---|---|
| Under `AC3FORGE_MINIMAL_DECODER`: the encoder, container writers, WAV I/O, analysis and QC layers, the object encoder | Decode only. `src/forge/minimal.cmake` lists what is compiled, with a line on why each file is reachable from a decode. `AC3FORGE_MINIMAL_ENCODER` is the same profile pointed the other way. |
| The direct-form transform tables | 1,900,544 bytes of `.bss` that are absent from the image rather than merely unused. `DecoderConfig::fast_imdct = false` returns `DecodeError::kUnsupported` here instead of being served quietly by the fast path. |

Object reconstruction compiles, links and decodes correctly, and it fits: the peak heap above is
the Atmos fixture decoded with its objects, and
[the ESP32-S3 page](esp32.md#objects-and-what-it-took-to-fit-them) has what it took. An Atmos
stream's 5.1 bed alone decodes via `DecoderConfig::skip_object_reconstruction`.

## Porting to another part

Two things are target-specific, and both live under `apps/baremetal/platform/`: a thread-pointer
stub (`baremetal/tls.cpp`, whose static block the linker script bounds with two `ASSERT()`s) and
the linker script and startup for the board. Nothing in `src/` branches on the target.

Whether a part is viable comes down to floating point. Every operation the decoder does in
`double` is software-emulated without an FPU, and `decode_scalar_t` is `float` under this profile
precisely because that is what the parts with hardware floating point actually have. The
[ESP32-S3 page](esp32.md#other-esp32-variants) works that argument through one vendor's range.
