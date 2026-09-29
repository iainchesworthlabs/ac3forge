# ESP32-P4

The minimum-footprint decoder on an Espressif ESP32-P4: a dual-core RISC-V (RV32IMAFC) with a
single-precision FPU, 768 KB of L2MEM, and no radio of its own. It decodes in the float tier, as
the [ESP32-S3](esp32-s3.md) does, from the same `esp-idf/ac3forge/` component, whose manifest
lists `esp32p4` beside `esp32s3`, `esp32c3` and `esp32c6`.

It is the "best" tier of the shared C6/S3/P4 sink family
([`planning/esp32-sink-tiers.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/esp32-sink-tiers.md)):
closed 2026-09-08 as a replacement for the S3 Wi-Fi Sendspin sink (no on-die radio, the S3 probe
already real-time), reopened 2026-09-21 as a complementary module once a board existed to measure
it on. This page is that plan's Phase P1 — the probe and a board timing table, no network yet.

Every figure on this page outside its [AC-4](#ac-4) section was measured on a board on 2026-09-23, with no network.

## Status

| | |
|---|---|
| Decode | Correct: all fourteen fixtures, every channel's level within the probe's tolerance, and every float-tier PCM hash matching the values the probe pins for this build. Also all eleven stream-set `714-*` files (a scratch probe copy, not committed — see [Stream set](#stream-set)), levels to the digit against `streams.json`'s own reference |
| Real time, no network | **Every fixture and every stream-set file**, from 0.027x (`ac3_mono`) to 0.448x (`eac3_714_fold`, 7.1.4 folded to Lo/Ro) among the fixtures, up to 0.700x (`714-ecpl`) among the stream set — comfortably inside a 32 ms frame even at this chip's 360 MHz ceiling, not the part's 400 MHz datasheet maximum (see [The chip revision](#the-chip-revision-and-what-it-blocks)) |
| Memory | 514,820 bytes free at boot, largest block 385,024; peak heap across every fixture 195,025 (`eac3_atmos_render`), leaving well over half the free total unused at the worst point measured |
| AC-4 decode | Behind `CONFIG_AC3FORGE_AC4`, off by default. Twenty plays of DEE's streams (2.0, 5.1 and 5.1.4; SIMPLE, A-SPX, A-CPL and S-CPL; the converter's four frame rates) run from an HTTP source with the network up. The board's `float` hash equals the host's on 15 of them. On the other five, the plays with companding, three `float` libm calls differ; routed through the project's own functions they agree on the host, the Cortex-M3 leg and the board, see [AC-4](#ac-4) |
| AC-4 real time | 2.0 in SIMPLE mode only, at 0.86 of a frame. 2.0 in A-SPX mode takes 1.04, 5.1 3.5 to 6.7, 5.1.4 3.8 to 6.1, and the converter's frame rates 5.9 to 6.5. E-AC-3 through the same image takes 0.19 for 5.1 and 0.38 for 7.1.4 |
| AC-4 memory | A peak heap of 0.96 MB at 2.0 to 2.8 MB at 5.1.4, with all the internal RAM the heap has (0 to 22 KB free at its least) and the rest in PSRAM; the decode task uses 49 to 50 KB of a 64 KB stack |
| Encode | Not measured. Both encoders are floating-point; nothing here rules it out |
| QEMU | Not emulated, see [QEMU](#qemu) |
| CI | The component pack builds for `esp32p4` from its archive; `.github/workflows/_build.yml` builds this probe target (decoder direction) and runs nothing, the same gap the ESP32-C6 leg has |

## The board

A DFRobot FireBeetle 2 ESP32-P4 (the compact AI-vision SKU: two MIPI FPC connectors for camera
and display, GPIO headers along both edges, no separate UART bridge chip). It carries:

- The ESP32-P4 itself, chip revision v1.3, efuse block revision v0.3 — pre-production silicon,
  not the v3.x this part's mass-production runs ship as (see below).
- An ESP32-C6-MINI-1 module wired to the P4 over SDIO (`GPIO14`-`GPIO19`) for Wi-Fi 6 and
  Bluetooth LE, per DFRobot's documentation. This probe does not touch it; the sink-tiers plan's
  Phase P3 is where a hosted Wi-Fi shape over that link would be measured.
- **Two USB-C connectors**, wired to two different on-die USB peripherals, not one connector
  shared between them: one silkscreened "USB 2.0 OTG", reaching the part's native high-speed
  USB-OTG controller (`SOC_USB_OTG_SUPPORTED`; the ROM's download mode answers here — esptool
  reports "USB mode: USB-OTG" connecting to it); the other reaching
  `SOC_USB_SERIAL_JTAG_SUPPORTED`, the lightweight controller the S3/C3/C6 boards use for their
  console, confirmed by a new composite device (`VID_303A`, `PID_1001`, the same PID those boards
  present) enumerating there the moment the chip boots an application, independent of whether the
  OTG connector is plugged in at all. Building against this board needs both connected: the OTG
  one to flash, the other to read anything back.
- 16 MB of flash (confirmed at boot: `SPI Flash Size: 16MB`) and 32 MB of PSRAM, per DFRobot's
  listing. PSRAM is not used by this profile — see [ESP32-S3 → Building](esp32-s3.md) for why a
  minimum-footprint probe leaves it off even when the board has it. The AC-4 decoder uses it
  ([AC-4](#ac-4)).

## The chip revision, and what it blocks

This is the finding that cost the most time, and the one most worth reading before touching this
part on this kind of board.

**ESP-IDF v6.1 defaults to ESP32-P4 chip revision v3.1 and above.** Its own Kconfig says why
(`components/esp_hw_support/port/esp32p4/Kconfig.hw_support`): revisions below v3.0 and v3.0-and-above
"have huge hardware difference... not compatible with 0.x and 1.x." This is not an errata floor
that a workaround papers over — it is IDF's own statement that these are two hardware generations
under one part number, and a default build's bootloader refuses outright to start on this board's
v1.3 silicon:

```
ERROR: A fatal error occurred: 'bootloader.bin' requires chip revision in range
[v3.1 - v3.99] (this chip is revision v1.3). Use the force argument to flash anyway.
```

The costly part was not the error — it was that `idf.py flash` never showed it. Its `ninja flash`
target passes esptool `--skip-flashed` by default, and against this board that produced a
`write-flash` step with **no write-progress output at all**, an exit code of 0 up to the point
its own post-flash hard-reset touch failed for an unrelated reason (see
[Reading the console](#reading-the-console)), and whatever had been in flash before — blank,
factory, or an earlier build — left running. The probe looked silent rather than never written,
which is a harder failure to diagnose than an error is. It surfaced only by flashing with esptool
directly, dropping `--skip-flashed`, which forces the real write-flash path and its checks to run.

The fix is the Kconfig path IDF already has for this, not a forced flash:

```
CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y
CONFIG_ESP32P4_REV_MIN_100=y
```

(`components/esp_system/port/soc/esp32p4/Kconfig.cpu`'s own default confirms the pairing: once
`ESP32P4_SELECTS_REV_LESS_V3` is set, `ESP_DEFAULT_CPU_FREQ_MHZ_360` becomes the *default* CPU
frequency choice, not merely an option — see below.)

**400 MHz is the other half of the same split, not a separate bug.** It is v3.x silicon's
maximum, reached from a 360 MHz base by a CPLL calibration IDF runs at startup
(`components/esp_hw_support/port/esp32p4/rtc_clk.c`). Asking for it on this board's v1.3 chip
does not fail cleanly: `esp_clk_init` hits `assert failed: esp_clk_init clk.c:105 (res)`
immediately after `cpu_start: Multicore app`, and the chip reboots into the same assertion in a
loop of about 160 ms a cycle, never reaching `app_main`. 187 such cycles were counted in one
30-second console capture before the clock setting was found and corrected. 360 MHz
(`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_360`) is this chip revision's ceiling, and every figure on this
page is measured there — DFRobot's own listing for this board says "360MHz" for the same reason.

## Reading the console

This board has no USB-UART bridge chip, so "attach a terminal" is not the formality it is on the
other three boards' pages. Two things about it cost real time:

**Which connector.** The OTG connector answers only the ROM's download-mode protocol; the
console is on the *other* one, once `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` is set (see
[Building](#building)) — `CONFIG_ESP_CONSOLE_USB_CDC`, the option the S2 and S3 use for a ROM CDC
console over their OTG-style peripheral, is not offered here at all: it depends on
`SOC_USB_OTG_CONSOLE_SUPPORTED`, which this part's `soc_caps.h` does not define.

**How to open it.** A bare `pyserial` open of that connector's COM port — however precisely timed
against a separate, deliberately triggered reset (esptool's own `--after hard-reset`, watched and
reopened within about a second of it dropping) — caught nothing, across roughly eight attempts,
including a build that printed once a second for twenty seconds after boot. `idf.py monitor`
caught a complete boot log and the full probe output on its first attempt, no special handling
needed beyond running it. The reset it produces on attach is a different one from esptool's:
`rst:0x17 (CHIP_USB_UART_RESET)` against esptool's `rst:0xc (SW_CPU_RESET)` — a reset asserted
over the USB/UART line itself rather than requested of a running stub, which on this board's
console peripheral evidently leaves the connection in a state a host can actually read from,
where the other kind does not. Why is not established further than that; what to do about it is:
use `idf.py monitor` (or another tool that toggles DTR/RTS on attach) to read this board, not a
plain serial open.

Flashing itself needs the ROM's manual download mode on every attempt — this board has no
auto-reset circuit: hold BOOT (the `35/BOOT` button), tap RST, hold BOOT roughly a second longer,
release. The chip presents nothing on either USB connector until this is done; the ghost of a
CH34x-style bridge chip that appears in Windows' device history if one was ever tried on this
machine before is unrelated hardware, not this board (see the two-connector note above).

## What the part carries

Fixed-point tier not measured — this part has an FPU, and every board figure here is the float
tier. Fourteen fixtures, no network, 360 MHz:

| Fixture | Decodes in real time | Peak heap |
|---|---|---|
| AC-3 mono, 2/0, 5.1 | Yes: 0.027x, 0.051x, 0.181x | 47,772 / 49,480 / 56,597 |
| AC-3 5.1 folded to Lo/Ro | Yes: 0.144x | 58,645 |
| E-AC-3 2/0, 5.1 with AHT, spectral extension, coupling | Yes: 0.071x, 0.176x | 76,090 / 102,342 |
| E-AC-3 §E3.5 enhanced coupling | Yes: 0.371x | 129,657 |
| E-AC-3 5.1 folded to Lo/Ro, line mode | Yes: 0.182x, 0.178x | 108,730 / 102,450 |
| Atmos bed, objects skipped | Yes: 0.141x | 103,596 |
| Atmos objects reconstructed | Yes: 0.354x | 194,655 |
| Objects placed onto 7.1.4 | Yes: 0.417x (render alone: 1,638 us/frame) | 195,025 |
| E-AC-3 7.1.4, a bed and two dependent substreams | Yes: 0.429x | 167,386 |
| E-AC-3 7.1.4 folded to Lo/Ro | **Yes: 0.448x**, the widest fixture and still under half real time | 173,794 |

Every row that misses real time on the ESP32-C6 (fixed tier, with WiFi) or sits at the line on
the ESP32-S3's *as-found* figures (before that page's optimisation work) is comfortably inside
budget here, unoptimised, at this chip revision's reduced 360 MHz clock. The part's headroom, not
the code, is what this table is measuring.

## Measured, on a board

A DFRobot FireBeetle 2 ESP32-P4, chip revision v1.3, 16 MB flash read in DIO mode at 80 MHz.
ESP-IDF v6.1 and GCC esp-15.2.0_20251204, `-Os` with the decode-critical sources at `-O2`
(`AC3FORGE_MINIMAL_HOT_O2`, the project's default), the task watchdog off, 360 MHz.

The probe decodes six frames of each fixture. Its timing is the decoder's own: the level and hash
accumulation it runs inside the decoder's block callback is timed and subtracted.

### Decode time

Microseconds per frame, and that as a fraction of the 32,000 microseconds a frame lasts.

| Fixture | us/frame | x real time |
|---|---:|---:|
| `ac3_mono` | 864 | 0.027 |
| `ac3_stereo` | 1,633 | 0.051 |
| `ac3_fold` 5.1 to Lo/Ro | 4,618 | 0.144 |
| `eac3_atmos_bed` | 4,536 | 0.141 |
| `eac3_stereo` | 2,273 | 0.071 |
| `eac3` 5.1, AHT, spectral extension, coupling | 5,663 | 0.176 |
| `ac3` 5.1 | 5,804 | 0.181 |
| `eac3_fold` 5.1 to Lo/Ro | 5,849 | 0.182 |
| `eac3_line` 5.1, line mode | 5,721 | 0.178 |
| `eac3_ecpl` 5.1, enhanced coupling | 11,880 | 0.371 |
| `eac3_atmos_objects` | 11,357 | 0.354 |
| `eac3_atmos_render` onto 7.1.4 | 13,360 | 0.417 |
| `eac3_714` 7.1.4 | 13,741 | 0.429 |
| `eac3_714_fold` 7.1.4 to Lo/Ro | 14,337 | 0.448 |

`eac3_atmos_render`'s render stage (placing objects onto loudspeakers, separate from the decode
above it) is 1,638 us/frame of the 13,360 total.

### Memory

Bytes, `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` — the same pool the S3 and C6 pages report, byte-
addressable internal SRAM.

| | Bytes |
|---|---:|
| Free at boot | 514,820 |
| Largest block at boot | 385,024 |
| Free after the full run (all fourteen fixtures) | 514,564 |
| Largest block after | 294,912 |
| Peak heap, worst fixture (`eac3_atmos_render`) | 195,025 |
| Retained after teardown | 12 |
| Main task stack left of 40,960 | 20,272 |

The peak is 38% of what was free at boot. Against the S3's own ceiling — 245,000 bytes gated in
CI, against roughly 280,000 free — this part's 768 KB of L2MEM makes the memory question a
non-question at this profile's scale; nothing here came close to pressuring it. The image itself
is 357,368 bytes, inside a 1 MB partition with half free.

### Stream set

The fourteen fixtures above are six-frame clips built for this probe alone. The sink-tiers plan's
exit criterion also asks for the actual stream-set files a real player streams -
`esp-idf/ac3forge/examples/hearth_sink/www/714-*.ec3`, eleven files, each isolating one Annex E
coding-tool combination at 7.1.4 (`planning/esp32-stream-set.md` has the full manifest). These
were wrapped bit for bit into a scratch copy of the probe - not re-encoded, since the point is to
decode what a player actually receives - the same shape the ESP32-C6 page's "2/0, 5.1 and 7.1
from one generator" section used for its own three stream-set files. That copy was never
committed, matching the C6 precedent (its own commit history has no trace of the probe copy
either, only the prose). Unlike that precedent, these rows have real reference levels: the
stream set's own manifest, `streams.json`, carries a `levels_714` array per file, computed the
same way `fixture.hpp`'s are and in the same coded order - so this is a level-checked
measurement, not a timing-only one.

Every one of the eleven files decodes correct - every channel's level to the digit against
`levels_714` - and in real time, no network, 360 MHz:

| Stream | Access units | us/frame | x real time | Peak heap |
|---|---:|---:|---:|---:|
| `714-none.ec3` | 16 | 9,924 | 0.310 | 166,930 |
| `714-cpl.ec3` | 16 | 10,427 | 0.325 | 168,014 |
| `714-ecpl.ec3` | 16 | 22,415 | **0.700** | 196,803 |
| `714-spx.ec3` | 16 | 11,226 | 0.350 | 166,662 |
| `714-aht.ec3` | 16 | 11,849 | 0.370 | 167,886 |
| `714-tpn.ec3` | 16 | 9,976 | 0.311 | 256,584 |
| `714-all.ec3` | 16 | 12,686 | 0.396 | 167,629 |
| `714-walk.ec3` | 150 (4.8 s) | 10,528 | 0.329 | 168,426 |
| `714-tones.ec3` | 63 (2.0 s) | 10,469 | 0.327 | 166,890 |
| `714-blocks2.ec3` | 47 (2-block syncframes) | 4,286 | 0.133 | 80,922 |
| `714-blocks3.ec3` | 32 (3-block syncframes) | 5,898 | 0.184 | 102,722 |

`714-ecpl` (enhanced coupling) is both the slowest and the largest - 196,803 bytes - consistent
with the existing `eac3_ecpl` fixture's cost among the fourteen. `714-tpn` is not the slowest but
is the largest at 256,584 bytes, matching `streams.json`'s own `psram: true` flag for that file
(and for `714-ecpl`) - on this part, with 514,820 bytes free at boot and no PSRAM at all in this
profile, neither needed it. `714-blocks2` and `714-blocks3` cost the least per access unit because
each one covers fewer blocks (2 and 3, against the standard 6) - less audio, proportionally less
work, not a cheaper decode path.

This closes the sink-tiers plan's exit criterion 1 in full: fourteen fixtures and the stream-set
`714-*` rows, both in real time, no network, on a board.

## AC-4

`CONFIG_AC3FORGE_AC4` (off by default, and offered only on a part with a floating-point unit)
builds the AC-4 inspector, core and decoder of `src/ac4`, `src/ac4core` and `src/ac4dec` into the
component, in single precision and in the minimum-footprint profile (`AC3FORGE_MINIMAL_AC4`), and
lets the player read a stream that opens with an AC-4 sync word. It takes the ring, the renderer
and the sinks an AC-3 or E-AC-3 stream takes, with `ac4::SyncFrameSplitter` and `ac4::Decoder` in
place of their framer and decoders. With the switch off the component builds as it did.
`hearth_sink` plays AC-4 from its HTTP source with `sdkconfig.ac4` in its defaults and ends each
play with the lines this section's figures come from ([Building](#building-with-ac-4)).

### How it was measured

Every figure in this section is from the board above on 2026-09-29, at 360 MHz, with the network
up (Wi-Fi over the C6, mDNS, the Sendspin player idle, the HTTP source's fetch task and the decode
task running), 32 MB of PSRAM in the heap, the decode task's stack at 64 KB and a null sink that
paces its writes as a DAC would. The decoder is the tree as D14a's second part left it, before the
third part reworks the QMF bank and the memory the decoder holds; the figures are what that work
is measured against. The streams are DEE's music streams from the local gold set, ten seconds
each (237 frames at 23.44 fps, and 240, 241, 251 and 300 at the converter's four frame rates),
served from a desktop over HTTP and played with `POST /play` after `PUT /layout`, to the coded
layout or to 2.0 through the decoder's Lo/Ro fold. `GET /log` brings the console back.

The decoder's time is the decode task's time less what the play spent placing blocks on the layout,
in the sink's write and in the PCM hash, and it is set against the audio a frame carries: 2,048
samples at 48 kHz, 42.7 ms, or 1,920, 2,002 or 1,602 samples at the other frame rates. A play is
one pass, since an HTTP source cannot rewind, and its first frame carries one-off set-up, so the
time per frame is the play without its first frame and the first frame has a column of its own. The
hash costs 0.6 ms a frame at 2.0: a play with it off (36,835 us a frame) agrees with the same play
with it on (36,768) to 0.2%. Heap is `heap_caps_monitor_local_minimum_free_size`, as
[the stream set's plan](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/esp32-stream-set.md)
has it: the peak of what the play took from internal RAM and PSRAM together, and the least
internal RAM that was free while it ran. Stack left is the decode task's
`stream.decode_stack_free`. The stage timers are those of `AC3FORGE_STAGE_TIMERS`: a marker in the
library at each part of the decode, whose self time, a stage's own less the stages inside it, the
stage table gives.

### What it decodes in real time

At 360 MHz with the network up, the AC-4 decoder keeps up with real time for one thing, 2.0 in
SIMPLE mode, which takes 0.86 of a frame. 2.0 in A-SPX mode sits at the line, at 1.04. 5.1 takes
3.5 to 6.7 times a frame's duration in each codec mode, and 2.1 to 4.9 when the decoder folds it
to 2.0. 5.1.4 in full decoding takes 5.4 to 6.1 to its own layout and 4.3 to 5.2 folded to 2.0, and
3.8 to 4.7 to its own layout in core decoding. The converter's four frame rates take 5.9 to 6.5,
of which the converter is five sixths.

Beside them, AC-3 and E-AC-3 through the same image, network and server, decoder time as above:

| Stream | To | us/frame | x real time |
|---|---|---:|---:|
| `dee-ac3-51.ac3`, AC-3 | 5.1 | 6,111 | 0.19 |
| `dee-eac3-51.ec3`, E-AC-3 | 5.1 | 6,097 | 0.19 |
| `714-walk.ec3`, E-AC-3 | 7.1.4 | 12,191 | 0.38 |

The probe above, with no network and other streams, has 0.18 for 5.1 and 0.43 for 7.1.4. A 2.0
AC-4 stream in SIMPLE mode takes 4.5 times as long per second of audio as E-AC-3 5.1, and a 5.1
AC-4 stream 19 to 35 times.

### Decode time and memory

| Stream | Codec mode | To | us/frame | x real time | First frame s | Peak heap MB | Internal RAM least free KB | Stack left KB |
|---|---|---|---:|---:|---:|---:|---:|---:|
| `20-music-192` | SIMPLE | 2.0 | 36,768 | 0.86 | 0.35 | 0.96 | 8 | 16.4 |
| `20-music-96` | A-SPX | 2.0 | 44,429 | 1.04 | 0.35 | 0.98 | 22 | 16.4 |
| `51-music-384` | SIMPLE | 2.0 | 103,340 | 2.42 | 0.44 | 1.59 | 6 | 15.5 |
| `51-music-384` | SIMPLE | 5.1 | 150,758 | 3.53 | 0.52 | 1.66 | 2 | 15.5 |
| `51-music-192` | A-SPX | 2.0 | 90,197 | 2.11 | 0.41 | 1.58 | 2 | 15.6 |
| `51-music-192` | A-SPX | 5.1 | 183,976 | 4.31 | 0.54 | 1.64 | 2 | 15.5 |
| `51-music-128` | A-SPX, A-CPL 2 | 2.0 | 114,908 | 2.69 | 0.40 | 1.61 | 5 | 15.5 |
| `51-music-128` | A-SPX, A-CPL 2 | 5.1 | 205,895 | 4.83 | 0.52 | 1.67 | 1 | 15.6 |
| `51-music-96` | A-SPX, A-CPL 3 | 2.0 | 208,733 | 4.89 | 0.42 | 1.92 | 2 | 15.6 |
| `51-music-96` | A-SPX, A-CPL 3 | 5.1 | 284,454 | 6.67 | 0.52 | 1.98 | 1 | 15.6 |
| `514-music-256` | A-SPX, A-CPL 2 | 2.0 | 222,511 | 5.21 | 0.55 | 2.37 | 2 | 15.3 |
| `514-music-256` | A-SPX, A-CPL 2 | 5.1.4 | 262,142 | 6.14 | 0.63 | 2.72 | 0 | 15.3 |
| `514-music-512` | A-SPX, S-CPL | 2.0 | 219,434 | 5.14 | 0.56 | 2.47 | 2 | 15.3 |
| `514-music-512` | A-SPX, S-CPL | 5.1.4 | 249,990 | 5.86 | 0.64 | 2.82 | 1 | 15.3 |
| `514-music-768` | S-CPL | 2.0 | 183,731 | 4.31 | 0.57 | 2.49 | 1 | 15.3 |
| `514-music-768` | S-CPL | 5.1.4 | 229,062 | 5.37 | 0.66 | 2.84 | 1 | 15.3 |
| `ims-music-64-23976` | A-SPX, 23.976 fps | 2.0 | 271,051 | 6.50 | 6.01 | 1.77 | 6 | 16.4 |
| `ims-music-64-24` | A-SPX, 24 fps | 2.0 | 245,299 | 5.89 | 0.57 | 1.04 | 8 | 16.4 |
| `ims-music-64-25` | A-SPX, 25 fps | 2.0 | 248,442 | 6.21 | 0.54 | 1.05 | 10 | 16.4 |
| `ims-music-64-2997` | A-SPX, 29.97 fps | 2.0 | 215,222 | 6.45 | 5.92 | 1.72 | 14 | 16.4 |

`20-music-192` and `20-music-96` are 2.0; the `51-` streams are 5.1 at 384, 192, 128 and 96 kbps,
and the `514-` streams 5.1.4 at 256, 512 and 768, each in the mode DEE writes at that rate; the
`ims-` streams are DEE's immersive stereo at 64 kbps at 23.976, 24, 25 and 29.97 fps. Core
decoding of the three 5.1.4 streams, which the standard lets a decoder do for the immersive element:

| Stream | To | us/frame | x real time | First frame s | Peak heap MB | Internal RAM least free KB | Stack left KB |
|---|---|---:|---:|---:|---:|---:|---:|
| `514-music-256` | 2.0 | 156,577 | 3.67 | 0.50 | 1.95 | 2 | 15.3 |
| `514-music-256` | 5.1.4 | 167,592 | 3.93 | 0.54 | 2.18 | 1 | 15.3 |
| `514-music-512` | 2.0 | 194,460 | 4.56 | 0.52 | 2.18 | 2 | 15.3 |
| `514-music-512` | 5.1.4 | 200,425 | 4.70 | 0.56 | 2.41 | 1 | 15.3 |
| `514-music-768` | 2.0 | 157,116 | 3.68 | 0.51 | 2.20 | 2 | 15.3 |
| `514-music-768` | 5.1.4 | 163,576 | 3.83 | 0.55 | 2.42 | 3 | 15.3 |

### Where a frame goes

Microseconds a frame. The reconstruction's column is the stage's own time less its first frame's
set-up, taken from a 24-frame cut of each stream whose totals the full play's are differenced
against; the other columns are the play's average, in which the first frame's share is 0.1 ms at
most. Parse is the frame's syntax and Huffman decoding; reconstruct is the spectral reconstruction,
the stereo and channel processing, the downmix, DRC and the output stage.

| Stream | To | parse | reconstruct | imdct | QMF analysis | QMF synthesis | A-SPX | A-CPL | converter | frame |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `20-music-192` | 2.0 | 7,215 | 11,036 | 4,494 | 6,822 | 6,950 |  |  |  | 36,768 |
| `20-music-96` | 2.0 | 6,454 | 12,544 | 4,530 | 6,774 | 7,119 | 6,903 |  |  | 44,429 |
| `51-music-384` | 2.0 | 13,333 | 15,268 | 14,363 | 20,942 | 39,186 |  |  |  | 103,340 |
| `51-music-384` | 5.1 | 13,058 | 14,536 | 14,494 | 21,106 | 87,315 |  |  |  | 150,758 |
| `51-music-192` | 2.0 | 12,050 | 19,887 | 14,320 | 20,542 | 7,435 | 15,667 |  |  | 90,197 |
| `51-music-192` | 5.1 | 12,077 | 16,872 | 14,302 | 20,529 | 104,154 | 15,730 |  |  | 183,976 |
| `51-music-128` | 2.0 | 8,977 | 19,836 | 14,070 | 20,234 | 7,615 | 9,911 | 33,796 |  | 114,908 |
| `51-music-128` | 5.1 | 8,727 | 14,796 | 14,072 | 20,119 | 103,845 | 10,036 | 33,856 |  | 205,895 |
| `51-music-96` | 2.0 | 7,728 | 18,178 | 13,982 | 20,280 | 23,309 | 6,470 | 118,004 |  | 208,733 |
| `51-music-96` | 5.1 | 7,697 | 13,260 | 14,023 | 20,284 | 103,908 | 6,479 | 118,025 |  | 284,454 |
| `514-music-256` | 2.0 | 14,859 | 25,921 | 22,690 | 89,250 | 7,290 | 20,270 | 41,804 |  | 222,511 |
| `514-music-256` | 5.1.4 | 15,050 | 37,141 | 22,666 | 88,951 | 36,590 | 20,161 | 41,727 |  | 262,142 |
| `514-music-512` | 2.0 | 24,729 | 38,756 | 22,732 | 89,089 | 7,284 | 36,414 |  |  | 219,434 |
| `514-music-512` | 5.1.4 | 24,398 | 39,975 | 22,792 | 89,506 | 36,617 | 36,358 |  |  | 249,990 |
| `514-music-768` | 2.0 | 27,204 | 36,255 | 23,026 | 89,743 | 7,251 |  |  |  | 183,731 |
| `514-music-768` | 5.1.4 | 27,487 | 36,555 | 22,891 | 105,540 | 36,287 |  |  |  | 229,062 |
| `ims-music-64-23976` | 2.0 | 5,686 | 9,812 | 3,238 | 8,362 | 8,738 | 5,462 |  | 229,118 | 271,051 |
| `ims-music-64-24` | 2.0 | 5,674 | 9,684 | 3,237 | 8,341 | 8,561 | 5,444 |  | 203,703 | 245,299 |
| `ims-music-64-25` | 2.0 | 5,639 | 11,775 | 4,524 | 6,747 | 7,123 | 5,168 |  | 206,772 | 248,442 |
| `ims-music-64-2997` | 2.0 | 5,365 | 8,959 | 2,113 | 5,102 | 5,518 | 4,617 |  | 183,145 | 215,222 |

A 2.0 frame in SIMPLE mode is half syntax and reconstruction (parse 7.2 ms, the reconstruction's
own work 11.0) and half transforms (IMDCT 4.5, QMF analysis 6.8, QMF synthesis 7.0). The parse
grows with the bit rate: 5.4 to 7.3 ms in stereo, 7.7 to 13.3 at 5.1 and 14.9 to 27.5 at 5.1.4.
A-SPX takes 4.6 to 6.9 ms in stereo, 6.5 to 15.8 at 5.1 and 20 to 36 at 5.1.4. A-CPL is the
largest stage of the modes that have it: 34 ms in mode 2 at 5.1 (42 at 5.1.4) and 118 ms in mode 3,
the same at 2.0 as at 5.1 because its decorrelators run before the fold.

A QMF bank takes 3.4 to 3.7 ms a channel-frame when its state is in internal RAM, as at 2.0, and
7.4 to 17.4 ms when it is in PSRAM, for the same code on the same data. QMF analysis is 3.4 ms a
channel at 5.1 and 7.4 to 8.8 at 5.1.4 (12 channels a frame); QMF synthesis is 14.6 to 17.4 at 5.1
and 3.6 at 5.1.4 (10 channels); at 2.0, `51-music-96` synthesis took 11.7 ms a channel in this
image and the other streams' 3.7 to 3.8.
Which side a buffer lands on is not chosen. `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` sends an
allocation of up to 16 KB to internal RAM while any is left, the decoder's many blocks of that size
use it all, and a bank's state goes wherever the allocation happens to arrive. An earlier image of
the same tree, whose only difference was compiling the decoder's own files at `-O2` (which moved
neither parse nor reconstruct), took 166.9 ms a frame for `51-music-384` at 5.1 against 150.8, and
166.3 ms for `514-music-768` at 2.0 against 183.7, all of it in the QMF stages.

### The frame-rate converter

| Stream | Ratio | Samples a frame | us/frame | x real time | Converter us/frame | First frame s |
|---|---|---:|---:|---:|---:|---:|
| `ims-music-64-24` | 25/24 | 2,000 | 245,299 | 5.89 | 203,703 | 0.57 |
| `ims-music-64-23976` | 1001/960 | 2,002 | 271,051 | 6.50 | 229,118 | 6.01 |
| `ims-music-64-25` | 15/16 | 1,920 | 248,442 | 6.21 | 206,772 | 0.54 |
| `ims-music-64-2997` | 1001/960 | 1,602 | 215,222 | 6.45 | 183,145 | 5.92 |

Part 1 clause 6.2.15's three ratios are 25/24 (24 fps), 1001/1000 x 25/24 = 1001/960 (23.976 fps,
and 29.97 fps at its own frame length) and 15/16 (25 fps). The converter's polyphase filter runs in
`double` on a part whose FPU is single precision, so each multiply and add is a call to a software
routine of the compiler's runtime. It costs 183 to 229 ms a frame, which is 4.9 to 5.5 times real
time by itself and 83 to 85% of the frame, or 102 to 114 us for each output sample of the pair.
The phase table is designed in `double` as well: at 1001/960 the first frame takes 5.4 s more than
an ordinary one (1,001 phases of 94 taps, a table of 752,752 bytes), and at 25/24 and 15/16 it takes
no longer than any other.

### Float output on the host, the Cortex-M3 leg and the board

[Decision 26](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/ac4.md#decisions-of-2026-09-25)
promises identical `float` output everywhere. The board's PCM hash (FNV-1a over the sample bit
patterns in the order the player delivered them, the probe's own) equals the host's
`AC3FORGE_DECODE_SCALAR=float` output on 15 of the 20 plays above: every 5.1 and 5.1.4 play, and
`20-music-192`. It differs on `20-music-96` and the four converter streams, the plays with
companding, which is on in stereo A-SPX at these rates. Four 24-frame cuts of such streams were
decoded on the host with MSVC and with glibc, on a Cortex-M3 program of the probe's kind
(arm-none-eabi GCC 14.2.1 and newlib-nano, under QEMU) and on the board:

| Cut | Host, MSVC and glibc | Cortex-M3 | Board | With the three calls below |
|---|---|---|---|---|
| 2.0 SIMPLE | `94f18a46e23443ed` | same | same | same, on all four |
| 5.1 | `6a85a1ef66521390` | same | same | `2ce8c136c70865c4`, on all four |
| 2.0 A-SPX at 25 fps (converter) | `1fd55416a6b39425` | `21115fe614b233a9` | `8eda725509656c7d` | `301eede696d7c642`, on all four |
| 2.0 A-SPX with companding | `20c9abb0cf887695` | `3fa7a0b73387a218` | `e13f0f3d698d2301` | `9d0310fa5eed3268`, on all four |

The host's two libraries agree; the Cortex-M3 leg's and the board's each give an answer of their
own. The first differing sample of the 25 fps cut, on the Cortex-M3 leg against the host, is sample
2,180, the right channel of the second output frame: 6.383271739e-06 on the host and 6.383272193e-06
on the M3, one unit in the last place. By the 24th frame 8,960 of 92,160 samples differ, the median
gap one unit and the largest 12,307. The cause is three calls of `float` `std::pow` and
`std::exp2`, whose results the C libraries do not agree on to the last bit: `pow(level, (1 - alpha)
/ alpha)` and `exp2(1 / alpha)` in `pcm/companding.cpp`, and the `exp2` of A-SPX's gain in
`pcm/aspx.cpp`. A gain that differs in its last bit scales a companded QMF sample, and the synthesis
bank spreads the difference over the frame. Routing the three through `ac3::internal::scalar_exp2`
and `scalar_log2`, which `hf_generator.cpp`'s gains already use, makes the four platforms agree on
all four cuts, the board included. Which of the three calls carries the difference was not
separated, and that change is in a scratch copy of the tree, not in this one.

### What the decoder holds

Internal RAM is used up. The decoder makes many allocations under the 16 KB that
`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` sends there first, 373 to 381 KB of them with the network
stack and the player's buffers counted in, and a play ends with 0 to 22 KB of it free at its least.
The rest goes to PSRAM: 0.59 MB at 2.0 in SIMPLE mode, 1.2 to 1.6 MB at 5.1 and 2.0 to 2.5 MB at
5.1.4, which with internal RAM is a peak heap of 0.96 MB, 1.6 to 2.0 MB and 2.4 to 2.8 MB. The
decode task uses 49.1 to 50.3 KB of its stack, and the 32 KB the example gives an AC-3 stream
overflows on the first play, in the parser's per-track element tables (one local of 13.5 KB). The
first frame takes 0.35 to 0.66 s; of what it takes beyond a steady frame, 0.26 to 0.43 s is in the
reconstruction stage's own time, which the stage timers do not split further.

### Building with AC-4

```bash
idf.py -DIDF_TARGET=esp32p4 \
  "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.hw;sdkconfig.p4;sdkconfig.sendspin;sdkconfig.ac4" \
  -DAC3FORGE_STAGE_TIMERS=ON build
```

from `esp-idf/ac3forge/examples/hearth_sink/`. `sdkconfig.ac4` turns on `CONFIG_AC3FORGE_AC4` and
the 64 KB decode stack. The measurement image adds `AC3FORGE_EXAMPLE_SINK_NULL`,
`AC3FORGE_EXAMPLE_AC4_PCM_HASH`, `ESP_TASK_WDT_INIT=n` and the network's credentials, and goes to
the board with `tools/hearth/ota.py push`. A play's location can carry `?decoding=core` for core
decoding and `?hash=off` for a play without the hash. A play ends with `ac4.lap` (frames,
samples, the decoder's time, the worst frame's, the hash), `ac4.heap` and one `play.stage[...]`
line for each stage. The packer leaves the AC-4 sources out of the archive unless it is given
`--with-ac4`, and `--verify` then builds an ESP32-P4 project against the archive.

## QEMU

ESP-IDF v6.1's `qemu-system-riscv32` emulates one machine, `esp32c3`, and no other RISC-V part —
the same gap [ESP32-C6](esp32-c6.md#qemu) has. `idf.py qemu` refuses this target; CI builds it and
runs nothing, and every figure on this page comes from the board.

## Building

`apps/baremetal/platform/esp32p4/` is the probe target:

```bash
. $IDF_PATH/export.sh
cd apps/baremetal/platform/esp32p4
idf.py set-target esp32p4
idf.py build                                  # -DAC3FORGE_ESP_PROFILE=decoder by default
```

The decode arithmetic needs no override: the component
(`esp-idf/ac3forge/CMakeLists.txt`) picks `float` from `SOC_CPU_HAS_FPU`, which this part has, the
same as the S3 — see [ESP32-S3 → The ESP-IDF component](esp32-s3.md#the-esp-idf-component).

On a board reached over its OTG connector held in the ROM's manual download mode (see
[Reading the console](#reading-the-console) for both):

```bash
python -m esptool --chip esp32p4 -p <OTG-PORT> -b 460800 \
  --before default-reset --after hard-reset write-flash \
  --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x2000 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/ac3probe_esp32p4.bin
idf.py -B build -p <CONSOLE-PORT> monitor
```

`idf.py flash` works once the chip revision is set correctly (see
[The chip revision](#the-chip-revision-and-what-it-blocks)) — the direct `esptool` form above is
what this page's own figures were flashed with, to keep `--skip-flashed` out of the loop while
that finding was still being pinned down; either works on a corrected build.

There is no `run_esp32p4_probe.sh`: no board is reachable from CI, so nothing there needs one —
`.github/workflows/_build.yml` builds this target beside the ESP32-C6 step, in the same job, for
the same reason (see that step's own comment).

## Where to go next

- [`planning/esp32-sink-tiers.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/esp32-sink-tiers.md) —
  the plan this page is Phase P1 of; Phase P2 is a networked shape (Ethernet or the onboard C6
  over `esp_hosted`) onto TDM and a pair of ES9080 DACs.
- [`planning/ac4.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/ac4.md) —
  phase D14b, of which the [AC-4](#ac-4) section is the measurement, and D14a's third part, which
  reworks what it found to cost most.
- [ESP32-S3](esp32-s3.md) — the "better" tier, hardware-verified, the primary Wi-Fi Sendspin sink.
- [ESP32-C6](esp32-c6.md) — the "good" tier, and the sibling page with the same "no QEMU for this
  part" gap.
- [Bare metal overview](index.md) — how the pages in this section relate.
