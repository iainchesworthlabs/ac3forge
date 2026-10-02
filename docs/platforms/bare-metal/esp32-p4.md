# ESP32-P4

The minimum-footprint decoder on an Espressif ESP32-P4: a dual-core RISC-V (RV32IMAFC) with a
single-precision FPU, 768 KB of L2MEM, and no radio of its own. It decodes in the float tier, as
the [ESP32-S3](esp32-s3.md) does, from the same `esp-idf/iclforge/` component, whose manifest
lists `esp32p4` beside `esp32s3`, `esp32c3` and `esp32c6`.

It is the "best" tier of the shared C6/S3/P4 sink family
([`planning/esp32-sink-tiers.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/esp32-sink-tiers.md)):
closed 2026-09-08 as a replacement for the S3 Wi-Fi Sendspin sink (no on-die radio, the S3 probe
already real-time), reopened 2026-09-21 as a complementary module once a board existed to measure
it on. This page is that plan's Phase P1 — the probe and a board timing table, with no network —
and, in its [AC-4](#ac-4) section, what `hearth_sink` measured on the same board with Wi-Fi up.

Every figure on this page outside its [AC-4](#ac-4) section was measured on a board on 2026-09-23, with no network.

## Status

| | |
|---|---|
| Decode | Correct: all fourteen fixtures, every channel's level within the probe's tolerance, and every float-tier PCM hash matching the values the probe pins for this build. Also all eleven stream-set `714-*` files (a scratch probe copy, not committed — see [Stream set](#stream-set)), levels to the digit against `streams.json`'s own reference |
| Real time, no network | **Every fixture and every stream-set file**, from 0.027x (`ac3_mono`) to 0.448x (`eac3_714_fold`, 7.1.4 folded to Lo/Ro) among the fixtures, up to 0.700x (`714-ecpl`) among the stream set — comfortably inside a 32 ms frame even at this chip's 360 MHz ceiling, not the part's 400 MHz datasheet maximum (see [The chip revision](#the-chip-revision-and-what-it-blocks)) |
| Memory | 514,820 bytes free at boot, largest block 385,024; peak heap across every fixture 195,025 (`eac3_atmos_render`), leaving well over half the free total unused at the worst point measured |
| AC-4 decode | Behind `CONFIG_ICLFORGE_AC4`, off by default. Twenty plays of DEE's streams (2.0, 5.1 and 5.1.4; SIMPLE, A-SPX, A-CPL and S-CPL; the converter's four frame rates) run from an HTTP source with the network up, and the probe's six fixtures decode to its pinned `float` PCM hashes exactly. The board's hash equals the host's (GCC 16, Clang 22 and MSVC) on all twenty plays and on all six core-decoding plays, and the Cortex-M3 leg's on the 24-frame cut of each, since D14a4 took the `float` calls whose last bit differs between C libraries out of libm, see [AC-4](#ac-4) |
| AC-4 real time | 2.0 streams in SIMPLE mode (0.28 of a frame), in A-SPX mode (0.37) and, through the converter, at 24 fps (0.51), 25 fps (0.53), 23.976 fps (0.68) and 29.97 fps (0.68), played to a 2.0 layout; and, since D14e, 5.1 to its own layout in SIMPLE mode (0.64), in A-SPX mode (0.83) and in A-SPX mode with A-CPL mode 2 (0.90), which folded to 2.0 take 0.60, 0.77 and 0.82. A-CPL mode 3 takes 1.13 at 5.1 (1.06 folded to 2.0), 5.1.4 in full decoding 1.55 to 1.89 (1.32 to 1.63 folded) and in core decoding 1.23 to 1.57 (1.04 to 1.38 folded). D14e took a 5.1 frame from 1.17, 1.52, 1.94 and 3.86 times its duration (SIMPLE, A-SPX, A-SPX with A-CPL mode 2 and 3) to those, with the decoder's transforms, A-CPL and output in fewer passes, the compiler's settings and the flash read in quad mode ([What D14e changed](#what-d14e-changed)). Before it D14a6 had taken the low-power SRAM out of the heap, D14a's third part had made a frame 1.1 to 2.5 times faster, D14a4's `float` converter and D14a5's tables had made the converter's streams run. AC-3 and E-AC-3 5.1 through the same image each take 0.18, and E-AC-3 7.1.4 0.36 |
| AC-4 memory | A peak heap of 0.58 MB at 2.0 to 2.2 MB at 5.1.4, with internal RAM used up under ESP-IDF's default allocation policy (1 to 8 KB free at its least, of the 344 to 350 KB of main RAM a play starts with; the low-power SRAM has not been in the heap since D14a6). The decode task uses 19 to 30 KB of a 64 KB stack (30 KB in A-CPL mode 3), from 20 to 24 KB at D14a6 and 49 to 50 KB before D14a's third part |
| Encode | Not measured. Both encoders are floating-point; nothing here rules it out |
| QEMU | Not emulated, see [QEMU](#qemu) |
| CI | The component pack builds for `esp32p4` from its archive, with the AC-4 decoder too (`pack_esp_component.py --with-ac4 --verify --verify-targets esp32p4`); `.github/workflows/_build.yml` builds this probe target (decoder direction) and `hearth_sink` for the part, with and without AC-4, and runs nothing, the same gap the ESP32-C6 leg has. These are in the `esp` lane of `ci.yml`, which runs after a merge to main that changes the ESP32 trees or a tree its component ships (the [lane table](../../ci-lanes.md#lane-table) lists them), and nightly ([CI for many agents](../../ci-agentic.md#the-tiers)) |

## The board

A DFRobot FireBeetle 2 ESP32-P4 (the compact AI-vision SKU: two MIPI FPC connectors for camera
and display, GPIO headers along both edges, no separate UART bridge chip). It carries:

- The ESP32-P4 itself, chip revision v1.3, efuse block revision v0.3 — pre-production silicon,
  not the v3.x this part's mass-production runs ship as (see below).
- An ESP32-C6-MINI-1 module wired to the P4 over SDIO (`GPIO14`-`GPIO19`) for Wi-Fi 6 and
  Bluetooth LE, per DFRobot's documentation. This probe does not touch it. `hearth_sink` does: it
  reaches Wi-Fi through `esp_hosted` over that link ([the example's
  README](https://github.com/iainchesworthlabs/iclforge/blob/main/esp-idf/iclforge/examples/hearth_sink/README.md#on-the-esp32-p4)),
  and the [AC-4](#ac-4) figures were measured that way.
- **Two USB-C connectors**, wired to two different on-die USB peripherals, not one connector
  shared between them: one silkscreened "USB 2.0 OTG", reaching the part's native high-speed
  USB-OTG controller (`SOC_USB_OTG_SUPPORTED`; the ROM's download mode answers here — esptool
  reports "USB mode: USB-OTG" connecting to it); the other reaching
  `SOC_USB_SERIAL_JTAG_SUPPORTED`, the lightweight controller the S3/C3/C6 boards use for their
  console, confirmed by a new composite device (`VID_303A`, `PID_1001`, the same PID those boards
  present) enumerating there the moment the chip boots an application, independent of whether the
  OTG connector is plugged in at all. Building against this board needs both connected: the OTG
  one to flash, the other to read anything back.
- 16 MB of flash (confirmed at boot: `SPI Flash Size: 16MB`; a Winbond part, which `hearth_sink` reads in QIO mode at 80 MHz since D14e, see [Flash mode](#flash-mode)) and 32 MB of PSRAM, per DFRobot's
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

A DFRobot FireBeetle 2 ESP32-P4, chip revision v1.3, 16 MB flash read in DIO mode at 80 MHz (the probe's configuration; `hearth_sink` reads it in QIO mode since D14e, see [Flash mode](#flash-mode)).
ESP-IDF v6.1 and GCC esp-15.2.0_20251204, `-Os` with the decode-critical sources at `-O2`
(`ICLFORGE_MINIMAL_HOT_O2`, the project's default), the task watchdog off, 360 MHz.

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
`esp-idf/iclforge/examples/hearth_sink/www/714-*.ec3`, eleven files, each isolating one Annex E
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

`CONFIG_ICLFORGE_AC4` (off by default, and offered only on a part with a floating-point unit)
builds the AC-4 inspector, core and decoder of `src/ac4`, `src/ac4core` and `src/ac4dec` into the
component, in single precision and in the minimum-footprint profile (`ICLFORGE_MINIMAL_AC4`), and
lets the player read a stream that opens with an AC-4 sync word. It takes the ring, the renderer
and the sinks an AC-3 or E-AC-3 stream takes, with `iclforge::ac4::SyncFrameSplitter` and `iclforge::ac4::Decoder` in
place of their framer and decoders. With the switch off the component builds as it did.
`hearth_sink` plays AC-4 from its HTTP source with `sdkconfig.ac4` in its defaults, and ends each
play with the lines this section's figures come from ([Building](#building-with-ac-4)).

### How it was measured

Every figure in this section is from the board above on 2026-09-29, 2026-09-30, 2026-10-01 and 2026-10-02, at 360 MHz, with
the network up (Wi-Fi over the C6, mDNS, the Sendspin player idle, the HTTP source's fetch task and
the decode task running), 32 MB of PSRAM in the heap and the low-power SRAM not in it (since D14a6, see
[The low-power SRAM](#the-low-power-sram)), the flash read in QIO mode (since D14e, see
[Flash mode](#flash-mode)), the decode task's stack at 64 KB, a null sink
that takes a block and returns at once (it paces only a Sendspin stream's timed writes, so a play
runs as fast as the decoder does), and ESP-IDF's default allocation policy
(`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` at 16 KB) unless a column says otherwise. The main figures
are for the decoder with D14e in it ([What D14e changed](#what-d14e-changed)); the same streams on D14a6's
tree, measured the same way, and the figures from before D14a's third part (the QMF bank on split planes,
the cached bit reader and table Huffman decoder, one transform scratch a substream) are beside them as the
baselines those parts are measured against. The streams are DEE's
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
[the stream set's plan](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/esp32-stream-set.md)
has it: the peak of what the play took from internal RAM and PSRAM together, and the least
internal RAM that was free while it ran. Stack left is the decode task's
`stream.decode_stack_free`, of 64 KB. The stage timers are those of `ICLFORGE_STAGE_TIMERS`: a
marker in the library at each part of the decode, whose self time, a stage's own less the stages
inside it, the stage table gives.

The converter's four plays, the hash comparison and the sixth probe fixture were measured again on
2026-09-30 with D14a4 in the decoder, on the same board, streams and image configuration, and the
converter's four again on 2026-10-01 with D14a5's tables, beside the same plays on D14a4's tree built
the same way; the other plays measured again agree with D14b's rows to 3.5% and are not repeated here.
The twenty plays, the six core-decoding plays, the 24-frame cut of each, the 512-byte policy's column and the AC-3
and E-AC-3 plays were measured again on 2026-10-02 with D14e in the decoder and the flash in QIO mode, on a second
FireBeetle 2 of the same kind (chip revision v1.3), with the same streams, server and image configuration: the tables
below are those figures. Their D14a6 column is D14a6's image played on that board the same day, flashed with its own
bootloader (DIO), and its times agree with the figures of [The low-power SRAM](#the-low-power-sram), which came from the
first board, to 3.4% except `20-music-192` at 2.0, which took 22.1 ms a frame here and 19.2 there. A play's time moves a
little with the boot: ten boots of D14e's image, each followed by a 5.1 SIMPLE play and a 5.1 A-SPX play, gave 28.4 to 29.9 ms
and 36.7 to 37.5 ms a frame (the play's average with its first frame), so a difference of 1 ms between two images
is within what a boot changes.

### What it decodes in real time

At 360 MHz with the network up, the AC-4 decoder keeps up with real time, to a 2.0 layout, for 2.0
streams in SIMPLE mode, which take 0.28 of a frame, in A-SPX mode, which take 0.37, and, through the
converter at 24, 25, 23.976 and 29.97 fps, DEE's immersive stereo, which takes 0.51, 0.53, 0.68 and 0.69.
Since D14e it keeps up at 5.1, to 5.1, in three of the four codec modes: SIMPLE takes 0.64 of a frame's
duration, A-SPX 0.83 and A-SPX with A-CPL mode 2 0.91, and folded to 2.0 they take 0.60, 0.76 and 0.83.
A-CPL mode 3 takes 1.14 (1.06 folded to 2.0) and does not keep up. A 5.1 frame also spends 0.5 ms placing its
blocks on the layout and 0.8 ms in the sink's write (a null sink), about 3% of a frame, which these figures leave
out. 5.1.4 in full decoding takes 1.57 to 1.90 to its own layout and 1.34 to 1.66 folded to 2.0, and core
decoding 1.24 to 1.58 to 5.1.4 and 1.05 to 1.39 to 2.0. The converter's 23.976 and 29.97 fps are the ratio
1001/960, whose table the compiler builds and the converter keeps in PSRAM. Before D14e (D14a6's tree on the same
board) the same plays took 0.52 and 0.66 at 2.0, 0.97, 0.79, 0.80 and 1.00 through the converter at 23.976, 24, 25 and
29.97 fps, 1.17, 1.52, 1.94 and 3.86 at 5.1, 2.4 to 3.4 at 5.1.4 in full decoding and 1.7 to 2.7 in core decoding;
[What D14e changed](#what-d14e-changed) has the parts. Before D14a6 took the low-power SRAM out of the heap the 29.97 fps
stream took 3.51 ([The low-power SRAM](#the-low-power-sram)), and before D14a's third part the plays took 0.86 and 1.04 at
2.0, 3.5 to 6.7 at 5.1, 5.4 to 6.1 at 5.1.4 and 5.9 to 6.5 through the converter: the part made a frame 1.1 to 2.5 times
faster and the converter's streams not at all, D14a4's `float` converter took them from 5.6 to 6.6 to 0.82 to 1.25 (3.65),
and D14a5's tables took the 23.976 fps stream to 1.11.

Beside them, AC-3 and E-AC-3 through the same image, network and server, decoder time as above:

| Stream | To | us/frame | x real time |
|---|---|---:|---:|
| `dee-ac3-51.ac3`, AC-3 | 5.1 | 5,727 | 0.18 |
| `dee-eac3-51.ec3`, E-AC-3 | 5.1 | 5,656 | 0.18 |
| `714-walk.ec3`, E-AC-3 | 7.1.4 | 11,739 | 0.37 |

D14a6's image took 0.19, 0.19 and 0.38 on the same board: the flash in QIO mode takes 4 to 8% off these decoders' time too.
The probe above, with no network and other streams, has 0.18 for 5.1 and 0.43 for 7.1.4. A 2.0
AC-4 stream in SIMPLE mode takes 1.6 times as long per second of audio as E-AC-3 5.1, and a 5.1
AC-4 stream played to 5.1 takes 3.6 to 6.4 times as long.

The board's `float` output equals the host's and the Cortex-M3 leg's on the probe's six fixtures
and the host's on all twenty plays and all six core-decoding plays. D14b measured it on 15 of the
twenty: on the other five, the plays with companding, which is on in stereo A-SPX at these rates,
some samples differed (over 24 frames of one such stream, 8,960 of 92,160, by a median of one unit
in the last place and at most 12,307), until D14a4 took the calls that caused it out of libm
([below](#float-output-on-the-host-the-cortex-m3-leg-and-the-board)).

### Decode time and memory

The first ratio is with ESP-IDF's default allocation policy, the second D14a6's tree measured the same way, the third the
same decoder before D14a's third part (measured, as everything was before D14a6, with the low-power SRAM in the heap and on the
first board), and the fourth with allocations over 512 bytes sent to PSRAM first ([Allocation policy](#allocation-policy)).

| Stream | Codec mode | To | us/frame | x real time | D14a6 | before D14a's third part | 512-byte policy | First frame s | Peak heap MB | Internal RAM least free KB | Stack left KB |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `20-music-192` | SIMPLE | 2.0 | 12,039 | 0.28 | 0.52 | 0.86 | 0.35 | 0.30 | 0.58 | 4 | 46.1 |
| `20-music-96` | A-SPX | 2.0 | 15,800 | 0.37 | 0.66 | 1.04 | 0.43 | 0.31 | 0.63 | 4 | 42.8 |
| `51-music-384` | SIMPLE | 2.0 | 25,781 | 0.60 | 1.07 | 2.42 | 0.72 | 0.31 | 1.02 | 8 | 45.3 |
| `51-music-384` | SIMPLE | 5.1 | 27,400 | 0.64 | 1.17 | 3.53 | 0.73 | 0.31 | 1.06 | 8 | 45.3 |
| `51-music-192` | A-SPX | 2.0 | 32,553 | 0.76 | 1.46 | 2.11 | 0.89 | 0.31 | 1.08 | 2 | 42.8 |
| `51-music-192` | A-SPX | 5.1 | 35,589 | 0.83 | 1.52 | 4.31 | 0.96 | 0.31 | 1.12 | 3 | 42.8 |
| `51-music-128` | A-SPX, A-CPL 2 | 2.0 | 35,536 | 0.83 | 1.85 | 2.69 | 0.95 | 0.31 | 1.21 | 4 | 43.0 |
| `51-music-128` | A-SPX, A-CPL 2 | 5.1 | 38,823 | 0.91 | 1.94 | 4.83 | 1.03 | 0.31 | 1.25 | 2 | 42.8 |
| `51-music-96` | A-SPX, A-CPL 3 | 2.0 | 45,449 | 1.06 | 3.77 | 4.89 | 1.18 | 0.30 | 1.32 | 6 | 35.4 |
| `51-music-96` | A-SPX, A-CPL 3 | 5.1 | 48,689 | 1.14 | 3.86 | 6.67 | 1.26 | 0.31 | 1.36 | 2 | 35.4 |
| `514-music-256` | A-SPX, A-CPL 2 | 2.0 | 66,867 | 1.57 | 3.15 | 5.21 | 1.72 | 0.34 | 1.78 | 1 | 42.9 |
| `514-music-256` | A-SPX, A-CPL 2 | 5.1.4 | 78,306 | 1.83 | 3.42 | 6.14 | 1.98 | 0.35 | 2.06 | 1 | 42.9 |
| `514-music-512` | A-SPX, S-CPL | 2.0 | 70,968 | 1.66 | 2.87 | 5.14 | 1.82 | 0.34 | 1.84 | 1 | 42.9 |
| `514-music-512` | A-SPX, S-CPL | 5.1.4 | 81,240 | 1.90 | 3.19 | 5.86 | 2.08 | 0.36 | 2.13 | 1 | 42.8 |
| `514-music-768` | S-CPL | 2.0 | 57,022 | 1.34 | 2.09 | 4.31 | 1.50 | 0.35 | 1.89 | 1 | 45.0 |
| `514-music-768` | S-CPL | 5.1.4 | 66,947 | 1.57 | 2.40 | 5.37 | 1.73 | 0.36 | 2.18 | 1 | 45.0 |
| `ims-music-64-23976` | A-SPX, 23.976 fps | 2.0 | 28,502 | 0.68 | 0.97 | 6.50 | 0.77 | 0.30 | 0.82 | 5 | 43.0 |
| `ims-music-64-24` | A-SPX, 24 fps | 2.0 | 21,329 | 0.51 | 0.79 | 5.89 | 0.58 | 0.28 | 0.63 | 5 | 43.0 |
| `ims-music-64-25` | A-SPX, 25 fps | 2.0 | 21,257 | 0.53 | 0.80 | 6.21 | 0.61 | 0.30 | 0.64 | 3 | 42.9 |
| `ims-music-64-2997` | A-SPX, 29.97 fps | 2.0 | 22,978 | 0.69 | 1.00 | 6.45 | 0.80 | 0.24 | 0.75 | 6 | 43.0 |

`20-music-192` and `20-music-96` are 2.0; the `51-` streams are 5.1 at 384, 192, 128 and 96 kbps,
and the `514-` streams 5.1.4 at 256, 512 and 768, each in the mode DEE writes at that rate; the
`ims-` streams are DEE's immersive stereo at 64 kbps at 23.976, 24, 25 and 29.97 fps. Core
decoding of the three 5.1.4 streams, which the standard lets a decoder do for the immersive element:

| Stream | To | us/frame | x real time | D14a6 | before D14a's third part | 512-byte policy | First frame s | Peak heap MB | Internal RAM least free KB | Stack left KB |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `514-music-256` | 2.0 | 44,971 | 1.05 | 1.84 | 3.67 | 1.19 | 0.32 | 1.44 | 1 | 42.8 |
| `514-music-256` | 5.1.4 | 52,703 | 1.24 | 2.06 | 3.93 | 1.38 | 0.34 | 1.63 | 1 | 42.8 |
| `514-music-512` | 2.0 | 59,393 | 1.39 | 2.51 | 4.56 | 1.57 | 0.33 | 1.66 | 2 | 42.8 |
| `514-music-512` | 5.1.4 | 67,376 | 1.58 | 2.74 | 4.70 | 1.75 | 0.35 | 1.86 | 1 | 42.8 |
| `514-music-768` | 2.0 | 45,236 | 1.06 | 1.70 | 3.68 | 1.23 | 0.33 | 1.70 | 4 | 45.0 |
| `514-music-768` | 5.1.4 | 52,902 | 1.24 | 1.93 | 3.83 | 1.41 | 0.35 | 1.88 | 1 | 45.0 |

### What D14a's third part changed

The decoder's time a frame, the decode task's stack and what the play took of PSRAM, before and
after, under ESP-IDF's default policy and with the low-power SRAM in the heap, as it was on 2026-09-29 (the
tables above are lower since D14a6):

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
first play (`sdkconfig.ac4` gives it 40 KB). Internal RAM was still used up under the default
policy: the play took 367 to 379 KB of it, the low-power SRAM counted, and ended with 2 to 14 KB free at its least,
the decoder having moved its large blocks to PSRAM and kept its many small ones. With the low-power SRAM out of the
heap it takes 339 to 349 KB and ends with 1 to 11 KB free.

### What D14e changed

D14e is three changes to the decoder and two settings of the build and the board: the transforms in fewer passes, A-CPL's
interpolation once for each run of subbands, the reconstruction and the output without their copies, the compiler's
`-O3` for the kernels and `-O2` for the decoder's hot files, and the flash read in QIO mode ([Flash mode](#flash-mode)). None
moves a bit of the PCM ([below](#float-output-on-the-host-the-cortex-m3-leg-and-the-board)). The decoder's time a frame under
ESP-IDF's default policy, D14a6's tree and D14e's, on the same board in the same session:

| Stream | To | D14a6 us/frame | D14e us/frame | Change | D14a6 x real time | D14e x real time |
|---|---|---:|---:|---:|---:|---:|
| `20-music-192` | 2.0 | 22,136 | 12,039 | -46% | 0.52 | 0.28 |
| `20-music-96` | 2.0 | 28,270 | 15,800 | -44% | 0.66 | 0.37 |
| `51-music-384` | 2.0 | 45,818 | 25,781 | -44% | 1.07 | 0.60 |
| `51-music-384` | 5.1 | 49,913 | 27,400 | -45% | 1.17 | 0.64 |
| `51-music-192` | 2.0 | 62,286 | 32,553 | -48% | 1.46 | 0.76 |
| `51-music-192` | 5.1 | 64,836 | 35,589 | -45% | 1.52 | 0.83 |
| `51-music-128` | 2.0 | 78,849 | 35,536 | -55% | 1.85 | 0.83 |
| `51-music-128` | 5.1 | 82,568 | 38,823 | -53% | 1.94 | 0.91 |
| `51-music-96` | 2.0 | 160,758 | 45,449 | -72% | 3.77 | 1.06 |
| `51-music-96` | 5.1 | 164,748 | 48,689 | -70% | 3.86 | 1.14 |
| `514-music-256` | 2.0 | 134,334 | 66,867 | -50% | 3.15 | 1.57 |
| `514-music-256` | 5.1.4 | 145,851 | 78,306 | -46% | 3.42 | 1.83 |
| `514-music-512` | 2.0 | 122,550 | 70,968 | -42% | 2.87 | 1.66 |
| `514-music-512` | 5.1.4 | 135,933 | 81,240 | -40% | 3.19 | 1.90 |
| `514-music-768` | 2.0 | 89,302 | 57,022 | -36% | 2.09 | 1.34 |
| `514-music-768` | 5.1.4 | 102,241 | 66,947 | -35% | 2.40 | 1.57 |
| `ims-music-64-23976` | 2.0 | 40,681 | 28,502 | -30% | 0.97 | 0.68 |
| `ims-music-64-24` | 2.0 | 32,967 | 21,329 | -35% | 0.79 | 0.51 |
| `ims-music-64-25` | 2.0 | 31,801 | 21,257 | -33% | 0.80 | 0.53 |
| `ims-music-64-2997` | 2.0 | 33,455 | 22,978 | -31% | 1.00 | 0.69 |

What each part bought at 5.1 and at two other plays, one image of the tree for each row with the parts above it, each
flashed with its own bootloader and played in the same session (the decoder's microseconds a frame without the first frame,
and times a frame's duration):

| | 5.1 SIMPLE | 5.1 A-SPX | 5.1 A-CPL 2 | 5.1 A-CPL 3 | 2.0 SIMPLE | 25 fps |
|---|---:|---:|---:|---:|---:|---:|
| D14a6 | 52,937 (1.24) | 65,797 (1.54) | 82,756 (1.94) | 164,627 (3.86) | 19,244 (0.45) | 31,920 (0.80) |
| and the three code commits | 41,390 (0.97) | 48,144 (1.13) | 50,994 (1.20) | 60,398 (1.42) | 16,000 (0.38) | 27,079 (0.68) |
| and `-O3` for the kernels and `-O2` for the decoder's files | 35,026 (0.82) | 42,921 (1.00) | 46,329 (1.08) | 56,153 (1.32) | 14,905 (0.35) | 25,540 (0.64) |
| and the flash in QIO mode (D14e) | 27,399 (0.64) | 35,621 (0.83) | 38,932 (0.91) | 48,255 (1.13) | 11,148 (0.26) | 21,206 (0.53) |
| D14e with the decoder's files at `-Os` | 31,217 (0.73) | 37,496 (0.88) | 40,502 (0.95) | 50,175 (1.18) | 11,737 (0.28) | 21,768 (0.54) |

The stage timers say where each part landed, for the two ends of the 5.1 range, in microseconds a frame (the play's average
with its first frame, so a column's stages differ from [Where a frame goes](#where-a-frame-goes) by the first frame's share):

| Stage | 5.1 SIMPLE: D14a6 | 5.1 SIMPLE: code | 5.1 SIMPLE: compiler | 5.1 SIMPLE: QIO | 5.1 A-CPL 3: D14a6 | 5.1 A-CPL 3: code | 5.1 A-CPL 3: compiler | 5.1 A-CPL 3: QIO |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| parse | 13,669 | 11,194 | 9,419 | 6,543 | 7,409 | 6,699 | 6,557 | 4,224 |
| reconstruct | 15,663 | 8,141 | 7,439 | 5,654 | 12,605 | 9,904 | 9,645 | 7,813 |
| inverse transform | 7,990 | 5,428 | 5,056 | 4,293 | 6,872 | 4,253 | 4,128 | 3,690 |
| QMF analysis | 8,565 | 9,003 | 7,114 | 5,938 | 7,930 | 8,115 | 6,572 | 5,841 |
| QMF synthesis | 7,962 | 8,526 | 6,940 | 5,959 | 7,082 | 7,394 | 6,122 | 5,473 |
| A-SPX |  |  |  |  | 6,483 | 3,252 | 3,146 | 2,558 |
| A-CPL |  |  |  |  | 116,577 | 21,550 | 20,814 | 19,566 |

**The transforms.** The FFT's passes are one function for each radix and direction (`dsp/fft_kernels.hpp`), where the plan ran a
generic butterfly with a runtime radix and a branch on the direction at every element. `Imdct::inverse_overlap()` takes a
full-length block that follows a full-length block from the spectrum to the PCM in one pass: the pre-twiddle is the first
pass's read, and the post-twiddle, the unfolding, the window and the overlap-add are one loop, with no 2N-sample block
between them. With these the inverse transform takes 5.4 ms a 5.1 SIMPLE frame from 8.0, and 4.3 once
the compiler's settings and the flash are in; the QMF banks are the compiler's and the flash's (8.6 and 8.0 ms to 5.9 and 6.0),
and A-SPX's cubic fit, which made four orthonormal polynomials in `double` at every frame (4n calls of `std::pow` and a hundred
multiplies and adds of the compiler's software routines for each channel), keeps them for the number of points a channel
last had: A-SPX's stage takes 8.3 ms a 5.1 frame from 16.0 with that alone, the only change to A-SPX in the three commits.

**A-CPL.** Pseudocode 109 gives the subbands that share a parameter band and `acpl_param_prev` the same value at every slot, to
the bit, and the stage evaluated it for each of the 64 subbands in `double`, which on this part is a call into the compiler's
software for every operation (mode 3 interpolates seventeen parameters: 35,000 evaluations a frame). `acpl::Interpolator`
evaluates the pseudocode's expression operation for operation once for each run, with the ramp's `(ts + 1)` a table entry and
the division by 16 or 32 slots a multiplication by its exact reciprocal; the stage forms each slot's coefficients once for a
run and narrows them to `Real` once; and the decorrelator narrows Tables 199 to 201 once, in its constructor. The stage takes
7.7 ms in mode 2 from 28.3 and 21.6 in mode 3 from 116.6 with these, and 6.7 and 19.9 with the rest.

**The reconstruction and the output.** A frame that SIMPLE mode passes through whole is read from `ext` by the synthesis
where nothing else takes its matrix (no dialogue enhancement, mixing, side chain, objects, DRC level gain or downmix), and is
not copied to `out` first; a frame of one long block is ungrouped by swapping buffers; the 256 scale factor gains are a table
and not a `std::pow` for each band of each frame (250 a frame at 5.1); the spectra that concealment repeats change places
with the frame's in place of a copy of 48 KB; the delay queue moves each sample once; an element's tracks, 15 KB each, are
reserved and not moved 63 times a frame; and the two or four lines of a Huffman codeword are stored by name, where GCC had
made a call into ROM's `memcpy` for 8 or 16 bytes. The reconstruction takes 8.1 ms a 5.1 SIMPLE frame from 15.7 with these,
and parse 11.2 from 13.7.

**The compiler.** `src/ac4core`'s kernels (the FFT, the inverse transform, the QMF banks, the converter, the synthesis, A-SPX's
generator and A-CPL) are built at `-O3` under `ICLFORGE_MINIMAL_HOT_O2` from `-O2`, which unrolls their loops and interleaves
the independent multiplies that a rolled loop leaves waiting on the FPU of an in-order core: 2.7 to 2.9 ms a 5.1 frame for
9 KB of flash. Thirteen of `src/ac4dec`'s translation units (the Huffman and scale factor reading, the reconstruction, the
stereo and downmix passes) are built at `-O2` where they stayed at `-Os`: 1.6 to 3.8 ms a 5.1 frame for 71 KB of flash, and
1.8 KB more of the decode task's stack. `-O3` on those files gained nothing over `-O2` and cost 73 KB more.

**A-CPL mode 3 does not keep up.** It takes 1.14 times a frame's duration, 6.0 ms over, and its stage takes 19.9 ms of the 48.7. Some
7 to 11 ms of that is the compiler's soft-float `double` routines at 60 to 100 cycles a call, which the interpolation needs for its bits
(a profile by caller of the stage's image counted 8,000 evaluations of three operations and 11,500 narrowings to `float` a frame,
48,000 calls of those routines in all, 43,000 of them in the stage); the decorrelators, the ducker and the loops over the subbands are `float`, and the
rest, which was not read separately, is the stage's passes over 16 KB buffers in PSRAM. A ramp scaled by the reciprocal of the slot count (the multiplication by a power of two is
exact and commutes with the rounding) would save one multiplication in three of the evaluations, about 2 ms, which is not
enough, and the narrowings and the sums would need `double` hardware or the stage's arithmetic changed, which changes the PCM.

**Tried and not taken.**

| Setting | Effect at 5.1 | Cost |
|---|---|---|
| `-funroll-loops` on the kernels at `-O2` | 1.3 to 3.2 ms | 21 KB of flash over `-O2` |
| `-O3 -funroll-loops` on the kernels | the same as `-O3` | 31 KB over `-O2` |
| `-O2 -funroll-loops -fschedule-insns -fsched-pressure` on the kernels | 1.3 to 2.8 ms | 21 KB over `-O2` |
| `-O3` on the decoder's files too | none over `-O2` there | 73 KB over `-O2` |
| L2 cache of 256 KB | 2.6 to 3.7 ms | 128 KB of internal RAM, 1 KB free at the end of a play |
| L2 line of 128 bytes | 1.3 to 2.2 ms on three plays and 0.6 ms more on a fourth, within a boot's variation | three plays of nine stopped on that image (not traced; its `esp_hosted` buffers are PSRAM that DMA reads) |
| The cache's autoload prefetcher, from the ROM | not measured | enabling it from a scratch image panicked the board, and the setting survived the CPU reset into a boot loop in PSRAM initialisation until a watchdog reset cleared it |
| Flash at 120 MHz | not measured | `IDF_EXPERIMENTAL_FEATURES` for this part, with the flash's high-performance mode |

Not attempted: the second core, idle in a play (87% of its samples in a profile), which would take a job hook in the library and a
second set of working buffers; and the P4's vector extension, which works on integers and so cannot form the `float` kernels' bits.

### Where a frame goes

Microseconds a frame. The parse and reconstruction columns are the stages' own time less their
first frames' set-up, taken from a 24-frame cut of each stream whose totals the full play's are
differenced against; the other columns are the play's average, in which the first frame's share is
0.1 ms at most. Parse is the frame's syntax and Huffman decoding; reconstruct is the spectral
reconstruction, the stereo and channel processing, the downmix, DRC and the output stage.

| Stream | To | parse | reconstruct | imdct | QMF analysis | QMF synthesis | A-SPX | A-CPL | converter | frame |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `20-music-192` | 2.0 | 4,373 | 2,139 | 1,422 | 2,231 | 1,830 |  |  |  | 12,039 |
| `20-music-96` | 2.0 | 4,063 | 3,350 | 1,341 | 2,172 | 1,934 | 2,894 |  |  | 15,800 |
| `51-music-384` | 2.0 | 6,810 | 6,591 | 4,284 | 6,001 | 2,012 |  |  |  | 25,781 |
| `51-music-384` | 5.1 | 6,625 | 4,497 | 4,209 | 5,963 | 5,902 |  |  |  | 27,400 |
| `51-music-192` | 2.0 | 6,103 | 7,777 | 4,071 | 5,845 | 1,984 | 6,630 |  |  | 32,553 |
| `51-music-192` | 5.1 | 5,981 | 7,350 | 4,062 | 5,893 | 5,525 | 6,587 |  |  | 35,589 |
| `51-music-128` | 2.0 | 5,046 | 7,505 | 3,931 | 5,931 | 1,976 | 4,134 | 6,820 |  | 35,536 |
| `51-music-128` | 5.1 | 4,973 | 7,320 | 3,896 | 5,936 | 5,574 | 4,172 | 6,749 |  | 38,823 |
| `51-music-96` | 2.0 | 4,369 | 7,027 | 3,686 | 5,832 | 2,001 | 2,596 | 19,693 |  | 45,449 |
| `51-music-96` | 5.1 | 4,395 | 6,574 | 3,673 | 5,799 | 5,484 | 2,618 | 19,920 |  | 48,689 |
| `514-music-256` | 2.0 | 9,943 | 12,226 | 8,738 | 11,491 | 2,007 | 9,342 | 12,867 |  | 66,867 |
| `514-music-256` | 5.1.4 | 10,107 | 15,243 | 8,744 | 11,579 | 9,763 | 9,460 | 13,194 |  | 78,306 |
| `514-music-512` | 2.0 | 17,642 | 15,927 | 9,192 | 11,723 | 2,020 | 14,374 |  |  | 70,968 |
| `514-music-512` | 5.1.4 | 17,637 | 18,576 | 9,160 | 11,723 | 9,674 | 14,376 |  |  | 81,240 |
| `514-music-768` | 2.0 | 19,142 | 14,518 | 9,402 | 11,870 | 1,982 |  |  |  | 57,022 |
| `514-music-768` | 5.1.4 | 18,555 | 17,516 | 9,373 | 11,717 | 9,687 |  |  |  | 66,947 |
| `ims-music-64-23976` | 2.0 | 3,389 | 3,051 | 1,415 | 1,938 | 2,013 | 2,380 |  | 14,108 | 28,502 |
| `ims-music-64-24` | 2.0 | 3,467 | 2,956 | 1,439 | 1,946 | 1,662 | 2,379 |  | 7,320 | 21,329 |
| `ims-music-64-25` | 2.0 | 3,409 | 2,990 | 1,195 | 2,048 | 1,794 | 2,467 |  | 7,181 | 21,257 |
| `ims-music-64-2997` | 2.0 | 3,296 | 2,579 | 998 | 1,614 | 1,577 | 1,964 |  | 10,761 | 22,978 |

A 2.0 SIMPLE frame is 12.0 ms: parse 4.4, reconstruction 2.1, the inverse transform 1.4 and the QMF
banks 4.1, which are 34% of it and 43% of a 5.1 SIMPLE frame's 27.4. The inverse transform takes 3.7 to 4.2
ms at 5.1 in the four modes and 8.7 to 9.4 at 5.1.4, and A-CPL mode 3 takes 19.9 ms, 41% of its play's frame
(116.8 ms and 71% at D14a6). Parse grows with the bit rate: 3.3 to 4.4 ms in stereo, 4.4 to 6.8 at
5.1 and 9.9 to 19.1 at 5.1.4. A-SPX takes 2.0 to 2.9 ms in stereo, 2.6 to 6.6 at 5.1 and 9.3 to 14.4
at 5.1.4. A-CPL mode 2 takes 6.7 to 6.8 ms at 5.1 (12.9 to 13.2 at
5.1.4), the same at 2.0 as at 5.1 because its decorrelators run before the fold. The QMF banks
take 0.8 to 1.1 ms a channel-frame for analysis and 0.8 to 1.0 for synthesis in every play. Before D14a6
some plays had stages that took 1.4 to 10 times as long: synthesis in `51-music-96` at 2.0 took 13.6 ms a channel
and 2.3 in the 23.976 fps stream, the inverse transform 13.1 to 13.4 ms a frame at 5.1 in three of the four modes
and 20.4 to 33.3 at 5.1.4 (2.3 ms a call in `514-music-768`) and, now that the converter was fast enough to
show it, the converter of the 29.97 fps stream, 93.8 ms a frame.
[The low-power SRAM](#the-low-power-sram) has the cause.

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

After D14a's third part the 512-byte policy was 1.0 to 1.2 times faster over the twenty plays, and one play was
an exception: the 29.97 fps play's converter took 93.8 ms a frame under the default policy and 12.2 under the
512-byte one, from the same code and the same table. It repeated: the play's converter took 98.5 to 98.6 ms in each
of four runs under the default policy on D14a4's tree, 98.5 on that tree measured again for D14a5 and
93.8 with D14a5's tables, and 17.2 to 17.5 in each run under the 512-byte one on D14a4's tree and
12.2 with D14a5's tables. D14a5's runs ruled out the table (the
[converter section](#the-frame-rate-converter) has them), and the cause was left open. It is the heap's
low-power SRAM, [below](#the-low-power-sram), which the 512-byte policy seldom reaches: it keeps 293 to 298 KB of
internal RAM free at its least where the default keeps 1 to 11.

With that out of the heap the default policy is the faster of the two on all twenty-six plays: the 512-byte policy
takes 1.04 to 1.16 times as long a frame over the twenty and 1.05 to 1.08 over the six core-decoding
plays, and its converter 1 to 2% longer at each frame rate. The AC-3 and E-AC-3 decoders run
1.1 to 1.7 times slower with it (0.21 for AC-3 5.1, 0.31 for E-AC-3 5.1 and
0.65 for 7.1.4, against 0.19, 0.19 and 0.38 with the default), so `sdkconfig.ac4` keeps ESP-IDF's default. The
tables above give the 512-byte policy's figures beside the default's. The advantage that policy had before D14a's
third part, 1.0 to 1.9 times, may have been the same cause, since the default is now the faster on the current
decoder; the tree before the third part was not measured again.

With D14e's decoder the default policy is the faster of the two on all twenty-six plays again, by more: the 512-byte
policy takes 1.08 to 1.24 times as long a frame over the twenty and 1.11 to 1.16 over the six core-decoding plays,
its converter 1.4 to 12% longer at the four frame rates, and the AC-3 and E-AC-3 decoders 1.05 to 1.5 times as long
(0.19, 0.25 and 0.54 against 0.18, 0.18 and 0.37 with the default). A play leaves 281 to 298 KB of internal RAM free
at its least under the 512-byte policy and 1 to 8 KB under the default. At 5.1 the 512-byte policy takes
0.73, 0.96, 1.03 and 1.26 times a frame's duration in SIMPLE mode, A-SPX, A-SPX with A-CPL mode 2 and A-CPL mode 3, against 0.64, 0.83, 0.91
and 1.14: the third does not keep up under it. `sdkconfig.ac4` keeps ESP-IDF's default.

### The low-power SRAM

ESP-IDF adds the ESP32-P4's low-power SRAM to the heap by default: 32 KB at 0x50108000, which it calls RTC fast
memory (`CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP`, whose help says it "does not have much performance impact").
It joins as internal memory of the lowest priority (`components/heap/port/esp32p4/memory_layout.c`), so an
allocation that is tried in internal RAM first (a block of up to 16 KB under the default policy, 512 bytes under the other)
and that main RAM cannot satisfy comes from it before PSRAM is tried, and nothing in this component asks for it by name
(`MALLOC_CAP_RTCRAM`). The HP cores reach it over the LP bus with no
cache between. A loop that touches nothing else, in a scratch image that adds a `/bench` route to the example's
control surface (not committed), took these cycles an access at 360 MHz:

| Memory | Load, the next one dependent on it | Load in sequence | Store in sequence |
|---|---:|---:|---:|
| Main RAM, 8 KB | 6.0 | 7.0 | 6.0 |
| PSRAM, 8 KB, in the L1 data cache | 6.9 | 7.0 |  |
| PSRAM, a 16 MB chase, every load a miss | 167.2 |  |  |
| PSRAM, 4 MB read in sequence, none of it cached | | 18.2 |  |
| Low-power SRAM, 8 KB | 173.9 | 194.9 | 173.3 |

An access to the low-power SRAM takes 28 to 29 times what one to main RAM does, whatever the pattern. PSRAM behind its
caches takes the same as main RAM when the data is cached and 18 cycles a word when a sequence of reads is not.

The AC-4 decoder takes nearly all of main RAM: 343 to 350 KB is free when a play starts and 1 to 11 KB at its least, so
what it allocates after that comes from the low-power SRAM, then from PSRAM. In each of the four converter plays
25.3 to 31.5 KB of the SRAM's 32.6 were in use at the most. Whether a hot buffer was among those allocations depended on how
full main RAM was at that moment, which is why a play was slow after some plays and not after others, and why two
images of the same code could differ. In the 29.97 fps play, in a scratch image that prints the converter's addresses,
one channel's history was at 0x50108364 and the output vector the two channels share at 0x5010ad6c, both in the
low-power SRAM, and the converter took 93.6 ms a frame. In the same image built without the region both were in PSRAM
(0x480f1a98 and 0x480efa94) and it took 11.9 ms. The table was in PSRAM (0x480bb7c8 and 0x480bbd08) in both.

The change is one line of the example's configuration, in `sdkconfig.p4`:

```
CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP=n
```

The heap is 32,640 bytes smaller, what spilled into the SRAM goes to PSRAM behind the cache, and the allocation policy
stays ESP-IDF's default. A project of one's own that decodes AC-4 on this part should set the same option. The `float`
PCM did not move: the board's hash of 56 plays and cuts equals D14a5's, and the probe's six fixtures equal their pins.
The decoder's time a frame in microseconds, before (the figures this page had) and after:

| Stream | To | Before | After | Change |
|---|---|---:|---:|---:|
| `20-music-192` | 2.0 | 22,818 | 19,151 | -16% |
| `20-music-96` | 2.0 | 31,446 | 27,334 | -13% |
| `51-music-384` | 2.0 | 58,982 | 44,759 | -24% |
| `51-music-384` | 5.1 | 60,706 | 48,829 | -20% |
| `51-music-192` | 2.0 | 72,006 | 60,488 | -16% |
| `51-music-192` | 5.1 | 77,812 | 64,613 | -17% |
| `51-music-128` | 2.0 | 89,058 | 78,777 | -12% |
| `51-music-128` | 5.1 | 93,487 | 82,721 | -12% |
| `51-music-96` | 2.0 | 192,052 | 159,948 | -17% |
| `51-music-96` | 5.1 | 176,296 | 161,737 | -8% |
| `514-music-256` | 2.0 | 144,199 | 133,285 | -8% |
| `514-music-256` | 5.1.4 | 158,523 | 147,294 | -7% |
| `514-music-512` | 2.0 | 140,619 | 122,733 | -13% |
| `514-music-512` | 5.1.4 | 150,557 | 135,646 | -10% |
| `514-music-768` | 2.0 | 106,677 | 88,953 | -17% |
| `514-music-768` | 5.1.4 | 119,976 | 101,910 | -15% |
| `ims-music-64-23976` | 2.0 | 46,351 | 41,105 | -11% |
| `ims-music-64-24` | 2.0 | 38,057 | 32,781 | -14% |
| `ims-music-64-25` | 2.0 | 32,748 | 31,860 | -3% |
| `ims-music-64-2997` | 2.0 | 117,017 | 33,504 | -71% |

The six core-decoding plays take 11 to 19% less. The stages that stood out before, in microseconds a frame (the
table in [Where a frame goes](#where-a-frame-goes) has the rest):

| Stage | Stream | To | Before | After |
|---|---|---|---:|---:|
| converter | `ims-music-64-2997` | 2.0 | 93,761 | 11,978 |
| QMF synthesis | `51-music-96` | 2.0 | 27,256 | 2,576 |
| QMF synthesis | `ims-music-64-23976` | 2.0 | 4,658 | 2,727 |
| inverse transform | `514-music-768` | 2.0 | 33,322 | 15,060 |
| inverse transform | `514-music-768` | 5.1.4 | 33,145 | 15,012 |
| inverse transform | `51-music-384` | 5.1 | 13,396 | 7,317 |

The addresses of the stages' buffers were read in one play, the 29.97 fps one, and for the converter only; that the
others went away with the region is the evidence for them. Not measured: the 8 KB of tightly coupled memory at
0x30100000, which is also in the heap at the same low priority and stays there; the same option on the ESP32-S3 and
the ESP32-C6, whose heaps have their RTC fast memory by default too; and the tree before D14a's third part, whose QMF
banks took 7.4 to 17.4 ms in some plays and may have had the same cause.

### Flash mode

`sdkconfig.p4` reads the flash in QIO mode since D14e (`CONFIG_ESPTOOLPY_FLASHMODE_QIO`; ESP-IDF's default is DIO): four data lines to the
flash where DIO has two, at the same 80 MHz. The board's flash is a Winbond part, and the second stage bootloader says so as it sets the
part's quad-enable bit (`qio_mode: Enabling QIO for flash chip WinBond` and `SPI Mode: QIO` in its log). The decoder's code (1.4 MB of
`.flash.text`) and the constants it reads (0.8 MB of `.flash.rodata`, among them the Huffman and QMF tables) are larger than the 16 KB
instruction cache and the 128 KB L2 cache, which the 0.7 MB of data a frame also passes through, so a frame refills about 2,300 64-byte
lines of code from flash (the L2's access counters, in a scratch image), 1,000 of them in parse. A line is 256 clocks of the flash's at
80 MHz in DIO, 1,150 of the CPU's, plus the command and the address, and half that in QIO. The same application image (built for QIO)
under D14a6's DIO bootloader and under the QIO one, six plays, the decoder's microseconds a frame without the first frame and times a
frame's duration:

| Stream | To | DIO us/frame | QIO us/frame | Less | DIO x real time | QIO x real time |
|---|---|---:|---:|---:|---:|---:|
| `51-music-384` | 5.1 | 35,026 | 27,399 | 7,627 (22%) | 0.82 | 0.64 |
| `51-music-192` | 5.1 | 42,921 | 35,621 | 7,300 (17%) | 1.00 | 0.83 |
| `51-music-128` | 5.1 | 46,329 | 38,932 | 7,397 (16%) | 1.08 | 0.91 |
| `51-music-96` | 5.1 | 56,153 | 48,255 | 7,898 (14%) | 1.32 | 1.13 |
| `20-music-192` | 2.0 | 14,905 | 11,148 | 3,757 (25%) | 0.35 | 0.26 |
| `ims-music-64-25` | 2.0 | 25,540 | 21,206 | 4,334 (17%) | 0.64 | 0.53 |

QIO takes 3.8 ms off a 2.0 SIMPLE frame and 7.3 to 7.9 off a 5.1 one, 14 to 25% in every play measured, to the same PCM hash. The AC-3 and
E-AC-3 decoders take 4 to 8% less time with it, and the converter's 25 fps stream 17% less.

The mode is the bootloader's, and the application image carries none of it: the application above, built for QIO, ran at the DIO column's
times under the DIO bootloader and started and played as it does under the QIO one, so an update of the application over the network is
safe on a board flashed either way. An update replaces the application and keeps the bootloader that is on the board, so a board flashed
before D14e decodes at the DIO figures until it is flashed again over USB with its bootloader (`idf.py flash`, or `esptool write-flash
@flash_args` from a build directory). The QIO bootloader is 944 bytes larger, 24,368 bytes at the Info log level against a window of 24,576
(`CONFIG_BOOTLOADER_LOG_LEVEL_WARN` gives 2.5 KB back; [the OTA plan](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/esp32-ota.md)
has the table). A bootloader that cannot set a flash chip's quad-enable bit stays in DIO (`sdkconfig.c6`, which reads its flash this way,
says so), and a project whose flash has no quad mode sets `CONFIG_ESPTOOLPY_FLASHMODE_DIO=y` after `sdkconfig.p4`. Not measured: a flash
clock of 120 MHz, which ESP-IDF offers this part only under `IDF_EXPERIMENTAL_FEATURES`, with the flash's high-performance mode.

### Paced output

The tables above use a null sink, which takes a block and returns at once. The example's I2S sink paces: its write returns when
the frame is in the DMA queue, and the queue holds about 21 ms of audio by default (4 descriptors of 240 frames; the sink's own line reads
`dma=30x32 frames (20 ms)`). The decode and the
write are in one task, so a frame whose decode takes more than the queue holds leaves it dry for the difference, whatever the
average does against the frame's duration. A 5.1 A-SPX stream folded to 2.0, which decodes in 0.76 of a frame, took 13.5 s for
10.1 s of audio through the default queue, and the sink counted 236 underruns and 3.5 s of silence. With 12 descriptors of 256
frames (64 ms, 24 KB of internal RAM at 2.0) it played in 10.0 s with one underrun of 11 ms, the first frame's, which takes
0.31 s to decode. `sdkconfig.p4` carries the 64 ms queue since D14e. The plays, through the I2S sink at 2.0 on the image as
`sdkconfig.p4` and `sdkconfig.ac4` build it (a 40 KB decode stack, the task watchdog on), with the hash on; the underruns and the
dry time are the sink's own count (`sink.underruns` and `sink.dry_ms` in a play's last lines):

| Stream | To | Decoder x real time | Queue | Seconds for 10.1 s of audio | Underruns | Dry ms |
|---|---|---:|---|---:|---:|---:|
| `20-music-192` | 2.0 | 0.28 | default, 20 ms | 10.0 | 0 | 0 |
| `51-music-192` | 2.0 | 0.76 | default, 20 ms | 13.5 | 236 | 3,474 |
| `20-music-192` | 2.0 | 0.28 | 64 ms | 10.0 | 0 | 0 |
| `51-music-384` | 2.0 | 0.60 | 64 ms | 10.0 | 0 | 0 |
| `51-music-192` | 2.0 | 0.76 | 64 ms | 10.0 | 1 | 11 |
| `51-music-128` | 2.0 | 0.83 | 64 ms | 10.0 | 3 | 3 |
| `51-music-96` | 2.0 | 1.06 | 64 ms | 11.4 | 236 | 1,351 |

The three streams that keep up play in real time through the longer queue, with at most three underruns and 11 ms of silence in all,
at the start; the one that does not, A-CPL mode 3, runs dry in every frame there as it does in the short one. The wide TDM sink, which
a 5.1 layout needs, does not start on this chip revision (`could not start I2S in TDM mode`; `sink/i2s_wide/audio_sink.cpp` says why), so 5.1
itself could not be played through a sink on this board, and the 2.0 plays above are the same decodes folded. A wide TDM frame is 64
bytes, so a 64 ms queue for it would be 196 KB of internal RAM, which the decoder has no room for: that layout wants the decoded PCM
held in PSRAM in front of the sink, a ring of a few frames filled by the decode task and emptied by a task of its own, which the
player does not have.

### The frame-rate converter

| Stream | Ratio | Samples a frame | us/frame | x real time | Converter us/frame | Before D14a5 | Before D14a4 | First frame s | Before D14a5 |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `ims-music-64-24` | 25/24 | 2,000 | 21,329 | 0.51 | 7,320 | 7,283 | 204,951 | 0.28 | 0.43 |
| `ims-music-64-23976` | 1001/960 | 2,002 | 28,502 | 0.68 | 14,108 | 22,369 | 230,897 | 0.30 | 5.87 |
| `ims-music-64-25` | 15/16 | 1,920 | 21,257 | 0.53 | 7,181 | 7,184 | 208,009 | 0.30 | 0.40 |
| `ims-music-64-2997` | 1001/960 | 1,602 | 22,978 | 0.69 | 10,761 | 98,508 | 184,609 | 0.24 | 5.97 |

Part 1 clause 6.2.15's three ratios are 25/24 (24 fps), 1001/1000 x 25/24 = 1001/960 (23.976 fps,
and 29.97 fps at its own frame length) and 15/16 (25 fps). The converter's polyphase filter is
designed in `double` and, in the decoder's `float` build, kept in `float` (D14a4): each phase's
coefficients, 94 taps at 25/24 and 1001/960 and 100 at 15/16, are computed and normalised in
`double` and rounded to `float` once as the table is built (by the compiler since D14a5, below),
and an output is the sum of that many `float` products of the `float` history, added over four
lanes in an order that `dsp/resampler_vector.hpp` fixes, so that the host, the Cortex-M3 leg and
the board give the same sample. Before D14a4 the sum ran in `double` on a part whose FPU is single
precision, so that each
multiply and add was a call to a software routine of the compiler's runtime: 185 to 231 ms a frame,
4.9 to 5.5 times real time by itself and 84 to 88% of the frame. It then took 7.2 to 7.3 ms a frame
at 25/24 and 15/16, 28 to 29 times less (3.7 us for each output sample of the pair, about 7 cycles a
tap), and the frame was 0.91 and 0.82 of its duration: the P4 decodes DEE's immersive stereo at 24
and 25 fps in real time (the table above has 7.2 to 7.3 ms and 0.51 and 0.53 since D14e, whose changes to the rest of the frame
and the flash's mode left the converter's own time where it was; 7.3 to 7.4 ms and 0.79 and 0.80 at D14a6). In the images of D14a5
the same code took 7.3 to 9.7 ms at 25/24 (7.2 to 7.6 at 15/16): the converter's time at 25/24 moved by a third with
the layout of the heap from one image to the next, whichever memory the table was kept in. In D14a6's two images,
three runs, it took 7.3 to 7.6 ms at 25/24 and 7.3 to 7.5 at 15/16; the variation between D14a5's images was not
traced.

D14a5 builds the three tables when the library is compiled. The design is a `constexpr` function
(`dsp/resampler_design.hpp`) over `sin`, `sqrt`, `ceil` and the Kaiser window's I0, which the build
takes from `dsp/portable_math.hpp` in plain `double` arithmetic with no C library call, so that the
compiler's evaluation and every target give the same bits; the tables are the C library's design
rounded to `float` in every coefficient of 25/24, 15/16 and 1001/960, which the tests check, and the
`double` filter that the encoder uses keeps the C library's. A table keeps phases 0 to up / 2: phase
up - p is phase p read from its last coefficient to its first, since the window and the sinc are even,
and a second dot-product kernel reads it that way, bit for bit the sum of the phase written out
backwards. At 1001/960 that is 501 phases of 94 taps, 188 KB as `float` where the table was 376,376
bytes; the three tables are 196,464 bytes of constants in the image, which grew by 199,696 bytes
(1,977,504 to 2,177,200 for the measurement image). Nothing is designed on the board: the first frame at
1001/960 takes 0.31 s and 0.42 s, from 5.9 and 6.0, within 0.12 s of the first frame at 24 and 25 fps.

The constants are in flash, which on this board was read in DIO mode at 80 MHz when these were measured (QIO
since D14e, [Flash mode](#flash-mode)) behind the 128 KB L2 cache, where the PSRAM is read in hex mode at 200 MHz,
and a table of 188 KB does not fit the cache.
Read in place, the half table took the converter 60.5 ms a frame at 23.976 fps (22.4 ms before, from
PSRAM), the whole 376 KB table 110 ms, and the 29.97 fps play 124.7 and 153 ms; at 25/24 and 15/16,
whose tables are 4.9 and 3.2 KB, reading in place made no difference that showed through the layout
noise above. The filter therefore copies the table when it is made: the 188 KB go to PSRAM, and the
two small tables, 4.9 and 3.2 KB where the ones designed at run time were 9.4 and 6 KB, are blocks
below the policy's 16 KB. From the copy the converter took 16.5 ms a frame at 23.976 fps (1.11 times real time, from 1.25), and
the peak PSRAM of the play fell from 632 to 435 KB, and from 572 to 383 at 29.97 fps. The 29.97 fps play's converter,
93.8 ms a frame (12.2 with allocations over 512 bytes sent to PSRAM first), did not follow from the table: it had
the same table and the same code, and its buffers were in the low-power SRAM ([The low-power SRAM](#the-low-power-sram)).
With that out of the heap the converter took 12.0 ms a frame at 29.97 fps and 15.2 at 23.976 fps (0.98 times real
time), and with D14e's decoder and the flash in QIO mode it takes 10.8 and 14.1 ms (0.69 and 0.68 times real time).

### Float output on the host, the Cortex-M3 leg and the board

[Decision 26](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/ac4.md#decisions-of-2026-09-25)
promises identical `float` output everywhere, and the probe's six AC-4 fixtures pin it
(`tests/golden/ac4-probe-pcm-hashes.json`, held on the x86-64 host and on the Cortex-M3 under QEMU).
The board's PCM hash (FNV-1a over the sample bit patterns in the order the player delivered them, the
probe's own) equals all six, played from the same committed streams: `ac4_20_music`
`f860e51f602ae753`, `ac4_20_acpl` `e8b8cfa716dd0e1c`, `ac4_51_music` `6d36d7d2e8a070e9`,
`ac4_51_acpl` `dc9687f3f1be5f67`, `ac4_514_tones` `e3c8eeec6565b940` and `ac4_20_companding`
`5b93c61c57566b0c`, the stream D14a4 added because none of the five before it had companding.

On the twenty ten-second plays above the board's hash equals the host's
`ICLFORGE_DECODE_SCALAR=float` output (GCC 16 and Clang 22 with glibc, and MSVC) on all twenty and on
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

D14a4 took the three through `iclforge::internal::scalar_exp2` and `scalar_log2` at `float`, the pair
`hf_generator.cpp`'s gains already use, and left `std::pow` at `double`. It replaced the `hypotf` of
Pseudocode 87's prediction limit with a comparison of the squared magnitude, and gave the converter's
sum the fixed order above. What remains of libm in the `float` decode, read from the symbols of the
Cortex-M3 image, is `sqrtf` and `floorf`, which are exact, and calls at `double` whose results are
rounded to `float` once: `pow`, `exp2`, `log`, `log10`, `log2`, `sin`, `cos`, `fmod` and `lround`, which
build the transforms' tables, the DRC, downmix and dialogue enhancement gains, the
noise substitution's amplitudes and A-SPX's frequency tables. A library whose `double` differs in its
last bit changes the `float` only where a value lies within that bit of a rounding boundary of
`float`, about one in 2^29, and none of the plays compared differs. The converter's table is no longer
among those: D14a5's compiler builds it from functions that use no C library, and its FNV-1a image is
pinned at the three ratios and equals the C library's design rounded to `float`, so no hash on this
page moved when it was built, and the 84 plays and cuts compared across the host with MSVC, GCC 16 and
Clang 22, the Cortex-M3 program and the board are equal again. D14a6 moved no hash either: the board's hash of each of
the twenty plays, the six core-decoding plays, their 24-frame cuts, D14b's four cuts and the probe's six fixtures (56 in
all) equals D14a5's, and the six fixtures equal the pins above.

D14e moved none either, and was built to move none: each speed-up has a test that holds it to a verbatim copy of the code it
replaced, to the bit (`[exact]`). The host's hash of each of the 84 plays and cuts of D14a4's lists, built at `float` with MSVC,
GCC 16 and Clang 22 from the tree that has D14e, equals D14a5's in every row, and the three compilers agree on all 84. The
board's hash of the twenty plays, the six core-decoding plays and the 24-frame cut of each (46 plays) equals the host's under the
default allocation policy, and that of the twenty and the six (26) under the 512-byte one; the probe's six fixtures equal
their pins on the board, on the host and on the Cortex-M3 under QEMU
(`run_baremetal_probe.sh --ac4 --icount`: 54.8 M to 206.3 M instructions a frame, every ceiling held, an image of 691,896 bytes,
[the Cortex-M3 page](cortex-m3.md#the-ac-4-probe)). The Cortex-M3 program was not run on the 26 cuts again.

The four cuts of D14b's comparison, decoded on the tree with D14a4, have one hash each on the host
with MSVC, GCC 16 and Clang 22, on the Cortex-M3 program and on the board:

| Cut | Hash |
|---|---|
| 2.0 SIMPLE | `e5812ac315509622` |
| 5.1 | `6fd5e30cd9430f02` |
| 2.0 A-SPX at 25 fps (converter) | `aed7f679dfeee053` |
| 2.0 A-SPX with companding | `24f1c0e157ad0819` |

### What the decoder holds

Under ESP-IDF's default policy the decoder's large blocks are in PSRAM (0.24 MB at 2.0 in SIMPLE
mode, 0.7 to 1.0 MB at 5.1 and 1.7 to 1.8 MB at 5.1.4) and its many small ones fill internal RAM, which
with the network's makes a peak heap of 0.58 MB, 1.1 to 1.4 MB and 2.1 to 2.2 MB. The decode task
uses 19 to 30 KB of its stack (A-CPL mode 3 the most, 30 KB: D14e's A-CPL stage added 4.7 KB to it and
`-O2` on the decoder's files 1.8 KB). The first frame takes 0.24 to 0.36 s. The frame-rate converter's table is 188 KB of PSRAM at 1001/960 (376 KB before
D14a5) and 4.9 or 3.2 KB of internal RAM at 25/24 and 15/16 (9.4 and 6 KB), and the image holds the
three tables in 196 KB of flash.

### Building with AC-4

```bash
idf.py -DIDF_TARGET=esp32p4 \
  "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.hw;sdkconfig.p4;sdkconfig.sendspin;sdkconfig.ac4" \
  -DICLFORGE_STAGE_TIMERS=ON build
```

from `esp-idf/iclforge/examples/hearth_sink/`. `sdkconfig.ac4` turns on `CONFIG_ICLFORGE_AC4` and a
40 KB decode stack. The measurement image adds `ICLFORGE_EXAMPLE_SINK_NULL`,
`ICLFORGE_EXAMPLE_AC4_PCM_HASH`, `ESP_TASK_WDT_INIT=n`, a 64 KB stack and the network's credentials,
and goes to the board with `tools/hearth/ota.py push`, which replaces the application and keeps the bootloader on
the board: the flash reads in QIO mode (`sdkconfig.p4`) only on a board whose bootloader was built for it, which
`idf.py flash` over USB puts there once ([Flash mode](#flash-mode)). A play's location can carry `?decoding=core`
for core decoding and `?hash=off` for a play without the hash. A play ends with `ac4.lap` (frames,
samples, the decoder's time, the worst frame's, the hash), `ac4.heap` and one `play.stage[...]`
line for each stage. The packer leaves the AC-4 sources out of the archive unless it is given
`--with-ac4`; with `--verify` it then builds a throwaway project against the archive for each of
the manifest's parts, with the decoder switched on and constructed (in the fixed-point tier on the
parts with no floating-point unit).
CI narrows that to the ESP32-P4 with `--verify-targets esp32p4`. A `float` build evaluates the
converter's tables while it compiles `src/ac4core/src/dsp/resampler.cpp`, which takes ESP-IDF's
RISC-V GCC 15.2 8.4 s where it took 1.5, and `src/ac4core/CMakeLists.txt` raises the compiler's limit
on constant evaluation for that file (`-fconstexpr-ops-limit`).

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
idf.py build                                  # -DICLFORGE_ESP_PROFILE=decoder by default
```

The decode arithmetic needs no override: the component
(`esp-idf/iclforge/CMakeLists.txt`) picks `float` from `SOC_CPU_HAS_FPU`, which this part has, the
same as the S3 — see [ESP32-S3 → The ESP-IDF component](esp32-s3.md#the-esp-idf-component).

On a board reached over its OTG connector held in the ROM's manual download mode (see
[Reading the console](#reading-the-console) for both):

```bash
python -m esptool --chip esp32p4 -p <OTG-PORT> -b 460800 \
  --before default-reset --after hard-reset write-flash \
  --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x2000 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/iclforge_probe_esp32p4.bin
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

- [`planning/esp32-sink-tiers.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/esp32-sink-tiers.md) —
  the plan this page is Phase P1 of, with the Ethernet shape (P2, for a board that has a PHY,
  which this one does not) and the Wi-Fi shape over the onboard C6 and `esp_hosted` (P3), both
  onto TDM and a pair of ES9080 DACs, and where each stands.
- [`planning/ac4.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/planning/ac4.md) —
  phase D14b, of which the [AC-4](#ac-4) section is the measurement, D14a's third part, which
  reworks what it found to cost most, D14a4, which took the converter and the last libm
  disagreements out of the `float` decode, and D14a5, which builds the converter's tables at
  compile time.
- [ESP32-S3](esp32-s3.md) — the "better" tier, hardware-verified, the primary Wi-Fi Sendspin sink.
- [ESP32-C6](esp32-c6.md) — the "OK" tier (a C61 is proposed as "good" between it and the S3), and
  the sibling page with the same "no QEMU for this part" gap.
- [Bare metal overview](index.md) — how the pages in this section relate.
