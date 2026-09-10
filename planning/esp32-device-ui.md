# The ESP32 player's web UI

!!! note "Status as of 2026-09-11: designed; nothing built yet"
    The first pages the ESP32 player serves from its own firmware: one page that shows what the
    player is doing and drives it, served by `ac3forge::Control` beside the REST routes it
    already has, and calling only those routes. This is the design, committed before the code.
    The implementation, its tests and the CI that runs them follow in the same pull request.

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
| [`ac3forge::Control`](../esp-idf/ac3forge/include/ac3forge/control.hpp) (`esp-idf/ac3forge/src/control.cpp`) | A REST surface on `esp_http_server`: `GET /` (a text list of the routes), `GET /status` (JSON), `POST /play` (a URL or a path as the body; `202 Accepted`), `POST /stop`, `POST /volume` (0.0 to 1.0), `GET` and `PUT /layout` (a name such as `7.1.4` or a speaker list such as `L,R,C,LFE,Ls,Rs`). JSON written by hand: IDF v6.1's core has no cJSON. |
| Its server | `max_uri_handlers` 7, exactly the routes; `max_open_sockets` 3, with the least recently used closed when a fourth arrives. Everything else is IDF's default: one task, a 4,096-byte stack from internal RAM, priority 5, either core. Every handler runs on that one task, one request at a time. |
| The owner's side | Control never touches a player. `POST` routes put a command on a queue that `app_main` empties every 100 ms (`examples/stream_player/main/stream_player.cpp`, four commands deep); `GET /status` reads a snapshot under a mutex. |
| `GET /status` | `state`, `location`, `source`, `sink`, `layout`, `volume`, `stream{codec, acmod, channels, substreams, dialnorm, objects, objects_rendered, slots}`, `frames`, `held`, `us_per_frame`, `worst_frame_us`, `render_us_per_frame`, `sink_us_per_frame`, `realtime_permille`, `resync_bytes`, `fetched_bytes`, `ring_low`, `passes`, `layout_mismatches`, `finished`, `failed`, `why`, `error`. A handler the owner leaves empty drops its field (`control.hpp`); `stream` is `null` before the first access unit and `ring_low` is `null` until measured. |
| The streaming example | Mounts Control on `CONFIG_AC3FORGE_EXAMPLE_CONTROL_PORT`; the `http` source's configurations use port 80. The control surface starts after the source opens and before the player's tasks, because its task stack has to come from internal RAM (below). |
| CI | `_build.yml`'s ESP32 job boots `sdkconfig.ci-http` in QEMU with `-nic user,model=open_eth,hostfwd=tcp::8080-:80`, lets the boot play finish, and drives `/status`, `/volume`, `/play` and `/stop` with `curl`. |

Nothing in the component embeds a file in firmware today.

**The constraint is internal RAM.** Measured on an ESP32-S3-DevKitC-1 on 2026-09-10 in the network
shape (`sdkconfig.defaults;sdkconfig.hw;sdkconfig.psram` plus an http overlay): with WiFi up and a
stream playing, 14 to 16 KB of internal heap stays free. One boot that started the HTTP server
after the decoder found the largest free block at 3,328 bytes and came up with no server
([On the board](../esp-idf/ac3forge/examples/stream_player/README.md#on-the-board)). Anything this
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
| What is playing | `location`, `source` | The location as text, and the source it came through |
| Codec | `stream.codec`, `stream.substreams`, `stream.dialnorm` | `E-AC-3, 1 substream, dialnorm -31` |
| Channels and objects | `stream.channels`, `stream.acmod`, `stream.objects`, `stream.objects_rendered` | `6 channels (3/2)`; objects carried, and whether this layout places them |
| Layout | `layout`, `stream.slots` | The layout the next play uses, and the slots this play renders onto |
| Volume | `volume` | The volume slider's position and a percentage |
| Real-time margin | `us_per_frame`, `render_us_per_frame`, `sink_us_per_frame`, `worst_frame_us`, `realtime_permille` | Each frame's 32 ms split into decoder, render and sink, the worst frame, and `realtime_permille` as reported |
| Ring low-water | `ring_low` | Bytes, or "not measured yet" for `null` |
| Progress | `frames`, `passes` | Audio played (`frames` x 32 ms) and passes |
| Errors | `failed`, `why`, `error`, and the page's own requests | An error line; a refused request's reply text |
| The rest | `sink`, `held`, `fetched_bytes`, `resync_bytes`, `layout_mismatches` | Counters in a section that starts closed |

Three of these need more than a label.

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
not open, and the stats it reports are then the previous run's, whose `failed` is false. So the
page shows the reason (`why`, `error`) only when `failed` is true, and otherwise says that the
firmware reported no decode error and the location may not have opened.

**An accepted play.** `202 Accepted` means the location went onto the queue, not that it plays.
A location the source refuses - anything but `http://` for the `http` source - leaves `state` at
`stopped` and `location` at the previous one, and the refusal is printed on the console. So after
a `202` the page watches `/status`: `location` becoming the one sent, with `state` leaving
`stopped`, is a play that started; `failed` is a location that did not open; `stopped` with the
old `location` after a few seconds is a location the player did not take.

## What the page does

Four actions, each one request to an existing route. Nothing else writes to the device.

| Action | Request | Answered with |
|---|---|---|
| Play | `POST /play`, the location field's text as the body | `202`: watched through `/status` as above. `400` or `409`: the reply's text. |
| Stop | `POST /stop` | `200 stopped` |
| Volume | `POST /volume`, the slider's value as `0.00` to `1.00` | `200 ok`; `400` or `409`: the reply's text |
| Layout | `PUT /layout`, the layout field's text | `200`, effective at the next play; `400` or `409`: the reply's text |

The location field starts with the location `/status` reports, so replaying is one press. The
layout field suggests the common names (`2.0`, `1.0`, `5.1`, `7.1`, `5.1.2`, `5.1.4`, `7.1.4`,
`9.2.4`) and takes any text, since `OutputLayout`'s grammar has more names than those and a
speaker list is also a layout; the firmware decides.

**Volume is sent one change at a time.** A slider dragged with a mouse, or held with an arrow key,
changes thirty times a second. The queue behind `POST /volume` holds four commands and is
emptied every 100 ms, and a full queue is answered `409` with "this sink has no volume to set",
which would be a false report. So the page keeps at most one volume request in flight, sends the
latest value when it returns, and leaves at least 200 ms between sends. While a change is
pending, `/status` does not move the slider under the user's hand.

## How the page updates

By polling `GET /status`:

- once a second while the page is visible, and not at all while it is hidden (the Page
  Visibility API), so a tab left open in the background costs the device nothing;
- one request at a time: the next poll is scheduled when the previous one answers or times out,
  never on a fixed interval, so a slow device is never handed a queue of requests;
- a 4-second timeout on each request; after a failure, every 5 seconds, with the page saying
  since when the device has not answered;
- at once after an action, so the change shows without waiting for the next poll.

**Why not push.** esp_http_server runs every handler on its one task, so a handler cannot hold a
response open. Server-Sent Events are possible on it without a task per viewer - the handler
sends the headers and returns, and a timer calls `httpd_queue_work()` to send each event from the
server's task with `httpd_socket_send()` - at the cost of a socket held open per viewer (of
three), sends made at the device's pace rather than the viewer's, and code for a viewer that
disappears mid-send. WebSockets need `CONFIG_HTTPD_WS_SUPPORT` and frame buffers. Polling sends
nothing to a viewer that is not asking, keeps nothing open between requests beyond the
browser's keep-alive connection, and its cost is one small request a second, measured below.
Push would save the request headers parsed per update and give sub-second latency, which a page
about a 32 ms frame counter and a ten-minute stream does not need.

**What a poll costs the device.** The server reads the request's headers into a buffer it grows
to their length (a browser sends several hundred bytes of headers where `curl` sends under a
hundred; `CONFIG_HTTPD_MAX_REQ_HDR_LEN` is 1,024 in IDF v6.1, so either fits), the `/status`
handler builds about 700 bytes of JSON in a `std::string`, `httpd_resp_send` allocates a buffer
for the status line and its two fixed headers, and lwIP holds the response until it is
acknowledged. All of it is freed when the request ends. The server's task runs at priority 5,
below the decode task's 6. The peak is [measured](#budget) rather than estimated.

**Viewers.** A browser keeps one keep-alive connection, sometimes two. `max_open_sockets` stays at
3: a fourth connection makes the server close the least recently used, and a request that finds
its connection closed fails and the page says so. The page does not retry a `POST` by itself: a
play retried after a lost reply would restart it. The design is for one or two viewers at a
time.

## The routes

| Route | Before | After |
|---|---|---|
| `GET /` | The list of routes, `text/plain` | The page: `text/html; charset=utf-8`, from flash |
| `GET /ui.js` | - | The page's script: `text/javascript; charset=utf-8`, from flash |
| `GET /api` | - | The list of routes, `text/plain`, with `/` and `/api` added to it |
| `GET /status`, `POST /play`, `POST /stop`, `POST /volume`, `GET /layout`, `PUT /layout` | Unchanged | Unchanged, byte for byte |

`max_uri_handlers` goes from 7 to 9. The page and its script are sent with
`Cache-Control: no-cache`, so a browser does not keep a script from before a firmware update,
and a `Content-Security-Policy` that allows scripts from the device only. The page declares an
empty icon (`<link rel="icon" href="data:,">`), because a browser otherwise asks for
`/favicon.ico`, and esp_http_server answers an unknown route with a 404 and closes the
connection.

The CSS is inside the HTML and the script is its own file: one request fewer than three files,
a policy that can forbid inline script, and coverage that maps straight onto a file in the tree
([decision 3](#decisions)). The list of routes moves to `/api`, so `curl http://<board>/api` is
what `curl http://<board>/` was ([decision 4](#decisions)).

## Where the files live

In the component, beside Control, because Control is what serves them and any firmware that
mounts it gets the page: `esp-idf/ac3forge/ui/ac3forge_ui.html` and `esp-idf/ac3forge/ui/ac3forge_ui.js`,
listed as `EMBED_FILES` in the component's `idf_component_register`. ESP-IDF places embedded files
in `.rodata.embedded`, which is flash, and names their symbols after the file's base name
(`_binary_ac3forge_ui_html_start`); the prefix is there because a firmware that embeds its own
`index.html` would otherwise fail to link with two definitions of the same symbol.

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
serves and the files the tests load.

## Budget

| Item | Budget | How it is measured |
|---|---|---|
| Flash: the page and its script together, as stored | 16,384 bytes | A host test fails above it; the QEMU step prints what the board served |
| Internal heap held once the server is up: two more route registrations and handler slots | 256 bytes | The `heap:` line, which prints after the control surface has started and before the player, compared between this branch and its base in `sdkconfig.ci-http` under QEMU |
| Internal heap at the peak of one `GET /status` from a browser | 3,072 bytes | Under QEMU, the least free internal heap across the request (`heap_caps_monitor_local_minimum_free_size_start`), with nothing else running |
| Internal heap at the peak of one page load | 8,192 bytes | The same, across `GET /` and `GET /ui.js` |
| The server's task stack | Unchanged at 4,096 | The new handlers keep no buffers on the stack |

Flash has room. On the base branch the CI shape's image is 778,064 bytes and the board's network
shape's 1,136,064, in a 1,572,864-byte application partition. The budget is set so that the page
costs a percent or so of that and gzip has nothing to earn ([decision 2](#decisions)).

The heap budgets are measured under QEMU, which has no PSRAM, so everything lwIP and the server
allocate comes from internal RAM there. On the board's network shape lwIP and WiFi try PSRAM
first (`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`), so the QEMU figures are a ceiling for the board
rather than a measurement of it. A page load's peak is dominated by lwIP's send buffer, which any
response longer than a window fills; `/status` is shorter than one. A per-socket send-buffer cap
would lower that peak, and IDF v6.1's lwIP does not offer one: its Kconfig help names a
`TCP_SNDBUF` socket option that its sources do not define.

## Accessibility

WCAG 2.2 AA is the target.

- Native elements for everything: a `<form>` with a `<label>` for each field, `<button>`,
  `<input type="range">` with an `<output>` showing the percentage, `<details>` for the counters;
  landmarks (`header`, `main`, `footer`), one `h1` and an `h2` per section, `lang="en"`.
- Values in a description list, and state as words: colour is never the only signal.
- One polite live region (`role="status"`) announces what an action did and when the state
  changes (Playing to Finished). The counters are not in a live region, or a screen reader would
  read them every second.
- Every control reachable and usable from the keyboard in page order, with a visible focus
  outline (`:focus-visible`).
- Text at 4.5:1 contrast or better in both the light and the dark colour scheme
  (`prefers-color-scheme`).
- One column on a narrow screen, nothing of fixed width, usable at 200% zoom; controls at least
  44 CSS pixels tall; no animation, so there is nothing for `prefers-reduced-motion` to stop.

## Security

- No authentication, as the REST API has none: whoever can reach the port can drive the player,
  with or without the page. Serving a page does not change who can.
- The page requests relative URLs on its own origin only, and Control sends no CORS headers.
- A page on another site can already send `POST /play` with a text body to a device on the
  viewer's network, because a cross-origin request of that kind needs no preflight. The web UI
  neither adds that nor closes it ([decision 9](#decisions)).
- Strings from `/status` - a location anyone could have sent, a layout, a reason - go into the
  page as text (`textContent`), never as markup. The content security policy allows scripts
  from the device only.

## Tests

**On the host.** Playwright, from the WASM demo's harness: its package, its lockfile and its
Chromium install in `apps/wasm/tests`, with a second configuration for the device page, so there
is one browser-test stack in the repository. A small Node server stands in for the device: it
serves the page and the script with the headers Control sends, and implements the REST contract -
routes, methods, status codes, reply texts, content types, and the state a play goes through. A
test compares its reply texts with the literals in `control.cpp`, so the stand-in cannot drift
from the firmware unnoticed. The tests drive every action through the page (play, stop, volume,
layout) and assert on the requests the stand-in received; every error path (`400` and `409`
replies, a device that does not answer, a timeout, a malformed or partial `/status`); the polling
rules (one request in flight, none while hidden, the retry interval, a poll after an action); the
volume coalescing; keyboard-only use; and contrast in both colour schemes. State rendering is
tested from `/status` payloads recorded from QEMU runs of the CI shape - playing, finished,
failed, objects carried and not - plus payloads that leave fields out, as a firmware with fewer
handlers would. A payload the emulator cannot produce, such as objects placed on a height
layout, which needs PSRAM, is marked as derived from a recorded one.

**Coverage.** Chromium's V8 coverage of the script, collected by Playwright per test, written in
the form Node's own coverage takes, and reported by c8 with line, branch and function thresholds
that fail the job below them.

**Budget.** A host test sums the two files and fails above 16,384 bytes.

**On the target.** A step in the ESP32 job, after the HTTP step and without changing it, boots the
image the HTTP step built again with the same port forward, and runs a Playwright smoke through
it against the emulated board: the page loads from the firmware and shows the stream the boot
play decoded; the volume set with the slider reads back through `GET /status` and the next play's
levels on the console come out at that volume; a layout the capture sink cannot carry is refused
with the firmware's own reply; a play started from the form finishes; Stop reads back as
`stopped`; `GET /api` lists the routes. Then the step checks the console for a panic or a second
boot. This is what exercises the new handlers in the firmware; the host suite is what exercises
the script.

**Where CI runs them.** The host suite in a job of its own in `_build.yml` on `ubuntu-latest`,
beside the WASM job, with no toolchain to build first. The target smoke in the ESP32 job, which
has the emulator, with Node and Playwright added to its `espressif/idf:v6.1` container
(Ubuntu 24.04, run as root, so Playwright can install Chromium's system libraries). That adds a
couple of minutes to that job.

## What cannot be verified

- **The board's network shape.** QEMU's Ethernet is lighter than WiFi and QEMU has no PSRAM. The
  heap figures above come from the emulator and are a ceiling for the board, not a measurement
  of it; the page has not polled a board playing over WiFi in this work. The second board
  belongs to another session.
- **Other browsers.** CI runs Chromium. The page uses nothing newer than what current Firefox and
  Safari support, which is a statement about the code, not a test.
- **Screen readers.** The structure is what the tests check; no screen reader is run.

## Decisions

1. **How the page learns of changes.** (a) **poll `/status` once a second while visible, not at
   all while hidden**; (b) Server-Sent Events, sent from a timer through `httpd_queue_work()`;
   (c) WebSockets. **Recommend (a).** (b) holds one of three sockets per viewer and sends at the
   device's pace; (c) needs frame buffers the brief rules out. Cost: up to a second before a
   change shows, and one request a second per open page, headers and all.

2. **Compression.** (a) **none, within a 16,384-byte budget**; (b) gzip at build time, sent with
   `Content-Encoding: gzip`. **Recommend (a).** Text of this kind compresses to about a third, so
   (b) saves about 10 KB of flash at the budget's ceiling, in an image with over 400 KB of room;
   it does not lower the peak RAM of a page load, which is lwIP's send window either way; and it
   needs a generator in the component's CMake, a second copy or a refusal for a client that does
   not accept gzip, and served bytes that differ from the files the tests load. Cost: about
   10 KB of flash and a longer page load than (b).

3. **How many files.** (a) **the HTML with its CSS, and one script**; (b) one HTML file with
   everything inline; (c) HTML, CSS and script apart. **Recommend (a).** Against (b): a policy
   that forbids inline script, and coverage that maps onto the script file with no extraction
   step. Against (c): one request fewer per load. Cost: a second route and a second request per
   page load, and a policy that has to allow inline style.

4. **Where the list of routes goes.** (a) **`GET /api`**; (b) content negotiation on `/`, the list
   for a client whose `Accept` header does not ask for HTML. **Recommend (a).** (b) keeps
   `curl http://<board>/` printing the list, at the price of a response that depends on a header
   nobody sees. Cost: `curl http://<board>/` prints the page's HTML.

5. **Where the page lives.** (a) **in the component, always served by Control**; (b) in the
   component, with a flag to leave it out; (c) in the example. **Recommend (a).** Control's routes
   are the component's, and a page that only the example carries is one an integrator has to
   copy. (b) saves no flash unless the flag also keeps the files out of the link, which means a
   second source file and a second way to start Control. Cost: every firmware that mounts Control
   carries the page, within the 16,384-byte budget, whether or not anyone opens it.

6. **How accessibility is checked.** (a) **locators by role and label throughout, a keyboard-only
   run of every action, and a contrast check computed in the browser, in the host suite**; (b)
   add axe-core. **Recommend (a).** A control without an accessible name fails every test that
   drives it, which is the check that matters most here, without a dependency. Cost: no automated
   check for the rules axe-core has beyond these, such as ARIA misuse.

7. **How coverage is measured.** (a) **Chromium's V8 coverage, reported by c8**; (b) the script
   instrumented by istanbul before it is served. **Recommend (a).** The tests load the file the
   board serves, unchanged. Cost: coverage from Chromium only, and c8 and its dependencies in the
   harness's lockfile.

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
    directory named for the WASM demos.

11. **The queue's ambiguous `409`.** (a) **the page never has more than one volume request in
    flight and sends at most five a second**; (b) Control tells "queue full" apart from "no
    volume" with its own status code. **Recommend (a) here and (b) with decision 8.** Cost of
    (a): holding an arrow key moves the volume five steps a second on the device, not thirty.
