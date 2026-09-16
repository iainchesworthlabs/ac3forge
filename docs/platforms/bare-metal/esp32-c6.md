# ESP32-C6

The minimum-footprint decoder on an Espressif ESP32-C6: one 160 MHz RISC-V core (RV32IMAC) with
no floating-point unit, 512 KB of SRAM shared with WiFi 6, Bluetooth LE and 802.15.4, and no
external PSRAM, which ESP-IDF does not support on this part. It decodes in the fixed-point tier,
as the [ESP32-C3](esp32-c3.md) does, from the same `esp-idf/ac3forge/` component, whose manifest
lists `esp32c6` beside `esp32s3` and `esp32c3`.

Every figure on this page was measured on a board on 2026-09-15, with no network and again with
WiFi connected and a TCP stream arriving while the decoder ran.

## Status

| | |
|---|---|
| Decode | Correct: all fourteen fixtures, every channel's level within the probe's tolerance, and every fixed-tier PCM hash equal to the values the x86-64 host, the Cortex-M3 leg and the ESP32-C3 leg are held to (`tests/golden/fixed-probe-pcm-hashes.json`). A fourth architecture on one set of hashes |
| Real time, no network | AC-3 5.1 (0.64x), E-AC-3 5.1 with AHT, spectral extension and coupling (0.75x), an Atmos stream's bed (0.59x), E-AC-3 7.1 (0.87x), and every stereo and mono row. Not the folds to stereo (1.03x and 1.62x), E-AC-3 line mode (1.15x), enhanced coupling (1.71x), 7.1.4 (1.88x) or the Atmos objects rows |
| Real time, with WiFi and a stream | AC-3 5.1 (0.82x), E-AC-3 5.1 with AHT, spectral extension and coupling (0.96x), the Atmos bed (0.71x), a 192 kbit/s E-AC-3 5.1 stream (0.64x), and stereo and mono (0.17x to 0.41x). Not E-AC-3 7.1 (1.09x), the folds, line mode, enhanced coupling or the Atmos objects rows |
| Memory, no network | Every fixture fits: 383,416 bytes free before the decode, largest block 352,256, against a largest peak of 234,070 (7.1.4 folded to stereo) |
| Memory, with WiFi and a stream | About 236,000 bytes free before the decode, largest block 217,088 to 221,184. Everything up to the Atmos objects rows (212,253 bytes of peak) fits, and leaves 18,152 bytes free at the lowest; 7.1.4 (227,662) does not. With ESP-IDF's WiFi IRAM options off every fixture fits, 7.1.4 included, and the decode is slower |
| Encode | Not measured. Both encoders are floating-point, which on this part is software floating point |
| QEMU | Not emulated, see [QEMU](#qemu) |
| CI | The component pack builds for `esp32c6` from its archive, and the `build-esp32c3` job builds this probe with both network loads. Nothing runs |

## What the part carries

With WiFi connected and a 1,536 kbit/s TCP stream arriving, in the fixed-point tier:

| Stream | Decodes in real time | Fits in memory |
|---|---|---|
| AC-3 2/0 and 1/0 | Yes: 0.28x and 0.17x | Yes: 49,328 and 47,608 bytes of peak |
| E-AC-3 2/0 | Yes: 0.41x | Yes: 108,094 |
| AC-3 5.1 | Yes: 0.82x. Folded to stereo, no: 1.25x | Yes: 56,685 |
| E-AC-3 5.1 | Yes: 0.96x with AHT, spectral extension and coupling, 0.71x for an Atmos stream's bed and 0.64x for a 192 kbit/s stream. No with enhanced coupling (2.09x), in line mode (1.38x) or folded to stereo (1.92x) | Yes: 164,066, 121,139 and 120,978 |
| E-AC-3 7.1 | No: 1.09x for a 288 kbit/s stream with no Annex E tools | Yes: 152,926 |
| E-AC-3 7.1.4 | No: 1.88x with no network | Not with ESP-IDF's default WiFi configuration: 227,662 bytes, where an earlier run that tried it had a largest free block of 217,088. Yes with WiFi's code kept in flash, [below](#wifis-code-kept-in-flash) |
| Atmos objects, reconstructed or placed onto 7.1.4 | No: 6.52x and 6.71x | Yes, 18,152 bytes left at the lowest |

In the float tier, with the network up, mono is the only row in real time (0.52x).

### 2/0, 5.1 and 7.1 from one generator

The probe has no 7.1 fixture, so the stream set's three layout streams,
`esp-idf/ac3forge/examples/hearth_sink/www/layout-20.ec3`, `layout-51.ec3` and `layout-71.ec3`,
were decoded on the same board by a copy of the probe with them added as rows. They come from one
generator, use no Annex E tools and hold 32 access units each; 2/0 and 5.1 are 192 kbit/s, and
7.1, a 5.1 substream and a dependent one, is 288 kbit/s. That copy measured time and heap, and did
not check levels. Fixed-point tier; the WiFi column is the slower of two runs.

| Stream | No network | WiFi and a stream | Peak heap |
|---|---:|---:|---:|
| `layout-20.ec3` 2/0 | 7,487 (0.23x) | 9,872 (0.31x) | 93,270 |
| `layout-51.ec3` 5.1 | 16,492 (0.52x) | 20,322 (0.64x) | 120,978 |
| `layout-71.ec3` 7.1 | 27,816 (0.87x) | 34,755 (1.09x) | 152,926 |

## Measured, on a board

An ESP32-C6 in the QFN40 package, chip revision v0.2, with 16 MB of flash read in DIO mode at
80 MHz, reached over its USB-Serial/JTAG port. ESP-IDF v6.1 and GCC esp-15.2.0_20251204, `-Os`
with the decode-critical sources at `-O2` (`AC3FORGE_MINIMAL_HOT_O2`, the project's default),
the task watchdog off, and 160 MHz as the probe measures it against `esp_timer`.

The probe decodes six frames of each fixture. Its timing is the decoder's own: the level and hash
accumulation it runs inside the decoder's block callback is timed and subtracted.

### Decode time

Microseconds per frame, and that as a fraction of the 32,000 microseconds a frame lasts. With no
network, two runs of the fixed-point build agreed within 3 microseconds on every row. With WiFi,
three runs of the same build varied by up to 16% on the stereo and mono rows and 12% on the
others, and each WiFi cell is the slowest run.

| Fixture | Fixed | Fixed, WiFi | Float | Float, WiFi |
|---|---:|---:|---:|---:|
| `ac3` 5.1 | 20,500 (0.64x) | 26,164 (0.82x) | 83,324 (2.60x) | 100,056 (3.13x) |
| `ac3_fold` 5.1 to Lo/Ro | 33,002 (1.03x) | 40,100 (1.25x) | 87,152 (2.72x) | 104,083 (3.25x) |
| `ac3_stereo` 2/0 | 6,436 (0.20x) | 8,822 (0.28x) | 28,631 (0.89x) | 34,454 (1.08x) |
| `ac3_mono` 1/0 | 3,486 (0.11x) | 5,332 (0.17x) | 13,389 (0.42x) | 16,518 (0.52x) |
| `eac3` 5.1, AHT, spectral extension, coupling | 23,941 (0.75x) | 30,613 (0.96x) | 105,618 (3.30x) | 122,993 (3.84x) |
| `eac3_ecpl` 5.1, enhanced coupling | 54,629 (1.71x) | 67,036 (2.09x) | 231,125 (7.22x) | 270,525 (8.45x) |
| `eac3_atmos_bed` 5.1 bed | 18,970 (0.59x) | 22,674 (0.71x) | 73,246 (2.29x) | 86,377 (2.70x) |
| `eac3_atmos_objects` | 178,686 (5.58x) | 208,748 (6.52x) | 223,255 (6.98x) | 258,915 (8.09x) |
| `eac3_stereo` 2/0 | 10,193 (0.32x) | 13,133 (0.41x) | 40,417 (1.26x) | 48,418 (1.51x) |
| `eac3_714` 7.1.4 | 60,240 (1.88x) | not run | 275,780 (8.62x) | not run |
| `eac3_fold` 5.1 to Lo/Ro | 51,902 (1.62x) | 61,512 (1.92x) | 117,682 (3.68x) | 136,404 (4.26x) |
| `eac3_714_fold` 7.1.4 to Lo/Ro | 103,183 (3.22x) | not run | 294,249 (9.20x) | not run |
| `eac3_line` 5.1, line mode | 36,650 (1.15x) | 44,294 (1.38x) | 111,316 (3.48x) | 131,286 (4.10x) |
| `eac3_atmos_render` onto 7.1.4 | 184,477 (5.76x) | 214,565 (6.71x) | 231,213 (7.23x) | 269,475 (8.42x) |

"Not run" is the WiFi build's heap budget (`AC3FORGE_PROBE_HEAP_BUDGET_BYTES`, 215,000) skipping
a fixture: `eac3_714` ran out of heap on a 1,692-byte request in the first WiFi run made without
one.

The fixed-point tier is 2.3x to 4.6x faster than float here, except on the two rows that
reconstruct objects (1.25x), where JOC's transform is float in every build. With the network up
the fixed-point decode takes 16% to 53% longer, the smallest fixtures the most.

### Where the time goes

Self time per frame in microseconds, fixed-point tier, no network, from a build with
`-DAC3FORGE_STAGE_TIMERS=ON`. A pair of stage markers costs 3.0 microseconds on this part.

| Stage | `ac3` 5.1 | `ac3_stereo` | `eac3` 5.1 | `eac3_atmos_bed` | `eac3_stereo` |
|---|---:|---:|---:|---:|---:|
| IMDCT and overlap-add | 13,398 | 3,846 | 11,553 | 11,599 | 3,943 |
| AHT dequantisation and inverse | - | - | 4,434 | - | 1,680 |
| spectral extension | - | - | 4,096 | - | 1,641 |
| mantissa read and dequantisation | 2,550 | 1,272 | 500 | 2,916 | 415 |
| decoupling | 1,174 | 7 | - | - | - |
| bit allocation | 1,073 | 435 | 766 | 907 | 448 |
| exponents | 363 | 221 | 337 | 510 | 172 |
| everything else | 2,254 | 983 | 2,628 | 3,259 | 2,231 |
| total | 20,812 | 6,764 | 24,314 | 19,191 | 10,530 |

The transform and overlap-add are 64% of an AC-3 5.1 frame, 48% of E-AC-3 5.1 and 60% of the
Atmos bed. An AC-3 5.1 frame is 36 blocks of transform and overlap-add, six channels in each of
six blocks, about 370 microseconds each here. On the [ESP32-S3](esp32-s3.md#where-the-time-went),
in float with its FPU at 240 MHz, the same stage measured 4.8 ms of an AC-3 5.1 frame and 3.4 ms
of an E-AC-3 one.

### The fixed-point arithmetic on this core

Before the changes below, the same stage was 25,326 of 34,490 microseconds of an AC-3 5.1 frame,
about 700 a block. Disassembled (`riscv32-esp-elf-objdump` on the `-O2` objects), each of the
2,436 Q7.24 products in a long block's IMDCT was the two halves of a 64-bit multiply (`mul` and
`mulh`), a carry for the rounding and a shift, then four branches on the high word for
`Fixed32`'s saturation, two of them taken on every product. The overlap-add after it converted
each sample with the ROM's `__floatsisf`, scaled it with `__mulsf3`, and shifted on 64 bits
through `__ashldi3` and `__ashrdi3` whenever the two halves' exponents differed. The E-AC-3 tools
added a 64-bit division (`__divdi3`) for each AHT mantissa and about six (`__udivdi3`) for each
square root of a spectral extension band.

Each change below leaves the fixed-point tier's PCM identical on every fixture: the pinned hashes
(`tests/golden/fixed-probe-pcm-hashes.json`) do not move, and `tests/core/test_fixed32.cpp`,
`tests/core/test_mdct_fixed.cpp` and `tests/decoder/test_block_norm.cpp` hold each new form to
the arithmetic it replaces. Microseconds per frame, no network, two runs of each build, which
agreed within 3 microseconds; each row includes the ones above it:

| Change | `ac3` 5.1 | `eac3` 5.1 |
|---|---:|---:|
| None: the starting build | 34,619 | 38,673 |
| The IMDCT pair's products without the saturation, which a value below 90.5 times a factor of at most one cannot reach: seven instructions and no branch (`mdct_fixed.hpp`) | 27,981 | 32,331 |
| The overlap-add on 32 bits, with the power of two applied to the converted sample's exponent field (`block_norm.hpp`) | 24,382 | 28,847 |
| The transform's functions placed in IRAM by a linker fragment | 24,088 | 28,894 |
| The output float built from the integer's bits instead of by `__floatsisf` | 22,102 | 27,505 |
| `Fixed32`'s product testing its saturation once, marked likely: ten instructions and a branch not taken (`fixed32.hpp`) | 21,713 | 26,257 |
| `Fixed32`'s power-of-two and integer scaling on 32 bits | 20,491 | 25,658 |
| The IRAM fragment taken out again | 20,531 | 25,838 |
| The AHT inverse's and the spectral extension blend's products without the saturation, and ratios of small integers in two 32-bit divisions | 20,495 | 24,237 |
| The integer square root started from the root of its top 32 bits | 20,499 | 23,941 |

The same source as the starting build, built in another tree, measured 34,725 and 38,681: code
placement alone moves a build by about 100 microseconds. The IRAM placement bought 294
microseconds of AC-3 5.1 and none of E-AC-3 when it went in, 40 and 180 when it came out, and
costs 5,056 bytes of heap at boot, so the probe does not carry it. A product taken from the high
word of a 32x32 multiply alone would lose the rounding bit in the low word and change the
output, so every product keeps both halves.

### Memory

Bytes. The WiFi column's ranges span three runs of the fixed-point build.

| | No network | WiFi and a stream |
|---|---:|---:|
| Free at boot | 383,416 | 295,104 |
| Free after joining the network | - | 241,432 to 241,636 |
| Free before the decode, the stream arriving | 383,416 | 236,080 to 236,580 |
| Largest block before the decode | 352,256 | 217,088 to 221,184 |
| Lowest free during the run | 145,364 | 18,152 to 19,084 |
| Main task stack left of 32,768 | 10,520 | 10,512 |

The WiFi image leaves 88,312 fewer bytes free at boot, before WiFi starts: `idf.py size` puts
59,728 more bytes of code in RAM, where ESP-IDF's defaults place parts of the WiFi and PHY code,
and 28,509 more of data and bss. Joining the network and taking an address uses about 53,500
more, and the receiving task's 4,096-byte stack, its socket and the first segments about 5,000 to
7,000.

Peak heap per fixture, the same with and without the network, since the decoder allocates the
same bytes whatever else runs:

| Fixture | Fixed | Float |
|---|---:|---:|
| `ac3` 5.1 | 56,685 | 56,421 |
| `ac3_fold` 5.1 to Lo/Ro | 58,733 | 58,469 |
| `ac3_stereo` 2/0 | 49,328 | 49,304 |
| `ac3_mono` 1/0 | 47,608 | 47,596 |
| `eac3` 5.1, AHT, spectral extension, coupling | 164,066 | 156,602 |
| `eac3_ecpl` 5.1, enhanced coupling | 153,593 | 153,113 |
| `eac3_atmos_bed` 5.1 bed | 121,139 | 120,659 |
| `eac3_atmos_objects` | 211,883 | 211,403 |
| `eac3_stereo` 2/0 | 108,094 | 105,518 |
| `eac3_714` 7.1.4 | 227,662 | 220,366 |
| `eac3_fold` 5.1 to Lo/Ro | 170,422 | 162,958 |
| `eac3_714_fold` 7.1.4 to Lo/Ro | 234,070 | 226,774 |
| `eac3_line` 5.1, line mode | 164,142 | 156,678 |
| `eac3_atmos_render` onto 7.1.4 | 212,253 | 211,773 |

### The network load

`CONFIG_AC3FORGE_PROBE_NETWORK_WIFI` (`apps/baremetal/platform/esp32c6/main/net/wifi/`) joins the
access point, turns modem sleep off, listens on TCP port 4953 and reads what arrives from a task
at priority 5: above the decode on the main task at priority 1, below WiFi and lwIP. That is the
load a Sendspin player carries, since the Sendspin server connects to the player. The probe
starts once 32,768 bytes have arrived, and the bytes are counted and dropped.

A host sent 1,536 kbit/s in 32 ms chunks, the rate of 48 kHz 16-bit stereo PCM, cycling the bytes
of `apps/wasm/assets/demo.ec3`. During the decode the part received 1,500 to 1,537 kbit/s, and
the longest gap between two reads was 81 to 114 ms. The access point negotiated 802.11n on
channel 1 at -61 dBm, so the part's WiFi 6 was not exercised.

### WiFi's code kept in flash

ESP-IDF's defaults put parts of the WiFi and PHY code in RAM (`CONFIG_ESP_WIFI_IRAM_OPT`,
`CONFIG_ESP_WIFI_EXTRA_IRAM_OPT`, `CONFIG_ESP_WIFI_RX_IRAM_OPT`, `CONFIG_ESP_WIFI_SLP_IRAM_OPT`
and `CONFIG_ESP_PHY_IRAM_OPT`). With all five off, the same fixed-point WiFi build was run twice
with no heap budget:

| | Defaults | IRAM options off |
|---|---:|---:|
| Code in RAM (`idf.py size`) | 93,190 | 44,804 |
| Free at boot | 295,104 | 343,968 |
| Free before the decode | 236,080 to 236,580 | 285,408 to 285,428 |
| Largest block before the decode | 217,088 to 221,184 | 262,144 to 270,336 |
| Lowest free during the run | 18,152 to 19,084 | 37,528 to 38,588 |
| `eac3_714` 7.1.4 | out of heap | 95,547 (2.99x) |
| `eac3_714_fold` 7.1.4 to Lo/Ro | not run | 155,716 (4.87x) |
| `ac3` 5.1 | 26,164 (0.82x) | 34,871 (1.09x) |
| `eac3` 5.1 | 30,613 (0.96x) | 39,716 (1.24x) |
| `eac3_atmos_bed` 5.1 bed | 22,674 (0.71x) | 33,526 (1.05x) |
| `ac3_stereo` 2/0 | 8,822 (0.28x) | 14,978 (0.47x) |
| `eac3_stereo` 2/0 | 13,133 (0.41x) | 17,444 (0.55x) |
| `ac3_mono` 1/0 | 5,332 (0.17x) | 6,555 (0.20x) |

Every fixture fits with the options off, and the stream still arrived at 1,536 and 1,537 kbit/s.
The WiFi code then runs from flash, as the decoder's does. The decode is slower and varies more
between runs, up to 41% on the stereo and mono rows and 19% on the others; each time is the
slowest run. Stereo and mono stay in real time either way, and 5.1 does not with the options
off.

## I2S

The part has one I2S controller. ESP-IDF v6.1 holds a TDM slot configuration to
`I2S_LL_SLOT_FRAME_BIT_MAX`, 128 bits a frame on this part as on the ESP32-S3
(`components/esp_driver_i2s/i2s_tdm.c`), so one line carries eight 16-bit slots or four 32-bit
ones and nothing wider. On the board, a scratch application opened a master TDM channel for
every slot count from 2 to 16 at 16, 24 and 32 bits and wrote a second of 48 kHz frames to each
one the driver accepted, timing how long the writes took to drain:

| Slot width | Accepted | Drained a second of frames in 999 ms | 937 ms |
|---|---|---|---|
| 16 bits | 2 to 8 slots; 9 to 16 refused (`ESP_ERR_INVALID_ARG`) | 2, 4, 6, 7 and 8 slots | 3 and 5 slots |
| 24 bits | 2 to 5 slots; 6 to 16 refused | 2 and 4 slots | 3 and 5 slots |
| 32 bits | 2 to 4 slots; 5 to 16 refused | 2, 3 and 4 slots | - |

The 937 ms shapes play 6.7% fast. The driver's clock setup (`i2s_tdm_calculate_clock` in the same
file) divides the MCLK by the bit clock in integers and only logs a warning when it does not
divide: with the default MCLK of 256 times the sample rate, three 16-bit slots need 5.33 and get
5, and the frame runs at 51,200 Hz. A divider of 2 or less is raised to 3 with the MCLK adjusted
to match, which is why six, seven and eight slots come out exact. So a TDM line on this part runs
correctly at the full 128-bit frame, eight 16-bit slots or four 32-bit ones, with the slots a
layout leaves unused written as zeros. Nothing was connected to the pins; a DAC on the line is
not part of this measurement.

Getting samples onto those slots costs time here too. The decoder hands a sink planar `float`
blocks, and the component's conversions (`esp-idf/ac3forge/include/ac3forge/interleave.hpp`)
scale, clip and convert each sample in `float`, which on a part with no FPU is a call into the
software floating-point routines for each operation. Timed on the board for one frame, six
256-sample blocks, from a build at `-Os`:

| Work per frame | Microseconds |
|---|---:|
| `to_pcm16` into eight interleaved 16-bit slots | 11,744 |
| `to_slot_24in32` into four interleaved 32-bit slots (`interleave_24in32`) | 5,159 |
| `to_pcm16` into a stereo pair (`interleave_16`) | 2,879 |
| A float level meter over eight slots, squares summed sixteen at a time | 9,621 |

About 0.9 microseconds a sample for the conversions and 0.8 for the meter, so eight 16-bit slots
and a level meter on them take two thirds of a frame before the decode is counted.

## QEMU

ESP-IDF v6.1's `qemu-system-riscv32` (esp_develop_9.2.2_20260417) emulates one Espressif machine,
`esp32c3`, and `idf.py qemu` refuses this target with "QEMU is not supported for target
esp32c6". There is no emulated leg for the part: CI builds it, and every figure here comes from
the board.

## Building

`apps/baremetal/platform/esp32c6/` is the probe target:

```bash
. $IDF_PATH/export.sh
cd apps/baremetal/platform/esp32c6
idf.py set-target esp32c6
idf.py build                                  # fixed-point tier, no network
idf.py -DAC3FORGE_DECODE_SCALAR=float build   # the float tier
idf.py -p <PORT> flash monitor
```

The WiFi load is `sdkconfig.wifi`, with the credentials (`CONFIG_AC3FORGE_PROBE_WIFI_SSID` and
`CONFIG_AC3FORGE_PROBE_WIFI_PASSWORD`) in a file of their own outside the repository:

```bash
idf.py -B build-wifi -DSDKCONFIG=build-wifi/sdkconfig \
  "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.wifi;/path/to/credentials" build
```

It prints `net.ip=` once it has an address and waits up to two minutes for a stream. Anything that
connects and sends will do; the figures above came from a sender paced like this one:

```python
import socket, sys, time

host, port, kbit, path = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
payload = open(path, "rb").read()
chunk = kbit * 1000 // 8 * 32 // 1000  # bytes per 32 ms
with socket.create_connection((host, port)) as s:
    offset, due = 0, time.perf_counter()
    while True:
        data = (payload[offset:] + payload)[:chunk]
        offset = (offset + chunk) % len(payload)
        s.sendall(data)
        due += 0.032
        time.sleep(max(0.0, due - time.perf_counter()))
```

## What was not measured

- Levels for the three stream-set rows, which ran in a copy of the probe with no reference
  levels for them.
- Playback: the probe decodes six frames a fixture into no output. I2S, a DMA queue and
  underruns belong to the sink.
- WiFi 6, stream rates other than 1,536 kbit/s, and modem sleep on.
- The float tier with WiFi's code kept in flash.
- Encode.
- A second board.

## Where to go next

- [ESP32-C3](esp32-c3.md): the same tier under `qemu-riscv32` in CI, and why a part with no FPU
  wants it.
- [ESP32-S3](esp32-s3.md): the part with an FPU, where every fixture decodes in real time.
- [Bare metal overview](index.md): how the pages in this section relate.
