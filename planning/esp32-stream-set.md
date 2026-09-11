# The ESP32 player's stream set

!!! note "Status as of 2026-09-11: built; played under QEMU in CI's twelve-slot shape, and on a board"
    Streams for the player's `http` source, in one directory a device can be pointed at: 7.1.4
    streams that decode to all twelve slots of a 7.1.4 output, and beside them a range across the
    layouts the player renders onto, both codecs, dependent substreams, two programmes, dual
    mono, the Annex E coding tools, short frames, VBR, DRC metadata, other encoders' streams and
    object audio. A manifest says what each stream is and the level each slot of a 7.1.4 output
    should get from it. CI plays the set under QEMU onto 7.1.4 and holds every slot to its level.

    Playing the set found four things in the player ([What the set found](#what-the-set-found)):
    a stream carrying two programmes had both played, a frame of each, and now plays its first;
    a stream at 44.1 or 32 kHz played at the wrong speed, and is now refused; a stream using
    transient pre-noise processing loses its last access unit; and folding a stream with a
    four-channel dependent substream to 2.0 does not fit a network shape's internal RAM without
    PSRAM. The last two are the decoder core's, handed over in the player plan.

    On a board with PSRAM ([On a board](#on-a-board)) every stream in the set played with the
    host's levels, and the wide folds fit. What the board found is time: over WiFi a 7.1.4
    stream folded to 2.0 takes longer to decode than it lasts, and so does a 7.1.4 stream using
    AHT or enhanced coupling.

    Written beside [the web UI's output layout](esp32-device-ui.md#the-output-layout), which
    uses these streams to show what a layout does, and in the shape of [the player
    plan](esp32-player.md): what exists, what changes and why, what was measured, what cannot be
    verified, and [Decisions](#decisions) with a recommendation and a cost each.

## Why

The player's `http` source has been given one stream, in CI and on the board: the WASM page's
demo, E-AC-3 5.1 with objects, played onto two slots every time. What reaches a 7.1.4 output
from a 7.1.4 stream over the network had not been played; nor a 5.1 stream on 7.1.4, which shows
the heights left silent; nor most of Annex E through the player rather than through the
footprint probe. The page's report of a layout (the web UI's design) needs streams whose
reports differ, and a board session needs something to point a device at.

## What was in the tree

| Streams | Where | What they carry, checked with `ac3cli probe` |
|---|---|---|
| The WASM page's demo | `apps/wasm/assets/demo.ec3` | E-AC-3 5.1, JOC objects in the QMF domain, 8 s. What CI's HTTP step serves. |
| The example's own | `esp-idf/ac3forge/examples/stream_player/stream/` | `sample.ac3` (AC-3 5.1, six frames), flashed to a partition; `height.ec3` (the probe's height fixture: five objects over a 5.1 bed, three on the ceiling, MDCT-band domain, six access units), which `sdkconfig.ci-render` plays from FAT onto 7.1.4 |
| The fuzz seeds | `fuzz/seeds/fuzz_eac3_decode/` | One-second streams from `fuzz/generate-seeds.sh`: a tone per speaker at every layout the encoder names, the Annex E tool combinations at 5.1 and 7.1.4, objects, two external streams |
| The external baseline | `tests/golden/external-baseline/` | Dolby Encoding Engine and FFmpeg streams: AC-3 and E-AC-3, stereo and 5.1, music and speech |
| A licensed encoder's objects | `tests/golden/object-fixture/dee_joc_514.ec3` | 5.1.4 carried as JOC objects, QMF domain |

Two things the checking found. The seeds named `eac3-encode-*-714` are encoded from a 5.1
source, so four of their twelve channels - three heights and the LFE - are silent: they serve
the fuzzers and are not used here. And the seeds with a tone per speaker code a dependent
substream's four channels at a rate low enough to move those channels' levels by up to 15%;
the levels below are what the decoder makes of them, so a check is not affected, and the new
7.1.4 streams are coded at a rate that leaves every speaker at its level.

## The set

`esp-idf/ac3forge/examples/stream_player/www/`, served as it is. **No PSRAM** is whether CI's
twelve-slot network shape, which has none, plays the stream to its end; see [What the network
shape holds at 7.1.4](#what-the-network-shape-holds-at-714).

| File | What it is | Codec, channels | Substreams | Coding tools | Blocks | kbit/s | Length | Size | No PSRAM |
|---|---|---|---|---|---|---|---|---|---|
| `714-walk.ec3` | 7.1.4, one speaker at a time in slot order, 0.4 s each | E-AC-3, 12: L C R Ls Rs Lrs Rrs Vhl Vhr Lts Rts LFE | 3 | spx | 6 | 384 | 4.8 s | 225 KB | yes |
| `714-tones.ec3` | 7.1.4, every speaker at once, each its own tone | E-AC-3, 12 as above | 3 | spx, blksw | 6 | 384 | 2.0 s | 94 KB | yes |
| `objects-mdct.ec3` | Four objects orbiting at different heights over a 5.1 bed, JOC in the MDCT-band domain | E-AC-3, 6: L C R Ls Rs LFE, objects | 1 | - | 6 | 384 | 4.0 s | 188 KB | as bed |
| `demo.ec3` | The WASM page's demo: 5.1 with objects, JOC in the QMF domain | E-AC-3, 6, objects | 1 | - | 6 | 448 | 8.0 s | 438 KB | as bed |
| `514-joc-dee.ec3` | Dolby Encoding Engine: 5.1.4 carried as objects, QMF domain | E-AC-3, 6, objects | 1 | cpl, blksw | 6 | 448 | 2.0 s | 110 KB | as bed |
| `height.ec3` | The probe's height fixture: five objects, three on the ceiling, MDCT-band domain | E-AC-3, 6, objects | 1 | - | 6 | 448 | 0.2 s | 10 KB | as bed |
| `layout-10.ec3` | A tone per speaker | E-AC-3, 1: C | 1 | - | 6 | 192 | 1.0 s | 24 KB | yes |
| `layout-20.ec3` | The same | E-AC-3, 2: L R | 1 | - | 6 | 192 | 1.0 s | 24 KB | yes |
| `layout-51.ec3` | The same | E-AC-3, 6: L C R Ls Rs LFE | 1 | - | 6 | 192 | 1.0 s | 24 KB | yes |
| `layout-71.ec3` | The same: 5.1 and a dependent substream | E-AC-3, 8: L C R Ls Rs Lrs Rrs LFE | 2 | - | 6 | 288 | 1.0 s | 36 KB | yes |
| `layout-512.ec3` | The same: 5.1 and a dependent substream | E-AC-3, 8: L C R Ls Rs Vhl Vhr LFE | 2 | - | 6 | 288 | 1.0 s | 36 KB | yes |
| `layout-514.ec3` | The same: 5.1 and a dependent substream | E-AC-3, 10: L C R Ls Rs Vhl Vhr Lts Rts LFE | 2 | - | 6 | 288 | 1.0 s | 36 KB | yes |
| `layout-714.ec3` | The same: 5.1 and two dependent substreams | E-AC-3, 12 | 3 | - | 6 | 384 | 1.0 s | 48 KB | yes |
| `714-none.ec3` | 7.1.4, no coding tools | E-AC-3, 12 | 3 | blksw | 6 | 512 | 0.5 s | 32 KB | yes |
| `714-cpl.ec3` | 7.1.4, coupling | E-AC-3, 12 | 3 | cpl, blksw | 6 | 512 | 0.5 s | 32 KB | yes |
| `714-ecpl.ec3` | 7.1.4, enhanced coupling | E-AC-3, 12 | 3 | cpl, ecpl, blksw | 6 | 512 | 0.5 s | 32 KB | no |
| `714-spx.ec3` | 7.1.4, spectral extension | E-AC-3, 12 | 3 | spx, blksw | 6 | 512 | 0.5 s | 32 KB | yes |
| `714-aht.ec3` | 7.1.4, adaptive hybrid transform | E-AC-3, 12 | 3 | aht, blksw | 6 | 512 | 0.5 s | 32 KB | no |
| `714-tpn.ec3` | 7.1.4, transient pre-noise processing | E-AC-3, 12 | 3 | tpn, blksw | 6 | 512 | 0.5 s | 32 KB | no |
| `714-all.ec3` | 7.1.4, coupling, spectral extension and AHT together | E-AC-3, 12 | 3 | cpl, spx, aht, blksw | 6 | 512 | 0.5 s | 32 KB | no |
| `714-blocks2.ec3` | 7.1.4, two-block syncframes | E-AC-3, 12 | 3 | cpl, blksw | 2 | 768 | 0.5 s | 47 KB | yes |
| `714-blocks3.ec3` | 7.1.4, three-block syncframes | E-AC-3, 12 | 3 | cpl, blksw | 3 | 768 | 0.5 s | 48 KB | yes |
| `51-aht.ec3` | 5.1, adaptive hybrid transform | E-AC-3, 6 | 1 | aht, blksw | 6 | 384 | 0.5 s | 24 KB | yes |
| `51-ecpl.ec3` | 5.1, enhanced coupling | E-AC-3, 6 | 1 | cpl, ecpl, blksw | 6 | 384 | 0.5 s | 24 KB | yes |
| `51-tpn.ec3` | 5.1, transient pre-noise processing | E-AC-3, 6 | 1 | tpn, blksw | 6 | 384 | 0.5 s | 24 KB | yes |
| `51-blocks1.ec3` | 5.1, one-block syncframes | E-AC-3, 6 | 1 | cpl, blksw | 1 | 447 | 0.5 s | 27 KB | yes |
| `51-vbr.ec3` | 5.1, variable bit rate | E-AC-3, 6 | 1 | spx, blksw | 6 | 640 | 0.5 s | 40 KB | yes |
| `51-drc.ec3` | 5.1 with DRC words (film standard) and dialnorm -24 | E-AC-3, 6 | 1 | spx, blksw | 6 | 384 | 0.5 s | 24 KB | yes |
| `ac3-51.ac3` | AC-3 5.1 | AC-3, 6 | 1 | blksw | 6 | 448 | 0.5 s | 28 KB | yes |
| `ac3-51-cpl.ac3` | AC-3 5.1 with coupling, a tone per speaker | AC-3, 6 | 1 | cpl | 6 | 448 | 1.0 s | 56 KB | yes |
| `ac3-51-44k.ac3` | AC-3 5.1 at 44.1 kHz, which the player refuses: its sink runs at 48 kHz | AC-3, 6 | 1 | blksw | 6 | 448 | 0.5 s | 29 KB | refused |
| `ac3-20.ac3` | AC-3 2/0 | AC-3, 2: L R | 1 | blksw, remat | 6 | 192 | 0.5 s | 12 KB | yes |
| `eac3-dualmono.ec3` | E-AC-3 1+1: two mono programmes in one substream | E-AC-3, 2: Ch1 Ch2 | 1 | blksw | 6 | 192 | 1.0 s | 24 KB | yes |
| `eac3-programmes.ec3` | E-AC-3 with two programmes: 5.1, and a mono second programme | E-AC-3, 6, and 1 | 1 per unit, 2 programmes | blksw | 6 | 240 | 1.0 s each | 60 KB | yes |
| `dee-eac3-51.ec3` | Dolby Encoding Engine, E-AC-3 5.1 at 256 kbit/s | E-AC-3, 6 | 1 | cpl, spx, aht, blksw | 6 | 256 | 2.5 s | 79 KB | no |
| `ffmpeg-eac3-51.ec3` | FFmpeg, E-AC-3 5.1 at 256 kbit/s | E-AC-3, 6 | 1 | cpl | 6 | 256 | 2.5 s | 79 KB | yes |
| `dee-ac3-51.ac3` | Dolby Encoding Engine, AC-3 5.1 at 448 kbit/s | AC-3, 6 | 1 | cpl, blksw | 6 | 448 | 2.5 s | 138 KB | yes |
| `dee-eac3-music.ec3` | Dolby Encoding Engine, E-AC-3 2/0 music at 96 kbit/s | E-AC-3, 2 | 1 | cpl, spx, aht, remat | 6 | 96 | 30.0 s | 352 KB | yes |

Every stream but `ac3-51-44k.ac3` is at 48 kHz; that one is in the set to be refused.

## How the streams are made

`tools/generators/gen_device_streams.py --ac3cli <a host ac3cli>` writes the directory and its
manifest, `streams.json`. It synthesises four signals and encodes them with this repository's
encoder:

- a tone per speaker, a third of an octave apart (L 250 Hz up to Rts 2,500 Hz, the LFE at
  63 Hz), all at once: `714-tones.ec3`;
- the same tones one speaker at a time, 0.4 s each, in slot order: `714-walk.ec3`, the one to
  listen to on a 7.1.4 room;
- for the coding tools, each speaker's tone with a second tone above where coupling and spectral
  extension start, a little noise across the band, and a click every quarter second so that
  block switching has transients to switch on: the `714-*` and `51-*` streams and the AC-3 ones;
- two mono tones, for dual mono and the second programme;
- the 5.1 tones at 44.1 kHz, for the stream the player refuses.

The objects stream is `ac3cli atmos` - four objects orbiting at different heights - with
`joc-domain=mdct`. The rest are copies of the streams in the table above, left as they are.

For each stream, `streams.json` has what `ac3cli probe` reports - codec, coded channels by
location, substreams, programmes, objects, blocks per syncframe, the coding tools the stream
uses, bit rate, length - and two things a play is checked against: the access units a player of
its first programme decodes, and `levels_714`, what each slot of a 7.1.4 output should get, as
RMS x 1e6, the form the player prints. The levels are the host decoder's: the stream decoded as
coded (no dialnorm normalisation and no DRC, the player's `CONFIG_AC3FORGE_EXAMPLE_DRC_MODE=2`),
objects decoded bed-only, and each channel's level placed on the slot of its own location. A 0
is a slot that nothing in the stream is at, and the player must leave it at exactly 0.

The 24 streams made are 1.2 MB; the directory is 2.7 MB with the fourteen copies, which cost the
repository nothing, since git keeps one copy of identical content whatever its path.

## Where the set lives, and how it is served

Beside the example that plays it. Any static HTTP server will do:

```bash
python3 -m http.server 8000 --bind 0.0.0.0 --directory esp-idf/ac3forge/examples/stream_player/www
```

and a device plays `http://<host>:8000/714-walk.ec3` - `10.0.2.2` from QEMU's user-mode network,
the serving machine's LAN address from a board. The packing script leaves the directory out of
the component's registry archive; it is the repository's, not the component's.

## What CI plays

A new shape, `sdkconfig.ci-http714`: `sdkconfig.ci-http`'s source, network and control surface,
with the capture sink's twelve-slot TDM conversion, the layout `7.1.4`, levels as coded
(`CONFIG_AC3FORGE_EXAMPLE_DRC_MODE=2`) and objects played as their bed
(`CONFIG_AC3FORGE_EXAMPLE_OBJECTS=1`, [decision 4](#decisions)), booting on `714-walk.ec3`. A
step in the ESP32 job, "Play the stream set onto 7.1.4 over QEMU's Ethernet", after "Drive the
web page on the emulated board":

1. builds the shape, serves the set on port 8000 and boots it under QEMU with the control
   surface forwarded, as the HTTP step does;
2. plays every stream not marked PSRAM-only through `POST /play`, one at a time, and
   `tools/checks/check_stream_set.py` holds each play to the manifest: `result=pass`, no bytes
   skipped for sync, the stream's access units (played and held together, see below), and every
   slot's level within 1% + 20 of the host's, a slot at 0 at exactly 0 - and, from
   `GET /status`, the play's `stream.layout`, `render`, `coded` and `silent` against what the
   manifest's levels imply;
3. drives the web page on this twelve-slot board (`device-ui/board/layouts.spec.js`): the Output,
   Silent and Next play rows through a 5.1 stream on 7.1.4, a 7.1.4 stream on 5.1, a 5.1 stream
   folded to 2.0, a layout wider than the bus, and the 44.1 kHz stream refused;
4. checks the console for a panic or a second boot (`tools/checks/check_esp_console.py`).

The tolerance is tighter than the 5% + 200 the other ESP32 steps allow, on the evidence of the
measurement below: over 34 plays the largest difference between a slot's level on the emulated
target (float32) and the host's (float64) was one unit of RMS x 1e6. One percent still catches
every fault a level can show - a channel in the wrong slot, a silent one, a doubled programme,
a fold where there should be none - with a hundredfold margin over the arithmetic.

Cost: one more build of the example in the ESP32 job, a few minutes on the fleet's runners, and
about two minutes of plays and page. `tools/checks/test_check_stream_set.py` tests the
checker's parsing and rules on the host, in the Oracle unit tests step.

## What the network shape holds at 7.1.4

Measured under QEMU on 2026-09-11 in the shape above, with objects played as their bed except
where the row says otherwise: 288 KB of internal heap free as each play started, the largest
block 200 KB, no PSRAM.

| What | Result |
|---|---|
| Every channel-based layout to 7.1.4, AC-3, no tools, coupling, spectral extension, AHT, enhanced coupling and TPN at 5.1, short frames, VBR, DRC words, dual mono, FFmpeg's stream, the Dolby Encoding Engine's AC-3 and its stereo music | Played to the end. Every slot's level the host's to within one unit of RMS x 1e6; every slot the manifest has at 0 exactly 0. The decode task's stack had 7.9 to 9.1 KB of its 24 KB left. |
| Objects as their bed: `demo.ec3`, `objects-mdct.ec3`, `height.ec3`, `514-joc-dee.ec3` | Played; the bed's levels the host's |
| Objects reconstructed and placed (the default policy) | `abort()` for `height.ec3`, the orbiting-objects fuzz seed and `514-joc-dee.ec3`: a request of 6,144 to 22,528 bytes failed with no free block that large. `demo.ec3` placed its objects and left 1,872 bytes of the 24 KB decode stack. |
| AHT at 7.1.4 (`714-aht`, `714-all`) | `abort()`: 43,008 bytes asked for, 24,316 free, largest block 13,312. That is the decoder's per-frame AHT buffer for one substream: 256 bins, six blocks, seven channels, four bytes. |
| The Dolby Encoding Engine's 5.1 (`dee-eac3-51`: coupling, spectral extension and AHT) | `abort()`: the same 43,008 bytes, with 86,036 free but no block larger than 39,936. The 5.1 AHT stream this set makes, with no coupling channel, asks for 36,864 and plays. |
| Enhanced coupling and TPN at 7.1.4 | `abort()`: 6,144 bytes, 8,212 and 8,140 free, largest blocks 4,480 and 1,792 |

**At 2.0 the fold is what does not fit.** In the two-slot HTTP shape, the same streams played
to `2.0`: 5.1 and 5.1.2 fold, and 7.1, 5.1.4 and 7.1.4 abort - `OutputStage::apply`, the
fold's scratch, asks for 6,144 bytes with 7,564 to 8,844 free and no block larger than 5,632.
The twelve-slot shape does the same for 7.1.4 at `2.0` (3,212 free). Played as coded onto twelve
slots those streams leave 147 KB free, so it is folding a programme with a four-channel
dependent substream that costs the memory. The page's twelve-slot test folds a 5.1 stream for
that reason.

The allocations that failed are the decoder's, which are the same whatever the output layout:
the player's block storage is sixteen slots at every layout, and the capture sink converts one
block at a time. A board with PSRAM puts allocations of 16 KB and over there
(`sdkconfig.psram`), which is where the streams marked "no" are meant to play. Each 1-second
play took about two seconds of wall clock under QEMU.

## What the set found

**A stream with two programmes had both played, a frame of each.** `eac3-programmes.ec3` carries
a 5.1 programme and a mono one as two independent substreams. The player handed every access unit
to the decoder, whose `DecoderConfig::programme` it leaves unset, and the decoder renders
whichever programme a unit belongs to: the play decoded 64 access units and reported 2,048 ms of
audio for a 1,024 ms programme, with the mono programme's tone in the centre slot and every other
slot at 0.71 of the host's level. `result=pass` does not show it; the levels do. `ac3cli`
decodes the first programme a stream carries, and so should the player ([decision
5](#decisions)).

**A stream using transient pre-noise processing loses its last access unit.** §3.7's tool holds
each frame back one call, and at the end of the stream the last one is still held: `51-tpn.ec3`
played 15 of its 16 access units, with `stream.held=1` and 480 ms of its 512. `Eac3Decoder::flush()`
releases it, but as raw per-substream results rather than through the block form the player uses,
and adding a flush to the block form is the decoder core's to do. So the set's checker counts a
play's units played and held together, and the player plan records the hand-over.

**Nothing checked a stream's sample rate against the sink's.** The example opens its sink at
48 kHz once, and no part of the player compared a stream's rate with it, so a 44.1 kHz AC-3
stream played 9% fast and its pitch a semitone and a half high. The player now refuses one
([decision 6](#decisions)), and `ac3-51-44k.ac3` is in the set as the stream it refuses.

**Folding a wide programme to 2.0 does not fit without PSRAM**, as [the network shape's
figures](#what-the-network-shape-holds-at-714) show: 7.1, 5.1.4 and 7.1.4 abort in the fold's
scratch. A board with PSRAM folds them ([On a board](#on-a-board)), but cannot fold 7.1.4 in
real time. Both are the decoder core's to change ([decision 8](#decisions)), and the player plan
records the hand-over.

**Dual mono reported no channels.** `stream.channels` for E-AC-3 1+1 was the decoder's layout
count, which is 0 because 1+1 has no Table E2.5 layout; the player now reports 2.

## On a board

Played on 2026-09-11 on the second ESP32-S3-DevKitC-1 (N16R8, 240 MHz, no DAC wired: the I2S
peripheral clocks out whether or not anything listens, so its pacing and its counters are real),
from this directory served over the LAN, in the board's network shape
(`sdkconfig.defaults;sdkconfig.hw;sdkconfig.psram` with the source, its URL and the network's
credentials). Two images of this branch:

- **2.0 on the I2S sink**, the shape the board already ran: fifteen streams, 5.1 to 7.1.4, the
  Annex E tools at 5.1, AC-3, dual mono, two programmes and the 44.1 kHz stream.
- **7.1.4 on the null sink**, levels as coded and objects as their bed, as CI's shape has them:
  every stream in the set, one `POST /play` at a time. The null sink paces nothing, so a frame's
  time is the decode and the render.

**Every stream played, and the levels are the host's.** At 2.0 each fold's two levels are the
host's `ac3cli decode ... downmix=loro drcmode=line` to the digit, the four wide streams
included. At 7.1.4, 34 of 38 plays have every slot the host's to the digit, two are one unit
out, and the two TPN streams differ by the access unit the block form holds back. The five
streams marked "no" played at 7.1.4 with the rest. Each play started with 169 KB of internal RAM
free, the largest block 90 to 94 KB, and 8.3 MB of PSRAM; nothing failed to allocate, and the
board did not restart. `GET /status` reported each play as the page shows it: `loro` at 2.0,
`channels` at 7.1.4, the coded channels by location, and the speakers left silent - ten of
twelve for the Dolby Encoding Engine's stereo music at 7.1.4.

**The folds fit; the time does not.** Per 32 ms frame, where allocations of 16 KB and over are
in PSRAM:

| Stream | 2.0: decode with the fold | Blocks to an empty queue at 2.0 | 7.1.4: decode | 7.1.4: render |
|---|---|---|---|---|
| `layout-51` (5.1) | 13.8 ms | 0 of 192 | 9.3 ms | 1.4 ms |
| `layout-512` (5.1.2) | 20.6 ms | 0 of 192 | 14.5 ms | 1.5 ms |
| `layout-71` (7.1) | 23.5 ms | 4 of 192 | 16.2 ms | 1.5 ms |
| `layout-514` (5.1.4) | 23.7 ms | 3 of 192 | 16.1 ms | 1.7 ms |
| `714-walk` (7.1.4) | 36.0 ms | 149 of 900 | 27.4 ms | 2.2 ms |
| `714-tones` (7.1.4) | 35.6 ms | 62 of 378 | 27.1 ms | 2.2 ms |

At 2.0 the decode includes the decoder's output stage - the fold, with the example's line-mode
DRC and dialnorm - which the 7.1.4 image, playing levels as coded, does not run. The two images'
decode differs by 4.5 ms of the frame for 5.1 and 8.6 ms for 7.1.4, where the renderer places
the same channels on twelve slots in 1.4 to 2.2 ms. Nothing has profiled that difference: the
fold's arithmetic is already float in this build (`decode_scalar_t`), the decoder times its
output stage as the `eac3_output` zone, and the probe, whose stage timers report zones like it
(`AC3FORGE_STAGE_TIMERS`), folds 5.1 (`eac3_fold`) but no 7.1.4 stream. So a 7.1.4 stream at
2.0 falls behind: `714-walk` put 149 of its 900 blocks
into an empty queue, 940 ms of silence in 4.8 s. Behind, the decode task never waits on the
sink, so core 1's idle task missed the five-second task watchdog, which printed a backtrace in
`Eac3Decoder::decode_substream_core` and let the play go on (`CONFIG_ESP_TASK_WDT_PANIC` is
off). 7.1 and 5.1.4 keep up, a few blocks short at the start of a one-second play.

Onto twelve slots a 7.1.4 stream with no coding tools decodes and renders in 26.7 to 29.6 ms,
inside the frame before any sink converts a sample. The coding tools take it to the edge of the
frame and past it:

| 7.1.4 with | Decode and render | Of the frame |
|---|---|---|
| no tools (`714-none`) | 26.7 ms | 0.83 |
| TPN | 30.4 ms | 0.95 |
| coupling | 30.7 ms | 0.96 |
| spectral extension | 31.1 ms | 0.97 |
| AHT | 35.2 ms | 1.10 |
| all of them | 39.1 ms | 1.22 |
| enhanced coupling | 59.2 ms | 1.85 |

A 7.1.4 output also needs a TDM DAC, which this board does not have, and a sink that converts
twelve slots in what is left of the frame. One S3 I2S line cannot carry them: a TDM frame holds
at most 128 bits, four 32-bit slots, so twelve need both I2S controllers at 16 bits or a TDM
device fed by several lines ([7.1.4 in real time](esp32-714-realtime.md#twelve-slots-on-this-part)). The network shape's internal RAM is not enough for a
twelve-slot DMA queue as the shapes stand: at twelve 32-bit slots the queue takes 2,304 bytes of
internal RAM per millisecond - about 46 KB at the example's default depth and about 147 KB at
`sdkconfig.psram`'s 64 ms - where the network shape keeps 14 to 16 KB free while it plays
stereo. That is a calculation, not a measurement. A local source (FAT, SD, a partition) leaves
out WiFi's share of internal RAM, and is where a board would first play 7.1.4 into a DAC.

## What cannot be verified

- **Object placement over the network.** CI's shape and the board's 7.1.4 image play objects
  as their bed; `sdkconfig.ci-render` places objects from FAT. Under QEMU `demo.ec3` placed its
  objects with 1,872 bytes of the 24 KB decode stack left; a board's network shape gives the
  task 32 KB, and has not placed objects over the network.
- **7.1.4 into a TDM DAC.** There is none here, QEMU has no I2S, and one S3 I2S line cannot
  carry twelve 32-bit slots. The conversion of twelve slots has since been timed on the board
  ([7.1.4 in real time](esp32-714-realtime.md#the-frame-on-the-board)); a twelve-slot DMA
  queue has not.
- **The streams marked "no", in CI.** They played on the board on 2026-09-11, and CI's shape
  has no PSRAM, so a change that breaks them shows only on a board.

## Decisions

1. **Where the set lives.** (a) **`www/` beside the example, left out of the registry archive**;
   (b) `apps/wasm/assets/`, which CI's HTTP step already serves; (c) served from where the streams
   are now. **Recommend (a).** (b) puts some forty device fixtures in the WASM page's asset
   directory, beside the one file that page loads. (c) ties the device's checks to fuzz seeds that
   `fuzz/generate-seeds.sh` rewrites and to fixtures kept for other checks. Cost: 1.2 MB of new
   files in the repository, and an entry in `pack_esp_component.py`.

2. **How the streams are made.** (a) **a generator in `tools/generators`, its output committed**;
   (b) made in CI. **Recommend (a).** The ESP32 job's container has no host `ac3cli`, and
   building one there would cost minutes on every run. Cost: the set is regenerated by hand when
   the decoder's output moves; until it is, CI fails, because it holds the target to the host's
   levels recorded in the manifest - which is the check doing its job.

3. **What a play is held to.** (a) **the host decoder's levels, as coded, placed by location and
   recorded in the manifest**; (b) the target's own first run, recorded. **Recommend (a)**, which
   is independent of the thing it checks. Cost: every stream in the set has to place exactly on
   7.1.4, so none has a channel 7.1.4 lacks (Lw, Rw, Vhc, Ts, Cs, LFE2); those are spread by the
   renderer, which `tests/io/test_layout.cpp` covers on the host.

4. **Objects in CI's network shape.** (a) **played as their bed**; (b) placed, with the object
   streams left out of the plays; (c) placed, with room made - a smaller ring, a larger decode
   stack, smaller network buffers. **Recommend (a).** Placement was measured not to fit, and
   `sdkconfig.ci-render` keeps placing objects from FAT. Cost: no object placement over the
   network in CI.

5. **Two programmes.** (a) **the player plays the first programme a stream carries, and skips
   the others' access units before decoding them**, by the substream id in the header it already
   reads; (b) `DecoderConfig::programme` set to 0, which the decoder answers with an empty result
   the player would count as held; (c) left as it is. **Recommend (a).** (b) would count every
   unit of another programme as a held frame. Cost: a check of one header field per access unit,
   and no way to pick a second programme, which a later setting could add.

6. **A stream whose sample rate is not the sink's.** (a) **the player refuses it: the play fails
   with the reason `sample rate` and the stream's rate in `error`**; (b) the sink re-clocked for
   each play; (c) left as it is. **Recommend (a).** (b) needs every sink to reopen at another
   rate, which the I2S and TDM sinks do not do today. Cost: a stream at 44.1 or 32 kHz, which a
   DAC could play at its own rate, does not play at all until (b).

7. **The coding tools 7.1.4 cannot carry without PSRAM.** (a) **in the set at 7.1.4, marked
   PSRAM-only, and at 5.1, which CI plays**; (b) left out of the set. **Recommend (a).** A board
   with PSRAM plays the 7.1.4 ones, as one did on 2026-09-11, and CI still decodes AHT, enhanced
   coupling and TPN through the player. Cost: 72 KB for the 5.1 streams.

8. **A wide programme folded to 2.0.** (a) **record it, and hand it to the decoder core**, with
   CI's folds at 2.0 made of 5.1 streams; (b) the player places a wide stream at `2.0` with its own
   renderer, panning each channel onto the pair, instead of the decoder's fold; (c) the player
   refuses a stream wider than 5.1.2 at `2.0`. **Recommend (a).** (b) would give two downmixes
   for one layout, §7.8's for some streams and a panner's for others, and (c) takes away a
   layout that works on a board with room. Cost: until the decoder changes, a 7.1, 5.1.4 or 7.1.4
   stream at `2.0` aborts a player without the internal RAM for the fold - QEMU's network shape
   is one, the board's is not - and a 7.1.4 stream at `2.0` falls behind real time over WiFi.
