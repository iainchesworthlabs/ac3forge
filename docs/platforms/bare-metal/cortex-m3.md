# Bare metal

A board with no operating system, no C++ runtime to speak of, and a few hundred kilobytes of RAM.
The library builds for it as `iclforge::ac3_minimal`: one static archive, no exceptions, no RTTI, and
none of the direct-form transform tables.

The reference target is `arm-none-eabi` cross-compiled for QEMU's `mps2-an385` machine — a
Cortex-M3 with no floating-point unit, where every floating-point operation is software-emulated.
It is the target CI measures the profile on. The [ESP32-S3](esp32-s3.md) is the second bare-metal
target and the first with hardware floating point.

## Status

| | |
|---|---|
| AC-3 decode | Correct. Mono, stereo and 5.1, and 5.1 folded to Lo/Ro stereo in line mode by the §7.8 output stage, every channel level exact against `apps/baremetal/fixture.hpp` |
| E-AC-3 decode | Correct. 5.1, 2/0 and 7.1.4 (a bed and two dependent substreams), including AHT, spectral extension and §7.5.4 rematrixing; 5.1 and 7.1.4 folded to Lo/Ro stereo in line mode; and 5.1 in line mode from a stream carrying dynrng words and dialnorm 24 |
| E-AC-3 §E3.5 enhanced coupling | Correct, on its own fixture |
| Atmos bed and objects | Correct. Objects reconstruct here, and are placed onto 7.1.4 by their positions (`eac3_atmos_render`, through the block form's object views); the flat newlib heap makes it easier than on the [ESP32-S3](esp32-s3.md#objects) |
| Fixed-point decode | `-DICLFORGE_DECODE_SCALAR=fixed` builds every decode row above in Q7.24 integers under a per-block exponent, for a part with no FPU at all - the plan is [arithmetic-tiers.md](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/arithmetic-tiers.md). CI runs the probe twice on this leg, and the fixed build's PCM is identical to the x86 host's and to an ESP32-C3's under `qemu-riscv32` - three architectures, one pinned set of hashes (`tests/golden/fixed-probe-pcm-hashes.json`). It costs 0.33x the instructions the default build spends on the same frame: `eac3.instructions_per_frame=4241000` against 12,942,000, integer arithmetic where that one's is software floating point |
| Encode | A separate encode-only profile, `ICLFORGE_MINIMAL_ENCODER`: six rows (5.1 and 2/0 through each encoder, 2/0 with coupling, spectral extension and AHT, 2/0 §E3.5), each hashed against `encode_fixture.hpp` with its peak and its time per frame; 226,780-byte image, 158,911 peak, 9.1 M to 48.2 M instructions a frame under `--encoder --icount` - see [Building](../../building.md#what-the-encode-direction-costs) |
| AC-4 decode | A third probe, `run_baremetal_probe.sh --ac4`, for the AC-4 decoder in `float` (`iclforge::ac4dec` with its inspector and core, static, without exceptions; the encoder is not built): six committed streams, 2.0 and 5.1 with and without A-CPL, 5.1.4 and 2.0 with companding, each channel's level exact against `apps/baremetal/ac4_fixture.hpp`, and the PCM bit-identical to the x86-64 host's (`tests/golden/ac4-probe-pcm-hashes.json`). 750,276-byte image (196 KB of it the sample rate converter's tables and 53 KB the inverse transform's), 286 KB to 1.49 MB peak heap by fixture, 48 to 202 allocations a frame, 19.5 KB of stack, 25.2 M to 161.8 M instructions a frame under `--ac4 --icount` (planning/ac4.md, D14f) - see [the AC-4 rows](#the-ac-4-probe) |
| Image size | 355,709 bytes — 293,092 `.text`, 400 `.data`, 62,217 `.bss` |
| Peak heap | 195,025 bytes, the height-object fixture placed onto 7.1.4 (`eac3_atmos_render`); 194,655 with Atmos objects reconstructed, 173,794 for the 7.1.4 fixture folded to stereo, 167,386 as coded |
| Retained after teardown | 12 bytes, one `__cxa_thread_atexit` record; the enhanced-coupling scratch (23,552 bytes while §E3.5 is in use) is handed back between fixtures |
| Allocation per frame | 1 to 27, by fixture — see [the footprint table](../../performance-trend.md#minimum-footprint-decoder) |
| Audio output | None. The probe decodes built-in fixtures and prints levels |
| Real silicon | None. Correctness is established under emulation |
| CI | `build-footprint` in `.github/workflows/_build.yml`, in the `esp` lane of `ci.yml`: after a merge to main that changes the ESP32 trees or a tree its component ships (the [lane table](../../ci-lanes.md#lane-table) lists them), and in the nightly run ([CI for many agents](../../ci-agentic.md#the-tiers)). A pull request's gate builds none of it |

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

# The AC-4 decoder's probe, in its own build: the same three ways
tools/checks/run_baremetal_probe.sh --ac4
tools/checks/run_baremetal_probe.sh --ac4 --host
tools/checks/run_baremetal_probe.sh --ac4 --icount
```

`--icount` is the one timing figure this leg can give. QEMU is not cycle-accurate and the probe's
ordinary clock is semihosting's, which reports the host's time; but under `-icount shift=0` the
guest's own clock advances one nanosecond per executed instruction, and a build whose clock reads
the mps2-an385's 25 MHz timer (`ICLFORGE_BAREMETAL_CLOCK=timer`, its own preset and build
directory) follows it. Every microsecond the probe then prints is a thousand Thumb-2 instructions,
identical on every host and every run — `eac3.instructions_per_frame=12942000` — gated per fixture
with the same headroom rule as the other ceilings, and with `--stage-timers` counted per stage. It
is not cycles on any real part; it is a number that moves when the code does, which the host-time
figure never was, and it is what the [ESP32-C3](esp32-c3.md) row's speed estimate rests on. That part now runs the fixed tier's probe under `qemu-riscv32` for correctness, but its time still comes from here, and instructions are not cycles on either part.

Both drive the presets, which you can also use directly: `config-arm-none-eabi-minimal` /
`build-arm-none-eabi-minimal`, and `config-linux-gcc-minimal` or `config-linux-llvm-minimal` for
the host (the runner's `--host` uses the GCC one). The AC-4 probe's are the same names with `-ac4`
after `minimal`, for `arm-none-eabi` and for GCC on the host; there is no LLVM one. GCC and Clang
only.

[Building from source](../../building.md#minimum-footprint-decoder-profile) covers what the profile
changes and the gaps it has not closed. The measured figures are in
[the footprint table](../../performance-trend.md#minimum-footprint-decoder).

## What you give up

The profile replaces what `src/ac3` builds rather than adding to it, and configure fails with a
list if any component needing the full library is still switched on.

| Not compiled | Consequence |
|---|---|
| Under `ICLFORGE_MINIMAL_DECODER`: the encoder, container writers, WAV I/O, analysis and QC layers, the object encoder | Decode only. `src/ac3/minimal.cmake` lists what is compiled, with a line on why each file is reachable from a decode. `ICLFORGE_MINIMAL_ENCODER` is the same profile pointed the other way. |
| The direct-form transform tables | 1,900,544 bytes of `.bss` absent from the image rather than merely unused. `DecoderConfig::fast_imdct = false` returns `DecodeError::kNoReferenceTransform` here instead of being served quietly by the fast path. |

## The probe

`apps/baremetal/probe.cpp` links the archive, decodes six frames each of fourteen fixtures, compares
every channel's level against `apps/baremetal/fixture.hpp`, and prints `key=value` lines the
runner gates on: the levels, image size, peak heap, retained bytes and allocations per frame.
`encode_probe.cpp` is its counterpart, checking a byte count and FNV-1a hash against
`encode_fixture.hpp`, and `ac4_probe.cpp` the AC-4 decoder's, decoding six streams through
`Decoder::decode_by_block` and reading the stack the decode used by painting a window of it first.

It is not a unit test — the profile requires `ICLFORGE_BUILD_TESTS=OFF`, since nothing under
`tests/` builds against this archive — and it answers three questions a test could not: does the
archive link with everything else absent, does it produce the right audio on a 32-bit soft-float
target, and what did it cost.

Nothing regenerates the fixtures automatically and nothing detects that they have drifted from the
encoder: the probe decodes a committed bitstream and compares it against committed levels, so both
moving together is invisible to it. Regenerate with
`python tools/generators/gen_baremetal_fixture.py --forge <path>`, and the AC-4 fixtures with
`python tools/generators/gen_baremetal_ac4_fixture.py --forge <path>`.

### The AC-4 probe

`ac4_probe.cpp` decodes six committed streams (`apps/baremetal/ac4_fixture.hpp`): DEE's 2.0 with
A-SPX, a constructed 2.0 in A-CPL, DEE's 5.1, a constructed 5.1 in A-CPL, DEE's 5.1.4 tones and
DEE's 2.0 at 48 kbit/s, whose A-SPX runs companding (added by D14a4, the one fixture that reaches
`float` `pow` and `exp2`), two to four frames each, all at frame rate index 13 (2,048 samples at 48
kHz). The rows are the `build-footprint` job's on 2026-09-29, in `float`, with GCC 14.2.1 under
QEMU 10.2.1, `-Os`, soft float, and the generic SIMD seam, so the QMF banks' vector kernels run
through its portable types; the instruction counts, the peak heaps, the allocations and the stack are
measured again with D14e on 2026-10-02 (the counts are within 1.1% of D14a4's, the peaks 0.8 to 6.8% lower, and the
image 8,448 bytes larger):

| Fixture | Frames | Instructions a frame | Peak heap | Allocations a frame, steady | Stack |
|---|---:|---:|---:|---:|---:|
| `ac4_20_music`, 2.0 | 3 | 54,766,000 | 413,611 | 56 | 16,580 |
| `ac4_20_acpl`, 2.0 A-CPL | 4 | 57,284,000 | 601,504 | 50 | 19,480 |
| `ac4_51_music`, 5.1 | 3 | 116,737,000 | 946,390 | 147 | 19,480 |
| `ac4_51_acpl`, 5.1 A-CPL | 4 | 122,882,000 | 1,147,590 | 84 | 19,480 |
| `ac4_514_tones`, 5.1.4 | 2 | 206,264,000 | 1,800,312 | 203 | 19,480 |
| `ac4_20_companding`, 2.0 with companding | 3 | 58,818,000 | 462,435 | 73 | 19,480 |

The runner holds each column to a ceiling about a tenth above the figure (`ICOUNT_CEILING_AC4`,
`PEAK_CEILING_AC4` and `CHURN_CEILING_AC4` in `tools/checks/run_baremetal_probe.sh`), and the image
(691,896 bytes, ceiling 750,000) and the stack (ceiling 21,500) have ceilings of their own. Nothing
is retained after the decoders are destroyed. The stack is read by painting a window of it below
the probe's frame before each fixture and finding how far the decode reached. The PCM of each
fixture hashes to the value `tests/golden/ac4-probe-pcm-hashes.json` pins, on this leg and on the
x86-64 host, and an ESP32-P4 board's hash equals the same six
([ESP32-P4](esp32-p4.md#float-output-on-the-host-the-cortex-m3-leg-and-the-board)). Instructions
are not cycles: the [P4 page](esp32-p4.md#what-it-decodes-in-real-time) has what the decoder takes on
a part with an FPU. In the fixed-point tier (`--ac4 --scalar=fixed`) the six fixtures take 6.7 M
to 43.0 M instructions a frame on this leg, 0.27 to 0.30 of the `float` tier's (planning/ac4.md, D14f), and hash to the
values `tests/golden/ac4-fixed-probe-pcm-hashes.json` pins, on this leg, on the x86-64 host and on
RV32IMC ([the performance trend](../../performance-trend.md#the-ac-4-decoder) has the table). No
board has run AC-4 in the fixed-point tier.

## Porting to another part

Two things are target-specific, both under `apps/baremetal/platform/`: a thread-pointer stub
(`baremetal/tls.cpp`, whose static block the linker script bounds with two `ASSERT()`s) and the
linker script and startup for the board. Nothing in `src/` branches on the target.

Whether a part is viable comes down to floating point. Every operation the decoder does in
`double` is software-emulated without an FPU, and `decode_scalar_t` is `float` under this profile
precisely because that is what the parts with hardware floating point actually have. The
[ESP32-C3 page](esp32-c3.md) works that argument through one vendor's range.
