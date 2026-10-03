# One implementation, three arithmetics, one effort axis

!!! note "Status as of 2026-09-30: three tiers built for AC-3 and E-AC-3; AC-4's decoder has two of them"
    All three arithmetic tiers exist for AC-3 and E-AC-3 and are gated. `double`, the reference, in
    every ordinary build; `float`, the ESP32-S3's, for decode since 2026-09-09 and for encode since
    2026-09-10 (#617, #618); and `fixed`, the decoder for parts with no floating-point unit, whose
    phases A to C below are done on the host and the Cortex-M3 leg with the numbers each measured.
    Phase D's correctness half is done on the ESP32-C3 target under `qemu-riscv32`, where the
    tier's hashes agree across three architectures on the 12 of fourteen fixtures that fit in the
    part's SRAM. Its timing half is not done for the C3, which has never been on a board; an
    ESP32-C6 took its place on 2026-09-15 and ran all fourteen fixtures on the same hashes. The
    effort axis has its first measured point: the search and the planner cost (#619, on the
    ESP32-S3).

    **AC-4 joined the seam in phase D14 of [the AC-4 plan](ac4.md#d14-ac-4-on-the-esp32s).**
    D14a moved `Fixed32`, the float scalar functions and the SIMD seam into a header-only target,
    `src/arithmetic`, and made the AC-4 decoder's kernels take `AC3FORGE_DECODE_SCALAR`: `double`
    (the default) or `float`. D14b built the `float` decoder into the ESP-IDF component behind
    `CONFIG_AC3FORGE_AC4` and measured it on an ESP32-P4. D14d adds the AC-4 decoder's
    fixed-point tier, for the C6. There is no S3 phase yet (D14c), and the encoder stays
    in `double` on every platform. See
    [AC-4 on the same seam](#ac-4-on-the-same-seam).

    Design sections say what each tier and each effort level is and what it guarantees; each
    phase carries an exit criterion and how it is verified; [Decisions](#decisions) lists what
    was open, the option recommended on each and what came of it; [What cannot be verified, and why](#what-cannot-be-verified-and-why)
    says where the evidence runs out. Plain tone, like the other pages here.

## Why three, and why one implementation

The codec's arithmetic is one thing and the platform's arithmetic unit is another. A/52 is
specified in real arithmetic; the reference decoder is floating point; certified decoders in
phones and televisions are fixed point. Between them sit every microcontroller this project has
looked at: an ESP32-S3 with a single-precision FPU, where `double` is a call into the mask ROM's
software routines and a 5.1 E-AC-3 frame decoded at 2.46x real time until the arithmetic moved
to `float` ([the ESP32-S3 page](../docs/platforms/bare-metal/esp32-s3.md#timing)); an ESP32-C3 with no FPU at
all, where even `float` is a compiled subroutine and the same frame is
[12.9 M soft-float instructions](../docs/performance-trend.md#instructions-per-frame) against a
5.1 M-cycle budget at 160 MHz.

The choice this page makes is that these are **one implementation with three scalars**, not
three implementations. The bitstream syntax, the exponent and mantissa machinery, the coding
tools' logic, the planners and the searches are integer or structural and shared; what varies
is the type the coefficients, the transforms and the analyses run in. That is already how the
first two tiers are built: `decode_scalar_t` and `encode_scalar_t`
(`src/ac3/variants/`) are template parameters whose `<double>` instantiations are
textually the functions the reference build always called, so the golden bitstream hashes and
the fixture levels pin the reference while the other tier is measured against it.

What each tier promises, and how the promise is checked:

| Tier | Scalar | Where it runs | What it guarantees | The gate |
|---|---|---|---|---|
| Reference | `double` | Every hosted platform: x86-64, AArch64, macOS, Android, WASM | The bitstream hashes in `tests/golden/bitstream-hashes.json`, byte-identical across kernels and architectures; the gold-reference SNR floors against FFmpeg | `verify_gold_reference.sh`, `check_cross_platform_hash.py`, the full suite |
| Single precision | `float` | ESP32-S3 (and any part with a single-precision FPU); measurable on any host with `-DAC3FORGE_DECODE_SCALAR=float` / `-DAC3FORGE_ENCODE_SCALAR=float` | Decode: reproduces the double decode to ~139 dB. Encode: a different, equally valid stream whose worst channel is within 0.5 dB of the double encoder's (identical to the hundredth on the five gold streams); the profile's fixture hashes identical on the x86 host, the Cortex-M3 leg and the board | `check_decode_scalar_snr.py`, `check_encode_scalar_quality.py`, the float gold gates in CI, `run_baremetal_probe.sh` on three legs |
| Fixed point | `Fixed32` | ESP32-C3/C6, Cortex-M0+/M3/M4 without FPU, any RV32IM; measurable on any host with `-DAC3FORGE_DECODE_SCALAR=fixed` | Decode: 121 dB and above from the double decode on the worst channel of the three gold streams, 111 dB and above on every checked-in third-party stream (measured 2026-09-10, held to 110 in CI); **bit-identical output on every platform by construction**, integer arithmetic having no rounding-mode or library variation to differ by | The probe's `pcm_hash` lines, pinned in `tests/golden/fixed-probe-pcm-hashes.json` and held there on the x86 host and the Cortex-M3 leg (`check_probe_hashes.py`); `check_decode_scalar_snr.py` against the double CLI at 110 dB; the gold gate with the fixed CLI at the double decoder's floors |

The fixed tier's guarantee is the strongest of the three - integer arithmetic is the same on
every machine - and its fidelity is between the other two's: below the float tier's 139 dB,
and above the 100 dB this page set out to reach, because the store carries a block exponent
(see [The block exponent](#the-block-exponent)) rather than an absolute scale. That is the
trade the choice matrix below is for.

## The effort axis

Arithmetic is the cost of each operation; the search decides how many there are. An E-AC-3 5.1
frame on the ESP32-S3, with the encoder in `float` end to end, spent 84 ms self time of which
28 was exponent-run planning, 13 bit-allocation calls and 9 mantissa bit counts - integer work,
and a count of candidates rather than an arithmetic type ([the Encoding section](../docs/platforms/bare-metal/esp32-s3.md#encoding)).

Two kinds of change apply to that count, and the distinction matters for the guarantees above:

- **Exact.** The same candidates scored, the same answer found, less work: the planner scoring
  both frame forms in one pass with a lower bound that skips runs whose exponent set alone
  costs more than the best plan found, the masking curve computed once per run and the offset
  applied per probe (`compute_masking_curve` / `allocate_from_curve`), blocks that read the
  same runs counted once. These change nothing about the stream and are gated by the stream
  not changing - the fixture hashes and the golden pins.
- **Path or budget.** A different probe sequence, a candidate not considered, a search stopped
  early. These change streams. The first of them was not meant to: the rate-control search's
  warm start was documented as never changing the answer, and does, because the frame's
  mantissa cost is not monotone in the SNR offset (`snr_search.hpp` now says why), so the
  probe path decides which of several fitting boundaries a rare frame lands on. Every such
  answer is valid, the quality gates do not move, and the hashes are re-pinned when the path
  changes - which is what makes a path change a *release note*, not a refactor.

The effort axis is the second kind, made explicit. A platform picks a level; the level names
concrete budgets; the levels are measured, on the board and on the gold gates, and documented
with their cost and their quality. Levels are per encoder instance (a configuration field), so
one library serves every platform and a platform's default is its own to set - the ESP-IDF
component's profile is where the ESP32 chooses.

| Level | What it changes | Cost on the ESP32-S3 | Quality |
|---|---|---|---|
| `reference` (default) | Nothing: the exhaustive planner, the delta race, the full search | The figures on the ESP32-S3 page | The gates as they stand |
| `reduced` | §7.2.2.6 delta bit allocation off (`delta_allocation = false`, `delta=off`, `nodelta`): no segments chosen, no second search to weigh them | About 9 ms of an E-AC-3 5.1 frame's 55.5 on the ESP32-S3: the segments' own stage (4.8 ms), the second search (about 4.6) and the side-information re-measurement between them | 0.01 dB on the worst channel of the two E-AC-3 gold streams, nothing on the two AC-3 ones (the race was already dropping the segments on most of their frames) |
| `minimal` (proposed) | `reduced`, plus the hoisted frame form only and a search that stops within four offset units of the boundary | Not measured | Not measured; four units is 0.75 dB of offset |

The first level after `reference` is the one #619 added, measured as the table says
(`EncoderConfig::delta_allocation` and `eac3::FrameConfig::delta_allocation`, spelled `delta=off`
or `nodelta` on the CLI); the third is listed so the axis has a shape, not because it is
decided, and nothing in the tree implements it. The effort axis is AC-3 and E-AC-3's: the AC-4
encoder has no effort levels.

## The choice matrix

Platform to arithmetic to effort, with the state of each cell. "Real time" is a 32 ms frame at
48 kHz.

| Platform | Decode | Encode | Effort default | State |
|---|---|---|---|---|
| x86-64, AArch64 (desktop, server, Raspberry Pi 4/5, Android, macOS) | `double` | `double` | `reference` | Shipping; the reference build |
| WASM | `double` | `double` | `reference` | Shipping ([the WASM page](../docs/platforms/wasm.md)) |
| ESP32-S3 (LX7, single-precision FPU) | `float` | `float` | `reference` for 2/0; `reduced` is the candidate for 5.1 | Decode: every fixture in real time. Encode: AC-3 2/0 and E-AC-3 2/0 in real time, AC-3 5.1 at the line, E-AC-3 5.1 at 1.7x |
| ESP32 (LX6, single-precision FPU) | `float` | `float` | as the S3 | Not measured; the S3's arithmetic without the PIE and with a smaller cache |
| ESP32-P4 (RV32IMAFC, single-precision FPU, 360 MHz on chip revision v1.3) | `float` | `float` | as the S3 | Decode: every one of the fourteen fixtures in real time on a board with no network, 0.027x to 0.448x ([its page](../docs/platforms/bare-metal/esp32-p4.md)). Encode: not measured |
| ESP32-C3 / C6 (RV32IMC / RV32IMAC, no FPU) | `Fixed32` | none at first | `reduced` | C3: built and gated, a probe target under `qemu-riscv32`, 12 of fourteen fixtures decoding to PCM identical to the host's and the Cortex-M3 leg's; the two 7.1.4 rows do not fit in the part's SRAM; time on a board unmeasured. C6: timed on a board ([its page](../docs/platforms/bare-metal/esp32-c6.md)), all fourteen fixtures on the same hashes; AC-3 and E-AC-3 5.1, stereo and mono in real time with WiFi running (5.1 at 0.82x and 0.96x) |
| Cortex-M3 (the CI leg, QEMU) | `float`, soft | `float`, soft | `reference` | Correctness and instruction counts only; the soft-float proxy every embedded estimate rests on |
| Cortex-M4F / M7 (single-precision FPU) | `float` | `float` | `reference` | Not targeted; would behave as the S3 without its vector loads |

The matrix is AC-3 and E-AC-3's. AC-4's rows are in [the section that follows](#ac-4-on-the-same-seam).

## AC-4 on the same seam

The choice above was carried to AC-4 by phase D14 of [the AC-4 plan](ac4.md#d14-ac-4-on-the-esp32s),
as [decision 25](ac4.md#decisions-of-2026-09-25) took it: one implementation with three scalars, as
AC-3 and E-AC-3 have. What is built, and what is not:

| Part | State |
|---|---|
| The scalar (D14a) | `src/ac4core`'s kernels and `src/ac4dec/src/pcm` are templated on `Real`, with a complex type of the project's own (`dsp::Complex<Real>`) in place of `std::complex`. `AC3FORGE_DECODE_SCALAR` selects `double` (the default), `float` or, since D14d, `fixed` for AC-4 as it does for AC-3 and E-AC-3. The `float` build of `src/ac4core` compiles with `-Wdouble-promotion` as an error. The AC-4 tests pass at all three scalars |
| The shared target (D14a) | `src/arithmetic` (`ac3::arithmetic`) is header-only and holds `Fixed32` (`ac3/internal/fixed32.hpp`), the float scalar functions (`ac3/internal/scalar_math.hpp`) and the SIMD seam (`arch/generic`, `arch/x86_64`, `arch/aarch64`). `ac3::forge` and `src/ac4core` link it, so nothing is copied ([decision 31](ac4.md#decisions-of-2026-09-25)). Before D14a the first two lived in `src/ac3` |
| The decoder's size (D14a) | `SubstreamPcm` fell from 299 KB to 10.9 KB at `double`, D14a removed every guarded function-local static from `src/ac4core`, `src/ac4dec` and `src/ac4`, the QMF banks run on split real and imaginary planes with vector kernels that equal their scalar loops bit for bit, and the bit reader and the Huffman decoder are cached and table-driven |
| The probe's AC-4 rows (D14a) | `tools/checks/run_baremetal_probe.sh --ac4` decodes five committed streams (2.0 and 5.1 with and without A-CPL, and DEE's 5.1.4 tones) on the Cortex-M3 leg in `float`: 54.5 M to 205.8 M instructions a frame, a peak heap of 0.43 to 1.93 MB, a 486,192-byte image. The PCM equals the x86-64 host's, and the hashes are pinned in `tests/golden/ac4-probe-pcm-hashes.json` |
| `float` against `double` (D14a's exit) | On the 67 committed streams (`tools/checks/check_ac4_decode_scalar_snr.py`), the worst channel is 109.4 to 136.0 dB from the `double` decode below the lowest A-SPX crossover, and 37.5 to 102.1 dB above the highest where a stream has A-SPX. The floors are pinned 3 dB under those figures in `tests/golden/ac4dec/scalar-agreement.json`. The cause of the high band's gap is open |
| The ESP32-P4 (D14b) | `CONFIG_AC3FORGE_AC4` builds the `float` decoder into the ESP-IDF component, off by default and offered only on a part with a floating-point unit. On a board at 360 MHz with Wi-Fi up, 2.0 in SIMPLE mode decodes in 0.28 of real time and 2.0 in A-SPX mode in 0.37, and so does the frame-rate converter at 24, 25, 23.976 and 29.97 fps (0.51, 0.53, 0.68 and 0.69; D14a4 runs its dot product in `float`, D14a5's tables are built by the compiler and D14a6 takes the low-power SRAM out of the heap); D14e brings 5.1 in SIMPLE mode (0.64), A-SPX (0.83) and A-SPX with A-CPL mode 2 (0.91) under real time (the transforms, A-CPL and the output in fewer passes, `-O3` for the kernels and the flash read in QIO mode), and A-CPL mode 3 takes 1.14 and 5.1.4 in full decoding 1.57 to 1.90. The output equals the probe's pinned `float` hashes and the host's on all twenty plays: D14a4 took `std::pow`, `std::exp2` and `hypotf` at `float`, whose last bit differs between C libraries, out of libm. [The P4 page](../docs/platforms/bare-metal/esp32-p4.md#ac-4) has the tables |
| The ESP32-S3 (D14c) | Not built. The Cortex-M3 probe's 2.0 stream peaks at 432 KB of heap, where the S3's probe allows 245,000 bytes, so 2.0 in internal RAM needs the allocations halved again, or PSRAM |
| The fixed-point tier (D14d) | `Fixed32` with a block exponent per transform block and per QMF slot, and energies and gains as a mantissa and a power of two (`MantExp`, `src/arithmetic`). Against the `double` decode on the 67 committed streams, the worst channel is 105.7 to 132.1 dB below the lowest A-SPX crossover and 34.2 to 97.2 dB above the highest, pinned in `tests/golden/ac4dec/scalar-agreement-fixed.json`. The probe's six fixtures give one PCM hash on the x86-64 host, the Cortex-M3 leg and RV32IMC (`tests/golden/ac4-fixed-probe-pcm-hashes.json`); on the Cortex-M3 they take 6.7 M to 43.0 M instructions a frame since D14f, 0.27 to 0.30 of the `float` tier's (34.2 M to 90.4 M at D14d, when the first frame built the transform tables in software), and peak at 0.29 to 1.50 MB |
| The ESP32-C6 (D14d, D14f) | Builds with `CONFIG_ICLFORGE_AC4`. 2.0 peaks at 286,365 bytes on a 32-bit core since D14f (429,667 at D14d), where the board had about 236,000 free with WiFi up, 285,408 with WiFi's code in flash and 383,416 with no network. Not run on the board |
| The encoder | `double` on every platform. The AC-4 encoder has no `float` or fixed-point tier and never runs on an ESP32 ([decision 34](ac4.md#decisions-of-2026-09-25)); a `float` build of `src/ac4core` instantiates the kernels the encoder calls at `double` as well (`AC4CORE_ALSO_AT_DOUBLE`) |

The figures are those of the AC-4 plan's D14a and D14b records, which name their sources; this page
does not re-derive them.

## The fixed-point tier

### What the decoder needs of its scalar

The decode path is templated on `decode_scalar_t` in the files the survey found
(`eac3_decoder.cpp`, `decoder.cpp`, `output.cpp`, `mdct.cpp`, `eac3_tools.cpp`, `coupling.cpp`,
`mantissas.hpp`, `joc.cpp`, `spatial.cpp`). What it asks of the type:

- Arithmetic: `+ - *`, comparison, negation, `abs`; multiplication by a constant table entry
  (twiddles, windows, downmix coefficients); scaling by a power of two (`ldexp`, from an
  exponent).
- Functions: `sqrt` (three sites, the enhanced-coupling and spectral-extension gains), `exp2`
  and `lround` (spectral-extension noise gains, JOC), `log2` (a coupling coordinate), `cos`/`sin`
  (tables only: the transform twiddles, the enhanced-coupling DFT, the fold coefficients).
- The transforms: the §7.9.4 inverse pair through the FFT (`fft.cpp`, `mdct.cpp`), the
  §3.5.5 DFT for enhanced coupling, and the six-block DCT of the adaptive hybrid transform.

### The type

`Fixed32` (`src/arithmetic/include/iclforge/arithmetic/fixed32.hpp`): a signed 32-bit integer in Q7.24 - seven bits of
headroom above unity, twenty-four below. Products go through 64 bits (`mul`/`mulh` on RV32IM,
`smull` on Cortex-M3), round half up on the shift back and saturate; sums wrap; conversions
from a wider type saturate. Constants - the twiddles, the window, a downmix coefficient - are
the same format: a value of exactly 1.0 is 2^24 and fits, and twenty-four bits of a twiddle are
more than the arithmetic around it keeps (the plan had said Q2.30 for these; nothing needed the
extra bits).

Two things the type does not try to be. It is not a general fixed-point library: only the
operations the decode path asks of a scalar exist, each with one rounding rule, so the result
is defined by the source and not by a template's cleverness. And it is not
`std::floating_point`: the templates the decode path already has are written against a scalar
that behaves like a number, and where they call a `std::` function the call is an overload set
the way `scalar_math.hpp` already does for the float encode path - `scalar_sqrt`, `scalar_abs`,
`scalar_ldexp` - with the `double` and `float` overloads being what they were and the `Fixed32`
ones integer routines. Where a template needs more than arithmetic - a noise draw from a 32-bit
state, a dequantiser's integer over a power of two, a ratio of two integers - the public headers
(`mantissas.hpp`, `eac3_tools.hpp`) ask for it through a member of the type on their
non-floating branch.

### The block exponent

This is the part the plan did not have, and the measurement that put it there. Phase A stored
coefficients in Q7.24 directly, with the transform bridged through double, and the worst
channel of the gold AC-3 stream came out 98.8 dB from the double decode, the E-AC-3 one 99.0,
and the E-AC-3 coupling one 87.8. A raw unit is 2^-24 of full scale wherever a value sits, so a
mantissa of sixteen bits under an exponent of twelve keeps twelve of them; the transform sums
two hundred and fifty-six such errors; and standard coupling's factor of eight scales them by
eight. The information was on the wire and lost at dequantisation, because the store put every
value at one absolute scale; a wider store would not have kept it.

So the store is normalised (`src/ac3/src/decoder/block_norm.hpp`): each stream's
coefficients are kept scaled up by 2^norm per block, with norm chosen so the largest sits just
below one half, and every mantissa keeps all of its bits. The exponent travels with the block.
A stream's own coded bins set it before any mantissa is read; a tool that can raise a channel
above them - decoupling, enhanced coupling's reconstruction, spectral extension's synthesis,
rematrixing - lowers it where it runs, shifting what the channel already holds down to match,
so no bits are reserved that might not be needed. An AHT stream's six blocks are reconstructed
at once, so its exponent is exact from their peaks and no bound is needed at all; a coupling
or spectral extension coordinate is kept as its mantissa and its power of two, so the product
with a coefficient is one rounding and a shift; the §7.7 gain is split into a mantissa applied
to the coefficients and a power of two added to the exponent; and the overlap-add aligns the two
halves it sums, in 64 bits, before one float conversion applies the power of two exactly. The
floating tiers see none of this: their norms are zero, and `exponent_scale(exp - 0)` is the
call they always made.

Each of those was a measurement before it was a design. The first normalised build reached
121, 122 and 122 dB on the gold streams but 74 on a Dolby Encoding Engine 5.1 stream with
coupling, spectral extension and the AHT; keeping the spectral extension coordinate's power of
two apart from its mantissa took that to 97; and replacing a two-bit guard on AHT streams with
the exact exponent took it to 116. A rematrixed pair that was given its shared exponent twice -
once before dequantisation and once at rematrixing - cost the 2/0 streams 6 dB until the second
pass learned the first had already made room.

### The transform

The FFT-based inverse is its own kernel beside the double and float ones
(`src/ac3/src/core/mdct_fixed.hpp`): the same pre-twiddle, N/4-point FFT, post-twiddle and
window as `mdct.cpp`'s fast branch, transcribed step for step in `Fixed32`, on the shared
`fft_kernel.hpp` tables instantiated at that type. The plan had it scaling per stage - block
floating point inside the transform. It does not, and the reason is the block exponent above:
the spec's inverse is an unscaled sum, the 128-point FFT of it can grow by exactly seven bits
(four per radix-4 stage, then two), and Q7.24 has seven bits of headroom, so a transform whose
input is below one half - which the store's exponent guarantees - cannot wrap on any input at
all, the coherent one no real stream produces included. Every rounding step inside is a raw
unit of an intermediate up to two orders of magnitude larger than the output, so what sets the
floor is the post-twiddle and the window, about a raw unit per output sample, relative to a
block scaled up to the format. `tests/ac3/core/test_mdct_fixed.cpp` holds it to the double inverse
above 120 dB on dense blocks, 110 on sparse ones, and without wrapping at the worst case.

The §3.5.5 DFT, the six-block DCT and the spectral extension notch still run through `float`
copies at the seam - see Phase C.

### Phases

**Phase A - the scalar and the straight-line path. Done 2026-09-10.** `Fixed32` with its
operators and the overload sets; `-DAC3FORGE_DECODE_SCALAR=fixed` on the host, the transform
bridged through double at the seam; dequantisation, coupling, spectral extension, the gain and
the output stage in it. Measured: 98.8, 99.0 and 87.8 dB on the worst channel of the three gold
streams (`check_decode_scalar_snr.py`), which is the finding [The block exponent](#the-block-exponent)
records. The exit criterion was met as written and the number said the store had to change.

**Phase B - the transform and the block exponent. Done 2026-09-10.** `mdct_fixed.hpp` without
per-stage scaling, the store under its block exponent, the overlap-add in 64 bits. Measured:
121.2, 122.5 and 122.3 dB on the gold streams; the gold gate passes with the fixed CLI at the
double decoder's floors and its bitstreams are the pinned ones byte for byte; the probe's
twelve fixtures decode on the host and on the Cortex-M3 leg with identical `pcm_hash` lines
(all twelve; fourteen since the 7.1.4 fold and line-mode rows); the Catch2 suite is unchanged in the double build. On the M3 leg an E-AC-3 5.1 frame is
6.6 M instructions against the float tier's 12.9 M, AC-3 5.1 3.8 M against
10.2 M, 2/0 1.2 M against 3.5 M (`docs/performance-trend.md` has every row).

**Phase C - the tools. Done 2026-09-10.** Standard coupling and spectral extension went in
first, with their coordinates split (mantissa and power of two) and the extension's band energy
summed in 64 bits, and the AHT's exponent made exact. Measured on the thirteen checked-in
third-party streams (Dolby Encoding Engine and FFmpeg; AC-3 and E-AC-3; coupling, spectral
extension, the AHT and JOC among them): no channel below 111 dB, the DEE 5.1 stream that had
been at 74 at 116. The rest followed: the AHT's dequantisers and six-point inverse, the
spectral extension notch, and enhanced coupling's spectrum, amplitudes, angles and
reconstruction, with the tier's own sine and cosine. Fidelity did not move - every stream is
within a tenth of a dB of what the float bridges gave - and the cost did: enhanced coupling on
the Cortex-M3 leg went from 24.3 M instructions to 10.1 M, E-AC-3 5.1 from
6.6 M to 4.8 M, 7.1.4 from 17.7 M to 12.1 M.

Two things this phase settled that the plan had wrong. The DFT is where block floating point
belongs, not the IMDCT: enhanced coupling's 512-point transform can grow by nine bits where the
format has seven, and taking those bits off its input instead cost the enhanced coupling stream
seven decibels (109 dB against the double decode, where the float bridge had given 117). With
the stages shedding bits only where the next would overflow, and the spec's 1/N carried in the
exponent rather than taken out of the values, it measures 116.6. And JOC is not a bridge of this
tier's at all: its reconstruction runs in `float` in every build of this library
(`recon_scalar_t`), so the object rows are a float transform sandwich whatever the decoder's
scalar is. Bringing JOC into the tier is a fixed forward MDCT and a fixed QMF path - real work,
with its own quality question, and out of this tier's scope.

Enhanced coupling also had no SNR measurement until this phase: none of the gold streams used
the tool. `check_decode_scalar_snr.py` now encodes and checks a fourth stream that does, for
both non-double scalars.

**Phase D - the part. Correctness done 2026-09-10; the C3's timing needs a board, and a C6's was
measured on 2026-09-15.**
`apps/baremetal/platform/esp32c3/` is the probe's third target: an ESP-IDF project like the S3's,
defaulting to `-DAC3FORGE_DECODE_SCALAR=fixed` because the part has no floating-point unit.
`qemu-riscv32` is installed beside the Xtensa one, `tools/checks/run_esp32c3_probe.sh` drives the
leg, and the component's manifest lists `esp32c3` beside `esp32s3` - with the packaging check now
building the archive for every target the manifest claims rather than only the first, since a
claimed target nobody links is the failure that list exists to prevent.

The exit criterion is met for 12 of the fourteen fixtures: they decode under
`qemu-system-riscv32` and their PCM is **identical to the x86 host's and the Cortex-M3 leg's**,
held to the same pinned set (`tests/golden/fixed-probe-pcm-hashes.json`). Three architectures -
x86-64, Thumb-2, RV32IMC - three compilers, one set of hashes. That is the tier's central claim,
and until this leg existed it rested on two.

The twelfth is a finding rather than a pass. 7.1.4 needs 238,094 bytes of heap and this
part does not have them: it reports 249,180 free, but its heap is regioned with a
largest block of 114,688, and the fixture failed on a 6,144-byte request with eleven
kilobytes still nominally free. So the probe now takes a per-target heap budget
(`AC3FORGE_PROBE_HEAP_BUDGET_BYTES`), skips a fixture above it and names it -
`eac3_714.skipped=heap_budget needed=238094 budget=230000` - and the hash check treats a
declared skip as a skip while still failing on an undeclared absence. What the C3 does run peaks
at 225,038 bytes with 12 retained after teardown, leaves 10,688 of
the main task's 32,768-byte stack, and links to a 523,072-byte image.

What is NOT done, and cannot be here: the time. QEMU is not cycle-accurate and reports a
fabricated clock, so this leg says the decode is correct on RISC-V and nothing about whether it
keeps up. A 32 ms frame at 160 MHz is 5.12 M cycles; the Cortex-M3 leg's instruction counts put
AC-3 2/0 and mono comfortably inside that and E-AC-3 5.1 near it, but instructions are not cycles
and no leg models this part's 16 KB flash cache. A board settles it.

**2026-09-15: the first RISC-V board was an ESP32-C6**, the same kind of core at the same clock
with 512 KB of SRAM ([its page](../docs/platforms/bare-metal/esp32-c6.md)). All fourteen fixtures
decode there with every PCM hash on the pinned set, a fourth architecture on one set of hashes.
The proxy was optimistic: AC-3 5.1 takes 34.7 ms a frame (5.56 M cycles against the M3's 3.8 M
instructions) and E-AC-3 5.1 38.7 ms, so neither is in real time even with no network, while
stereo and mono are. Stage timers put 73% of an AC-3 5.1 frame in the fixed-point IMDCT and
overlap-add. The float tier on the same part is 2.1x to 3.5x slower. The C3 itself is still
untimed.

**Later the same day, the tier's arithmetic on that core.** Disassembled, the transform's cost
was not its multiplies: each Q7.24 product carried four branches for the saturation, two of them
taken, and the overlap-add a software float conversion and multiply per sample. The products the
transform's growth bound keeps below the format's edge now skip the saturation test, the
overlap-add runs on 32 bits and builds its floats from the integers' bits, `Fixed32`'s product
tests for saturation once, and its shifts, small ratios and square root no longer call 64-bit
library routines. The pinned hashes do not move. AC-3 5.1 takes 20.5 ms a frame and E-AC-3 5.1
23.9 ms with no network, 26.2 and 30.6 ms with WiFi and a stream arriving, so both are in real
time with the network up; the float tier is now 2.3x to 4.6x slower. The C6 page has the per-change
table.

**Phase E (optional) - the encoder.** Not planned in this round, and not built. The encoder's
analysis is more precision-sensitive than the decoder's synthesis, and the S3's float encoder is
the shape a C3 encoder would take only after the decoder has shown what the fixed tier costs.

## Decisions

Each carries the option recommended when the page was written and, in the last sentence, what
came of it.

1. **Which fidelity the fixed tier promises.** Options: bit-exact to a spec-defined fixed-point
   reference (there is none in A/52), a stated SNR to the double decode, or "conforms to
   FFmpeg's fixed decoder". Recommended: a stated SNR, measured, with 100 dB as the target -
   it is the same kind of promise the float tier makes and the same tool measures it. *Taken as
   recommended; the measured figures (121 dB and above on the gold streams, 111 dB on the
   third-party ones, held to 110 in CI) are in the tier table above.*
2. **Decoder first, encoder later or never.** Recommended: decoder first (Phase E optional).
   The C3 is a sink-class part; nothing on the platform matrix asks it to encode. *Taken; Phase E
   has not started.*
3. **Q-format.** Q7.24, constants included. The plan's alternative - a narrower format with
   block exponents everywhere - was half right: the format stayed, and Phase A's measurement
   put the exponent on the store anyway (see [The block exponent](#the-block-exponent)),
   because no width of absolute format keeps a small mantissa's bits. Decided by measurement.
4. **The soft-float proxy.** The Cortex-M3 leg is what every embedded estimate rests on; a
   RISC-V QEMU leg would be a second. Recommended: install `qemu-riscv32` through ESP-IDF's
   `idf_tools.py` (about 30 MB into `D:\esp\tools`) when Phase D starts, not before; the M3
   carries Phases A to C. *Taken; the leg exists (`tools/checks/run_esp32c3_probe.sh`).*
5. **A C3 board.** QEMU gives correctness and instruction counts, not the part's cache
   behaviour, and the C3 executes from flash through a 16 KB cache. Recommended: a board for
   Phase D; the S3 work showed QEMU's timing is not the board's. *Not met for the C3: no
   board has run it. An ESP32-C6 board, the same core at the same clock, supplied the
   measurements on 2026-09-15.*
6. **The effort axis's first level.** Recommended: `reduced` = delta bit allocation off, as a
   configuration field on both encoders with a CLI spelling, measured on the board and the
   gold gates before it is documented as a level. *Taken and built (#619).*

## What cannot be verified, and why

- **The fixed tier's time on a C3** until there is a board; QEMU has no cache model. The M3
  instruction count is a proxy for the arithmetic, not the memory system. An ESP32-C6 has been
  timed instead, and its figures are the nearest available.
- **The fixed tier's fidelity on material other than the sixteen streams measured.** The
  block exponent's bounds are stated in `block_norm.hpp` and each is on the side of never
  wrapping, but the SNR figures are those streams'; a stream whose tools push a channel to
  the format's edge would show as clipping at a conversion, not as wrapped arithmetic, and the
  probe's hashes would still agree across legs.
- **Exactness of a search-path change.** The rate-control predicate is not monotone, so no
  change to the probe sequence can be shown stream-preserving; it is shown quality-preserving
  by the gates and re-pinned. The exact changes in #619 are shown exact by their reference
  tests and by the hashes not moving.
- **The effort levels' quality on material other than the five gold streams.** The gates are
  what a laptop can hold in a minute; the ViSQOL trend in CI is the broader measure.
- **The AC-4 `float` decode above A-SPX's crossover.** The gap to the `double` decode is measured
  (37.5 to 102.1 dB on the committed streams that use A-SPX) and pinned, and its cause is open:
  accumulating Pseudocode 86's covariances and solving Pseudocode 87 in `double` moved no figure
  by 0.1 dB, so the prediction is not it, and the worst frames of a channel are far below its
  aggregate, which points at a few decisions or gains that flip or move.
- **AC-4 companding streams on a board.** The ESP32-P4's `float` output equals the host's on 15
  of the 20 plays and differs on the five where companding runs, because `std::pow` and
  `std::exp2` at `float` give a different last bit in each C library. Routing them through the
  project's own functions gave one hash on the host, a Cortex-M3 program and the board in a
  scratch copy; that change is not in the tree.
- **AC-4 on the S3 and the C6.** Nothing is built or measured for the S3; the P4's figures say
  what a single-precision FPU manages at 360 MHz, not what its 240 MHz will. The C6's
  fixed-point tier is built and measured on the host and under QEMU; it does not fit the part
  beside WiFi, and its time on the part has not been measured.
