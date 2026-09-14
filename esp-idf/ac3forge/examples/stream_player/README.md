# Streaming player

Decodes AC-3 or E-AC-3 on an ESP32-S3 from wherever the bytes are — a flash
partition by default, a FAT volume in flash, an SD card, or an HTTP body over
WiFi — and plays it. It reads the stream a piece at a time, through a ring
between the player's fetch and decode tasks (32 KB by default) and a 16 KB
framing buffer.

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

**Access units, not syncframes.** `Eac3Decoder::decode_access_unit_by_block`
wants an independent substream together with the dependents that extend it
(§E3.8.2). A reader that handed over one syncframe at a time would give the
decoder a dependent with nothing to extend.

**Blocks, not frames.** The decoder hands its audio over 256 samples at a time,
every coded channel and, when the stream has them, the objects beside the bed
with the metadata that places them. The player renders each block onto the
configured layout and writes it to the sink before the next arrives, so what it
holds of the audio is one block per output slot - 16 KB for a sixteen-slot
layout where a frame of them would be 96 KB. See
[Layouts](#layouts-and-more-than-two-channels).

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

**Two tasks and a ring, which are the component's.** Since 2026-09-10 the loop
lives in `esp-idf/ac3forge` as `ac3forge::Player`
([`include/ac3forge/player.hpp`](../../include/ac3forge/player.hpp)): a fetch
task on core 0, beside WiFi and TCP/IP, reads the source into a ring buffer; a
decode task on core 1 drains the ring through the accumulator, decodes, and
writes to the sink. A source that blocks — a socket waiting on the network —
blocks the fetch task and nothing else, and the ring's depth is how long a
stall the DAC never hears. The single loop this replaced had 20 ms of I2S DMA
between a slow read and silence. `main/stream_player.cpp` is what is left: two
adapters from the seams to the player's `ByteSource` and `PcmSink`, a level
meter, and the reporting. The ring's size, its placement in PSRAM, and both
cores are under *ac3forge stream player* in `idf.py menuconfig`.

| Source | Sink |
| --- | --- |
| `partition` — flash (default) | `i2s` — an I2S DAC (default); standard mode or TDM, reconfigured to whatever the layout needs |
| `sd` — SD card over SDMMC | `capture` — converts and checks; what CI runs |
| `fatfs` — a FAT volume in flash | `null` — counts blocks |
| `http` — an HTTP body over WiFi | |

Chosen in `idf.py menuconfig` under *ac3forge stream player*, with the output
layout the stream is rendered onto.

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
so the console comes out of the same cable. The reset esptool performs after
flashing can leave the chip in `boot:0x0 (DOWNLOAD)`; its watchdog reset boots
the application instead, as [`i2s_player`](../i2s_player/README.md#running)
shows, and failing that it is the board's RESET button and
`idf.py monitor --no-reset`. The port re-enumerates on every reset, so a
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
sink: capture 48000 Hz 24-in-32 x2 in 2 slots (no peripheral, no pacing)
source: partition 'audio' at 0x190000, 10752 bytes of audio in 262144
heap: internal free 336512 (largest block 270336), psram free 0
player: ring 32768 bytes in internal SRAM, fetch on core 0 at priority 5, decode on core 1 at priority 6
player: layout 2.0, 2 slots, the decoder's Lo/Ro fold
stream: AC-3 acmod=7 channels=6 substreams=1 dialnorm=-31 objects=no, onto 2.0 (2 slots)
lap=1 frames=12 us_per_frame=12084 worst_frame_us=75312 realtime_permille=377 render_us_per_frame=208 sink_us_per_frame=593 resync=0 ring_low=6144 heap_free=194704
lap=2 frames=12 us_per_frame=12084 worst_frame_us=75312 realtime_permille=377 render_us_per_frame=208 sink_us_per_frame=593 resync=0 ring_low=6144 heap_free=194704
stream: partition ended (passes)
stream.rms[0]=107811
stream.rms[1]=106647
capture.slots=2 capture.channels=2 capture.low_byte_set=0 capture.padding_nonzero=0 capture.carried_nonzero=36861 capture.rms=107231
stream.units=12 stream.held=0 stream.resync_bytes=0 stream.sink=capture-i2s stream.sink_frames=72 stream.source=partition stream.fetched=21504 stream.layout=2.0 stream.layout_mismatches=0 stream.ring_low=6144 stream.decode_stack_free=19644 stream.audio_ms=384 stream.wall_ms=78
result=pass
```

`player: layout` says what the layout is and how the stream reaches it: the
decoder's own fold for `2.0` and `1.0`, the renderer for anything else, with the
objects placed when the layout has heights. `stream:` is what the first access
unit said the stream is — which generation, its coded layout, how many
substreams, its dialnorm, whether it carries object audio — and what it is
being rendered onto. `stream.sink_frames` counts blocks, six per frame.
`stream.layout_mismatches` counts units whose decoded layout was not the one
their headers announced, which sets up the bed's placement before the unit
decodes; zero for every stream met so far. `stream.held` counts units the decoder
released one call late (§3.7's transient pre-noise processing; zero for a
stream that does not use the tool). `stream.audio_ms` against `stream.wall_ms`
is the whole-pipeline real-time check: a player that kept up spent as long
playing as the audio lasted, one that stalled spent longer by exactly the
silence it inserted, and a sink with no peripheral runs ahead of the clock, as
here. With `CONFIG_AC3FORGE_EXAMPLE_REPORT_EVERY_FRAMES` set, the same figures
also print cumulatively every N frames as a `progress=` line, for a source that
makes one long pass and would otherwise be silent for minutes.

`render_us_per_frame` and `sink_us_per_frame` are the parts of `us_per_frame`
spent placing each block onto the layout and inside the sink's write, the level
meter included; the rest is the decoder's own. On a paced sink the sink's part
is mostly the wait for the DAC.

**A play's start.** With `CONFIG_AC3FORGE_EXAMPLE_HOLD_FIRST_UNIT` set, the
player holds a play's first access unit until the second has decoded, so the
sink starts with two frames queued rather than one. A play's first frames
decode more slowly than the rest, and without the hold a 7.1.4 stream played
over WiFi ran the DAC dry in them. It costs 32 ms before a play is heard, and a
copy of that one unit while it waits. `sdkconfig.psram` turns it on for the
network shapes, together with a 32 KB instruction cache, and `sdkconfig.ci`
runs CI through it; `planning/esp32-714-realtime.md` in the repository has the
measurements.

**A local 7.1.4 stream folded to 2.0** fits without PSRAM once the output stage
folds a block at a time, and it needs a DMA queue that holds a whole frame:
twelve descriptors of 256 frames, 64 ms, as `sdkconfig.psram` sets them
(`CONFIG_AC3FORGE_EXAMPLE_I2S_DMA_DESCRIPTORS=12`,
`CONFIG_AC3FORGE_EXAMPLE_I2S_DMA_FRAMES=256`), for 16 KB more of internal SRAM
than the default. A frame's six blocks arrive together, and the default queue
cannot hold them and cover the next frame's decode as well: on a DevKitC-1 with
no PSRAM, `714-tones.ec3` played from the partition had 251 of its 1,512
blocks reach an empty queue with the default, and none with twelve.

`heap:` is the internal RAM free once the source has opened and before the
decoder has allocated anything - with a network stack up, the room the decoder
has. If an allocation fails later, a `heap:` line says what was asked for and
what was left, before the abort that follows.

`ring_low` (and `stream.ring_low` at the end) is the least the ring between
the fetch and decode tasks ever held when the decoder came for more, in bytes,
measured while the source was still delivering: the first frame's fill and the
drain after the source ends are both zeros that say nothing, so neither counts,
and it prints as `-` until there has been something to measure (a stream
shorter than the ring never gives one). Zero means the decoder waited on the
source at least once; how far above zero it stays is the margin the ring's
depth is buying, and the number to read before making the ring bigger. Under QEMU the HTTP source keeps a 16 KB ring
at 14,336 throughout — the emulator's loopback is faster than the emulated
decode — so the figure that matters is the board's. `stream.fetched` is what
the source delivered in total, which should agree with its `Content-Length` or
file size.

The `i2s` sink adds a line of its own at each report, of this shape (a board's
are under [On the board](#on-the-board)):

```
sink.writes=<frames> sink.underruns=<count> sink.dry_ms=<ms> sink.min_headroom_ms=<ms> sink.dma_ms=<depth>
```

The DAC's DMA drains at exactly the sample rate whatever the CPU does, and the
driver says nothing when it runs dry — it plays zeros and carries on. So the
sink models the queue from that one fact: what was queued when the last write
returned, less what has drained since, is what is left when the next block
arrives. `min_headroom_ms` is the least that was left as a block arrived;
`underruns` counts blocks that arrived to an empty queue, and `dry_ms` is how
long it had been empty, summed. The model is out by up to one DMA descriptor
(5 ms at the default depth), which is enough to read a stall and not enough to
mistake one for a smooth run. `sink.dma_ms` is the queue's depth, from
`CONFIG_AC3FORGE_EXAMPLE_I2S_DMA_DESCRIPTORS` and `_DMA_FRAMES`.

Every figure on that line is the play's own. `begin_play` tells the sink a play
is starting (`sink_begin_play()` in [`main/audio_sink.hpp`](main/audio_sink.hpp)),
and the counts start again from zero, with the play's first block exempt as the
first block after boot always was: the queue has been draining since the last
play ended, and nothing was owed to it in between. Until 2026-09-11 the model
counted from boot, so a play started by `POST /play` counted the idle time
before it as one underrun and reported its minimum headroom as zero; the
ten-minute run under [On the board](#on-the-board) shows one.
`stream.sink_frames` still counts from boot, across every play, so from the
second play on it runs ahead of `sink.writes`. The `capture` sink's line starts
again with each play too, as the `stream.rms` levels always have.

`stream.rms[n]` is the RMS of what was sent to slot `n` of the layout, scaled by
1e6 — the same form `apps/baremetal/probe.cpp` reports its own levels in. The
player reports it and does not judge it: what the levels *should* be is a
property of the stream and the layout, so CI holds the expectation.

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
[`docs/platforms/esp32.md`](../../../../docs/platforms/esp32.md#timing). The
player's own figures from a board are below.

## On the board

Measured on 2026-09-10 on an ESP32-S3-DevKitC-1-N16R8 at 240 MHz with no DAC
wired: the I2S peripheral clocks the audio out whether or not anything is
listening, so the pacing and the sink's counters are real.

**The default shape plays in real time, paced by the DAC.** `partition` to
`i2s` at `2.0` (`sdkconfig.defaults;sdkconfig.hw`), the six-frame AC-3 5.1
sample looped 150 times (`CONFIG_AC3FORGE_EXAMPLE_MAX_LAPS=150`):

```
lap=150 frames=900 us_per_frame=31251 worst_frame_us=33420 realtime_permille=976 render_us_per_frame=750 sink_us_per_frame=20271 resync=0 ring_low=0 heap_free=198084
sink.writes=5400 sink.underruns=0 sink.dry_ms=0 sink.min_headroom_ms=5 sink.dma_ms=21
stream.units=900 stream.held=0 stream.resync_bytes=0 stream.sink=i2s stream.sink_frames=5400 stream.source=partition stream.fetched=1612800 stream.layout=2.0 stream.layout_mismatches=0 stream.ring_low=0 stream.decode_stack_free=19704 stream.audio_ms=28800 stream.wall_ms=28707
```

28.8 seconds of audio in 28.7 of wall clock - the difference is the queue
still draining when the player stops - and not one block arrived to an empty
queue. Of the 31 ms each frame took, 20 ms was the sink waiting for the DAC and
about 10 ms the decode. The same run took 51.6 ms a frame before two fixes made
that day: each pass ended in a 100 ms wait on a ring nothing would refill, and
blocks that ended part-way through a DMA descriptor let silence out (see
[the I2S sink](#the-i2s-sink)).

**What a 7.1.4 render costs.** The probe's height-object fixture
(`stream/height.ec3`) from the FAT volume onto `7.1.4`, objects reconstructed
in the MDCT-band domain, through the `capture` sink's twelve TDM slots so that
nothing paces it, twenty passes, with every decoder allocation in internal SRAM
and PSRAM holding the ring alone. Per frame, in microseconds:

| Build | Decoder | Render | Sink | Total | Against real time |
| --- | --- | --- | --- | --- | --- |
| Hot sources at `-Os`, meter in double | 28,429 | 3,160 | 59,677 | 91,266 | 2.85x |
| Hot sources at `-O2` | 22,802 | 3,167 | 59,682 | 85,651 | 2.68x |
| And the meters in float and integers | 22,837 | 3,172 | 8,722 | 34,731 | 1.09x |

The twelve levels are the same in all three, to the digit. The decode and the
render together take 26 ms of the frame's 32, the same work as the probe's
`eac3_atmos_render` row at 25.1 ms. What is left over is the `capture` sink,
which checks every sample it converts. The real `i2s` sink converts without
checking, but on this part one I2S line carries at most four 32-bit slots (two
lines, eight), so twelve slots cannot leave through it either way. The table found two things. The component
had never compiled the decoder's hot sources at `-O2` as the probe does; it
does now, for 48.6 KB of flash and no SRAM. And the level meter that makes
`result=pass` mean something squared every sample in double - a soft-float call
on this part - which cost twice the decode it was measuring.

**The network shape needs PSRAM for the decoder, and a deeper queue.** `http`
to `i2s` at `2.0` over WiFi (`sdkconfig.defaults;sdkconfig.hw;sdkconfig.psram`,
with the source, its URL, the network credentials and the control surface's
port set), playing the E-AC-3
demo concatenated eight times - 64 seconds, 2,000 access units - from a PC on
the same LAN:

| Configuration | What happened |
| --- | --- |
| Every decoder allocation in internal SRAM, as `sdkconfig.psram` had it | `abort()` in `operator new` at the first unit, and a boot loop. With WiFi's IRAM optimisations off to free more heap: 227,852 bytes free at the start, and 31 ms in, 6,144 asked for with 7,656 left. |
| Allocations of 16 KB and over in PSRAM, the default 21 ms DMA queue | Plays, levels exact, but 484 of 12,000 blocks reached an empty queue: 2.1 s of silence. |
| The same with a 64 ms queue - `sdkconfig.psram` now | Plays, levels exact, no block reached an empty queue, and the least ever left in it was 4 ms. |

The last row:

```
lap=1 frames=2000 us_per_frame=31078 worst_frame_us=36502 realtime_permille=971 render_us_per_frame=878 sink_us_per_frame=13017 resync=0 ring_low=8192 heap_free=48967
stream.rms[0]=56699
stream.rms[1]=47349
sink.writes=12000 sink.underruns=0 sink.dry_ms=0 sink.min_headroom_ms=4 sink.dma_ms=64
stream.units=2000 stream.held=0 stream.resync_bytes=0 stream.sink=i2s stream.sink_frames=12000 stream.source=http stream.fetched=3584000 stream.layout=2.0 stream.layout_mismatches=0 stream.ring_low=8192 stream.decode_stack_free=17104 stream.audio_ms=64000 stream.wall_ms=63824
```

The levels are the host's decode of the same bytes to the digit. `ring_low`
never fell below 8 KB, so the network kept up throughout; what the queue
absorbs is the decoder, which with part of its state in PSRAM took 17 ms of the
frame on average and 36 ms at worst. In the same run with progress lines on, 14
to 16 KB of internal heap stayed free while it played: enough, and not much.
Not enough for anything started after the decoder, which is why the control
surface starts before the player's tasks: its server's task stack has to come
from internal RAM, and a board run that started it 41 ms after the player found
6,787 bytes free with the largest block 3,328, too small for the 4 KB stack,
and came up with no control surface.

Then ten minutes of it, which is the player's Phase 1 exit criterion: the demo
concatenated seventy-five times, played by `POST /play` to the running board
and read back by `GET /status` every two minutes. It played 18,750 access
units in 599.9 s of wall clock for 600.0 s of audio, with levels 56,703 and
47,350, the host decode of the same bytes to the digit, and the worst frame
took 33.6 ms. The one underrun the sink line counts, 55.4 s long, is the board
sitting idle between the play it made at boot and this one. When this was
measured the queue model counted from boot, as `stream.sink_frames` does, so it
saw a queue empty since the last play when this one's first block arrived; it
has started again with each play since, so a play started after idle time no
longer counts the idle (see [What it prints](#what-it-prints)). None happened
during the ten minutes - the wall clock came in under the audio's length, which
any silence inserted while playing would have pushed over:

```
sink.writes=124500 sink.underruns=1 sink.dry_ms=55400 sink.min_headroom_ms=0 sink.dma_ms=64
stream.units=18750 stream.held=0 stream.resync_bytes=0 stream.sink=i2s stream.sink_frames=124500 stream.source=http stream.fetched=33600000 stream.layout=2.0 stream.layout_mismatches=0 stream.ring_low=6144 stream.decode_stack_free=17104 stream.audio_ms=600000 stream.wall_ms=599866
```

## Controlling it

With `CONFIG_AC3FORGE_EXAMPLE_CONTROL_PORT` set (the HTTP source's
configurations set 80; the default is 0, none), the component's
`ac3forge::Control` answers on that port:

| | |
| --- | --- |
| `GET /` | a web page that shows what the player is doing and drives it through the routes below and nothing else (below) |
| `GET /api` | these routes, as text - what `GET /` answered before the page |
| `GET /status` | what is playing and how it is going, as JSON |
| `POST /play` | body: a URL for the `http` source, a path for `fatfs` or `sd`. `202 Accepted` — the location is handed to the task that owns the player, and `/status` says how the open went. `409` from `partition`, which has one thing in it. |
| `POST /stop` | |
| `POST /volume` | body: `0.0` to `1.0`, a linear gain the decode task applies before the sink |
| `GET /layout` | the output layout, as text |
| `PUT /layout` | body: a name (`5.1.4`) or a speaker list (`L,R,C,LFE,Ls,Rs`), the same grammar as `CONFIG_AC3FORGE_EXAMPLE_LAYOUT`. Takes effect at the next play - the `i2s` sink reconfigures its mode and slot count to match, so this never needs a rebuild. `400` for text that is not a layout, `409` for one with more slots than the sink's ceiling. |

The configured location plays at boot as before; the surface can stop it and
play something else. `state` is `opening` while a play's source opens - by
then the location is the new one and the previous run's figures are gone -
then `playing`, and `finished` or `failed` when the run ends; `stopped` after
`POST /stop` or a location the source refused. A `/status` taken under QEMU
once the E-AC-3 demo had
played:

```
{"state":"finished","location":"http://10.0.2.2:8000/demo.ec3","source":"http","sink":"capture-i2s","layout":"2.0","volume":1.000,
 "stream":{"codec":"E-AC-3","acmod":7,"channels":6,"substreams":1,"dialnorm":-31,"objects":true,"objects_rendered":false,"slots":2},
 "frames":250,"held":0,"us_per_frame":4075,"worst_frame_us":46299,"render_us_per_frame":112,"sink_us_per_frame":252,"realtime_permille":127,
 "resync_bytes":0,"fetched_bytes":448000,"ring_low":6144,"passes":1,"layout_mismatches":0,"finished":true,"failed":false,"why":"end of stream","error":0}
```

Since 2026-09-11 `/status` also says how a play serves its layout. `sink_slots`,
after `sink`, is the sink's ceiling - the most it could ever be asked to carry,
not whatever it happens to be open for right now - and so the widest layout
`PUT /layout` takes. In `stream`, `layout` is the layout this play renders onto
(the top-level `layout` is the next play's), `render` is how - `loro`, `ltrt` or
`mono` for the decoder's fold, `channels` for the coded channels placed,
`objects` for the objects placed - `coded` is the stream's channels by location,
and `silent` is the layout's speakers the play has sent nothing:

```
"sink":"capture-tdm","sink_slots":12, ...
"stream":{..."slots":12,"layout":"7.1.4","render":"channels","coded":"L,C,R,Ls,Rs,LFE","silent":"Lrs,Rrs,Vhl,Vhr,Lts,Rts"}
```

Two things the player does before it decodes a unit. A stream carrying more
than one programme (§E2.3.1.2's independent substreams) plays its first; the
others' access units are skipped, as `ac3cli decode` skips them. And a stream
whose sample rate is not the sink's 48 kHz is refused rather than played at the
wrong speed: the play fails, the console says `error: sample rate failed (44100)`
and `/status` has `why` `sample rate` with the rate in `error`.

The HTTP server's task never touches the player: `/play`, `/stop`, `/volume`
and `PUT /layout` go through a queue to `app_main`, which owns the player, and
`/status` reads a snapshot under a mutex. `stream.sink_frames` in the end-of-run
line counts since boot, across every play; `stream.units`, the levels and the
sink's own line are the run's.

To reach it under QEMU, run the emulator with a port forward rather than
through `idf.py qemu`, which fixes the network options:

```bash
esptool --chip=esp32s3 merge-bin --output=build/qemu_flash.bin --pad-to-size=16MB \
  --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x0 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin \
  0x10000 build/ac3forge_stream_player.bin 0x190000 stream/sample.ac3 0x1d0000 build/storage.bin
qemu-system-xtensa -M esp32s3 -m 32M -drive file=build/qemu_flash.bin,if=mtd,format=raw \
  -drive file=build/qemu_efuse.bin,if=none,format=raw,id=efuse \
  -global driver=nvram.esp32s3.efuse,property=drive,value=efuse \
  -global driver=timer.esp32s3.timg,property=wdt_disable,value=true \
  -nic user,model=open_eth,hostfwd=tcp::8080-:80 -nographic -serial mon:stdio
curl http://127.0.0.1:8080/status
curl -X POST -d 0.5 http://127.0.0.1:8080/volume
curl -X POST -d http://10.0.2.2:8000/demo.ec3 http://127.0.0.1:8080/play
```

(`qemu_efuse.bin` is what `idf.py qemu` generates on its first run.) On
2026-09-10 that sequence played the demo twice, the second time at half volume
with per-channel levels exactly half the first's, stopped on request, and
reported a refused `ftp://` location on the console.

The QEMU shape without PSRAM is the tightest this example runs: WiFi's
stand-in, lwIP, the HTTP client, the HTTP server, the E-AC-3 decoder and the
player's two stacks in one 280 KB, so `sdkconfig.ci-http` gives it an 8 KB
ring and a 24 KB decode stack, with the measurements that justify both in its
comments. A board has a squeeze of its own: the radio needs more internal RAM than
QEMU's Ethernet stand-in, and with WiFi up the decoder does not fit beside it.
`sdkconfig.psram` is what the board's network shape needs, and
[On the board](#on-the-board) has the measurements behind it.

## The sources

`partition` runs without hardware, which is why it is the default and the one
CI exercises first. `sd` cannot: QEMU has no SD host, so CI runs the same file
layer from a FAT volume in flash (`fatfs`) and only the SDMMC host waits for a
board. `http` **runs under QEMU too**, since 2026-09-10: QEMU has no WiFi but
`idf.py qemu` attaches an OpenCores Ethernet MAC to the host's network, so the
source has a network seam of its own — [`main/source/http/network.hpp`](main/source/http/network.hpp),
`net/wifi/` for a board and `net/openeth/` for the emulator — and
`sdkconfig.ci-http` selects the latter with the stream served from the host:

```bash
python3 -m http.server 8000 --bind 0.0.0.0 --directory apps/wasm/assets &
SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci-http" idf.py build
idf.py qemu
```

The guest is 10.0.2.15 and the host 10.0.2.2 in QEMU's user-mode network, so
the URL is `http://10.0.2.2:8000/demo.ec3` and no firewall is involved. The
stream is the WASM page's demo: E-AC-3 5.1 with JOC objects, 448 kbit/s, 250
access units. On 2026-09-10 the run fetched and decoded all 250 with zero
resynchronised bytes, and its per-channel levels matched the host's
`ac3cli decode demo.ec3 out.wav downmix=loro drcmode=line` to the digit —
56,673 and 47,346 — which is the check CI holds it to. Three `E (esp_eth)`
lines about multicast filters print at start-up: the emulated MAC has no
filter, IDF says so, and nothing depends on one. What QEMU cannot say is
anything about WiFi, or about time: its `realtime_permille` is shape only.

**Streams to point it at.** [`www/`](www/README.md) is a set of them to serve:
7.1.4 streams that reach all twelve slots of a 7.1.4 output, and beside them
E-AC-3 at the seven layouts from 1.0 to 7.1.4, AC-3 at 2.0 and 5.1, dependent
substreams, two programmes, dual mono, each Annex E coding tool, short frames,
VBR, DRC words, other encoders' streams and objects. `www/streams.json` says
what each one is and the level each slot of a 7.1.4 output should get from it,
and CI plays the set under QEMU onto 7.1.4 (`sdkconfig.ci-http714`) and holds
every slot to it.
[`planning/esp32-stream-set.md`](../../../../planning/esp32-stream-set.md) has
what was measured - which streams a network shape without PSRAM can play, and
which need a board with it.

Two things differ between the sources and are worth knowing before writing a
third:

**Length.** A partition is the only source with none. Every other kind has one —
`Content-Length`, a file size — and without it the player reads the whole 256 KB
partition while the framer skips a quarter of a megabyte of erased flash looking
for a sync word, on every lap. The build supplies it from the file's own size.

**Rewind.** `http` cannot. A socket has delivered what it delivered;
re-requesting the URL would be a new stream, not a rewind, and the decoder's
overlap-add state would carry across the seam as a click. `source_rewind()`
returns false and the player stops rather than pretending.

## Layouts, and more than two channels

The player is configured for the speakers it has, not for the stream it is
sent. `CONFIG_AC3FORGE_EXAMPLE_LAYOUT` names them, one per output slot, either
as a name — `2.0` (the default), `5.1`, `7.1`, `5.1.4`, `7.1.4`, `9.2.4`,
`5.0.4` — or as a speaker list, one token per slot in slot order:
`L,R,C,LFE,Ls,Rs` for a 5.1 DAC wired in WAV order, `30/0,-30/0,lfe` by angles,
`-` for a slot nothing is on. A name is Table E2.5's order with the LFE last, so
`5.1` is L C R Ls Rs LFE; a list is whatever order the board is wired in. The
grammar is [`ac3forge/layout.hpp`](../../include/ac3forge/layout.hpp)'s and
`PUT /layout` on the control surface takes the same text for the next play.

What happens to a stream depends on the layout, not the stream:

| Layout | How |
| --- | --- |
| `2.0`, `1.0` | The decoder's own §7.8 fold (`CONFIG_AC3FORGE_EXAMPLE_STEREO_FOLD` picks Lo/Ro or Lt/Rt). What every player before 2026-09-10 did, unchanged. |
| anything wider, no heights | As coded. Each coded channel goes to the slot of its own location exactly, or, where the room has no such speaker (a 7.1 stream's rears in a 5.1 room), is spread over its neighbours by `ac3::spatial::pan_direction` at constant power. The LFE goes to the LFE slots and nowhere else. |
| with heights | As above for a stream without objects. For a stream with an object layer the objects are reconstructed and placed by their own positions, the bed's LFE passes through, and the bed's other channels are **not** added — an Atmos bed is the objects' own 5.1 fold, and adding it would play everything twice. `CONFIG_AC3FORGE_EXAMPLE_OBJECTS` widens or narrows when that happens. |

**Nothing is upmixed.** The renderer never makes a signal for a speaker out of
other channels: a slot gets a coded channel at its location, a coded channel
with no slot of its own spread onto it, an object placed near it, or, for an
LFE slot, the LFE. A 5.1 stream on `7.1.4` leaves the rear surrounds and the
four heights at exactly zero. At `2.0` and `1.0` the fold takes in every channel
but the LFE, heights and rear surrounds included - the decoder first puts each
location in one of §7.8's seats, a height in L or R and a rear or top surround
in Ls or Rs, each at -3 dB - and leaves the LFE out, as §7.8 does by default.
`GET /status` says which of these a play is doing and which of the layout's
speakers it has sent nothing, and the web page puts both into words
([Controlling it](#controlling-it)).

Until 2026-09-11 the fold cost memory and time the as-coded render does not.
Under QEMU's network shape, which has no PSRAM, a stream with a four-channel
dependent substream - 7.1, 5.1.4, 7.1.4 - aborted at `2.0` for want of 6 KB in
the fold's scratch; on the board over WiFi, where the fold fitted, a 7.1.4
stream at `2.0` took 36 ms to decode each 32 ms frame and fell behind. The
output stage now folds 256 samples at a time: its scratch fits a shape without
PSRAM, and on the board the same 7.1.4 play decodes in 30 ms a frame, level
with real time. The same stream decodes and renders onto twelve slots in about
30 ms. See
[`planning/esp32-stream-set.md`](../../../../planning/esp32-stream-set.md#on-a-board)
and [Folded to stereo](../../../../docs/platforms/esp32.md#folded-to-stereo).

All of it is [`ac3forge/render.hpp`](../../include/ac3forge/render.hpp), one
256-sample block at a time, which is why a 7.1.4 layout costs the player 16 KB
of block storage rather than 96 KB of frame. The geometry is the library's
(`tests/spatial/`); what the header adds is indexing between coded channels,
objects and slots, tested on the host in `tests/io/test_layout.cpp` because a
swapped subscript there puts the centre in the subwoofer and nothing complains.

CI renders one under QEMU (`sdkconfig.ci-render`): the footprint probe's
height-object fixture — five objects over a 5.1 bed, three on the ceiling —
onto `7.1.4` through the twelve-slot TDM conversion, and checks every slot's
level against the probe's own `eac3_atmos_render` reference. Reconstructing the
objects costs about 148 KB of heap in the MDCT-band domain the fixture was
encoded in and about 233 KB in the QMF domain a real stream needs
(`CONFIG_AC3FORGE_EXAMPLE_JOC_DOMAIN`) — PSRAM territory on a board, and the
reason the QEMU shape runs an 8 KB ring.

### The I2S sink

`i2s` is one sink, not a choice between a stereo one and a TDM one: it opens
standard I2S for one or two channels and TDM for three or more, reconfiguring
between them - and between slot counts within a mode - as the layout in force
changes, rather than a build fixing one shape and staying there
([`ac3forge/sink_plan.hpp`](../../include/ac3forge/sink_plan.hpp) decides
which). `PUT /layout` takes effect this way at the very next play: no rebuild,
no reflash, just whatever the new layout needs.

**The hardware ceiling this cannot get past.** On an ESP32-S3 one I2S line's
TDM frame holds at most 128 bits, because the peripheral's half-frame length
is a 6-bit register field: four slots at 32 bits - a 6.1 MHz bit clock at 48
kHz - or eight at 16, and ESP-IDF v6.1 refuses more, as does this sink before
it ever asks the driver. 16-bit slots stay standard-mode-only here regardless
(`CONFIG_AC3FORGE_EXAMPLE_I2S_SLOT_BITS`, 32 by default, 16 for a DAC that
insists): a padding-capable TDM interleave at that width does not exist in
this codebase, so a layout past two slots at 16 bits is refused rather than
attempted. `CONFIG_AC3FORGE_EXAMPLE_I2S_SECOND_LINE` brings up a second,
independent I2S peripheral at 32 bits to double the ceiling to eight, sharing
line 0's BCLK and WS as inputs - through the GPIO matrix, which routes a pad's
input side to a peripheral independently of whichever end drives it as an
output, so this needs no external wire, only the second line's own DATA pin
(`CONFIG_AC3FORGE_EXAMPLE_I2S_DOUT2_GPIO`) - to keep both lines' frames sample
aligned. A 7.1.4 layout's twelve slots of 24-bit audio still do not fit
either way; the `capture` sink stands in for that (below). Whichever DAC or
DSP is on the wire has to speak whatever mode a channel count lands it in - a
PCM3168A speaks TDM, a SigmaDSP does on its serial inputs, the common
MAX98357A and PCM5102 breakouts do not, and neither speaks a second,
independent TDM line at all. `CONFIG_AC3FORGE_EXAMPLE_I2S_SLAVE` hands BCLK
and WS to line 0's other end instead of generating them - how an ADAU1452 or
ADAU1467 that is the house's clock wants it; a second line is always a slave,
since its only job is reading those same two pins.

**Reconfiguring, not rebuilding.** A slot-count change that stays within one
mode (say 2 channels to 4, both TDM) uses
`i2s_channel_reconfig_std_slot`/`_tdm_slot` rather than tearing the channel
down; crossing standard/TDM, or a line coming up or going down entirely, does
tear it down and recreate it. Every channel this sink ever creates asks for
the same DMA depth regardless of how many slots it is carrying at the time -
sized once, from that line's own ceiling rather than from the layout in hand,
the way both sinks used to size their descriptors from the bus width
([`main/sink/sink_common.hpp`](main/sink/sink_common.hpp)): the driver caps a
descriptor at 4,092 bytes and quietly shortens one that asks for more, and
each descriptor divides the player's 256-frame block - 128 frames for stereo,
64 for a twelve-slot layout at capture - because ESP-IDF v6.1's
`i2s_channel_write` abandons a partly written buffer whenever two or more sent
ones are waiting, and the rest of it goes out as silence. [`i2s_player`](../i2s_player/README.md)
measured what that costs a player that writes across descriptors: 3 ms in
every 35. Whether keeping the DMA depth constant across a reconfigure also
keeps ESP-IDF from reallocating the buffers under it, rather than just saving
the channel recreation around them, has not been confirmed on a board.

**Reconfiguring between standard and TDM mode, in both directions, has run on
hardware.** On the same ESP32-S3-DevKitC-1-N16R8 as [On the board](#on-the-board),
over WiFi, no DAC wired: a play at `2.0` (standard mode, the decoder's own
fold), then `PUT /layout 4.0` and a new play with no reflash in between,
reconfigures line 0 to TDM and plays the same six-channel stream spread onto
four slots at 970 per mille of real time; `PUT /layout 2.0` and a further play
reconfigures it back to standard mode and plays that too, at the host's RMS to
the digit. Neither crossing left the control surface any less responsive than
before it.

**The second line reconfigures correctly and has not proven itself past
that.** Bringing one up (`AC3FORGE_EXAMPLE_I2S_SECOND_LINE=1`) for a
six-channel, unfolded `5.1` layout did the right thing in every way this can
check without a second DAC on the wire: both lines came up in TDM mode, line 1
shared line 0's BCLK/WS as the design intends, and `sink_slots` read 8. What
it did not do was leave enough contiguous internal RAM for the decode task's
own stack to start, on top of WiFi's footprint and two lines' worth of DMA
buffers rather than one - `heap_caps_malloc could not allocate 32768 bytes`,
`player: could not start the decode task`, and the play failed cleanly rather
than hanging or crashing. A real memory budget this project has now measured,
not a wrong answer; whether the two lines' samples stay aligned once
something is actually wired to both remains to be seen, and
`AC3FORGE_EXAMPLE_I2S_SECOND_LINE`'s own help text has the numbers behind
both findings. The slave role (`AC3FORGE_EXAMPLE_I2S_SLAVE`) is untested
either way: there is no DAC or DSP here that drives the clocks.

**What CI establishes about the real `i2s` sink itself, without a board, is
that it compiles and links.** qemu-system-xtensa has no I2S, so every CI run
that decodes and checks samples plays them through `capture` instead: eight
slots for the padding check (`sdkconfig.ci-tdm`), twelve for a rendered 7.1.4
(`sdkconfig.ci-render`, `sdkconfig.ci-http714`) - both wider than the real
sink's own ceiling on any board this project has, one line or two. The build
matrix (`main/CMakeLists.txt`'s sink choice) compiles `i2s` as well, so an IDF
component rename or a driver API change is caught there, but nothing under
QEMU runs it. The exceptions are the parts worth testing without a board at
all, free of
ESP-IDF and unit-tested on the host:
[`ac3forge/interleave.hpp`](../../include/ac3forge/interleave.hpp)
(`tests/io/test_interleave.cpp`), because planar-to-interleaved indexing with
slot padding is where the bugs are; the mode/slot-count arithmetic itself,
[`ac3forge/sink_plan.hpp`](../../include/ac3forge/sink_plan.hpp)
(`tests/io/test_sink_plan.cpp`); and the queue model behind the `sink.*` line,
[`ac3forge/dac_queue_model.hpp`](../../include/ac3forge/dac_queue_model.hpp)
(`tests/io/test_dac_queue_model.cpp`), which runs there against a simulated
DMA. The rest of this sink is peripheral setup that either works on a board or
does not.

The padding is the part that bites. A TDM frame is a fixed shape, so a 5.1
layout on an 8-slot bus leaves two slots with nothing to carry — and they must
be **written as zeros, not skipped**. The DMA buffer is reused, so whatever the
previous block left there is what the DAC clocks out: two channels of stale
audio nobody is listening for and everybody can hear. There is a test for
exactly that, and the `capture` sink checks it on the target. `capture`
converts up to sixteen slots with no peripheral behind it and no hardware
ceiling to refuse against, which is how CI checks a twelve-slot conversion
that no real line here could carry at all; `CONFIG_AC3FORGE_EXAMPLE_TDM_SLOTS`
sets its emulated width, unrelated to the real sink's own ceiling.

## What it costs

`idf.py size`, IDF v6.1, `-Os`, the default shape (`partition` to `i2s`, `2.0`)
with the console on USB-Serial-JTAG, with the player on the block form. The
decoder's hot sources at `-O2` move flash, not these figures:

| | Bytes |
| --- | --- |
| Internal SRAM (DIRAM) used by the image | 88,563 |
| …of which `.bss` | 46,232 |
| …leaving for the heap, by the linker's estimate | 253,197 |
| Taken from that heap when the player starts: one block of sixteen slots, the framing buffer, the staging block and the renderer's gain tables | 37,376 |
| The ring between fetch and decode (`CONFIG_AC3FORGE_EXAMPLE_RING_BYTES`; PSRAM when present) | 32,768 |
| The decode task's stack, and the fetch task's | 32,768 + 8,192 |
| Interleave buffers (one block, 32-bit and 16-bit, static, in the sink) | 3,072 |
| I2S DMA queue (8 × 128 frames, stereo, 32-bit: Kconfig's 4 × 240, reshaped so each descriptor divides a block) | 8,192 |

Not in the table because it is the decoder's: the block form keeps one frame of
the coded channels inside the decoder rather than in storage the player owns,
so the frame of PCM the player used to allocate (eight channels, 49,152 bytes)
has moved rather than gone, and now grows with the coded width rather than with
a fixed ceiling. Under QEMU the AC-3 shape shows 168,924 bytes free after a
pass against the 204,484 the frame form left, and the E-AC-3 HTTP shape 122,376
against 63,752 - the two decoders keep different things. Reconstructing
objects for a height layout adds about 148 KB in the MDCT-band domain and
233 KB in the QMF domain, allocated at the first unit that has them; that is
what `sdkconfig.psram` is for on a board, and why the QEMU render shape runs an
8 KB ring. Every source and sink's components are linked whichever pair is
selected (`main/CMakeLists.txt` says why the `REQUIRES` list cannot follow the
choice). The `http` shape adds the WiFi and TCP/IP stacks on top: with PSRAM
off, WiFi's stand-in, the E-AC-3 decoder, the player's stacks and a 32 KB ring
did not all fit under QEMU, and the CI shape runs an 8 KB ring in internal SRAM
with the main task's stack cut to 8 KB.
