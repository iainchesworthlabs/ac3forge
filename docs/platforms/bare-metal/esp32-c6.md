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
| Real time, no network | AC-3 stereo and mono, E-AC-3 stereo, and E-AC-3 5.1 without AHT or spectral extension: 0.93x for an Atmos stream's bed, 0.86x for a 192 kbit/s stream. AC-3 5.1 takes 1.09x a frame, E-AC-3 5.1 with AHT, spectral extension and coupling 1.21x, E-AC-3 7.1 1.45x |
| Real time, with WiFi and a stream | The stereo and mono rows only, 0.26x to 0.64x. No 5.1 stream: 1.07x to 1.45x. E-AC-3 7.1: 1.79x |
| Memory, no network | Every fixture fits: 383,416 bytes free before the decode, largest block 352,256, against a largest peak of 234,070 (7.1.4 folded to stereo) |
| Memory, with WiFi and a stream | About 235,000 bytes free before the decode, largest block 217,088 to 221,184. Everything up to the Atmos objects rows (212,253 bytes of peak) fits, and leaves 17,920 bytes free at the lowest; 7.1.4 (227,662) runs out of heap. With ESP-IDF's WiFi IRAM options off every fixture fits, 7.1.4 included, and 5.1 decodes about a fifth slower |
| Encode | Not measured. Both encoders are floating-point, which on this part is software floating point |
| QEMU | Not emulated, see [QEMU](#qemu) |
| CI | The component pack builds for `esp32c6` from its archive, and the `build-esp32c3` job builds this probe with both network loads. Nothing runs |

## What the part carries

With WiFi connected and a 1,536 kbit/s TCP stream arriving, in the fixed-point tier:

| Stream | Decodes in real time | Fits in memory |
|---|---|---|
| AC-3 2/0 and 1/0 | Yes: 0.49x and 0.26x | Yes: 49,328 and 47,608 bytes of peak |
| E-AC-3 2/0 | Yes: 0.64x | Yes: 108,094 |
| AC-3 5.1 | No: 1.39x, and 1.87x folded to stereo | Yes: 56,685 |
| E-AC-3 5.1 | No: 1.45x with AHT, spectral extension and coupling; 1.15x for an Atmos stream's bed and 1.07x for a 192 kbit/s stream, neither using AHT or spectral extension | Yes: 164,066, 121,139 and 120,978 |
| E-AC-3 7.1 | No: 1.79x for a 288 kbit/s stream with no Annex E tools | Yes: 152,926 |
| E-AC-3 7.1.4 | No: 3.03x with no network | Not with ESP-IDF's default WiFi configuration: 227,662 bytes, where the run that tried it had a largest free block of 217,088. Yes with WiFi's code kept in flash, [below](#wifis-code-kept-in-flash) |
| Atmos objects, reconstructed or placed onto 7.1.4 | No: 6.97x and 7.16x | Yes, 17,920 bytes left at the lowest |

In the float tier, with the network up, mono is the only row in real time (0.53x).

### 2/0, 5.1 and 7.1 from one generator

The probe has no 7.1 fixture, so the stream set's three layout streams,
`esp-idf/ac3forge/examples/stream_player/www/layout-20.ec3`, `layout-51.ec3` and `layout-71.ec3`,
were decoded on the same board by a copy of the probe with them added as rows. They come from one
generator, use no Annex E tools and hold 32 access units each; 2/0 and 5.1 are 192 kbit/s, and
7.1, a 5.1 substream and a dependent one, is 288 kbit/s. That copy measured time and heap, and did
not check levels. Fixed-point tier; the WiFi column is the slower of two runs.

| Stream | No network | WiFi and a stream | Peak heap |
|---|---:|---:|---:|
| `layout-20.ec3` 2/0 | 11,553 (0.36x) | 14,885 (0.47x) | 93,270 |
| `layout-51.ec3` 5.1 | 27,643 (0.86x) | 34,144 (1.07x) | 120,978 |
| `layout-71.ec3` 7.1 | 46,318 (1.45x) | 57,201 (1.79x) | 152,926 |

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
the same build's times varied between runs by up to 16% on the stereo and mono rows and 4% on the
others, and each WiFi cell is the slowest run.

| Fixture | Fixed | Fixed, WiFi | Float | Float, WiFi |
|---|---:|---:|---:|---:|
| `ac3` 5.1 | 34,725 (1.09x) | 44,324 (1.39x) | 83,510 (2.61x) | 100,891 (3.15x) |
| `ac3_fold` 5.1 to Lo/Ro | 47,636 (1.49x) | 59,799 (1.87x) | 87,334 (2.73x) | 104,283 (3.26x) |
| `ac3_stereo` 2/0 | 11,293 (0.35x) | 15,710 (0.49x) | 28,569 (0.89x) | 35,611 (1.11x) |
| `ac3_mono` 1/0 | 5,716 (0.18x) | 8,283 (0.26x) | 13,340 (0.42x) | 17,039 (0.53x) |
| `eac3` 5.1, AHT, spectral extension, coupling | 38,681 (1.21x) | 46,356 (1.45x) | 105,642 (3.30x) | 122,158 (3.82x) |
| `eac3_ecpl` 5.1, enhanced coupling | 74,951 (2.34x) | 92,463 (2.89x) | 231,141 (7.22x) | 270,111 (8.44x) |
| `eac3_atmos_bed` 5.1 bed | 29,602 (0.93x) | 36,898 (1.15x) | 73,211 (2.29x) | 84,791 (2.65x) |
| `eac3_atmos_objects` | 191,235 (5.98x) | 222,944 (6.97x) | 223,241 (6.98x) | 257,849 (8.06x) |
| `eac3_stereo` 2/0 | 16,003 (0.50x) | 20,400 (0.64x) | 40,414 (1.26x) | 48,354 (1.51x) |
| `eac3_714` 7.1.4 | 96,902 (3.03x) | out of heap | 275,741 (8.62x) | not run |
| `eac3_fold` 5.1 to Lo/Ro | 67,479 (2.11x) | 80,926 (2.53x) | 117,696 (3.68x) | 136,727 (4.27x) |
| `eac3_714_fold` 7.1.4 to Lo/Ro | 141,220 (4.41x) | not run | 294,207 (9.19x) | not run |
| `eac3_line` 5.1, line mode | 52,124 (1.63x) | 63,188 (1.97x) | 111,339 (3.48x) | 129,577 (4.05x) |
| `eac3_atmos_render` onto 7.1.4 | 196,061 (6.13x) | 229,226 (7.16x) | 231,191 (7.22x) | 268,036 (8.38x) |

"Not run" is the WiFi build's heap budget (`AC3FORGE_PROBE_HEAP_BUDGET_BYTES`, 215,000) skipping
a fixture after `eac3_714` ran out of heap on a 1,692-byte request in the first WiFi run.

The fixed-point tier is 1.7x to 3.1x faster than float here, except on the two rows that
reconstruct objects, where JOC's transform is float in every build. With the network up the
fixed-point decode takes 17% to 45% longer, the smallest fixtures the most.

### Where the time goes

Self time per frame in microseconds, fixed-point tier, no network, from a build with
`-DAC3FORGE_STAGE_TIMERS=ON`. A pair of stage markers costs 3.0 microseconds on this part.

| Stage | `ac3` 5.1 | `ac3_stereo` | `eac3` 5.1 | `eac3_atmos_bed` | `eac3_stereo` |
|---|---:|---:|---:|---:|---:|
| IMDCT and overlap-add | 25,326 | 8,335 | 22,604 | 21,920 | 8,049 |
| AHT dequantisation and inverse | - | - | 6,501 | - | 2,397 |
| spectral extension | - | - | 5,780 | - | 2,441 |
| mantissa read and dequantisation | 2,946 | 1,347 | 541 | 3,330 | 475 |
| decoupling | 2,488 | 7 | - | - | - |
| bit allocation | 1,047 | 431 | 756 | 938 | 437 |
| exponents | 348 | 219 | 335 | 513 | 168 |
| everything else | 2,335 | 1,045 | 2,691 | 3,315 | 2,360 |
| total | 34,490 | 11,384 | 39,208 | 30,016 | 16,327 |

The transform is 73% of an AC-3 5.1 frame, 58% of E-AC-3 5.1 and 73% of the Atmos bed. An AC-3
5.1 frame is 36 blocks of transform and overlap-add, six channels in each of six blocks, about
700 microseconds each here. In the fixed-point tier every product in the transform goes through a
64-bit multiply, a rounding add and a saturation (`src/forge/src/core/fixed32.hpp`,
`src/forge/src/core/mdct_fixed.hpp`). On the [ESP32-S3](esp32-s3.md#where-the-time-went), in
float with its FPU at 240 MHz, the same stage measured 4.8 ms of an AC-3 5.1 frame and 3.4 ms of
an E-AC-3 one.

### Memory

Bytes. The WiFi column's ranges span three runs of the fixed-point build.

| | No network | WiFi and a stream |
|---|---:|---:|
| Free at boot | 383,416 | 295,104 |
| Free after joining the network | - | 241,604 to 241,632 |
| Free before the decode, the stream arriving | 383,416 | 234,648 to 236,576 |
| Largest block before the decode | 352,256 | 217,088 to 221,184 |
| Lowest free during the run | 145,364 | 17,920 to 19,852 |
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
of `apps/wasm/assets/demo.ec3`. During the decode the part received 1,536 to 1,542 kbit/s, and
the longest gap between two reads was 82 to 96 ms. The access point negotiated 802.11n on
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
| Free before the decode | 234,648 to 236,576 | 284,936 to 285,192 |
| Largest block before the decode | 217,088 to 221,184 | 262,144 |
| Lowest free during the run | 17,920 to 19,852 | 39,424 to 39,432 |
| `eac3_714` 7.1.4 | out of heap | 156,775 (4.90x) |
| `eac3_714_fold` 7.1.4 to Lo/Ro | not run | 218,867 (6.84x) |
| `ac3` 5.1 | 44,324 (1.39x) | 53,746 (1.68x) |
| `eac3` 5.1 | 46,356 (1.45x) | 58,816 (1.84x) |
| `eac3_atmos_bed` 5.1 bed | 36,898 (1.15x) | 46,778 (1.46x) |
| `ac3_stereo` 2/0 | 15,710 (0.49x) | 20,424 (0.64x) |
| `eac3_stereo` 2/0 | 20,400 (0.64x) | 26,141 (0.82x) |
| `ac3_mono` 1/0 | 8,283 (0.26x) | 11,191 (0.35x) |

Every fixture fits with the options off, and the stream still arrived at 1,539 and 1,542 kbit/s.
The WiFi code then runs from flash, as the decoder's does. The decode is slower and varies more
between runs, up to 32% on the mono row where the defaults varied by up to 16%; each time is the
slowest run. Stereo stays in real time either way.

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

The 937 ms shapes play 6.7% fast, so a TDM line on this part runs correctly at the full 128-bit
frame, eight 16-bit slots or four 32-bit ones, with the slots a layout leaves unused written as
zeros. Nothing was connected to the pins; a DAC on the line is not part of this measurement.

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
| A float level meter over eight slots, squares summed sixteen at a time | 1,529 |

About 0.9 microseconds a sample for the conversions, so eight 16-bit slots take more than a
third of a frame before the decode is counted.

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
