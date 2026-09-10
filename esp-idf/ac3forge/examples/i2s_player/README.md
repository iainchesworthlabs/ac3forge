# I2S player

Decodes AC-3 on an ESP32-S3 and plays it out of an I2S DAC.

The 5.1 fixture from `apps/baremetal/fixture.hpp` — the same stream the
footprint probe checks against known levels — folded to stereo by the decoder's
own §7.8 output stage and written to I2S at 48 kHz, 16-bit, on a loop.

## Wiring

Three pins, set under `ac3forge I2S player` in `idf.py menuconfig`:

| Signal | Also called | Default GPIO |
| --- | --- | --- |
| BCLK | SCK, SCLK | 5 |
| WS | LRCK, LRCLK | 6 |
| DOUT | SDIN, DIN, SD | 7 |

The defaults are ordinary S3 GPIOs chosen to avoid the strapping pins (0, 45,
46), the USB pair (19, 20) and the console UART (43, 44). Any free GPIO works —
the I2S signals go through the GPIO matrix. Ground must be common between the
board and the DAC.

Tested shapes: a MAX98357A (class-D amplifier, drives a speaker directly, no
MCLK needed) and a PCM5102 (line-level, jumper it to its internal PLL so it does
not need MCLK either). No MCLK pin is configured, so a DAC that requires one
needs `std_cfg.gpio_cfg.mclk` set in `i2s_player.cpp`.

## Running

```bash
. $IDF_PATH/export.sh
idf.py set-target esp32s3
idf.py -p <PORT> flash monitor
```

On a DevKitC-1 reached through its **native USB connector** rather than the
UART bridge, add `SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.hw"` so the
console comes out of the same cable — and then flash, press the board's RESET
button, and attach with `idf.py monitor --no-reset`, because every
host-initiated reset over USB-Serial-JTAG lands in `boot:0x0 (DOWNLOAD)` and the
application never starts. The port also re-enumerates on every reset, so a
terminal that does not reopen it misses the first lines.

## What it prints

Once per lap of the fixture — 192 ms of audio:

```
lap=12 frames=72 us_per_frame=8421 worst_frame_us=11003 realtime_permille=263 heap_free=213480
```

`realtime_permille` is decode time against playback time. 1000 is exactly real
time; the number above would mean the decode uses about a quarter of the
available CPU. **This is the measurement QEMU cannot give.** CI runs the probe
under `qemu-system-xtensa`, which is not cycle-accurate and reports a CPU clock
that disagrees with its own boot log, so every timing figure there is printed
for shape rather than for truth. Here the I2S peripheral is a real clock — the
DMA drains at exactly 48,000 frames a second whatever the CPU does — so a decode
that cannot keep up is audible, and these numbers come from silicon.

`worst_frame_us` matters as much as the average. The DMA holds 20 ms; a single
frame that takes longer than the 32 ms it produces will be absorbed, but a run
of them will not.

## What it costs

Measured with `idf.py size` on IDF v6.1, `-Os`, with the console on
USB-Serial-JTAG (`sdkconfig.hw`):

| | Bytes |
| --- | --- |
| Internal SRAM (DIRAM) used by the image | 93,967 |
| …leaving for the heap, by the linker's estimate | 247,793 |
| I2S DMA buffers (4 × 240 frames, stereo, 16-bit) | 3,840 |
| Interleave buffer (one frame, static) | 6,144 |

Less than the footprint probe's 134,676 bytes, because this reaches only the
AC-3 path: no Annex E decoder, no QMF bank, no object reconstruction. An
E-AC-3 or Atmos player is a bigger build — `docs/platforms/esp32.md` has those
numbers.

## Using the component in your own project

Two lines, which is what this example's own `CMakeLists.txt` does:

```cmake
set(EXTRA_COMPONENT_DIRS "/path/to/ac3forge/esp-idf")
set(AC3FORGE_ESP_PROFILE "decoder")   # or "encoder"
```

Both must appear **before** `include($ENV{IDF_PATH}/tools/cmake/project.cmake)`,
because the component is read during IDF's component scan, which `project()`
performs. `EXTRA_COMPONENT_DIRS` wants the directory that *contains* components
— `esp-idf`, not `esp-idf/ac3forge`.

The two profiles are mutually exclusive: no two of decode, AC-3 encode and
E-AC-3 encode fit in this part's internal SRAM at once. Switching between them
needs `idf.py fullclean` first, since the choice reaches the library as CMake
cache variables that a warm build directory has already resolved.
