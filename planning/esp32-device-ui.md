# The ESP32 player's web UI

!!! note "Status as of 2026-09-11: built, tested on the host and under QEMU, and its requests measured on a board"
    The first pages the ESP32 player serves from its own firmware: one page that shows what the
    player is doing and drives it, served by `ac3forge::Control` beside the REST routes it
    already has, and calling only those routes. The design below was committed before the code;
    the same pull request built it as designed, with the tests and the CI that run them, and
    this page now records what the measurements said. Under QEMU the two new routes hold 76 bytes
    of internal heap and the page and its script are 16,190 bytes of a 16,384-byte flash budget.
    The per-request heap targets were missed there, by lwIP's buffers rather than by the page's
    own code, and the measuring found a fault that was there before the page: `PUT /layout`
    overflowed the server task's stack. On a board playing over WiFi the page's requests did not
    lower the least free internal heap, and the larger stack held. [Budget](#budget) and
    [What the measurements found](#what-the-measurements-found) have the figures.

    **The output layout, built 2026-09-11.** Seen on a board, the page's Layout field did not
    say that it sets the speakers the player drives, what the player does with a stream whose
    channels differ from them, or that nothing is upmixed. [The output
    layout](#the-output-layout) is what the page says and does about that now, and the five
    fields `/status` gained so that it can; [the stream set](esp32-stream-set.md) is the 7.1.4
    streams, and the range of layouts, codecs and coding tools beside them, that CI plays onto a
    twelve-slot emulated board with the page driven on it. Decisions 14 to 18 are that work's.
    The page and its script are 19,187 bytes of a 20,480-byte budget.

    **Status plus the board's own settings, 2026-09-16 (Hearth B2).** The page stopped driving
    playback: a server owns that now
    ([the Hearth plan](hearth-reference-player.md#b2-joining-a-network)), so the location field,
    Play, Stop and the volume slider are gone, and `POST /play`, `/stop` and `/volume` stay in
    the REST surface for a person with curl. What replaced them is what only the board can
    answer for: its name, the network it joins, its slot width and whether a second I2S line is
    wired, each stored in NVS. Pairing joins them in B3, when there is something to pair with.
    The budget was re-derived rather than raised to fit: **24,576 bytes**, against 20,571 used.
    Decision 18 has the reasoning.

    **The Sendspin player, 2026-09-16 (Hearth B3).** A board that plays as a Sendspin player
    reports the player in `/status`, and the page shows it in a section of its own: the server
    playing to the board and how the two are linked, whether the board's clock is in step with
    the server's, what is playing, how far from the server's time it plays, underruns, and a
    level for each output over the last 100 ms
    ([the Hearth plan](hearth-reference-player.md#b3-the-sendspin-player-on-the-board)). A pairing in progress
    puts its code on the page, and three actions go to `POST /pairing`: cancel the pairing,
    allow pairing again after codes that did not match, and forget every server. The code is
    readable by anyone who can reach the page, as it is by anyone at the board's USB port: the
    page has no login ([Security](#security) has what that means). The budget
    was re-derived again: **28,672 bytes**, against 25,594 used. Decision 18 has the reasoning.

    Shape follows [the player plan](esp32-player.md): what exists, what changes and why, a
    budget with how each figure is measured, [Decisions](#decisions) with a recommendation and
    a cost each, and [what cannot be verified](#what-cannot-be-verified).

Someone with a browser on the same network as a board should be able to see what the player is
doing - what it is playing, in what codec and channels, onto which layout, at what volume, how
much of each frame the decode takes, how low the ring between fetch and decode ran, and why a
play ended - and play a URL, stop, change the volume and change the layout without writing a
`curl` command. The REST API stays as it is. The page is a client of it: every action it offers
is one request to a route that exists today, so the device has one control path and the page
adds no state of its own to the firmware.

## What exists

| Part | What it is |
|---|---|
| [`ac3forge::Control`](../esp-idf/ac3forge/include/ac3forge/control.hpp) (`esp-idf/ac3forge/src/control.cpp`) | A REST surface on `esp_http_server`: `GET /` (a text list of the routes, before this work), `GET /status` (JSON), `POST /play` (a URL or a path as the body; `202 Accepted`), `POST /stop`, `POST /volume` (0.0 to 1.0), `GET` and `PUT /layout` (a name such as `7.1.4` or a speaker list such as `L,R,C,LFE,Ls,Rs`). JSON written by hand: IDF v6.1's core has no cJSON. |
| Its server | `max_uri_handlers` 7, exactly the routes; `max_open_sockets` 3, with the least recently used closed when a fourth arrives. Everything else is IDF's default: one task, a 4,096-byte stack from internal RAM, priority 5, either core. Every handler runs on that one task, one request at a time. |
| The owner's side | Control never touches a player. `POST` routes put a command on a queue that `app_main` empties every 100 ms (`examples/hearth_sink/main/hearth_sink.cpp`, four commands deep); `GET /status` reads a snapshot under a mutex. |
| `GET /status` | `state`, `location`, `source`, `sink`, `layout`, `volume`, `stream{codec, acmod, channels, substreams, dialnorm, objects, objects_rendered, slots}`, `frames`, `held`, `us_per_frame`, `worst_frame_us`, `render_us_per_frame`, `sink_us_per_frame`, `realtime_permille`, `resync_bytes`, `fetched_bytes`, `ring_low`, `passes`, `layout_mismatches`, `finished`, `failed`, `why`, `error`. A handler the owner leaves empty drops its field (`control.hpp`); `stream` is `null` before the first access unit and `ring_low` is `null` until measured. |
| The streaming example | Mounts Control on `CONFIG_AC3FORGE_EXAMPLE_CONTROL_PORT`; the `http` source's configurations use port 80. The control surface starts after the source opens and before the player's tasks, because its task stack has to come from internal RAM (below). |
| CI | `_build.yml`'s ESP32 job boots `sdkconfig.ci-http` in QEMU with `-nic user,model=open_eth,hostfwd=tcp::8080-:80`, lets the boot play finish, and drives `/status`, `/volume`, `/play` and `/stop` with `curl`. |

Nothing in the component embedded a file in firmware before this work.

**The constraint is internal RAM.** Measured on an ESP32-S3-DevKitC-1 on 2026-09-10 in the network
shape (`sdkconfig.defaults;sdkconfig.hw;sdkconfig.psram` plus an http overlay): with WiFi up and a
stream playing, 14 to 16 KB of internal heap stays free. One boot that started the HTTP server
after the decoder found the largest free block at 3,328 bytes and came up with no server
([On the board](../esp-idf/ac3forge/examples/hearth_sink/README.md#on-the-board)). Anything this
page adds to the firmware is weighed against those figures.

## What the comparable does

ESPHome's `web_server` component is what a user of an ESP32 media device will have met. It serves
one page per node listing the node's entities - a sensor with its value, a switch, number, select
or button with a control for each - with a log view and, optionally, firmware upload. The page
learns of changes over Server-Sent Events (`/events`), and each entity also has REST routes of
its own (`GET /sensor/<id>`, `POST /switch/<id>/turn_on`). Its current page versions load their
script from a CDN unless the configuration sets `local: true`, which puts it in the firmware.

What this page takes from it: one page, the state readable at a glance, a control beside each
thing that can be changed, the same verbs as the API, and everything in the firmware, since a
board may have no internet. What it leaves: pushed updates ([How the page updates](#how-the-page-updates)
says why), a log view (the example's console is the USB serial port and nothing captures it),
firmware upload (the partition table has one application slot), and an entity model (the player
is one object with one status).

## What the page shows

Everything comes from one `GET /status`. A field the firmware does not report is not shown.

| What | From `/status` | Shown as |
|---|---|---|
| State | `state`; `finished`, `failed`, `why`, `error` | The headline: Playing, Stopped, Finished and why, Failed and why |
| What is playing | `location`, `source`, `sink`, `sink_slots` | The location as text, the source it came through, and the sink it goes to with its slots |
| Codec | `stream.codec`, `stream.substreams`, `stream.dialnorm` | `E-AC-3, 1 substream, dialnorm -31` |
| Channels and objects | `stream.channels`, `stream.coded` (`stream.acmod` from a firmware without it), `stream.objects`, `stream.objects_rendered` | `12: L C R Ls Rs Lrs Rrs Vhl Vhr Lts Rts LFE`; objects carried, and whether they are placed onto the layout |
| Output layout | `stream.layout`, `stream.slots`, `stream.render`, `stream.silent`, `layout` | This play's output and how it is served, the speakers it leaves silent, and the next play's layout while it differs: [The output layout](#the-output-layout) |
| Volume | `volume` | The volume slider's position and a percentage |
| Real-time margin | `us_per_frame`, `render_us_per_frame`, `sink_us_per_frame`, `worst_frame_us`, `realtime_permille` | Each frame's 32 ms split into decoder, render and sink, the worst frame, and `realtime_permille` as reported |
| Ring low-water | `ring_low` | Bytes, or "not measured yet" for `null` |
| Progress | `frames` | Audio played (`frames` x 32 ms), and the frame count |
| Errors | `failed`, `why`, `error`, and the page's own requests | An error line; a refused request's reply text |
| The rest | `held`, `passes`, `fetched_bytes`, `resync_bytes`, `layout_mismatches` | Counters in a section that starts closed |

Four of these need more than a label.

**The real-time margin.** `realtime_permille` is the decode call's time against the audio it
produced, and the blocks reach the sink from inside that call. On a sink with a DAC behind it
the sink's part is mostly the wait for the DAC, so a healthy board reads close to 1000: 976 in
the default shape on 2026-09-10, with no underruns. Shown as a load, that reads as a player about
to fail. The page shows the frame split instead - decoder (`us_per_frame` less render and sink),
render, and sink, in milliseconds of the frame's 32 - with the sink's part labelled as including
any wait for the DAC, and `realtime_permille` beside it as the firmware reports it. What would
settle whether a paced sink ever ran dry is the sink's underrun count, and `/status` does not
carry it: the sinks print it on the console. See [decision 8](#decisions).

**A failed play with no error.** The streaming example sets `state` to `failed` when a source does
not open, and `failed` - the run's own verdict - is then false, since no run began. Since #638 the
example clears the last play's figures when a play begins, so a location that did not open shows
none; before it, the figures beside it were the previous play's. So the page shows the reason
(`why`, `error`) only when `failed` is true, and otherwise says the location may not have opened.

**An accepted play.** `202 Accepted` means the location went onto the queue, not that it plays.
A location the source refuses - anything but `http://` for the `http` source - leaves `state` at
`stopped` and `location` at the previous one, and the refusal is printed on the console. So after
a `202` for a new location the page watches `/status`: `location` becoming the one sent is a
location the player took, whatever comes of the play; the old `location` still there after six
seconds is one it did not take, and the page says so.

**A stopped play.** After `POST /stop`, `/status` reports the location of the play that was stopped
beside the figures of the last play that ended by itself - none, if it was stopped part-way, since
the example clears them when a play begins (#638) and keeps a player's own only when its play
ends. While a source opens, `state` is `opening`, with the new location and no figures yet; the
page shows the firmware's word, "Opening". `payloads/stopped.json` was recorded before #638 and
shows the older mix: the stopped play's location beside an earlier play's figures.

## What the page does

Four actions, each one request to an existing route. Nothing else writes to the device.

| Action | Request | Answered with |
|---|---|---|
| Play | `POST /play`, the location field's text as the body | `202`: watched through `/status` as above. `400` or `409`: the reply's text. |
| Stop | `POST /stop` | `200 stopped` |
| Volume | `POST /volume`, the slider's value as `0.00` to `1.00` | `200 ok`; `400` or `409`: the reply's text |
| Layout | `PUT /layout`, the layout field's text | `200`, effective at the next play; `400` or `409`: the reply's text |

The location field starts with the location `/status` reports, so replaying is one press. The
layout field suggests the common names (`2.0`, `5.1`, `7.1`, `5.1.4`, `7.1.4`) and takes any
text, since `OutputLayout`'s grammar has more names than those and a speaker list is also a
layout; the firmware decides.

**Volume is sent one change at a time.** A slider dragged with a mouse, or held with an arrow key,
changes thirty times a second. The queue behind `POST /volume` holds four commands and is
emptied every 100 ms, and a full queue is answered `409` with "this sink has no volume to set",
which would be a false report. So the page keeps at most one volume request in flight, sends the
latest value when it returns, and leaves at least 200 ms between sends. While a change is
pending, `/status` does not move the slider under the user's hand.

## The output layout

Seen on a board on 2026-09-11, playing a looped demo stream at `2.0`, the Layout field left
three things unsaid that a person using it needs: that it sets the speakers the player drives,
one per output slot, and says nothing about the stream; what the player does with a stream whose
channels differ from those speakers; and what "the next play uses it" changes. This section is
what the page says instead, and what `/status` adds so that the page can take it from the
device's answers rather than work out the player's rules in the script.

### What a layout does

From the code - `esp-idf/ac3forge/src/player.cpp`, `src/forge/include/ac3/render/render.hpp` and the
decoder's output stage, `src/forge/src/decoder/output.cpp` - and checked under QEMU with the
[stream set](esp32-stream-set.md):

| The layout | What the player does, whatever the stream |
|---|---|
| Two full-range speakers and nothing else (`2.0`), or one (`1.0`) | The decoder folds (§7.8): Lo/Ro, or Lt/Rt where `CONFIG_AC3FORGE_EXAMPLE_STEREO_FOLD` says so; mono for one speaker. Every channel but the LFE is in the fold. The output stage first puts each Table E2.5 location in a §7.8 seat - a height in L or R, a rear or top surround in Ls or Rs, each at -3 dB - and then folds, so a 7.1.4 stream at `2.0` plays its heights and rear surrounds in the two channels. The LFE is left out, which is §7.8's default and the player does not change it (`OutputConfig::mix_lfe`). |
| Anything wider, without height speakers | As coded. Each coded channel goes to the slot of its own location at unit gain. A channel whose location the layout has no slot for is spread over the layout's speakers by `ac3::spatial::pan_direction`. The LFE goes to LFE slots and nowhere else, and a layout with none drops it. |
| With height speakers | The same, for a stream without objects. For a stream with objects, when the player reconstructs them (`CONFIG_AC3FORGE_EXAMPLE_OBJECTS`, by default whenever the layout has a height speaker), each object is placed by its own position, the bed's LFE passes through, and the bed's other channels are not added. |

**Nothing is upmixed.** The renderer never derives a signal for a speaker from other channels. A
slot gets audio from a coded channel at its location, from a coded channel with no slot of its
own that is spread onto it, from an object placed near it, or, for an LFE slot, from the LFE,
and from nothing else. Under QEMU a 5.1 stream played onto `7.1.4` left the rear surrounds and
all four heights at exactly zero, and a 7.1 stream left the four heights at zero.

### What the page says

In **Now**, for a 5.1 stream playing on a twelve-slot sink at `7.1.4`:

| Row | From `/status` | Shown as |
|---|---|---|
| Sink | `sink`, `sink_slots` | `capture-tdm, 12 slots` |
| Channels | `stream.channels`, `stream.coded` | `6: L C R Ls Rs LFE`; from a firmware without `coded`, `6 (3/2)` as now |
| Output | `stream.layout`, `stream.slots`, `stream.render` | `7.1.4, 12 slots: each channel on the speaker at its location` |
| Silent | `stream.silent` | `Lrs, Rrs, Vhl, Vhr, Lts, Rts: nothing in the stream for these`; not shown when empty |
| Next play | `layout` | Shown while it differs from `stream.layout`, or before a play has a stream: `5.1` after a `PUT /layout` |

The Output row's words, one for each value of `stream.render`: `folded to two channels by the
decoder (Lo/Ro)`, the same with `(Lt/Rt)`, `folded to one channel by the decoder`, `each channel
on the speaker at its location`, and `objects placed by their positions`.

In **Control**, the field is **Output layout**, described as the speakers the player drives, one
per output slot, given as a name or a speaker per slot, and taking effect at the next play; the
description adds the sink's slot count when `/status` gives one. Its suggestions are the named
layouts that fit that count: `1.0` and `2.0` on a two-slot sink, up to `7.1.4` on twelve,
`9.1.6` on sixteen. Under the form, a closed **What an output layout does** says the table above
in four sentences, the last of them that nothing is upmixed. A refused layout is explained with
the page's own count where it can make one - `Output layout 5.1 refused (409): it needs 6 slots
and this sink has 2.` - and with the firmware's reply otherwise; an accepted one reads `Output
layout 5.1 from the next play.` The page still sends every layout and lets `OutputLayout::parse`
decide ([decision 16](#decisions)); it counts slots only to explain an answer.

### What `/status` adds

Five fields. All are additive: no existing field changes its name, type or meaning, and none
moves relative to the others.

| Field | What it is | Where it comes from |
|---|---|---|
| `sink_slots` | An integer, after `sink`: the slots on the sink's bus, and so the widest layout `PUT /layout` accepts | A new `ControlHandlers::sink_slots`; the example answers with `player::sink_slots()` |
| `stream.layout` | The output layout this play renders onto, as its text. `layout` stays the next play's. | `StreamInfo`, which the player fills at the first access unit |
| `stream.render` | How this play serves it: `loro`, `ltrt` or `mono` for the decoder's fold, `channels` for as coded, `objects` for objects placed | The same |
| `stream.coded` | The stream's channels by Table E2.5 location, comma-separated in the decoder's order; `Ch1,Ch2` for dual mono | The same, from the headers the player already reads to place the bed |
| `stream.silent` | The output layout's speakers this play has sent nothing to, comma-separated by their slot names; empty when every speaker has had something | The player, from the renderer's gains: fixed for `channels`, a running union over each access unit's object positions for `objects`, empty for a fold |

For the 5.1 stream above, the changed parts of the body:

```json
"sink":"capture-tdm","sink_slots":12,
"stream":{"codec":"E-AC-3","acmod":7,"channels":6,"substreams":1,"dialnorm":-31,"objects":false,
 "objects_rendered":false,"slots":12,"layout":"7.1.4","render":"channels","coded":"L,C,R,Ls,Rs,LFE",
 "silent":"Lrs,Rrs,Vhl,Vhr,Lts,Rts"}
```

What it costs, measured under QEMU on 2026-09-11 in the twelve-slot shape, with the server
task's stack high-water mark printed after each request and the print then taken out: 115 bytes
more JSON per `GET /status` for a 7.1.4 stream, a body of up to 680 bytes, and at most 420 more
when every field is full; the JSON is built in the handler's `std::string` and freed with the
request. `StreamInfo` grows by 356 bytes, three text fields and a name, copied under the player
mutex onto the server task's stack for each poll: the `/status` handler's deepest use went from
2,020 bytes to 2,288 of 6,144, and `PUT /layout` is still the deepest at 4,576. Then the code's
flash, and a mask the decode task updates once per access unit. Nothing is allocated during a
play that was not allocated before. The heap at the peak of a poll was not measured again.

(A later change adds ":small"/height-realization suffixes to `OutputLayout`'s list grammar. It
does not raise `OutputLayout::kTextBytes` past 96: a QEMU remeasurement on 2026-09-12 found that
224 boot-loops the example on a stack overflow in the main task, where `app_main` holds a
`PlayerConfig` - and with it an `OutputLayout` by value - on a stack already tight enough that the
extra 128 bytes was enough by itself. The figures above stand unchanged; a fully decorated list
longer than 96 characters loses its tail in `text()`'s echo rather than the configuration itself,
which is parsed from the caller's full string before any truncation happens.)

### What `/hardware` adds

A route of its own (decision 19), not a `/status` field: `GET /hardware`, JSON, fetched once when
the page loads rather than polled every second, since nothing in it changes while the board runs.

| Field | What it is | Where it comes from |
|---|---|---|
| `target` | `CONFIG_IDF_TARGET`, verbatim (`esp32p4`): what this firmware was built for | The build |
| `chip` | `esp_chip_info()`'s model, named (`ESP32-P4`): what is actually running it | The chip |
| `revision` | The chip's silicon revision (`1.3`) | `esp_chip_info()` |
| `cores` | CPU core count | `esp_chip_info()` |
| `fpu` | Whether this die has a hardware floating-point unit | `CONFIG_SOC_CPU_HAS_FPU` |
| `cpu_freq_mhz` | The CPU clock this build actually runs at - not a peripheral's own clock, and not a ceiling the silicon could reach under a different build | `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ` |
| `psram_bytes` | PSRAM actually brought up, in bytes; 0 when none is fitted or this build never turned it on | `esp_psram_get_size()` |
| `sink_max_slots` | This sink's own ceiling at any setting it takes, not just the one in force; left out when the owner has nothing to say | A new `ControlHandlers::sink_max_slots`; the example answers with `player::sink_max_slots()` |
| `capabilities` | Plain sentences: cores and arithmetic, clock speed, PSRAM size, the sink's ceiling | `ac3forge::describe_hardware` (`ac3forge/hardware_info.hpp`) |
| `notices` | Plain sentences: no FPU, no PSRAM, a firmware running on a different chip than it was built for, or (the ESP32-P4 only) a build accommodating pre-production silicon whose detected chip actually clears v3.0 | The same |

For a P4 dev board built with `CONFIG_ESP32P4_SELECTS_REV_LESS_V3` (so the bootloader admits
anything from v1.0 up), running genuinely v3.0+ silicon - the notice checks against v3.0
specifically (where both `hal/i2s_ll.h`'s own clock-source choice and `esp32p4/Kconfig.cpu`'s own
CPU-frequency choice change), not against this build's own lowered floor, since a v1.3 board under
the same build has nothing to be noticed about:

```json
{"target":"esp32p4","chip":"ESP32-P4","revision":"3.0","cores":2,"fpu":true,"cpu_freq_mhz":360,
 "psram_bytes":33554432,"sink_max_slots":16,
 "capabilities":["2 cores, a hardware floating-point unit","Running at 360 MHz",
 "32 MiB of PSRAM","This sink's bus reaches up to 16 slots"],
 "notices":["The detected chip is v3.0, v3.0 or newer. This build was compiled to also accept
 older, pre-production silicon: it runs the CPU at 360 MHz rather than 400 (esp32p4/Kconfig.cpu),
 and falls back to the 40 MHz crystal for I2S rather than the 160 MHz PLL v3.0+ silicon supports
 (I2S_CLK_SRC_XTAL/PLL_160M, hal/i2s_ll.h) - a build that required v3.0 or newer could reach
 both."]}
```

The page's own **Hardware** section reads `chip`, `revision`, `cores`, `cpu_freq_mhz`, `fpu` and
`psram_bytes` as its own rows (`hw-chip`, `hw-cores`, `hw-clock`, `hw-arithmetic`, `hw-psram`) and
`sink_max_slots` as `hw-sink`; `notices` is shown as a plain list under them, verbatim, and
`capabilities` is not re-rendered on the page at all - it says the same thing the rows above it
already do, in words a `curl` of the route can read without a browser. `target` is not its own row
either: a mismatch with `chip` is exactly what a `notices` entry already says, and showing both a
build target and a chip name that almost always agree would read as two facts where there is one.

**What it costs**, measured on the two files together: 25,594 to 28,103 bytes, 2,509 more - within
decision 18's 28,672-byte budget with 569 to spare, so nothing there was raised for this.

## How the page updates

By polling `GET /status`:

- once a second while the page is visible, and not at all while it is hidden (the Page
  Visibility API), so a tab left open in the background costs the device nothing;
- one request at a time: the next poll is scheduled when the previous one answers or times out,
  never on a fixed interval, so a slow device is never handed a queue of requests;
- a 4-second timeout on each request; after a failure, every 5 seconds, with the page saying
  since when the status has not been read;
- at once after an action, so the change shows without waiting for the next poll.

**Why not push.** esp_http_server runs every handler on its one task, so a handler cannot hold a
response open. Server-Sent Events are possible on it without a task per viewer - the handler
sends the headers and returns, and a timer calls `httpd_queue_work()` to send each event from the
server's task with `httpd_socket_send()` - at the cost of a socket held open per viewer (of
three), sends made at the device's pace rather than the viewer's, and code for a viewer that
disappears mid-send. WebSockets need `CONFIG_HTTPD_WS_SUPPORT` and frame buffers. Polling sends
nothing to a viewer that is not asking, keeps nothing open between requests beyond the
browser's keep-alive connection, and costs what [Budget](#budget) measures. Push would save the
request headers parsed per update and give sub-second latency, which a page about a 32 ms frame
counter and a ten-minute stream does not need.

**What a poll costs the device.** The server reads the request's headers into a buffer it grows
to their length (a browser sends several hundred bytes of headers where `curl` sends under a
hundred; `CONFIG_HTTPD_MAX_REQ_HDR_LEN` is 1,024 in IDF v6.1, so either fits), the `/status`
handler builds about 700 bytes of JSON in a `std::string`, `httpd_resp_send` allocates a buffer
for the status line and its two fixed headers, and lwIP holds the response until it is
acknowledged. All of it is freed when the request ends. The server's task runs at priority 5,
below the decode task's 6.

**Viewers.** A browser keeps one keep-alive connection, sometimes two. `max_open_sockets` stays at
3: a fourth connection makes the server close the least recently used, and a request that finds
its connection closed fails and the page says so. Chromium sends a request again, once, when a
reused connection closes before any answer; the page itself retries nothing, since a play sent
twice after a lost reply would restart. The design is for one or two viewers at a time.

## The routes

| Route | Before | After |
|---|---|---|
| `GET /` | The list of routes, `text/plain` | The page: `text/html; charset=utf-8`, from flash |
| `GET /ui.js` | - | The page's script: `text/javascript; charset=utf-8`, from flash |
| `GET /api` | - | The list of routes, `text/plain`, with `/` and `/api` added to it |
| `GET /status`, `POST /play`, `POST /stop`, `POST /volume`, `GET /layout`, `PUT /layout` | Unchanged | Unchanged, byte for byte |

`max_uri_handlers` is the size of Control's route table, 9, so a route added without a slot
cannot go missing, and a registration that fails says so on the console. The page and its script
are sent with `Cache-Control: no-cache`, so a browser does not keep a script from before a
firmware update, and a `Content-Security-Policy` that allows scripts from the device only. The
page declares an empty icon (`<link rel="icon" href="data:,">`), because a browser otherwise asks
for `/favicon.ico`, and esp_http_server answers an unknown route with a 404 and closes the
connection.

The CSS is inside the HTML and the script is its own file: one request fewer than three files,
a policy that can forbid inline script, and coverage that maps straight onto a file in the tree
([decision 3](#decisions)). The list of routes moves to `/api`, so `curl http://<board>/api` is
what `curl http://<board>/` was ([decision 4](#decisions)).

## Where the files live

In the component, beside Control, because Control is what serves them and any firmware that
mounts it gets the page: [`esp-idf/ac3forge/ui/ac3forge_ui.html`](../esp-idf/ac3forge/ui/ac3forge_ui.html)
and [`ac3forge_ui.js`](../esp-idf/ac3forge/ui/ac3forge_ui.js), listed as `EMBED_FILES` in the
component's `idf_component_register`. ESP-IDF places embedded files in `.rodata.embedded`, which
is flash, and names their symbols after the file's base name (`_binary_ac3forge_ui_html_start`);
the prefix is there because a firmware that embeds its own `index.html` would otherwise fail to
link with two definitions of the same symbol.

The handlers send each file with `httpd_resp_send` from its place in flash. That call builds its
headers in a small buffer and sends the body with `send()` from the pointer it is given; lwIP
copies the body into its send buffer (5,760 bytes by default) as the window allows. No part of
either file is copied to the heap by this code.

The embedded data sits in the component's archive and is linked only into a firmware that
references it, which is one that uses Control. The packing script copies the component
directory whole, so `ui/` goes into the registry archive with it. A `.gitattributes` line pins
`esp-idf/ac3forge/ui/**` to LF, so the bytes in flash, the size budget and the coverage offsets
are the same on every checkout, Windows included.

Plain HTML, CSS and JavaScript with no build step: the files in the tree are the files the board
serves and the files the tests load. Holding them to the budget meant writing them short -
comments that point here rather than repeat it, and CSS a rule to a line.

## Budget

Measured on 2026-09-11 under QEMU in `sdkconfig.ci-http`: the image built from this branch,
instrumented for the purpose with a once-a-second print of the least free internal heap in that
second (`heap_caps_monitor_local_minimum_free_size_start`) and the server task's stack high-water
mark after each handler, then reverted.

| Item | Budget | Measured |
|---|---|---|
| Flash: the page and its script together, as stored | 28,672 bytes ([decision 18](#decisions)); 24,576 before Hearth B3, 20,480 before B2, 16,384 before the output layout | 25,594 (9,018 + 16,576); 20,571 at B2, 19,187 at the output layout, 16,190 before. A host test fails above the budget |
| Internal heap held once the server is up: two more route registrations and handler slots | 256 bytes | 76, from the `heap:` line: 290,428 free against the base's 290,504 |
| Internal heap held while a browser has the page open | - | 376, the keep-alive connection |
| Internal heap at the peak of one `GET /status` | 3,072 bytes | 4,700 to 7,700 from the page's keep-alive connection; 5,100 from `curl`, a new connection each time |
| Internal heap at the peak of one page load | 8,192 bytes | 18,008: the page, the script and the first poll, over two or three connections |
| The server task's stack | 4,096, unchanged | The page's routes and `/status` use at most 2,020 bytes; 2,288 with the output layout's fields. `PUT /layout`, which the page calls and which predates it, used 4,596 - see below |

Flash has room. On the base branch the CI shape's image is 778,064 bytes and the board's network
shape's 1,136,064, in a 1,572,864-byte application partition; this branch adds 18,000 to the CI
shape's.

**The per-request heap figures missed their budgets, and not by the page's own code.** What
Control allocates for a request is small and short-lived: the header buffer, the JSON string, the
reply's header line. What fills a request's peak is the network stack: lwIP's connection
structures, the segments it holds until they are acknowledged - up to 5,760 bytes a connection -
and the emulated Ethernet driver's frame buffers. A `/status` poll from `curl` costs as much as one
from the page, so the page adds no cost per poll that any client of the REST API did not already
have. A page load does add one: three responses at once, two of them several kilobytes, so
several send windows fill together. It happens once per viewer; the page then costs 376 bytes held
and a poll a second.

The QEMU figures are a ceiling for the board. QEMU has no PSRAM, so there every one of those
buffers is internal RAM; on the board's network shape lwIP and WiFi try PSRAM first
(`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`). A per-socket send-buffer cap would lower the QEMU peak,
and IDF v6.1's lwIP does not offer one: its Kconfig help names a `TCP_SNDBUF` socket option that
its sources do not define.

**On the board.** Measured on 2026-09-11 on the second DevKitC-1 (an ESP32-S3 rev v0.2 with 8 MB
of octal PSRAM), with this branch's image in the network shape
(`sdkconfig.defaults;sdkconfig.hw;sdkconfig.psram`, the `http` source over WiFi, the control
surface on port 80, progress lines), playing the E-AC-3 demo looped to 64 seconds from a PC on the
LAN. A 10 ms timer took the least free internal heap in each second; a dip shorter than 10 ms can
escape it.

| | Measured |
|---|---|
| The `heap:` line as the play starts, the server up | 170,583 free, largest block 98,304; 172,775 with the 4,096-byte stack |
| Least free internal heap in any second of the play | 13,619 |
| ...in the seconds with the page's requests: `GET /`, `GET /ui.js`, five `GET /status` a second apart, six `PUT /layout` and a `GET /layout` | 13,635 |
| Free between the dips, which come every few seconds with or without requests | about 18,500 |
| Largest free internal block, the whole play | 6,144 |
| `GET /` (5,808 bytes), `GET /ui.js` (10,419), `GET /status` (544) | 0.19 s, 0.08 s, 0.09 to 0.16 s |
| The play | 12,000 blocks written, no underrun, least headroom 3 ms of 64, `realtime_permille` 971, `result=pass` |

The page's requests did not lower the least free internal heap: the dips come from the play and
are the same without them, so on the board the network's buffers for a page load stay out of
internal RAM. What the board adds is the largest free block, 6,144 bytes for the whole play: no
single internal allocation larger than that can succeed while the player plays - no new task
stack, no buffer - which bounds whatever this page, or the next one, might want to start.

## What the measurements found

**`PUT /layout` overflowed the server task's stack, before the page existed.** Recording the status
payloads for the tests, the emulated board panicked in FreeRTOS's list code - `uxListRemove`
reached from `xTaskPriorityDisinherit` as the server task released lwIP's lock, writing through a
null pointer - within a few requests of a `PUT /layout`, and in the same place with the base
branch's image, built without the page. The server task's stack high-water mark settled it: with
the stack raised to 8,192 bytes for the experiment, seventeen `PUT /layout` requests, a page load
and a play ran with no panic, and the task's deepest use was 4,596 bytes. The streaming example's
layout handler parses the layout on that task, and `OutputLayout::parse` and its temporaries do
not fit beside esp_http_server's own frames in 4,096; the overflow went past the stack's canary
without touching it, and the damage showed later. `Control::start` now takes the stack's size,
6,144 bytes by default, documented beside the callbacks that run on it. That costs 2,048 bytes of
internal RAM on every firmware that mounts Control, allocated when the server starts - which in
the streaming example is before the player's tasks take theirs ([decision 12](#decisions)). On
a board playing over WiFi the `heap:` line reads 2,192 bytes lower than with the old stack, and
six `PUT /layout` requests during the play left the least free internal heap at 13,635
([Budget](#budget)). `PUT /layout` had never run in CI: the HTTP step drives `/status`,
`/volume`, `/play` and `/stop`.
The page's smoke test on the emulated board now calls it.

**The page load's peak** is above, with the other heap figures.

## Accessibility

WCAG 2.2 AA is the target.

- Native elements for everything: a `<form>` with a `<label>` for each field, `<button>`,
  `<input type="range">` with the percentage beside it as text and in `aria-valuetext`,
  `<details>` for the counters; landmarks (`header`, `main`, `footer`), one `h1` and an `h2` per
  section, `lang="en"`.
- Values in a description list, and state as words: colour is never the only signal.
- One polite live region (`role="status"`) announces what an action did and when the state
  changes (Playing to Finished). The counters are not in a live region, or a screen reader would
  read them every second; nor is the volume's percentage, which `<output>` would have made one.
- Every control reachable and usable from the keyboard in page order, with a visible focus
  outline (`:focus-visible`).
- Text at 4.5:1 contrast or better in both the light and the dark colour scheme
  (`prefers-color-scheme`).
- One column on a narrow screen, nothing of fixed width, usable at 200% zoom; controls at least
  44 CSS pixels tall; no animation, so there is nothing for `prefers-reduced-motion` to stop.

## Security

- No authentication, as the REST API has none: whoever can reach the port can drive the player,
  with or without the page. Serving a page does not change who can.
- A Sendspin pairing code is on the page while its pairing runs, as it is on the console, so
  whoever can reach the port can read it and pair a server of their own with the board. That
  is no more than the REST API already lets anyone on the network do - play to the board - so
  the network stays the boundary for pairing as for the rest. The pairing token, which pairs a
  server with no code at all, is never on the page or in `/status`: the console alone prints it.
- The page requests relative URLs on its own origin only, and Control sends no CORS headers.
- A page on another site can already send `POST /play` with a text body to a device on the
  viewer's network, because a cross-origin request of that kind needs no preflight. The web UI
  neither adds that nor closes it ([decision 9](#decisions)).
- Strings from `/status` - a location anyone could have sent, a layout, a reason - go into the
  page as text (`textContent`), never as markup, and a test sends markup to prove it. The
  content security policy allows scripts from the device only.

## Tests

**On the host.** Playwright, from the WASM demo's harness: its package, its lockfile and its
Chromium install in `apps/wasm/tests`, with a second configuration for the device page
(`device-ui.config.js`), so there is one browser-test stack in the repository. A small Node
server stands in for the device (`device-ui/stub.js`), one per test: it serves the page and the
script with the headers Control sends, and implements the REST contract - routes, methods, status
codes, reply texts, content types, and the state a play goes through. `contract.spec.js` compares
its reply texts, headers and routes with the literals in `control.cpp`, and checks that every
request the script makes is to a route the firmware registers. Seventy-nine tests drive every action
through the page and assert on the requests the stand-in received; every error path (`400` and
`409` replies, a connection closed unanswered, a device that does not answer, a malformed or
partial `/status`); the polling rules on Playwright's clock (one request in flight, none while
hidden, the retry interval, a poll after an action); a layout's confirmation after the state change
that a poll already out brings back; the volume coalescing; keyboard-only use, contrast in both
colour schemes, focus and target size; and the page's rendering of `/status`
bodies recorded from the emulated board (`device-ui/payloads/`) - playing, finished, failed in
the decoder, a location that did not open, stopped, refused, objects carried and not - plus
payloads with fields changed or left out, as an older firmware or one with fewer handlers would
send. Objects placed on a height layout need PSRAM the emulator lacks, so that payload is a
recorded one changed, and says so. Since the output layout, bodies from the twelve-slot shape as
well (`sdkconfig.ci-http714`): a 7.1.4 stream playing and finished, a 5.1 one with the rears and
heights silent, a 7.1.4 one spread onto 5.1, a 5.1 one folded to 2.0, dual mono, objects played as
their bed, the next play's layout beside this one's, and a stream refused for its sample rate;
the field's suggestions and its explanation of a refusal; and a check that the stand-in writes
`/status`'s keys in `control.cpp`'s order.

**Coverage.** Chromium's V8 coverage of the script, collected by Playwright per test, written in
the form Node's own coverage takes, and reported by c8 (`npm run coverage:device-ui`), which fails
below 98% of statements, lines and functions and 90% of branches. The suite reaches 100, 100, 100
and 95.6.

**Budget.** A host test sums the two files, fails above 28,672 bytes, and fails on a carriage
return.

**On the target.** A step in the ESP32 job, after the HTTP step and without changing it, boots the
image the HTTP step built again with the same port forward and runs `device-ui/board/smoke.spec.js`
through it: the page loads from the firmware, byte for byte the files in the tree, and shows the
stream the boot play decoded; the volume set with the slider reads back through `GET /status`; a
layout the capture sink cannot carry is refused with the firmware's own reply; a play started
from the form finishes, and its levels on the console come out at a quarter of the boot play's;
Stop reads back as `stopped`; `GET /api` lists the routes. Then the step checks the console for a
panic or a second boot. A second step plays [the stream set](esp32-stream-set.md) onto 7.1.4 on a
twelve-slot image (`sdkconfig.ci-http714`) and runs `device-ui/board/layouts.spec.js` on it: the
Output, Silent and Next play rows through a 5.1 stream on 7.1.4, a 7.1.4 stream on 5.1 and a 5.1
stream folded to 2.0, a layout wider than the bus refused and explained, and a 44.1 kHz stream
refused. This is what runs the new handlers in the firmware; the host suite is what
exercises the script.

**Where CI runs them.** The host suite in a job of its own in `_build.yml`, `device-ui`, on
`ubuntu-latest`, with no toolchain to build first. The target smoke in the ESP32 job, which has
the emulator, with Node and Playwright added to its `espressif/idf:v6.1` container (Ubuntu 24.04,
run as root, so Playwright can install Chromium's system libraries).

## What cannot be verified

- **A long session on the board.** The board's figures come from the requests one page load and
  five polls make, sent during one 64-second play. A page left open against a board through a
  long play - Phase 1's ten minutes - has not been measured, and neither has the network shape
  with a height layout, whose object reconstruction takes more of the same internal RAM.
- **Other browsers.** CI runs Chromium. The page uses nothing newer than what current Firefox and
  Safari support, which is a statement about the code, not a test.
- **Screen readers.** The structure is what the tests check; no screen reader is run.
- **The output layout's report on a board.** On 2026-09-11 a board's `/status` gave the new
  fields for every stream in the set, at `2.0` on its I2S sink and at `7.1.4` on the null sink,
  and its page showed a 7.1.4 stream folded to 2.0. The twelve-slot page itself was read on the
  emulated board only, and what it says of objects placed over the network is from host tests
  and recorded bodies. No S3 sink sends 7.1.4 to a DAC: one TDM line carries at most four 32-bit
  slots.

## Decisions

1. **How the page learns of changes.** (a) **poll `/status` once a second while visible, not at
   all while hidden**; (b) Server-Sent Events, sent from a timer through `httpd_queue_work()`;
   (c) WebSockets. **Recommend (a).** (b) holds one of three sockets per viewer and sends at the
   device's pace; (c) needs frame buffers the brief rules out. Cost: up to a second before a
   change shows, and one request a second per open page, headers and all. **Taken, (a).**

2. **Compression.** (a) **none, within a 16,384-byte budget**; (b) gzip at build time, sent with
   `Content-Encoding: gzip`. **Recommend (a).** Gzip takes these files to 5,796 bytes, saving about
   10 KB of flash in an image with over 400 KB of room; it would not lower a page load's peak by
   much, since that is set by send windows filling at once; and it needs a generator in the
   component's CMake, a second copy or a refusal for a client that does not accept gzip, and
   served bytes that differ from the files the tests load. Cost: about 10 KB of flash and a
   longer page load than (b). **Taken, (a).**

3. **How many files.** (a) **the HTML with its CSS, and one script**; (b) one HTML file with
   everything inline; (c) HTML, CSS and script apart. **Recommend (a).** Against (b): a policy
   that forbids inline script, and coverage that maps onto the script file with no extraction
   step. Against (c): one request fewer per load. Cost: a second route and a second request per
   page load, and a policy that has to allow inline style; (b) would also have put the page on
   one connection, which bears on decision 13. **Taken, (a).**

4. **Where the list of routes goes.** (a) **`GET /api`**; (b) content negotiation on `/`, the list
   for a client whose `Accept` header does not ask for HTML. **Recommend (a).** (b) keeps
   `curl http://<board>/` printing the list, at the price of a response that depends on a header
   nobody sees. Cost: `curl http://<board>/` prints the page's HTML. **Taken, (a).**

5. **Where the page lives.** (a) **in the component, always served by Control**; (b) in the
   component, with a flag to leave it out; (c) in the example. **Recommend (a).** Control's routes
   are the component's, and a page that only the example carries is one an integrator has to
   copy. (b) saves no flash unless the flag also keeps the files out of the link, which means a
   second source file and a second way to start Control. Cost: every firmware that mounts Control
   carries the page, within the 16,384-byte budget, whether or not anyone opens it. **Taken, (a).**

6. **How accessibility is checked.** (a) **locators by role and label throughout, a keyboard-only
   run of every action, and a contrast check computed in the browser, in the host suite**; (b)
   add axe-core. **Recommend (a).** A control without an accessible name fails every test that
   drives it, which is the check that matters most here, without a dependency. Cost: no automated
   check for the rules axe-core has beyond these, such as ARIA misuse. **Taken, (a).**

7. **How coverage is measured.** (a) **Chromium's V8 coverage, reported by c8**; (b) the script
   instrumented by istanbul before it is served. **Recommend (a).** The tests load the file the
   board serves, unchanged. Cost: coverage from Chromium only, and c8 and its dependencies in the
   harness's lockfile. **Taken, (a).**

8. **Underruns in `/status`.** (a) **leave `/status` as it is, and show the frame split with the
   sink's part labelled**; (b) add the sink's counters (underruns, dry time, least headroom) to
   `/status`. **Recommend (a) here**, since the API stays as it is in this work, and (b) as its own
   change. Cost of (a): the page cannot say whether a DAC ever ran dry; the console can.

9. **Cross-site requests.** (a) **leave the API as it is and record the exposure**; (b) require
   something a cross-site request cannot send without a preflight, such as a custom header or a
   JSON content type. **Recommend (a) here and (b) as a separate decision about the API.** Cost
   of (a): a page on another site, opened by someone on the same network, can drive the player,
   as it can today. Cost of (b): every existing `curl` command changes.

10. **Where the host tests live.** (a) **a second Playwright configuration in `apps/wasm/tests`,
    sharing its package, lockfile and browser**; (b) a package of their own beside the page.
    **Recommend (a)**, which is one browser-test stack. Cost: tests for a device page under a
    directory named for the WASM demos. **Taken, (a).**

11. **The queue's ambiguous `409`.** (a) **the page never has more than one volume request in
    flight and sends at most five a second**; (b) Control tells "queue full" apart from "no
    volume" with its own status code. **Recommend (a) here and (b) with decision 8.** Cost of
    (a): holding an arrow key moves the volume five steps a second on the device, not thirty.

12. **The server task's stack.** (a) **a parameter of `Control::start`, 6,144 bytes by default**;
    (b) 4,096 as before, with the example's layout parse moved to the owner's task and its
    verdict handed back to the waiting handler; (c) 8,192. **Recommend (a).** It fixes the
    overflow for every owner, whose callbacks all run on that stack, and leaves 1,548 bytes above
    the deepest use measured. (b) costs no RAM and adds a reply path, up to 100 ms of latency and
    code in a file three other pull requests are changing; (c) doubles the margin for 2 KB more.
    Cost of (a): 2,048 bytes of internal RAM, held from the moment the server starts; 2,192 on the
    board's `heap:` line. **Taken, (a)**: the board's play kept 13,619 bytes free at its lowest
    with it.

13. **A page load's peak on the board.** (a) **leave it, and measure it on the board's network
    shape**; (b) lower lwIP's default send buffer (`CONFIG_LWIP_TCP_SND_BUF_DEFAULT`, 5,760) on the
    network shapes, which bounds every connection's segments in flight; (c) one file, so one
    connection. **Recommend (a)**: the QEMU figure is a ceiling that the board's PSRAM-first
    network buffers may not come near, and (b) and (c) change the device's TCP behaviour or the
    page's structure on the strength of an emulator. Cost: until the board is measured, a page
    load while the board plays over WiFi has a margin nobody has seen. **Answered by the board,
    2026-09-11:** the page's requests left the least free internal heap where the play puts it,
    13,635 against 13,619, so neither (b) nor (c) is needed. What the board showed instead is a
    largest free internal block of 6,144 bytes throughout the play.

14. **What `/status` says about the layout.** (a) **`sink_slots`, and in `stream` the play's
    `layout`, `render`, `coded` and `silent`**; (b) `sink_slots` and `stream.render` only; (c)
    nothing new, the page working the rest out from `layout` and the stream's fields.
    **Recommend (a).** (c) would put `OutputLayout`'s grammar, the fold's seats and the
    renderer's exact-or-spread rule into the script, where they would drift from the firmware's;
    and `acmod` cannot name a dependent substream's channels, so a 7.1.4 stream reads
    `12 (3/2)`. (b) says how a play is rendered but not which speakers it leaves silent, which is
    what "can it upmix?" asks. Cost of (a), as built: 115 bytes more JSON per poll
    for a 7.1.4 stream and at most 420; 356 bytes more in `StreamInfo`, copied onto the server
    task's stack per poll, which took the `/status` handler's deepest use from 2,020 to 2,288
    bytes of 6,144; and a mask the decode task updates once per access unit. (A later change adds
    bass-management and height-realization suffixes to the list grammar without raising
    `OutputLayout::kTextBytes` past 96 - see the "What it costs" paragraph above.)

15. **How the page explains a layout.** (a) **rows in Now for this play's output, the speakers
    it leaves silent and the next play's layout; a description on the field; and a closed "What
    an output layout does" under it**; (b) the explanation always open; (c) a select of fixed
    layouts in place of the text field. **Recommend (a).** The rows answer for the play in front
    of the user, and the closed block keeps the explanation one press away without pushing the
    controls down the page. (c) would drop speaker lists, which a DAC wired in WAV order needs.
    Cost: about 700 bytes of text and 1 KB of script, within decision 18's budget.

16. **Checking a layout against the sink.** (a) **send it, and explain a `409` with the page's
    own slot count when it can make one**; (b) refuse it in the page, without a request, when the
    count is over `sink_slots`. **Recommend (a).** The firmware stays the one judge of the
    grammar. The page's count - a name's three figures added, a list's tokens counted - is only
    used to say why. Cost: one request for a layout the page could have known would be refused.

17. **Which layouts the field suggests.** (a) **the named layouts whose slots fit
    `sink_slots`, and all of them from a firmware that does not report it**; (b) the same five for
    every sink, as now. **Recommend (a).** Cost: the suggestions can change after the first
    `/status`, and no speaker list is suggested.

18. **The flash budget.** (a) **20,480 bytes for the two files**; (b) 16,384 as now, paid for
    by cutting the page's existing text and CSS; (c) gzip, decision 2's option (b). **Recommend
    (a).** The files are 16,190 bytes and this adds about 2 KB. What the budget protects is
    flash, and the image has over 400 KB of its application partition free. The heap is not
    affected: the files are sent from flash, lwIP's send buffer bounds what one connection holds
    whatever a file's length, and on the board a page load did not lower the least free internal
    heap. (b) would cut wording that readers and the tests rely on. Cost: up to 4,096 bytes more
    flash in every firmware that mounts Control, and about 2 KB more sent per page load.

    **Re-derived 2026-09-16 (Hearth B2): 24,576 bytes.** The page traded playback for the
    board's own settings and came out at 20,571, past the old figure. The question the budget
    answers has not changed — how much flash a page may take, and how much a board sends per
    load — and neither has the answer's ceiling: the image is about 518 KB of a 1,536 KB
    factory partition. What changed is that a settings page is a form per setting, and each of
    the four costs a label, a control and a sentence saying what it does. Sending 20 KB rather
    than 16 costs one more lwIP send buffer's worth of turns on a load that happens when
    somebody opens the page, not while anything plays. `apps/wasm/tests/device-ui/budget.spec.js`
    holds the new figure.

    **Re-derived 2026-09-16 (Hearth B3): 28,672 bytes.** The Sendspin player's section took the
    page to 25,594: the server, the link, the clock, the timing, the underruns, a table of
    levels, a pairing code and three actions, each with the words a person needs to act on it.
    The question is the one B2 answered, and its ceiling has moved: a board's image with the
    player in it is 1,388,448 bytes of the 1,572,864-byte factory partition, 184 KB free, and the
    CI shape's is 1,026,576. The page is 2% of the board's image and 14% of what is left; the
    boards have 16 MB of flash, so a larger partition is a table change away when the image
    needs one. What the budget holds is still how much one page load sends, which the section's
    table of levels, at up to sixteen rows, is the largest part of that is not text.

19. **Where the hardware self-report goes.** (a) **a route of its own, `GET /hardware`, fetched
    once when the page loads**; (b) fields added to `/status`, polled every second like the rest
    of it; (c) console text only, nothing on the page. **Recommend (a).** Nothing in a chip
    model, a silicon revision, whether an FPU or PSRAM came up, or this sink's own ceiling changes
    while the board runs, so (b) would resend the same bytes once a second forever for no reason
    - the exact waste decision 1 already chose polling over WebSockets to avoid elsewhere on this
    page - and would grow `/status`'s own key-order contract test
    (`apps/wasm/tests/device-ui/contract.spec.js`) with a second hand-maintained nested-object
    special case beside `sendspin`'s. (c) puts exactly the fact a board misbehaving in the field
    needs - "I am an ESP32-P4, revision 1.3, no PSRAM" - somewhere a phone browser on the same
    network cannot reach, which is the debugging path this exists to shorten. Cost of (a): a
    second request per page load (about 480 bytes down, once), and an owner with a sink now
    answers one more optional `ControlHandlers` callback (`sink_max_slots`) if it wants that row
    filled in. **Taken, (a).**

20. **What counts as a notice versus a capability.** (a) **a capability is what this build's own
    facts already say positively (cores, PSRAM size, the sink's ceiling); a notice is a limit, an
    absence, or two facts that disagree (no FPU, no PSRAM, built for one chip and running on
    another, or - the ESP32-P4 only - a detected chip revision that clears the threshold a
    stricter build would have required)**;
    (b) one flat list, unlabelled; (c) a severity field per entry. **Recommend (a).** Splitting the
    two answers "what can this board do" and "what should I watch out for" separately, which is
    what the user asked this feature to say in the first place, without inventing a severity scale
    for a handful of plain-English sentences. (b) reads as a random pile once both kinds are mixed
    in; (c) is precision this page does not need - every notice here is worth reading, not worth
    triaging. Cost of (a): `ac3forge::describe_hardware` (`ac3forge/hardware_info.hpp`) decides
    which list a fact goes in, so a future fact's placement is a judgement call made once, in one
    place, rather than left to whoever reads the JSON. **Taken, (a).**
