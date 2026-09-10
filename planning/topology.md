# Source, transport, sink: how the encoder and decoder reach people

!!! note "Status as of 2026-09-08: one decision taken, nothing built"
    Written 2026-09-07. The transport question is settled ([decision 1](#decisions)); no code
    on this page has been written, and the applications it frames — a plugin, a player, a
    reporter — remain unstarted.

    Four plans are in flight, each scoped as an application — a plugin, a player, a reporter,
    an embedded port. This page is the frame they are missing: the roles those applications
    occupy, and the transport between them. The transport question was settled on 2026-09-07
    ([decision 1](#decisions)); everything else here carries a recommendation and a cost.

    Shape follows [the recasting plan](recasting.md) and
    [the promotion plan](../docs/crucible/promotion.md): design sections say what changes and why,
    phases carry exit criteria and how each is verified, [Decisions](#decisions) lists what only
    remains open, and [What cannot be verified](#what-cannot-be-verified-and-why) says
    where the evidence stops.

An encoder and a decoder are not usable by anyone. They become usable when something wraps them,
and the repository now has four wrappers planned at once, written independently:

| In flight | Wraps | State |
|---|---|---|
| [Host plugin study](host-plugin.md) | the encoder, in a DAW | Study done. No open plugin format carries object metadata; beds only. The VST 3 SDK is MIT, per Steinberg's licensing FAQ. |
| [Delivery-QC report](qc-report.md) | the analysis code | Plan. |
| [The playback appliance](player-appliance.md) | the decoder, in a room | Plan, parked on the question this page answers. |
| The ESP32-S3 port | the decoder, on a microcontroller | **AC-3 and E-AC-3 both decode on an ESP32-S3**, inside internal SRAM. [PR #546](https://github.com/iainchesworthlabs/ac3forge/pull/546). |

Each was scoped as "what application is this". None of them asks what they have in common, and
the player plan stalled precisely because it had no answer: it reached the whole-house question
with no framework and could only ask whether to join somebody else's protocol.

## The three roles

The wrappers are not four products. They are instances of three roles.

| Role | Produces | Shipped today | Planned or proven |
|---|---|---|---|
| **Source** | an encoded elementary stream | Crucible (live desktop audio), `ac3cli` (files, live capture), `ac3gui`, the Shield demo, the WASM encode page | the DAW plugin; games |
| **Transport** | carries that stream to a sink | **IEC 61937 over HDMI or S/PDIF. That is the entire list.** | ← this page |
| **Sink** | sound in a room | a third-party AV receiver | the appliance on a Pi or PC; an **ESP32-S3** node; a browser over the WASM decoder; a phone |

Every path in the tree today is one of two shapes:

```
  our encoder ──► local wire (IEC 61937) ──► somebody else's decoder      (Crucible, ac3cli play)
  a file      ──► local disk            ──► our decoder                   (ac3cli decode/monitor, the GUI player)
```

**There has never been `our encoder → a network → our decoder`.** That single absence explains
three separate things that looked unrelated: why Crucible can only ever feed the one receiver it
is cabled to, why the appliance plan had nowhere to put multi-room audio, and why the ESP32-S3
port reads as a curiosity rather than as a product.

## Why no existing ecosystem closes the gap

This was checked before proposing anything, because the cheapest answer would have been to adopt
someone else's protocol. It does not work, and the reasons are in the projects' own
documentation rather than in anybody's opinion.

| Ecosystem | What it does to the audio | Consequence |
|---|---|---|
| **Snapcast** | Time synchronisation works "by removing/duplicating single samples" — continuously, by design (snapcast README) | Fatal to an IEC 61937 burst stream even with the PCM or FLAC codec and volume disabled. The sync mechanism itself is the corruption. |
| **Sendspin** (formerly Resonate) | Opus (`esp-libopus`) | Lossy. Cannot carry a bitstream at all. |
| **Music Assistant** | Decodes everything to **32-bit float PCM**, applies gain, re-encodes to FLAC or MP3 before it reaches any player (MA tech-info) | There is no non-transcoding path through it. None of its twenty-four player providers can receive original bytes. |

So: **no open whole-house ecosystem can deliver a compressed AC-3, E-AC-3 or Atmos bitstream to
a renderer intact.** A sink fed by any of them is a PCM speaker, and the codec — the entire
reason this project exists — is out of the loop.

That finding is recorded here once, rather than re-derived in each plan. It is also not a
complaint: Music Assistant is a good library and controller, and it stays one. It cannot be the
transport, and it does not need to be.

## The transport, now: HLS and CMAF over HTTP

Settled as [decision 1](#decisions). The reason it is settled cheaply is that most of it is
already shipped and nobody had connected it to this problem.

**What exists.** Roadmap IO4 built the streaming fMP4/CMAF fragmenter and IO5 added the DASH JOC
signalling:

- `src/mp4/include/mp4/mp4.hpp` — `FragmentWriter`, `MediaSegment`, `SegmentInfo`,
  `FragmentOptions`.
- `src/mp4/include/mp4/hls.hpp` — `build_hls_master_playlist`, `build_hls_media_playlist`,
  `hls_codec_string`.
- `src/mp4/include/mp4/dash.hpp` — the dynamic MPD, with TS 103 420 D.2's supplemental
  properties and the `ceao` compatibility brand.
- `src/mp4/include/mp4/reader.hpp` — **the read direction**, so a sink can pull an access unit
  back out of a segment without new demuxing code.
- `apps/common/fmp4_folder_writer.hpp` — and this is the piece that matters most. It writes
  `init.mp4`, one `segment<N>.m4s` per closed fragment, and `audio.m3u8`, `master.m3u8` and
  `manifest.mpd` rewritten beside them on every close. Its own header says what that makes it:
  *"Live-shaped while the take runs — no `#EXT-X-ENDLIST`, a `type="dynamic"` MPD with an
  `availabilityStartTime`, so the folder is a servable origin mid-take."*

  It is already the shared write-as-you-go path for `ac3cli record`, `ac3cli live`, the GUI's
  Record button and the GUI's live session.

**So a source already produces a servable live origin.** What is missing is small and named:

1. **Serving it.** A static file server over that folder. The appliance plan already chose
   `cpp-httplib` ([player-appliance](player-appliance.md), decision 8), which serves a directory
   in a few lines.
2. **Consuming it.** A client that fetches the media playlist, follows it, pulls segments,
   extracts access units through `mp4::reader`, and feeds the decoder. This is the real new
   work, and it is bounded: an `.m3u8` parser, an HTTP GET loop, and a rebuffer policy.
3. **Finding it.** mDNS, so a sink discovers sources and a controller discovers both.
4. **Deleting behind itself.** `FragmentOptions::playlist_window_segments` exists for exactly
   this and defaults to 0 (keep everything). A live origin sets it to its time-shift depth. RFC
   8216 §6.2.2 wants at least three target durations in the playlist.

**Latency, and the lever that is already there.** Plain HLS at the default 48 frames per
fragment is 1.536 s segments; a player holding three of them is roughly 5 s behind live. That is
fine for music in another room and wrong for anything watched. Low-Latency HLS is **not**
implemented — `#EXT-X-PART` appears nowhere, and the only tag beyond the basics is
`#EXT-X-INDEPENDENT-SEGMENTS` (`src/mp4/src/hls.cpp:117`).

It may not be needed. `FragmentOptions`' own comment records the property that makes segment
length free: *"Every AC-3/E-AC-3 access unit this project produces is independently decodable
… so any grouping is valid; this only trades segment count for segment-switch/start-up
latency."* Four frames per fragment is 128 ms segments. The cost is playlist churn and HTTP
request rate, not correctness — so the latency lever exists today and LL-HLS becomes an
optimisation rather than a prerequisite. What that costs in practice is measured in
[Phase 2](#phase-2-one-source-one-sink-over-http), not assumed here.

**Bandwidth, which is the argument for carrying the encoded stream at all.** A 5.1.4 object bed
as E-AC-3 with JOC is on the order of 768 kb/s. The same content as 10-channel 48 kHz 16-bit PCM
is about 7.7 Mb/s, and lossless compression takes maybe half of that. **An order of magnitude**
— which is the difference between an ESP32-S3 receiving Atmos over Wi-Fi and an ESP32-S3 not
receiving surround at all.

## The transport, later: a Sendspin extension

Also settled as [decision 1](#decisions): after HLS, as a conversation rather than a task.

Sendspin (formerly Resonate) is the Open Home Foundation's synchronised multi-room protocol,
developed with the Music Assistant and Home Assistant contributors. WebSocket transport, Opus
audio, Kalman-filter time sync quoting sub-0.05 ms deviation between nodes, a published spec at
`github.com/Sendspin/spec`, an `aiosendspin` Python implementation, built-in Music Assistant
server support from 2.7.0, and an ESPHome component that is still an unmerged pull request.

**What an extension would have to add.** Two things, and they are ordinary protocol work rather
than research:

1. **Codec negotiation beyond Opus** — an endpoint advertising that it accepts `eac3`, and a
   server choosing it when every member of a group does.
2. **A passthrough-capable endpoint capability** — an endpoint saying it will hand the bitstream
   onward to a receiver rather than render it, which the server needs in order to know that this
   endpoint's latency is unknown and unbounded.

**Why it is strategically the right target.** It is where Home Assistant is going, ESPHome will
ship the client, and Music Assistant is already the controller. An endpoint that speaks Sendspin
appears in an existing installation with no integration to write. And the extension would be
useful to that project independently of this one: Opus at multi-channel is not what anyone wants
for a home cinema bed.

**Why not now.** Three reasons, all timing:

- The spec is in technical preview and moving; building against it means rebuilding.
- The ESPHome component is not merged, so the client population is approximately zero today.
- It is **a proposal to another project**, not a task that can be scheduled. It needs a working
  implementation to argue from, which is what the HLS phase produces.

**The risk worth stating.** Sendspin may decline a compressed-bitstream extension, on the reasonable
grounds that unknown-latency endpoints break the guarantee the protocol exists to make. That
outcome is survivable — the HLS transport is not contingent on it — and it should be assumed
rather than hoped against.

## Synchronisation

Two facts, and one line between them that should be written down once.

**Frame granularity.** An AC-3 frame is 1536 samples, 32 ms at 48 kHz. E-AC-3 access units carry
1, 2, 3 or 6 blocks of 256 samples, so 5.33 ms to 32 ms. A decoder can only *begin* on an access
unit boundary, so the coarsest possible alignment before any resampling is one access unit.

**But our sinks decode.** An endpoint running this project's decoder produces PCM and can then
time-align in the PCM domain — sample-dropping and duplicating, resampling, or a buffer offset —
which is exactly what Snapcast does. So:

> **Our own sinks can synchronise to the sample. A third-party AV receiver cannot, because we do
> not control its decode and it does not report its latency.**

That is a clean boundary and it decides the product shape. A room with an AVR is its own zone. A
group of our own endpoints can be a synchronised group. The two do not mix, and no amount of
protocol work changes it.

## What the ESP32-S3 result establishes

Measured under `idf.py qemu` and landed in
[PR #546](https://github.com/iainchesworthlabs/ac3forge/pull/546). The current figures live on
[the ESP32-S3 page](../docs/platforms/esp32.md); the summary below is what this page's argument rests
on.

- **AC-3 *and* E-AC-3 5.1 both decode correctly on an ESP32-S3.** Six frames each, all twelve
  channel levels exact against `apps/baremetal/fixture.hpp`. It **fits internal SRAM with no
  PSRAM**: the allocator reports 280,792 bytes free against a 236,391-byte peak heap.
- **float32 closed it, and closed PF7's float32 gap with it.** A profile-selected
  `decode_scalar_t`, validated at roughly 139 dB against the double decode on four real streams
  and 2.7e-7 at the transform. The memory fix and the speed fix were the same fix, because the
  S3's FPU is single-precision only. The full build is untouched.
- **Real-time throughput is still unknown and cannot be answered there.** QEMU-Xtensa is not
  cycle-accurate and reports a clock disagreeing with its own boot log. It needs an
  `ESP32-S3-DevKitC-1-N16R8`, and everything for it is wired.
- **The port paid for itself beyond this target.** A 32 KB `thread_local` in `eac3_tools.cpp`
  made `ac3::forge_minimal` unlinkable into *any* FreeRTOS application — FreeRTOS carves each
  task's thread-local area out of that task's own stack, and IDF's IPC task has 1 KB, so the app
  died inside `esp_ipc_init()` before `app_main`, having never decoded a frame.

| | Before #546 | After #546 |
|---|---|---|
| `arm-none-eabi` image | 418,244 | **283,484** (−32%) |
| `.bss` | 237,592 | **97,152** |
| Peak heap | 270,886 | **171,558** (−37%) |

Those were the figures #546 itself moved. Fixture coverage added since has taken the peak to
236,391 and the image to 320,940: the table above predates the Atmos fixture that reconstructs its
objects, which is what the peak now measures. [The footprint
table](../docs/performance-trend.md#minimum-footprint-decoder) carries the current set, measured on
`main` at `be71f454`.

There is now a `build-esp32s3` CI leg (in `espressif/idf:v6.1`, leg 6 of the Linux fan-out) and a
`docs/platforms/esp32.md`.

What that means for this page: **the sink role reaches a microcontroller**, as a decode that ran
and matched rather than as a projection. That is what makes the network transport worth building,
because the population of possible sinks is no longer "a Pi or a PC".

## Where everything sits

| Thing | Role | Transport it uses | Status |
|---|---|---|---|
| Crucible | source | IEC 61937 today; HTTP origin would be new | shipped; the network output is a scope question, [decision 3](#decisions) |
| `ac3cli record` / `live`, `ac3gui` live session | source | **already writes a servable HTTP origin** through `Fmp4FolderWriter` | shipped, unconnected to any sink |
| A DAW **encode** plugin | source | whatever the host renders to; **bed-only, unconditionally** | [study](host-plugin.md) done; the constrained one |
| The Shield demo | source | IEC 61937 over the Shield's HDMI | shipped |
| The WASM encode page | source | none — it produces a file | shipped |
| An out-of-tree GStreamer element or FFmpeg wrapper (**AP10**) | source | whatever the pipeline is muxing into | on the roadmap (`ROADMAP.md:2163`), unstarted, and its dependency AP5 is done |
| A third-party AV receiver | sink | IEC 61937 | not ours; **cannot be synchronised** |
| The playback appliance | sink | local files today; HTTP client is the new work | [plan](player-appliance.md), to be rewritten against this page |
| An ESP32-S3 node | sink | HTTP client, then Sendspin | **both codecs decode, fits internal SRAM** ([#546](https://github.com/iainchesworthlabs/ac3forge/pull/546)); real time unmeasured |
| The WASM decode page | sink | a file today; could be an HLS client for free | shipped |
| A DAW **metering** plugin | **neither** — an instrument, not a node | n/a | what [the study](host-plugin.md)'s Part 2 actually plans; no capability blocker |
| The delivery-QC report | **neither** — an instrument, not a node | n/a | [plan](qc-report.md); belongs under Forge, and this page is why |

Three notes on those rows, all from [the plugin study](host-plugin.md).

**"Plugin" is two things, and only one of them is a source.** An *encode* plugin would produce a
stream; a *metering* plugin measures one, which puts it beside the delivery-QC report as an
instrument rather than a node. Part 2 of that study plans the metering one. The two have
different blockers and the paragraphs below keep them apart.

**An encode plugin's stream is bed-only and always will be.** Not a limit of any one format: a
plugin on an object track sees that track's audio as a plain channel arrangement, and the host
and its renderer own the object metadata and never hand it over. There is no authoring path
elsewhere in a session that reaches a plugin, so "objects arrive from upstream" is not a case
that exists. The transport carries objects fine — they are inside the E-AC-3 JOC bitstream — but
a plugin is not where they can enter.

**AP10 may be the cheaper source.** FFmpeg's `ff_ac3_ch_layouts` still caps its E-AC-3 encoder at
5.1 on current master (verified 2026-09-07), and GStreamer inherits that because `avenc_eac3`
wraps FFmpeg's encoder. So an out-of-tree element or external-encoder wrapper over the C API is
how anything above 5.1, and JOC at all, reaches the whole transcode ecosystem. It is already on
the roadmap, its dependency is done, and unlike the plugin it has no format-expressiveness
blocker to work around — GStreamer's `GstMeta` is extensible, so object metadata is at least
*representable* across a pipeline, which is the one place in that study where the door is not
closed.

**But it is not a difficulty ranking, and this page should not imply one.** AP10 is the cheaper
piece of work, and **neither it nor the metering plugin has a capability blocker** — the plugin
carries a UI, seven locales, accessibility, signing and two format identities that freeze on
first release, none of which AP10 does. The *encode* plugin is the one with real blockers (a
worst-case block-time measurement that does not exist, a missing `encode_frame_into()`, and no
objects), and AP10 clears all of them. Which comes first is an **audience** question — transcode
operators or mixing engineers — and the plugin study declines to answer it for that reason. It
is on this page so it is asked rather than buried, not so it is settled here.

## Format negotiation is one problem, not several

The appliance plan proposes an `ac3::audio` helper so that `ac3cli play` and Crucible's output
policy stop answering "what does this sink accept" separately
([player-appliance](player-appliance.md), decision 10). The topology generalises it: **the
question is the same whether the far end is a receiver over EDID, an ESP32 over Wi-Fi, or a
browser.** What differs is only how the answer arrives — CEA-861 short audio descriptors,
WASAPI's `IsFormatSupported`, PipeWire's `iec958.codecs`, an HTTP capability document, a
Sendspin `hello`.

So the helper should return a capability set, not a wire-specific answer, and the network
transports become additional providers of the same struct rather than a parallel mechanism. That
is a design constraint on work already planned, not new work.

## What this page does not change

- **It creates no new member.** The topology is a frame. The sink member is still whatever
  [the appliance plan](player-appliance.md) names; the source members already exist.
- **It moves nothing on disk** and renames no identifier.
- **It does not make the QC reporter a node.** It confirms the opposite.
- **It does not commit to Sendspin.** [Decision 1](#decisions) sequences a conversation.

## Phases

### Phase 1: this page, and the plans repointed

Land the frame; rewrite [the appliance plan](player-appliance.md) against it (it currently
answers a whole-house question this page makes obsolete, and predates the ESP32 result and the
transport decision); add a pointer from [the plugin study](host-plugin.md) and
[the QC plan](qc-report.md) naming their role.

**Exit:** each of the four plans states its role in the first screen and links here; no plan
proposes joining a third-party protocol as a client.

**Verified by:** `mkdocs build --strict`; `tools/checks/check_doc_paths.py`; the docs-only fast
path (`ci.yml:314`) classifies each PR as docs-only.

### Phase 2: one source, one sink, over HTTP

The smallest end-to-end proof. Serve an existing `Fmp4FolderWriter` output over `cpp-httplib`;
write the HLS client that follows the playlist, pulls segments, extracts access units through
`mp4::reader` and decodes them. No discovery, no sync, no UI.

**Exit:** `ac3cli live` on one machine, playing on another over the network, with the sink's own
decode matching a local decode of the same take byte for byte at the PCM level.

**Verified by:** a new `ac3tests` label over the playlist parser and the segment-follow state
machine, both pure and fake-driven; an end-to-end test over loopback in CI; **on the Pi**, a real
two-machine run. Latency measured, not assumed, at 48, 16, 8 and 4 frames per fragment.

### Phase 3: the latency and window numbers

Set `playlist_window_segments`, choose a default segment length for each of the two cases (a
file being played, and a live source), and record what LL-HLS would add over the shortest
segment length that works.

**Exit:** a table in the docs giving measured glass-to-glass latency per segment length, and a
default for each case chosen from it.

**Verified by:** the measurements themselves, on the Pi and on a workstation, at least one of
them over Wi-Fi rather than Ethernet.

### Phase 4: discovery, and more than one sink

mDNS for sources and sinks; a group of our own sinks playing the same origin; PCM-domain
alignment between them.

**Exit:** two sinks on one origin, measured drift stated as a number rather than as "sounds
fine".

**Verified by:** a measured offset between the two outputs — which needs two machines and a way
to capture both, so this phase says how before it starts.

### Phase 5: the ESP32-S3 sink

float32 for the `double` buffers, E-AC-3 fitting in internal SRAM, and the HLS client on the
device. Depends on a board for the real-time answer.

**Exit:** an ESP32-S3 decoding E-AC-3 from a network origin in real time, or a measured statement
of how far short it falls.

**Verified by:** an `ESP32-S3-DevKitC-1-N16R8`; QEMU cannot answer it. **Needs hardware.**

### Phase 6, conditional: the Sendspin conversation

With Phases 2–5 as the argument, propose codec negotiation and a passthrough-capable endpoint
capability to the Sendspin project.

**Exit:** a spec issue or pull request opened, and their answer recorded here whichever way it
goes.

**Verified by:** the thread itself. Not schedulable, and not contingent for anything above it.

## What cannot be verified, and why

| Claim | Can it be verified | Blocker |
|---|---|---|
| An ESP32-S3 decodes E-AC-3 in real time | yes, with a board | QEMU-Xtensa reports 40 MHz against its own boot log's 160. No amount of emulation closes this. |
| Segment length *N* gives latency *L* on a real network | yes, in Phase 3 | needs two machines and Wi-Fi, not loopback |
| The Sendspin project would accept a bitstream extension | **no** | it is another project's decision, and they have a reasonable case for refusing |
| Sendspin's spec is stable enough to build against | **no**, today | technical preview; the ESPHome component is an unmerged PR |
| A third-party AVR can be synchronised with our sinks | **no**, and it never will be | the receiver does not report its decode latency; this is a property of the device class |
| Multi-room drift is inaudible | **no** as stated | "inaudible" is not measurable; Phase 4 states a number instead |
| Any of this is what a user wants | **no** | four plans, no users yet. The appliance plan's own note applies: the install page has not survived an outside reader. |

## Coordination

**The cross-branch links are converted.** *Closed 2026-09-07.* While
[#537](https://github.com/iainchesworthlabs/ac3forge/pull/537),
[#538](https://github.com/iainchesworthlabs/ac3forge/pull/538) and this page's own PR were all
unmerged, each named the others by absolute branch URL, because `mkdocs build --strict` rejects a
relative link to a file not yet under `docs/`. Branch blob URLs 404 once the branch is deleted.
#537, #538 and #540 merged first, so this PR — the last of them — converted all of them to
relative paths in one change, here and in the two sibling pages.

Two things worth keeping from it, because the situation recurs whenever two documentation PRs
cross. **Whichever merges last owns the conversion**, and it has to be done deliberately:
`tools/checks/check_doc_paths.py` skips http(s) targets by design (its own docstring, line 13),
so nothing in CI catches a rotting branch URL. A narrow rule flagging
`blob/<ref>/` URLs where `<ref>` is not `main` would be syntactic and reliable, and is proposed
separately — it touches `tools/`, so it costs one matrix run for the PR that adds it and nothing
afterwards, since Script Lint (`ci.yml:434`) has no docs-only gate and already runs on every PR.

**The ESP32-S3 work is [PR #546](https://github.com/iainchesworthlabs/ac3forge/pull/546)**, opened after this page was first written. Its
`docs/platforms/esp32.md` is a fifth cross-branch link if this page ever cites it directly — it
does not today, and should not until one of the two lands.

**PF7's footprint table is being re-measured** in
[#540](https://github.com/iainchesworthlabs/ac3forge/pull/540), and [#546](https://github.com/iainchesworthlabs/ac3forge/pull/546) moves the same
numbers much further (the ARM image by 32%, `.bss` from 237,592 to 97,152). Those two PRs are
measuring the same table against different trees, and the sequencing between them is open — it
is the one coordination item on this page with a real chance of landing a wrong number on `main`.

## Deliberately not in scope

- **Inventing a protocol.** HLS and CMAF exist, are implemented here, and need nobody's
  agreement.
- **Becoming a Snapcast, Sendspin or Music Assistant *client*.** Each would mean receiving PCM,
  which puts the codec out of the loop and leaves no reason for the member to exist.
- **Video, or any A/V sync.**
- **A media library, catalogue or metadata store.** That is Music Assistant and it stays
  Music Assistant.
- **DRM, or any conditional-access story.**
- **Making the QC reporter a node.**
- **Renaming or moving anything.** No identifier, directory or package changes here.
- **Editing `ROADMAP.md` or `CHANGELOG.md`** — the roadmap entry is proposed as text in each
  plan, since no roadmap ID is allocated here.

## Decisions

1. **The transport.** (a) **HLS/CMAF over HTTP now, a Sendspin extension discussed once there is
   something to argue from**; (b) Sendspin first; (c) a protocol of our own; (d) HLS only, never
   Sendspin. **Taken 2026-09-07: (a).** Cost: two transports to support eventually, and the
   Sendspin half may be refused by its own project. The reason it is cheap now is that the
   source side is already shipped and unconnected.

2. **Is the sink one member or two?** (a) **one member covering Pi, PC and ESP32-S3, with the
   microcontroller as a platform of it**; (b) two — a desktop-class appliance and a separate
   embedded node; (c) the ESP32 node is a demo of the library, the way the Shield app is.
   **Recommend (a)**: it is one decoder, one client and one protocol, differing only in size,
   and the platform tree already expresses that split
   (`apps/baremetal/platform/{baremetal,host,esp32s3}`). Cost: one member's CI has to build for
   Xtensa, which nothing in the repository does today.

3. **Does Crucible gain a network output?** (a) **yes, in a later phase — it is a source and the
   transport is the point**; (b) no, Crucible stays a local-AVR application. **Recommend (a)**,
   because Crucible plus this transport is the one thing on the page with no competitor: live
   desktop audio as Atmos, to every room. Cost: a scope change to a shipped member, and its
   `OutputStage` gains a mode that is not a sink at all.

4. **Who serves the HTTP origin?** (a) **the source serves it itself** (`cpp-httplib`, already
   chosen for the appliance); (b) the source writes a folder and the user points a web server at
   it; (c) both. **Recommend (a)** with (b) documented, since (b) already works today and needs
   no code. Cost: every source gains a listener, and therefore a threat-model section.

5. **Default segment length.** (a) **choose it from Phase 3's measurements, one default for
   file playback and one for live**; (b) keep 48 frames everywhere; (c) go straight to LL-HLS.
   **Recommend (a)**. Cost: one phase ships before the defaults are right.

6. **When to approach the Sendspin project.** (a) **after Phase 2, with a working
   implementation**; (b) now, before building; (c) never. **Recommend (a)**: a protocol
   extension proposed without an implementation is a wish. Cost: if they are designing codec
   negotiation right now, we miss the window — worth watching their spec repository for that
   specifically.

7. **Does the WASM decode page become an HLS sink?** (a) **yes, nearly free — browsers already
   do HLS, and the decode module exists**; (b) no. **Recommend (a)** as a demo rather than a
   product: it makes the transport visible to anyone with a browser, which is the cheapest
   possible proof that the origin is standard. Cost: a page and a fetch loop.
