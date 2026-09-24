# Hearth sink

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
between a slow read and silence. `main/hearth_sink.cpp` is what is left: two
adapters from the seams to the player's `ByteSource` and `PcmSink`, a level
meter, and the reporting. The ring's size, its placement in PSRAM, and both
cores are under *ac3forge hearth sink* in `idf.py menuconfig`.

| Source | Sink |
| --- | --- |
| `partition` — flash (default) | `i2s` — an I2S DAC (default); standard mode or TDM, reconfigured to whatever the layout needs, on up to two of a part's I2S lines (the ESP32-S3, the ESP32-C6) |
| `sd` — SD card over SDMMC | `i2s_wide` — the same arithmetic, one line, for a part whose one controller reaches the product's full channel target alone at a wider frame (the ESP32-P4) |
| `fatfs` — a FAT volume in flash | `capture` — converts and checks; what CI runs |
| `http` — an HTTP body over WiFi | `null` — counts blocks |

Chosen in `idf.py menuconfig` under *ac3forge hearth sink*, with the output
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

On native Windows, run that first line from **PowerShell**, not Git Bash:
ESP-IDF's own tooling refuses MSYS/MinGW shells outright, and
`idf.py`/`export.sh` fail with `ERROR: MSys/Mingw is not supported. Please
follow the getting started guide of the documentation to set up a supported
environment.` Use `export.ps1` from PowerShell instead of `export.sh`; WSL is
unaffected — this is specifically about MSYS2/Git-Bash-style shells.

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

Building more than one target shape from this project source at once — say
esp32s3, esp32c6 and esp32p4 together, each with its own `-B` — also races the
component manager: `idf.py build` downloads dependencies (`mdns`, `esp_hosted`,
and the like) into `managed_components/` inside the project directory, not
under each target's own `-B`. Two builds populating
`managed_components/espressif__mdns` at the same time can collide
mid-`copytree` with `FileExistsError: [WinError 183] Cannot create a file when
that file already exists`; the losing process fails at `os.makedirs`, before
writing anything, so a build that reports success was not corrupted by a
concurrent loser. Build multiple target shapes from one project source
serially, or expect the occasional build to need one retry once the others
have finished.

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
[`docs/platforms/bare-metal/esp32-s3.md`](../../../../docs/platforms/bare-metal/esp32-s3.md#timing). The
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
checking, and on this part one I2S line carries 128 bits a frame: four 32-bit
slots, or eight 16-bit ones, so twelve slots leave through it only at 16 bits
with both lines wired. The table found two things. The component
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
| `GET /name` | what the board calls itself, as text |
| `PUT /name` | body: a name, up to 32 bytes. Stored on the board, so it survives a reflash; it is the mDNS instance name and what a server lists the sink under. `409` if it does not fit or NVS refused it. |
| `GET /wiring` | `1` if a second I2S line is wired, `0` if not |
| `PUT /wiring` | body: `1` or `0`. Moves the sink's ceiling with it - two lines carry twice one line's slots - and takes effect at the next play. `409` while a play is running. |
| `PUT /network` | body: an SSID, a newline, then the passphrase. Stored for the next boot; the station stays on the network it is already associated with. Improv over the serial port is the other way in, and the one a board with no network at all needs. |
| `POST /pairing` | body: `reset`, `cancel` or `forget`, for a Sendspin player's pairing ([Pairing](#pairing)). `400` for any other body, `409` on a board with no Sendspin player. |
| `GET /slot-width` | the slot width in bits, 16 or 32 |
| `PUT /slot-width` | body: `16` or `32`. Takes effect at the next play, and moves the sink's ceiling with it: an I2S line carries 128 bits a frame, so two lines reach sixteen slots at 16 bits and eight at 32. `400` for a body that is not a number, `409` while a play is running, for a width the sink does not have, or on the `capture` and `null` sinks, which keep the width they were built for. A layout already set may be too wide after a change to 32; the next play says so. |

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
  0x10000 build/ac3forge_hearth_sink.bin 0x190000 stream/sample.ac3 0x1d0000 build/storage.bin
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

## Playing from a Sendspin server

`sdkconfig.sendspin` makes the board a Sendspin player
([planning/hearth-reference-player.md](../../../../planning/hearth-reference-player.md),
B3), on WiFi with the page on port 80:

```bash
SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.hw;sdkconfig.psram;sdkconfig.sendspin" idf.py build
```

[An ESP32-S3 sink](../../../../docs/hearth/sink-esp32-s3.md) takes a board
through this step by step: flashing, joining a network, pairing, groups,
wiring and slot widths.

Nothing plays at boot. The player listens on port 8928 at `/sendspin`, the
board advertises `_sendspin._tcp` under its name, and a server that finds it
dials it. Two roles are offered:

- `_ac3forge_player@v1`
  ([planning/hearth-sendspin-extension.md](../../../../planning/hearth-sendspin-extension.md)):
  AC-3 or E-AC-3 in IEC 61937 bursts, decoded on the board, rendered onto its
  layout, then routed, trimmed and delayed as the server's settings say. The
  server can also set the layout, the crossover and the decoder's settings
  (operating mode, heavy compression, dialnorm, the downmix and its phase
  shift, the LFE mix, the programme, objects and concealment), play an
  identify tone on one output, and change the volume. `ac3hearth` plays this
  role.
- `player@v1`: stereo PCM at 48 kHz, 24 or 16 bits, which Music Assistant
  sends to a player that lists nothing else. FLAC and Opus are not offered:
  their decoders are not on the board, and what they would cost it has not
  been measured.

`POST /play` still plays a URL over HTTP, for debugging. While it plays, the
Sendspin player reports itself unavailable to its servers, and it takes the
sink back when the play ends.

### Joining a network

A board with no network stored listens for [Improv Wi-Fi](https://www.improv-wifi.com/)
on its serial port: the page at improv-wifi.com, in a browser with Web Serial,
connects to the board and gives it an SSID and a passphrase, which the board
stores and joins. A board already on a network moves to another with
`PUT /network`, at its next boot.

A board that could not join its network at boot also listens: the network
stored or built in may have gone, or its passphrase may be wrong. If a join
over Improv fails, the client reports that it could not connect, and the board
keeps listening for another network. A network that accepts the board but
gives it no address within 30 s counts as a failed join, and the board stays
associated in case an address comes later. Once a join succeeds, the board
advertises itself and starts the Sendspin player, as it would at boot. It
does not restart.

### Keeping the network

**A board does not give up on the network it has been given.** A join that
fails, and a network the board has joined and then lost, are both retried the
same way: `CONFIG_AC3FORGE_EXAMPLE_WIFI_RETRIES` tries at once (five by
default, about 15 s against a network that is not there), then after 1, 2, 4
and 8 s, then every 15 s, for as long as the board runs or until it is given
another network. So an access point that restarts, and a board that boots
before its access point after a power cut, both end with the board back on
the network by itself:

```
network: lost 'kitchen' (reason 200); rejoining
network: back on 'kitchen' after 140 s, address 192.168.1.45
```

The quick tries are what `CONFIG_AC3FORGE_EXAMPLE_WIFI_RETRIES` bounds, so a
wrong passphrase is still *reported* in about 25 s rather than waited on for
good: `network_up()` returns false, the console says why, and Improv answers
`cannot_connect`. The board goes on trying in the background all the same, and
says so:

```
error: could not associate with 'kitchen'
network: still trying 'kitchen' in the background
```

While a board is off its network, `network_ready()` is false, and with it:

- mDNS stops answering, and announces the board again when it has an address.
- Improv's current state is *ready*, not *provisioned*, and carries no page
  address. A client can hand such a board another network, which it joins at
  once, dropping the one it was retrying. (A board that is *on* a network
  keeps it: a network given to it over Improv or `PUT /network` is the one it
  joins at its next boot, and the console says so.)
- The page and the Sendspin player keep listening. Nothing is started again
  when the network comes back; a server finds the board once mDNS answers and
  connects to it as it did before.

**What a board did**, on an ESP32-S3-DevKitC-1-N16R8 against an access point
switched off and on — a second ESP32-S3 running a SoftAP on its own, so the
outages are exact:

| What happened | The board noticed | It was back |
|---|---|---|
| Access point off for 60 s, board idle | after 8.5 s (reason 200, beacon timeout) | 1.2 s after it returned |
| Access point gone without a word for 125 s, as in a power cut | after 9.1 s | 14.7 s after it returned |
| Board booted while the access point was off, which came back 45 s later | the join failed 15 s into the boot | 12.5 s after it returned |
| Access point off for 35 s while a Sendspin stream played | after 8.5 s | 8.7 s after it returned |

The board reports a loss only when its beacons stop arriving, which is why
every outage above took about 9 s to notice; an access point that says goodbye
first is noticed at once. Coming back takes at most the 15 s between tries
plus the scan, which is the 14.7 s above.

In the third row the board did at its own pace everything a join at boot would
have: it advertised itself over mDNS and started the Sendspin player, 12.5 s
after the access point came back, with no restart.

**A stream that is playing when the network goes** ends where it stopped: the
connection is closed with the network, the player prints the stream's figures
and gives the sink back, and its memory goes with it — which matters, because
the network shape runs with about a kilobyte of internal heap free while a
stream plays. A few small internal allocations failed in the seconds between
the access point going and the board noticing, with no effect on anything, and
the board rejoined from there. The play that followed the rejoin started with 9
chunks late and 3 underruns while the clock filter converged again, about a
second of audio; the next play was clean (630 of 630 bursts, nothing late).

### Pairing

Every connection is encrypted with Noise (`KKpsk2`, ChaChaPoly by default;
`CONFIG_AC3FORGE_SENDSPIN_SUITE` chooses AES-GCM). A server plays to the board
once they are paired, which happens one of two ways:

- **By the token.** The console prints the board's pairing token at boot, and
  again for `pair token`:

  ```
  sendspin: pairing token SP:0AO4BQKDC3YEKAULMZ4UMZFDRQGZSLVY9...
  ```

  A server given the token pairs at once, with no code. Anyone who has it can
  pair, so it is printed nowhere else: not on the page and not in `/status`.
- **By a code.** A server that asks to pair by a code gets six digits, on the
  console as `sendspin: PAIRING CODE 482-913` and at the top of the page's
  Sendspin section, for as long as the pairing runs. The code goes into the
  server. After twenty codes that did not match, the board holds pairing back
  until `pair reset` on the console or *Allow pairing again* on the page.

The board keeps eight pairings in NVS. `pair forget`, or *Forget every server*
on the page, removes them all and gives the board a new identity, so every
server has to pair again. Lines typed on the console are commands: `pair
token`, `pair reset`, `pair cancel`, `pair forget`, and `sendspin`, which
prints the player's state.

A server that has not paired gets nothing to play unless
`CONFIG_AC3FORGE_EXAMPLE_SENDSPIN_UNPAIRED_ACCESS` is set, and then only once
its own operator approves the board.

### When a sample plays

A server stamps each chunk with the time its first sample should play, on
its own clock, and the player follows that clock with Sendspin's time
filter
([`clock_sync.hpp`](../../../../src/sendspin/include/ac3/sendspin/clock_sync.hpp)):
bursts of eight exchanges, one after another until the filter has converged,
then thirty a second apart, then one every ten seconds. Convergence itself is
confirmed rather than taken on trust: once a run of bursts reads as converged,
one more burst, a learning interval later and so genuinely apart in time, must
measure within a millisecond of that run before the player calls the clock
converged and a stream is let start. Without that, a run taken in the seconds
right after a Wi-Fi reconnect - where reassociation, mDNS's re-announce and an
ARP round can all delay a reply the same way - could read as converged on an
offset that was still off by several milliseconds, since the filter's own
error estimate says only that its samples agree with each other. A burst
whose replies all came back late, as they do behind a stream's chunks, is
left out. A
chunk's local time is worked out when it arrives, which may be seconds before
it plays, and it is moved by as much as the clock has moved by the time it
does. The I2S sink says when each buffer it is given will play, from the
channel's own end-of-frame interrupts
([`ac3forge/playout.hpp`](../../include/ac3forge/playout.hpp)). The player
pads the start of a stream with silence, or leaves out the frames already
late, so that its first frame plays when the server asked; after that it
drops or repeats one frame in 256 while the smoothed error is outside
250 µs. Corrections are made to decoded PCM, never to a burst. A burst that
arrives too late to play any of is dropped before it is decoded. The
`capture` and `null` sinks have no interrupts, and pace and time themselves
as a DMA ring would. The capture sink does so only with
`CONFIG_AC3FORGE_EXAMPLE_CAPTURE_PACED`; unpaced, it plays each frame as it
comes, which is what CI compares levels with.

### What it reports

`/status` gains a `sendspin` object: the server playing to the board and its
dialect (`specification`, or `aiosendspin 9.1.1` for Music Assistant), the PSK
the connection uses, the active role, whether the clock has converged, a
pairing code while one runs, and for the stream the bursts played, underruns,
bursts late, dropped for want of room or invalid, how far from the server's
time the stream plays (`error_us`, and `worst_error_us` since its first
second), where it put the stream's first frame on the server's clock
(`origin_server_us`, which two boards playing one programme should agree on),
and each output's peak and RMS over the last 100 ms. The same levels and
counters reach the server in the extension role's `client/state`, up to ten
times a second while a stream plays.

The object is `null` until the player has started, which it has only once its
WebSocket server is listening. A board that joins a network over Improv starts
its player just as the client is given the page's address, so a page opened at
once can see `null` for a moment. A `PUT /layout`, `/name`, `/wiring` or
`/slot-width` sent before the player has started still reaches it.

When a stream ends, the console prints its figures and each output's RMS
over the whole stream:

```
sendspin.stream=bursts bursts=315 late=0 dropped=0 invalid=0 underruns=0 resyncs=0 silence_frames=256 skipped_frames=0 dropped_frames=0 repeated_frames=0 worst_error_us=0 burst_us=3940 sink_us=182 ring_high=0 heap_free=43700
sendspin.rms[0]=7891
sendspin.rms[1]=7414
```

`heap_free` is the least free internal heap while the stream played, and
`ring_high` the most the ring between the network and the decoder held.
`burst_us` and `sink_us` are the average time per chunk spent decoding and
rendering, and inside the sink's write.

While a stream plays, a `sendspin.progress` line gives the same figures so
far every `CONFIG_AC3FORGE_EXAMPLE_REPORT_EVERY_FRAMES` chunks (125, about
four seconds, in `sdkconfig.sendspin`), with the internal heap free now, its
largest block, and the least since the stream began:

```
sendspin.progress chunks=18750 late=0 underruns=0 resyncs=0 burst_us=20074 worst_burst_us=41083 sink_us=11410 ring_high=117504 heap_free=723 heap_largest=108 heap_least=43
```

The player's buffers, stacks, lead and ring are under *Sendspin player* in
`idf.py menuconfig`.

### On two boards

On 2026-09-16 two ESP32-S3-DevKitC-1-N16R8 boards on the same Wi-Fi, with no
DAC wired, played the E-AC-3 JOC fixture
(`tests/golden/object-fixture/dee_joc_514.ec3`) for ten minutes as one group
from `ac3hearth-testserver`, beside a test sink of its own. One played 2.0 on
32-bit standard I2S, the other 5.1 on eight 16-bit TDM slots
(`CONFIG_AC3FORGE_EXAMPLE_I2S_SLOT_BITS=16`: one line carries four 32-bit
slots). Each board already held an unpaired connection from another server
on the network, which runs Music Assistant, and displaced it when the test
server's connection came.

| | 2.0 board | 5.1 board |
|---|---|---|
| Bursts played | 18,774 of 18,774 | 18,774 of 18,774 |
| Underruns, late, dropped | 0, 0, 0 | 0, 0, 0 |
| Decode and render per burst, average (worst) | 20.9 ms (40.3 ms) | 24.3 ms (48.8 ms) |
| Least internal heap free | 139 bytes | 23 bytes |
| Decode task's stack unused, of 32,768 bytes | 13,916 bytes | 13,840 bytes |
| Sendspin server's stack unused, of 8,192 bytes | 2,960 bytes | 2,960 bytes |

At each of the 602 seconds the test server read both boards' `/status`, their
`origin_server_us` were within 549 µs of each other; read four times a second,
99% of the readings were within 386 µs. The 2.0 board's RMS was the test
sink's to the digit, 7,891 and 7,414.

Four things were found on the boards before that held, and each is now in the
firmware or the library:

- Modem sleep is off (`main/net/wifi/network.cpp`). With it on, the access
  point holds what it sends the board until the board next wakes: a clock
  exchange reads that as an offset that moves by milliseconds, and a server's
  read-ahead stalled the stream's start on retransmissions for up to 700 ms.
- Nagle's algorithm is off on the player's sockets, and a WebSocket frame goes
  out in one write. With Nagle on, a frame's second segment waited for the
  server's delayed acknowledgement of the first, the server's read gave up in
  that gap, and pairing failed.
- lwIP's task is pinned to core 0 in `sdkconfig.sendspin`, with the network and
  the sessions, so it does not take core 1 from the decoder.
- The clock's learning bursts, the bursts it leaves out, and chunks moved by
  the clock as it stands, all under [When a sample plays](#when-a-sample-plays).

Internal RAM is what the network shape is short of. While a stream plays, the
decoder's allocations of up to 16 KB go to internal RAM first
(`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` in `sdkconfig.psram`) and take nearly
all the network leaves. Nothing failed in the ten minutes; in a one-minute run
before it a 108-byte internal allocation did, with no effect on the stream.
Two changes were tried and not kept. Sending allocations over 4 KB to PSRAM
first left 80 KB free, but a burst then took 28 ms rather than 20, and the
board underran. A larger `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` changed
nothing: ESP-IDF v6.1 lets ordinary small allocations take that reserve once
the rest of internal RAM is full.

### On the ESP32-C6

Building and flashing the Sendspin player, in that order - `sdkconfig.hw` puts the console (and
Improv) on the USB-Serial-JTAG port, `sdkconfig.c6` is the part's own overlay (flash mode, clock,
partition table), and `sdkconfig.sendspin-c6` sizes the player for a part with no PSRAM; this is
the same combination each of those three files' own header comments name:

```bash
. $IDF_PATH/export.sh
idf.py -DIDF_TARGET=esp32c6 \
  "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.hw;sdkconfig.sendspin;sdkconfig.c6;sdkconfig.sendspin-c6" \
  build
idf.py -p <PORT> flash monitor
```

CI builds this combination (not the network-loaded baremetal probe two directories over) but does
not run it - `idf.py qemu` refuses `esp32c6` outright, so nothing shorter than a board proves it
still plays. [Joining a network](#joining-a-network) and [Pairing](#pairing) above are unchanged
on this part; only what follows in this section is C6-specific.

The C6 has one core, which the Sendspin server task and the decode task
(priority 5 and 6) share. A clock reply used to be dated by when the server
task read it, and a burst's decode holds that task off the CPU for up to
about 45 ms; every reply read during a burst looked that late, and once
thirty bursts in a row had been left out of the clock's filter this way
(`ClockSync::kFloorBursts`), the offset followed the delay and jumped 13 to
31 ms. A reply is now dated by when its bytes reached the board instead: an
lwIP IPv4 input hook (`ac3forge/tcp_arrivals.hpp`, installed through
`ESP_IDF_LWIP_HOOK_FILENAME` on the `lwip` component) logs each Sendspin
connection's TCP stream from its SYN, on the network task, well above the
decode task; `PlayerSession::receive()` takes an arrival time from that log
instead of reading its own clock at the point it happens to be scheduled. A
ten-minute play with the fix kept every clock reading within 651 us of the
server's own clock, with no jump.

`sdkconfig.c6` reads the flash in quad I/O mode rather than the default
single-I/O DIO: on this board that left the part 6 to 9% idle while a stream
played, where DIO left about 1%, and took a burst's decode and render from
22.4 to 20.7 ms - every task, the decode included, spends less time waiting
on flash reads through a narrower bus. A bootloader that cannot set a flash
chip's quad-enable bit stays in DIO on its own.

Sendspin's ring (`sdkconfig.sendspin-c6`) is 48 KB, up from an earlier
32 KB. Playing AC-3 2.0 over WiFi for five to ten isolated minutes at a
time, this board's WiFi link went quiet for close to a second every few
minutes - its wifi and tcpip tasks' shares of the part fell by half or more
rather than rose, so this was a gap in the link, not a backlog of work
still to do - and by the time it cleared, a 32 KB ring had run dry and the
chunks queued behind the gap arrived too late to play: 43 to 131 of them a
run, in one or two clusters. 48 KB cut that by about two thirds with the
internal heap never short an allocation across several ten-minute runs. A
64 KB ring stopped it entirely in one ten-minute run, but at the cost of
two failed 1,848-byte WiFi receive-buffer allocations against a
1,824-byte largest free block - the fragmentation the WiFi and PHY IRAM
options above are there to avoid, back once the ring leaves this little
room for it. 48 KB is the ring kept.

On 2026-09-22, this ESP32-C6 and one ESP32-S3-DevKitC-1-N16R8, both playing
2.0 (the S3 on 32-bit standard I2S), played one AC-3 2.0 programme from
`ac3hearth-testserver` as a group for ten minutes:
0 underruns on either board and a 479 us worst spread between their play
times, inside B3's 1 ms group criterion. An earlier ten-minute run with the
same two boards had one underrun on the S3 board alone, about two minutes
in, with no late chunk before or after it and no effect on the group's
alignment; a repeat of that run had none, and the C6 has never underrun in
any run in this section.

AC-3 and E-AC-3 5.1 do not fit this player's memory budget once the ring,
the WebSocket server and WiFi's own buffers are all resident, though both
decode in real time on the part with none of that overhead (see
[Real time, with WiFi and a stream](../../../../docs/platforms/bare-metal/esp32-c6.md#status)
on the platform page). Before this was a Kconfig setting, playing either
onto 5.1 aborted the board 10 to 12 seconds in: the decoder's own scratch
allocation failed, and by then the heap was short enough that even the C++
exception the failed allocation threw could not itself be allocated
(`__wrap___cxa_allocate_exception`), which aborts rather than closing the
stream in the ordinary way.

`_ac3forge_player@v1` sends the server's own programme at whatever channel
count it codes: `outputs.count` in the role's capability advertisement
bounds *routing*, the stage after decode, not what reaches the decoder, so
it does nothing to stop a wide syncframe from being sent and decoded in
the first place - `support().outputs.count` being `sink_slots()`, the I2S
wiring's own ceiling (up to 8 on this board), made that plainer still,
since a compliant server following it would see no reason not to offer
5.1. `CONFIG_AC3FORGE_EXAMPLE_SENDSPIN_MAX_CODED_CHANNELS` closes this at
the one place both codecs' frame headers already say the channel count
before any decoder-specific memory is touched: `BurstPlayer` reads
`FrameHeader::coded_channels()` and, past this count, refuses the
syncframe instead of opening a decoder for it, printing why once a stream
(not once a burst). `sdkconfig.sendspin-c6` sets it to 2: playing the same
AC-3 and E-AC-3 5.1 fixtures with the setting in place, the board stayed up
for the whole run, printed `refusing a 6-channel syncframe: more than the
2 this part decodes in this build` once, and reported every burst refused
through the ordinary invalid_chunks counter rather than going quiet mid-
play - the counter the test server's own "found invalid chunks" failure
reads. Only 2.0 and 5.1 were tried; a layout between them may or may not
fit, since the decoder's own scratch scales with its channel count and
5.1's alone is 36,864 bytes.

### On the ESP32-P4

The "best" tier of `planning/esp32-sink-tiers.md`: one I2S controller whose TDM frame
holds 512 bits, four times the S3/C6's 128, is meant to reach this product's full
sixteen-channel target alone (`sink/i2s_wide/audio_sink.cpp`) rather than needing a
second line. WiFi reaches a P4 board over `esp_hosted`/SDIO to an onboard ESP32-C6
co-processor - this part has no radio of its own - and needs nothing extra in
`SDKCONFIG_DEFAULTS`: `main/idf_component.yml` gates the two managed components
(`espressif/esp_wifi_remote`, `espressif/esp_hosted`) to `esp32p4`/`esp32h2` by target,
so they are resolved and linked automatically, unused everywhere else this example
already builds for.

Brought up on a DFRobot FireBeetle 2 ESP32-P4, pre-production silicon (chip revision
v1.3 - see [ESP32-P4](../../../../docs/platforms/bare-metal/esp32-p4.md) for that
board's own chip-revision and clock findings from the bare-metal probe). Three more
board-only boot crashes found bringing this example itself up, all fixed in
`sdkconfig.p4` and none hit by the smaller probe:

- **`CONFIG_PM_SLEEP_CLK_ICG_ENABLE=n`.** A Light Sleep feature this build never
  reaches (`CONFIG_PM_ENABLE` is off) still ran at boot and aborted reaching into a
  memory pool that, on this chip revision, overlaps the app's own `.data`/`.bss` - a
  bigger image leaves less of it free, which the tiny probe never came close to.
- **PSRAM on.** Without it, FreeRTOS could not create its own startup task under
  WiFi, lwIP, mbedtls and FAT's combined footprint - a crash before `app_main` ever
  ran. This board has 32 MB, unused by the minimum-footprint probe on purpose; this
  example has no such goal and needs the room.
- **`CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM=y`.** Even with PSRAM on, `esp_hosted`'s
  own SDIO transport buffers still defaulted to internal RAM; the component's own
  changelog names this option for exactly this chip.

With those three, a clean boot, WiFi join and a full paired Sendspin play, built with
`SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.hw;sdkconfig.p4;sdkconfig.sendspin"`
plus real `AC3FORGE_EXAMPLE_WIFI_SSID`/`_PASSWORD`: SDIO up, the ESP32-C6 identified, a
real access point joined and a DHCP lease taken, mDNS advertising `_sendspin._tcp`, the
REST control surface up on port 80 - all through this example's own code, not a
standalone test. `ac3hearth-testserver` then paired by token and played an Atmos/JOC
E-AC-3 fixture (`tests/golden/object-fixture/dee_joc_514.ec3`, acmod=7, 11 objects) five
times through over ten seconds onto the 2.0 layout: 315 of 315 bursts played, 0
underruns, 0 late, 0 dropped, 0 invalid, `/status` polled throughout with a worst
timing error of 419 µs, result `pass` - the decode, the burst player and the WiFi/
Sendspin path all exercised together for the first time on this part.

**TDM mode - any layout of three channels or more - could not be verified on this
exact board.** Only 1-2 channel standard I2S (this player's own default 2.0 layout)
opened; `7.1.4` and `9.1.6` both failed identically
(`sample rate is too large`/`could not start I2S in TDM mode`). The reason is a real
limit of this pre-production chip revision, not a bug: TDM always opens the sink's
full configured line width regardless of how many channels a layout actually needs
(the same rule this section's own S3/C6 sinks already follow), and this chip revision
has no PLL clock source for I2S at all - only a 40 MHz crystal or an audio PLL capped
at 125 MHz, both short of the roughly 147 MHz a 512-bit frame needs at 48 kHz. The
practical ceiling this leaves, on this exact silicon, is about 13 slots at 32-bit -
under the sink's full sixteen, so TDM stays unreachable here at any width past two
channels. A `>=v3.0` chip's 160 MHz PLL clears this with room to spare; the sink
design and its clock-source choice (`I2S_CLK_SRC_APLL`, itself needed - the default
source cannot open TDM at all on this revision) are otherwise unchanged and correct.

### Under QEMU

`sdkconfig.ci-sendspin`, over `sdkconfig.ci-http`, is the player on QEMU's
Ethernet with the capture sink, a 16 KB ring and no PSRAM.
`tools/checks/run_sendspin_qemu.sh` boots it with the Sendspin port and the
page forwarded to the host, and `ac3hearth-testserver` pairs with it by the
token on its console, gives it a 2.0 layout and plays the E-AC-3 JOC fixture
to it and to a test sink of its own, in one group. The board's RMS lines are
then held to the test sink's WAV file by `tools/checks/check_sendspin_levels.py`,
and its console to one clean boot and a heap floor. On 2026-09-16, paired
by its token and then, on a fresh board, by the code it printed, all 315
bursts played each time, with each output's RMS equal to the test sink's;
with the firmware the two boards above ran, at least 43,700 bytes of internal
heap were free.

[Joining a network](#joining-a-network) is checked the same way, by
`tools/checks/run_improv_qemu.sh`. It boots the same image with QEMU's link
down, so the board's own attempt gives up after 30 s and it listens for
Improv. `tools/checks/improv_qemu.py` then asks the board its state, gives it
a network, and brings the link up while the board waits for an address. The
board must answer with its page, `http://10.0.2.15/`, then advertise itself
and start the player, which the test server plays to as above. The network it
is given has a 13-byte name and the board is then told to take a 10-character
one, so that the length byte in front of each is a CR one way and an LF the
other: a console that converted line endings, as ESP-IDF's default does and
`sdkconfig.defaults` does not, corrupts exactly those packets. The script
also boots `sdkconfig.ci-wifi`, the player on WiFi with nothing stored and
nothing built in. That board must boot once, with its control surface up, and
answer Improv's state and device requests. It is never given a network: QEMU
has no radio, and `esp_wifi_start()` does not return there. Each run boots a
copy of its image, because QEMU writes the board's NVS into the file it runs.
On 2026-09-17 the first board held its address 1.8 s after the link came up,
all 315 bursts played with each output's RMS equal to the test sink's, and at
least 43,884 bytes of internal heap were free. Firmware from before
2026-09-16 fails both: the first board aborts in the join Improv asked for,
and the second restarts in a loop.

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
grammar is [`ac3/render/layout.hpp`](../../../../src/forge/include/ac3/render/layout.hpp)'s and
`PUT /layout` on the control surface takes the same text for the next play.

What happens to a stream depends on the layout, not the stream:

| Layout | How |
| --- | --- |
| `2.0`, `1.0` | The decoder's own §7.8 fold (`CONFIG_AC3FORGE_EXAMPLE_STEREO_FOLD` picks Lo/Ro or Lt/Rt). What every player before 2026-09-10 did, unchanged. |
| anything wider, no heights | As coded. Each coded channel goes to the slot of its own location exactly, or, where the room has no such speaker (a 7.1 stream's rears in a 5.1 room), is spread over its neighbours by `ac3::spatial::pan_direction` at constant power. The LFE goes to the LFE slots and nowhere else. |
| with heights | As above for a stream without objects. For a stream with an object layer the objects are reconstructed and placed by their own positions, the bed's LFE passes through (held back by the objects' reconstruction delay, so that it stays with them), and the bed's other channels are **not** added — an Atmos bed is the objects' own 5.1 fold, and adding it would play everything twice. `CONFIG_AC3FORGE_EXAMPLE_OBJECTS` widens or narrows when that happens. |

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
and [Folded to stereo](../../../../docs/platforms/bare-metal/esp32-s3.md#folded-to-stereo).

All of it is [`ac3/render/render.hpp`](../../../../src/forge/include/ac3/render/render.hpp),
one 256-sample block at a time, which is why a 7.1.4 layout costs the player 12 KB
of block storage rather than 72 KB of frame. The geometry is the library's
(`tests/spatial/`); what the header adds is indexing between coded channels,
objects and slots, tested on the host in `tests/render/test_layout.cpp` because a
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
between them as the layout in force changes, rather than a build fixing one
shape and staying there
([`ac3forge/sink_plan.hpp`](../../include/ac3forge/sink_plan.hpp) decides
which). `PUT /layout` takes effect this way at the very next play: no rebuild,
no reflash, just whatever the new layout needs. A TDM line always runs its full
frame, four 32-bit slots or eight 16-bit ones, with the slots past the layout's
channels written as zeros: a TDM DAC is set up for a fixed frame, and on an
ESP32-C6 the driver clocked three- and five-slot frames 6.7% fast at 16 bits.

**A fixed frame for a TDM DAC.** `CONFIG_AC3FORGE_EXAMPLE_I2S_FIXED_FRAME=1`
opens that full TDM frame for every layout, mono and stereo included
(`ac3forge::SinkFrame::fixed`). A TDM DAC set up for one frame shape needs it:
an ESS ES9080 has its slot count, slot width and channel map written over I2C,
and its PLL can lock to the bit clock, so a 2.0 play opened as standard I2S
would change the bit clock under it and put the samples in slots it does not
read. A mono layout rides slot 0 alone, as in any TDM frame. With a second
line, line 1 runs for every layout too, its slots zeroed while line 0 holds
the whole layout, so a second DAC on its data pin always reads defined
samples. Nothing is reconfigured between plays, since every layout gets the
same frame. The DAC's own I2C setup is not part of this example. Leave it at 0
for a stereo I2S DAC such as a PCM5102 or MAX98357A, which reads the two-slot
frame.

On an ESP32-C6 board with no DAC wired, playing `layout-20.ec3` from the FAT
partition onto `2.0` at 16 bits, the option changed the sink's line from
`line0 2 slots` to `line0 8 slots (tdm, fixed frame)`. Both channels' levels
were unchanged to the digit, and a frame took 34,581 microseconds against
34,583 without it: 1.08 times real time either way, so both runs had the
same 127 underruns. The sink took 81 microseconds a frame
longer, zeroing six more slots and handing the driver four times the bytes.
The DMA buffers grow: the same depth of eight 16-bit slots is 16 KB where
the stereo pair's is 4 KB, and the heap had 12,304 bytes less free during the
play. A `1.0` layout at 32 bits opened `line0 4 slots (tdm, fixed frame)`
with its one channel in slot 0.

**The hardware ceiling this cannot get past.** On an ESP32-S3 one I2S line's
TDM frame holds at most 128 bits, because the peripheral's half-frame length
is a 6-bit register field: four slots at 32 bits - a 6.1 MHz bit clock at 48
kHz - or eight at 16, and ESP-IDF v6.1 refuses more, as does this sink before
it ever asks the driver. `CONFIG_AC3FORGE_EXAMPLE_I2S_SLOT_BITS` chooses
between them: 32 by default, carrying 24-bit samples in up to four slots, or
16, carrying 16-bit samples in up to eight (`ac3forge::interleave_16in16`), so
a 7.1 layout fits one line at 16 bits and not at 32.
`CONFIG_AC3FORGE_EXAMPLE_I2S_SECOND_LINE` brings up a second,
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

**Reconfiguring, not rebuilding.** A change that stays within standard mode
(mono to stereo at 32 bits) uses `i2s_channel_reconfig_std_slot` rather than
tearing the channel down, and a change between TDM layouts reconfigures
nothing, since the frame stays full width; crossing standard/TDM, or a line
coming up or going down entirely, does tear it down and recreate it. Every channel this sink ever creates asks for
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
(`sdkconfig.ci-render`, `sdkconfig.ci-http714`), and sixteen 16-bit ones onto
a 9.1.6 layout (`sdkconfig.ci-tdm916`), which is the widest frame the part
reaches - two lines of eight - and so checks the planner's split and the
16-bit interleave across a full sixteen slots. The build
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

**How a sample becomes a slot depends on the part.** `ac3forge::to_pcm16` and
`to_slot_24in32` scale, clip and truncate a sample in `float`: a handful of
instructions on a part with a floating-point unit, such as the ESP32-S3. The
ESP32-C6 has none, and each of those four operations is a call into the
software floating-point routines. `to_pcm16_from_bits` and
`to_slot_24in32_from_bits` compute the same integers from the sample's
IEEE-754 bits with 32-bit integer arithmetic instead - equal to the float
forms for every input that is not a NaN, checked exhaustively on the host and
against a real decoded stream under QEMU (S3 in float, C3 in bits, identical
converted slots). `ac3forge/interleave.hpp`'s interleaves take the conversion
as a template argument and the component chooses it from
`CONFIG_SOC_CPU_HAS_FPU`, so a sink's own code is unchanged either way.

Measured on an ESP32-C6 at 160 MHz, one frame of six 256-sample blocks,
`-Os`:

| Conversion | In float | From the bits |
|---|---:|---:|
| Eight 16-bit slots (`interleave_16in16`) | 12,196 us | 4,541 us |
| Four 24-in-32 slots (`interleave_24in32`) | 5,159 us | 2,092 us |
| A stereo pair (`interleave_16`) | 2,879 us | 770 us |

And on the same board, this example's own `i2s` sink playing a 7.1 stream
onto eight 16-bit TDM slots (`sink_us_per_frame`, which also carries the
level meter in front of the sink): 20,875 us in float, 12,689 us from the
bits - `-Os` still calls the conversion once a sample rather than inlining
it, which building the sink's source at `-O2`
(`AC3FORGE_MINIMAL_HOT_O2`'s reasoning, applied to this file) brings to
11,551. Levels are unchanged to the digit across all three. The ESP32-S3's
own sink compiles to identical object code before and after - confirmed on a
board, no difference outside measurement jitter.

## What it costs

`idf.py size`, IDF v6.1, `-Os`, the default shape (`partition` to `i2s`, `2.0`)
with the console on USB-Serial-JTAG, with the player on the block form. The
decoder's hot sources at `-O2` move flash, not these figures:

| | Bytes |
| --- | --- |
| Internal SRAM (DIRAM) used by the image | 88,563 |
| …of which `.bss` | 46,232 |
| …leaving for the heap, by the linker's estimate | 253,197 |
| Taken from that heap when the player starts: one block for each of the layout's slots (two for `2.0`, 2,048 bytes; twelve for `7.1.4`, 12,288), the framing buffer, the staging block and the renderer's gain tables | 23,040 |
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
