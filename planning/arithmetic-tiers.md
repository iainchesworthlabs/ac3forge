# One implementation, three arithmetics, one effort axis

!!! note "Status as of 2026-09-10: the first axis measured, the second written down, the third proposed"
    Two of the three arithmetic tiers exist and are gated: `double`, the reference, in every
    ordinary build; `float`, the ESP32-S3's, for decode since 2026-09-09 and for encode since
    2026-09-10 (#617, #618). The effort axis has its first measured point: the search and the
    planner cost (this branch, on the ESP32-S3). The third tier - fixed point, for the ESP32-C3
    and every other part without a floating-point unit - is proposed here and not started:
    no `Fixed32` exists in the tree, no C3 target, and the RISC-V QEMU is not installed.

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
| Fixed point | `Fixed32` (proposed) | ESP32-C3/C6, Cortex-M0+/M3/M4 without FPU, any RV32IM | Decode: within a stated SNR of the double decode on every fixture (the number to be measured; 100 dB is the target, see [Phases](#phases)); **bit-identical output on every platform by construction**, integer arithmetic having no rounding-mode or library variation to differ by | The probe's PCM hashes, exact, on x86, Cortex-M3 and RISC-V; `check_decode_scalar_snr.py` against the double CLI with a fixed floor |

The fixed tier's guarantee is the strongest of the three - integer arithmetic is the same on
every machine - and its fidelity is the lowest. That is the trade the choice matrix below is
for.

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
| ESP32-C3 / C6 (RV32IMC, no FPU) | `Fixed32` | none at first | `reduced` | Proposed here |
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

`Fixed32`: a signed 32-bit integer in Q7.24 - seven bits of headroom above unity, twenty-four
below. Coefficients and time-domain samples are below unity in magnitude for a legal stream;
the headroom absorbs the intermediate growth of a transform stage and a downmix sum, and the
twenty-four fractional bits put the quantisation floor at -144 dB of full scale, so a 512-point
transform whose stages each lose half a bit still leaves the output above 110 dB of the double
decode. Products go through 64 bits (`mul`/`mulh` on RV32IM, `smull` on Cortex-M3) and shift
back; constants are Q2.30 so that a twiddle or a window coefficient of exactly 1.0 is
representable.

Two things the type does not try to be. It is not a general fixed-point library: only the
operations above exist, each with one rounding rule (round half up on the shift back), so the
result is defined by the source and not by a template's cleverness. And it is not
`std::floating_point`: the templates the decode path already has are written against a scalar
that behaves like a number, and where they call a `std::` function the call becomes an overload
set the way `scalar_math.hpp` already does for the float encode path - `scalar_sqrt`,
`scalar_exp2`, `scalar_log2`, `scalar_ldexp` - with the `double` and `float` overloads being
what they are today and the `Fixed32` ones integer routines.

### The transform

The FFT-based inverse is the one piece that is not a template instantiation with integer
operators substituted. An unscaled fixed-point FFT of 128 complex points can grow by seven bits
across its stages, which is exactly the headroom Q7.24 has and none to spare for the window and
the overlap-add after it. So the fixed transform scales: block floating point, one exponent per
block found by a count of leading zeros at each stage, the shift applied only when a stage would
overflow, and the block's exponent carried into the window and the overlap so that nothing is
lost that did not have to be. This is its own kernel beside the double and float ones, a
fixed-point transform source next to `mdct.cpp` with the same pre-twiddle, N/4-point FFT and
post-twiddle structure and the same tables in Q2.30.

The §3.5.5 DFT and the six-block DCT follow the same pattern and are smaller.

### Phases

**Phase A - the scalar and the straight-line path.** `Fixed32` with its operators and the
overload sets; the minimum-footprint decoder profile compiles with `-DAC3FORGE_DECODE_SCALAR=fixed`
on the host with the transform stubbed; dequantisation, the coupling reconstruction, the output
stage (folds, DRC, the int16 and float outputs) run in it. Exit: AC-3 mono and 2/0 fixtures
decode on the host through a reference (double) transform whose input and output are converted
at the seam, and the probe's levels are within the stated SNR of the double decoder's. Verified
by `check_decode_scalar_snr.py` with the fixed CLI, floor set from what is measured.

**Phase B - the transform.** `mdct_fixed.cpp` with block floating point; the window and overlap
in `Fixed32`. Exit: every AC-3 fixture and the plain E-AC-3 ones (2/0, 5.1, the folds) decode in
the fixed tier on the host and on the Cortex-M3 leg with identical PCM hashes, and the SNR to
the double decoder is at or above the Phase A floor. Verified by the probe on both legs and the
SNR tool; the M3 instruction count is the first cost figure, against the float tier's on the
same leg (12.9 M for E-AC-3 5.1).

**Phase C - the tools.** Spectral extension's noise and gains, enhanced coupling's DFT and
reconstruction, the adaptive hybrid transform's DCT and dequantisation, JOC's object
reconstruction if it fits the C3's memory. Exit: the remaining fixtures decode with identical
hashes on both legs at the same floor. The SNR target for the whole tier is 100 dB to the
double decode, and the number actually reached is what the documentation states.

**Phase D - the part.** A C3 platform directory beside the S3's under the bare-metal probe,
the ESP-IDF component's profile extended with the scalar choice, the RISC-V QEMU installed beside the Xtensa one, the probe run
under it for correctness and, on a board, for time. Exit: the fixtures' hashes under QEMU
identical to the host's and the M3's; on a board, the time per frame per fixture on the ESP32
page's terms. Verified by the same runner that gates the S3, with the C3 as a third target.

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
3. **Q-format.** Q7.24 with Q2.30 constants, as above; the alternative (Q1.31 with block
   exponents everywhere) buys four more bits at the cost of carrying an exponent through
   every buffer. Recommended: Q7.24, revisited only if Phase B's SNR falls short.
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
- **Exactness of a search-path change.** The rate-control predicate is not monotone, so no
  change to the probe sequence can be shown stream-preserving; it is shown quality-preserving
  by the gates and re-pinned. The exact changes on this branch are shown exact by their
  reference tests and by the hashes not moving.
- **The effort levels' quality on material other than the five gold streams.** The gates are
  what a laptop can hold in a minute; the ViSQOL trend in CI is the broader measure.
