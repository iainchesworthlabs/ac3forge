# Hearth: a desktop reference player and Sendspin sinks

!!! note "Status as of 2026-09-22: being built"
    Written and decided on 2026-09-15, in one session of questions and answers recorded under
    [Decisions](#decisions). What has merged since, by chip:

    - **[The desktop app](#chip-a-the-desktop-app).**
        - Merged: A1 (#686), A2 (#708) and A3 (#714, #720, #727, #732, #736, #742 and #745 to
          #752).
        - A4 has merged (#684, #704 and #706), but its Music Assistant exit is still open.
        - A0 is signed off (2026-09-22; [planning/hearth-design.md](hearth-design.md)). A5 is
          under way: the application shell and Play page (#787) and the engine's routing/trim/
          delay/crossover surface (#793) have merged; the Speakers and Decoder pages (#810) are
          open. A6 to A8 follow it.
    - **[The ESP32-S3 sink](#chip-b-the-esp32-s3-sink).**
        - Merged: B1 (#709), B2 (#726), B3 (#737), B4 (#738) and B5 (#756), with fixes in #741
          and #743.
        - The exits on the boards wait for the TDM DAC boards, and B3's Music Assistant exit is
          still open. So is B5's second check: someone setting up a board from the guide alone.
    - **[The ESP32-C6](#chip-c-the-esp32-c6).**
        - Merged: C1 (#677) and C2 (#678 and #701), with the C6's fixed-point speed-up in #694.
        - C2's exit on a TDM DAC waits for a board. C3 follows chip B.
    - **[The AC-4 decoder](#chip-d-the-ac-4-decoder).** The plan is not in the repository yet.
      Its first code has merged: the channel-coded substream syntax (#700), with #712, #715, #739
      and #744.

    Checks on the user's hardware are still to run for A2 and A3: the identify tone, and the
    passthrough and monitor position tests, on the Onkyo receiver and the Pi.

    This page replaces the **form** of [the appliance plan](player-appliance.md): its headless
    daemon, web control page, kiosk window and HLS client are dropped. Hearth's name, its place
    as the family's fourth member and its build identity carry over. It also replaces
    [topology decision 1](topology.md#decisions) for Hearth's own sinks, whose transport becomes
    Sendspin with an extension role, and [the Sendspin sections](esp32-player.md#sendspin) of the
    ESP32 player plan.

    The work is four task chips, each with its phases below:
    [the desktop app](#chip-a-the-desktop-app), [the ESP32-S3 sink](#chip-b-the-esp32-s3-sink),
    [the ESP32-C6](#chip-c-the-esp32-c6) and [the AC-4 decoder](#chip-d-the-ac-4-decoder).

    Shape follows [the appliance plan](player-appliance.md) and [the topology](topology.md):
    design sections say what changes and why, phases carry an exit criterion and how it is
    verified, [Decisions](#decisions) records what was asked and what was answered, and
    [What cannot be verified](#what-cannot-be-verified-and-why) says where the evidence stops.

## What Hearth is

Hearth is the project's playback member. It has two forms, and they talk to each other.

**`ac3hearth`** is a desktop application for Windows, Linux and macOS. It opens AC-3, E-AC-3 and
E-AC-3 JOC (Atmos) media, decodes it with the library's decoder under settings the user can see
and change, renders it to a chosen speaker layout, routes each rendered channel to an output, and
shows per-channel levels and the bitstream information while it plays. It plays to a local
device, passes the bitstream through to a receiver, or streams to sinks on the network. Every
decoder option the library has is reachable from it, and what it did with a stream is visible,
which is what "reference player" means on this page.

**`hearth_sink`** is firmware for ESP32-S3 and ESP32-C6 boards. A sink joins the network as a
Sendspin player: Music Assistant can play to it like any other Sendspin speaker, and `ac3hearth`
can send it the undecoded bitstream through an extension role. The sink decodes that stream,
renders it to its own layout and plays it on up to sixteen TDM outputs (S3) or eight (C6). It
serves a small status page.

Between the two sits **Sendspin**, the synchronised-audio protocol of the Open Home Foundation and
Music Assistant (`github.com/Sendspin/spec`). `ac3hearth` is a conformant Sendspin server and each
sink a conformant Sendspin player. The compressed stream travels in an application-specific
role, `_ac3forge_player@v1`, which is the extension mechanism the specification provides and which
other servers ignore. Groups, clock synchronisation, encryption and pairing are Sendspin's.
[Why Sendspin, and how far](#why-sendspin-and-how-far) gives the reasons.

A fourth binary, **`ac3hearth-testsink`**, is a sink that runs on a computer: the same Sendspin
player and extension role, decoding with the same renderer as the boards, writing each channel to
a file or an audio device. It is a test tool for CI, a workstation or a VM, and is not packaged
for end users.

## What changed from the 2026-09-07 plan

| Item | 2026-09-07 | 2026-09-15 | Decision |
|---|---|---|---|
| Desktop form | Headless daemon `ac3hearth`, a web control page, a Qt kiosk client of its API | A Qt Quick application, `ac3hearth`. No daemon, no web page, no kiosk | 1 |
| Sinks | A Linux, Windows or macOS appliance beside a receiver; the ESP32-S3 as a platform of the same member (topology decision 2, recommended) | `hearth_sink` firmware on ESP32-S3 and ESP32-C6; no appliance on a computer | 1, 8 |
| Transport to Hearth's sinks | HLS/CMAF over HTTP (topology decision 1) | Sendspin, with the bitstream in an extension role | 2, 13, 17 |
| Sendspin | "Not a client of Snapcast, Sendspin or Music Assistant" (the appliance plan's scope) | Sinks are Sendspin players; the app is a Sendspin server | 2, 17 |
| Groups | Hearth as its own zone; groups measured later (topology Phase 4) | Sendspin groups in v1 | 12, 13, 14 |
| Rendering | "No room correction, no renderer" (the appliance plan's scope) | The ESP32 player's `LayoutRenderer` moves into the library for the app; per-output trim, delay and bass management | 16 |
| HTTP layer | cpp-httplib for a REST API, SSE and an HLS client | cpp-httplib for Sendspin's WebSocket | 11 |
| AC-4 | Not in scope | Designed into the app now; the decoder is a follow-on chip | 7 |
| Carried over | The name, the fourth member, `ac3::hearth`, `AC3FORGE_BUILD_HEARTH`, the `ac3forge-hearth` package token, the six sink-following gaps in `ac3cli play` | Unchanged; the gaps become part of the app's passthrough mode | appliance plan 1, 2, 10 |

## The desktop app

### Media

- One or more items: files, a folder, or a drop onto the window, held in a queue with play,
  pause, stop, next, previous and seek.
- Containers through `apps/common/container_input.hpp`, which sniffs Matroska, MP4 and MPEG-TS
  and joins the first track: raw `.ac3`/`.ec3`, `.mp4`/`.m4a`, `.mkv`, `.ts`. The container
  readers return neither timestamps nor a track list, `mp4::Reader` needs `moov` before `mdat`,
  and MPEG-TS yields PES payloads that have to be re-split. WAV carrying IEC 61937 is readable
  through `BurstReader` but is not sniffed.
- AC-4 (`.ac4`, and AC-4 in MP4 or TS) is listed with its bitstream information from `src/ac4`
  and marked as not playable until [chip D](#chip-d-the-ac-4-decoder) delivers a decoder.
- Duration and seek come from each stream's samples per access unit. The GUI's stream player
  assumes 1,536 (`apps/gui/stream_player_controller.cpp:147`), which is wrong for E-AC-3 with
  fewer than six blocks per frame.

### Playback configuration

- **Output**, one of: a local PCM device; a passthrough-capable device (IEC 61937 to HDMI or
  S/PDIF); a Sendspin group of sinks and players ([Groups](#groups)).
- **Speaker layout**, in the renderer's grammar (`esp-idf/ac3forge/include/ac3forge/layout.hpp`
  today): a named `F.L.H` layout of up to sixteen speakers or a per-speaker list of locations and
  angles, with `:small`, `:height`, `:top` and `:upfiring`. A coded channel whose location is in
  the layout goes to that speaker, other channels are panned, objects are panned by position, and
  nothing is upmixed. Stereo and mono targets use the decoder's §7.8 fold.
- **Gapless playback**, on or off ([Gapless playback](#gapless-playback)).

### Decoder configuration

What the controls map to, and what each needs:

| Control | In the library today | Work |
|---|---|---|
| Operating mode: line, RF, custom | `OperatingMode` (`src/forge/include/ac3/decoder/output.hpp:57`) and `rf_ceiling` | none |
| DRC scale | `DecoderConfig::drc_scale`, one exponent for cut and boost | separate cut and boost scale factors, in `src/forge` |
| Heavy compression | `DecoderConfig::heavy_compression` (`decoder.hpp:168`) | none |
| Dialogue normalisation | `OutputConfig::apply_dialnorm` (`output.hpp:76`), a fixed −31 target that only attenuates | none unless the design keeps a target control |
| Stereo downmix | `DownmixTarget` as coded, Lo/Ro, Lt/Rt, mono (`output.hpp:46`), `ltrt_phase_shift`, `mix_lfe` | none |
| Mix levels | Read from the stream; `OutputStage::apply` already accepts caller levels | overrides in the configuration |
| Dual mono | Left to the caller by design (`output.hpp:166-170`) | the engine selects channel 1, channel 2 or both |
| Programme | `DecoderConfig::programme` (`decoder.hpp:239`); `io::scan` lists programmes | none |
| Objects | `skip_object_reconstruction`, `joc_domain`; objects are reconstructed only when the JOC downmix is five channels | the ESP32 player's auto, never, always policy moves up with the renderer |
| Concealment | `concealment`: none, repeat and fade, mute | none |
| Fast inverse transform | `DecoderConfig::fast_imdct` (`decoder.hpp:170`) | none |

The AC-4 controls are designed in [A0](#a0-design-rounds) and disabled until the decoder exists:
presentation (from the table of contents: presentations, language, channel mode), main and
associated with a mix level, dialogue enhancement level, DRC decoder mode and profile, and the
downmix modes. Where an AC-4 control and an E-AC-3 control are the same idea, such as DRC mode
or downmix target, the app shows one control. `src/ac4` is an inspector
(`src/ac4/include/ac4/ac4.hpp:22`) and does not parse the dialogue enhancement or DRC payloads
yet, which is part of chip D.

### Monitor

- One meter per output channel, labelled with the speaker the routing put there: peak, hold, RMS
  and clip latch from `ac3::analysis::LevelMeter`
  (`src/forge/include/ac3/analysis/levels.hpp`).
- Loudness from `ac3::meta::LoudnessMeter` (`src/forge/include/ac3/meta/loudness.hpp`):
  momentary, short-term, integrated, loudness range and true peak. Today only QC uses it.
- Meter snapshots are released at their play time. The GUI's player meters each chunk before it
  is queued, so its meters run ahead of the sound by the device queue; A2's playback position is
  what fixes this.
- A view of object positions for E-AC-3 JOC, if the design keeps one: `ac3gui` has a soundfield
  view and Crucible a room.
- For a network group, the app decodes the stream locally for the monitor, since it holds the
  bitstream, and shows each sink's reported per-output levels beside that decode.

### Speaker setup and routing

- **Routing**: each rendered channel to one output of the device, or one slot of a sink, with
  outputs allowed to stay unassigned. `src/audio` has no routing today and assumes the
  WAVE_FORMAT_EXTENSIBLE channel order. On the ESP32 a per-speaker layout list already expresses
  a patch, and duplicates are refused.
- **Identify tone**, one channel at a time.
- **Per-output trim** in dB and **per-output delay** in milliseconds.
- **Bass management**: small speakers crossed over into the LFE. The renderer takes a crossover
  frequency (`esp-idf/ac3forge/include/ac3forge/render.hpp:95-107`) and the player passes the
  80 Hz default; the app makes it a setting.
- Saved per output device, and on each sink for that sink's own wiring.

This is speaker management: no measurement, no equalisation, no filters beyond the crossover.
ROADMAP.md lists "Renderer and room-correction territory" under Out of scope; see
[ROADMAP.md](../ROADMAP.md#out-of-scope).
that line is amended when this lands, since the renderer already shipped in the ESP32 player and
now reaches the desktop.

### Media information

- **AC-3 and E-AC-3**: per frame from `DecodedFrame` (bitstream mode, dialnorm, centre and
  surround mix levels, compr and dynrng, block switching), `BsiInfo` and `AlternateBsi`, per
  substream mixing metadata and channel maps, `io::scan` (programmes, associated services,
  channel map), and the whole-file `io::probe` report (measured and declared bitrate, VBR,
  metadata ranges, EMDF payload ids, OAMD and JOC, CRC, coding tools). `ac3cli probe json=1`
  (schema `ac3forge.probe/1`) is the precedent and reads raw streams only.
- **Objects**: OAMD object metadata; whether an authenticity tag is present
  (`has_authenticity_tag`; verification needs a key and checks only this project's own tag).
- **Container**: codec configuration box (`dac3`, `dec3`), track, duration.
- **AC-4**: table of contents, presentations, substream groups, channel modes, bitrates, language
  and A-JOC information from `src/ac4`.
- Copyable, and exportable as JSON.

### Passthrough

`PassthroughSink` with `wrap_frame` and `Eac3BurstPacker`, the capability read where the platform
has one (ALSA's ELD) and the live probe elsewhere. The six sink-following gaps in
[the appliance plan](player-appliance.md#what-ux9-needs-before-it-can-carry-this) apply to this
mode; each is checked against the tree before it is fixed, since some code has moved since
2026-09-07. In passthrough the decoder settings are inactive and say why, and the monitor shows a
local decode. Every stream shape, signed Atmos included, has locked on a receiver from Windows
(the Onkyo) and from the Raspberry Pi.

### Network outputs

- Discovery of Sendspin players over mDNS (`_sendspin._tcp`, whose `path` TXT key is required).
- Pairing: the sink shows a six-digit code on its serial console and page, or carries a static
  eight-digit code, and the user enters it in the app. Pairing records are kept on both sides.
- Groups, per [Groups](#groups). A sink held by another server, such as Music Assistant, shows
  as in use; taking it over is an explicit action, because a player admits one playback
  connection and a new one displaces the old.
- Hearth sinks receive the bitstream and the app's settings for that sink (layout, routing,
  trims, delays, bass management, decoder settings). Standard Sendspin players receive stereo
  PCM, FLAC or Opus that the app has decoded.

### Documentation

A `docs/hearth/` guide with screenshots generated by `ac3hearth --shot`, following Crucible's
`--shot` and `--page` options (`apps/crucible/ui/main.cpp`), and a script under `tools/` that
regenerates them so the pictures follow the interface.

### Architecture

| Layer | Path | Target | Qt | Used by |
|---|---|---|---|---|
| Renderer and speaker management | `src/forge/include/ac3/render/` (moved from `esp-idf/ac3forge/include/ac3forge/{layout,render}.hpp`) | part of `ac3::forge` | no | engine, test sink, `hearth_sink` |
| Sendspin | `src/sendspin/` | a static library beside `mp4` and `mpegts` | no | engine (server half), test sink and `hearth_sink` (player half) |
| Output devices | `src/audio/` | `ac3::audio` | no | engine, test sink |
| Engine | `apps/hearth/engine/` | `ac3hearth_engine` | no | app, test sink |
| Application | `apps/hearth/ui/` | `ac3hearth` | yes | |
| Test sink | `apps/hearth/testsink/` | `ac3hearth-testsink` | no | CI, contributors |
| Firmware | `esp-idf/ac3forge/examples/hearth_sink/` | ESP-IDF project | no | S3, C6 |

The engine follows Crucible's split: a Qt-free engine with thread-safe commands and a status
snapshot, a controller that polls it (Crucible's is 60 ms), platform seams as directories under
the no-`#if` rule that `tools/checks/check_platform_macros.ps1` enforces, and fakes that let the
engine run in `ac3tests` on every leg.

**Constraints on `src/sendspin`'s player half**, so chip B does not rewrite it: it builds with
ESP-IDF's toolchain; no `thread_local` above 4 KiB (FreeRTOS carves thread-local storage out of
every task stack); no large stack frames (the HTTP server's tasks run on 6,144 bytes); bounded,
measurable allocations (the S3 heap is regioned and the C6 has no PSRAM); sockets, crypto, clock
and randomness behind seams, backed by cpp-httplib, mbedTLS and the OS on a computer and by
`esp_http_server`, ESP-IDF's mbedTLS, `esp_timer` and the hardware RNG on a board. JSON is written
in-tree: the only JSON reader in the tree is private to `ac3::oba`
(`src/forge/src/oba/scene_json.cpp`).

**Dependencies**, behind a vcpkg manifest feature `hearth` so that a library-only build pulls
none of them: cpp-httplib (MIT), mjansson's `mdns` (public domain), mbedTLS (Apache-2.0), libFLAC
and Opus (BSD-3-Clause) for the codecs a conformant server must offer, and Sendspin's reference
time filter (`SendspinTimeFilter`, Apache-2.0), vendored with its licence. Qt comes through
`cmake/FindQt6.cmake` as for the other applications. Each one enters the generated notices
through `cmake/Notices.cmake`.

## Why Sendspin, and how far

Sendspin already has what a group of sinks needs, read from the specification on 2026-09-15:

- A WebSocket transport with Noise `KKpsk2` encryption on every connection. Servers implement
  both `25519_ChaChaPoly_SHA256` and `25519_AESGCM_SHA256`; clients implement at least one.
- Pairing through CPace: a dynamic six-digit code, a static eight-digit code, or a pre-shared
  token, ending in a long-term PSK stored with the peer's identity. A connection keyed with the
  published sentinel PSK carries no playback until the client is paired or approved.
- mDNS discovery in both directions: `_sendspin._tcp` for players, `_sendspin-server._tcp` for
  servers.
- A clock filter over offset and drift, which a player must let converge before it reports
  itself available.
- Timestamped audio chunks (the play time of the first sample, in microseconds, and a
  `send_ahead`), 15 to 150 ms long, with `buffer_capacity`, `stream/clear`, group state
  (`group/update`), player-side volume, and a sync requirement of ±1 ms with playback speed
  within ±0.5%.

What it does not have is a compressed multichannel stream. The player role carries `opus`, `flac`
and `pcm`, "Servers MUST support all audio codecs", and describes mono and stereo only. Its
extension mechanism is a role whose name starts with `_`, versioned like any other
(`_vendor_role@v1`), with binary message ids from the unmanaged range 192 to 255; every
implementation must ignore payload fields it does not recognise.

So the bitstream goes in `_ac3forge_player@v1` inside a conformant session. The specification
does not change and nobody else's agreement is needed, and Music Assistant keeps using
`player@v1` with the same sink.

**Upstream, later.** Once boards play groups, the implementation is the argument for proposing
E-AC-3 to the Sendspin project: a codec in the player role and a decoder interface in
`sendspin-cpp`, the three-repository path in
[Atmos over Sendspin](esp32-player.md#atmos-over-sendspin). It is not scheduled here. If it never
lands, the extension role keeps working as it is.

### The extension role

A draft shape. [A4](#a4-sendspin)'s first deliverable is the normative page,
`planning/hearth-sendspin-extension.md`, reviewed before chip B starts.

- **Support object in `client/hello`**: the data types the sink decodes (AC-3 and E-AC-3; AC-4
  later), its sample rates (48 kHz on the boards), its output slots and the slot widths available,
  the layout grammar's version, the speaker management it offers (routing, trim range, delay
  range, crossover range, identify), the decoder settings it accepts, `buffer_capacity`, and its
  fixed pipeline latency.
- **Server to sink, JSON**: stream start for the role (data type, sample rate); the sink's
  settings (layout, routing, trims, delays, bass management, decoder settings); identify
  (channel, on or off); clear and end, as `player@v1` has them.
- **Server to sink, binary**, one id in 192 to 255: the `player@v1` chunk header (int64 play
  time of the first sample, uint32 `send_ahead`) followed by one IEC 61937 burst without its sync
  words or zero stuffing: `Pc` (data type), `Pd` (length) and the payload. A burst is 32 ms for
  AC-3, and for E-AC-3 as `Eac3BurstPacker` groups the frames.
- **Sink to server, JSON**: what the decoder found (codec, audio coding mode, substreams, objects
  rendered, dialnorm), per-output peak and RMS at about 10 Hz, underruns, late or dropped chunks,
  and a `why` string on failure, the discipline `/status` already keeps.
- **Timing**: a sink plays a burst's first sample at its DAC at the chunk's timestamp, having
  subtracted its own decode, render and DMA latency. Synchronisation corrections are applied to
  decoded PCM and never to bursts.

### Groups

- Sendspin's semantics: every member plays the same programme at the same time. Each Hearth sink
  renders it to its own layout; each standard player receives the app's stereo.
- Members in v1: Hearth sinks, test sinks and standard Sendspin players.
- Not members in v1: the computer's own output, which is not a Sendspin player and whose play
  position `src/audio` does not report yet (an in-process player in the app is the later step).
  A receiver fed a bitstream is never a member, because it reports no decode latency.
- One speaker layout split across several boards is out of scope: it needs alignment well inside
  1 ms.

### Security

- Noise and CPace as the specification sets them; device keys from a CSPRNG on each device, never
  a shared default.
- Pairing records: in the app's settings directory, readable by the user only; in NVS on a sink.
  OS keychains are a later step.
- A section in `docs/threat-model.md`: sinks accept connections on the local network, and the
  sink's page and REST API have no authentication (`planning/esp32-device-ui.md` records the
  cross-site exposure).

## The sinks

### The firmware

- The `stream_player` example becomes `hearth_sink` by `git mv`, and its CI shapes and docs
  follow.
- A Sendspin player: `player@v1` for Music Assistant, with PCM always and FLAC or Opus where
  memory and time allow, measured; `_ac3forge_player@v1` for the bitstream; the Noise responder;
  pairing codes on the serial console and the page; `_sendspin._tcp` through the `espressif/mdns`
  component.
- The player comes from `src/sendspin`, over `esp_http_server`'s WebSocket and ESP-IDF's mbedTLS.
  `sendspin-cpp` was the earlier recommendation ([esp32-player.md decision 11](esp32-player.md#decisions)).
  B3 measured both on 2026-09-16, each as a minimal player app for the S3 built at `-Os` with
  GCC 15.2 and run under QEMU with no PSRAM, paired and played 24-bit PCM by aiosendspin 9.1.1's
  server:

  | | `src/sendspin` | `sendspin-cpp` 696e75ff |
  |---|---|---|
  | Image | 521,780 bytes | 695,384 bytes |
  | Internal heap free when idle | 333,916 bytes | 307,676 bytes |
  | Least internal heap free while streaming | 305,240 bytes | 254,736 bytes |
  | Internal heap after the connection closed, against idle | 236 bytes less (the pairing record) | 35,576 bytes less |
  | WebSocket server task, stack used | 4,776 of 8,192 bytes | 4,548 of 8,192 bytes |
  | 10 s of PCM | played | stream stopped after 0.8 s, `Lost sync (-9364us off)` |

  `sendspin-cpp` completed a Noise handshake only with `noise-c` pinned to 0.1.13, which adds
  29,460 bytes; the 0.1.30 its manifest resolves to accepts only the NNpsk0 pattern on ESP-IDF.
  It links its Opus and FLAC decoders into a PCM-only player (109,050 bytes of flash), and it has
  no decoder interface and no hook for a custom role, so carrying the extension through it means
  a fork. `src/sendspin` was chosen. In `hearth_sink` on a board, starting the player (the Noise
  keys are made then) used 7,272 bytes of stack, more than the main task has to spare, so it
  starts on a 16 KB task of its own.
- Slot width is a setting. At 16 bits: 16 channels on the S3 (two lines of eight), 8 on the C6
  (one line). At 32 bits: 8 on the S3, 4 on the C6. The sink advertises the count for its current
  setting. Today a 16-bit slot width is standard I2S only, because TDM at 16 bits needs an
  interleave that is not written (`esp-idf/ac3forge/include/ac3forge/sink_plan.hpp:17-26`), so a
  board outputs at most eight channels and only the capture sink reaches twelve.
- Decoder settings at runtime. DRC mode is a Kconfig choice today.
- Speaker management on the board: routing, per-slot trim, per-slot delay, the crossover setting
  and the identify tone. Delay lines cost memory: sixteen slots of 20 ms at 48 kHz in float is
  about 61 KB, which the S3 can place in PSRAM. The C6's allowance comes from C1's measurements.
- Per-slot levels computed in the output stage, reported through the role and shown on the page.
  Today they reach only the console, at the end of a play.
- The page shows status and the settings only the board owns: name, network, slot width, output
  wiring and pairing. Playback control moves to the app and Music Assistant; `POST /play` with a
  URL stays in the REST API for debugging. The page uses 20,184 of its 20,480-byte budget, so B2
  re-derives the budget.
- Improv Wi-Fi over USB serial on both chips, and over BLE only where memory allows. Credentials
  in NVS; the build-time SSID stays for CI.

### Memory and time on each part

- **ESP32-S3**: PSRAM, and 7.1.4 in real time measured on a board
  ([esp32-714-realtime.md](esp32-714-realtime.md)). B3 measured the Sendspin player with the
  heap monitor API. Under QEMU, with no PSRAM and a 16 KB ring in internal RAM, at least
  43,700 bytes of internal heap stayed free while a stream played. On a board the 256 KB ring is
  in PSRAM, and the decoder's allocations of up to 16 KB take nearly all the internal RAM the
  network leaves: 23 and 139 bytes at the least on two boards over ten minutes, with nothing
  failing. The decode task used about 18.9 KB of its 32 KB stack, the WebSocket server's task
  5.2 KB of 8 KB, and starting the player 7.3 KB of the 16 KB it is given
  ([the sink's README](../esp-idf/ac3forge/examples/hearth_sink/README.md#on-two-boards)).
- **ESP32-C6**: no PSRAM (ESP-IDF has no external-RAM support for the part), 512 KB of SRAM shared
  with WiFi, and one 160 MHz core with no FPU, so the fixed-point tier. On the C3 the tier's
  largest fixture that fit peaked at 225,038 bytes and the 7.1.4 fixtures did not fit
  (`docs/platforms/bare-metal/esp32-c3.md`). No RISC-V board has been timed. C1 measures decode
  time and peak heap with WiFi connected before any sink work starts.

## The test sink

- `ac3hearth-testsink` is `src/sendspin`'s player half with the extension role, decoding with the
  library and the moved renderer, so below the transport it runs the boards' code paths.
- Outputs: a multichannel WAV per stream with a log of play times; a local device through
  `src/audio` with A2's routing; a null output with counters. It can emulate a board's slot count
  and slot width.
- Several instances on one host, with distinct names and ports, form a group. They share one
  clock, so their logged play times measure the group's alignment without a capture interface.
- It runs in CI, on a workstation, and in the VMware guest.
- It is not packaged; the contributor docs describe it.

## Gapless playback

- Between queue items with the same sample rate and output layout, the output stays open and no
  silence is inserted. In a group, the Sendspin stream continues and its timestamps run on.
- No container field about encoder delay or padding is read today. `mp4.hpp` says "No edit lists,
  no multiple tracks" (`src/mp4/include/mp4/mp4.hpp:35`) and nothing reads `iTunSMPB`. A3 adds
  `elst` reading to `mp4::Reader` and trims what it states. A raw elementary stream carries no
  such field, so at each join up to a frame of padding remains, plus the transform delay, which
  is 256 samples for this project's encoder (`src/forge/include/ac3/latency.hpp:29-37`).
- Each item gets a new decoder; neither decoder has a `reset()`. A continuous stream that has been
  split into files is not detected.
- Where the sample rate or layout changes, the output reopens and the app says so.
- With gapless off, the output stops and restarts between items.

## Chip A: the desktop app

Order: [A0](#a0-design-rounds) starts at once and gates only A5 and A6. A1, A2 and A3 do not
depend on the design and start alongside it, in that order. A4 starts alongside A1, and its test
sink renders once A1 has landed. A5 follows A0's sign-off and A3; A6 follows A4 and A5; A7 and A8
grow with each phase and close last.

### A0: design rounds

A design canvas (the `design` skill) with an artboard for each part of the application: the
main window at its minimum and a typical size, with the queue, now playing and transport; the
output picker listing local devices, passthrough devices, sinks, groups and standard players;
speaker setup (layout, routing matrix, identify, trims, delays, bass management); decoder
settings for AC-3 and E-AC-3, and the AC-4 page in its disabled state saying why; the monitor
(per-output meters labelled by speaker, loudness, object view); media information for AC-3,
E-AC-3 JOC and AC-4; the network pages (discovery, pairing, a sink in use by Music Assistant,
group editing, a sink's settings, reported levels beside the local decode's); settings, first
run, and About with licences. Light and dark, with keyboard focus states. Drawn from the family's
existing QML components (Theme, Card, RailBlock, SegmentedControl, FocusRing, which Crucible
copies from `apps/gui` at configure time) so what is drawn can be built.

At most two review rounds with the user, each round's feedback and changes recorded.

**Exit:** the user signs the design off in writing, and it is recorded in the repository
(`planning/hearth-design.md` with exported images), not only as an artifact link.

**Verified by:** the sign-off, quoted in that record. No QML is written before it.

### A1: the renderer moves into the library

`layout.hpp` and `render.hpp` move from `esp-idf/ac3forge/include/ac3forge/` into the library
(proposed `src/forge/include/ac3/render/`, namespace `ac3::render`), and the ESP-IDF component
includes them from there. Added beside them: a routing patch from rendered channel to output
index with unassigned outputs allowed, per-output trim and delay, an identify-tone generator, and
the crossover frequency as a setting. Their host tests move with them and grow.

**Exit:** every ESP-IDF CI shape that checks slot levels reports the same numbers before and
after the move, and the routing, trim, delay and identify cases pass on every leg, sanitisers
included.

**Verified by:** the QEMU steps of the ESP32-S3 job on the pull request (the partition, TDM,
render and 7.1.4 HTTP shapes and the stream set) and `ac3tests` on every leg.

### A2: output devices in `src/audio`

- Device records gain a channel count, speaker positions or a mask where the OS gives them, and
  supported rates, on WASAPI, CoreAudio, ALSA and PipeWire. PipeWire and CoreAudio set no channel
  count today, and ALSA's enumeration lists only HDMI and S/PDIF outputs, filtering analogue and
  USB devices out by name.
- A PCM output that opens at the device's own channel count and places each rendered channel by
  the routing patch. This also removes CoreAudio's requirement that the stream's channel count
  equal the device's.
- Playback position and output latency, flush, and pause without closing the stream. All are
  absent from `MonitorSink` today.
- Hot-plug through `DeviceWatcher` on Windows, PipeWire and CoreAudio, and by re-probe on ALSA.

**Exit:** `ac3cli outputs` lists channels and positions per device on Windows and on the Pi; a
stream routed with channels swapped plays each channel from the speaker the routing names; the
reported playback position agrees with a fake device's clock in tests.

**Verified by:** fakes in `ac3tests`; the Onkyo receiver from Windows over HDMI as an 8-channel
LPCM device, with the identify tone walked across every output and heard from the expected
speaker; the Pi for ALSA and PipeWire; macOS compiled and unit-tested in CI only.

### A3: the engine

`apps/hearth/engine/` (`ac3hearth_engine`, no Qt):

- The queue and the transport state machine.
- A session per item: container input, access units, decoder, renderer, speaker management,
  output.
- The decoder settings model over `DecoderConfig` and `OutputConfig`, with separate cut and boost
  scale factors added in `src/forge`, mix-level overrides, and dual-mono selection in the engine.
- Gapless playback, including `elst` in `mp4::Reader`.
- Passthrough, reusing `PassthroughSink` and a pure output decision in the shape of Crucible's
  `output_policy` (a mode, an endpoint and a reason), with the six sink-following gaps checked and
  closed.
- Meter snapshots released at play time, the media information model, the settings model, and a
  diagnostics ring in Crucible's pattern.

**Exit:** the `[hearth]` cases pass on every leg including ASan, UBSan and TSan (the engine's
threads tagged `[concurrency]`); the output decision's case table runs with no sound card; a
queue of mixed containers plays to a fake device gaplessly, with the expected sample count at
every join.

**Verified by:** `ac3tests` on every leg; the gain tests in `src/forge` for the cut and boost
split.

### A4: Sendspin

1. **The conformance reading.** The specification's obligations on a server (roles, codecs, both
   Noise suites), the CPace ciphersuite, pairing records and group messages, written as a
   conformance table at the top of the extension page. Where the text is ambiguous, such as
   whether a server must implement every role or may leave some unactivated, the question goes
   to the Sendspin project.
2. **`planning/hearth-sendspin-extension.md`**, the normative `_ac3forge_player@v1`.
3. **`src/sendspin/`**: JSON; messages; WebSocket over cpp-httplib behind a transport seam; mDNS
   behind a discovery seam; Noise over mbedTLS behind a crypto seam; CPace pairing; the time
   filter; the server half (discover, connect, pair, group, schedule chunks, `player@v1` with
   PCM, FLAC and Opus, and the other roles the conformance table requires); the player half
   (accept, pair, converge, schedule playout, report); the extension role on both halves.
4. **`apps/hearth/testsink/`**, the test sink.
5. **Fuzz targets** in `fuzz/` over the JSON reader, the chunk and burst parser and the handshake
   messages, and the threat-model section.
6. **The vcpkg feature** and the notices.

**Exit:**

- Two test sinks paired to the engine over loopback play one E-AC-3 JOC programme as a group.
  Each one's WAV equals a local decode and render of the same stream sample for sample from its
  logged start, and their logged play times agree within 1 ms over ten minutes.
- The engine plays PCM, FLAC and Opus to Sendspin's reference Python player.
- The Music Assistant in the user's Home Assistant pairs with a test sink and plays to it through
  `player@v1`.
- The user has reviewed the extension page. Chip B starts after this.

**Verified by:** a loopback integration test in `ac3tests`; the interoperability runs recorded in
the pull request with the versions of every other implementation involved; the fuzz targets over
their configured budget.

### A5: the application

`apps/hearth/ui/` (`ac3hearth`): the QML of the signed-off design; a controller polling the
engine's snapshot; the shared family components; `--shot <png>` with `--page <name>`; the six
languages with the lupdate gate; accessibility at Crucible's level (announcements, focus ring,
text size, contrast tests); a first-run dialog; settings through the four-argument `QSettings`
constructor; diagnostics export; About with licences from the generated notices.

**Exit:** every artboard of the design exists as a page with a `--shot` image; the
`hearth-ui` QML suites pass offscreen on Windows, Linux and macOS in CI; the `xx` pseudo-locale
shows no hard-coded string; every control works from the keyboard.

**Verified by:** the QML suites; the user comparing the screenshots with the design.

### A6: network outputs in the application

The discovery list, pairing, groups (create, add, remove, volume, mute), a sink in use elsewhere
and the takeover, a sink's settings page (layout, routing, trims, delays, bass management,
decoder settings, and its slot width shown as the sink reports it), and reported levels beside the
local decode's.

**Exit:** from the app, a group of two test sinks and the reference Python player plays one
programme; a change to a sink's layout or trim takes effect at the next burst boundary; taking a
test sink over from Music Assistant works and is reported on both sides.

**Verified by:** QML suites over a fake server; a run with test sinks. The run with boards belongs
to chip B.

### A7: packaging and CI

- A CPack component `hearth` on Windows (NSIS and zip), macOS (DMG) and Linux (DEB, RPM and TGZ,
  package `ac3forge-hearth`), with notices. Hearth becomes the first member after the library
  and Forge with a macOS package; Crucible's component exists on Windows and Linux only. The test
  sink is not in any package.
- `tools/ci/classify_changes.py` gains prefixes for `apps/hearth/` and `src/sendspin/`; an
  unmapped path lights every lane.
- The CI legs that build Qt applications build Hearth; the engine and `src/sendspin` join
  `ac3tests` on every leg; the loopback group test runs; a package check in the shape of
  `tools/ci/check_crucible_package.py`.
- The Windows `windeployqt` passes, which are ordered with `add_dependencies`, gain the third
  application.
- Which application is the default handler for `.ac3` and `.ec3`, today `ac3gui`'s, is decided
  here with the user.

**Exit:** a release dry run collects `ac3forge-hearth-*` for Windows, macOS and both Linux
architectures, and the DEB installs and starts on the Pi.

**Verified by:** CI; a `release.yml` dry run; installs on machines that have never had a build
tree.

### A8: docs and the member

The `docs/hearth/` guide rewritten around the application and the sinks: index with a status
table per platform, install, playing media, outputs and groups, speaker setup, decoder settings,
monitor, media information, sinks and pairing (chip B writes the board pages), the test sink for
contributors, troubleshooting, accessibility and localisation. Screenshots from `ac3hearth --shot`
through the script. `docs/hearth/design/` points here. Update the `mkdocs.yml` nav, the README and
`docs/index.md` rows, CHANGELOG, and [ROADMAP.md](../ROADMAP.md) (plain-English status, no new ID).

**Exit:** `mkdocs build --strict` and `tools/checks/check_doc_paths.py` pass; the script
regenerates every screenshot from the shipped interface; a reader can go from the install page to
sound from a local device using that page alone.

**Verified by:** the gates, and a walk-through by someone other than the author.

## Chip B: the ESP32-S3 sink

Starts once A4 has landed on main: the extension page reviewed, `src/sendspin` and the test sink
merged. Proven on the S3 development boards. No DAC is wired to either of them: the ES9080 design
is on paper, and the boards clock their audio out with nothing listening. So each phase's exit
below is met as far as reported levels, timing and counters go, and the parts that need a DAC
wait for one.

### B1: the firmware and its outputs

`stream_player` becomes `hearth_sink`. Slot width becomes a setting, extending the one-line
16-bit TDM support that [C2](#c2-i2s-on-the-c6) adds to two lines on the S3, or adding it here if
B1 starts first. The sink reports the slot count for its current setting.

**Exit:** on an S3 with the TDM DAC boards, 16 channels at 16 bits and 8 at 32 bits each play the
stream set with per-slot levels matching the host decode, and the width changes between plays
without a reboot.

**Verified by:** host tests of the sink planner; the QEMU capture shape at sixteen slots; the
board, with its per-slot RMS lines checked against the host decode as
`tools/checks/check_stream_set.py` does, and each DAC output listened to.

### B2: joining a network

Improv Wi-Fi over USB serial (BLE if the measured memory allows); credentials in NVS; mDNS with
the sink's name; the sink's own settings (name, slot width, wiring) in NVS; the page rebuilt as
status and sink-owned settings, with its budget re-derived and its Playwright suites updated.

**Exit:** a freshly flashed board joins WiFi from a browser over USB, appears by name in
`ac3hearth`'s discovery list, and keeps its settings across a reboot.

**Verified by:** the board; the device-UI suites.

### B3: the Sendspin player on the board

First the choice between `src/sendspin`'s player half and `sendspin-cpp`, measured and recorded
([The firmware](#the-firmware): `src/sendspin`). Then: the Noise responder, pairing codes on serial and the page, the time filter, playout
scheduled against the DAC with corrections applied to decoded PCM, `player@v1` with PCM for Music
Assistant (FLAC and Opus by measurement), the extension role feeding the component's decoder,
renderer, speaker management and sink, decoder settings at runtime, and per-slot levels and
underruns reported.

**Exit:** two S3 boards play one E-AC-3 JOC programme from `ac3hearth` as a group, each to its
own layout, with zero underruns over ten minutes and reported play times within 1 ms of each
other; the same boards play from Music Assistant; a board's reported per-slot levels match a test
sink's under the same settings.

**Verified by:** the boards; the test sink comparison; heap monitor figures recorded in the pull
request.

### B4: CI across the network

A QEMU shape of `hearth_sink` on emulated Ethernet, using the HTTP shape's port forward, with the
engine's server on the host playing to it and its slot levels compared with a test sink's.

**Exit:** the step runs in the ESP32-S3 job and fails on a level mismatch or a pairing or
protocol failure.

**Verified by:** the step failing when a deliberate mismatch is introduced, and passing without
it.

**As built:** `hearth-esp32s3` in `.github/workflows/_build.yml`, a job of its own that runs
after the ESP32-S3 job and takes that job's QEMU image as an artifact. The server is a host build
with GCC 16 and vcpkg's `hearth` feature, and Espressif's image carries neither. The engine has
no Sendspin server yet, so `ac3hearth-testserver`, on `src/sendspin`'s `ServerHost`, plays in its
place. `tools/checks/run_sendspin_qemu.sh` runs the step, and its `--board-trim-db` option makes
the deliberate mismatch.

### B5: docs

The sink guide (flashing, Improv, pairing, groups, wiring and slot widths), the ESP32-S3 platform
page, the ESP32 player plan's status block, CHANGELOG.

**Exit and verified by:** `mkdocs build --strict` and `check_doc_paths.py`; someone flashing a
board from the guide alone.

## Sink module tiers (C6 / S3 / P4)

Same `hearth_sink` family on a longer-term **shared PCB** with a modular ESP32 and a **pair of
ES9080** DACs. Ceilings: C6 **≤5.1** (one DAC); S3 **≤7.1.4 without enhanced coupling** (both
DACs, both I2S controllers, 16×16-bit); P4 **≤9.1.6 with full tools** desired (both DACs, one
I2S controller, 16×32-bit). Detail and P4 exit criteria:
[`esp32-sink-tiers.md`](esp32-sink-tiers.md). Chip B is better; Chip C is good; P4 is Proposed
best, not a chip letter here yet.

## Chip C: the ESP32-C6

C1 and C2 start at once; the board arrived on 2026-09-15. C3 follows chip B.

### C1: bring-up and measurement

`esp32c6` in the component manifest, and a probe project beside `apps/baremetal/platform/esp32c3/`.
Decode time per fixture at 160 MHz in the fixed-point tier (the default) and the float tier; peak
heap per fixture; the same again with WiFi connected and a TCP stream arriving. Recorded as
numbers on a new `docs/platforms/bare-metal/` page for the part before any sink work.

**Exit:** a table of microseconds per frame and peak bytes per fixture, both tiers, with and
without the network, measured on the board; and a statement of which streams (AC-3, E-AC-3 5.1,
7.1) decode in real time with the network up and fit in memory.

**Verified by:** the board. CI builds the manifest target; whether QEMU emulates the C6 is
checked and stated.

### C2: I2S on the C6

The C6's TDM frame limit, read from ESP-IDF and confirmed on the board. The 16-bit TDM
interleave and the sink planner change for one line (the interleave `sink_plan.hpp:17-26` names as
not written), then 8 × 16-bit and 4 × 32-bit on the C6's one controller.

**Exit:** the C6 plays eight channels at 16 bits and four at 32 bits to a TDM DAC with per-slot
levels matching the host decode.

**Verified by:** host tests of the planner; the board with a TDM DAC board.

### C3: the sink on the C6

After chip B: `hearth_sink` built for the C6, with PCM-only `player@v1`, the extension role,
Improv over serial and mDNS, and `buffer_capacity` sized from C1's figures.

**Exit:** a C6 joins a group with an S3 from `ac3hearth` and plays within the group's alignment
with zero underruns over ten minutes, or the stream shapes it cannot carry are stated with C1's
numbers.

**Verified by:** the boards.

## Chip D: the AC-4 decoder

A follow-on. The app is designed for AC-4 from A0; playback waits for this chip.

### D0: the plan

`planning/ac4-decoder.md`, in this repository's plan shape:

- Scope by phase. For example: channel-based 2.0 and 5.1 first (the transform, the stereo and
  multichannel tools, A-SPX, A-CPL, DRC, dialogue enhancement, loudness, presentation selection,
  main and associated mixing, downmix); then 7.1 and channel-based immersive; then A-JOC objects.
- The verification problem. No local tool decodes AC-4 audio: the Dolby Reference Player's
  `dlbac4dec` produced no samples when it was tried for the AC-4 inspector (IM4). The Dolby
  Encoding Engine install encodes AC-4 from known WAV sources, so decoded output can be scored
  against the source. Whether decoder conformance streams are published is checked.
- What `src/ac4` has to grow: dialogue enhancement and DRC payloads, object substream groups.
- The fixed-point tier and the ESP32 question.
- The decisions.

**Exit:** the user takes the plan's decisions.

**Integration:** the app's AC-4 pages activate when a decoder lands; the extension role gains an
AC-4 data type (IEC 61937-14 defines AC-4 carriage, to be confirmed against the standard).

## What cannot be verified, and why

| Claim | Can it be verified | Blocker |
|---|---|---|
| The macOS application and CoreAudio output work on a Mac | **no** | No Mac. CI compiles it and runs its unit and offscreen QML tests, as for Crucible |
| Two boards are aligned at their DAC outputs | **partly** | Reported play times measure the software schedule; no capture interface is available to record both analogue outputs together |
| Music Assistant's Sendspin server matches the specification version implemented here | **at a version** | Sendspin is moving; each interoperability run records the versions |
| Decoded AC-4 audio is correct | **partly**, after chip D | No local AC-4 decode oracle; scoring against DEE's source WAVs measures closeness, not identity |
| A sink's outputs are heard, at either slot width | **no** | No DAC hardware exists. The ES9080 design is on paper, so a board clocks its audio out with nothing listening: levels, timing and counters are measured, and nothing is heard |
| Group alignment holds on a congested WiFi network | **partly** | Measured on the user's network only |
| A screen-reader user can operate the application | **partly** | Automated role and name checks; no screen-reader pass has been done on any member |
| Pairing and Noise interoperate with Sendspin implementations beyond the two partners | **at a version** | The reference Python player and Music Assistant are the partners available |

## Coordination

- Before each phase: `gh pr list`. Branch names are `<type>/<kebab-name>` with type one of
  `feature`, `bugfix`, `hotfix`, `docs`, or `chore`, which CI's branch-name gate requires.
- A1 moves headers the ESP-IDF component and its QEMU steps include. An open ESP32 branch that
  edits `layout.hpp` or `render.hpp` lands first, or rebases onto the move.
- The 16-bit TDM interleave and planner change belong to whichever of C2 and B1 starts first; the
  other builds on the merged change.
- Files more than one chip edits: `CHANGELOG.md`, `vcpkg.json`, the root `CMakeLists.txt`,
  `tools/ci/classify_changes.py`, the `_ci-*.yml` and `_build.yml` workflows, `mkdocs.yml`,
  `docs/threat-model.md`, and `tools/checks/check_doc_paths.py`, where every new planning page
  joins `PROSE_PATHS_UNCHECKED`.
- The appliance plan's Phase 1, the sink-following gaps in `ac3cli play`, is done inside A3.
- Unaffected: driver signing under `apps/windows/`, Crucible, the ESPHome component.

## Deliberately not in scope

- A headless daemon, a web control page on a computer, a kiosk window, an SD-card image.
- An HLS/CMAF client in Hearth. Topology decision 1's transport stays open to other members.
- The computer's own output as a group member in v1, and a receiver fed a bitstream as a group
  member at any time.
- One speaker layout split across several boards.
- Room correction, equalisation, measurement-driven filters, binaural or headphone rendering.
- Video, a media library or tag database, artwork scraping, streaming-service clients.
- Encoding.
- Proposing E-AC-3 to the Sendspin project in v1.
- An ESPHome `media_player`, which keeps its own schedule in the ESP32 player plan.
- TrueHD.
- Renaming any published identifier other than adding this member's own.

## Decisions

Twenty questions were put on 2026-09-15. Five answers went against the recommendation and are
marked. Six were given in the user's own words rather than as one of the options offered (2, 7,
10, 13, 14 and 15), and are recorded here in shortened form.

| # | Question | Recommended | **Taken** |
|---|---|---|---|
| 1 | The headless appliance | Replace it with a headless host sink on the same engine | **Drop the headless form** ← against |
| 2 | What Sendspin support means | (three options: sinks as players, the app playing to players, the protocol extending Sendspin) | **Sinks are Sendspin players; the bitstream travels over the network, in a protocol that extends Sendspin later or stays separate** |
| 3 | Where the design rounds happen | The first phase of the desktop chip | **The first phase of the desktop chip** |
| 4 | Passthrough as an output mode | Yes | **Yes** |
| 5 | How the stream relates to Sendspin | Separate, in Sendspin's shape | **Separate, in Sendspin's shape**, later superseded by 13 and 17 |
| 6 | What the stream carries | IEC 61937 bursts without padding | **IEC 61937 bursts without padding** |
| 7 | AC-4 | Design for it; the decoder separate | **Design for it; the decoder is a follow-on chip** |
| 8 | The ESP32-C6 | A feasibility gate first | **Commit to it; the boards arrived the same day** ← against |
| 9 | TDM slot width | Selectable per board | **Selectable per board** |
| 10 | When the bare-metal work starts | Now, hardware first | **After the desktop protocol lands, proven on S3 boards; C6 bring-up as its own chip** ← against |
| 11 | Networking dependencies | cpp-httplib and `mdns` | **cpp-httplib and `mdns`** |
| 12 | Groups in v1 | One output at a time | **Synchronised groups** ← against |
| 13 | What a group is | (multi-room, a split layout, or both) | **Sendspin's groups rather than a new design: become a Sendspin client with an extension** |
| 14 | Which outputs join a group | Hearth sinks only | **Whatever is a stepping stone to a Sendspin client with E-AC-3**: sinks, test sinks and standard players |
| 15 | Verification hardware | (asked) | **Two or more S3 boards with TDM DACs, the Onkyo over HDMI from a PC, and a multichannel test sink to build**. The DAC boards have not arrived: every board run so far has had nothing on its I2S pins |
| 16 | Speaker setup in v1 | (asked) | **Identify tone, per-output trim, per-output delay, bass management** |
| 17 | When encryption arrives | The profile now, Noise second | **Fully conformant in v1** ← against |
| 18 | The sink's page | Status and sink-owned settings | **Status and sink-owned settings** |
| 19 | Joining WiFi | Improv Wi-Fi | **Improv Wi-Fi** |
| 20 | Names | `ac3hearth` and `hearth_sink` | **`ac3hearth`, `hearth_sink` and `ac3hearth-testsink`** |

Decision 15 brought part of decision 1's recommendation back in another form: the headless sink
returned as a test tool, unpackaged.

**Consequences of the answers, recorded rather than asked**, each open to change when this page
is reviewed:

- A conformant server supports Opus, FLAC and PCM for `player@v1`, so libFLAC and Opus become
  dependencies, and `ac3hearth` can play stereo to any Sendspin player.
- mbedTLS provides the cryptography at both ends: ESP-IDF ships it, and vcpkg has a port. Final
  once A4 has read the CPace ciphersuite from the specification.
- JSON is written in-tree, since the tree's only JSON reader is private to `ac3::oba`.
- The new dependencies sit behind a vcpkg manifest feature, `hearth`.
- Sendspin's reference time filter is vendored with its licence.
- The renderer moves from the ESP-IDF component into the library.
- The Sendspin work is a phase of the desktop chip rather than a chip of its own, because the
  engine, the test sink and the server share one owner until chip B starts.
