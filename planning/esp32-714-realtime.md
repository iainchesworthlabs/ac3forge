# 7.1.4 in real time on the ESP32-S3 player

**Status, 2026-09-11:** profiled on a board. The user took the decisions the same day: the
component's side (decisions 1, 2, 6, 9 and 11) to be built, F and G to be proposed to the decoder
core's owner now, and the probe row (decision 10) left to the session profiling the fold. Decision
1 is being built as (c) rather than (a), because of that session's PR #654 (see decision 1). The
component's side is under way; nothing in `src/forge` has changed.

The player decodes a 7.1.4 E-AC-3 stream on the ESP32-S3 with every slot at the host decoder's
level, and over WiFi it does so too slowly. Folded to 2.0, a frame of `714-walk.ec3` took 36 ms of
its 32, and 149 of the stream's 900 blocks reached an empty DMA queue ([the stream set on a
board](esp32-stream-set.md#on-a-board)). This page records where that frame's time goes, stage by
stage, measured on the board. It then lists the options with what each is expected to save, and
the decisions, each with a recommendation and a cost.

## What was asked

- 7.1.4 E-AC-3 in real time on the streaming player: folded to 2.0, onto twelve slots, over WiFi
  and from a local source. Where that cannot be done, measurements that show what stands in the
  way.
- The whole path: the decode, the Annex E coding tools, the output stage, the render, the sink,
  where memory is placed, and the second core.
- Two constraints. `src/forge` belongs to the decoder core's session, and changes there need its
  agreement and the user's. The gold standard (the host's double-precision decode, and the hashes
  and levels held to it) and the other platforms must not change.

## How it was measured

The board is the second ESP32-S3-DevKitC-1 (N16R8) at 240 MHz, with no DAC. The I2S peripheral
clocks out regardless, so pacing and the sink's counters are real. The network shape is
`sdkconfig.defaults;sdkconfig.hw;sdkconfig.psram`, which sends allocations of 16 KB and over to
PSRAM, with `AC3FORGE_MINIMAL_HOT_O2` on and each shape in a fresh build directory. The streams
are [the stream set](esp32-stream-set.md), served from a PC on the LAN, and each play was started
by `POST /play`.

The instruments were applied to a throwaway worktree and not committed:

- **The library's stage timers** (`AC3FORGE_STAGE_TIMERS`), routed to
  `apps/baremetal/stage_timers.cpp` linked into the example, reset as each play starts and
  reported as it ends. An enter/leave pair costs 2.36 µs and a 7.1.4 frame opens about 160 pairs,
  so every stage-timed figure below includes about 0.4 ms of timer.
- **ESP-IDF's heap hooks** (`CONFIG_HEAP_USE_HOOKS`), counting the decode task's allocations by
  size, and by where each landed: internal RAM or PSRAM.
- **FreeRTOS run-time statistics per task** over each play, which give each core's idle time.
- **The least internal heap during a play**
  (`heap_caps_monitor_local_minimum_free_size_start`/`_stop`).
- **The twelve-slot conversion** (`ac3forge::interleave_24in32`), timed per block inside the null
  sink.

The bare-metal probe (`apps/baremetal/platform/esp32s3`) was stage-timed on the same board. It
gained a scratch `eac3_714_fold` row: the probe's 7.1.4 fixture folded to Lo/Ro in line mode, with
reference levels 62,837 and 61,994 from `ac3cli decode ... downmix=loro drcmode=line`. The board
matched both.

## The frame on the board

All figures are per 32 ms frame, in microseconds.

### The decoder alone, in internal RAM

The probe runs with no network and no PSRAM, and every allocation in internal SRAM. Its 7.1.4
fixture is 640 kbit/s with coupling, spectral extension and AHT. Its three substreams decode
fourteen channels (6 + 4 + 4) into twelve, because the first dependent's surrounds replace the
bed's.

| Stage | `eac3_714`, as coded | `eac3_714_fold`, to Lo/Ro |
|---|---|---|
| Parse: exponents, bit allocation, mantissas (AHT's inverse apart) | 5,543 | 5,532 |
| AHT: all six blocks dequantised and inverted at block 0 | 7,968 | 7,979 |
| Spectral extension | 3,935 | 3,939 |
| IMDCT and overlap-add, 84 transforms | 8,160 | 8,161 |
| The rest of the three substream decodes | 2,799 | 2,782 |
| The access unit: split, identity keys, queue, assembly | 938 | 977 |
| The output stage: the fold and dialnorm | | 4,034 |
| **Frame** | **29,385** | **33,444** |
| Peak heap, bytes | 230,798 | 280,214 |

The probe's 5.1 fixture folds in 3,180 µs (`eac3_fold`). Both folds work out at about 18 cycles
for each sample the output stage zeroes, multiplies and adds, or copies. The stage makes six
passes over the frame, and `src/forge/src/decoder/output.cpp` is compiled at `-Os`: it is not on
`src/forge/minimal.cmake`'s `AC3FORGE_MINIMAL_HOT_O2` list. Folding the whole frame at once is
also what raises the peak by 49 KB: six seats and two outputs of 1,536 samples each.

### In the player over WiFi, folded to 2.0

The I2S sink, a 64 ms DMA queue, line-mode DRC and dialnorm; stage-timed.

| | `714-walk` | `714-tones` | `layout-714` | `layout-51` |
|---|---|---|---|---|
| Parse | 8,047 | 7,728 | 9,656 | 3,566 |
| Spectral extension | 5,045 | 4,993 | | |
| IMDCT and overlap-add | 10,974 | 10,933 | 10,942 | 4,306 |
| The rest of the substream decodes | 4,482 | 4,477 | 4,509 | 1,226 |
| The access unit | 1,130 | 1,138 | 1,146 | 645 |
| The output stage | 7,854 | 8,022 | 7,817 | 5,137 |
| Render and sink write, with no wait | 1,601 | 1,579 | 1,543 | waits for the DAC |
| **Frame** | **39,225** | **38,979** | **35,741** | paced |
| Blocks to an empty queue | 149 of 900 | 62 of 378 | 27 of 192 | 0 of 192 |
| Least internal heap during the play, bytes | 2,755 | 2,939 | 7,611 | 10,167 |
| The decoder's allocations placed in PSRAM, per frame | 48 KB | 50 KB | 58 KB | 11 KB |
| Core 0 idle | 77% | 78% | 77% | 85% |
| Core 1 idle | 0.1% | 0.4% | 1.2% | 43% |

`714-walk` and `714-tones` use spectral extension; `layout-714` uses no coding tools. These
streams carry dialnorm −31, which normalises by a gain of exactly 1, so the output stage is
skipping that multiply and its time here is the fold's. Line-mode DRC is applied to each block's
coefficients inside the substream decode. Its share of that stage is not separated here: the
substream decode at 2.0 is 1.2 ms longer than in the as-coded image, and placement and DRC are
both in that difference. The session profiling the fold, DRC and dialnorm owns that split.

### In the player over WiFi, onto twelve slots, by coding tool

The null sink, levels as coded, objects as their bed. Nothing paces this sink, so a frame is the
decode, the render onto 7.1.4 and the example's level meter; stage-timed.

| Stream | Frame | IMDCT and overlap-add | Spectral extension | AHT | Enhanced coupling | Render and meter | Least internal heap | PSRAM per frame |
|---|---|---|---|---|---|---|---|---|
| `layout-714` (no tools) | 28,859 | 10,419 | | | | 4,348 | 4,023 | 25 KB |
| `714-none` (block switching) | 30,276 | 10,333 | | | | 4,434 | 5,275 | 26 KB |
| `714-walk` | 32,910 | 10,611 | 4,995 | | | 4,289 | 5,459 | 25 KB |
| `714-tones` | 32,597 | 10,626 | 5,005 | | | 4,430 | 5,431 | 25 KB |
| `714-spx` | 34,094 | 10,567 | 4,826 | | | 4,530 | 5,355 | 26 KB |
| `714-tpn` | 33,833 | 11,105 | | | | 5,066 | 5,091 | 46 KB |
| `714-cpl` | 35,036 | 10,473 | | | | 4,455 | 5,451 | 29 KB |
| `714-aht` | 38,876 | 10,741 | | 11,825 | | 4,259 | 4,291 | 32 KB |
| `714-all` (coupling, spectral extension, AHT) | 42,615 | 10,777 | 4,616 | 9,246 | | 4,668 | 4,607 | 34 KB |
| `714-ecpl` | 63,288 | 11,852 | | | 31,330 | 4,409 | 1,731 | 39 KB |

Core 0 was 69 to 82% idle in every play. A TDM sink adds the conversion to 32-bit slots on top:
396 µs per 256-sample block for twelve slots, 2.4 ms a frame, measured on the board.

### What the network shape leaves the decoder

At the start of each play the board had 161 to 169 KB of internal RAM free, and the largest block
was 86 to 94 KB. During a 7.1.4 play the least free fell to 2.6 to 5.5 KB, and to 1.7 KB with
enhanced coupling.

The decoder allocates each channel's 1,536 samples every frame (6,144 bytes; fourteen of them for
these streams) and frees them when the unit has been delivered. An allocation under 16 KB goes to
internal RAM while there is room and to PSRAM when there is not. So where a frame's buffers land
depends on what WiFi, the ring, the DMA queue and the fold hold at that moment. At 2.0, about
48 KB of each frame's allocations landed in PSRAM; onto twelve slots, 25 KB.

The cost shows in the stages that write or read those buffers. The probe's internal-RAM decode is
the comparison, since its fixture also decodes fourteen channels:

- the IMDCT and overlap-add took 30% longer in the network shape (10.6 to 11.0 ms, against 8.2);
- the rest of the substream decodes took 3.2 to 4.5 ms, against 2.8;
- the output stage took twice as long (7.8 to 8.0 ms, against 4.0).

### The second core

Core 0 runs WiFi, TCP/IP, the fetch task and the control surface, and in every play it was 69 to
85% idle. WiFi took 6 to 9% of the wall clock, TCP/IP 4 to 6%, and every other task under 1%.
That leaves about 22 to 26 ms of each 32 ms frame unused on core 0. Core 1 meanwhile runs the
decode, the output stage, the render, the meter and the sink's conversion. Once the decode falls
behind, core 1 never waits, which is why the task watchdog reported `IDLE1`.

### A local source

The partition source, with no network and no PSRAM (`sdkconfig.defaults;sdkconfig.hw`), and
`714-tones.ec3` flashed as the partition's stream:

- **Folded to 2.0, it aborts at the first unit.** 6,144 bytes were asked for with 7,536 free and
  no block larger than 5,376, from 329,868 free at the start. The fold's frame-sized scratch (the
  49 KB of peak the probe shows) does not fit beside the player's ring and DMA queue.
- **Onto twelve slots through the `tdm` sink, it stops at `sink_open`**, as the next section
  explains.

### Twelve slots on this part

The ESP32-S3's I2S cannot carry twelve 32-bit slots on one data line. ESP-IDF v6.1 refuses the
configuration with "total slots(12) * slot_bit_width(32) exceeds the maximum 128". The limit is in
the register: `tx_half_sample_bits` is a 6-bit field, so a frame carries at most 128 bits, which is
four 32-bit slots or eight 16-bit ones. `tx_tdm_tot_chan_num` reaches sixteen slots only at 8 bits
each. ESP-IDF's HAL writes half the frame into that field for every TDM configuration
(`i2s_hal_tdm_set_tx_slot`), and the v6.1 I2S guide for the ESP32-S3 states the same limits: up to
sixteen slots, but four at 32 bits, eight at 16 and sixteen at 8. The v4.4 guide says "up to 16
channels" without the widths. The part has two I2S controllers, both with TDM, so between them at
most eight 32-bit slots or sixteen 16-bit slots.

The example's `tdm` sink says it carries up to sixteen slots, and its own comment says it has
never run on hardware. CI's TDM shapes (eight slots in `sdkconfig.ci-tdm`, twelve in
`sdkconfig.ci-render` and `sdkconfig.ci-http714`) all use the `capture` sink, which converts and
checks the samples without configuring a peripheral, so nothing had reached the limit before.
Eight 32-bit slots are 256 bits, which is over the limit too. On the
board, `sink_open` fails and the play ends with `result=fail`. With no progress lines configured,
the console says so in the first second after boot and prints nothing afterwards. That read as a
hang until a boot pause let the capture see the first second.

So on this part, 7.1.4 onto twelve slots means decoding and rendering twelve slots inside the
frame and handing them to a sink that can take them. That sink could be sixteen 16-bit slots
across both I2S controllers, or a TDM device that accepts several I2S lines. The example has
neither today.

### The data cache

The 2.0 image, again, with a 64 KB data cache and 64-byte lines (`ESP32S3_DATA_CACHE_64KB`,
`ESP32S3_DATA_CACHE_LINE_64B`):

| | 32 KB, 32-byte lines | 64 KB, 64-byte lines |
|---|---|---|
| `714-walk` frame | 39,225 | 37,271 |
| `714-tones` frame | 38,979 | 36,776 |
| `layout-714` frame | 35,741 | 33,256 |
| The output stage, `714-walk` | 7,854 | 5,973 |
| Blocks to an empty queue: walk, tones, `layout-714` | 149, 62, 27 | 149, 62, 4 |
| The decoder's allocations in PSRAM per frame, `714-walk` | 48 KB | 65 KB |

That is about 2 ms a frame, most of it in the output stage, for 32 KB of internal SRAM taken from
the heap.

### The component's own sources at `-O2`

The project builds at `-Os`, so the player, the renderer, the level meter and the conversion run at
`-Os` while the decoder's hot sources run at `-O2`. The null image again, with the conversion to
twelve 32-bit slots running in the sink, before and after compiling `player.cpp` and the example's
sources at `-O2`:

| `714-walk` onto twelve slots | `-Os` | `-O2` |
|---|---|---|
| Render | 2,578 | 2,029 |
| Meter, conversion and sink | 4,390 | 3,402 |
| The conversion alone, per 256-sample block | 398 | 336 |
| Everything after the decode (`eac3_au_emit`) | 7,020 | 5,482 |
| Frame | 35,902 | 34,193 |

`layout-714` and `714-tones` moved the same way (emit 7,510 to 6,098 µs, and 7,186 to 5,859).
The application image grew by 4,400 bytes of flash, and SRAM is unchanged.

## What the profile says

Three things push a 7.1.4 frame past 32 ms in the network shape, and their costs add:

1. **The output stage at 2.0** takes 7.9 ms on the decode's core, twice its internal-RAM cost.
2. **Internal RAM runs out.** WiFi, the ring, the DMA queue and the frame-sized fold leave the
   decoder too little room. Its per-frame buffers land partly in PSRAM, and the stages that touch
   them slow by 3 to 4 ms.
3. **Everything after the decode shares its core.** The output stage, the render, the meter and
   the sink's conversion run on core 1 after the decode: 9.5 ms at 2.0, and 6.7 ms onto twelve
   slots with a TDM conversion. Core 0 meanwhile is three quarters idle.

With those three dealt with, what remains is the decode itself. That is 27 to 28 ms a frame over
WiFi for a 7.1.4 stream without AHT or enhanced coupling, and about 21 ms in internal RAM. With AHT
the decode alone takes 33 ms. With coupling, spectral extension and AHT together it takes 37 ms,
and with enhanced coupling 58 ms. Those do not fit on one core, whatever happens on the output
side.

## Options, by expected saving

Each saving is per frame on core 1, the decode's core, for `714-walk` over WiFi unless the row
says otherwise. Where a row says "estimate", the saving is worked out from the stages above rather
than measured.

| | Option | Where | Expected saving | From |
|---|---|---|---|---|
| A | An output task on core 0, fed by a ring of decoded blocks in PSRAM. The fold, the render, the meter and the sink leave the decode's core | the component | 2.0: 9.5 ms, less about 1 ms of copying (estimate). Twelve slots: 4.3 ms, or 6.7 ms with a TDM conversion, less the copy | measured stages |
| B | Two cores inside the decode: each substream's reconstruction (spectral extension, IMDCT) in parallel, and the parse kept in bitstream order | `src/forge` | 8 to 11 ms at 7.1.4 (estimate) | the stage shares above |
| C | The output stage's own cost: `-O2`, and one pass per block | `src/forge`, the fold session's | up to about 6 of the 7.9 ms at 2.0 (estimate) | 18 cycles an element at `-Os` |
| D | The fold's scratch sized to a block rather than a frame. With A, that is 8 KB on the output task instead of 49 KB on the decoder | the component, with A | 1 to 1.5 ms of placement at 2.0 (estimate) | PSRAM per frame 48 KB at 2.0, against 25 KB with no fold |
| E | A 64 KB data cache with 64-byte lines | `sdkconfig.psram` | 2.0 ms at 2.0 | measured |
| F | Per-frame channel buffers kept by the decoder, where today they are allocated each frame | `src/forge` | up to 3 to 4 ms in the network shape (estimate), and fourteen fewer allocations a frame | network shape against probe |
| G | AHT's 43 KB frame buffer split into seven buffers, so it can stay in internal RAM, and a cheaper six-point inverse | `src/forge` | 2 to 5 ms when a stream uses AHT (estimate) | 9.2 to 11.8 ms on the board, against 8.0 in internal RAM |
| H | The component's sources at `-O2`: render, conversion, meter | the component | 1.5 ms onto twelve slots, less at 2.0; 4,400 bytes of flash | measured |
| I | A hand-written Xtensa FFT for the IMDCT, as esp-dsp does it | `src/forge` | about 2 ms (estimate) | esp-dsp's published cycle counts |
| J | The dependent substreams decoded by a second `Eac3Decoder` on core 0 | the component | as B | |

I and J are listed so they can be ruled out. I adds a kernel tier built on fused multiply-adds,
which round differently from the scalar reference the tiers are held to. J gives the dependents a
dither sequence of their own. The decoder draws dither for every substream from one generator
(`Eac3Decoder::Impl::dither_`) during the parse, so their noise would differ from the host
decode's, and the levels would stop matching the host's to the digit. J would also duplicate
§E3.8.2's assembly in the component.

## Decisions

1. **Where the fold, the render and the sink run.**
   - (a) **An output task on core 0**, fed by a ring of decoded blocks in PSRAM. The decode task
     decodes, and copies each block's channels into the ring. The output task folds with the
     library's own `OutputStage` a block at a time, then renders and writes to the sink.
   - (b) As now: all of it inside the decode call on core 1.
   - (c) The decoder keeps the fold, and the output task takes the render and the sink.

   **Recommend (a).** It moves 9.5 ms of a 2.0 frame, and 6.7 ms of a twelve-slot TDM frame, to
   the core that has 22 to 26 ms idle. Folding a block at a time needs 8 KB, where the decoder's
   frame fold holds 49 KB of internal RAM. The fold stays the same code with the same arithmetic.
   The decoder is asked for its output as coded, with line mode's DRC: `drc_scale` 1, which is
   what line mode resolves to. `OutputStage` then folds each block with the unit's layout, mixing
   levels and dialnorm. For Lo/Ro, Lt/Rt and mono in line or custom mode, folding a block at a
   time gives the same samples as folding a frame at a time. RF mode's limiter ramps across
   whatever it is handed, so in RF mode the output task folds a whole unit at once. (c) leaves the
   largest item on core 1.

   Cost:
   - a third task, with a 6 to 8 KB stack from internal RAM;
   - a ring in PSRAM, 192 KB at sixteen blocks of twelve channels, and more with objects;
   - copying each block out of the decoder's storage, about 72 KB a frame at 7.1.4 and an
     estimated 0.5 to 1.4 ms on core 1;
   - about 300 lines in the component;
   - the player, not the decoder, calls the fold, so a change to the fold's semantics has two
     callers to keep in step;
   - latency grows by the ring's depth.

   Host tests would cover the ring's index arithmetic, and a 7.1.4 stream decoded both ways (the
   decoder's own fold, and as coded then `OutputStage` per block) to identical samples.

   **Taken 2026-09-11, and built as (c) for now.** The session profiling the fold opened PR #654
   the same day, held for the user's OK. It makes `OutputStage` work in 256-sample blocks, bit-exact
   in every tier, and puts `output.cpp` on the `-O2` list. On the board the 7.1.4 fold's own time
   fell from 4.06 to 1.12 ms, its 43 KB of frame-long scratch went, and `714-walk` at 2.0 over WiFi
   fell from 35.3 to 30.0 ms a frame. With the fold that cheap, moving it to core 0 would save
   about 2 ms and cost the 72 KB-a-frame copy and a second caller of the fold. So the fold stays in
   the decoder, the ring carries the decoder's blocks as they come (the fold's two channels at
   2.0), and the output task renders and writes them. If #654 does not land, (a)'s player-side
   fold is the next step.

2. **How deep the ring and the DMA queue are.**
   - (a) **A sixteen-block ring (85 ms) in PSRAM, and a 21 ms DMA queue at 2.0**: eight
     descriptors of 128 frames, 8 KB of internal RAM.
   - (b) An eight-block ring, with the DMA queue at 64 ms as now.
   - (c) A thirty-two-block ring.

   **Recommend (a).** The worst frames measured were 50 to 72 ms, against averages of 28 to 39,
   so the ring has to hold a frame's excess over 32 ms and then some. With the ring absorbing the
   decoder's bursts, the DMA queue only has to cover the output task being held off by WiFi, and
   21 ms of it returns 16 KB of internal RAM to the decoder. Cost: 85 ms of latency against
   today's 64 ms queue. These depths are reasoned from the profile, and the board run has to
   confirm them.

3. **The data cache.**
   - (a) **Decide once decision 1 has been measured.**
   - (b) A 64 KB cache with 64-byte lines in `sdkconfig.psram` now.
   - (c) Keep 32 KB.

   **Recommend (a).** Most of the 2 ms came from the output stage, which decision 1 moves to
   core 0 and shrinks. The 32 KB the cache costs comes out of the internal RAM the decoder is
   short of. Cost: one more board run.

4. **Small changes to ask of the decoder core.**
   - (a) **Once decisions 1 and 2 are on the board, propose G's AHT buffer split and F's kept
     channel buffers**, each with footprint figures for every target.
   - (b) Propose them now.
   - (c) Neither.

   **Recommend (a).** Decisions 1 and 2 are expected to bring `714-walk` and `714-tones` to about
   28 ms a frame on core 1. That is real time with a margin of about 10%. F and G widen that
   margin, and bring the 7.1.4 streams that use AHT closer, but a board figure should say how much
   is still needed before `src/forge` changes. Cost of F and G: each needs the owner's agreement
   and the user's. Each must leave the double build's arithmetic textually untouched and must not
   raise the probe's peak heap on any target. F keeps up to 84 KB of channel buffers between
   frames that today are freed between frames, so its peak is unchanged but its low point rises.

   **Taken 2026-09-11 as (b):** the user asked for F and G to be proposed now, and they went to
   the decoder core's owner that day, with #654's changes to the same file as the reason to build
   them after it lands or on top of it. Nothing in `src/forge` changes before the owner answers.

5. **Two cores inside the decode (B).**
   - (a) **Record it with the stage shares, as the next step if 7.1.4 streams with AHT must
     play.**
   - (b) Propose it to the decoder core now.

   **Recommend (a).** It is the largest change on this page, and the output-side decisions come
   first. Cost of (b):
   - the substream decode split into a parse and a reconstruction, and the reconstruction able to
     run on another core;
   - the parse kept in bitstream order, so the shared dither generator hands out the values it
     hands out today, which keeps the output bit-identical;
   - a second copy of the per-substream scratch the decoder holds once today: `tails_`, the IMDCT
     scratch, and the AHT and enhanced-coupling buffers, about 50 KB;
   - a seam through which the caller supplies threads, in a library that has no RTOS.

6. **Twelve 32-bit slots on the S3.**
   - (a) **The `tdm` sink refuses a frame of more than 128 bits and says why; the Kconfig range
     and the README state what the part allows; and 7.1.4 onto twelve slots is shown on the
     `null` and `capture` sinks.**
   - (b) A sink across both I2S controllers at 16 bits, sixteen slots.
   - (c) Leave the sink as it is.

   **Recommend (a) now, and (b) once a TDM DAC is on the bench.** (c) leaves a sink that fails on
   every board as soon as it is asked for more than four 32-bit slots. Cost of (a): a check in
   `sink_open`, a narrower Kconfig range, and the sink's comment and README section rewritten.
   Cost of (b): two controllers kept in step on one clock, which cannot be verified without a DAC.

7. **The output stage's own speed (C).** This is not decided here. It belongs to the session
   profiling the fold, DRC and dialnorm ("Profile the S3's 7.1.4 fold, DRC and dialnorm cost").
   Decision 1 calls the same `OutputStage`, so whatever that session changes reaches the player
   with no change here.

8. **The local-source shape.**
   - (a) **With decision 1 in place, play 7.1.4 to 2.0 again from the partition source without
     PSRAM, and record whether the block-sized fold fits.**
   - (b) Require PSRAM for 7.1.4 at 2.0.

   **Recommend (a).** Cost: one board run.

9. **Keeping the stage timers in the example.**
   - (a) **The example links the stage-timer backend and prints a play's stages when the library
     is built with `AC3FORGE_STAGE_TIMERS`, as the probe does.** The heap and task counters stay
     scratch.
   - (b) Keep all of it scratch.

   **Recommend (a).** The next question about where a player's frame goes then needs one build
   flag, where this one needed a patched worktree. Cost: a few lines of CMake and about twenty in
   `stream_player.cpp`. A plain build is unchanged, because nothing calls the backend and the
   linker drops it.

10. **A 7.1.4 fold row in the bare-metal probe.**
    - (a) **Add `eac3_714_fold`, whichever of this session and the fold session commits first.**
    - (b) Leave the probe as it is.

    **Recommend (a).** It gates the 7.1.4 fold's peak heap on every leg. Cost: the probe's
    reported peak (`heap.peak_bytes`, the highest of any fixture) rises from the 7.1.4 row's
    230,798 to 280,214, against the ARM leg's ceiling of 300,000 (`AC3FORGE_MAX_HEAP_BYTES` in
    `tools/checks/run_baremetal_probe.sh`). The C3 leg skips the row by heap budget, as it skips
    the other 7.1.4 row. The generator's decode variant adds a levels array and no bitstream.

    **Taken 2026-09-11: left to the fold session.** Its PR #654 adds `eac3_714_fold` together with
    the block-wise fold, which brings the row's peak to 237,206.

11. **The component's own sources at `-O2` (H).**
    - (a) **Compile `player.cpp` and the example's sinks and meter at `-O2`**, as the decoder's hot
      sources already are.
    - (b) Leave them at `-Os`.

    **Recommend (a).** It saves 1.5 ms a frame onto twelve slots on whichever core runs the
    output side, for 4,400 bytes of flash and no SRAM. Cost: the component's CMake sets a
    per-source option, and a project that needs the flash back has to opt out, as
    `AC3FORGE_MINIMAL_HOT_O2` already allows for the decoder.

## What stays out of reach

- **Enhanced coupling at 7.1.4.** The decode alone takes 58 ms on one core. Split across two
  cores (B) it is still about 30 ms, before any output work. It does not fit on this part.
- **7.1.4 with AHT, and with coupling, spectral extension and AHT together.** These decode in 33
  and 37 ms on one core, and remain over the frame after A to D. B and G together might bring
  them in.
- **Twelve 32-bit slots on one data line.** The I2S register does not allow it.

## Verification, for the implementation

- **On the board:**
  - `714-walk.ec3` and `714-tones.ec3` at 2.0 over WiFi, with no block reaching an empty queue
    and levels matching the host's Lo/Ro to the digit;
  - the stream set onto 7.1.4 through the null sink, with core 1 under 32 ms a frame and every
    slot at the host's level;
  - the per-tool table again;
  - the local shape.
- **In CI:** the ESP32 job's QEMU steps pass through the new output task unchanged. These are the
  `capture` sink at 2.0 and at twelve slots, the render shape, and the stream set over QEMU's
  Ethernet (`tools/checks/check_stream_set.py`). The host tests above also run.

## What cannot be verified

- **A DAC on twelve slots.** There is none here, and one S3 I2S controller cannot carry twelve
  32-bit slots.
- **Time and WiFi under QEMU.** QEMU has neither, so those figures come from board runs only.
