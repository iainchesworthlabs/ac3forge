# Streaming player

Decodes AC-3 or E-AC-3 on an ESP32-S3 from wherever the bytes are — a flash
partition by default, an SD card, or an HTTP body over WiFi — and plays it,
without ever holding more than 16 KB of the stream in memory.

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

**One decoder for both generations.** `Eac3Decoder` reads Annex E with every
tool and accepts a plain AC-3 syncframe as one access unit of one substream, so
the player does not need to know which it was given. `FrameDecoder` reads AC-3
alone — bsid above 8 comes back as `kUnsupported` — and this example used it
until 2026-09-10, which meant an E-AC-3 stream failed before any audio and CI,
whose sample is AC-3, could not tell.

**Two seams.** Where bytes come from and where audio goes are both directories
CMake picks, not flags the player branches on — the same rule the library uses
for its own platform choices. The player mentions neither a partition nor I2S.
See [`main/byte_source.hpp`](main/byte_source.hpp) and
[`main/audio_sink.hpp`](main/audio_sink.hpp).

| Source | Sink |
| --- | --- |
| `partition` — flash (default) | `i2s` — stereo DAC (default) |
| `sd` — SD card over SDMMC | `tdm` — multi-channel on one data line |
| `http` — an HTTP body over WiFi | `null` — counts frames; what CI runs |

Chosen in `idf.py menuconfig` under *ac3forge stream player*.

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

On a DevKitC-1 reached through its **native USB connector** rather than the
UART bridge, build with `SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.hw"`
so the console comes out of the same cable, and expect two things: every
host-initiated reset lands in `boot:0x0 (DOWNLOAD)` and the application never
starts, so flash, press the board's RESET button, then attach with
`idf.py monitor --no-reset`; and the port re-enumerates on every reset, so a
terminal that does not reopen it misses the first lines. Building more than one
shape on one machine, give each its own build directory **and** its own
`-DSDKCONFIG=<build dir>/sdkconfig`, because ESP-IDF otherwise keeps a single
`sdkconfig` in the project directory and the second shape inherits the first's.

Without a board:

```bash
SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci" idf.py build
idf.py qemu
```

which is what CI does. `sdkconfig.ci` selects the capture sink and stops after
two passes. The capture sink is a real sink — same calls, same conversion, same
audio — with no peripheral behind it, so the partition reads, the framing and
the decode all run exactly as they do on hardware, and it checks the converted
samples on the way past. What it cannot do is prove a DAC makes a noise.

## What it prints

Under QEMU, through the capture sink:

```
source: partition 'audio' at 0x190000, 10752 bytes of audio in 262144
sink: capture 48000 Hz 16-bit x2 in 2 slots (no peripheral, no pacing)
stream: AC-3 acmod=7 channels=6 substreams=1 dialnorm=-31 objects=no, folded to 2
lap=1 frames=6 us_per_frame=11059 worst_frame_us=36028 realtime_permille=345 resync=0 heap_free=228708
lap=2 frames=12 us_per_frame=8166 worst_frame_us=36028 realtime_permille=255 resync=0 heap_free=228708
stream.rms[0]=107811
stream.rms[1]=106647
capture.low_byte_set=0 capture.padding_nonzero=0 capture.carried_nonzero=36677 capture.rms=107218
stream.units=12 stream.held=0 stream.resync_bytes=0 stream.sink=capture-i2s stream.sink_frames=12 stream.source=partition stream.audio_ms=384 stream.wall_ms=121
result=pass
```

`stream:` is what the first access unit said the stream is — which generation,
its coded layout, how many substreams, its dialnorm, whether it carries object
audio — and what it is being folded to. `stream.held` counts units the decoder
released one call late (§3.7's transient pre-noise processing; zero for a
stream that does not use the tool). `stream.audio_ms` against `stream.wall_ms`
is the whole-pipeline real-time check: a player that kept up spent as long
playing as the audio lasted, one that stalled spent longer by exactly the
silence it inserted, and a sink with no peripheral runs ahead of the clock, as
here. With `CONFIG_AC3FORGE_EXAMPLE_REPORT_EVERY_FRAMES` set, the same figures
also print cumulatively every N frames as a `progress=` line, for a source that
makes one long pass and would otherwise be silent for minutes.

The `i2s` sink adds a line of its own at each report, of this shape (the figures
a board produced are under [the sources](#the-sources) below):

```
sink.writes=<frames> sink.underruns=<count> sink.dry_ms=<ms> sink.min_headroom_ms=<ms> sink.dma_ms=<depth>
```

The DAC's DMA drains at exactly the sample rate whatever the CPU does, and the
driver says nothing when it runs dry — it plays zeros and carries on. So the
sink models the queue from that one fact: what was queued when the last write
returned, less what has drained since, is what is left when the next frame
arrives. `min_headroom_ms` is the least that was ever left; `underruns` counts
frames that arrived to an empty queue, and `dry_ms` is how long it had been
empty, summed. The model is out by up to one DMA descriptor (5 ms at the
default depth), which is enough to read a stall and not enough to mistake one
for a smooth run. `sink.dma_ms` is the queue's depth, from
`CONFIG_AC3FORGE_EXAMPLE_I2S_DMA_DESCRIPTORS` and `_DMA_FRAMES`.

`stream.rms[ch]` is the RMS of what was sent to the sink, scaled by 1e6 — the
same form `apps/baremetal/probe.cpp` reports its own levels in. The player
reports it and does not judge it: what the levels *should* be is a property of
the stream, so CI holds the expectation.

**That is what makes this an end-to-end check rather than a smoke test.**
`result=pass` on its own means "some units decoded without returning an error",
which a stream decoding to silence or to full-scale noise satisfies completely.
CI compares these against the host's answer for the same file through the same
configuration — `ac3cli decode … downmix=loro drcmode=line`, giving 107,370 and
106,234. The 0.4% gap is the float32 decode path against the host's float64.

`resync` is bytes skipped looking for a sync word. Non-zero means the stream did
not begin on a frame boundary, or that something between frames was not a frame.

**`realtime_permille` means nothing under the null sink.** The real sink blocks
until the DAC has taken the samples, and that back-pressure is what makes the
loop run at real time; the null sink runs flat out. Under QEMU it means less than
nothing — the emulator is not cycle-accurate and reports a CPU clock that
disagrees with its own boot log. Real-time decode on this part is measured on a
board, not here: an E-AC-3 5.1 frame decodes in 11.0 ms of its 32 at 240 MHz — see
[`docs/platforms/esp32.md`](../../../../docs/platforms/esp32.md#timing).

## The sources

Only `partition` can be **run** without hardware, which is why it is the default
and the one CI exercises end to end. `sd` and `http` are compiled by CI and no
further — QEMU has no SD host and no network — so both are deliberately small:
the less that lives behind an unrunnable seam, the less can be wrong in it.

Two things differ between them and are worth knowing before writing a third:

**Length.** A partition is the only source with none. Every other kind has one —
`Content-Length`, a file size — and without it the player reads the whole 256 KB
partition while the framer skips a quarter of a megabyte of erased flash looking
for a sync word, on every lap. The build supplies it from the file's own size.

**Rewind.** `http` cannot. A socket has delivered what it delivered;
re-requesting the URL would be a new stream, not a rewind, and the decoder's
overlap-add state would carry across the seam as a click. `source_rewind()`
returns false and the player stops rather than pretending.

## More than two channels

`tdm` puts up to eight channels on one data line: three pins (BCLK, WS, DATA)
instead of four data lines, at a 12.3 MHz bit clock for 8 slots × 32 bits ×
48 kHz. It needs a DAC that speaks TDM — a PCM3168A does, the common MAX98357A
and PCM5102 breakouts do not.

**It has never run on hardware.** There is no TDM DAC here and QEMU has no I2S,
so what CI establishes is that it compiles and links. The exception is the part
worth testing: [`main/interleave.hpp`](main/interleave.hpp) is free of ESP-IDF
and is unit-tested on the host (`tests/io/test_interleave.cpp`), because
planar-to-interleaved indexing with slot padding is where the bugs are and the
rest of that sink is peripheral setup that either works on a board or does not.

The padding is the part that bites. A TDM frame is a fixed shape, so a 5.1
programme on an 8-slot bus leaves two slots with nothing to carry — and they
must be **written as zeros, not skipped**. The DMA buffer is reused, so whatever
the previous frame left there is what the DAC clocks out: two channels of stale
audio nobody is listening for and everybody can hear. There is a test for
exactly that.

## What it costs

`idf.py size`, IDF v6.1, `-Os`, the default shape (`partition` to `i2s`) with
the console on USB-Serial-JTAG:

| | Bytes |
| --- | --- |
| Internal SRAM (DIRAM) used by the image | 155,615 |
| …of which `.bss` | 114,024 |
| …leaving for the heap, by the linker's estimate | 186,145 |
| Caller-owned PCM, eight channels of one frame (`kMaxChannels`) | 49,152 |
| Accumulator buffer (`kRecommendedBuffer`) | 16,384 |
| Interleave buffer (one frame, static) | 6,144 |
| I2S DMA queue (4 × 240 frames, stereo, 16-bit) | 3,840 |
| Partition read block | 2,048 |

More than `i2s_player`'s 93,967, and the difference is what a player that does
not know its stream in advance carries: PCM storage wide enough for 7.1 rather
than the fixture's 5.1, the framing buffer, and every source and sink's
components linked whichever pair is selected (`main/CMakeLists.txt` says why
the `REQUIRES` list cannot follow the choice). The `http` shape adds the WiFi
and TCP/IP stacks on top: 163,546 bytes of DIRAM before either has allocated
anything at run time.
