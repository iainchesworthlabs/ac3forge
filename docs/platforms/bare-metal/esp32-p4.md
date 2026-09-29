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
| AC-4 decode | Behind `CONFIG_AC3FORGE_AC4`, off by default. Twenty plays of DEE's streams (2.0, 5.1 and 5.1.4; SIMPLE, A-SPX, A-CPL and S-CPL; the converter's four frame rates) run from an HTTP source with the network up, and the probe's six fixtures decode to its pinned `float` PCM hashes exactly. The board's hash equals the host's (GCC 16, Clang 22 and MSVC) on all twenty plays and on all six core-decoding plays, and the Cortex-M3 leg's on the 24-frame cut of each, since D14a4 took the `float` calls whose last bit differs between C libraries out of libm, see [AC-4](#ac-4) |
| AC-4 real time | 2.0 in SIMPLE mode (0.53), in A-SPX mode (0.74) and, through the converter, at 24 fps (0.92) and 25 fps (0.82), with D14a's third part in the decoder (0.86 and 1.04 before it) and D14a4's `float` converter (5.6 to 6.6 before it). 5.1 takes 1.4 to 4.1 and 5.1.4 2.8 to 3.7; the converter's 1001/960 rates, 23.976 and 29.97 fps, take 1.25 and 3.65 (1.21 and 1.26 with the 512-byte policy). E-AC-3 through the same image takes 0.20 for 5.1 and 0.38 for 7.1.4 |
| AC-4 memory | A peak heap of 0.60 MB at 2.0 to 2.2 MB at 5.1.4, with internal RAM used up under ESP-IDF's default allocation policy (2 to 14 KB free at its least). The decode task uses 20 to 24 KB of a 64 KB stack, from 49 to 50 KB before D14a's third part |
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
`hearth_sink` plays AC-4 from its HTTP source with `sdkconfig.ac4` in its defaults, and ends each
play with the lines this section's figures come from ([Building](#building-with-ac-4)).

### How it was measured

Every figure in this section is from the board above on 2026-09-29 and 2026-09-30, at 360 MHz, with
the network up (Wi-Fi over the C6, mDNS, the Sendspin player idle, the HTTP source's fetch task and
the decode task running), 32 MB of PSRAM in the heap, the decode task's stack at 64 KB, a null sink
that paces its writes as a DAC would, and ESP-IDF's default allocation policy
(`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` at 16 KB) unless a column says otherwise. The main figures
are for the decoder with D14a's third part in it (the QMF bank on split planes, the cached bit
reader and table Huffman decoder, one transform scratch a substream); the same streams on the tree
before it are beside them as the baseline that part is measured against. The streams are DEE's
music streams from the local gold set, ten seconds each (237 frames at 23.44 fps, and 240, 241, 251
and 300 at the converter's four frame rates), served from a desktop over HTTP and played with
`POST /play` after `PUT /layout`, to the coded layout or to 2.0 through the decoder's Lo/Ro fold.
`GET /log` brings the console back.

The decoder's time is the decode task's time less what the play spent placing blocks on the layout,
in the sink's write and in the PCM hash, and it is set against the audio a frame carries: 2,048
samples at 48 kHz, 42.7 ms, or 1,920, 2,002 or 1,602 samples at the other frame rates. A play is
one pass, since an HTTP source cannot rewind, and its first frame carries one-off set-up, so the
time per frame is the play without its first frame and the first frame has a column of its own. The
hash costs 0.6 ms a frame at 2.0: a play with it off agrees with the same play with it on to 0.1%.
Heap is `heap_caps_monitor_local_minimum_free_size`, as
[the stream set's plan](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/esp32-stream-set.md)
has it: the peak of what the play took from internal RAM and PSRAM together, and the least
internal RAM that was free while it ran. Stack left is the decode task's
`stream.decode_stack_free`, of 64 KB. The stage timers are those of `AC3FORGE_STAGE_TIMERS`: a
marker in the library at each part of the decode, whose self time, a stage's own less the stages
inside it, the stage table gives.

The converter's four plays, the hash comparison and the sixth probe fixture were measured again on
2026-09-30 with D14a4 in the decoder, on the same board, streams and image configuration; the other
plays measured again agree with D14b's rows to 3% and are not repeated here.

### What it decodes in real time

At 360 MHz with the network up, the AC-4 decoder keeps up with real time for 2.0 in SIMPLE mode,
which takes 0.53 of a frame, 2.0 in A-SPX mode, which takes 0.74, and DEE's immersive stereo through
the converter at 24 and 25 fps, which takes 0.92 and 0.82. Nothing wider does. 5.1 takes
1.4 to 4.1 times a frame's duration to 5.1, one for each of the four codec modes, and 1.4 to 4.5
folded to 2.0. 5.1.4 in full decoding takes 2.8 to 3.7 to its own layout and 2.5 to 3.4 folded to
2.0, and core decoding 2.3 to 3.1 to 5.1.4 and 2.1 to 2.9 to 2.0. The converter's other two
frame rates, 23.976 and 29.97 fps, are the ratio 1001/960, whose table of 1,001 phases is read from
PSRAM: they take 1.25 and 3.65, the second being the play where a stage runs sporadically slow under
the default policy (1.21 and 1.26 with the 512-byte policy). Before D14a's third part the same
streams took 0.86 and 1.04 at 2.0, 3.5 to 6.7 at 5.1, 5.4 to 6.1 at 5.1.4 and 5.9 to 6.5 through the
converter: the part made a frame 1.1 to 2.5 times faster and the converter's streams not at all,
and D14a4's `float` converter took them from 5.6 to 6.6 to 0.82 to 1.25 (3.65).

Beside them, AC-3 and E-AC-3 through the same image, network and server, decoder time as above:

| Stream | To | us/frame | x real time |
|---|---|---:|---:|
| `dee-ac3-51.ac3`, AC-3 | 5.1 | 6,414 | 0.20 |
| `dee-eac3-51.ec3`, E-AC-3 | 5.1 | 6,246 | 0.20 |
| `714-walk.ec3`, E-AC-3 | 7.1.4 | 12,230 | 0.38 |

The probe above, with no network and other streams, has 0.18 for 5.1 and 0.43 for 7.1.4. A 2.0
AC-4 stream in SIMPLE mode takes 2.7 times as long per second of audio as E-AC-3 5.1, and a 5.1
AC-4 stream 7 to 21 times.

### Decode time and memory

The first ratio is with ESP-IDF's default allocation policy, the second the same decoder before
D14a's third part, and the third with allocations over 512 bytes sent to PSRAM first
([Allocation policy](#allocation-policy)).

| Stream | Codec mode | To | us/frame | x real time | before D14a's third part | 512-byte policy | First frame s | Peak heap MB | Internal RAM least free KB | Stack left KB |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `20-music-192` | SIMPLE | 2.0 | 22,818 | 0.53 | 0.86 | 0.52 | 0.30 | 0.60 | 8 | 45.3 |
| `20-music-96` | A-SPX | 2.0 | 31,446 | 0.74 | 1.04 | 0.71 | 0.31 | 0.64 | 14 | 44.6 |
| `51-music-384` | SIMPLE | 2.0 | 58,982 | 1.38 | 2.42 | 1.16 | 0.34 | 1.12 | 7 | 44.4 |
| `51-music-384` | SIMPLE | 5.1 | 60,706 | 1.42 | 3.53 | 1.25 | 0.35 | 1.16 | 6 | 44.4 |
| `51-music-192` | A-SPX | 2.0 | 72,006 | 1.69 | 2.11 | 1.52 | 0.34 | 1.13 | 5 | 44.4 |
| `51-music-192` | A-SPX | 5.1 | 77,812 | 1.82 | 4.31 | 1.63 | 0.35 | 1.16 | 5 | 44.4 |
| `51-music-128` | A-SPX, A-CPL 2 | 2.0 | 89,058 | 2.09 | 2.69 | 1.96 | 0.34 | 1.23 | 10 | 44.4 |
| `51-music-128` | A-SPX, A-CPL 2 | 5.1 | 93,487 | 2.19 | 4.83 | 2.05 | 0.34 | 1.27 | 8 | 44.4 |
| `51-music-96` | A-SPX, A-CPL 3 | 2.0 | 192,052 | 4.50 | 4.89 | 3.89 | 0.36 | 1.54 | 6 | 42.1 |
| `51-music-96` | A-SPX, A-CPL 3 | 5.1 | 176,296 | 4.13 | 6.67 | 3.99 | 0.35 | 1.58 | 10 | 42.1 |
| `514-music-256` | A-SPX, A-CPL 2 | 2.0 | 144,199 | 3.38 | 5.21 | 3.28 | 0.38 | 1.86 | 3 | 44.2 |
| `514-music-256` | A-SPX, A-CPL 2 | 5.1.4 | 158,523 | 3.71 | 6.14 | 3.59 | 0.39 | 2.14 | 2 | 44.2 |
| `514-music-512` | A-SPX, S-CPL | 2.0 | 140,619 | 3.29 | 5.14 | 3.04 | 0.40 | 1.88 | 2 | 44.1 |
| `514-music-512` | A-SPX, S-CPL | 5.1.4 | 150,557 | 3.53 | 5.86 | 3.35 | 0.42 | 2.16 | 5 | 44.2 |
| `514-music-768` | S-CPL | 2.0 | 106,677 | 2.50 | 4.31 | 2.22 | 0.41 | 1.90 | 7 | 44.2 |
| `514-music-768` | S-CPL | 5.1.4 | 119,976 | 2.81 | 5.37 | 2.53 | 0.43 | 2.18 | 5 | 44.2 |
| `ims-music-64-23976` | A-SPX, 23.976 fps | 2.0 | 52,321 | 1.25 | 6.50 | 1.21 | 5.90 | 1.00 | 9 | 44.6 |
| `ims-music-64-24` | A-SPX, 24 fps | 2.0 | 38,153 | 0.92 | 5.89 | 0.86 | 0.43 | 0.63 | 9 | 44.6 |
| `ims-music-64-25` | A-SPX, 25 fps | 2.0 | 32,814 | 0.82 | 6.21 | 0.87 | 0.40 | 0.65 | 13 | 44.6 |
| `ims-music-64-2997` | A-SPX, 29.97 fps | 2.0 | 121,919 | 3.65 | 6.45 | 1.26 | 6.00 | 0.95 | 7 | 44.6 |

`20-music-192` and `20-music-96` are 2.0; the `51-` streams are 5.1 at 384, 192, 128 and 96 kbps,
and the `514-` streams 5.1.4 at 256, 512 and 768, each in the mode DEE writes at that rate; the
`ims-` streams are DEE's immersive stereo at 64 kbps at 23.976, 24, 25 and 29.97 fps. Core
decoding of the three 5.1.4 streams, which the standard lets a decoder do for the immersive element:

| Stream | To | us/frame | x real time | before D14a's third part | 512-byte policy | First frame s | Peak heap MB | Internal RAM least free KB | Stack left KB |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `514-music-256` | 2.0 | 87,869 | 2.06 | 3.67 | 1.96 | 0.36 | 1.46 | 4 | 44.2 |
| `514-music-256` | 5.1.4 | 98,644 | 2.31 | 3.93 | 2.21 | 0.37 | 1.65 | 2 | 44.2 |
| `514-music-512` | 2.0 | 125,070 | 2.93 | 4.56 | 2.67 | 0.38 | 1.69 | 3 | 44.2 |
| `514-music-512` | 5.1.4 | 132,090 | 3.10 | 4.70 | 2.88 | 0.39 | 1.88 | 2 | 44.1 |
| `514-music-768` | 2.0 | 89,890 | 2.11 | 3.68 | 1.83 | 0.40 | 1.70 | 12 | 44.2 |
| `514-music-768` | 5.1.4 | 99,561 | 2.33 | 3.83 | 2.07 | 0.41 | 1.88 | 13 | 44.1 |

### What D14a's third part changed

The decoder's time a frame, the decode task's stack and what the play took of PSRAM, before and
after, under ESP-IDF's default policy:

| To | Stream | ms/frame before | ms/frame after | Stack used, KB | PSRAM peak, MB |
|---|---|---:|---:|---:|---:|
| 2.0 | `20-music-192`, SIMPLE | 36.8 | 22.8 | 49.1 to 20.2 | 0.59 to 0.23 |
| 2.0 | `20-music-96`, A-SPX | 44.4 | 31.4 | 49.2 to 20.9 | 0.62 to 0.27 |
| 5.1 | `51-music-384`, SIMPLE | 150.8 | 60.7 | 50.0 to 21.2 | 1.28 to 0.79 |
| 5.1 | `51-music-96`, A-CPL 3 | 284.5 | 176.3 | 49.9 to 23.5 | 1.61 to 1.21 |
| 5.1.4 | `514-music-256`, A-CPL 2 | 262.1 | 158.5 | 50.3 to 21.3 | 2.34 to 1.76 |
| 5.1.4 | `514-music-768`, S-CPL | 229.1 | 120.0 | 50.3 to 21.4 | 2.47 to 1.80 |

A QMF bank takes 1.1 to 1.6 ms a channel-frame now, from 3.4 to 3.8. The decode task uses 20 to
24 KB of its stack, from 49 to 50, so the example's default of 32 KB no longer overflows on the
first play (`sdkconfig.ac4` gives it 40 KB). Internal RAM is still used up under the default
policy: the play takes 367 to 379 KB of it and ends with 2 to 14 KB free at its least, the decoder
having moved its large blocks to PSRAM and kept its many small ones.

### Where a frame goes

Microseconds a frame. The parse and reconstruction columns are the stages' own time less their
first frames' set-up, taken from a 24-frame cut of each stream whose totals the full play's are
differenced against; the other columns are the play's average, in which the first frame's share is
0.1 ms at most. Parse is the frame's syntax and Huffman decoding; reconstruct is the spectral
reconstruction, the stereo and channel processing, the downmix, DRC and the output stage.

| Stream | To | parse | reconstruct | imdct | QMF analysis | QMF synthesis | A-SPX | A-CPL | converter | frame |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `20-music-192` | 2.0 | 6,554 | 7,817 | 3,257 | 2,772 | 2,196 |  |  |  | 22,818 |
| `20-music-96` | 2.0 | 6,150 | 9,364 | 3,258 | 2,783 | 2,544 | 7,089 |  |  | 31,446 |
| `51-music-384` | 2.0 | 12,523 | 22,317 | 13,361 | 7,971 | 2,565 |  |  |  | 58,982 |
| `51-music-384` | 5.1 | 12,362 | 17,418 | 13,396 | 8,049 | 9,287 |  |  |  | 60,706 |
| `51-music-192` | 2.0 | 11,428 | 20,273 | 13,229 | 8,050 | 2,657 | 16,050 |  |  | 72,006 |
| `51-music-192` | 5.1 | 11,531 | 19,341 | 13,188 | 7,787 | 9,107 | 16,600 |  |  | 77,812 |
| `51-music-128` | 2.0 | 8,458 | 17,213 | 13,102 | 7,848 | 2,636 | 10,438 | 28,864 |  | 89,058 |
| `51-music-128` | 5.1 | 8,811 | 15,218 | 13,075 | 7,845 | 8,955 | 10,299 | 28,884 |  | 93,487 |
| `51-music-96` | 2.0 | 7,324 | 16,648 | 6,941 | 7,921 | 27,256 | 6,796 | 118,677 |  | 192,052 |
| `51-music-96` | 5.1 | 7,364 | 18,278 | 7,924 | 8,026 | 9,082 | 6,680 | 118,440 |  | 176,296 |
| `514-music-256` | 2.0 | 13,948 | 26,461 | 20,597 | 15,905 | 2,662 | 21,352 | 42,631 |  | 144,199 |
| `514-music-256` | 5.1.4 | 14,189 | 31,813 | 20,433 | 15,660 | 12,397 | 21,114 | 42,147 |  | 158,523 |
| `514-music-512` | 2.0 | 23,412 | 39,719 | 20,819 | 15,728 | 2,589 | 38,139 |  |  | 140,619 |
| `514-music-512` | 5.1.4 | 23,152 | 40,917 | 20,935 | 15,874 | 12,234 | 37,311 |  |  | 150,557 |
| `514-music-768` | 2.0 | 24,378 | 30,350 | 33,322 | 16,001 | 2,518 |  |  |  | 106,677 |
| `514-music-768` | 5.1.4 | 24,930 | 33,619 | 33,145 | 15,720 | 12,291 |  |  |  | 119,976 |
| `ims-music-64-23976` | 2.0 | 5,642 | 8,225 | 3,020 | 2,597 | 4,587 | 5,461 |  | 22,395 | 52,321 |
| `ims-music-64-24` | 2.0 | 5,643 | 10,431 | 3,984 | 2,614 | 2,328 | 5,556 |  | 7,321 | 38,153 |
| `ims-music-64-25` | 2.0 | 5,563 | 7,199 | 2,152 | 2,764 | 2,374 | 5,301 |  | 7,169 | 32,814 |
| `ims-music-64-2997` | 2.0 | 5,253 | 7,150 | 1,920 | 2,163 | 2,119 | 4,690 |  | 98,547 | 121,919 |

A 2.0 SIMPLE frame is 22.8 ms: parse 6.6, reconstruction 7.8, the inverse transform 3.3 and the QMF
banks 5.0, which are 22% of it and 29% of a 5.1 SIMPLE frame's 60.7. The inverse transform is 13.1 to 13.4
ms at 5.1 in three modes and A-CPL mode 3 118 ms, two thirds of that play's frame. Parse grows with
the bit rate: 5.3 to 6.6 ms in stereo, 7.3 to 12.5 at 5.1 and 13.9 to 24.9 at
5.1.4. A-SPX takes 4.7 to 7.1 ms in stereo, 6.7 to 16.6 at 5.1 and 21.1 to 38.1
at 5.1.4. A-CPL mode 2 takes 29 ms at 5.1 (43 at
5.1.4), the same at 2.0 as at 5.1 because its decorrelators run before the fold. The QMF banks
take 1.1 to 1.4 ms a channel-frame for analysis and 1.1 to 1.6 for synthesis, with exceptions under
the default policy: synthesis in `51-music-96` at 2.0 took 13.6 ms a channel (1.3 with the 512-byte
policy), and 2.3 in the 23.976 fps stream (1.5), `514-music-768`'s inverse
transform 2.3 ms a call at both layouts (1.2) and, now that the converter is fast enough to show
it, the converter of the 29.97 fps stream, 98.5 ms a frame (17.5).

### Allocation policy

ESP-IDF's `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` defaults to 16 KB: an allocation of up to that size
goes to internal RAM while any is left. Before D14a's third part the decoder made so many blocks of
that size that a play took 373 to 381 KB of internal RAM and ended with none free, and with the
setting at 512 bytes (or at none, which measured the same to 1% on four plays) the decoder took 1.0
to 1.9 times less time a frame, 2.0 in SIMPLE mode 36.8 ms to 31.6 and 5.1 150.8 ms to 82.2. The
QMF banks took 3.3 to 3.8 ms a channel-frame in every play with the 512-byte policy, with their
state in PSRAM, and 3.4 to 3.7 ms in most plays and 7.4 to 17.4 in others with the default, with it
in internal RAM, so that residency alone did not account for the difference. Neither did the
allocator's time, from a link-time count on that decoder (before the third part): 38 allocations and as many frees a frame
at 2.0 in SIMPLE mode (69 at 5.1, 116 at 5.1.4), 0.3 ms a frame in the allocator under both
policies. About 9,400 calls of the compiler's soft-float `double` routines a frame (2.6 ms at a
guessed 100 cycles each) were as many under both.

After D14a's third part the policy matters much less: the 512-byte policy is 1.0 to 1.2 times
faster over the twenty plays, and the plays that ran slower under the default are the sporadic ones
above. The AC-3 and E-AC-3 decoders run 1.1 to 1.7 times slower with it (0.22 for AC-3 5.1, 0.30 for
E-AC-3 5.1 and 0.64 for 7.1.4, from 0.20, 0.20 and 0.38), so `sdkconfig.ac4` keeps ESP-IDF's
default. Why the default costs the AC-4 decoder anything once internal RAM is gone was not
established: the candidates are buffers whose addresses conflict in the caches and the cost of the
allocator's failing first attempts, and the count above points away from the second. With the
converter at `float` the effect is larger where it lands on the converter: the 29.97 fps play's
takes 98.5 ms a frame under the default policy and 17.5 under the 512-byte one, from the same code
and the same table.

### The frame-rate converter

| Stream | Ratio | Samples a frame | us/frame | x real time | Converter us/frame | Before D14a4 | First frame s |
|---|---|---:|---:|---:|---:|---:|---:|
| `ims-music-64-24` | 25/24 | 2,000 | 38,153 | 0.92 | 7,321 | 204,951 | 0.43 |
| `ims-music-64-23976` | 1001/960 | 2,002 | 52,321 | 1.25 | 22,395 | 230,897 | 5.90 |
| `ims-music-64-25` | 15/16 | 1,920 | 32,814 | 0.82 | 7,169 | 208,009 | 0.40 |
| `ims-music-64-2997` | 1001/960 | 1,602 | 121,919 | 3.65 | 98,547 | 184,609 | 6.00 |

Part 1 clause 6.2.15's three ratios are 25/24 (24 fps), 1001/1000 x 25/24 = 1001/960 (23.976 fps,
and 29.97 fps at its own frame length) and 15/16 (25 fps). The converter's polyphase filter is
designed in `double` and, in the decoder's `float` build, kept in `float` (D14a4): each phase's
coefficients, 94 taps at 25/24 and 1001/960 and 100 at 15/16, are computed and normalised in
`double` and rounded to `float` once as the table is built, and an output is the sum of that many
`float` products of the `float` history, added over four lanes in an order that
`dsp/resampler_vector.hpp` fixes, so that the host, the Cortex-M3 leg and the board give the same
sample. Before D14a4 the sum ran in `double` on a part whose FPU is single precision, so that each
multiply and add was a call to a software routine of the compiler's runtime: 185 to 231 ms a frame,
4.9 to 5.5 times real time by itself and 84 to 88% of the frame. It now takes 7.2 to 7.3 ms a frame
at 25/24 and 15/16, 28 to 29 times less (3.7 us for each output sample of the pair, about 7 cycles a
tap), and the frame is 0.92 and 0.82 of its duration: the P4 decodes DEE's immersive stereo at 24
and 25 fps in real time.

At 1001/960 the table is 1,001 phases of 94 taps, 376,376 bytes as `float` (752,752 as `double`),
in PSRAM, and every output reads one phase's 376 bytes from it, a different phase each time. The
converter takes 22.4 ms a frame at 23.976 fps, three times the small tables' time for an output
sample, and 98.5 ms in the 29.97 fps play, where the default policy's sporadic slow stage
([Allocation policy](#allocation-policy)) falls on it: the same play with allocations over 512
bytes sent to PSRAM first takes 17.5 ms (1.26 times real time) and the 23.976 fps play 21.0. The
first frame at 1001/960 still takes 5.9 s, 5.5 s more than an ordinary one, since the table is designed in
`double`: 94,094 evaluations of the Kaiser window and the sinc on the compiler's soft-float
routines. At 25/24 and 15/16 it takes no longer than any other.

### Float output on the host, the Cortex-M3 leg and the board

[Decision 26](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/ac4.md#decisions-of-2026-09-25)
promises identical `float` output everywhere, and the probe's six AC-4 fixtures pin it
(`tests/golden/ac4-probe-pcm-hashes.json`, held on the x86-64 host and on the Cortex-M3 under QEMU).
The board's PCM hash (FNV-1a over the sample bit patterns in the order the player delivered them, the
probe's own) equals all six, played from the same committed streams: `ac4_20_music`
`f860e51f602ae753`, `ac4_20_acpl` `e8b8cfa716dd0e1c`, `ac4_51_music` `6d36d7d2e8a070e9`,
`ac4_51_acpl` `dc9687f3f1be5f67`, `ac4_514_tones` `e3c8eeec6565b940` and `ac4_20_companding`
`5b93c61c57566b0c`, the stream D14a4 added because none of the five before it had companding.

On the twenty ten-second plays above the board's hash equals the host's
`AC3FORGE_DECODE_SCALAR=float` output (GCC 16 and Clang 22 with glibc, and MSVC) on all twenty and on
all six core-decoding plays, under either allocation policy, and equals the Cortex-M3 program's
(arm-none-eabi GCC 14.2.1 and newlib-nano, under QEMU) on the 24-frame cut of each of those 26 plays.
D14b measured it on 15 of the twenty: it differed on `20-music-96` and the four converter streams,
the plays with companding, which is on in stereo A-SPX at these rates. Four 24-frame cuts of such
streams, decoded before D14a's third part on the host with MSVC and with glibc, on a Cortex-M3 program
and on the board, first differed at sample 2,180 of the 25 fps cut, the right channel of the second
output frame: 6.383271739e-06 on the host and 6.383272193e-06 on the M3, one unit in the last place,
and by the 24th frame 8,960 of 92,160 samples differed, the median gap one unit and the largest
12,307. The cause was three calls of `float` `std::pow` and `std::exp2`, whose results the C libraries
do not agree on to the last bit: `pow(level, (1 - alpha) / alpha)` and `exp2(1 / alpha)` in
`pcm/companding.cpp`, and the `exp2` of A-SPX's gain in `pcm/aspx.cpp`. A gain that differs in its
last bit scales a companded QMF sample by a different float, and the synthesis bank spreads the
difference over the frame.

D14a4 took the three through `ac3::internal::scalar_exp2` and `scalar_log2` at `float`, the pair
`hf_generator.cpp`'s gains already use, and left `std::pow` at `double`. It replaced the `hypotf` of
Pseudocode 87's prediction limit with a comparison of the squared magnitude, and gave the converter's
sum the fixed order above. What remains of libm in the `float` decode, read from the symbols of the
Cortex-M3 image, is `sqrtf` and `floorf`, which are exact, and calls at `double` whose results are
rounded to `float` once: `pow`, `exp2`, `log`, `log10`, `log2`, `sin`, `cos`, `fmod` and `lround`, which
build the transforms' and the converter's tables, the DRC, downmix and dialogue enhancement gains, the
noise substitution's amplitudes and A-SPX's frequency tables. A library whose `double` differs in its
last bit changes the `float` only where a value lies within that bit of a rounding boundary of
`float`, about one in 2^29, and none of the plays compared differs.

The four cuts of D14b's comparison, decoded on the tree with D14a4, have one hash each on the host
with MSVC, GCC 16 and Clang 22, on the Cortex-M3 program and on the board:

| Cut | Hash |
|---|---|
| 2.0 SIMPLE | `e5812ac315509622` |
| 5.1 | `6fd5e30cd9430f02` |
| 2.0 A-SPX at 25 fps (converter) | `aed7f679dfeee053` |
| 2.0 A-SPX with companding | `24f1c0e157ad0819` |

### What the decoder holds

Under ESP-IDF's default policy the decoder's large blocks are in PSRAM (0.23 MB at 2.0 in SIMPLE
mode, 0.8 to 1.2 MB at 5.1 and 1.8 MB at 5.1.4) and its many small ones fill internal RAM, which
with the network's makes a peak heap of 0.60 MB, 1.1 to 1.6 MB and 1.9 to 2.2 MB. The decode task
uses 20.2 to 23.5 KB of its stack. The first frame takes 0.30 to 0.53 s; of what it takes beyond a
steady frame, 0.22 to 0.42 s is in the reconstruction stage's own time, which the stage timers do
not split further.

### Building with AC-4

```bash
idf.py -DIDF_TARGET=esp32p4 \
  "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.hw;sdkconfig.p4;sdkconfig.sendspin;sdkconfig.ac4" \
  -DAC3FORGE_STAGE_TIMERS=ON build
```

from `esp-idf/ac3forge/examples/hearth_sink/`. `sdkconfig.ac4` turns on `CONFIG_AC3FORGE_AC4` and a
40 KB decode stack. The measurement image adds `AC3FORGE_EXAMPLE_SINK_NULL`,
`AC3FORGE_EXAMPLE_AC4_PCM_HASH`, `ESP_TASK_WDT_INIT=n`, a 64 KB stack and the network's credentials,
and goes to the board with `tools/hearth/ota.py push`. A play's location can carry `?decoding=core`
for core decoding and `?hash=off` for a play without the hash. A play ends with `ac4.lap` (frames,
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
  phase D14b, of which the [AC-4](#ac-4) section is the measurement, D14a's third part, which
  reworks what it found to cost most, and D14a4, which took the converter and the last libm
  disagreements out of the `float` decode.
- [ESP32-S3](esp32-s3.md) — the "better" tier, hardware-verified, the primary Wi-Fi Sendspin sink.
- [ESP32-C6](esp32-c6.md) — the "good" tier, and the sibling page with the same "no QEMU for this
  part" gap.
- [Bare metal overview](index.md) — how the pages in this section relate.
