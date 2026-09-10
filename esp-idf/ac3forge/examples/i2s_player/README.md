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
console comes out of the same cable. Over USB-Serial-JTAG, the reset esptool
performs after flashing can leave the chip in `boot:0x0 (DOWNLOAD)`, and the
application never starts. Its watchdog reset boots from flash instead, with no
button to press. On 2026-09-10 it did so every time on a DevKitC-1 with an
ESP32-S3 rev v0.2, where the default reset never did:

```bash
cd build
python -m esptool --chip esp32s3 -p <PORT> --after watchdog-reset write-flash @flash_args
```

The alternative is to flash, press the board's RESET button, and attach with
`idf.py monitor --no-reset`. A board fresh from the factory may be running a
USB application that ignores esptool altogether: hold BOOT, press RESET and
release BOOT for the first flash. The port re-enumerates on every reset, so a
terminal that does not reopen it misses the first lines.

## What it prints

Once per lap of the fixture — 192 ms of audio. From a DevKitC-1 at 240 MHz on
2026-09-10, with no DAC wired (the peripheral clocks the audio out all the
same):

```
lap=152 frames=912 us_per_frame=9880 worst_frame_us=19296 realtime_permille=308 heap_free=284204 i2s_hz=48000 starved_buffers=16 wall_us_per_frame=32094
```

`realtime_permille` is decode time against playback time. 1000 is exactly real
time; here the decode uses just under a third of the CPU. **This is the
measurement QEMU cannot give.** CI runs the probe
under `qemu-system-xtensa`, which is not cycle-accurate and reports a CPU clock
that disagrees with its own boot log, so every timing figure there is printed
for shape rather than for truth. Here the I2S peripheral is a real clock — the
DMA drains at exactly 48,000 frames a second whatever the CPU does — so a decode
that cannot keep up is audible, and these numbers come from silicon.

The last three figures are the driver's view and the part's own clock, not the
loop's. `i2s_hz` is the rate the peripheral really runs at: the DMA buffers the
driver reported sent since the first lap, times their length, over the time
between. `starved_buffers` counts the buffers the DMA reached with nothing new
written into them, each 256 frames of silence. `wall_us_per_frame` is the
loop's own pace since the first lap. It converges on 32,000 from above: the
sixteen starved buffers here all went at start-up, before the first frame had
been written, and none after - between laps 52 and 152 the part's clock moved
exactly 19,200 ms for 600 frames. Read it rather than the times these lines
arrive, which USB-Serial-JTAG delivers in bursts.

`worst_frame_us` matters as much as the average. The DMA holds 21 ms; a single
frame that takes longer than the 32 ms it produces will be absorbed, but a run
of them will not.

**Why 256-frame buffers.** 256 divides the 1,536-frame write, so no write ends
part-way through a buffer, and that matters more than it looks. ESP-IDF v6.1's
`i2s_channel_write` starts a fresh buffer whenever two or more sent ones are
waiting for it, and the unwritten rest of the buffer in hand goes out as
zeros. After a 10 ms decode, that is every frame. With the 240-frame buffers
this example had before 2026-09-10, every frame ended 96 frames into a buffer
and the other 144 went out silent: the loop paced at exactly 35 ms a frame, 3 ms
of silence in every 35, with `i2s_hz` at 48,000 and not one buffer starved.

## What it costs

Measured with `idf.py size` on IDF v6.1, `-Os` with the decoder's hot sources
at `-O2`, and the console on USB-Serial-JTAG (`sdkconfig.hw`):

| | Bytes |
| --- | --- |
| Internal SRAM (DIRAM) used by the image | 93,999 |
| …leaving for the heap, by the linker's estimate | 247,761 |
| I2S DMA buffers (4 × 256 frames, stereo, 16-bit) | 4,096 |
| Interleave buffer (one frame, static) | 6,144 |

The component has compiled the decoder's hot sources at `-O2` by default since
2026-09-10, as the footprint probe always did (`src/forge/minimal.cmake` lists
them): 9.9 ms a frame here against 11.5 at `-Os`, for 7.6 KB of flash and no
SRAM. A project that needs the flash back sets `AC3FORGE_MINIMAL_HOT_O2` off
before `project()`.

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
