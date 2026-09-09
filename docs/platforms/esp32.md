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
| Atmos bed | Correct, decoded bed-only via `DecoderConfig::skip_object_reconstruction`. 20 allocations per frame |
| Atmos objects | **Correct, reconstructed on target.** 31 allocations per frame — see [Objects](#objects) |
| Encode | AC-3 and E-AC-3, six frames of synthesised 5.1 through each encoder, byte count and FNV-1a hash checked against `apps/baremetal/encode_fixture.hpp` |
| Fits internal SRAM | Yes, without PSRAM. 210,203-byte peak heap against 280,792 free — see [Memory](#memory) |
| Retained after teardown | 12 bytes, one `__cxa_thread_atexit` record, the spectrum scratch's pointer; 23,552 bytes while §E3.5 is in use |
| Audio output | Two examples drive real peripherals — see [Examples](#examples) |
| Real time | **Yes, on a board**, at 240 MHz, every fixture: from 0.07x for AC-3 mono to 0.66x for Atmos objects — see [Timing](#timing) |
| ESPHome | An external component, `esphome/components/ac3forge/` — the decoder and framer, not a `speaker` source. See [ESPHome](#esphome) |
| CI | `build-esp32s3` in `.github/workflows/_build.yml` under QEMU; `esphome config` and the component pack in their own workflows |

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

`idf_component.yml` carries registry metadata and names `esp32s3` as its only target. **It is not
published to the ESP Component Registry.** `.github/workflows/esp-component.yml` lints the
manifest and packs the archive on every change, but its `compote component upload` job is gated to
a manual `workflow_dispatch` on a `v` tag — a published version cannot be replaced, so the upload
is a decision rather than a consequence of merging.

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

Measured, decode direction: the image uses 133,588 bytes of DIRAM and the decode peaks at
210,203 bytes of heap. The encode image is smaller, 110,900. `app_main` prints the runtime
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
players report per-frame timing of their own. A frame is 1,536 samples at 48 kHz, so the budget
is 32,000 microseconds and `realtime_permille` is 1000 at exactly real time.

Under `idf.py qemu` these figures do not describe hardware: QEMU is not cycle-accurate and
reports `cpu_mhz=40` against its own boot log's 160 MHz, so treat the QEMU leg as a correctness
and footprint check only; under the streaming player's `null` sink the figure means less again,
since nothing paces the loop. The figures that follow are from a board.

### Measured, on an ESP32-S3-DevKitC-1-N16R8

2026-09-09, chip revision v0.2, 240 MHz, PSRAM off, `-Os`. Every fixture
decoded to its expected levels - `result=pass`, every channel's RMS matching its
reference to the digit - so what follows is about speed alone.

As found, before any of the work below:

| Fixture | us/frame | x real time | |
|---|---:|---:|---|
| `ac3_mono` | 5,180 | 0.16 | fits |
| `ac3_stereo` | 14,290 | 0.45 | fits |
| `eac3_atmos_bed` | 27,197 | 0.85 | fits |
| `ac3` 5.1 | 32,149 | 1.00 | at the line |
| `eac3_stereo` | 34,739 | 1.08 | misses by 8% |
| `eac3` 5.1 | 78,824 | 2.46 | no |
| `eac3_atmos_objects` | 82,711 | 2.58 | no |
| `eac3_ecpl` | 217,493 | 6.80 | no |

The same build at 160 MHz - the clock this project inherited by never setting
`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ`, until 2026-09-09 - ran 1.44x to 1.49x slower
across all eight fixtures, against an ideal ratio of 1.50. That near-linear
scaling says the decode is compute-bound, not stalled on the flash cache, so
configuration had nothing further to give; anything more had to come out of the
code. (The `ac3` row is from the stage-timed run described next, whose markers
cost it about 0.2 ms; the other seven are from a plain build.)

### Where the time went

Nothing in the table says which stage is slow, and the estimates that had been
made about it were wrong. So before changing anything the decoder was
profiled on the board, through the `AC3_ZONE_SCOPED_N()` markers it already
carries for Tracy: `-DAC3FORGE_STAGE_TIMERS=ON` routes them to an accumulator
in the probe (`apps/baremetal/stage_timers.cpp`, the application half of
`src/forge/src/internal/profiling/stage_timers/`) and each fixture then prints a
`<fixture>.stage[<zone>]` line per stage with its inclusive and self time per
frame. A pair of markers costs 2.4 us on this part, and a frame passes through
at most 99 of them, so the breakdown carries under 0.25 ms of its own weight -
the stage-timed totals sit within 0.3% of the plain ones above.

Self time per frame, microseconds, as found:

| Stage | `eac3` 5.1 | `eac3_stereo` | `eac3_atmos_bed` | `ac3` 5.1 |
|---|---:|---:|---:|---:|
| spectral extension synthesis (`eac3_spx`) | 45,060 | 19,093 | - | - |
| AHT dequantisation and inverse (`eac3_aht`) | 20,723 | 8,489 | - | - |
| mantissa read and dequantisation | 1,194 | 1,596 | 14,006 | 14,980 |
| decoupling | - | - | - | 4,904 |
| bit allocation (`compute_bit_allocation`) | 3,704 | 1,416 | 4,559 | 4,350 |
| IMDCT and overlap-add | 3,403 | 1,228 | 3,401 | 4,847 |
| everything else | 4,983 | 3,082 | 5,591 | 3,043 |
| total | 79,067 | 34,904 | 27,557 | 32,124 |

The transform - the stage the earlier estimates had put first, and the one the
hand-written kernel tier would have targeted - is 4% of a 5.1 decode. Spectral
extension is 57%, and it is not arithmetic: an extension region is roughly
150 bins in each of five channels in each of six blocks, 4,500 bins a frame,
and they were costing 10 us each, 2,400 cycles a bin for a copy, a notch and a
two-term blend.

The cycles were going into the ROM. This part's FPU is single-precision, and
every `double` operation compiles to a call into the mask ROM's software
routines - `__muldf3`, `__divdf3` and the rest, some hundreds of cycles each,
with `__divdf3` the worst. The coefficient store had moved to `float` under
this profile (`decode_scalar_t`), but the arithmetic between the bitstream and
that store had not: the mantissa dequantiser divided in `double` and then
divided again by 2^exp, the dither and spectral-extension noise generators
mapped their state in `double`, the coupling coordinates came out of
`std::ldexp`, decoupling multiplied through `double`, the AHT's six-by-six
inverse ran thirty-six `double` multiply-adds per bin, spectral extension's blend
and band energies were `double` throughout, and JOC's object mixing summed
`double`s over a `float` state. A static census of the linked image found 1,197
call sites into those routines. None of it is visible on a desktop, where
`double` costs what `float` costs, or on the Cortex-M3 leg, where everything is
software floating point alike. It was visible only here.

### What changed

The arithmetic between bitstream and coefficient store now runs in the store's
own type, `ac3::internal::decode_scalar_t`: `float` under this profile, `double`
in every other build, which is why no gold reference, bitstream hash or test
moved (the full Windows suite passes as before, 1,345 tests). The exported
`double` functions - `dequantize_mantissa`, `decode_coordinate`,
`spx_noise_ratio`, `aht_dequantize_mantissa`, `DitherGenerator::next`,
`SpxNoise::next` - are now the `<double>` instantiations of templates the
decoders call at their own scalar; `spx_attenuation` became a 96-entry table
filled once from the same `std::exp2`; `aht_inverse` gained a `float` overload
over the double kernel narrowed once; and the division by 2^exp everywhere
became a multiply by a table of exact powers of two, which is the same value in
either type. JOC's mixing follows `decode_scalar_t` too, narrowing the
frame's matrix once rather than at every read, and the float analysis window
it runs thirty times a frame stopped narrowing its 512 constants per sample.

Where the float result is the double one narrowed, and where it is not, is
stated at each site. Mantissas, coordinates and decoupling are bit-identical
to before on this profile - a small integer over a power of two rounds the
same way in either type. Spectral extension, the AHT inverse, the noise
generators and the JOC sum round in `float` now, which is the same class of
difference the float store already accepted at the transform; the probe's
RMS figures, printed to six digits, did not move on any of the eight fixtures.

The second lever is the optimiser. This profile compiles at `-Os`, and a board
run with the decode-critical sources at `-O2` (`AC3FORGE_MINIMAL_HOT_O2`, on
for this project, off in the profile's default) showed which files it pays for
and which it does not: bit allocation 4.55 to 2.09 ms and the JOC mixing 11.0
to 8.2 ms per frame, against nothing at all for the float32 IMDCT, which ran
in 3.40 ms either way. The five files it pays for cost 39,192 bytes of
flash code and no internal SRAM; `.bss`, `.data` and the DIRAM figure the
runner gates are unchanged.

After both, plain build, same board, same clock:

| Fixture | us/frame | x real time | was |
|---|---:|---:|---:|
| `ac3_mono` | 2,446 | 0.08 | 0.16 |
| `ac3_stereo` | 4,157 | 0.13 | 0.45 |
| `eac3_stereo` | 6,803 | 0.21 | 1.08 |
| `ac3` 5.1 | 11,772 | 0.37 | 1.00 |
| `eac3_atmos_bed` | 13,409 | 0.42 | 0.85 |
| `eac3` 5.1 | 14,169 | 0.44 | 2.46 |
| `eac3_atmos_objects` | 29,359 | 0.92 | 2.58 |
| `eac3_ecpl` | 23,784 | 0.74 | 6.80 |

Every E-AC-3 configuration this profile decodes now runs in real time on this
part, with the Atmos objects fixture the closest to the line. Where a 5.1
frame's time goes now, stage-timed: bit allocation 1.66 ms, the IMDCT 3.38,
the AHT 2.98, spectral extension 1.49, mantissas 0.45, and 2.3 ms at the
access-unit level outside every marker.

Enhanced coupling came last, on its own, because its routines are shared with
the encoder and the first pass stopped at that boundary. It was the same
disease at a larger scale - of 202 ms, 107 were the per-channel reconstruction
and 83 `ecpl_channel_spectrum`, all `double`: three double inverse transforms
and a double 512-point DFT per block, then `std::cos` and `std::sin` per bin of
every coupled channel, each a software routine of a thousand cycles or so on
this FPU. The §3.5.5 routines now exist in both scalars, the double forms
being the encoder's and the exported ones as before; the float forms run the
float inverses and a float `dft512`, take their sine and cosine from a short
series held to libm at float precision (`tests/encoder/test_enhanced_coupling.cpp`
pins every float form against its double one), and write into the decoder's
store directly instead of round-tripping 512 conversions per channel per
block. Stage-timed, the spectrum is 6.9 ms a frame and the reconstruction
4.7; the fixture decodes in 23.8 ms. The float scratch is 23,552 bytes against
the double one's 32,768, and the bin-angle vector that used to be `thread_local`
is a stack array, so what stays retained after the probe hands the scratch
back is one registration record, 12 bytes, rather than two.

A third pass took the stages the profile left largest, every change of it
producing the values its predecessor produced: the host suite passes
unchanged and the probe's levels are to the digit. `BitReader::read()` had
been one loop iteration per bit - some eight cycles for each bit of every
mantissa, exponent group and GAQ codeword - and now serves a field from a
64-bit cache. A block whose exponents, allocation parameters and region are
its predecessor's keeps the allocation it already has rather than deriving it
again, which E-AC-3's once-a-frame exponents make the common case:
`compute_bit_allocation` ran 36 times a frame on the 5.1 stream and runs
6 now. The symmetric mantissa quantisers' values come from a table
filled at compile time by the division they used to perform, the asymmetric
ones scale by an exact power of two, and an AHT bin resolves its dequantiser's
constants once for its six codewords. JOC's mixing reads each (channel,
band)'s data points once per object per block and forms the ramp's fractions
once per block, and `fft.cpp` joined the `-O2` list once the float DFT was on
the hot path.

Same board, same clock, plain build:

| Fixture | us/frame | x real time | was |
|---|---:|---:|---:|
| `ac3_mono` | 2,188 | 0.07 | 2,588 |
| `ac3_stereo` | 3,420 | 0.11 | 4,236 |
| `eac3_stereo` | 6,171 | 0.19 | 6,814 |
| `ac3` 5.1 | 9,939 | 0.31 | 11,803 |
| `eac3_atmos_bed` | 10,914 | 0.34 | 13,416 |
| `eac3` 5.1 | 12,775 | 0.40 | 14,185 |
| `eac3_atmos_objects` | 23,191 | 0.72 | 29,337 |
| `eac3_ecpl` | 21,547 | 0.67 | 23,784 |

A 5.1 frame, stage-timed, now: bit allocation 0.4 ms (it was 1.6), the IMDCT
3.5, the AHT 2.7, spectral extension 1.5, mantissas 0.3. The access-unit
level, which the earlier profile could only report as 2.3 ms outside every
marker, is now four zones: 2.0 ms assembling the unit from its queued
substreams (`eac3_au_assemble`), 0.1 keying them (`eac3_au_key`), and
splitting and queueing under 0.05 between them. The assembly is the largest
cost the profile then named outside the decoders - at 36 KB of output PCM a
frame it was some 50 cycles a sample - and was the next thing read.

Reading it found a copy. `std::copy` of each channel's samples into the
caller's spans lowers to `memmove`, and on this part that is a mask-ROM
routine which measured some twelve cycles a byte, where the ROM's `memcpy` -
the call every fixed-size copy in the decoders already reaches - moves the
same 36 KB in 0.12 ms. The two ranges never overlap, so it is `memcpy` now,
and `eac3_au_assemble` is 0.25 ms, of which the copy (`eac3_au_pcm`) is
0.12. The same pass stopped copying a substream's object description into
the access unit - the substream is consumed there, so it is moved - which is
where the objects fixture's peak heap fell from 234,803 bytes to 210,203 and
its allocations a frame from 41 to 31, the bed's from 23 to 20. Every level
is unchanged to the digit, on the board and on the host suite.

Same board, same clock, plain build:

| Fixture | us/frame | x real time | was |
|---|---:|---:|---:|
| `ac3_mono` | 2,229 | 0.07 | 2,188 |
| `ac3_stereo` | 3,476 | 0.11 | 3,420 |
| `eac3_stereo` | 5,550 | 0.17 | 6,171 |
| `ac3` 5.1 | 9,963 | 0.31 | 9,939 |
| `eac3_atmos_bed` | 9,066 | 0.28 | 10,914 |
| `eac3` 5.1 | 10,988 | 0.34 | 12,775 |
| `eac3_atmos_objects` | 21,199 | 0.66 | 23,191 |
| `eac3_ecpl` | 19,768 | 0.62 | 21,547 |

A 5.1 frame, stage-timed, is 11.3 ms: the IMDCT 3.5, the AHT
2.7, spectral extension 1.5, bit allocation 0.4, mantissas 0.4,
and the access-unit level 0.4 in all.

### What is left, and what would move it

- **Objects.** JOC reconstruction is 12.3 ms of the objects fixture's 21.7:
  4.2 ms mixing, 3.5 ms re-analysing the bed with thirty forward transforms a
  frame, 2.8 ms synthesising six objects. The bed analysis exists because
  `oba::joc::reconstruct` takes the bed as PCM; the decoder holds that bed's
  MDCT coefficients already, one block at a time, and a reconstruction that
  took them would skip the analysis outright. Beyond that, this is the one
  place the second core is worth its complexity: JOC for frame N is
  independent of the bed decode of frame N+1, so a second task can run it a
  frame behind, at the cost of one frame of latency, and throughput becomes
  the larger of the two halves rather than their sum. Neither is done.
- **Enhanced coupling** is at 0.62x and has one cheap step left. Each block's
  spectrum runs three inverse transforms, and two of them are the neighbouring
  blocks' - the same transforms the previous and next block run for
  themselves, so eighteen a frame where eight are distinct; a cache keyed by
  block would take about a millisecond off the 6.7 the spectrum costs now.
  `fft.cpp` joined the `-O2` list in the third pass and was worth 0.1 ms:
  `ecpl_channel_spectrum` went from 6.9 ms to 6.7 and the reconstruction
  stayed at 4.7. The pass's gain on this fixture came from the bitstream
  side instead - its mantissas 2.4 ms to 1.6.
- **The second core** was the lever the earlier estimates ranked first. It was
  not needed for stereo or 5.1, and the breakdown says why it would have
  disappointed: the stages that dominated were serial software floating point,
  not parallel work, and splitting them across two cores would have halved a
  cost that could be removed instead.
- **Per-frame allocations** are unchanged at 3 to 41 per frame (this profile's
  open PF7 gap). At a few microseconds each they are not on the path to real
  time for any fixture here; the coupling-coordinate vector that allocated once
  per coupled channel per block is gone as a side effect, but the count the
  runner gates did not move on any fixture, since no fixture couples.
- **A hand-written kernel tier** (`madd.s`, which `-ffp-contract=off` forbids
  project-wide) would apply to the IMDCT, which is 3.5 ms of a 11.3 ms 5.1
  frame. That bounds what the tier could return at under a quarter of the
  remaining time, and it is not needed for anything that now fits.

### Running it yourself

    idf.py -p <PORT> flash monitor

with `SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.hw"` if the board is
reached through its native USB connector rather than the UART bridge - see
`sdkconfig.hw` for what that changes and why. Add `-DAC3FORGE_STAGE_TIMERS=ON`
to the build for the per-stage lines.

On a DevKitC-1 the host cannot reset the part into the application over
USB-Serial-JTAG: both `esptool` and `idf.py monitor` assert IO0 during their
reset sequence, so every host-initiated reset lands in `boot:0x0 (DOWNLOAD)` and
the application never starts. Attach with `idf.py monitor --no-reset` and press
the board's RESET button instead. Flashing over the same connector is
unaffected. The probe pauses three seconds before its first line so the
re-enumeration that follows a reset does not swallow the first fixture's
output, which it otherwise does.

One trap for a machine that builds both shapes: ESP-IDF keeps `sdkconfig` in
the PROJECT directory, shared by every `-B` build directory, and regenerates it
from `SDKCONFIG_DEFAULTS` only when it is absent. A QEMU build made after a
hardware build therefore inherits `sdkconfig.hw`'s USB console and prints
nothing under QEMU, which has no such device. Give each shape its own
`-DSDKCONFIG=<build dir>/sdkconfig`, or delete `sdkconfig` between them.

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
Retained at exit went from 34,232 bytes to 24, and to 12 once the bin-angle vector became a
stack array and the scratch took its float form, 23,552 bytes.

**The bed does not need any of this.** An Atmos bed is ordinary E-AC-3 5.1 and the objects are
side data, so `DecoderConfig::skip_object_reconstruction` decodes the bed without allocating
`ReconstructionState` at all — 20 allocations per frame against 31. `tests/oba/test_atmos.cpp`
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
are independent CMake axes. Since 2026-09-09 the arithmetic between the bitstream and those
buffers — mantissa dequantisation, dither, coordinates, decoupling, spectral extension, the AHT
and JOC's mixing — follows the same scalar; it had stayed `double`, which on this FPU is
software, and [Timing](#timing) has what that cost.

## Open work

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

`esphome/components/ac3forge/` is an ESPHome external component. It is the plumbing:
`Ac3ForgeComponent` owns an `ac3::FrameDecoder` and an `ac3::io::AccessUnitAccumulator`, takes
bytes and hands back planar float PCM. It is **not** a `media_player` or a `speaker` source —
ESPHome's `speaker` platform is ESP-IDF-only, so that is the obvious next step rather than a
blocked one.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/iainchesworthlabs/ac3forge
      ref: main
      path: esphome/components
    components: [ac3forge]

esp32:
  board: esp32-s3-devkitc-1
  framework:
    type: esp-idf

ac3forge:
  version: v0.10.0-beta.1   # a git ref of ac3forge itself
  buffer_size: 16384
```

Two refs are in play: `external_components`' `ref` picks the version of the ESPHome component,
and `ac3forge:`'s `version:` picks the version of the library it fetches. Pin both for anything
meant to keep working.

`buffer_size` is the framer's working buffer, floored at 4,160 bytes — one syncframe plus the
next header, which is what deciding where an access unit ends requires. 16 KB holds an independent
substream plus three dependents, which covers Atmos.

The component reaches the library by git reference rather than the registry:
`add_idf_component` writes `git:`, `version:` and `path:` into the generated
`idf_component.yml`, which is the form the IDF component manager wants for a component in a
subdirectory. Nothing here is blocked on [publishing](#the-esp-idf-component).

CI runs `esphome config` over `esphome/tests/ac3forge-test.yaml` against a local source pointing
at the working tree, which exercises the schema and `to_code` including the `add_idf_component`
call, and asserts that a `buffer_size` no access unit fits in is rejected. It does **not** compile
the firmware: that would clone ac3forge at the configured ref and build the whole IDF project,
which says nothing about the code under review, since the ref it fetched is not that code.

[`esphome/README.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/esphome/README.md)
has the rest, including why PSRAM is worth having on a board that also runs WiFi.
