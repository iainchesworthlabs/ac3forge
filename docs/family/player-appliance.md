# A playback appliance: the player as a product

!!! note "Status: plan, written 2026-09-07. Nothing decided."
    This page plans a standalone player — the project's decode and passthrough path running on a
    small always-on machine with no keyboard, feeding an AV receiver over HDMI or S/PDIF. It
    keeps the shape of [the recasting plan](recasting.md) and
    [the promotion plan](../crucible/promotion.md): design sections say what changes and why,
    each phase carries an exit criterion and says how it is verified,
    [Decisions](#decisions) lists what only the owner can decide with an option taken for each,
    and [What cannot be verified, and why](#what-cannot-be-verified-and-why) says where the
    evidence runs out. **The name in particular is not chosen here** — [The name](#the-name)
    recommends one and prices the alternatives, the way Crucible's name was chosen from four.

    Every identifier below is written as `ac3hearth` / `ac3::hearth` because a plan needs
    something to write. Substitute whatever [decision 1](#decisions) settles.

The pieces of a player already exist and none of them is a product.

`ac3cli play` (`apps/cli/commands/audio_io.cpp:721-849`) opens a `.ac3`, `.ec3`, `.mkv`, `.mp4`
or `.ts`, works out whether the chosen endpoint takes the bitstream, and streams IEC 61937
bursts to it — and, when the sink refuses, transcodes E-AC-3 to AC-3 or falls back to decoded
PCM. That path has been run against a real Atmos-capable receiver: a Raspberry Pi 4B over HDMI,
every stream shape locking correctly at zero underruns, including signed Atmos with four height
channels ([Raspberry Pi](../platforms/raspberry-pi.md#live-hdmi-passthrough-to-a-real-receiver),
2026-08-20). The GUI has a player too — `StreamPlayerController`
(`apps/gui/stream_player_controller.hpp`) — with transport, seeking, metering and export, but
it decodes to PCM and plays through a shared-mode `MonitorSink`, and it lives inside a window on
a workstation.

So the passthrough half is proven and has no product around it, the transport half is a product
and does not passthrough, and the Pi in the project is a test box. This page plans the thing
that would join them: a machine you put next to a receiver, plug in, and never touch again.

## The name

The family's naming is settled and this inherits it. "AC3Forge" is the family in prose,
`ac3forge` in identifiers, and each member takes one capitalised word of its own — Forge,
Crucible ([recasting](recasting.md#the-name), decisions 2 and 3). A fourth member takes a fourth
such word. Crucible's own name was chosen from four options on 2026-09-04; these are this one's.

The metaphor the family already uses is the smithy. Forge is the workshop, Crucible is the
vessel where things are melted and made live. A playback appliance is the fixed, always-warm
thing that sits in the room where the work is heard.

| Option | Identifiers | Why it fits | What it costs |
|---|---|---|---|
| **Hearth, recommended** | `ac3hearth`, `ac3::hearth`, `AC3FORGE_BUILD_HEARTH`, `ac3forge-hearth` | The forge's own fire, and the fire in the room the appliance stands in. One syllable more than Forge, same register as Crucible. An always-on box that is never switched off is a hearth in both senses at once. | Hearth Display, Inc. (Brooklyn) holds a WIPO Class 9 mark covering a **wall-mounted always-on household display appliance** — an adjacent product category, not an adjacent goods description, and the closest collision of the four. PyPI, npm and crates.io each carry a small unrelated `hearth`. Debian, Fedora and Homebrew are free. |
| Anvil | `ac3anvil`, `ac3::anvil` | The smithy's fixed object — the thing that does not move while everything else does. | Foundry (the Ethereum toolkit) installs four binaries: `forge`, `cast`, `anvil` and `chisel`. The family would then collide with the *same* third-party toolkit twice on a developer's `PATH`, having already accepted the `forge` half as a display name only. Anvil.works is an established Python platform. PyPI, npm and crates.io taken. Debian, Fedora, Homebrew free. |
| Ember | `ac3ember`, `ac3::ember` | Small, quiet, always warm — what the box is. | Ember.js owns the word in software; `ember` on npm *is* the framework (1.1.1), and Debian shipped an `ember` source package through stretch. PyPI and crates.io taken. The one option where the collision is with something every web developer has heard of. |
| AC3Forge Player | `ac3player`, `ac3::player` | No metaphor to defend, and it says what it is on a shelf of unrelated boxes. | Unsearchable — "ac3forge player" and "the player page in the GUI" are the same words. Gives the member no identity of its own, and breaks the one-word convention Forge and Crucible set. The only option with no registry risk at all, because nobody would ever publish a bare `player`. |

**Availability, checked by hand on 2026-09-07.** Every registry below was queried directly; the
result is what the registry returned, not an inference.

| Name | PyPI | npm | crates.io | Homebrew | Debian (source) | Fedora | winget |
|---|---|---|---|---|---|---|---|
| `hearth` | taken — 0.5.0, PyTorch tooling | taken — 1.0.2, JSON test-data generator | taken — last touched 2023-01-24 | free (no formula, no cask) | free (404) | free (no exact match) | no publisher `Hearth` |
| `anvil` | taken — 0.33.2, cloud task runner | taken — 0.0.6, interactive-tools library | taken — 7 versions, last 2025-06 | free | free (404) | free (no exact match) | no publisher `Anvil` |
| `ember` | taken — 0.0.1-dev, DRF/Ember.js glue | taken — 1.1.1, **Ember.js itself** | taken — last 2025-07 | free | **taken** — 0.7.2+dfsg-1, through stretch | free (no exact match) | no publisher `Ember` |

Two things that check did establish, beyond the table. **No on-theme single word is free across
all six registries** — `mantel`, `bellows`, `stoker`, `cinder`, `quench`, `smithy`, `trivet`,
`billet` and `kindling` were each queried too, and every one of them is taken on at least PyPI
or crates.io. And **it does not need to be**: under [recasting](recasting.md#decisions) decision
5, every published token in this family carries the `ac3forge` prefix, so what would actually be
published is `ac3forge-hearth` (DEB, RPM, archive) and `iainchesworthlabs.ac3forge-hearth`
(winget, under a publisher namespace the project already owns). Those cannot collide by
construction. The bare-word column is a trademark and confusion check, not a blocker.

What the check could **not** establish is recorded in
[What cannot be verified](#what-cannot-be-verified-and-why): winget's `Moniker` field is a soft
alias that is neither namespaced nor enforced, and there is no public index of monikers to query.

Under the recommended option the three naming rules
([recasting](recasting.md#the-name)) extend by one line, written once on the new member's index
page: **Hearth**, capitalised and standing alone, names the appliance; `ac3hearth` and
`ac3::hearth` are its identifiers; "AC3Forge Hearth" is written the way "AC3Forge Crucible" is.

## Which member it belongs to

Three placements are possible and the plan takes the most expensive one, so the argument has to
carry its own weight.

**Not Forge.** Forge is the tooling over the library — `ac3cli`, `ac3gui`, `apps/common` — for a
person sitting at a workstation authoring and inspecting streams. The install paths, the docs
tabs and the `runtime` component all draw that boundary already. An appliance has no operator at
the machine, no window, and a lifecycle (a service that starts at boot and never exits) that
nothing in `runtime` has. Folding it in would put a systemd unit and a network listener into the
package that today contains two interactive binaries, and would give the appliance's
documentation nowhere to live but the CLI reference.

**Not Crucible.** Crucible already owns the pattern this needs, which is what makes it
tempting: a pure output policy that names a mode, an endpoint and a reason
(`apps/crucible/engine/output_policy.hpp`), a platform layer that acts on it
(`output_stage.hpp`), a headless runner over the same engine (`ac3crucible-run`,
`apps/crucible/runner/main.cpp`), and fakes that let the whole table of cases run on a CI leg
with no sound card (`tests/crucible/fake_devices.hpp`). But look at what Crucible's engine
actually is: a session monitor, a tap pool, a slot allocator, a bed mixer and placement
smoothing, all in service of capturing *other applications* and placing them as objects in a
room. An appliance captures nothing and places nothing. Of the twelve files under
`apps/crucible/engine/`, two — the policy and the output stage — are what a player would want,
and even those carry Windows rules an appliance does not have (the null-sink substring, and the
"never a bitstream mode on the endpoint applications are rendering to" rule at
`output_policy.hpp:19-24`, which exists because Crucible shares a machine with other audio and
an appliance does not).

Making the appliance a Crucible mode would also make Crucible's one-sentence description false.
It currently reads: the application that makes every application playing sound an Atmos object
the listener places in a room. A file player is not that.

**A fourth member, recommended.** [Recasting](recasting.md#deliberately-not-in-scope) ruled a
fourth member out of *its own* scope, which is not the same as ruling one out. Its working test
for membership is visible in its own "What each member owns" table: a member has its own source
directory, its own build identity and option, its own tests and labels, its own docs section,
its own packages, and its own settings identity. An appliance qualifies on all six, and does so
because of what it *is*, not because a plan says so:

- **Source.** `apps/hearth/` — a daemon, a control surface, a source/queue layer. Nothing in
  `apps/cli`, `apps/gui` or `apps/crucible` is any of those.
- **Build identity.** Its own target, option and component, because it must be buildable and
  packageable without Qt, without the GUI, and without Crucible's PipeWire tap.
- **Tests.** Its own label, and its own fakes — an appliance's fake is a *sink that refuses a
  format*, not Crucible's *tap that synthesises a tone per process*.
- **Docs.** A first-run story for a device with no keyboard, which is most of its documentation
  and belongs to nothing that exists.
- **Packages.** A DEB that installs a systemd unit and a service user. No existing component does.
- **Settings.** A file on disk read by a service at boot, not a `QSettings` store read by a
  window.

The cost of a fourth member is precise, and it is a documentation cost rather than a code one:
`docs/index.md:39` "The three members" becomes four, the README's three-row table gains a row,
[recasting](recasting.md)'s own model section gains a member and its "Deliberately not in scope"
bullet on a fourth member is superseded by name, `CONTRIBUTING.md:50-51`'s consumer list gains
`hearth`, and the seven-tab nav becomes eight. That is about six files and no identifier anyone
has installed. [Decision 2](#decisions) is where it is taken.

What the appliance **shares** rather than owns is the family's floor: `src/audio` (never
installed, `cmake/InstallLibrary.cmake:4-7`), the library itself, the single `ac3tests` binary,
CI and the version line — exactly as Crucible does.

## Scope

**What it does.**

- Plays AC-3, E-AC-3 and Atmos/JOC elementary streams and the containers the CLI already sniffs
  (`.mkv`, `.mp4`, `.ts`) to an HDMI or S/PDIF sink as IEC 61937 bursts, bit-exact, with no
  re-encode.
- Follows the sink: reads what the endpoint accepts, and when it will not take the bitstream,
  transcodes E-AC-3 to AC-3 or falls back to decoded PCM — roadmap UX9's two legs, with the
  gaps in [What UX9 needs](#what-ux9-needs-before-it-can-carry-this) closed first.
- Re-follows it. A receiver switched to another input, powered off, or replaced is a hot-plug
  event, and the appliance re-probes and re-decides rather than dying or streaming into
  nothing.
- Holds a queue: a list of files or directories, played in order, with transport (play, pause,
  next, previous, seek) and gapless-where-possible behaviour across items of the same format.
- Serves a control page on the local network, reachable from a phone, that says what is playing,
  what format was negotiated, and why — and lets someone change any of it.
- Starts at boot, survives a receiver that is off, and logs enough that a failure at 11pm is
  diagnosable at 9am.
- Reports its own state: negotiated format, underrun count, endpoint name, the reason the mode
  was chosen — the same `reason` string discipline Crucible's `OutputStatus` already keeps.

**What it does not do.** Each of these is a deliberate boundary, not a gap to fill later unless
this list is revised.

- **No video.** No player, no sync, no HDMI video output beyond whatever the OS does to keep the
  link up. This is an audio appliance.
- **No library management.** No metadata scraping, no cover art, no tag database, no
  transcoding library. It plays a path.
- **No streaming-service clients.** No Spotify, no AirPlay, no Chromecast, no DLNA renderer.
  Each is a protocol with its own certification and, in most cases, a licence the project cannot
  accept.
- **No encoding of live inputs.** That is Crucible.
- **No room correction, no renderer, no binaural fold.** Already
  [deliberately not on the roadmap](../roadmap.md); the receiver renders.
- **No mixing.** One stream at a time to one sink.
- **No cloud, no account, no telemetry.** It has no outbound network need at all, and
  [Phase 4](#phase-4-the-appliance-shape) makes that an assertion rather than a claim.
- **No authentication in v1**, and therefore no exposure beyond a local interface — see
  [The control surface](#the-control-surface).
- **Not a Windows or macOS product in v1** — see [Platforms](#platforms).

## What it builds on, by path

| Path | What it gives | What is missing for an appliance |
|---|---|---|
| `apps/cli/commands/audio_io.cpp:721-849` (`run_play`) | The whole passthrough decision and submit loop, proven on real hardware | Reads the entire file into memory (`read_elementary_stream`), splits every unit up front, then blocks in one loop until done. No transport, no queue, no cancellation, no re-follow. A five-minute Atmos programme is fine; an appliance left running is not. |
| `apps/cli/commands/audio_io.cpp:682-720` (`play_via_ac3_transcode`) | UX9's transcode-to-passthrough leg, metadata carried across | Goes through a **temp file**: the whole stream is transcoded to disk before a note is heard. On an SD-card appliance that is a write-amplification and latency problem both. Needs to become a streaming transcode. |
| `apps/gui/stream_player_controller.{cpp,hpp}` | Transport, seek, position reporting, level publication, the worker-thread and shared-result ownership pattern (see its own header comment on why `result_` is a `shared_ptr`) | Decodes the whole file to memory and plays PCM through `MonitorSink`. It is a *monitor*, not a passthrough player, and it is Qt. The transport *state machine* is what transfers; none of the code does. |
| `src/audio/include/ac3/audio/passthrough.hpp` | `PassthroughSink`, `enumerate_render_devices()`, the live AC-3/E-AC-3/exclusive-PCM probe | Nothing missing; this is the load-bearing piece and it works. |
| `src/audio/include/ac3/audio/sink_capabilities.hpp` + `src/backend/*/sink_capabilities.cpp` | UX9's EDID/ELD read | Real on **ALSA only** (84 lines, reading `/proc/asound/<card>/eld#*`). PipeWire, WASAPI, CoreAudio and posix each return `kNoBackend`, by name. See [What UX9 needs](#what-ux9-needs-before-it-can-carry-this). |
| `src/audio/include/ac3/audio/device_watcher.hpp` | Endpoint added / removed / state-changed / default-changed, on Windows, PipeWire and CoreAudio | **Nothing calls it from `play`.** It was written for Crucible (UX11). ALSA has no such API — that is udev's job — which matters, because ALSA is one of the appliance's two Linux backends. |
| `src/forge/include/ac3/iec61937/iec61937.hpp` | `wrap_frame`, `Eac3BurstPacker`, `BurstReader`, `PassthroughDetector`, the 6144/24576-byte burst constants | Nothing missing. |
| `apps/crucible/engine/output_policy.{cpp,hpp}` | The pattern: a **pure** decision over gathered facts returning a mode, an endpoint and a one-line reason — which is what lets the whole case table run on every CI leg | The policy itself does not transfer (null sink, default-endpoint exclusion, object modes). The *shape* is the thing to copy, and the plan copies it exactly. |
| `apps/crucible/engine/output_stage.{cpp,hpp}` | Owning whichever sink the mode means, routing units to it, switching without disturbing upstream, counting underruns | Built around `RawFrame` (objects + placements + bed) from a live encoder. An appliance's unit is an already-encoded access unit off disk. |
| `apps/crucible/runner/main.cpp` (`ac3crucible-run`) | The precedent for a headless binary over the same engine, driven by a line protocol, with a `status` verb | It reads stdin. An appliance has no stdin after boot. |
| `apps/common/container_input.{cpp,hpp}` | `sniff_container` / `elementary_stream_from_bytes` — what `decode`, `qc`, `levels`, `play`, `monitor` and both GUI pickers all call | Whole-buffer. An appliance wants an incremental reader for large files, though whole-buffer is acceptable for v1 on a 2 GB Pi given typical elementary-stream sizes. |
| `src/audio/src/net/udp_socket.hpp` + `net/{posix,windows}/` | **An in-tree socket layer on its own OS axis**, already justified in that header's own comment (sockets are the operating system, not the audio subsystem) | UDP only, receive-oriented. A TCP listener belongs beside it on the same axis — this is why the control surface needs no new dependency. |
| `src/audio/include/ac3/audio/live_positions.hpp` (UX4) | A working control-surface precedent: a socket and thread owning an OS resource, feeding a lock-free-ish snapshot, with counters for a status line, and an OSC 1.0 parser that is pure and lives in the library | It drives object positions in an encoder, not transport in a player. The *architecture* is directly reusable and the plan reuses it. |
| `apps/crucible/notices/`, `apps/notices/` | Per-platform composed notices, one small file per OS | A new component needs its own `notices.cmake` and platform directory. |
| `cmake/Packaging.cmake:300-317,399-401,463-464,534-537` | The exact recipe for a second application component with its own DEB/RPM names, its own `Depends`, and an archive name that is not CPack's default suffix | A third component follows it line for line. |

**What is missing today that has no home yet at all**, beyond the per-row gaps above:

1. **A daemon lifecycle.** Nothing in the tree starts at boot, reloads configuration, or reports
   readiness to an init system.
2. **A configuration file.** Every existing surface is argv (`ac3cli`), `QSettings` (`ac3gui`,
   Crucible) or stdin (`ac3crucible-run`). A service needs a file, and a control page needs to
   write it back.
3. **A queue.** No component in the tree holds an ordered list of things to play.
4. **A TCP listener and an HTTP/1.1 subset.** See [The control surface](#the-control-surface).
5. **Service discovery.** A keyboardless box's first-run problem is "what URL do I type", and
   nothing in the tree answers it.

## The form

Four forms were considered. The recommendation splits them: one now, one later, two never.

| Form | What it is | Verdict |
|---|---|---|
| **A package that turns an existing Linux install into an appliance** | `ac3forge-hearth` DEB/RPM: one binary, a systemd unit, a service user, a default config, a control page. `apt install`, `systemctl enable`, done. | **Recommended for v1.** It rides packaging and CI the project already has — `linux-gcc-arm64` is already `packageable` and `release_package`, and the arm64 `.deb` has already been inspected on a Pi ([Raspberry Pi](../platforms/raspberry-pi.md#packaging)). It adds no distribution form, no hosting, no OS to maintain, and it says what it is: the appliance is software, and the user brought the hardware. |
| **A headless service with a control surface** | The runtime shape, orthogonal to how it is delivered | **Recommended**, and it is what the package installs. See [The control surface](#the-control-surface). |
| **A kiosk QML application** | `ac3crucible`'s window, full-screen, no chrome, driven by a remote | **Not recommended for v1.** It requires a display attached to the appliance, which the product premise says there is not; it drags in Qt Quick, Quick Controls, a compositor and a GPU stack onto a 2 GB Pi that must also stream bursts without underrunning; and it puts the interface on the *television*, which is the one screen that is showing something else. A ten-foot interface is a later, separate product decision — [decision 4](#decisions) records it as such. |
| **An SD-card image** | A bootable Raspberry Pi OS image with the appliance pre-installed | **Not recommended for v1; a candidate for a later phase.** Fully costed in [Packaging](#the-sd-card-image-costed-not-recommended-for-v1), because it is a distribution form this project has never had and the costs are not obvious. |

## The control surface

Four candidates. The recommendation is a **local web control page over an in-tree HTTP/1.1
subset**, with OSC retained as a second, already-precedented surface.

| Surface | For | Against |
|---|---|---|
| **A web control page, recommended** | Every phone, tablet and laptop on the network already has the client. No app store, no signing, no install, no second codebase to ship. Works on the day the box is unboxed. The page is static assets compiled into the binary, so there is no web server to configure and no document root to get wrong. | Needs a TCP listener and enough HTTP/1.1 to serve a few files and a JSON API — new code, though see below on why it is not a new dependency. Needs the user to find the URL ([Identity assets](#identity-assets)). |
| A phone app | Native controls, offline, push | Two app stores, two signing identities (and DR6 is unresolved for the *desktop* ones), two toolchains, and a release cadence tied to review queues. For a control surface with roughly eight controls this is not proportionate. |
| A physical remote over HDMI-CEC | No second device. The remote is already in the room and pointed at the television. | CEC is the least reliable part of consumer HDMI; `libcec` is LGPL-2.1-or-later (compatible, but a new dependency); the Pi's CEC support depends on the vc4 driver and the receiver's willingness to forward keys; and the appliance is not the active source, so it may never receive them. Worth adding **later**, as a convenience over the same command layer, never as the only surface. |
| An OSC/HTTP API with no page | The project already has OSC: `ac3::audio::LivePositionSource` over `src/audio/src/net/udp_socket.hpp`, with a pure OSC 1.0 parser in the library (UX4). Cheapest possible surface. | An API is not a first-run story. Somebody with a new box and no keyboard cannot send an OSC packet. It is a good *second* surface for automation, and the plan keeps it as one. |

**Why the web page needs no new dependency.** The project's entire vcpkg dependency list is
`catch2` and `fmt`. That budget is a deliberate property of the codebase and this plan does not
spend it. `src/audio/src/net/` already establishes exactly the right precedent: a socket
abstraction on its own **operating-system** axis (`posix/`, `windows/`) rather than on the audio
backend axis, with its own written reasoning for why the split falls there. A `TcpListener`
belongs beside `UdpSocket` on that same axis. Over it, the appliance needs: request-line and
header parsing, `GET` and `POST`, `Content-Length` bodies, static responses from a compiled-in
asset table, and JSON — for which the responses are generated (so `fmt` is enough) and the
requests are a handful of scalar fields (so a fifty-line reader is enough, and is testable
without a socket).

That is a meaningful amount of code, and its scope has to be stated: an HTTP **subset for a
LAN control page**, not a web server. It never serves user-supplied paths, never proxies, and
never accepts a request body larger than a fixed cap. [Threat model](../threat-model.md) gains a
section, and [Phase 3](#phase-3-the-control-surface) makes the parser a fuzz target — `fuzz.yml`
already exists and the appliance's request parser is exactly the shape it is good at.

**The API.** `GET /api/status` (what is playing, the negotiated format, the endpoint, the
reason, underruns), `GET /api/queue`, `POST /api/transport`, `POST /api/queue`,
`POST /api/settings`, `GET /api/outputs`. The page is these calls and nothing else, so the OSC
and any future CEC surface drive the same command layer rather than a parallel one.

**Binding and exposure.** Default bind is the local network interface, port configurable,
**no authentication in v1** — and therefore the default configuration and the documentation both
state plainly that this control page must not be exposed to the internet. That is the same
posture every consumer media appliance takes on a home LAN, it is stated rather than assumed,
and [decision 6](#decisions) is where a different posture would be taken.

## Platforms

**Linux, x86_64 and arm64, ALSA and PipeWire. Recommended for v1.** The argument is DR9's own
evidence and nothing else:

- **ALSA on real HDMI hardware: confirmed.** A Pi 4B drove an Atmos-capable AVR through
  `ac3cli play`, every stream shape locking, zero underruns
  ([Raspberry Pi](../platforms/raspberry-pi.md#live-hdmi-passthrough-to-a-real-receiver)).
- **PipeWire on real HDMI hardware: confirmed 2026-09-05.** The receiver's own front panel read
  "5.1 DD+" and "Atmos/DD+" at 7.1 ([DR9](../roadmap.md)).
- **Windows/WASAPI exclusive passthrough: unconfirmed.** DR9 says only a Realtek analogue
  endpoint has ever been tried. An appliance is a passthrough product; shipping one on a backend
  whose passthrough has never locked a receiver would be shipping the unverified part as the
  whole product.
- **CoreAudio: blocked.** No Mac has run any of it.

There is a second reason, independent of evidence: an always-on headless appliance running
Windows or macOS is not a thing people build. The hardware is Linux hardware.

**Windows and macOS come later rather than never.** The daemon core is written platform-free
from the start, exactly as `apps/crucible/engine/`'s pure half is, so it compiles into
`ac3tests` on all eleven legs from Phase 1. What is Linux-only is the service integration
(systemd), the packaging and the product claim. If DR9's Windows row ever closes, a Windows
service is a platform directory and a packaging component, not a rewrite. That is
[decision 3](#decisions).

**Does it share the Crucible engine?** No, and the reason is above under
[Which member](#which-member-it-belongs-to). What it shares is one thing, and the plan makes
that sharing explicit rather than duplicating it:

> **The "what does this sink take" question moves to the shared floor.** Today it is answered in
> two places that do not know about each other: `run_play` (`audio_io.cpp:783-806`, EDID first,
> live probe on `kNoBackend`) and Crucible's `output_policy` (`EndpointFacts`, filled by its
> platform layer). A third copy in the appliance is where drift starts. The recommendation is a
> small `ac3::audio` helper — facts gathered from `read_sink_capabilities()` with
> `enumerate_render_devices()` as the documented fallback, returned as one struct — that all
> three call. It sits in `src/audio`, which is already named as the family's shared floor
> ([recasting](recasting.md#the-model)), it is pure above the backend seam, and it is where the
> fallback rule is currently written as a comment in a CLI command.

## What UX9 needs before it can carry this

UX9 shipped, and its own roadmap entry states its limit: it was "not verified against real
EDID/ELD hardware this round". Since then the ALSA path *has* been verified on a Pi. Six things
stand between it and carrying an appliance, in the order they bite.

1. **The default endpoint is never probed.** `run_play` takes the branch at
   `audio_io.cpp:792` only when `chosen != nullptr`; with no device index it sets
   `takes_native = true` and streams. The comment says so plainly — "the default endpoint is
   taken at its word". An appliance's normal configuration is *no device index*, so the whole
   sink-following feature is off by default in exactly the case the appliance ships in. This is
   the single most important fix and it is small: resolve the default endpoint to its id, then
   take the same path every named endpoint takes.

2. **There is no PipeWire capability read.** `src/audio/src/backend/pipewire/sink_capabilities.cpp`
   returns `kNoBackend`, and its comment gives a good reason: the property names that would map
   a node back to `/proc/asound/<card>/eld#*` are not confirmed stable, and guessing would hand
   a caller a confidently wrong answer. DR9 has since supplied the missing confirmation from the
   other direction — WirePlumber sets `iec958.codecs` on the node **from the ELD**, and both
   enumeration and `start()` are now gated on it. That leaves an implementation the comment's
   objection does not reach: report what `iec958.codecs` says, which is the ELD's own answer
   arriving through a supported property, rather than the `api.alsa.card` mapping the comment
   declines to guess at. It is a capability read the appliance can trust on its primary
   backend.

3. **`play` never re-follows.** Capabilities are read once, before the sink is started, and
   never again. A receiver switched to another input, powered down, or swapped mid-programme
   leaves the appliance streaming into a sink whose answer has changed.
   `ac3::audio::DeviceWatcher` already exists and already reports exactly these events on
   PipeWire — nothing calls it from `play`. **And on ALSA there is no watcher at all**, by
   design (that is udev's job), so an ALSA-backed appliance needs either a udev subscription or
   a periodic re-probe. The plan takes the re-probe on a timer for ALSA and the watcher on
   PipeWire, with the same re-decide path behind both.

4. **The transcode leg goes through a temp file.** `play_via_ac3_transcode` transcodes the whole
   stream to `std::filesystem::temp_directory_path()` and then plays that file. For an appliance
   this is three problems: nothing is heard until the whole transcode finishes, an SD card takes
   a full-programme write per play, and a stream that never ends cannot be transcoded this way
   at all. It needs to become a streaming transcode feeding the sink directly.

5. **EDID is read once, at start.** Tied to (3): the re-decide path must re-read, not reuse.

6. **Nothing distinguishes "no descriptor" from "no backend" to a user.** `EdidError` already
   separates `kNoEdid` (nothing plugged in downstream — "a real, expected outcome") from
   `kNoBackend` (this platform cannot look). The CLI collapses both into one `note:` line. The
   appliance's control page must show the difference, because "your receiver is off" and "this
   machine cannot read EDID" are different problems for the person holding the phone.

Items 1, 3, 5 and 6 are appliance-blocking and small. Item 2 is appliance-blocking on PipeWire
and medium. Item 4 is a quality bar rather than a blocker — an appliance can ship refusing the
transcode leg and saying why, which is what `follow=off` already does.

## Build identity

Following `CMakeLists.txt:136-138,457` and `cmake/Packaging.cmake` exactly as Crucible does.

| Thing | Value | Precedent |
|---|---|---|
| Directory | `apps/hearth/` with `core/`, `net/`, `platform/`, `web/` | `apps/crucible/{engine,ui,runner,platform}` |
| Library target | `ac3hearth_core` (STATIC), alias `ac3::hearth_core` | `ac3crucible_engine` / `ac3::crucible_engine` (`apps/crucible/CMakeLists.txt:21,32`) |
| Executable | `ac3hearth` (the daemon) | `ac3crucible`, `ac3crucible-run` (:252,380) |
| Namespace | `ac3::hearth` | `ac3::crucible` |
| Option | `option(AC3FORGE_BUILD_HEARTH "Build AC3Forge Hearth (the playback appliance; Linux)" OFF)` | `AC3FORGE_BUILD_CRUCIBLE` (`CMakeLists.txt:138`), default OFF for the same reason |
| Root guard | `if(AC3FORGE_BUILD_HEARTH AND LINUX)` | `if(AC3FORGE_BUILD_CRUCIBLE AND (WIN32 OR APPLE OR LINUX))` (:457) — the guard names platforms rather than being dropped, so a platform with no arm fails at configure rather than at link |
| QML URI | none in v1 | it has no window; reserve `Ac3ForgeHearth` if [decision 4](#decisions) ever adds a local screen |
| CPack component | `hearth` | `crucible` (`cmake/Packaging.cmake:463-464`) |
| Configure summary | one line, `Build Hearth   : ${AC3FORGE_BUILD_HEARTH}` | `CMakeLists.txt:523-524`, which the recasting plan already notes omits Crucible |
| Config file | `/etc/ac3forge/hearth.toml` (or `.conf`) | new; `QSettings` is not available to a Qt-free daemon |
| systemd unit | `ac3hearth.service`, installed to `${CMAKE_INSTALL_PREFIX}/lib/systemd/system` | new |
| Service user | `ac3hearth`, in `audio`, created by the DEB/RPM post-install | new |

`ac3hearth_core` links `ac3::forge`, `ac3::audio` and `ac3::fmt` and **nothing else** — no Qt,
no PipeWire headers, no socket. The daemon links the core plus the platform half. That split is
what makes the next section possible.

## Tests

**It joins `ac3tests`.** There is one test binary (`tests/CMakeLists.txt`) and this does not
change that — [recasting](recasting.md#deliberately-not-in-scope) rules out splitting it, and
the reason applies here: no gate would key on anything but source paths.

**How it compiles everywhere.** `tests/crucible/` is compiled into `ac3tests` **ungated, on
every platform**, because the engine's pure half is plain C++ over `ac3::oba` types with no
platform in it (`tests/CMakeLists.txt:440-448`). `apps/hearth/core/` follows exactly that rule:
the queue, the transport state machine, the format decision, the configuration parser, the HTTP
request parser and the JSON reader/writer are all pure, so `tests/hearth/` compiles into
`ac3tests` on all eleven legs — including the ones that will never build the daemon.

**Labels.** `catch_discover_tests(ac3tests ADD_TAGS_AS_LABELS)` (`tests/CMakeLists.txt:617`)
turns each case's Catch2 tags into ctest labels, so the tags *are* the labels:

- `[hearth]` — the pure core, everywhere.
- `[hearth-net]` — the listener and the request parser against a loopback socket. Runs
  everywhere a socket can bind, which on hosted runners is everywhere.
- `[hearth-device]` — anything that touches a real endpoint. Skipped where there is none, the
  way the existing device tests are.

**How the audio is faked.** Hosted CI runners have no sound, and the precedent for this is
`tests/crucible/fake_devices.hpp`: an in-memory `AudioDevices` with a scripted endpoint list and
sinks that record what reached them, everything `shared_ptr`'d so a test keeps a handle after
the fact, thread-safe where two threads touch it. `tests/hearth/fake_sink.hpp` is the same idea
with a different script, because the appliance's interesting cases are refusals:

| Fake | The case it makes testable |
|---|---|
| A sink that reports EDID with `eac3=false, ac3=true` | the transcode-to-AC-3 leg |
| A sink that reports `kNoEdid` | receiver off — a real, expected outcome, and a distinct message |
| A sink that reports `kNoBackend` | the live-probe fallback, on a platform that cannot read |
| A sink that accepts nothing | the refusal, and the reason string |
| A sink that disappears mid-stream | re-follow, and that the queue survives it |
| A sink that refuses N submits then accepts | underrun counting — `FakeBurstSink`'s `refuse_submits` already does exactly this |
| A capability read whose answer *changes* between probes | re-decide, the case a static fake cannot reach |

**A platform probe.** `tools/checks/crucible_platform_probe.cpp` exists so a CI leg can assert
which platform half got compiled in. `tools/checks/hearth_platform_probe.cpp` does the same for
the backend and the socket axis, so a leg that meant to build the PipeWire arm and silently got
the ALSA one fails loudly.

**Coverage floor.** `tools/checks/coverage_report.sh:118-129` holds per-component line/branch
floors, and its own comment explains the two low ones: `src/audio` sits at 25/15 and
`apps/cli` at 40/34 because device paths never execute headless — "the floor holds the line
while that is true; raising it is a matter of writing the missing tests, not of editing this
table."

An `apps/hearth` row belongs in that table, and the plan deliberately does **not** guess its
number. The existing floors were set from measurement and then held; this one should be too. The
recommendation is: land the row in the phase that lands the code, set from the first green
measurement with the margin the other rows carry, and **design toward 55/45** — meaningfully
higher than `apps/cli`'s 40/34, because the appliance's split is deliberate. The queue, the
transport state machine, the format decision, the config parser, the request parser and the JSON
layer are all pure and have no excuse; only `apps/hearth/platform/` has the headless problem,
and it is a small fraction of the lines rather than `apps/cli`'s ~15%.

## CI

`_build.yml` is eleven matrix legs plus eight standalone jobs; `ci.yml` aggregates 22 jobs behind
`CI Status`. **The job names behind `CI Status` are branch-protection contracts**
(`.github/branch-protection.md:27-31`, alongside `Branch Name` and `Scan dependency diff`), so
this plan adds no job and renames none — a new leg or a renamed one would need the rule edited,
and `CI Status` exists precisely so that never has to happen.

What it adds is a flag on legs that already run, matching how `crucible: true` was added
(`_build.yml:329,342,430,487,590,617`).

| Leg | `hearth: true`? | Why |
|---|---|---|
| linux-gcc-arm64 (`_build.yml:440-447`) | **yes** | The product architecture. Already `gui`, `packageable` **and** `release_package: true`, so its packages already ride the release route. |
| linux-llvm (`:416-430`) | **yes** | x86_64, the other compiler, and already the leg with the PipeWire pass and `crucible: true`. |
| linux-gcc, linux-llvm-arm64, the two sanitiser legs, the two Windows legs, windows-msvc-arm64, the two macOS legs | no | `apps/hearth/core/` compiles into `ac3tests` on **all eleven** regardless, ungated, so every leg — including the ASan/UBSan and TSan legs — already exercises the pure half. TSan matters here: the appliance is a daemon with a listener thread, a playback thread and a watcher thread, and that leg is where a data race in the transport state machine is caught. |

**Flags.** Both new-flagged legs configure a second build tree the way the Crucible pass does
(`_build.yml:1659-1663`): `-DAC3FORGE_BUILD_HEARTH=ON -DAC3FORGE_BUILD_TESTS=ON`, with the
backend pinned per leg — `-DAC3FORGE_WITH_PIPEWIRE=ON -DAC3FORGE_WITH_ALSA=OFF` on one and the
reverse on the other, so **both backends are built and tested every run** rather than whichever
the runner happened to have headers for. The Crucible pass then greps its own configure log to
assert the backend it meant to get (`:1664-1670`); the appliance does the same, which is what
`hearth_platform_probe.cpp` above is for.

**Cost in matrix time.** A second configure, a build of one static library and one small binary,
`ctest -L hearth -L hearth-net`, and `cpack` for one component — on two legs. Measured against
the Crucible pass, which does all of that *plus* Qt, five Qt Quick suites and a window: this is
the cheaper half of it. Neither leg gains a runner, and the eleven-leg matrix stays eleven.

**Does an arm64 leg carry it?** Yes, and that is not optional. `linux-gcc-arm64` is the leg whose
architecture the product ships on. The Crucible plan learned this the expensive way: it ran
x86_64-only until 2026-09-06, so "an aarch64-only compilation fault would have reached a user"
(`_build.yml:465-473`). An appliance whose only real hardware is a Pi cannot repeat that.

**The docs-only fast path.** `ci.yml:311` classifies a PR touching only `docs/`, `*.md` and
`mkdocs.yml` as docs-only: it runs the strict docs build and skips the matrix. This page is
inside that set. Every later phase is not.

## Packaging and release identity

Following `cmake/Packaging.cmake` line for line where Crucible established the recipe.

| Identity | Value | Line it follows |
|---|---|---|
| Component | `hearth` | `:463-464` (`list(APPEND CPACK_COMPONENTS_ALL crucible)`) |
| Archive | `ac3forge-hearth-${PROJECT_VERSION_FULL}-${CPACK_SYSTEM_NAME}.tar.gz` | `:534-537` — named for what it is rather than taking CPack's `-hearth` suffix on the base name |
| DEB | `ac3forge-hearth`, section `sound`, `DEB-DEFAULT` file name | `:300-307` |
| DEB `Depends` | `libasound2` **or** `pipewire, wireplumber \| pipewire-media-session`, per the backend the leg built; plus `adduser` for the service user | `:308` (Crucible's PipeWire-only set) |
| RPM | `ac3forge-hearth`, `RPM-DEFAULT` | `:399-401` |
| Component description | names the member, per [recasting](recasting.md) Phase 6 | `:183-191` — and inherits the known cosmetic gap that every DEB component's one-line synopsis is the library's `PROJECT_DESCRIPTION`, so `apt show ac3forge-hearth` will open with "Clean-room AC-3 encoder" until CPack offers a per-component override that takes effect |
| Version style | `PROJECT_VERSION_FULL` with the prerelease suffix | `:363-365` — the `crucible` and `dev` style, not the runtime's `M.m.p`; [recasting decision 13](recasting.md#decisions) settled that the two styles are deliberate |
| Release artifact | uploaded as `packages-hearth-<preset>` | `_build.yml:1768` (`packages-crucible-${{ matrix.preset }}`). `release.yml:275-279` downloads by the `packages-*` pattern and `:617-628` uploads whatever `find` turns up; the `.deb`, `.rpm` and `.tar.gz` globs in the checksum, signing, provenance and SBOM steps (`:484-491`, `:534-545`) cover it with no edit |

Two things follow from that last row and both are worth stating rather than discovering. Any
step uploading a `packages-*` artifact needs `DERIVED_VERSION_OVERRIDE` threaded through
(`_build.yml:1662`) or the filenames carry a `git describe` version instead of the release tag;
and the artifact rides the **glob**, not a `release_package` gate, so a leg that fails to
produce it fails quietly unless `release.yml`'s "name what a release is documented to ship"
check (`:290-301`) gains a row for it. It should.

### The SD-card image, costed, not recommended for v1

This is a distribution form the project has never had, so the costs are laid out rather than
waved at.

**How it would be built.** `pi-gen` (Raspberry Pi's own image builder, the tool that produces
Raspberry Pi OS itself) with a custom stage: Pi OS Lite arm64 as the base, the appliance's
`.deb` installed, the service enabled, first-boot expansion left on, SSH left off. It runs in a
container and would be a standalone workflow — never a matrix leg, because it needs
`binfmt`/qemu or a native arm64 runner and takes tens of minutes.

**How large.** Pi OS Lite arm64 is roughly 500 MB compressed and ~2.5 GB uncompressed; the
appliance adds a few MB. Expect ~600 MB as `.img.xz`. GitHub release assets cap at 2 GB per
file, so it fits — but it would be, by an order of magnitude, the largest thing the project has
ever published, and it would be published on every release.

**Where hosted.** As a release asset, the same route as everything else. There is no second
place to put it that does not become infrastructure someone maintains.

**How verified.** `.sha512` beside it (the release already writes `SHA512SUMS`),
`gh attestation verify --repo iainchesworthlabs/ac3forge`, and — the part that cannot be
automated — **a boot test on real hardware.** Flash, boot a Pi, reach the control page, play a
file to a receiver. That is a manual gate on every release that ships an image.

**Why not v1**, in order of weight:

1. **It makes the project a distributor of an operating system.** Every security update to
   Debian, Pi OS, PipeWire, the kernel and OpenSSL becomes a reason to cut a new image, or the
   published image becomes a known-vulnerable download with the project's name on it. That is an
   ongoing obligation with no end date, taken on for a convenience.
2. **The licensing weight** — see [Licensing](#licensing). Distributing an image means conveying
   other people's GPL software, which triggers GPL-3.0 §6's source-offer obligation for the
   parts conveyed.
3. **Raspberry Pi OS is not entirely free software.** The VideoCore boot firmware
   (`start4.elf`, `fixup4.dat`) ships under a Broadcom licence that permits redistribution but
   is not a free-software licence. Pi OS ships it; an image derived from Pi OS inherits it. That
   is legally fine and it is a fact the project would be stating about its own download for the
   first time.
4. **It cannot be tested in CI.** Every other artefact this project ships has an automated check
   somewhere. This one has a person and a Pi.

**When it would become right:** once the `.deb` route has been used by someone other than its
author, and the first-run documentation has survived contact with them. [Decision 5](#decisions)
records it as a deferred yes rather than a no.

## The docs

The nav is seven tabs (`mkdocs.yml:65-160`). A fourth member makes it eight, matching how
Crucible got its own — and until [decision 2](#decisions) is taken, **this page sits under
Project beside [the family recasting](recasting.md)**, which is where it is added now and is
inside the docs-only fast path.

When the member lands, the tab is:

```yaml
  - Hearth:
      - What it is: hearth/index.md
      - Install and first run: hearth/install.md
      - The control page: hearth/control.md
      - Sources and the queue: hearth/sources.md
      - Following the sink: hearth/following-the-sink.md
      - Settings: hearth/settings.md
      - Keyboard and screen readers: hearth/accessibility.md
      - Languages: hearth/localisation.md
      - Troubleshooting: hearth/troubleshooting.md
      - The appliance plan: family/player-appliance.md   # this page, moved in nav only
```

That is Crucible's own page set (`docs/crucible/`: index, install, room, signal-path, settings,
accessibility, localisation, troubleshooting, promotion) with `room.md` replaced by
`sources.md` and `signal-path.md` by `following-the-sink.md`. That is the shape a member's
documentation takes in this tree, and following it is cheaper than inventing one.

**`hearth/install.md` is the load-bearing page**, and for a keyboardless device it is most of the
documentation. It must answer, in this order, the questions someone actually has:

1. What hardware do I need? (A Pi 4 or later, or any Linux machine with an HDMI or S/PDIF
   output. 2 GB is enough. **Build with `-j2` on a 2 GB Pi or it reboots.**)
2. How do I install it? (`apt install ./ac3forge-hearth_*.deb`, `systemctl enable --now
   ac3hearth`.)
3. **How do I reach it?** — the question with no keyboard attached. `http://ac3hearth.local`
   via mDNS; the printed IP as the fallback; how to find it from the router if both fail.
4. How do I point it at my music? (A directory, an NFS or SMB mount, a USB stick.)
5. How do I know it worked? (What the control page says, and what the receiver's front panel
   should read for each stream shape — the table
   [Raspberry Pi](../platforms/raspberry-pi.md#live-hdmi-passthrough-to-a-real-receiver) already
   has.)
6. What if there is no sound? (Which of the six UX9 states applies, in the page's own words.)

**Changes to existing pages.**

- `README.md`: the three-row member table gains a fourth row (what it is, how to get it, where
  its docs are); the layout block gains `apps/hearth`; the Documentation table gains its guide.
- `docs/index.md:39` "The three members" becomes "The four members", with a `### Hearth` section
  in the same shape as `### Crucible` (:64-74) — what it does, which platforms, what is
  confirmed on hardware and what is not, and a link to install-and-first-run as the fastest way
  in. The "Where to go next" list (:83) gains a row.
- `CONTRIBUTING.md:50-51`: the consumer list gains `hearth`.
- [recasting](recasting.md): the model section gains the member, the "What each member owns"
  table gains a column, and its "Deliberately not in scope" bullet on a fourth member is
  superseded by name and date rather than deleted.
- `docs/platforms/linux.md` and `docs/platforms/raspberry-pi.md`: the Pi page in particular, which
  currently opens by saying there is no Pi-specific code — still true, and now there is a
  product that assumes one.
- `docs/verification.md`: the per-platform hardware table gains the appliance's own rows.
- `docs/threat-model.md`: a section on the listener — what it accepts, from where, and what it
  never does.
- The proposed ROADMAP entry is in [The roadmap entry](#the-roadmap-entry) below, as text, not
  as an edit.

## Localisation and accessibility

**It has a UI, and the UI is a web page**, which diverges from the family's convention, and the
plan does not paper over it. `apps/gui` carries seven `.ts` catalogues
(ar, de, es, fr, he, yi, plus the `xx` pseudo-locale); `apps/crucible` carries six (the same
languages, **no `xx`**). Qt Linguist is what maintains both, and the daemon links no Qt.

**Recommended: keep the `.ts` format, drop the Qt dependency.**
`apps/hearth/translations/ac3hearth_<lang>.ts` are ordinary XML files. A small script in
`tools/` parses them and emits
`ac3hearth_<lang>.json`, compiled into the binary alongside the page assets. This keeps one
catalogue format across the family, keeps `lupdate` usable by anyone who has Qt installed, keeps
the existing translation gate (`docs/crucible/localisation.md#what-the-gate-checks`) applicable
unchanged, and adds no build-time or runtime dependency at all. The alternative — a second i18n
mechanism for the web half — means two glossaries and two gates.

**The `xx` pseudo-locale ships**, unlike Crucible's. It costs one generated file and it is the
only way to see, without reading six languages, that a string was hardcoded or a layout cannot
take a longer word.

**RTL.** Arabic, Hebrew and Yiddish. On a web page this is `dir="rtl"` on the document root and a
CSS logical-properties layout (`margin-inline-start`, not `margin-left`) throughout — which is
cheaper than QML's `LayoutMirroring` and needs the same discipline: no physical direction
anywhere in the stylesheet. Two cases in the page's own test suite hold it, the way
`tst_shell.qml` holds Crucible's.

**`Accessible.role` / `name` / `description` on every custom control** is the family's rule and
the web equivalent is exact: every control is either a native element with a real label, or
carries `role`, `aria-label` and `aria-describedby`. A transport button that is an icon and
nothing else is the same bug in both technologies. The page's test suite asserts it per control,
which is easier here than in QML because the DOM is queryable.

**The ten-foot / headless accessibility problem, which is this member's own.** Crucible's
accessibility page can talk about focus order and what a screen reader announces, because there
is a window in front of the user. Here there is not, and four distinct problems follow:

1. **There is nothing to read.** The appliance has no screen. Everything it could tell you is on
   a device you have to already know how to reach — so if the network is what failed, the
   accessibility story is that there isn't one. This argues for a **non-network status
   signal**: at minimum the daemon's exit and error states must be in the journal and readable
   over the serial console; better, a single LED or GPIO pattern that distinguishes "running",
   "no sink" and "failed". [Decision 7](#decisions).
2. **The format is on the receiver's front panel**, in small text, across the room, and for some
   people not readable at all. The control page must therefore state the negotiated format,
   endpoint and reason **as text**, not as an icon or a colour — which the `reason` string
   discipline already gives it for free.
3. **The control page is the whole interface**, so it carries the whole burden: operable by
   keyboard alone, at 200% zoom, on a phone in one hand, and by a screen reader. No hover-only
   affordance, no drag-only reorder without a keyboard equivalent, no colour-only state.
4. **First run has no accessible fallback if mDNS fails.** "Find the IP in your router's admin
   page" is not an accessible instruction. This is what pushes the plan toward *also* printing
   the URL to the journal and, if a display is ever attached, to the console.

Each of these belongs on `hearth/accessibility.md`, alongside the two sections Crucible's page
already has and this one needs verbatim in spirit: **What is still mouse-only** and
**What has not been checked**.

## Identity assets

| Asset | Value | State |
|---|---|---|
| Icon | `ac3hearth.png` at 32×32 and 256×256 | **Does not exist**, and neither does anyone else's — see below |
| `.desktop` entry | **none** | A daemon has no launcher entry. If a future control-page shortcut wants one it is a `Type=Link` pointing at the local URL, which is a different thing |
| AppStream | `ac3hearth.metainfo.xml`, `<component type="console-application">` | New. Crucible's is `type="desktop-application"` and points at its `.desktop`; a service has neither |
| systemd unit | `ac3hearth.service` | New |
| mDNS service | `_ac3hearth._tcp`, advertising the control port | New — and it is the appliance's most important identity asset, because it is how the box is found |
| Reverse-DNS id | `com.iainchesworthlabs.ac3hearth`, reserved | [recasting decision 12](recasting.md#decisions) settled the root; reserve it now even though v1 ships no bundle |
| Hostname | `ac3hearth` (settable) | What makes `http://ac3hearth.local` work |

**The family-wide icon gap, noted rather than solved.** `apps/crucible/CMakeLists.txt:659-664`
installs `apps/gui/icons/ac3forge-256.png` and `ac3forge-32.png` with `RENAME "ac3crucible.png"`.
So Crucible ships Forge's icon under Crucible's name, and no member has a mark of its own. A
fourth member would be the third to do it. This plan does not solve it — a mark is a design
commission, not a code change — but it records that the fourth member arriving is the point at
which the gap stops being a Crucible detail and becomes a family one, and that the appliance is
the member that needs a mark **least**, because it has no launcher and no dock.

## Third-party notices

`apps/notices/` composes Forge's per-platform notices (`platform/<os>/components.cmake` plus
`fragments/`), and `apps/crucible/notices/` does the same for Crucible with its own fragments
and licences directories. A new component needs `apps/hearth/notices/notices.cmake` and a
`platform/linux/components.cmake`, installing `NOTICES.txt` and `LICENSE.txt` into
`share/doc/ac3forge-hearth/`, and `NOTICES.txt` again as `copyright` — the name dpkg and lintian
look for and CPack's DEB generator never writes (`apps/crucible/CMakeLists.txt:668-675`).

**What goes in it, under the recommended design:**

| Dependency | Licence | Fragment needed |
|---|---|---|
| `{fmt}` | MIT | Reuse Forge's existing fragment; it is compiled in the same way |
| The library and `src/audio` | GPL-3.0, this project | The project's own licence text |
| ALSA (`libasound2`) | LGPL-2.1-or-later | New fragment. Dynamically linked, so the LGPL's relinking condition is met by the shared library |
| PipeWire (`libpipewire-0.3`) | MIT | New fragment; Crucible's notices already carry one — reuse |
| Any web font or CSS in the control page | — | **Recommended: none.** System font stack, no webfont, no framework. A control page with eight controls needs no dependency, and every one added is a fragment, a licence review and a byte on an SD card |

**If [decision 8](#decisions) adds an HTTP library instead of the in-tree listener**, that is one
new fragment and one new licence review; cpp-httplib (MIT) and Boost.Beast (BSL-1.0) are both
GPL-3.0-compatible. **If CEC is ever added**, `libcec` is LGPL-2.1-or-later and needs its own
fragment.

## Licensing

The project is **GPL-3.0**. Three questions follow and each has a definite answer.

**What it permits for a dependency.** Anything under a GPL-3.0-compatible licence can be
linked: MIT, BSD-2/3-clause, ISC, Zlib, BSL-1.0, Apache-2.0 (compatible with GPLv3 — and *not*
with GPLv2, which is why "GPL-3.0" rather than "GPL-2.0-or-later" matters), LGPL-2.1 and
LGPL-3.0, and GPL-3.0 itself. Everything this plan proposes is inside that set.

**What it forbids.** GPL-2.0-**only** code cannot be combined with GPL-3.0 code — the two are
mutually incompatible, and "GPL-2.0-or-later" is fine only because the "or later" lets it be
taken as v3. Anything under a proprietary, source-available, non-commercial or
field-of-use-restricted licence is out, whatever its marketing says. Anything under the original
4-clause BSD (the advertising clause) is out. In practice this rules out most vendor SDKs — the
reason the project's dependency list is `catch2` and `fmt`, and a reason to keep it there.

**What it means for shipping a disk image.** An image is a *conveyance* of every program on it,
so:

- Each program keeps its own licence; an image of Debian packages is not a derivative work of
  them, it is an aggregate. That part is routine and Debian does it daily.
- **GPL-3.0 §6 requires a source offer for the GPL-covered parts conveyed.** The accepted
  practice — and Debian's own — is to point at a public archive where the corresponding source
  is available for as long as spare parts are offered. The project would need that offer written
  down in the image and on the download page. It is a paragraph, not a project, but it is a
  paragraph nobody has written.
- **The Raspberry Pi VideoCore boot firmware is redistributable but not free software.** An
  image derived from Raspberry Pi OS contains it. That is legally fine — the Broadcom licence
  permits redistribution on Pi hardware — and it means the image is not an all-free-software
  download, which the project has never had to say about anything it publishes.
- The appliance's own code and the control page are GPL-3.0 like the rest of the tree. A web
  page served over a network is not distribution under GPL-3.0 (that is the AGPL's territory, and
  this is not AGPL), so serving the control page triggers no obligation the `.deb` does not
  already carry.

## Signing and install

**Roadmap DR6 is unresolved** — Developer ID and notarisation for macOS, Authenticode for
Windows, blocked on certificates rather than code, and a Known gap in every release since
0.8.0-beta.2. It gates every application member that ships to a consumer OS.

**What this member needs from it: nothing, in v1.** Linux has no Gatekeeper and no SmartScreen.
A `.deb` or `.rpm` installs without a code-signing certificate, and what the project already does
for integrity is the right thing and already automated: `SHA512SUMS`, the GPG release key
(`ac3forge-signing-key.asc`), and `gh attestation verify --repo iainchesworthlabs/ac3forge`
build provenance over `*.deb`, `*.rpm` and `*.tar.gz` (`release.yml:484-491,534-545`). The
appliance's packages ride all three by the existing globs.

This is worth stating positively rather than as an absence: **the appliance is the one
application member that can ship to users today without DR6 being resolved.** That is an
argument for Linux-first independent of DR9's evidence.

**What changes if the member ever reaches Windows or macOS.** It is gated by DR6 exactly as
Crucible is, with one addition: a Windows *service* that binds a listening socket will prompt
Windows Firewall on first run, and an unsigned binary doing that is a worse experience than an
unsigned desktop application doing nothing of the kind.

**Does an image need signing of its own?** Not code signing — there is nothing to sign for.
It needs:

- A `.sha512` beside it and its checksum in `SHA512SUMS`, which the release already writes.
- Build provenance via `gh attestation`, which needs `*.img.xz` added to the `subject-path` list
  at `release.yml:534-545`. One line.
- A documented verification recipe on the download page, because the audience for an image is
  wider and less likely to verify by habit.

And what it explicitly does **not** get: boot integrity. The Raspberry Pi 4 has no Secure Boot
worth the name, so signing an image verifies the *download*, never the *boot*. Anyone who can
write to the SD card owns the appliance. That belongs on the install page in those words.

## The roadmap entry

`ROADMAP.md` is not edited by this plan — ID allocation is the owner's. The entry below is
proposed text, in the file's own form, with a `CR`-style code for a new member (the recasting
plan's Phase 5 established `CR` for Crucible; a fourth member takes the next such code).

> **PLn (XL, Hearth)** — A playback appliance: the decode and passthrough path as an always-on
> headless product, with a local control page, feeding an AV receiver over HDMI or S/PDIF.
> Linux first, on the two backends confirmed on real hardware.
>
> <details markdown="1">
> <summary>Full record</summary>
>
> Planned in `docs/family/player-appliance.md`. Builds on UX1's transport, UX9's sink-following
> and DR9's Pi/ALSA and Pi/PipeWire hardware confirmations. Six things stand between UX9 and
> carrying it, listed on that page: the default endpoint is never probed, PipeWire has no
> capability read, `play` never re-follows, the transcode leg goes through a temp file, EDID is
> read once, and `kNoEdid` and `kNoBackend` reach the user as one message. Proposed as a fourth
> family member, which supersedes the recasting plan's "a fourth member" exclusion; the name is
> undecided.
>
> </details>

Two existing entries would gain a pointer rather than change: **UX9**, whose six gaps this
depends on, and **DR9**, whose Linux rows are the evidence the platform choice rests on.

## Phases

Each ends with something that can be checked and says how. Phases 1 and 2 are the same under
every decision and can start before any of them is taken; Phase 0 is this page.

### Phase 0: this page

Land the plan, in the nav under Project beside [the family recasting](recasting.md).

**Exit:** the page is on the site and reachable from the Project tab; nothing else in the tree
changed.

**Verified by:** `mkdocs build --strict`; the docs-only fast path (`ci.yml:311`) classifies the
PR as docs-only and skips the matrix; `tools/checks/check_doc_paths.py` green.

### Phase 1: close UX9's gaps in `ac3cli play`

No new member, no new target. The six items in
[What UX9 needs](#what-ux9-needs-before-it-can-carry-this), landed where they already live, so
the CLI gets better whether or not a fourth member is ever approved. Specifically: resolve and
probe the default endpoint; add the PipeWire capability read over `iec958.codecs`; separate
`kNoEdid` from `kNoBackend` in what the user is told; and add the shared
"what does this sink take" helper to `ac3::audio` so `play` and Crucible's policy stop answering
it separately.

**Exit:** `ac3cli play` with **no** device index takes the same sink-following path a named
endpoint takes; `read_sink_capabilities()` returns a real answer on PipeWire; a receiver that is
off and a platform that cannot look produce different messages; the helper has one caller in
`apps/cli` and one in `apps/crucible`.

**Verified by:** `ctest -L cli` and `-L crucible` on Linux and Windows; new cases over the fakes
for each of the six states; **on the Pi**, `ac3cli play` with no index to the receiver over both
ALSA and PipeWire, and `ac3cli outputs` agreeing with what the sink then accepts;
`-DAC3FORGE_WITH_ALSA=OFF -DAC3FORGE_WITH_PIPEWIRE=ON` and the reverse both configure and pass.
**Needs an AV receiver** for the last one — the ALSA and PipeWire capability reads can be
unit-tested against captured ELD bytes, but "the sink actually accepts what it said it would" is
a hardware claim.

### Phase 2: the core, pure and everywhere

`apps/hearth/core/`: the queue, the transport state machine, the format decision (its own
policy, in the shape of `output_policy.hpp` — pure, returning a mode, an endpoint and a reason),
the configuration reader and writer, and the JSON layer. `tests/hearth/` compiles into
`ac3tests` ungated on all eleven legs, with `fake_sink.hpp` covering the seven cases in
[Tests](#tests). No daemon, no socket, no platform.

**Exit:** `ctest -L hearth` passes on all eleven legs including the sanitiser ones; the format
decision's whole case table runs with no sound card; the coverage row is measured and the number
recorded.

**Verified by:** the eleven legs; `-L hearth` under ASan/UBSan and TSan;
`tools/checks/coverage_report.sh` reporting the new component.

### Phase 3: the control surface

`TcpListener` beside `UdpSocket` on `src/audio/src/net/`'s existing OS axis; the HTTP/1.1
subset; the JSON API; the page assets compiled in; the `.ts` catalogues and the script that
turns them into JSON; a fuzz target over the request parser in `fuzz/`; the threat-model
section.

**Exit:** the page serves and drives the core over loopback with no audio device present; the
fuzz target runs clean for its configured budget; every control has a role and a name; the `xx`
pseudo-locale renders and nothing is hardcoded; the RTL cases pass.

**Verified by:** `ctest -L hearth-net` on every leg that can bind a socket; `fuzz.yml`; the
page's own suite at 200% zoom, keyboard-only, and under `dir="rtl"`; a manual screen-reader pass
recorded on `hearth/accessibility.md` with its own "what has not been checked" section.

### Phase 4: the appliance shape

The daemon: `apps/hearth/platform/linux/`, the systemd unit, the service user, the config file at
`/etc/ac3forge/hearth.toml`, mDNS advertisement, journal logging, readiness notification, and
the re-follow path behind both the PipeWire watcher and the ALSA timer. The build identity and
the `hearth: true` flag on the two CI legs.

**Exit:** `systemctl start ac3hearth` on a fresh Pi brings up a control page reachable at
`http://ac3hearth.local`; a queue plays end to end; unplugging the receiver mid-stream is
survived and reported rather than fatal; the service restarts cleanly at boot; the daemon makes
no outbound network connection (asserted, not claimed).

**Verified by:** the two flagged legs building and packaging; `hearth_platform_probe` asserting
the compiled-in backend; **on the Pi**, a full boot-to-playback run on both backends, a
receiver power-cycle mid-programme, and `ss -tunap` plus a packet capture showing no outbound
traffic. **Needs an AV receiver** for the re-follow case: a sink whose answer changes is
simulable in a test, but "the receiver came back and the stream re-locked" is not.

### Phase 5: packaging and release identity

The `hearth` component, the DEB/RPM/archive names, the notices, the AppStream entry, the
`packages-hearth-<preset>` upload with `DERIVED_VERSION_OVERRIDE`, and the row in `release.yml`'s
"name what a release is documented to ship" check.

**Exit:** a release dry run produces `ac3forge-hearth-*` for both Linux architectures among its
collected artefacts; the `.deb` installs, enables and runs on a clean Pi OS; `lintian` finds no
error; `apt show ac3forge-hearth` names the member.

**Verified by:** `cpack` locally per component; the two flagged legs in CI; a `release.yml` dry
run, the way DR8 was verified; `tools/checks/check_packaging_versions.sh`; **on the Pi**, an
install of the CI-produced arm64 `.deb` onto a machine that has never had a build tree on it —
which is the only way to catch a missing runtime dependency.

### Phase 6: the docs and the member

The eight-page guide, the eighth nav tab, the README row, `docs/index.md`'s four members, the
CONTRIBUTING list, the recasting supersession note, and this page relabelled and moved in the
nav. Depends on [decisions 1 and 2](#decisions).

**Exit:** a reader arriving at the home page sees four members and can reach each one's guide and
its download in one link; someone with a Pi, a receiver and no prior knowledge can get from the
install page to sound.

**Verified by:** `mkdocs build --strict`; `check_doc_paths.py`; a walk of all eight tabs; the
README rendered on GitHub; and the part that is not automatable — **someone other than the author
following `hearth/install.md` on a fresh Pi, and every place they stop being written down.**

### Phase 7, conditional: the image

Only if [decision 5](#decisions) says yes, and only after Phase 6's outside reader. The `pi-gen`
stage, the standalone workflow, the `.img.xz` release asset, the attestation subject-path line,
the source offer, and the firmware-licence statement.

**Exit:** a flashed image boots on a Pi with no configuration, reaches the control page, and
plays to a receiver; the download page carries the verification recipe, the source offer and the
non-free-firmware statement.

**Verified by:** flash and boot on real hardware, every release — a manual gate, stated as one.
**Needs an AV receiver.**

## What cannot be verified, and why

| Claim | Can it be verified | Blocker |
|---|---|---|
| The name is free as a winget `Moniker` | **no** | `Moniker` is a soft alias, neither namespaced nor enforced, and there is no public index of monikers to query. The *identifier* `iainchesworthlabs.ac3forge-hearth` cannot collide, and was confirmed by checking that no publisher directory of that name exists in `microsoft/winget-pkgs` |
| The name does not infringe an existing mark | **no**, not from here | Registry searches find registrations, not risk. Hearth Display, Inc.'s Class 9 mark for a wall-mounted household display is recorded above as the closest adjacency found; whether it matters is a question for someone qualified, and only if the project ever files |
| Windows/WASAPI exclusive passthrough works to a receiver | yes, by cabling the workstation's HDMI | DR9's own open item (S, hardware). Until then the appliance is Linux-only on evidence, not preference |
| CoreAudio passthrough works | **no** | DR9: no Mac has run any of it |
| The transcode leg keeps metadata across a *streaming* transcode | yes, once written | Today's leg goes through a temp file, where `transcode` carries dialnorm/compr/mix; whether a streaming version preserves the same fields is a property of code that does not exist |
| A receiver other than the one on the bench behaves the same | **no** | One machine, one receiver — the same caveat `docs/crucible/install.md` already states about its own reading. Every receiver's EDID and every manufacturer's fallback behaviour differs; the Pi record already shows one receiver falling back *gracefully* on an unsigned object container where another might refuse |
| The appliance survives being left on for a month | **no**, not before it exists | Needs an appliance and a month. DR9's PipeWire run already found a WirePlumber hot-plug activation that had "silently failed after a long uptime" — a class of bug only uptime finds |
| The `.deb` has no missing runtime dependency | yes | Install the CI artefact on a machine that has never had a build tree. A developer's Pi has the `-dev` packages and cannot see the gap |
| The control page is usable by a screen-reader user | **partly** | Automated checks find missing roles and names; whether the page is *usable* needs a person who uses one, and the page says which it is |
| No user is exposing the control page to the internet | **no** | No telemetry, and none is proposed. It is the reason the default binds locally and the documentation says so plainly |

## Coordination

**Open pull requests.** Checked 2026-09-07: #531 (dependabot), #532, #534, #535 and #536, all
`apps/crucible` engine bugfixes — none touches `apps/cli`, `src/audio` or `docs/`. Phase 0
collides with nothing. Phase 1 touches `src/audio` and `apps/crucible/engine/output_policy.*`,
so it should land with the Crucible queue drained; `gh pr list` first, the way the recasting
plan's tree-wide phases do.

**The driver-signing session.** Unaffected. The appliance is Linux-only, needs no virtual
device, and touches nothing under `apps/windows/`.

**The recasting plan.** Phase 6 here supersedes one bullet of
[recasting](recasting.md#deliberately-not-in-scope)'s "Deliberately not in scope" list, by name
and date. Nothing else in that plan changes; every one of its fifteen decisions still holds, and
this member inherits all of them.

**The Pi.** The Raspberry Pi at `iain@192.168.1.167` is the verification hardware for Phases 1,
4, 5, 6 and 7. It has 2 GB of RAM: **build with `-j2` or it reboots.** A desktop environment,
where one is needed, comes from `wf-panel-pi`'s `/proc` environ — though the appliance itself
needs none, which is the point.

## Deliberately not in scope

- **Choosing the name.** [Decision 1](#decisions).
- **Video, of any kind.**
- **A media library, tag database, cover art or metadata scraping.**
- **Streaming-service clients** — AirPlay, Chromecast, DLNA, Spotify Connect. Each is a
  certification programme, a protocol, or a licence the project cannot accept.
- **Authentication, user accounts or remote access.** The control page is a LAN interface, and
  making it anything else is a security design this plan does not open.
- **A Windows or macOS appliance in v1**, on DR9's evidence.
- **A kiosk window or a ten-foot interface.** [Decision 4](#decisions) records it as a separate
  product question rather than a deferred feature.
- **HDMI-CEC**, in v1. A later convenience over the same command layer.
- **Encoding anything.** The appliance decodes and passes through; Crucible encodes.
- **Room correction, renderers and binaural folds** — already off the roadmap.
- **A web framework, a webfont, or any new vcpkg dependency.**
- **Splitting `ac3tests`**, exporting `src/audio`, or moving any existing directory.
- **Renaming any published identifier**, including the ones this member would add.
- **Renumbering roadmap IDs**, or editing `ROADMAP.md` or `CHANGELOG.md` from this plan.
- **A mark of the project's own.** The icon gap is recorded, not solved.

## Decisions

Only what the owner has to decide. Each carries options, a recommendation and the cost of taking
it. **None is taken here.**

1. **The name.** (a) **Hearth**; (b) Anvil; (c) Ember; (d) AC3Forge Player. **Recommend (a)** —
   the forge's fire and the room's, one word, the register Forge and Crucible set. Cost: an
   adjacent Class 9 mark held by a company making a wall-mounted household appliance, and three
   registries carrying an unrelated small `hearth`. Neither blocks the published token
   `ac3forge-hearth`. (b) collides with Foundry's installed `anvil` binary, next to its
   installed `forge`; (c) collides with Ember.js; (d) has no risk and no identity.

2. **Which member.** (a) **a fourth member**; (b) part of Crucible; (c) part of Forge.
   **Recommend (a)**, on the six-way test in
   [Which member it belongs to](#which-member-it-belongs-to). Cost: about six documentation files
   — `docs/index.md`'s "The three members" becomes four, the README table gains a row,
   `CONTRIBUTING.md`'s consumer list gains one, the nav becomes eight tabs, and
   [recasting](recasting.md)'s "a fourth member" exclusion is superseded by name. No identifier
   anyone has installed changes.

3. **Platforms in v1.** (a) **Linux only** (ALSA and PipeWire, x86_64 and arm64); (b) Linux and
   Windows; (c) all three. **Recommend (a)**, because DR9 confirms Linux passthrough on real
   hardware and does not confirm the other two, and an appliance is a passthrough product. Cost:
   the member ships to one platform, and the Windows row of DR9 stays the reason. The core is
   written platform-free from Phase 2 so (b) is a platform directory later, not a rewrite.

4. **The form.** (a) **a headless service with a web control page, delivered as a DEB/RPM**;
   (b) a kiosk QML application; (c) both. **Recommend (a)**. Cost: no interface on the television,
   and the user must reach a URL from another device — which is what the mDNS name and most of
   the install page exist for. (b) puts Qt Quick and a compositor on a 2 GB Pi that must stream
   without underrunning, and puts the interface on the one screen already showing something else.

5. **The SD-card image.** (a) **not in v1, reconsider after the install page survives an outside
   reader**; (b) ship one from the first release; (c) never. **Recommend (a)**. Cost: the
   easiest possible first run — flash and boot — is not available at launch. Taking (b) means the
   project
   distributes an operating system, owns its security updates, writes a GPL-3.0 §6 source offer,
   states that the Pi's VideoCore firmware is non-free, and adds a manual boot test to every
   release, for a ~600 MB asset that no CI job can check.

6. **The control page's security posture.** (a) **bind locally, no authentication, say so
   plainly**; (b) a shared password; (c) proper accounts. **Recommend (a)** for v1: it is what a
   home-LAN appliance does, it is stated rather than assumed, and it does not overstate what a
   password over plain HTTP would buy. Cost: the page must never be port-forwarded, and
   only documentation says so.

7. **A non-network status signal.** (a) **journal and serial console only**; (b) an LED or GPIO
   pattern; (c) an audible tone through the sink. **Recommend (a)** for v1 and (b) as the first
   thing added, because a keyboardless box whose network failed has no other way to tell anyone.
   Cost of (a): the failure mode where the appliance cannot say it has failed. (c) is rejected —
   a tone through a sink that may be misconfigured is not a diagnostic.

8. **The HTTP layer.** (a) **an in-tree HTTP/1.1 subset over a `TcpListener` beside
   `UdpSocket`**; (b) cpp-httplib (MIT); (c) Boost.Beast (BSL-1.0). **Recommend (a)**: the
   dependency list is `catch2` and `fmt`, `src/audio/src/net/` already establishes the OS axis
   and the reasoning for it, and the surface needed is a few hundred lines that fuzz well. Cost:
   the project owns a request parser, which is exactly why Phase 3 makes it a fuzz target and
   gives it a threat-model section. (b) and (c) are both GPL-3.0-compatible and each costs one
   notices fragment and the first new dependency in a long time.

9. **The coverage floor.** (a) **set it from the first green measurement, design toward 55/45**;
   (b) pick a number now; (c) no floor. **Recommend (a)**, which is how every existing row was
   set. Cost: one phase ships before the row exists.

10. **Where the shared "what does this sink take" answer lives.** (a) **a helper in `ac3::audio`,
    called by `play`, Crucible's policy and the appliance**; (b) leave the two existing copies
    and add a third. **Recommend (a)**. Cost: Phase 1 touches `apps/crucible/engine/`, so it must
    land with that queue drained. Taking (b) costs a third place for the EDID-then-probe fallback
    rule to drift.
