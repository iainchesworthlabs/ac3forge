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

Every figure on this page was measured on a board on 2026-09-23, with no network.

## Status

| | |
|---|---|
| Decode | Correct: all fourteen fixtures, every channel's level within the probe's tolerance, and every float-tier PCM hash matching the values the probe pins for this build. Also all eleven stream-set `714-*` files (a scratch probe copy, not committed — see [Stream set](#stream-set)), levels to the digit against `streams.json`'s own reference |
| Real time, no network | **Every fixture and every stream-set file**, from 0.027x (`ac3_mono`) to 0.448x (`eac3_714_fold`, 7.1.4 folded to Lo/Ro) among the fixtures, up to 0.700x (`714-ecpl`) among the stream set — comfortably inside a 32 ms frame even at this chip's 360 MHz ceiling, not the part's 400 MHz datasheet maximum (see [The chip revision](#the-chip-revision-and-what-it-blocks)) |
| Memory | 514,820 bytes free at boot, largest block 385,024; peak heap across every fixture 195,025 (`eac3_atmos_render`), leaving well over half the free total unused at the worst point measured |
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
  minimum-footprint probe leaves it off even when the board has it.

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
- [ESP32-S3](esp32-s3.md) — the "better" tier, hardware-verified, the primary Wi-Fi Sendspin sink.
- [ESP32-C6](esp32-c6.md) — the "good" tier, and the sibling page with the same "no QEMU for this
  part" gap.
- [Bare metal overview](index.md) — how the pages in this section relate.
