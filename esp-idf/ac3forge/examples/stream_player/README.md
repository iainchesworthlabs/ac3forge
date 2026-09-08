# Streaming player

Decodes AC-3 out of a flash partition on an ESP32-S3 and plays it, without ever
holding more than 16 KB of the stream in memory.

The sibling of [`i2s_player`](../i2s_player), differing in one thing: where the
audio comes from. That one decodes a bitstream linked into its own image, which
proves the codec works and is not how anything real gets its audio.

## What it demonstrates

**An input path.** `ac3::split_frames` and `ac3::split_access_units` take a span
over the whole stream. Nothing streaming can produce one — an SD card, an HTTP
body and this partition all arrive in pieces, and on a part with 280 KB of RAM
the whole file is not going to be resident anyway.
`ac3::io::AccessUnitAccumulator` applies the same boundary rule incrementally,
over a buffer the caller owns, so framing allocates nothing.

**Access units, not syncframes.** `Eac3Decoder::decode_access_unit_into` wants an
independent substream together with the dependents that extend it (§E3.8.2). A
reader that handed over one syncframe at a time would give the decoder a
dependent with nothing to extend.

**A sink seam.** Where audio goes is a directory CMake picks, not a flag the
player branches on — the same rule the library uses for its own platform
choices. See [`main/audio_sink.hpp`](main/audio_sink.hpp).

## Running it

On a board, with an I2S DAC wired to the three pins under `ac3forge stream
player` in `idf.py menuconfig` (BCLK, WS, DOUT — defaults 5, 6, 7):

```bash
. $IDF_PATH/export.sh
idf.py set-target esp32s3
idf.py -p <PORT> flash monitor
```

`idf.py flash` writes `stream/sample.ac3` into the `audio` partition as well as
the application, so there is nothing to copy by hand.

Without a board:

```bash
SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci" idf.py build
idf.py qemu
```

which is what CI does. `sdkconfig.ci` selects the null sink and stops after two
passes. The null sink is a real sink — same calls, same audio — with no
peripheral behind it, so the partition reads, the framing and the decode all run
exactly as they do on hardware. What it cannot do is prove a DAC makes a noise.

## What it prints

```
stream: partition 'audio' at 0x190000, 10752 bytes of audio in 262144
sink: null 48000 Hz 16-bit x2 (no peripheral, no pacing)
lap=1 frames=6 us_per_frame=16145 worst_frame_us=40737 realtime_permille=504 resync=0 heap_free=292696
stream.units=12 stream.resync_bytes=0 stream.sink=null stream.sink_frames=12
result=pass
```

`resync` is bytes skipped looking for a sync word. Non-zero means the stream did
not begin on a frame boundary, or that something between frames was not a frame.

**`realtime_permille` means nothing under the null sink.** The real sink blocks
until the DAC has taken the samples, and that back-pressure is what makes the
loop run at real time; the null sink runs flat out. Under QEMU it means less than
nothing — the emulator is not cycle-accurate and reports a CPU clock that
disagrees with its own boot log. Real-time decode on this part is still
unmeasured; see [`docs/platforms/esp32.md`](../../../../docs/platforms/esp32.md).

## Changing the source

`read_block()` in [`main/stream_player.cpp`](main/stream_player.cpp) is the only
function that knows where bytes come from. An SD card is the same function over
`f_read()`; HTTP is the same function over `esp_http_client_read()`. Nothing
else changes.

One thing a partition does not give you is a length, which every other source
does — `Content-Length`, a file size, a directory entry. The build supplies it
from the file's own size, because without it the player reads a quarter of a
megabyte of erased flash on every lap.

## More than two channels

Standard I2S carries two slots, so 5.1 and 7.1 need TDM
(`driver/i2s_tdm.h`) — the S3 packs up to 16 slots onto one data line, so eight
channels needs three pins rather than four data lines, and a DAC that speaks it
(a PCM3168A does; the common MAX98357A and PCM5102 breakouts do not).

No TDM sink exists yet. The seam is shaped for one — sinks are handed the
decoder's planar float and own their own interleave and sample format — and
`main/audio_sink.hpp` records what is known about writing it, including the slot
zeroing a 5.1 programme on an 8-slot stream needs.

## What it costs

`idf.py size`, IDF v6.1, `-Os`:

| | Bytes |
| --- | --- |
| Internal SRAM (DIRAM) used by the image | 111,055 |
| …leaving for the heap | 230,705 |
| Accumulator buffer (`kRecommendedBuffer`) | 16,384 |
| Partition read block | 2,048 |

More than `i2s_player`'s 94,383, and the difference is mostly the accumulator
buffer — which is the price of not having the whole stream in memory.
