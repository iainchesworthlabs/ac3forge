# One implementation, three arithmetics, one effort axis

!!! note "Status as of 2026-09-10: three tiers built, the third measured on the host and the Cortex-M3 leg, no C3 yet"
    All three arithmetic tiers exist and are gated. `double`, the reference, in every ordinary
    build; `float`, the ESP32-S3's, for decode since 2026-09-09 and for encode since 2026-09-10
    (#617, #618); and `fixed`, the decoder for parts with no floating-point unit, whose phases
    A to C below are done on the host and the Cortex-M3 leg with the numbers each measured.
    What is not done: Phase D - no ESP32-C3 target directory, no RISC-V QEMU installed, no board
    - and the three tools that still run through `float` copies at the seam. The effort axis
    has its first measured point: the search and the planner cost (#619, on the ESP32-S3).

    Design sections say what each tier and each effort level is and what it guarantees; each
    phase carries an exit criterion and how it is verified; [Decisions](#decisions) lists what
    is open and the option recommended on each; [What cannot be verified, and why](#what-cannot-be-verified-and-why)
    says where the evidence runs out. Plain tone, like the other pages here.

## Why three, and why one implementation

The codec's arithmetic is one thing and the platform's arithmetic unit is another. A/52 is
specified in real arithmetic; the reference decoder is floating point; certified decoders in
phones and televisions are fixed point. Between them sit every microcontroller this project has
looked at: an ESP32-S3 with a single-precision FPU, where `double` is a call into the mask ROM's
software routines and a 5.1 E-AC-3 frame decoded at 2.46x real time until the arithmetic moved
to `float` ([the ESP32-S3 page](../docs/platforms/esp32.md#timing)); an ESP32-C3 with no FPU at
all, where even `float` is a compiled subroutine and the same frame is
[12.9 M soft-float instructions](../docs/performance-trend.md#instructions-per-frame) against a
5.1 M-cycle budget at 160 MHz.

The choice this page makes is that these are **one implementation with three scalars**, not
three implementations. The bitstream syntax, the exponent and mantissa machinery, the coding
tools' logic, the planners and the searches are integer or structural and shared; what varies
is the type the coefficients, the transforms and the analyses run in. That is already how the
first two tiers are built: `decode_scalar_t` and `encode_scalar_t`
(`src/forge/src/internal/scalar/`) are template parameters whose `<double>` instantiations are
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
and a count of candidates rather than an arithmetic type ([the Encoding section](../docs/platforms/esp32.md#encoding)).

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

The first level after `reference` is the one this branch adds, measured as the table says; the
third is listed so the axis has a shape, not because it is decided.

## The choice matrix

Platform to arithmetic to effort, with the state of each cell. "Real time" is a 32 ms frame at
48 kHz.

| Platform | Decode | Encode | Effort default | State |
|---|---|---|---|---|
| x86-64, AArch64 (desktop, server, Raspberry Pi 4/5, Android, macOS) | `double` | `double` | `reference` | Shipping; the reference build |
| WASM | `double` | `double` | `reference` | Shipping ([the WASM page](../docs/platforms/wasm.md)) |
| ESP32-S3 (LX7, single-precision FPU) | `float` | `float` | `reference` for 2/0; `reduced` is the candidate for 5.1 | Decode: every fixture in real time. Encode: AC-3 2/0 and E-AC-3 2/0 in real time, AC-3 5.1 at the line, E-AC-3 5.1 at 1.7x |
| ESP32 (LX6, single-precision FPU) | `float` | `float` | as the S3 | Not measured; the S3's arithmetic without the PIE and with a smaller cache |
| ESP32-C3 / C6 (RV32IMC, no FPU) | `Fixed32` | none at first | `reduced` | The tier is built and measured on the host and the Cortex-M3 leg; the part itself is Phase D, not started |
| Cortex-M3 (the CI leg, QEMU) | `float`, soft | `float`, soft | `reference` | Correctness and instruction counts only; the soft-float proxy every embedded estimate rests on |
| Cortex-M4F / M7 (single-precision FPU) | `float` | `float` | `reference` | Not targeted; would behave as the S3 without its vector loads |

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

`Fixed32` (`src/forge/src/core/fixed32.hpp`): a signed 32-bit integer in Q7.24 - seven bits of
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
eight. The information was on the wire and lost at dequantisation. The store was not too
narrow - it was in the wrong place.

So the store is normalised (`src/forge/src/decoder/block_norm.hpp`): each stream's
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
(`src/forge/src/core/mdct_fixed.hpp`): the same pre-twiddle, N/4-point FFT, post-twiddle and
window as `mdct.cpp`'s fast branch, transcribed step for step in `Fixed32`, on the shared
`fft_kernel.hpp` tables instantiated at that type. The plan had it scaling per stage - block
floating point inside the transform. It does not, and the reason is the block exponent above:
the spec's inverse is an unscaled sum, the 128-point FFT of it can grow by exactly seven bits
(four per radix-4 stage, then two), and Q7.24 has seven bits of headroom, so a transform whose
input is below one half - which the store's exponent guarantees - cannot wrap on any input at
all, the coherent one no real stream produces included. Every rounding step inside is a raw
unit of an intermediate up to two orders of magnitude larger than the output, so what sets the
floor is the post-twiddle and the window, about a raw unit per output sample, relative to a
block scaled up to the format. `tests/core/test_mdct_fixed.cpp` holds it to the double inverse
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
(all twelve); the Catch2 suite is unchanged in the double build. On the M3 leg an E-AC-3 5.1 frame is
6.6 M instructions against the float tier's 12.9 M, AC-3 5.1 3.8 M against
10.2 M, 2/0 1.2 M against 3.5 M (`docs/performance-trend.md` has every row).

**Phase C - the tools. Partly done 2026-09-10.** Standard coupling and spectral extension are
in the tier with their coordinates split (mantissa and power of two) and the extension's band
energy summed in 64 bits; the AHT's exponent is exact. Measured on the thirteen checked-in
third-party streams (Dolby Encoding Engine and FFmpeg; AC-3 and E-AC-3; coupling, spectral
extension, the AHT and JOC among them): no channel below 111 dB, the DEE 5.1 stream that had
been at 74 at 116. Still through `float` copies at the seam: the AHT's six-point inverse,
enhanced coupling's DFT and reconstruction (two bits of room kept above the coupling channel
for it), the spectral extension notch, and JOC's per-coefficient reads. Each is a bounded
piece of work with a measurement waiting for it; none is what the 100 dB target needed.

**Phase D - the part. Not started.** A C3 platform directory beside the S3's under the
bare-metal probe, the ESP-IDF component's profile extended with the scalar choice, the RISC-V
QEMU installed beside the Xtensa one, the probe run under it for correctness and, on a board,
for time. Exit: the fixtures' hashes under QEMU identical to the host's and the M3's; on a
board, the time per frame per fixture on the ESP32 page's terms. Verified by the same runner
that gates the S3, with the C3 as a third target. The runner already takes `--scalar=fixed`
and the minimum-footprint profile already honours it; what is missing is the target.

**Phase E (optional) - the encoder.** Not planned in this round. The encoder's analysis is
more precision-sensitive than the decoder's synthesis, and the S3's float encoder is the shape
a C3 encoder would take only after the decoder has shown what the fixed tier costs.

## Decisions

1. **Which fidelity the fixed tier promises.** Options: bit-exact to a spec-defined fixed-point
   reference (there is none in A/52), a stated SNR to the double decode, or "conforms to
   FFmpeg's fixed decoder". Recommended: a stated SNR, measured, with 100 dB as the target -
   it is the same kind of promise the float tier makes and the same tool measures it.
2. **Decoder first, encoder later or never.** Recommended: decoder first (Phase E optional).
   The C3 is a sink-class part; nothing on the platform matrix asks it to encode.
3. **Q-format.** Q7.24, constants included. The plan's alternative - a narrower format with
   block exponents everywhere - was half right: the format stayed, and Phase A's measurement
   put the exponent on the store anyway (see [The block exponent](#the-block-exponent)),
   because no width of absolute format keeps a small mantissa's bits. Decided by measurement.
4. **The soft-float proxy.** The Cortex-M3 leg is what every embedded estimate rests on; a
   RISC-V QEMU leg would be a second. Recommended: install `qemu-riscv32` through ESP-IDF's
   `idf_tools.py` (about 30 MB into `D:\esp\tools`) when Phase D starts, not before; the M3
   carries Phases A to C.
5. **A C3 board.** QEMU gives correctness and instruction counts, not the part's cache
   behaviour, and the C3 executes from flash through a 16 KB cache. Recommended: a board for
   Phase D; the S3 work showed QEMU's timing is not the board's.
6. **The effort axis's first level.** Recommended: `reduced` = delta bit allocation off, as a
   configuration field on both encoders with a CLI spelling, measured on the board and the
   gold gates before it is documented as a level.

## What cannot be verified, and why

- **The fixed tier's time on a C3** until there is a board; QEMU has no cache model. The M3
  instruction count is a proxy for the arithmetic, not the memory system.
- **The fixed tier's fidelity on material other than the sixteen streams measured.** The
  block exponent's bounds are stated in `block_norm.hpp` and each is on the side of never
  wrapping, but the SNR figures are those streams'; a stream whose tools push a channel to
  the format's edge would show as clipping at a conversion, not as wrapped arithmetic, and the
  probe's hashes would still agree across legs.
- **Exactness of a search-path change.** The rate-control predicate is not monotone, so no
  change to the probe sequence can be shown stream-preserving; it is shown quality-preserving
  by the gates and re-pinned. The exact changes on this branch are shown exact by their
  reference tests and by the hashes not moving.
- **The effort levels' quality on material other than the five gold streams.** The gates are
  what a laptop can hold in a minute; the ViSQOL trend in CI is the broader measure.
