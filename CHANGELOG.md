# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this
project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

See [README.md](README.md) for the project overview and
[Releasing](docs/releasing.md) for the release process.

## [Unreleased]

This release adds:

- Hearth's ESP32-S3 Sendspin sink, desktop engine, and development tools;
- fixed-point decoding for ESP32-C3 and ESP32-C6, with real-time ESP32-S3 work;
- Crucible on Windows and Linux, with macOS code built and tested in CI;
- per-channel quality gates and continued performance, quality, and memory histories;
- AC-4 container support, wider WebAssembly encoding, microphone capture, and expanded Rust
  bindings.

The sections below contain the complete change list and fixes.

### Added

**Associated-service identification, both directions**

- **MPEG-TS's `mainid`/`asvc` now read back, not just write.** `mpegts::demux`/`Reader` decode
  the PMT's own AC-3/E-AC-3 audio descriptor into `ReadStream::service` — `bsmod`, `full_service`,
  `mainid`, `asvc`, `bsid`, `mix_metadata` and the `substream1`–`3` bytes, for both the DVB and
  ATSC profiles — where before only the descriptor *tag* was read, to identify the codec.
- **`ac3cli ts`'s `asvc=` accepts a comma-separated main-service list** (`asvc=0,2`) alongside the
  existing raw mask (`asvc=0x05`), and `mainid=`/`asvc=` are now checked against the stream's own
  `bsmod`: giving `asvc=` on a stream `bsmod` calls a main service, or `mainid=` on one it calls
  an associated service, is a usage error instead of a descriptor that silently says the wrong
  thing.

**Minimum-footprint / ESP32 decode profile**

- **A Hearth sink knows what it is, joins a network it was told about, and is
  found by name.** What the board is — its name, the network it joins, how wide its
  DAC's slots are and whether a second I2S line is wired — lives in NVS rather than in
  the image, so none of it needs a reflash. A board with nothing stored still behaves
  exactly as the build says, which is what keeps CI unchanged. Two ways in: **Improv
  Wi-Fi** over the same USB serial port the console uses, which is how a board with no
  network at all is told about one; and `PUT /name`, `/wiring`, `/network` and
  `/slot-width` over the REST surface for a board already on one. Once it has a
  network it advertises **`_sendspin._tcp` over mDNS**, port 8928 with `path=/sendspin`,
  which is what a Sendspin server looks for. The network now comes up at boot rather
  than at the first play, because a sink is found before it is played to. Costs about
  8 KB of internal RAM for mDNS on every shape; the Improv listener's 4 KB is only
  spent on a board that has no network to join, since that board is not decoding
  anything.
- **A Hearth sink plays as a Sendspin player** (`hearth_sink` with `sdkconfig.sendspin`).
  The board pairs with a server by its token or by a six-digit code on the console and its page,
  over Noise, and follows the server's clock with Sendspin's time filter. Compatibility with the
  aiosendspin 9.1.1 server library used by Music Assistant is validated in CI; Music Assistant
  itself has not been tested. The `player@v1` role carries stereo PCM. Hearth's
  `_ac3forge_player@v1` sends AC-3 or E-AC-3 with any Atmos objects, which the board decodes and
  renders to its own layout, routed, trimmed and delayed as the server's settings say. Each
  sample leaves the I2S port when the server asked:
  playout is scheduled against the channel's end-of-frame interrupts, and corrections are made
  to decoded PCM. Per-output peak and RMS, underruns and play times are reported on the page, in
  `/status` and to the server. Two ESP32-S3 boards played one E-AC-3 JOC programme as a group
  for ten minutes over Wi-Fi, one at 2.0 and one at 5.1, with no underrun and their play times
  within 549 µs. The player is `src/sendspin`'s player half: measured against `sendspin-cpp`,
  it took 173,604 bytes less flash and left 50,504 bytes more internal RAM free while streaming.
  [An ESP32-S3 sink](docs/hearth/sink-esp32-s3.md) is the guide: flashing, Improv, pairing,
  groups, wiring and slot widths. A new CI job, `Hearth Sendspin sink (ESP32-S3, QEMU)`, plays
  to the emulated board from `ac3hearth-testserver` and holds its levels to a test sink's. In
  this shape the console listens on every board, for the pairing commands, so the Improv
  listener's 4 KB is spent whether or not the board has a network.
- **Sixteen channels out of an ESP32-S3, and the slot width as a setting.** An I2S
  line carries 128 bits a frame, so the two the part has reach sixteen 16-bit slots or
  eight 32-bit ones — a 7.1.4 layout leaves through the `i2s` sink for the first time,
  where eight channels was the ceiling before. The width is no longer fixed when the
  image is built: `GET` and `PUT /slot-width` beside `/layout` change it between plays,
  `/status` reports it as `slot_bits`, and the sink's ceiling moves with it, since which
  width a board wants is a property of the DACs it is wired to rather than of the
  firmware. A change is refused while a play is running, and takes effect at the next
  one. Kconfig still sets the width the sink starts at.
- **A fixed-point decode tier** (`-DAC3FORGE_DECODE_SCALAR=fixed`), a Q7.24 integer
  scalar path for parts with no FPU (an ESP32-C3, a Cortex-M3), joining `double` and
  `float` on the decode-scalar axis. Measured at 121 dB+ on the gold streams and 111 dB+
  on every third-party fixture, with byte-identical bitstreams; on the Cortex-M3 leg,
  enhanced coupling drops from 28.9M instructions/frame to 10.1M and E-AC-3 5.1 from
  12.9M to 4.8M. Covers Annex E's tools too (AHT, spectral extension, enhanced
  coupling); JOC object reconstruction stays `float` in every build. See
  `planning/arithmetic-tiers.md`.
- **An ESP32-C3 target** for the minimum-footprint profile
  (`apps/baremetal/platform/esp32c3/`), decoding in the fixed-point tier since the part
  has no FPU. CI builds and runs it under `qemu-riscv32`: 12 of 14 fixtures decode with
  PCM identical to the x86 host and Cortex-M3 legs; the two 7.1.4 rows need more heap than the
  part's largest free block and are declared skipped rather than silently missing. Speed
  is unmeasured — QEMU isn't cycle-accurate.
- **An ESP32-C6 target** (`apps/baremetal/platform/esp32c6/`), with `esp32c6` in the ESP-IDF
  component's manifest, timed on a board with no network and with WiFi connected and a
  1,536 kbit/s TCP stream arriving (a network load the probe project can build in). All
  fourteen fixtures decode with PCM identical to the other fixed-tier legs. With the network up,
  AC-3 and E-AC-3 5.1, stereo and mono decode in real time (after the fixed-point arithmetic
  change under Changed), E-AC-3 7.1 does not, and 7.1.4 fits only with ESP-IDF's WiFi IRAM
  options off. QEMU does not emulate the part, so CI builds it and runs nothing. See
  `docs/platforms/bare-metal/esp32-c6.md`.
- **`hearth_sink`'s Sendspin player runs on the ESP32-C6**, the part's single core doing double
  duty as the WebSocket server and the decode task. A clock reply is now dated by when its bytes
  reached the board rather than by when the server task got to read them: lwIP's IPv4 input hook
  (`ac3forge/tcp_arrivals.hpp`, `ESP_IDF_LWIP_HOOK_FILENAME`) logs each Sendspin connection's TCP
  stream as its segments arrive, well above the decode task, and `PlayerSession::receive()` takes
  that time instead of `esp_timer_get_time()` at the read. Without it every reply the server task
  read while a burst decoded looked as late as the decode, and once thirty such bursts in a row
  had been left out of the clock's filter the offset jumped 13 to 31 ms; with it a ten-minute play
  kept every reading within 651 us of the server's own clock. Quad SPI flash reads
  (`CONFIG_ESPTOOLPY_FLASHMODE_QIO`) left the part 6 to 9% idle while a stream played, where
  DIO left about 1%, and cut a burst's decode and render from 22.4 to 20.7 ms. The Sendspin ring
  is 48 KB (`sdkconfig.sendspin-c6`, up from 32 KB): a WiFi link that goes quiet for close to a
  second, seen a few times an hour on this network, drains a smaller ring before it recovers, and
  the chunks queued behind the gap arrive too late to play; 48 KB cut how often that happened by
  about two thirds with no allocation ever failing, where 64 KB stopped it in a ten-minute run at
  the cost of the same WiFi receive-buffer allocation failures the IRAM options above are there to
  avoid. One ESP32-C6 and one ESP32-S3, both running `hearth_sink`, played one programme from
  `ac3hearth-testserver` as a group for ten minutes with zero underruns on either board and a
  479 us worst spread between their play times, inside B3's 1 ms group criterion. AC-3 and
  E-AC-3 5.1 do not fit the player's memory budget once the ring, the WebSocket
  server and WiFi's own buffers are all resident: the decoder's scratch allocation failed 10 to
  12 seconds into a 5.1 stream in each of two runs, one AC-3 and one E-AC-3, and by then the heap
  was short enough that even the C++ exception the failed allocation threw could not itself be
  allocated, which aborted the board rather than closing the stream - `outputs.count` in the
  role's capability advertisement bounds routing, the stage after decode, and did nothing to
  stop a server sending one. `BurstPlayerConfig::max_coded_channels`
  (`CONFIG_AC3FORGE_EXAMPLE_SENDSPIN_MAX_CODED_CHANNELS`, 2 on this board) now refuses a wider
  syncframe before a decoder opens for it, in every build that sets it. See
  `docs/platforms/bare-metal/esp32-c6.md`.
- **`delta_allocation`** on `EncoderConfig`/`eac3::FrameConfig` (`delta=off`): the first
  rung of an effort axis for parts with little time for the §7.2.2.6 search. Removes
  about 9 ms of an ESP32-S3 E-AC-3 5.1 frame for 0.01 dB on the worst channel of the
  E-AC-3 gold streams.
- **An `f32x4` SIMD lane** alongside `f64x2`/`i32x4` in the arch seam (roadmap PF7): the
  float32 IMDCT twiddle stages now vectorise under SSE2/NEON, pinned bit-for-bit against
  scalar `float` including denormal underflow.
- **Block-granular decoder output**
  (`decode_frame_by_block`/`decode_access_unit_by_block`): PCM delivered 256 samples at
  a time through a non-owning `BlockSink` callback instead of a whole frame, cutting a
  DMA-fed caller's storage need from a frame to a block (73,728 bytes for 7.1.4 on an
  ESP32-S3) and taking 73,824 bytes out of the footprint probe's `.bss`. Pinned sample-
  for-sample against the frame forms.
- **A web page on the ESP32 player** (`esp-idf/ac3forge/ui/`), served at `/`: state,
  codec, layout, volume, per-frame stage timing and ring depth, with
  play/stop/volume/layout controls over the existing REST routes. 16,190 bytes against a
  16,384-byte budget; tested in Chromium and on the emulated board.
- **A Hearth sink reports its network and its firmware.** `GET /status` gains `network` - the
  link (`wifi`, or `ethernet` under QEMU), the access point's SSID and signal in dBm, and the
  board's address - from `ControlHandlers::network`, and `GET /hardware` gains `project`,
  `version` and `idf_version` from the image's own description. The page shows both.
- **The ESP32 web page explains the output layout**, showing this play's fold, each
  channel's speaker or object placement, and the speakers left silent; `GET /status`
  gains `sink_slots`, `stream.layout`, `render`, `coded` and `silent`. A 38-stream set
  for the `http` source exercises every layout and coding tool; CI plays it under QEMU
  onto 7.1.4, holding every slot to the host's level. Found and fixed two player bugs: a
  dual-programme stream played both, and a 44.1/32 kHz stream played at the wrong speed
  (now refused).
- **The ESP32 player can hold a play's first unit** (`PlayerConfig::hold_first_unit`),
  queuing two frames before the sink starts rather than one, since a play's first frames
  decode more slowly than the rest and were running the DAC dry over WiFi at 7.1.4.
  Combined with a 32 KB instruction cache, this takes 3.1–3.8 ms off decode; CI's
  default config plays through the hold.
- **The ESP32 output layout takes speaker size and height realization**: a `:small`
  speaker redirects its bass to the LFE through a matched Butterworth pair, and
  `:height`/`:top`/`:upfiring` distinguish how a height position is physically realized
  without changing the render.
- **The ESP32 streaming example's I2S sink can hold every layout to the full TDM frame**
  (`AC3FORGE_EXAMPLE_I2S_FIXED_FRAME`), mono and stereo included, for a TDM DAC set up
  over I2C for one frame shape, such as an ESS ES9080; a second line then runs zeroed
  slots for every layout. `ac3forge::plan_sink` takes the choice as a `SinkFrame`. On an
  ESP32-C6 a 2.0 play opened eight 16-bit slots with levels and frame time unchanged, for
  12 KB more DMA buffer.
- **The ESP32 streaming example's I2S sink reconfigures itself** instead of needing a
  rebuild: `PUT /layout` takes effect at the next play via `i2s_channel_reconfig_*` or a
  channel recreate when it crosses standard/TDM modes, replacing the old build-time
  stereo/TDM split. `AC3FORGE_EXAMPLE_I2S_SECOND_LINE` brings up a second I2S line
  sharing the first's clocks, doubling the slot ceiling to eight; verified on an
  ESP32-S3-DevKitC-1-N16R8, though a six-channel unfolded layout with both lines up left
  too little RAM for the decode task's stack.
- **A part with no floating-point unit converts a sample to an I2S slot in integer
  arithmetic** instead of `float`: `to_pcm16_from_bits`/`to_slot_24in32_from_bits`
  (`ac3forge/interleave.hpp`) compute the float conversion's own result from the
  sample's IEEE-754 bits, equal to it for every input that is not a NaN. The component
  chooses the conversion from `CONFIG_SOC_CPU_HAS_FPU`; an ESP32-S3's sink is unchanged,
  confirmed identical object code and, on a board, identical timing. On an ESP32-C6
  playing a 7.1 stream onto eight 16-bit TDM slots, `sink_us_per_frame` drops from
  20,875 to 12,689 microseconds a frame, 11,551 with the sink's source at `-O2`
  (`AC3FORGE_MINIMAL_HOT_O2`); levels unchanged to the digit.

**Crucible desktop application**

- **AC3Forge Crucible is built and tested on Windows and Linux, with its macOS code built and
  tested in CI.** The Windows release asset is
  `ac3forge-crucible-<version>-win64.zip`. It carries the driver's install and remove scripts
  only. The test-signed driver must be built from source until attestation signing is in place.
- **Per-process loopback capture and endpoint change notifications on Windows** (roadmap
  UX11): `Capture::start_process_loopback` taps one process tree's render output at a
  caller-stated format (Windows 10 build 20348+), and `DeviceWatcher` delivers endpoint
  add/remove/state/default-changed events on a callback instead of requiring polling.
  Every other backend refuses both honestly.
- Crucible places each captured application in a room and streams E-AC-3 JOC over HDMI or
  AC-3, PCM, Spatial Sound, or stereo as the endpoint requires. `ac3crucible-run` is the
  console runner and `ac3crucible` is the Qt Quick window.
- `MonitorSink::start` takes a `low_latency` flag: on Windows it asks `IAudioClient3`
  for the engine's smallest shared-mode period, falling back to the default where
  unsupported; other backends ignore it.
- The demo's engine flushes its taps on output start/switch and bounds the PCM sink's
  queue at two frames, so a pipeline's start-up offset no longer becomes the session's
  latency.
- Coverage on Windows: a clang-cl arm (`cmake/Coverage.cmake`), the `config-windows-
  llvm-coverage` preset, and `tools/checks/coverage_windemo.ps1` report per-file
  line/branch coverage over `apps/windows`.
- **Crucible can be operated without a mouse, and describes itself to a screen reader**
  ([Keyboard and screen readers](docs/crucible/accessibility.md)): every control is a
  tab stop, the room is a keyboard-navigable focus scope, and every element carries a
  role/name/description built from the same live state the window draws — announced on
  every meaningful change. **Settings → Appearance → Text size** (100–175% or System)
  scales the whole window. Requires Qt 6.8+ for `Accessible.announce`; no screen reader
  has been run against the window by hand yet.
- **Applications have their own icons on Linux**, resolved from PipeWire's icon name, a
  matched `.desktop` entry, the binary's own theme entry, or a monogram, in that order.
  Qt SVG is an optional dependency for SVG-only icons.
- **Crucible explains itself on first run, and restores the default output on quit**: a
  first-launch dialog names the silent device and offers to move the default output
  automatically; quitting (tray or window) restores the previous default when Crucible
  moved it. On Linux, this also creates the "Crucible (silent)" node.
- **Crucible saves a diagnostics file** (Settings → Save diagnostics…): version,
  platform, engine counters, endpoints, applications, the signal path and recent log
  lines — never the signing key or its path.
- **Every Crucible package carries its third-party notices, and About has a Licences
  view** (`apps/crucible/notices/`): `NOTICES.txt` is generated per platform at
  configure time from the actual component list and versions, so the window and the
  package cannot disagree; `check_crucible_package.py` enforces it.

**Hearth**

- **The Play page's queue rows and "Now playing" line, and the Decoder page's "This stream" and
  "Programme" cards, read the same per-item media information the Media page does.** A queue row
  carries its own codec chip (`A3`/`E3`, or `A4` from the file's own extension for an AC-4 item a
  probe never reaches), a "playing"/"not playable" pill, a metadata line (stream kind, channels,
  sample rate, measured bitrate, duration) and, under the item playing now, a progress bar
  (`HearthController.positionMs`/`durationMs`). "Now playing" gains its own metadata line - codec,
  layout and objects, sample rate, bitrate, container, and a "next: <item>, gapless" hint. The
  Decoder page's new "03 This stream" card reads dialogue level, dynamic range, heavy compression,
  mix levels and objects off `HearthController.currentMedia`; "04 Programme" (renamed from "Dual
  mono", which moves into it) adds a read-only picker over the stream's own programme list -
  `Session::open()`'s own comment says why picking a different one has no setter yet.
- **The Media page reads a queue item's own file** (`Media.qml`;
  `HearthController.currentMedia`/`inspectedMedia`, backed by `apps/hearth/engine/media_info.hpp`'s
  already-built `MediaInfo` and `MediaInspector`, off a thread of their own): codec, sample rate,
  measured bitrate, duration and container for AC-3, E-AC-3 and AC-4 alike; dialogue level, mix
  levels and the programme list for AC-3/E-AC-3; the table of contents, presentations and
  substream groups for AC-4, which this build still cannot play but can now describe. A "Showing"
  picker follows the item playing now by default and can point at any other queue item instead -
  the only way to reach an AC-4 item's own information, since it never plays here. Copy and Export
  JSON reuse `media_info_json()`'s own document. `ItemFacts` gains a measured `bitrate_kbps`, and
  the file loader accepts `.ac4` for reading (never for playback) so the Media page can describe
  one.
- **The Media page's Objects · OAMD table, Container card, and AC-4 Immersive card.**
  `describe_media()` now walks with `detail`/`on_access_unit` on, capturing the first OAMD
  payload's full per-object detail (`MediaInfo::objects`) alongside the bed/count summary
  `probe->program` already read; a new "Objects · OAMD" card reuses `display_object_to_map()`
  (the Play page's own monitor) for a position/gain/snap/active table, one row per
  `oba::describe_objects()` result. A new Container card (format, track, edit list, dec3/codec
  box, or MPEG-TS's programme/PID/stream type) shows the container facts
  `media_container_to_map()` already computed but nothing read; AC-4's new "Immersive" card says
  whether an A-JOC substream is present, honestly, since this build's inspector reads no further
  into it. Card numbers on both sides of the page now run as one sequence over whichever optional
  card actually renders, rather than the AC-4 cards' own fixed 03-05 that skipped 04/05 on every
  non-AC4 file. Lt/Rt mix levels join the existing Lo/Ro pair, EMDF payload ids show their own
  name ("OAMD (11)"), dynamic range and heavy compression show their real dB range rather than a
  bare carried/not-carried boolean, and the Container row no longer reads an item not yet probed
  as a confirmed elementary stream.
- **`ac3hearth` gets an About dialog and a Licences view of the generated notices.** About
  states what the player does, its version and build provenance, and the GPL/Dolby-trademark
  line, with a Licences… button that opens the full third-party `NOTICES.txt` this build
  embeds. A new "?" button in the header opens About; `--page about`/`--page licences` open
  either one directly, for a capture.
- **`ac3hearth` packages, as `ac3forge-hearth`, on Windows (NSIS and ZIP), macOS (DMG) and
  Linux (DEB, RPM and TGZ).** Its own CPack component follows `apps/crucible`'s own pattern -
  notices and licence beside the executable, Qt's runtime deployed into the package - except
  that it ships in the shared NSIS installer and the CI-built RPM, since neither of Crucible's
  reasons for staying out (a test-signed driver, no RPM host to verify against) applies to it.
  `ac3hearth` also becomes the `.ac3`/`.ec3` handler on all three platforms, taking that role
  from `ac3gui`.
- **The Sendspin time filter learns faster and ignores delayed replies.** Once it has
  converged, `ac3::sendspin::ClockSync` runs thirty bursts a second apart before settling to one
  every ten seconds. It leaves out a burst whose best reply is well above the recent floor, since
  clock replies that wait behind a stream's chunks would otherwise read as a change of offset.
- **cpp-httplib's WebSocket reads wait out a frame split across packets.** Its 0.56 port
  failed a connection when a read's timeout fell inside a frame, which on Wi-Fi broke pairing
  with a board. The overlay port carries a patch: a read that has begun a frame waits for the
  rest until the connection closes.
- **`src/sendspin`, the first part of Hearth's Sendspin implementation**
  (`planning/hearth-sendspin-extension.md`): an in-tree JSON reader and writer, strict
  base64url, transport-mode fragments and the `player@v1` audio chunk in both the
  specification's forms and those of aiosendspin 9.1.1 (the version used by Music Assistant),
  and the `_ac3forge_player@v1` burst chunk. Built with `-DAC3FORGE_BUILD_HEARTH=ON`. The
  JSON reader parses into caller-owned storage without recursing, and refuses invalid
  UTF-8 and duplicate keys. Two fuzz harnesses (`fuzz_sendspin_json`,
  `fuzz_sendspin_frames`) and a CI job, `Hearth Sendspin (Linux, GCC)`, cover it.
- **Sendspin's encryption in `src/sendspin`**: Noise `KKpsk2` for both of the
  specification's suites (`25519_ChaChaPoly_SHA256` and `25519_AESGCM_SHA256`), matching the
  cacophony test vectors byte for byte, with the PSK bound late as Sendspin needs and the
  Sentinel retry; the handshake messages (`client/init`, `server/init`, `server/error`,
  `noise/handshake`) with the specification's order of `server/error` reasons; and PSK
  identities. The cryptography sits behind a seam over the PSA Crypto API, which both
  vcpkg's mbedTLS 3.6 and ESP-IDF's mbedTLS 4 provide, and comes in through vcpkg's new
  `hearth` feature. A third fuzz harness, `fuzz_sendspin_handshake`, reads the handshake
  messages.
- **Sendspin's pairing values in `src/sendspin`**: CPace (CPACE-X25519-SHA512 from
  draft-irtf-cfrg-cpace-21, matching the draft's test vectors byte for byte: Elligator 2 is
  written in-tree, and the scalar multiplications go through the crypto seam's X25519), and
  around it the pairing tokens, the dynamic pairing code, the commitment to `nonce_B`, the
  CPace session id and the wrapping of the long-term PSK and `nonce_B`, each in both the
  specification's form and aiosendspin 9.1.1's where the two differ.
- **Sendspin's reference time filter**, vendored unmodified into
  `src/sendspin/third_party/time-filter` for the player half's clock synchronisation.
- **Sendspin's WebSocket transport in `src/sendspin`**: the seam every session runs over,
  with an in-memory pair for tests and loopback groups, and plain `ws://` over cpp-httplib in
  both directions the specification allows, a listener and a dialler. A close from any thread
  reaches a waiting reader within 100 ms whether or not the peer answers it, a message longer
  than one Noise message ends the connection, and a second listener on a port that one
  already holds fails to start, where cpp-httplib's default socket options would let it share
  the port. cpp-httplib joins the `hearth` feature at 0.56.0 through an overlay port, ahead of
  the vcpkg baseline's 0.52.0, for the read timeout that makes the close possible.
- **Sendspin's core messages in `src/sendspin`**: `server/hello` through `group/update` with
  `player@v1`'s objects, each a struct with a writer and a reader in both the specification's
  form and aiosendspin 9.1.1's, and the `client/hello` field that tells a 9.1.1 client apart.
  Readers ignore what they do not recognise where the specification says to. Standard Base64
  for `codec_header`, and a fourth fuzz harness, `fuzz_sendspin_messages`, which reads every
  message in both dialects and checks that what it writes back reads back the same.
- **Sendspin's handshake as two state machines in `src/sendspin`**: the server's and the
  client's side of `client/init` through Noise message 2, fed the frames they receive and
  answering with the frames to send, so a thread on a computer and a board's WebSocket handler
  drive the same code. They choose the PSK as the specification says, including the client's
  Sentinel fallback and the credential-mismatch signal it gives the server, tell an
  aiosendspin 9.1.1 server apart by its message 1, and run re-handshakes. A transport-mode
  channel seals messages into Noise ciphertexts, one per frame in the connection's dialect,
  and opens them again.
- **Sendspin's clock synchronisation for the player half**: `client/time` exchanges in bursts
  of eight over the vendored time filter, one after another until the clock converges and
  every ten seconds after, with convergence taken as the filter's error staying under 1 ms
  for eight updates in a row. Against a simulated server 35 ppm fast over a 0.5 to 3 ms
  network it converges in under two seconds and stays within 1 ms.
- **Sendspin's server and player sessions in `src/sendspin`**: one connection each, from the
  handshake through `server/hello`, `client/hello` and `server/activate` to a `player@v1`
  stream whose chunks the player receives on its own clock, with commands, group updates,
  unpairing and re-handshakes. Like the handshake machines they are fed frames and the time
  and answer with frames, so a board can run the player's. The player checks each activation
  as the specification's admissibility rules say; the server refuses what the specification
  does not allow at that moment, such as a stream to an unavailable player or a command it
  did not list.
- **Sendspin's pairing flows in `src/sendspin`**: the Pairing PSK Flow, the Dynamic Pairing
  Code Flow in digits or as a QR token with its retry rounds and round limit, and the Static
  Pairing Code Flow behind its gesture window, from both the client's side and the server's,
  in the specification's form and aiosendspin 9.1.1's. The server checks the client's tag,
  the commitment to `nonce_B` and the binding of the typed code to the handshake in the order
  each dialect uses, and two ends of different handshakes cannot pair whatever code is typed.
- **Pairing in Sendspin's sessions**: a pairing activation on either session runs one attempt
  of the method it names. The server session checks the method against the client's offer and
  the matched PSK, takes the operator's code, and once its listener has stored the record
  acknowledges and re-handshakes to the new long-term PSK in the same output; the player
  session emits the code, waits for a gesture or the round limit's reset where the method
  says, and sends nothing but pairing messages until the re-handshake, which Music Assistant
  expects. Cancels from either side, a new activation that supersedes the attempt, the
  player's two-minute attempt timeout and the server's own timeouts are covered, and a
  static code's window admits attempts only on the connection that carried its first. The
  pairing messages join `fuzz_sendspin_messages`.
- **A session driver for Sendspin on a computer**: `SessionDriver` runs a server or player
  session over one connection with a reader thread and a writer thread, which sends the
  session's frames in order outside its lock and ticks it when due, and disconnects a peer
  that stops reading once a bounded queue fills. Over a loopback WebSocket a server pairs a
  player with its pairing PSK, activates it under the new long-term PSK and streams PCM that
  the player receives within 2 ms of each chunk's time.
- **Admission between Sendspin servers**: `Arbiter` decides which server's connection a client
  holds, as the specification ranks them (playback above pairing above nothing, equal or
  higher displacing the holder), with its three exceptions: a pairing attempt in progress is
  not displaced, the last-playback server wins when neither declares anything, and one pairing
  connection is held beside a playback holder. The player session asks its owner about each
  admissible activation, refuses a rejected one with `concurrent_attempt`, and leaves with
  `another_server`, or `pair/abort concurrent_attempt` while pairing, when displaced.
- **Sendspin discovery over mDNS**: a discovery seam in `src/sendspin` and its backend on a
  computer over mjansson's `mdns`, which joins the `hearth` vcpkg feature. An advertiser
  answers DNS-SD questions for `_sendspin._tcp` or `_sendspin-server._tcp` on every IPv4
  interface with that interface's own address, announces itself twice and says goodbye when it
  stops; a browser queries at a lengthening interval, asks for the SRV, TXT and address records
  a response left out, and reports each service with the `ws://` URL to dial once complete and
  when it goes. The packets are tested without a network, and an advertiser and a browser find
  each other on the loopback interface.
- **`ac3hearth-testsink`**, the first of Hearth's applications (`apps/hearth/testsink`): a
  Sendspin player that listens on its port, advertises `_sendspin._tcp`, keeps its identity,
  pairing PSK and pairing records in a state directory, pairs by its `SP:0` token or a dynamic
  or static code, admits servers as the specification ranks them, and writes each `player@v1`
  PCM stream to a WAV file with a play time logged for every chunk. Several can run side by
  side with distinct names, ports and state. Not packaged; `ac3tests` runs one in process,
  pairs a server with it by its token over a loopback WebSocket, finds the PCM it sent in the
  WAV sample for sample, and reaches it again after a restart under the stored long-term PSK.
- **`player@v1`'s codecs in `src/sendspin`**: encoders and decoders for PCM at 16, 24 and 32
  bits, FLAC over libFLAC and Opus over Opus, both joining the `hearth` vcpkg feature. A FLAC
  stream's `codec_header` is its `fLaC` marker and STREAMINFO block and each unit one frame; an
  Opus unit is one 20 ms packet, and the encoder reports its look-ahead so a server can time
  Opus players with the rest of a group. PCM and FLAC decode to exactly what was encoded at
  every depth. The test sink now offers and decodes all three, and its loopback test finds in
  its WAV exactly what a local decode of the same units gives, for each codec.
- **A Sendspin server host in `src/sendspin`**: `ServerHost` holds every connection to a
  server's clients, listening and advertising `_sendspin-server._tcp`, and browsing for and
  dialling players that advertise `_sendspin._tcp`. It activates each client from its
  `ServerStore`: playback for a paired client or an approved unpaired one, pairing by the
  pairing PSK once the operator has entered a client's token, pairing by a code on request, and
  nothing otherwise. A `Group` plays one programme to several clients on one timeline, each in
  the first of its formats the group can produce, started far enough ahead for the member that
  needs the most lead and paced by what the members' buffers hold. Two test sinks in one group,
  one taking PCM and the other FLAC, each write exactly the programme, and every chunk they log
  puts its first frame at the same local time within 1 ms; a host pairs one sink by its token
  and another by the dynamic code it shows.
- **`_ac3forge_player@v1`'s objects in `src/sendspin`**: the support object in `client/hello`,
  the state object in `client/state`, the object in `stream/start` and the role's commands in
  `server/command` (volume, mute, output delay, settings and identify), each with a writer and a
  reader, as `planning/hearth-sendspin-extension.md` defines them. A settings object with a
  known key out of range is refused whole, and the reader names the revision it refused so a
  sink can report `settings_error` for it; what depends on the sink, such as one trim per output
  inside its range, is checked against the sink's own support object. The objects join
  `fuzz_sendspin_messages`.
- **`_ac3forge_player@v1` in Sendspin's sessions**: a player that lists the role offers its
  support object, reports its state, and hands its listener the role's stream, each burst chunk
  at its time on the player's clock less the role's output delay, and the commands its state
  lists; a chunk of another data type than the stream's is passed on to be counted as invalid.
  The server session activates the role only for a client that offers it, and sends a stream
  only in a data type and sample rate the client listed, a burst only when its Pc and Pd fit its
  payload and the stream, and settings only when the client would read them back whole.
- **E-AC-3 over `_ac3forge_player@v1`, from a group to test sinks**: `ServerHost` activates the
  extension role instead of `player@v1` for a paired client that offers it, and a `Group`
  programme can carry the coded stream beside its PCM. Members playing the role get its IEC 61937
  bursts on the group's timeline, each timed by its first decoded sample, paced as `player@v1`'s
  chunks are and never past a sink's `buffer_capacity`. The test sink offers the role, decodes
  AC-3 and E-AC-3 with any object layer and renders them to a speaker layout (`--layout`, 7.1.4
  by default) in its WAV file. Two test sinks paired to a host play the Dolby Encoding Engine's
  E-AC-3 JOC fixture as one group: each WAV equals a local decode and render sample for sample,
  every burst's play time agrees on both within 1 ms, and a hidden case, `[hearth-soak]`, does the
  same over ten minutes.
- **Sendspin's other six roles in `src/sendspin`**: the objects and binary messages of
  `metadata@v1`, `controller@v1`, `color@v1`, `artwork@v1`, `visualizer@v1` and `source@v1`, and
  the messages that carry them (`server/state`, `client/command`, `client-stream/start` and
  `client-stream/end`), in the specification's form and, for the three state roles, aiosendspin
  9.1.1's, where a cleared field goes out as `null`. Beside them, the arithmetic the roles ask of a
  server: the controller's group volume, which applies a change to every player that supports
  volume and shares what clamping loses among the rest, its group mute, and the colours' 4.5:1
  contrast, reached by moving backgrounds and the colours on them towards black or white. The
  new JSON messages join `fuzz_sendspin_messages`, and the binary ones `fuzz_sendspin_frames`,
  which checks that each writes back to the bytes it was read from.
- **The other roles in Sendspin's sessions**: the server session sends a role's state only while
  the role is active, never a first state scheduled ahead, and a null state for a removed role that
  had one; runs artwork as one transfer at a time, cancelling a transfer and clearing a channel the
  client turns off before a new `stream/start`; keeps visualizer frames to their stream's types and
  rates, in time order and within the client's buffer; passes on only the controller commands its
  last state listed, and seeks within range; and opens a source's input stream only after its own
  start, closing a connection that opens one unasked. It does not activate `source@v1`,
  `artwork@v1` or `visualizer@v1` for an aiosendspin 9.1.1 client. The player session does the
  client's half, closing on an artwork message the role calls malformed.
- **The other roles from a server host's groups**: `ServerHost` activates `controller@v1`,
  `metadata@v1` and `color@v1` for a playing client that lists them, `artwork@v1` and
  `visualizer@v1` only for one that is not aiosendspin 9.1.1, and `source@v1` only for a client
  the operator allows. A `Group` gives its members' roles the programme's metadata, its colours at
  the contrast the role requires, the engine's transport with the group's volume and mute, its
  artwork at each channel's source, format and size, and visualizer frames of the types each member
  asked for. A controller's volume or mute reaches every player in the group over its playback
  role, the engine's commands arrive as host events, and a member that leaves the group has its
  states cleared and its streams ended. The test sink lists the roles when asked (`--roles`) and
  sends controller commands typed on its standard input; in `ac3tests`, two test sinks in one
  group, one paired and one approved unpaired, get the group's metadata, colours, artwork and
  visualizer frames, and a volume and mute set from either reaches both.
- **A scripted aiosendspin 9.1.1 player for A4's exit** (`tools/sendspin`), standing in for
  Sendspin's reference player: `aiosendspin_exit.py` runs `ac3tests`' hidden `[aiosendspin]` case
  against it once each for PCM, FLAC and Opus. The host pairs with the player by its token in
  aiosendspin 9.1.1's dialect and plays it three seconds of two tones; PCM and FLAC arrive sample
  for sample, Opus at 42.5 dB after its 312-frame look-ahead, and every chunk's timestamp is where
  the programme's timeline puts it. The released client refuses to offer Opus, so the player adds
  it to the SDK's decodable codecs for that run (`planning/hearth-sendspin-extension.md`, decision
  5). `hearth-validate` runs the script.
- **Hearth's player against an aiosendspin 9.1.1 server**, found with the scripts above: a player
  reports `available: true` from its activation, as 9.1.1's own
  client does, because the server starts from `available: true` and takes `available: false` for an
  external source, which would move the player out of its group whenever it connected. From such a
  server the player also holds `player@v1` chunks that arrive before its first clock update, within
  its `buffer_capacity`, and drops a chunk whose timestamp is not later than the last one it took:
  The scripted server starts a stream with the activation, holds back what it sends before the
  player's first `client/state`, and then sends it and replays the stream from its start as well
  (`planning/hearth-sendspin-extension.md`, C13 and C14).
- **An aiosendspin 9.1.1 server script** (`tools/sendspin/aiosendspin_server.py`), a rehearsal of
  Music Assistant's Sendspin path: it starts `ac3hearth-testsink`, dials it, pairs by
  the sink's `SP:0` token or by the dynamic code the sink shows, and plays it three seconds of two
  tones in each codec, driving aiosendspin's `SendspinServer` as Music Assistant's provider does.
  The sink's WAV file is the programme sample for sample in PCM and FLAC and within 20 dB in Opus,
  less the chunks the server sends only in its replay and the FLAC block it keeps when a stream
  stops. The sink lists `controller@v1`, `metadata@v1` and `color@v1` as well, and the run shows the
  server's metadata, colours and controller state reaching it, and a volume the sink asks for coming
  back as a player command. `hearth-validate` runs it for both pairing methods.
- **Hearth's third-party notices** (`apps/hearth/notices/`): `NOTICES.txt` for cpp-httplib,
  Mbed TLS, mdns, libFLAC, libogg, Opus and Sendspin's time filter, generated at configure time
  with the versions and licence texts vcpkg installs with each port, ready for Hearth's About page
  and package. The threat model gains Sendspin: what a peer on the network can reach without a
  key, what a key allows, what mDNS exposes, and the two parsers not yet fuzzed.
- **`ac3hearth_engine`, the start of Hearth's player engine** (`apps/hearth/engine`, no Qt): an
  output decision in the shape of Crucible's `output_policy` (a mode, an endpoint and a reason,
  from capability facts that can each be unknown), the play queue, and a transport that answers
  each command with the one action to carry out. A player puts them together with a session per
  item and a PCM sink, one of which drives A2's `PcmOutput`. Each item's AC-3 or E-AC-3 access
  units are decoded and rendered to the output layout 256 frames at a time, including the unit
  the E-AC-3 decoder is still holding for transient pre-noise processing when a stream ends. An
  item at the open output's rate joins it with nothing between the two; a rate change, or gapless
  turned off, reopens the output once the device's own clock says everything submitted has been
  heard, however much silence an underrun put in between. An item that cannot be read is marked
  with the reason and skipped, a device that will not open stops playback without marking the
  item, and a seek made while stopped applies when that item starts. In `ac3tests`, raw E-AC-3,
  E-AC-3 in MP4, AC-3 in Matroska and raw AC-3 play through one output to a fake device: each
  item delivers exactly the frames its access units code, starting where the one before ended,
  and the output is sample for sample what the same queue gives with an output per item.
- **Hearth's player applies an MP4 item's edit list**: the priming and padding it names are
  decoded but not played. Two such items join with nothing from either encoder between them, a
  seek counts from the first sample the item plays, and the queue shows the edited duration. An
  edit list of any other shape plays untrimmed, with a note beside the item. In `ac3tests`, an
  edited item, and a join of two, play sample for sample the matching stretches of an untrimmed
  decode.
- **Hearth's decoder settings** (`apps/hearth/engine/decoder_settings.hpp`): the plan's decoder
  controls, turned into the library configuration.
  - The controls are the operating mode, the custom mode's cut, boost, `compr` and
    normalisation switches, the stereo fold, the Lt/Rt phase shift, LFE mixing, fold levels,
    the dual-mono choice, the programme, the object policy and concealment.
  - The engine applies the dual-mono choice itself: channel 1, channel 2, or one each side.
    A multi-programme E-AC-3 stream plays the programme the setting names, when an item
    starts.
  - A change of settings reaches the playing item at its next unit, through a new decoder
    primed with the unit before. Nothing is lost or repeated, and a unit the old decoder was
    holding back for transient pre-noise processing is released first. Seeks are primed the
    same way, so a seek no longer starts with a block missing its overlap.
  - Two differences from an unbroken decode remain: the settings change itself, and the
    §7.3.4 dither, whose generator a new decoder restarts, some 95 dB down.
- **Hearth's engine thread** (`apps/hearth/engine/engine_thread.hpp`): the player on a thread of
  its own.
  - Commands from any thread are queued and carried out in order between pumps. The engine
    pumps each period while an output is open and sleeps while none is.
  - A snapshot of the queue, the transport, the settings and the history is published after
    every change, with a callback on the engine's thread. The play position is kept apart, and
    follows the device's clock through joins and seeks.
  - Queue edits while playing are the player's own. Removing the playing item moves on to the
    next; a reopen still waiting for the old item to be heard keeps its item through an edit;
    the history's queue indices follow their items.
  - In `ac3tests`, tagged `[concurrency]`, a queue plays to its end while the engine, a fake
    device's clock and the test's own thread all run at once. Commands from five threads all
    take effect, each thread's in its order, and a playing engine that goes away closes its
    output.
- **Hearth's meters, released at play time** (`apps/hearth/engine/play_meters.hpp`): a level
  meter per output slot (peak, hold, RMS and a clip latch) and the programme's loudness
  (momentary, short-term, integrated, loudness range and true peak), measured as the player
  renders each block.
  - Each reading is stamped with the output frame its audio ends on, and handed out only once
    the device's clock, less the output's latency, has reached that frame. The meters move with
    the sound, not ahead of it by what the device holds.
  - Loudness is measured over the slots with a Table E2.5 location; a slot placed only by angle
    has a level meter but no loudness weighting.
  - Each item's integrated loudness, loudness range and true peak are its own. Momentary and
    short-term loudness run on through a gapless join, read from the item before's meter until
    the new item has filled the 3 s window. A seek, a stop or a reopen starts every meter again
    and drops the readings still waiting, since their audio will not be heard.
  - Integrated loudness and loudness range are read once a second. The library works both out
    over the whole programme at each read; at 20 readings a second, that measured some 15% of a
    core three hours into an item.
  - The engine publishes the latest reading beside the play position, and none while nothing
    plays. In `ac3tests`, tagged `[play-meters]`, a reading comes out when the clock reaches it
    and not before, readings come out in order however many wait, each describes its audio's
    level and loudness, and a join, a flush and the once-a-second reads each behave as above.
- **Hearth's media information** (`apps/hearth/engine/media_info.hpp`): what a queue item's
  file says about itself, for the media page and its JSON export.
  - For AC-3 and E-AC-3: the programmes and associated services, the channel map, and the
    whole-stream report `ac3cli probe` makes, authenticity tags included. Also the first
    access unit's bitstream information: service, surround and headphone modes, copyright,
    audio production, time codes, Annex D's alternate syntax and the mixing metadata, with the
    fold levels they give.
  - For AC-4, which Hearth cannot play: the sync frames and the table of contents.
  - The container's facts arrive with the item from its loader. `apps/common`'s container
    input now reports the track, its language, an MP4 track's codec configuration box and
    edit list, and an MPEG-TS stream's programme, PIDs and signalling.
  - `MediaInspector` reads items on a thread of its own, one at a time, and keeps the last
    few descriptions. A newer request replaces one not yet started.
  - The export is `ac3forge.hearth.media/1`. Its `probe` member is the `stream` object of
    `ac3forge.probe/1`, written by the code `ac3cli probe json=1` uses, which moved to
    `apps/common/probe_json.cpp` for the purpose.
  - In `ac3tests`, tagged `[media-info]`: AC-3, E-AC-3 in MP4, Matroska and MPEG-TS, two
    programmes, signed objects and a real AC-4 stream are each described and exported, and
    the document parses. Tagged `[media-inspector]` and `[concurrency]`: a description is
    made on the inspector's thread, served from the cache until a reread is asked for, and a
    request replaced before it started is never read.
- **What the unit being heard says, at play time** (`apps/hearth/engine/unit_reports.hpp`).
  - Each access unit's report comes out when the device's clock passes the unit's first frame,
    as the meters' readings do.
  - A report gives the unit's channels and substreams; its service, dialnorm, `compr` and
    `dynrng` words; AC-3's short blocks; the fold levels in force; any concealment; and its
    object metadata, with every update block's positions.
  - `StreamDecoder` reads the report from what the decoders return, which it used to drop. A
    unit held back for transient pre-noise processing is reported by the call that releases
    it, and the last unit by `finish()`.
  - A unit the item plays nothing of, such as the one a seek decodes only to prime the
    decoder, is not reported. A seek, a stop or a reopen drops the reports still waiting.
  - `Engine::unit_report()` returns the latest report, and nothing while no output is open.
  - In `ac3tests`, four streams are each reported unit by unit: AC-3, E-AC-3 with mixing
    metadata, a stream a unit behind, and an object stream. The player's report changes with
    the item heard at a gapless join.
- **Hearth's diagnostics file** (`apps/hearth/engine/diagnostic_log.hpp` and
  `diagnostics_report.hpp`): the text the Settings page's "Save diagnostics" writes, in the
  pattern of Crucible's.
  - A bounded ring of stamped one-line notes. The engine notes each command as its thread
    carries it out, with anything the transport said about it. The player notes each output it
    opens and closes, with the format, and each item it starts, joins or cannot play. Units that
    will not decode are noted once with the reason, then as a count once the item is done with.
  - The file gives the version, the platform, the output, the playback state and decoder
    settings, the items that cannot be played, the last 50 items played, the settings the
    window passes, and the ring.
  - File paths are left out, as the page says. A note names an item by its place in the queue
    and its title. A loader's error can quote a path, so the item's folders are withheld before
    it is noted: `C:\Music\a.ec3` reads `<withheld>\a.ec3`. The file never reads the engine's
    free-text note or error. It withholds settings under `pairing/` and `queue/`, and scrubs
    the queue's folders and the window's secrets from the finished text.
  - `EngineStatus::output` gives the format the output is open at.
  - In `ac3tests`, tagged `[diagnostics]`: the ring's order and cut; paths withheld in
    Windows, POSIX, UNC and relative forms; the file's sections and limits; and what the
    player and the engine note, in order, for a queue with a missing item, a join, a reopen,
    damaged units and a refused output.
- **Hearth's settings model** (`apps/hearth/engine/settings_model.hpp` and
  `pairing_store.hpp`): what the Settings page's Playback and Network cards hold, the queue
  kept for the next start, and the pairing records.
  - The window keeps them through a `SettingsStore` over QSettings, each as text under a fixed
    key. A value that is missing, or does not read as one of its values, is the default.
  - Playback: gapless, picking up the queue where it was left, and what an item that fails
    does. Network: the name sinks and players show this computer by, cut to a DNS label's 63
    bytes, and whether to look for Sendspin players.
  - The saved queue keeps each item's path and title, the item being heard and how far into
    it, in QSettings' array layout. A damaged one reads as far as it goes.
    `Engine::restore()` brings it back without playing.
  - "An item fails: Stop" stops playback at an item that will not open, rather than passing
    over it. When the item was the next one, the item before it plays to its end first.
    `Transport::item_failed()` makes the choice.
  - The pairing records are a Sendspin `ServerStore`. A record, with the client's name and the
    date, is written as its pairing completes, and one the store would not write is not kept.
    A forgotten record stays forgotten. Keys typed in from a token, and approvals for unpaired
    access, are kept in memory only. A core-only build of `src/sendspin` leaves them out.
  - In `ac3tests`, tagged `[settings-model]` and `[pairing-store]`:
    - defaults, damaged values and names;
    - the saved queue's round trip, and a damaged saved queue;
    - records surviving a restart, a failed write, forgetting, and records that do not read;
    - lookups from other threads while records change.

    The player stops at an item that fails, both when starting and after the item before
    it, and a restored queue starts at its item and position.
- **Hearth's engine bitstreams** (`apps/hearth/engine/bitstream_sink.hpp` and
  `output_selector.hpp`): each item plays the way the output decision says, over IEC 61937 to
  a receiver or decoded here.
  - A bitstreamed item is sent its own access units: AC-3 a frame to a burst, E-AC-3 packed
    six blocks to a burst, across a join when a stream's frames are shorter. The decode still
    runs, for the meters and the unit reports, on the link's clock.
  - Only whole units can be sent, so an edit list's priming or padding inside a unit is heard.
    The decoder settings reach the meters only; the status says so.
  - An item joins the open output only when it would be played the same way. These reopen
    once the output has played out, and say why:
    - a different stream on the link, or a decode after a bitstream;
    - another endpoint;
    - units that cannot make whole bursts with those the last item left.
  - `OutputSelector` reads each endpoint twice, through the platform's probe and the sink's
    own descriptor, and takes a format as carried only when both do.
    - It reads again when told the outputs changed: `Engine::refresh_outputs()`, for
      `RenderDeviceWatch`'s callback, and `Engine::set_output_preferences()` for the Output
      screen.
    - The item playing then moves to the new output from where it was heard, paused if it
      was, once any join before it has been heard (the appliance plan's gaps 3 and 5).
    - The endpoint the player holds is judged by its last free probe and a fresh
      descriptor, since a probe reads a device this player holds as refusing everything.
    - An enumeration that finds nothing keeps the last list.
    - A player with no passthrough output decodes.
  - A programme other than a stream's first is decoded, since a receiver plays only the
    first. The meters stay in step after a unit that does not decode.
  - E-AC-3 on a sink that takes only AC-3 is transcoded (the next entry).
  - In `ac3tests`, tagged `[bitstream]` and `[output-decision]`: bursts checked byte for byte
    against `wrap_frame()` and `Eac3BurstPacker`, joins, reopens, a seek, pause, the meters,
    an edit list, a missing link, an output that changes mid-item, and the engine's commands.
- **Hearth's engine transcodes E-AC-3 to AC-3** (`apps/hearth/engine/ac3_transcoder.hpp`) for
  a receiver that takes AC-3 but not E-AC-3, over the same IEC 61937 link (the appliance
  plan's gap 4).
  - The item is decoded onto 5.1 with neutral settings and encoded as 3/2 with LFE at
    448 kbit/s, as `ac3cli transcode` does. A 7.1 stream is decoded from its independent
    substream, the 5.1 its own encoder made (`StreamDecoder`'s new `Substreams`).
  - Each frame carries the dialnorm and service of the unit that fills most of it, so at a
    join a frame is levelled as the item it mostly holds.
  - Each frame also carries a compr word. It is the most attenuating word sent by the units
    the frame's gain reaches. Where any of those units sent none, a word metered from the
    frame against its own dialnorm (as the encoder meters) also counts, so RF mode stays
    protected.
  - Dual mono heard as its second channel carries that channel's dialnorm and compr word.
  - dynrng is not carried, as on the command line.
  - An encoder's fold levels are fixed, so the link takes the first item's. An item that
    folds at other levels reopens rather than joining.
  - The decode can be cut, so an edit list is honoured to the sample. Items with the same
    fold levels join through one encoder whatever their frame lengths.
  - The encoder's 256-sample delay is part of the link's timeline, so the position, the
    meters and the unit reports run that much behind the decode. What the encoder still
    holds is padded out and sent before the output plays out or reopens.
  - The meters show what is sent, and the decoder settings do not apply; the status says so.
  - The output selector offers the transcode over a passthrough output at 48, 44.1 or 32 kHz.
  - `choose_output()` fixes: a pinned AC-3 bitstream sends AC-3 items untouched without a
    transcode. When the transcode is what is missing, the reason says so rather than
    claiming no output takes AC-3.
  - In `ac3tests`, tagged `[transcode]`:
    - each slot coming back through AC-3 in place, 256 samples late;
    - the metadata in every frame, and the padding;
    - the player's link checked byte for byte against a separate decoder and encoder,
      across a join, an edit list, a seek, a reopen and a 7.1 item;
    - the compr word matching what an encoder given the frame's dialnorm writes;
    - a join that changes dialnorm, and one that changes fold levels;
    - a concealment chosen mid-item reaching what is sent;
    - an output change into a transcode;
    - the engine choosing one.
- **Hearth's player plays the end of the queue as part of the queue.** The last item used to
  be taken as finished once its last unit was decoded, up to a second before it had been
  heard, so a pause or a seek in that time was refused.
  - Now, when what comes next cannot follow gapless, the player waits until the item's tail
    has been heard before asking the transport what is next. That covers the end of the
    queue, and an item needing another output.
  - Until then the item is still playing: a pause holds it, and a seek plays it again from
    the new place. A reopen decided just before a pause waits for the resume.
  - An item added meanwhile joins it where it can, and is heard to its end. Once the device
    has played everything, an added item reopens instead, since it would follow silence.
  - A transcode sends what its encoder holds first.
  - A tail on a link stays there through an output change. A seek back gives the item more
    to play, and then it moves.
  - Under the stop-at-failure policy, an item that will not open is remembered while the tail
    plays, and marked only when playback stops at it. An item put before it meanwhile plays
    first; a stop, or a change to passing over, forgets it.
  - `Transport::would_join()` answers the join question without deciding anything.
  - In `ac3tests`: pause, seek, an added item, and the stop, in the last moment of the queue,
    for a PCM output, a link and a transcode.
- **Hearth's Network page: discovery and pairing** (A6, its first slice). `ac3hearth` browses
  `_sendspin._tcp` and lists every player it finds, live: a Hearth sink's roles, the codecs and
  data types it takes, its output slot count and width; a standard Sendspin player's codecs.
  Selecting an unpaired sink starts a dynamic pairing code attempt at once — the sink shows a
  six-digit code on its own console or page, entered here — and a wrong or expired code says so
  without losing the attempt. `apps/hearth/engine/network_sinks.hpp` wraps `ac3::sendspin::
  ServerHost` and its own `_sendspin._tcp` browse (kept apart from `ServerHost`'s own, so
  `NetworkController`'s "Look again" is a real re-query); `network_view.hpp` turns what it learns
  into the page's rows and labels, tested the way `output_decision.hpp` is. Groups, a sink's own
  settings pages and reported levels are later slices — a sink already in use by another server
  needs `ac3::sendspin` to grow a way to learn that at all, which pairing alone does not give it.
- **Hearth's Network page: making and editing a group** (A6). The list now shows the groups
  alongside the sinks; "+ New group…" makes a real one, backed by `ac3::sendspin::Group`, and its
  own editor adds and removes members, sets a member's volume and mute directly, and sets the
  group's own volume and mute (redistributed across the members that support it, the same
  arithmetic a `controller@v1` client's own command already uses). `Group` gains
  `set_member_volume`/`set_member_muted`, `set_group_volume`/`set_group_muted` and
  `member_player` for this. Not in this slice: actually streaming a programme to a group, which
  needs a network-group output seam in `Player` — the editor says so rather than showing a
  number it cannot yet make true.
- **`ac3hearth` and `ac3hearth-testsink` register their own Windows Firewall exception before
  their first mDNS or Sendspin socket binds**, rather than leaving it to Windows' own "these
  features have been blocked" prompt. `ac3::sendspin::firewall::ensure_inbound_rule()`
  (`src/sendspin/include/ac3/sendspin/firewall.hpp`, Windows only) adds a rule scoped to the
  calling executable, the one port being bound, and the private/domain network profiles — never
  public — the first time it finds none there, elevating once through a UAC prompt if the process
  is not already elevated; every later run finds the rule already in place. A loopback-only bind
  needs none of this and skips it; Linux and macOS do nothing at all.

**Audio outputs**

- **Render device records say which speakers a device has, and at what rates**
  (`ac3::audio::RenderDeviceInfo`): a WAVEFORMATEXTENSIBLE speaker mask and a rate list
  beside the channel count, filled from WASAPI's `dwChannelMask`, ALSA's channel maps,
  PipeWire's `audio.position` and Core Audio's channel labels, with
  `ac3/audio/speakers.hpp` mapping those positions to the renderer's own locations.
  `ac3cli outputs` prints both. Either can be "not reported", which is not the same as
  none.
- **Monitor playback reports its position, and can flush and pause**
  (`ac3::audio::MonitorSink`): frames played from the device's own clock, frames still
  queued here and in the device, and the further latency the platform admits to; a flush
  that drops both buffers and counts from zero again; and a pause that stops the device
  with the stream, the format and the queue intact. Each backend reads the same two
  figures from its own platform — `GetCurrentPadding`, `snd_pcm_delay`,
  `pw_stream_get_time_n`, the Core Audio timestamps, AAudio's presentation position — and
  the arithmetic over them is shared and tested against a fake device's clock. ALSA
  hardware that cannot pause is dropped and prepared again instead, which loses what the
  device held.
  - On PipeWire the frames played are the stream's own, counted as they are handed over,
    rather than the graph's clock, which runs on through a pause.
  - A flush that a device does not reach in time is made when it next runs. It drops only
    what was submitted before the flush.
- **Passthrough reports its position, and can flush and pause**
  (`ac3::audio::PassthroughSink`): the same figures, flush and pause as monitor playback,
  counted in the content's frames. A burst is 1536 of them for AC-3 and for E-AC-3, whose
  link runs four times as fast.
  - A receiver loses its lock while the link is stopped, so the first moments after a
    resume can be silent.
  - On Android, the Shield app's AudioTrack bridge reports the head position and does the
    pause and flush. A bridge without those methods still bitstreams.
  - `ac3tests "[passthrough-live]"` runs all three against a receiver.
- **macOS passthrough fills device buffers shorter than a burst**: the output callback
  wrote only whole bursts into each buffer, so the usual 512-frame buffer went out as
  silence. It now streams the bytes, and writes to the buffer of the stream it opened
  rather than to the device's first. Not yet tried on a Mac.
- **PipeWire reads which codecs a sink takes** (`ac3::audio::read_sink_capabilities`),
  where it used to report no backend. It reads the `iec958.codecs` property the session
  manager sets on a digital node from the sink's ELD. The property names the codecs only,
  so it gives no PCM channel count or rates.
- **A PCM output at the device's own width** (`ac3::audio::PcmOutput`): the stream opens
  at the endpoint's channel count rather than the programme's, and each rendered channel
  is placed at the output a routing patch names (`ac3::render::Routing`), silence in the
  rest. The platform is never asked to widen anything, and Core Audio's requirement that
  the stream be exactly as wide as the device is met by construction. The patch starts
  from the endpoint's speaker mask, which matters because a rendered programme's slots
  are in the coded channel order while a device's outputs are in
  WAVEFORMATEXTENSIBLE's — counting outputs off from zero would put the centre on the
  right speaker.
- **`ac3cli identify`**: walks pink noise across an output's speakers, one rendered
  channel at a time at an AVR test tone's level and band-limited to 30–80 Hz for an LFE
  feed, printing which channel and which output each burst went to. Takes a layout and a
  routing patch, so a room wired differently from the patch can be heard and corrected.
- **A render-device list that keeps itself current**
  (`ac3::audio::RenderDeviceWatch`): endpoint notifications where the platform has them
  (Windows, PipeWire, Core Audio) and a re-probe timer where it does not (ALSA), behind
  one list with a generation to compare. A failed enumeration keeps the last good list,
  so a device held exclusively or a restarting audio service does not empty a picker.

**Containers and encoding**

- **`eac3-encode` authors all eight §E2.3.1.2 programmes, each with its own metadata.**
  `programme2=` (previously the only extra programme the CLI could author) is now
  `programme2=` through `programme8=`, one independent substream per token (I1–I7 beside
  the primary's I0), each with its own `programmeN-layout=`/`-bitrate=` and the full
  `programmeN-<field>=` metadata surface the primary programme's own bare tokens already
  had — `bsmod=`, `dsurmod=`, `dmixmod=`, `pgmscl=`/`extpgmscl=`, the whole `mixdef=`/
  `premixcmp=`/`extmix=`/`speechmix=`/`paninfo=`/`blkmixcfg=` group, `dialnorm=<1..31>|auto`
  (including its own BS.1770 measurement pass) and more. The library side
  (`AccessUnitConfig::additional`, `plan::eac3_programme`) already supported this; the gap
  was CLI surface, now closed. A `programmeN=` past `programme2=` without the ones before it
  is refused rather than silently renumbered, since §E2.3.1.2 assigns substream ids
  sequentially. The five fields meaningful only under 1+1 dual mono and AC-3's own Annex D
  fields are refused on an extra programme rather than accepted and left inert, since an
  extra programme can be neither.
- **AC-4 container carriage** (roadmap IM4): `ac3cli mp4`/`ts` read and write an AC-4
  elementary stream (TS 103 190-2 Annex E's `ac-4` sample entry/`dac4` box, EN 300 468
  Annex D.7's DVB descriptors); `demux` brings either back out byte-identical.
- **`numblkscod=N` (0–3) on the `atmos*` encode commands**, carrying the object layer
  over §E2.3.1.4 short syncframes across 1/2/3-block frames — completing roadmap EQ11.
  Worst-object SNR at every short code matches the six-block control on stationary
  material.
- **OAMD encoding now covers what this project's decoder already reads.** `AtmosEncoder`
  marks an object `b_object_not_active` for a frame where it has no energy in any band —
  the same per-band test the JOC reconstruction matrix already used for silence, now also
  read off as object metadata rather than only affecting the mix. `oba::build_payload_updates()`
  writes more than one §5.5.6/§5.5.7 metadata update inside a single E-AC-3 frame
  (`sample_offset_code`, `num_obj_info_blocks_bits`) instead of only at the frame boundary,
  for a caller that wants sub-frame object motion; `oba::build_payload()` itself is unchanged.
- **`downmix=auto` on `decode` and `monitor`**: A/52 §D3.1.1's automatic choice of
  stereo fold, from the stream's own `dmixmod`. Lt/Rt when it prefers Lt/Rt at an
  acmod Table D2.2 defines the field for (`3/0`, `2/1`, `3/1`, `2/2`, `3/2`); Lo/Ro
  otherwise, including no preference, the reserved code, and every narrower acmod,
  where the table's own note leaves the field's meaning reserved outright. The
  choice is made once, from the programme's first `dmixmod`, and printed.
  `ac3::automatic_stereo_target()` holds the rule for library callers.
- **`probe` reports `dmixmod`**, as a table line and as `metadata.dmixmod` plus a
  per-syncframe `dmixmod` in the `ac3forge.probe/1` JSON document.
- **MP4 edit lists, read and written**: `mp4::demux` and `mp4::Reader` report a track's `elst`
  entries as stored (`ReadTrack::edits`), with the `mvhd` timescale their durations are counted
  in (`ReadTrack::movie_timescale`). `MuxOptions::edit` makes `mp4::mux` write one edit (the
  samples to skip and the samples to play) and sets the movie and track durations to it. An edit
  list or movie header too short to read, or declaring more entries than it holds, is left out,
  and the file still reads. `apps/common`'s container input turns the edit list an audio encoder
  writes into the part of the stream to play. Hearth's player applies it; `ac3cli` and the GUI
  do not yet.

**AC-4 decoding**

- **The first phase of an AC-4 decoder** (`src/ac4dec`, `ac4::Decoder`), written from
  TS 103 190-1 and -2: it reads every syntax element of the presentation substream,
  channel-coded audio substreams (ASF spectral data, stereo processing, companding,
  A-SPX, A-CPL, and `metadata()` with DRC and dialogue enhancement) and EMDF payload
  substreams, and produces no audio yet. The syntax is transcribed a second time in
  Python (`tools/references/ac4_syntax.py`), and the two traces agree element for element
  over the eleven committed DEE streams, ten of them new (SIMPLE, ASPX and A-CPL at 2.0
  and 5.1, DRC curves, immersive stereo at three frame rates), checked in CI, and over
  107 local census streams and the public DASH-IF, CTA WAVE and Chromium channel-based
  streams. The readings taken where the text is ambiguous are in `src/ac4dec/ERRATA.md`.
- **`ac4_substream_info_ajoc()`'s `oamd_common_data()` (§6.2.8.1) is read**, at the one TOC-level
  site that reaches it, instead of refused: bed render info, trim and headphone metadata, and a
  declared-length `add_data` tail a nested element that reads past its own byte budget fails
  against. Transcribed independently in `tools/references/ac4_parse.py` and cross-checked by
  `tools/checks/ac4_syntax_differential.py` over hand-built synthetic streams and a random
  corpus exercising every branch. `oamd_substream()`'s own, separate `oamd_common_data()` embed
  stays out of scope, like every other non-audio substream.
- **A channel-coded substream's HSF extension, `ac4_hsf_ext_substream()` (§4.2.4.3), is read**
  for a 96 kHz or 192 kHz substream whose extension substream resolves to a distinct, readable
  one: the additional scale factor bands, spectral data and noise fill above 24 kHz. Reading it
  needs genuine interleaving between the two substreams' own bits - the owning channel's
  `asf_section_data()` needs a bound (`get_max_sfb_hsf(g)`, §4.3.16.2) that only the extension's
  own header carries, before either can be fully read - resolved regardless of which of the two
  substream indices is numerically lower. A substream reporting `sf_multiplier` whose extension
  cannot be resolved (unlinked, self-referencing, or itself unreadable) is refused, as before, now
  by that reason alone rather than for being 96 or 192 kHz as such. Transcribed independently in
  `tools/references/ac4_syntax.py`; cross-checked by `tools/checks/ac4_syntax_differential.py`
  over 3,800 mutated and synthetic streams, and by two hand-built synthetic frames
  (`tests/ac4dec/test_ac4dec_decoder.cpp`) covering both index orderings. The readings taken for
  Table 39's own `max_sfb` (an active extension needs it to mean `get_max_sfb_hsf(g)`, not
  `get_max_sfb(g)` as written) and for `ac4_hsf_ext_substream()`'s `num_channels`/
  `b_different_framing` are in `src/ac4dec/ERRATA.md`.

**Browser (WASM)**

- The encode demo now covers the whole of roadmap UX6 — wide E-AC-3 layouts
  (7.1/5.1.4/7.1.4), content-measured `dialnorm`, live microphone capture
  (`getUserMedia` → `AudioWorklet` → encoder, with a measure-then-encode pre-roll), and
  a new Atmos object-authoring page (`apps/wasm/atmos/`) that pans real audio objects on
  a room canvas. Playwright-tested end to end, including the microphone path via
  Chromium's fake media device.

**Library, Python and Rust**

- **Python completeness** (roadmap AP6): new `ac3forge.containers` (Matroska/MP4/MPEG-TS
  mux/demux), `ac3forge.meta` (BS.1770 loudness, QC presets/gate) and `ac3forge.signing`
  (EMDF object signing/verification); `Eac3Decoder` is now a context manager. Wheels
  build for manylinux aarch64 and Intel macOS; `stubtest` holds the type stubs to the
  compiled module on every push.
- **The Rust bindings now cover the whole codec surface** (roadmap AP9): the wide-layout
  encoder/decoder, the Atmos/JOC object encoder with OAMD/JOC decode accessors, stream
  framing/scan helpers and the BS.1770 meter, each with real-signal round-trip tests.
  `build-rust` runs on all three desktop OSes; the first Windows build found a real
  portability bug (bindgen types C enums `i32` on MSVC, `u32` elsewhere).
- **Decoding: cut and boost scaled apart, and fold levels a caller can set.**
  - `DecoderConfig::drc_boost_scale` gives a `dynrng` word above unity its own share of
    §7.7.1's partial compression. Unset, boost follows `drc_scale` as before.
  - `OutputConfig::mix_override` replaces the stream's Lo/Ro, Lt/Rt and LFE levels in any
    fold, one field at a time. An LFE level applies only where the stream allows LFE
    mixing.
  - Both default to what every decode did before. In `ac3tests`, cut and boost each move
    only the frames they govern, in both decoders, and an overridden fold is sample for
    sample the fold of a stream that sent those levels, through the coded and the
    rendered-layout forms.
- **The AC-3 decoder folds an Annex D stream with that stream's own `xbsi1` levels**
  (A/52 §D3.1.2, decoding that §D3 makes optional). Lt/Rt (`downmix=ltrt`) now uses
  `ltrtcmixlev`/`ltrtsurmixlev`, and Lo/Ro and mono use `lorocmixlev`/`lorosurmixlev`,
  where all three used to take bsi's `cmixlev`/`surmixlev`, with §7.8.2's −3 dB for
  Lt/Rt. `MixLevels::preferred` carries `xbsi1`'s `dmixmod` from 3/0 up, and a surround
  level Tables D2.4/D2.6 reserve now decodes and reports as −1.5 dB rather than as the
  raw code. Callers folding for themselves use the new
  `ac3::mix_levels(acmod, cmixlev, surmixlev, alternate_bsi)`. `bsid`-8 streams, and
  `bsid`-6 streams without `xbsi1`, fold as before.
- **Speaker management beside the renderer** (`src/forge/include/ac3/render/`, the first
  step of `planning/hearth-reference-player.md`): `Routing` patches each rendered channel
  to one device output or to none, `TrimDelay` applies a per-output trim in dB and delay in
  samples over caller-owned storage, `IdentifyTone` plays pink noise at a stated level on
  one output at a time (30-80 Hz for an LFE feed), and `LayoutRenderer::set_crossover_hz()`
  makes the bass-management corner a setting between 40 and 250 Hz. All header-only and
  allocation-free, so the boards can use them too.

**Verification and CI**

- **Hearth builds and is tested on every build-and-test leg.** `src/sendspin` and
  `apps/hearth` used to be compiled by one Linux job, so the `[sendspin]` and `[hearth]`
  cases ran there and nowhere else, and neither Windows nor macOS had ever compiled
  them in CI. Each leg now configures with vcpkg's `hearth` feature, and `ctest` runs
  those cases with the rest of the suite. A leg with the feature takes a vcpkg cache key
  of its own, since its install set is five ports larger. The first macOS build found
  one error: Sendspin's mDNS discovery passed `poll()` a `size_t` count, which narrows
  to macOS's 32-bit `nfds_t`. It is now cast.
- **A change under `apps/hearth/` now lights the three desktop lanes**, not every lane.
  It was an unmapped path, which the classifier deliberately treats as "build
  everything"; it is one desktop program built on Windows, Linux and macOS, like
  `apps/cli/` and `apps/crucible/` beside it.
- **Heap churn is now gated before a merge, not only after one** (`Memory gate` in
  `ci.yml`): `ac3membench` used to run only on `push` to `main`, so a regression (E-AC-3
  encode churn 67→199 allocs/frame at PR #352) was found blocking nothing. The new job
  builds and compares `ac3membench` at the PR's head and merge base; the hard tier
  (churn at least doubled) fails the gate, with `memory-regression-approved` as the
  override. The `steady_live_growth` leak check now applies its absolute thresholds to
  what the branch changed rather than the head alone.
- **CI now asserts that Linux and macOS packages carry the `ac3cli` man page and shell
  completions** (`check_cli_docs_package.py`), so the packaging bug fixed below cannot
  come back unseen — nothing had checked these five files before, and the only test that
  did (Homebrew's) passed for an unrelated reason.
- **Fuzz harnesses for the two parsers of third-party files that had none**:
  `fuzz_iab_parse` (IAB/MXF) and `fuzz_ac4_parse` (AC-4 scan/parse). Their first runs
  fuzzed the parsers uninstrumented; see Fixed for what the instrumented runs found.
- **The cross-platform bitstream-hash gate now pins `aarch64-neon`**, from real arm64
  CI: byte-identical to `x86_64-sse2`, proving the encoder is bit-exact across
  architectures and that the ~6.02 dB gold-reference gap is entirely decode-side.
- **Roadmap VX11 resolved: the ~6.02 dB cross-platform split is a last-bit arithmetic
  difference, not a systematic codec error.** It splits strictly by architecture, not OS
  or compiler, and steps rather than grades — every (check, channel) pair sits at
  0.00–0.11 dB or 5.85–6.05 dB, nothing between. Per-channel floor headroom drops from
  6.02 dB to 1.0 dB, so the gates now catch a 1 dB regression where they previously
  needed 6.
- The Python oracles under `tools/` now have unit tests of their own
  (`test_compare_wav.py`), pinning the single-floor blind spot fixed above.
- **Hearth's speaker layout can be changed live** (`Player::set_layout()`,
  `Engine::set_layout()`): reconfiguring `OutputLayout` while an item is playing closes and
  reopens the output at the new width, resuming the same item from where it had got to,
  rather than needing the whole engine torn down - the same close/reopen/seek-back shape an
  output-endpoint change already used. `ac3::render::OutputLayout` gained `with_small()` and
  `with_realization()`, structural mutators for a settings page that has a slot index or a
  Heights choice rather than text to re-parse. The Speakers page's layout picker, "As text"
  field, Heights control and per-speaker Size toggle are wired to it.
- **The Speakers page's setup survives a restart.** Trim, delay, crossover, routing and the
  layout itself are now kept through `SettingsStore` and reapplied at the next start, the same
  way playback settings and the resumed queue already are - previously every restart reset the
  whole page to stereo defaults. One setup today, not one per output device, despite the page's
  own "a setup for each output" wording.

### Changed

**Minimum-footprint / ESP32 decode and encode profile**

- **A Hearth sink's page is redesigned in the Hearth desktop app's look** (`esp-idf/ac3forge/ui/`,
  `planning/esp32-device-ui.md`): its palette in light and dark, numbered sections, Sendspin
  first on a board that has it, a source, decode and output path for Now, level meters, and the
  settings grouped as Speakers and Network. The slot width and the named output layouts are
  segmented controls sent as they are chosen; a refused choice goes back to what the board has.
  Outcomes show in a toast, and Forget every server asks in a dialog rather than `confirm()`,
  which stopped the page's polling while open. The page offers only settings the board reports:
  the ESP32-C6, the P4's wide sink and the capture and null sinks no longer show a wiring
  checkbox that could only be refused, and there `GET /wiring` answers `404`. The page and its
  script are 42,846 bytes against a re-derived budget of 45,056.
- **A Hearth sink's built-in WiFi network is empty by default, not `my-network`.** With
  the placeholder set, a freshly flashed board spent its `CONFIG_AC3FORGE_EXAMPLE_WIFI_RETRIES`
  attempts and up to 30 s failing to join it before Improv started listening. Empty means
  nothing stored or built in, so `network_up()` returns at once and Improv listens from
  the first second. A fleet meant to join one network from the image still sets the
  option; a board meant for Improv or `PUT /network` now needs nothing set.
- **The ESP-IDF streaming-player example is now `hearth_sink`.** It becomes Hearth's
  ESP32 sink (`planning/hearth-reference-player.md`), so it takes the name before the
  work starts: `esp-idf/ac3forge/examples/hearth_sink/`, the CMake project
  `ac3forge_hearth_sink`, and the *ac3forge hearth sink* menu in `idf.py menuconfig`.
  Its `CONFIG_AC3FORGE_EXAMPLE_*` options, sinks, sources, web page and stream set are
  unchanged, and the image it builds behaves as it did. Anyone pointing a script at the
  old path or flashing `ac3forge_stream_player.bin` needs the new name; the GUI's own
  stream player is a different thing and keeps its.
- **The fixed-point tier decodes 5.1 in real time on the ESP32-C6 with WiFi up**, with the
  same PCM bit for bit: the fixed-tier hashes do not move. The IMDCT pair's products drop a
  saturation they cannot reach, the overlap-add runs on 32 bits and builds its output floats
  from the integers' bits, `Fixed32`'s product tests its saturation once, and its shifts,
  small ratios and square root avoid 64-bit library calls on a 32-bit core, as do the AHT and
  spectral extension products. On the board at 160 MHz with no network, AC-3 5.1 went from
  34.7 ms a frame to 20.5 and E-AC-3 5.1 from 38.7 to 23.9; with WiFi and a 1,536 kbit/s
  stream arriving, from 44.3 to 26.2 and from 46.4 to 30.6. See
  `docs/platforms/bare-metal/esp32-c6.md`.
- **E-AC-3 decodes in real time on the ESP32-S3** (roadmap PF7), the result of five
  successive profiling passes. The double-arithmetic bottleneck between the bitstream
  and the float32 coefficient store (mantissa dequantisation, dither, coordinates,
  decoupling, spectral extension, AHT, JOC mixing — each a call into the ROM's software
  float) now runs in `decode_scalar_t`; enhanced coupling gained the same `float`
  overloads in a second pass. Two new switches, `AC3FORGE_STAGE_TIMERS` and
  `AC3FORGE_MINIMAL_HOT_O2` (five hot files at `-O2` under the `-Os` profile) back the
  work. Further passes replaced `BitReader`'s per-bit loop with a 64-bit cache, reused a
  block's allocation when its exponents/parameters repeat, moved the PCM handoff from
  `std::copy` to `memcpy` (the ROM's `memmove` cost ~12 cycles/byte), and folded the
  output stage a block at a time instead of per-sample. On an ESP32-S3-DevKitC-1-N16R8
  at 240 MHz: a 5.1 frame went from 78.8 ms to 6.2, an Atmos objects frame from 82.7 to
  21.2, enhanced coupling from 217 to 19.8, and a 7.1.4-to-stereo fold from 4.1 ms to
  1.1 — every level unchanged to the digit throughout. [The ESP32-S3
  page](docs/platforms/bare-metal/esp32-s3.md#timing) has the full stage tables and a capability table
  of what fits the part.
- **The E-AC-3 decoder no longer copies what its per-block coefficient store already
  holds.** An AHT stream sends all six blocks' mantissas in block 0, and the decoder held
  them in a buffer per stream until each block copied its own out: 6,144 bytes a stream in
  the float build, 36,864 for a 7.1.4 stream's six streams and 43,008 with the coupling
  channel. Block 0 now decodes them straight into the store, which keeps every stream of
  every block. Enhanced coupling's reconstruction likewise reads its neighbouring blocks'
  coupling channel there instead of from a 6,144-byte copy, and an access unit's substreams
  are gathered in an array the decoder keeps from unit to unit instead of one allocated
  for every unit (2,508 bytes for three substreams on the ESP32-S3). The PCM is unchanged
  bit for bit in the `double`, `float` and `fixed` tiers. Under QEMU, in the ESP32-S3's
  7.1.4 network shape without PSRAM, `714-aht.ec3` and `714-all.ec3` no longer abort for
  want of internal RAM: over four runs each, their least free internal heap during a play
  was 31,224 to 32,040 and 30,072 to 32,196 bytes, where the 7.1.4 streams CI already plays
  reach 30,252 to 35,040. `714-ecpl.ec3` now plays too, but with as little as 2,236 bytes
  to spare, so it stays a PSRAM-only stream.
- **The encoders now run their analysis front end and coefficient store in
  `encode_scalar_t`** (roadmap PF7, a second scalar axis beside the decoder's):
  transient detection, the block gather, the forward transform, and — in a second pass
  the same day — the coupling/spectral-extension/enhanced-coupling analyses, dither and
  delta-segment decisions, and the fixed-point conversion. Only the AHT and the masking
  model's internals stay `double`. On the ESP32-S3: AC-3 2/0 encode went from 75.0 ms to
  12.1 (real time), E-AC-3 5.1 from 348.9 to 81.2. Every `<double>` instantiation is the
  function the ordinary build already called, so the golden bitstream hashes hold; CI's
  `linux-gcc` leg builds the float encoder alongside the float decoder and gates it to
  within 0.5 dB of the double encoder.
- **The rate-control search and exponent-run planner cost less** — mostly exactly (the
  same candidates, the same answer, pinned by the fixture and golden hashes): both Annex
  E frame forms scored in one pass, the masking curve computed once per search rather
  than once per probe, repeated stream runs counted once. One change isn't exact — the
  delta race's two searches now warm-start from their own previous answer rather than
  each other's — because the frame's mantissa cost isn't monotone in the offset; the
  E-AC-3 golden hashes and profile fixtures are re-pinned to the new, still passing,
  answer. On the ESP32-S3: E-AC-3 5.1 encode dropped a further 81.2 to 55.5 ms.
- **The decoder's block form carries the objects**
  (`PcmBlock::objects`/`object_indices`/`object_metadata`, views onto the unit's own
  reconstruction) and **objects are placed on loudspeakers on the minimum-footprint
  targets** (`spatial.cpp` joins the decoder profile): a height-object stream pans onto
  7.1.4 at 25.1 ms/frame on the ESP32-S3 (0.78x), with `pan_ring`/`pan_direction` no
  longer allocating.
- **A minimum-footprint build resolves the SIMD arch seam** instead of naming `generic/`
  literally, fixing a minimum-footprint decoder built for aarch64 that had been missing
  NEON.

**Library internals**

- **`ac3/decoder/decoder.hpp` no longer includes `ac3/core/eac3_tools.hpp`.** The
  include was left over from a struct that moved out with the AP3 pimpl sweep; source-
  breaking only for a consumer relying on the transitive include (none in-repo). ABI
  unchanged.
- **Two coding-tool headers moved from `ac3/encoder/` into `ac3/core/`**
  (`coupling.hpp`, `eac3_tools.hpp`), where the code they hold — used by both decoders
  on every frame — already lived. Source-breaking, deliberately landing before the v1.0
  API freeze with no compatibility shim; ABI unchanged.
- **The ESP32 player's output layout and renderer moved into the library as
  `ac3::render`** (`esp-idf/ac3forge/include/ac3forge/{layout,render}.hpp` to
  `src/forge/include/ac3/render/`), with the player's fold-and-objects policy as
  `ac3::render::serve()`, so the desktop player and its test sink render with the boards'
  code. The arithmetic is unchanged: the QEMU render shape's twelve slot levels are the
  same as main's. Source-breaking for the component's `ac3forge::OutputLayout` and
  `ac3forge::LayoutRenderer`, which were never published to the component registry.

**SonarCloud and code quality**

- Four places now use the idiom SonarCloud's first scan asked for (`cpp:S6427`,
  `cpp:S1048`), because it's better code and not only a quieter report.
- The nightly SonarCloud scan now builds the examples, so the ~15 translation units
  under `examples/` are analysed instead of silently skipped by the coverage-only preset
  that had excluded them.

**Crucible desktop application**

- **The Desktop Atmos Demo is now AC3Forge Crucible** (roadmap UX12): a desktop
  application rather than a Windows-only demo, with the same idea.
  `ac3desk`/`ac3windemo` are `ac3crucible`/`ac3crucible-run`; settings migrate on first
  launch; everything the app asks of the OS goes through four platform seams under
  `apps/crucible/engine/` with no `#ifdef`s, tested against fakes on every platform.
- **Crucible runs on Linux, on PipeWire** — verified on a Raspberry Pi 4B against an
  Atmos receiver over HDMI on 2026-09-05: an application tapped through PipeWire,
  encoded live as E-AC-3 with a signed JOC object layer, read on the receiver's front
  panel as "Atmos/DD+". Applications are tapped through PipeWire's per-stream target,
  the silent device is a `support.null-audio-sink` node the app creates and removes
  itself (no driver needed), and the front window is read from X11
  (`AC3FORGE_CRUCIBLE_X11`) or reported off under Wayland. A build against ALSA is
  refused at configure time. Both the `.tar.gz` and the `.deb` ship as release assets,
  for x86_64 and aarch64.
- **The PipeWire backend's passthrough now offers AC-3/E-AC-3 only when the sink's
  `iec958.codecs` (its EDID) lists them**, never on the strength of a successful connect
  — which PipeWire grants a headphone jack as readily as a receiver, and which rejected
  the very receiver this was written for on the Pi.
- **`process_loopback` is now reported unavailable on macOS**, where the version gate
  used to claim it was available from 14.2. The Core Audio process tap never returned
  from `AudioDeviceCreateIOProcID` on the CI leg's first real run and froze the whole
  process; the tap stays in the tree behind `AC3FORGE_MACOS_PROCESS_TAP` for anyone with
  hardware to settle it on.
- **The Windows null-sink driver is now an ACX driver on KMDF**, derived from
  Microsoft's AudioCodec sample, in place of the PortCls/WaveRT miniport — about 1,900
  lines in place of 9,700, with Driver Verifier's DDI compliance now part of its
  verification. Nothing the demo or scripts see changes.

**CI and static analysis**

- **Code analysis now runs nightly against `main` instead of on every PR/push/merge-
  queue entry**: CodeQL, MSVC Code Analysis and clang-tidy (moved to its own workflow)
  each open or refresh a `nightly-analysis` issue on a finding. `CI Status` no longer
  waits on them. Measured motivation: the three engines held about 55 self-hosted
  runner-minutes per CI event, paid three times per merge on a fleet shared with another
  repository. A fourth engine, SonarCloud, joins them (maintainability, duplication,
  new-code coverage), pinned to GitHub-hosted since the CFamily analyser doesn't fit the
  shared fleet.
- The ABI gate no longer runs on merge-queue entries — the PR run already produced the
  comparison it exists for, and while `ABI_ENFORCE` is off nobody reads the release-
  relative second view before the merge lands.

**Quality gates**

- **The gold-reference quality gate now has one SNR floor per channel, not one per
  fixture.** A/52 leaves the values a decoder substitutes for zero-bit bins unspecified,
  so two spec-correct decoders legitimately differ there — a 5.1 fixture's surrounds
  sitting 35 dB below its fronts meant a single floor had to clear the surrounds, gating
  the centre channel at 22 dB while it measured 58.1. Each channel now carries its own
  floor, `floor(min_observed - 1.0)` derived across every CI leg and commit
  (`derive_channel_floors.py`), gaining 19–71 dB of real gate on front channels and LFE.
  The trend check follows the same rule, comparing each channel against its own trailing
  average. See [Validation](docs/verification.md).

**Documentation**

- The Crucible guide gained its two missing pages ([The room](docs/crucible/room.md),
  [Settings](docs/crucible/settings.md)).
- The library's docs page now presents it as a member in its own right, matching Forge,
  Crucible, and Hearth.
- The published-asset table now matches the pipeline: a Windows arm64 row, Crucible
  rows, and four stale claims corrected.
- The CLI reference lists all forty-two commands, including the previously-undocumented
  `spatial`.

**Release engineering**

- `ac3::version_details()` (and `ac3cli --version`) now puts commits-past-tag in the
  headline as semver build metadata (`0.10.0-beta.1+100`), so it no longer reads as a
  tagged release when it isn't.

### Fixed

**Containers**

- **The `dec3`/`EC3SpecificBox` `asvc` bit misclassified karaoke as an associated service.**
  `ac3::io::build_codec_config_box` used a plain `bsmod >= 2` test, which reads bsmod 7 (karaoke
  at an acmod other than 1/0 — a *main* service per A/52 Table 5.7) the same as bsmod 7's other
  meaning, voice-over. The MPEG-TS descriptor writer already got this split right; the `dec3`
  writer now shares its rule, `ac3::meta::is_associated_service`.

**Command line and GUI**

- **`ac3cli monitor` refused a §E2.3.1.2 legacy-core stream and dropped every stream's last
  unit.** It picked its decode path from the first frame's bsid alone, so a stream whose 5.1
  bed is a plain AC-3 syncframe with Annex E dependents extending it went to `FrameDecoder`,
  which refuses the first dependent it reaches - the same test `decode` already makes now
  reads `has_eac3_extension_substreams` too. Separately, the E-AC-3 loop never drained
  `Eac3Decoder::flush()`, so the final access unit of any stream whose last frames used §3.7's
  transient pre-noise tool never played - held back by the decoder and simply left there when
  the loop ended. `spatial` had the same missing flush. Both commands now play that unit,
  through a new `ac3::apps::held_back_unit` shared with future callers, laid out the same way
  as every other unit.
- **`ac3cli transcode`, `metadata`, `cut` and `cat` read a §E2.3.1.2 legacy-core stream as
  plain AC-3, missing the Annex E dependent's channels.** `decode_and_render`'s decoder
  choice and `codec_label`'s status-line label both tested `scan.kind == kEac3` alone, so a
  stream whose 5.1 bed is a plain AC-3 syncframe with an Annex E dependent extending it fell
  to `FrameDecoder` instead of `Eac3Decoder` - the same two-way test the `monitor` fix above
  closed, in the one place it remained. Both now recognise the third `StreamKind`
  (`kAc3CoreEac3Extension`) as E-AC-3-shaped, matching `decode`'s own dispatch.
- **`ac3cli spatial` refused a §E2.3.1.2 legacy-core stream outright.** It refused any
  stream whose first frame was AC-3 (`bsid <= 8`) before ever checking for an Annex E
  extension substream behind it - but a legacy-core delivery's object layer lives in
  exactly such a dependent, since a plain AC-3 core has nowhere to put an EMDF container.
  `spatial` now shares `run_monitor`'s own `ac3::apps::reads_as_access_units` test, so a
  legacy-core stream that does carry an object layer decodes and plays instead of being
  turned away.
- **`ac3cli decode` and `transcode` could misplace a stream's held-back last unit.** Both
  already drained `flush()`, but placed each flushed substream's channels by appending it
  straight into the WAV sink or the transcode sample queue - once per substream per Table
  E2.5 location, rather than assembling the whole unit first. A last unit that released a
  bed together with the dependent that had been holding it back could then land both
  substreams' channels in the same location, growing some channels past others instead of
  merely leaving stale audio behind. Both now build the held-back unit once through the same
  `ac3::apps::held_back_unit` `monitor`/`spatial` use above, and append it exactly once per
  slot, like every other unit.
- **The GUI offered E-AC-3 bitrates a source's sample rate couldn't frame.**
  `bitrates()` branched on codec but not on the loaded source's rate, so a 16 kHz file
  offered rungs no `frmsiz` could carry; encoding was refused only at the encode button.
  The list is now filtered per-rate by the same rule `plan::validate()` already applies,
  and a lower-rate source clamps an out-of-range selection down.
- **`ac3cli probe` swapped bsmod 7's two service names.** Table 5.7 makes acmod 1/0's
  bsmod 7 "voice over" and every wider acmod's "karaoke"; the table form and the JSON
  document's `bsmod_label` had the pair backwards. `ac3::meta::describe()`, used by
  `mpegts` and the library's own reporting, already had it the right way round.
- **`ac3cli spatial` and `qc objects=` played and measured a decoded Atmos programme's
  dynamic objects against an LFE that arrived 576 samples too early, and `decode ...
  adm_out=` exported the same mismatch into its ADM master.** A JOC-reconstructed object
  lags the bed it was pulled from by `oba::joc::reconstruction_delay(domain)` samples —
  576 under the QMF domain every decoder defaults to (`docs/library/decoding.md`, "Atmos
  objects lag the bed") — but all three sites combined a decoded unit's bed LFE with its
  already-lagged object audio unmodified, in the same update or the same exported track.
  The LFE is now held back to match: a small FIFO delay line ahead of the Windows Spatial
  Sound sink and the loudness meter, and a whole-channel shift on the batch-written ADM
  master, the last pinned by a regression test measuring the exported master's two
  channels before and after.
- **`ac3cli probe json=1` wrote invalid JSON for an AC-4 stream of bitstream version 0 or
  1.** Each `presentations_v0[].substreams[]` entry held an unnamed object beside its
  `role`. The substream's members now sit beside `role` in the entry. No stream on hand has
  such a table of contents, so no output seen so far changes.
- **`ac3cli play`, `monitor`, `identify` and `live`'s output legs spun for ever once an
  output device went away.** None of them looked at `running()`, so a lost render endpoint
  left `submit()` refusing and a drain loop waiting on counts that had stopped moving -
  the same hang the queue-full case already had before the sinks themselves learned to stop
  (see "An output device that went away left the sink saying it was still playing" above).
  Every submit and drain loop now ends as soon as the sink reports itself not running, and
  says which endpoint went and how (unplugged, switched off, disabled, or taken by the
  system), through a shared `ac3::apps::submit_while_running`/`wait_while_running`
  (`apps/common/sink_wait.hpp`). `play`, `monitor` and `identify` exit `5`; `live`'s
  monitor and passthrough legs are dropped and the take carries on, ending the session as a
  failure only because it did not do everything asked. `tools/checks/passthrough_probe.cpp`
  gets the same fix, exiting `5` rather than looping past a pulled cable. `ac3cli spatial`
  is unchanged - `SpatialObjectSink` was not touched by #775 and needs its own fix.

- **A twelve-channel play aborted on the ESP32-S3 for want of internal RAM.** The E-AC-3
  decoder held all 32 substream-identity slots (`strmtyp * 8 + substreamid`) by value, so
  every byte added to `DecodedSubstream` cost 32 bytes of heap in every decoder whatever
  the stream — and a stream has one to three identities. The three downmix-level fields
  added for a legacy-core fold grew that struct by 284 bytes and so the array by 9.1 KB,
  which was most of what the widest shape had left: a 7.1.4 play of a three-substream
  stream had been running on about 10 KB of free internal RAM, and an Ethernet buffer
  arriving at the wrong moment took the rest. The slots are now allocated per engaged
  identity, as the overlap-add and JOC states beside them already were, and an engaged
  slot is written through for the rest of the stream, so a steady-state decode still
  allocates nothing. Measured under QEMU on the 7.1.4 stream set: least free internal RAM
  during a play 9,540 → 35,332 bytes, each of the twelve slot levels unchanged to the
  digit, decode time no higher. The ESP32 QEMU legs now also fail on a failed allocation
  anywhere in the console — one can be survived, so a run could print hundreds and still
  report `result=pass` — and the stream set's free heap is held to a floor.
- **The ESP32 player kept block storage for sixteen output slots whatever the layout.**
  `ac3forge::Player` held 256 samples for each of sixteen slots inside its own
  allocation, 16 KB, so a 7.1.4 play carried 4 KB it never read and a stereo play 14 KB,
  in internal RAM on a part without PSRAM, where the twelve-channel shape runs short
  first. The storage is now sized from the play's layout at `start()`, placed in PSRAM
  when the part has it, and released at `stop()` with the ring and the hold. On the
  7.1.4 stream set under QEMU, the least free internal RAM during a play is 41,456 bytes
  against 36,904 on main's last CI run, and every stream's slot levels are still the
  host's.
- **The ESP32 streaming example's `tdm` sink claimed sixteen 32-bit slots on one data
  line; an ESP32-S3 carries four.** An S3 TDM frame holds at most 128 bits (ESP-IDF v6.1
  enforces it); the sink had never run on hardware before a 2026-09-11 board run found
  it. It now refuses an over-budget frame and says why; the docs say what the part
  actually carries.
- **A panic or reset after `result=pass` passed every ESP32 CI leg under QEMU.** The
  probe runners and the streaming player's steps ended QEMU on a timeout and looked for
  panic output only when the pass line was missing — but the application prints that
  line before it finishes, and a panic resets the part into a second run that prints it
  again. One HTTP-step player freed its ring buffer twice after its verdict and reached
  the step only as a stale `/status`. Every leg now also fails on panic output or a
  second boot banner anywhere after the first.
- **`PUT /layout` overflowed the ESP32 control surface's stack.** `esp_http_server`'s
  handler task has 4,096 bytes by default; parsing the layout there peaked at 4,596
  under QEMU, past the canary. `Control::start` now takes the stack size (6,144 bytes by
  default).
- The minimum-footprint decode profile's ESP32-S3 build left only 8,096 bytes of main-task
  stack free at high-water, 96 bytes under the CI runner's 8,192 floor — `DecodedSubstream`
  and `DecodedAccessUnit` grew by `bsid`/`cmixlev`/`surmixlev`/`alternate_bsi` (see below).
  `CONFIG_ESP_MAIN_TASK_STACK_SIZE` moves from 32,768 to 40,960.
- **An E-AC-3 decode kept two or three copies of its result on the stack.** `Eac3Decoder`
  returned each substream and access unit through a `std::optional` temporary, and
  `decode_substream` held a concealed substream beside the decoded one, so a field added to
  `DecodedSubstream` or `DecodedAccessUnit` cost the decode path several times its size: the
  four fields above cost the ESP32-S3 streaming player's decode task about 1.9 KB. The results
  are now built in place, in the caller's storage. The three stack frames live while a
  substream decodes shrink from 12,352 to 9,040 bytes on the ESP32-S3. Under QEMU, every
  E-AC-3 stream of the 7.1.4 stream set leaves the decode task at least 3,312 bytes more of
  its 24,576: 9,344 at the least, against 6,032 before, and 10,368 for `714-walk`, which
  left about 9,000 before those four fields. The footprint probe's decode leaves 19,344 bytes
  of its 40,960-byte main-task stack, against 16,064.
- **The ESP-IDF component decoded in `float` on parts with no FPU.** Its manifest says the
  decode arithmetic follows the part, but only the probe projects chose `fixed`:
  `src/forge/minimal.cmake` builds `float` when `AC3FORGE_DECODE_SCALAR` is unset, so any
  other project for an ESP32-C3 decoded in software floating point, which on an ESP32-C6
  board is up to 3.1 times slower than the fixed-point tier. The component now sets the
  option from ESP-IDF's `SOC_CPU_HAS_FPU` capability when the project has not: `fixed`
  without an FPU, `float` with one. A value set above `project()` or passed with `-D` stays.
- **A Hearth sink restarted when Improv gave it a network after a failed join.**
  `hearth_sink`'s `network_up()` ran the whole network setup on every call that had not
  yet joined, and ESP-IDF refuses a second default event loop. The call Improv makes
  after storing new credentials therefore aborted whenever an earlier join had failed:
  a mistyped passphrase followed by the right one, or a board whose stored or built-in
  network could not be joined at boot. The build's placeholder network, `my-network`,
  puts every freshly flashed board in the second case. The board came back on the new
  network, but the Improv client saw the port vanish instead of an answer.
  - The setup now runs once. Each later attempt stops the station, waits until the
    stop is reported, and starts it on the new network with a fresh retry count.
  - A board that joins after boot now starts mDNS and the Sendspin player without a
    restart. Before, only boot started them. The same applies when the boot play's
    source is what brings the network up.
  - An attempt no longer waits forever. A network that associates but gives no
    address is left after 30 s; once stored, it used to hang the board at boot before
    Improv started. A connect the driver refuses now fails the attempt.
  - A build with no network stored and none built in used to restart in a loop: its
    control surface opened a socket before lwIP was initialised. lwIP now comes up
    whether or not there is a network to join.
  - The QEMU Ethernet network set itself up again on a second call too, and is now
    set up once as well.
- **Over an ESP32-S3's USB console, a Hearth sink's Improv answers waited for the next
  line it printed.** ESP-IDF's driverless USB-Serial-JTAG console sends its buffer to
  the host only at a newline, and an Improv packet has none. On an idle board, or after
  `cannot_connect`, nothing followed, and the client never got its answer. Each packet
  is now synced to the host as it is written; on a board, a `current_state` request is
  answered in 0.5 s, where before its answer arrived 10 s later with the next request's
  output.
- **A Hearth sink stayed off its network once its access point restarted.** The WiFi
  station retried a disconnect `CONFIG_AC3FORGE_EXAMPLE_WIFI_RETRIES` times in quick
  succession and then gave up for good, so an access point away for the 30 s to two
  minutes a restart takes left the board off the network until someone power-cycled it.
  A power cut was worse: the board booted long before the access point, spent its
  retries in 15 s and never tried again. Meanwhile `network_ready()` never turned
  false, so mDNS and the Sendspin player believed the board was online, and an Improv
  client asking such a board was told *provisioned*, with the address of a page that no
  longer answered.
  - The station now keeps trying the network it has: the quick retries first, then
    after 1, 2, 4 and 8 s, then every 15 s, until it joins or is given another network.
    A network lost after joining is retried the same way. On two ESP32-S3 boards, one
    running a SoftAP as the access point, a board idle when the access point went was
    back 1.2 s after it returned, a board whose access point vanished without a word
    for 125 s was back 14.7 s after, and a board that booted while the access point was
    off joined 12.5 s after it came back and then advertised itself and started its
    Sendspin player, with no restart.
  - `network_ready()` now means the board holds an address now, not that it once did.
    A disconnect or a lost address clears it and rejoining sets it again, so mDNS, the
    Improv reply's URL and `app_main`'s watch for a network all follow the truth.
  - Improv's current state is *ready* whenever the board is not on a network, where a
    board with a network stored used to answer *provisioning*. The client
    improv-wifi.com uses offers its Wi-Fi form for *ready* and a spinner with no way out
    for *provisioning*, so a board whose network had gone could not be given another
    one from the page it tells people to use.
  - An Improv `wifi_settings` sent to a board that is off its network now joins the new
    network at once, dropping the one being retried. A board that is on a network keeps
    it and joins the new one at its next boot, as `PUT /network` does, and says so on
    the console.
  - A network that gives no address within 30 s no longer has its station stopped: the
    board stays associated, and an address that arrives later still joins it.
  - A Sendspin stream that is playing when the network goes now ends where it stopped,
    freeing the player's memory, and the board rejoins from there: on a board with
    about a kilobyte of internal heap free while streaming, the rejoin came 8.7 s after
    the access point returned, and the next play was clean.
- **A Hearth sink's first play right after a Wi-Fi reconnect could start with a few chunks
  late and an underrun or two, converging again over about a second.** Learning bursts run
  one after another until the clock filter's own error estimate reads as converged, which
  says only how well a run of replies agrees with itself, not with the truth - and a run
  taken in the turbulent seconds right after a reconnect, where reassociation, mDNS's
  re-announce and an ARP round can all delay a reply the same way, could agree with itself
  as well as an accurate run and read as converged on an offset that was still several
  milliseconds off. `ac3::sendspin::ClockSync` now takes convergence in two steps: once a
  run reads as converged, one more burst, a learning interval later and so genuinely apart
  in time, must measure within a millisecond of that run's own last reading before the
  clock is reported converged and a stream is let start. A confirming burst that disagrees
  is not trusted; the run starts over.
- **A Hearth sink refused a network whose name is 13 characters, and answered with a
  broken one about a 10-character board name.** Improv's packets share the console with
  the lines the board prints, and ESP-IDF's default line endings rewrite bytes inside
  them: a CR from a client arrives as LF, and a CR goes out before every LF. Either one
  lands in a packet - a length byte, a string, a checksum - and the packet then fails
  its checksum at the other end. A `wifi_settings` whose SSID is 13 bytes long, so that
  the length byte in front of it is a CR, was answered `invalid_packet`: a board could
  not be told about a network named, for instance, `MyHomeNetwork`. In the other
  direction, with a 10-character name stored, the `device_info` and `device_name`
  answers carrying it reached the client broken. The example's console now converts
  nothing in either direction. A command typed on it still ends at either CR or LF, and
  each line the application prints now ends in LF alone, which `idf.py monitor` and the
  checks under `tools/checks` read as they did; a terminal that needs the CR has a
  setting for it. The ROM's lines, and anything logged from an interrupt, still end
  CR LF: they are written by `esp_rom_printf`, which this setting never reached.
- **A Hearth sink's page could reach its Sendspin player before the player had started,
  and after a failed start had freed it.** `hearth_sink` set two global pointers to the
  player and its server as it made them, on the task that starts them. The control
  surface's task read the same pointers for `GET /status`, `PUT /layout`, `PUT /name`,
  `/wiring`, `/slot-width` and `POST /pairing`, so a request could find a server that had
  not started yet. A start that failed then freed both, whether or not a request was
  still using them. A board that joins a network over Improv starts its player just
  after Improv gives the client the page's address, so a browser that opens the page at
  once can send requests during the start. The player and its server now reach the other
  tasks together, once both have started, and nothing from a failed start reaches them.
  `/status` has `"sendspin": null` until then. Two requests sent during the start used to
  be lost, and now are not:
  - A `PUT /layout` sent before the player had started reached the next control-surface
    play only, and the player started with the layout from before. The player now starts
    with the new layout, or receives it as its start completes.
  - A `PUT /name`, `/wiring` or `/slot-width` sent while the player was starting could be
    lost: the server started with the board's old description, which is what servers
    read in its hello. The server now gets the new one.

**Codec correctness**

- **RF mode decoded 11 dB below a Dolby decoder.** `OperatingMode::kRf` normalised
  dialnorm onto −31 dBFS and applied `compr` with nothing on top, while the Dolby
  Reference Player's RF mode applies each `compr` word with 11 dB that put dialogue at
  −20 dBFS. DEE's own streams measured −30.90 LUFS here against −19.70 LUFS there; they
  now measure −19.90 LUFS. As on the Reference Player, a syncframe with no `compr` word
  stays at line mode's level, and `kCustom` with `heavy_compression` still applies the
  word alone. An E-AC-3 program with dependent substreams now takes its `compr` word
  from the last dependent for every substream (§E3.8.5), as the Reference Player does.
  Before, the bed took the independent substream's word and the dependents' channels
  took none, which the 11 dB would have set 11 dB apart. See `docs/library/decoding.md`.
- **Heavy compression did nothing for a program's dependent substreams.** The encoder
  wrote the last dependent's `compr` as unity regardless of `FrameConfig::heavy`, so a
  program with dependents (7.1, 5.1.2, ...) carried no ceiling for the channels riding
  on them, on top of the decoder gap above: an RF-mode decode applied the fixed 11 dB
  with no cut at all. `AccessUnitEncoder` now measures the last dependent's word from
  the complete rendered program - every dependent's channels folded in the way
  `ac3::OutputStage`'s rendered-layout overload seats a wide layout - while the
  independent substream keeps its own bed-only word, for a receiver that only ever
  decodes the 5.1 downmix.
- **Heavy compression's `compr` words played 11 dB hot on a Dolby decoder.** The
  encoder put RF mode's 11 dB and the dialnorm offset into the word itself, so a stream
  at dialnorm 31 decoded 22 dB above line mode on the Reference Player, which pushed
  pink noise peaking at −20 dBFS past full scale. Words are now written for an RF-mode
  decode that normalises dialnorm and adds the 11 dB itself, the way DEE writes them:
  unity for dialogue-level material at any dialnorm, and cuts sized so the mono downmix
  meets the ceiling after the decoder's own gain. `dialogue=`/`ceiling=` keep their
  meaning and defaults.
- **The AC-4 parser misread everything after a presentation with dialogue enhancement.**
  `presentation_config` 1 ("Main + DE") and 4 ("Main + DE + Associated Audio") read two
  and three substream group references (TS 103 190-2 §6.2.1.3) while counting one and
  two groups; `ac4::`, and so `ac3cli probe`, read by the count, one reference too few,
  and the Python reference parser shared the misreading. No DEE encode writes either
  configuration; synthetic frames in `tests/ac4` now cover both.
- **The AC-4 parser misread everything after an EMDF-only presentation.** A presentation
  with `presentation_config` 6 carries only additional EMDF substreams, whose count and
  `emdf_info()` list TS 103 190-2 §6.2.1.3 reads after the config-6 branch. `ac4::`, and
  so `ac3cli probe`, returned before that loop on both TOC paths, so later presentations,
  the substream groups and `substream_index_table()` were read from the wrong bit. The
  substream groups also took their frame-rate factor from the first presentation, which an
  EMDF-only presentation does not transmit. No DEE encode writes this configuration, and
  the Python reference parser shared the misreading on the `bitstream_version` 2 path.
  Synthetic frames in `tests/ac4` now cover both paths; the committed DEE fixture parses
  identically.
- **The AC-4 parser dereferenced a null pointer on a legal bitstream, and could be made
  to ask for gigabytes.** A stream that clears `b_size_present` left
  `Toc::substream_sizes` empty while `n_substreams` was 1, and `parse_raw_frame()`
  indexed element 0 of the empty vector; the untransmitted substream now runs to the end
  of the frame instead. Separately, five count-driven loops fed by `variable_bits()`
  (including the object-assignment loop, which reached 2^32 once the reader ran dry and
  kept reading phantom zeros) grew a vector without checking for exhaustion — one fuzzed
  frame allocated 1.8 GB and took 6.7 seconds; now 33 MB and 0.03 seconds. Found by the
  new `fuzz_ac4_parse.cpp` within seconds of its first run.
- **The AC-4 and IAB parsers read out of bounds, overflowed `int` and looped forever on
  malformed input, unseen by their fuzz harnesses.** `fuzz_ac4_parse` and
  `fuzz_iab_parse` linked their parser libraries without the ASan, UBSan and coverage
  flags every other fuzzed library is built with, so their earlier clean runs could
  catch a crash, a timeout or an oversized allocation and nothing inside the parsers.
  Instrumented, the committed AC-4 corpus read past a six-entry count table (3-bit
  `n_objects_code` and `isf_config` codes 6 and 7, now naming no objects);
  `presentation_config_ext_info()` overflowed `int` within 9,000 executions; and the MXF
  reader's KLV walk looped forever on a Length near 2^64 that wrapped back to offset 0. A
  `parse_raw_frame()` bounds check that could wrap into a read past the frame, and six
  more `int` additions on counts that escape through `variable_bits()`, are fixed
  alongside. The table reads, the bounds-check wrap, the skip overflow and the KLV loop
  each have a test that fails on the old code under ASan+UBSan, and the two found by
  mutation have reproducers under `fuzz/regressions/`. Both harnesses now run clean for
  300 seconds, and their corpora reach 1,270 (AC-4) and 711 (IAB) edges, against 1,064
  and 564 for corpora grown uninstrumented in the same time.
- **The BW64/ADM reader was the third parser fuzzed blind, and closing that needed a
  patch to a dependency, then a change of dependency.** `ac3adm_objects` was the last
  library a harness links that `fuzz/CMakeLists.txt` did not instrument, and adding it
  stopped `fuzz_adm_parse` within a few hundred executions: libbw64 0.10.0 takes
  `&buffer[0]` of a `std::vector<char>` that a zero-length chunk leaves empty — in
  `UnknownChunk`'s constructor, in `Bw64Reader::read()` and in `Bw64Writer::write()` —
  which UBSan reports and a standard library with its bounds checks on aborts over. A
  `FetchContent` patch fixed all five sites at populate time; upstream made the same
  change in 2021 and has tagged no release carrying it. Instrumented, the harness then
  found, all in `ac3adm`'s own handling of the chunk table: a `<fmt >` whose channel
  count and sample width overflow libbw64's `uint16_t` block alignment had its read
  buffer sized from the wrapped value and decoded against the real one — a heap overread
  that an uninstrumented build ran as a clean execution; the chunk-table pre-check
  stopped at an RF64 `<data>` declaring more than the file holds, leaving the chunks
  behind it to be allocated whole (1.7 GB, found by mutation); a 28-byte `<ds64>`
  declaring 4.26 billion table entries drove a loop of that many reads; a file ending in
  a fragment too short to be a chunk header had that header's size read out of
  uninitialised stack (`malloc(4278190080)`, from 19 bytes); and a hang in this module's
  own float-detection pass, which stepped over chunks in 32-bit arithmetic that wrapped
  to zero on one declared size, was reachable through every file that pass ran ahead of
  libbw64 on.

  Closing one further gap — a `<ds64>` table entry giving some other chunk than `<data>`
  a 64-bit size, which the pre-check does not read — meant moving off the EBU's own
  `github.com/ebu/libbw64` (last tagged January 2019) to a maintained fork,
  `github.com/pwnified/libbw64`, which carries the EBU's own 77 unreleased commits
  forward and closes it. The fork also reads `WAVE_FORMAT_IEEE_FLOAT` natively, so the
  hand-rolled container walk this module used to fall back to for float samples
  (`float_pcm_bw64.cpp`/`.hpp`) is retired — both integer PCM and float go through one
  path now. Two small patches remain against the fork, each with an upstream PR
  proposing the same fix: `<data>` still has to be exempt from the fork's stricter
  end-of-file check, the way every prior libbw64 allowed, for a recording truncated
  mid-capture to keep reading; and a 64-bit `WAVE_FORMAT_IEEE_FLOAT` `<fmt >`, which the
  fork's own decoder already handles correctly, needs one more accepted bit depth to
  reach it — a capability gap this module's own docs had claimed was covered since
  before this fuzz work, caught only once a test for it existed.

  Each finding has a reproducer under `fuzz/regressions/fuzz_adm_parse/`, and the
  memory-safety ones have tests in `tests/adm/` that fail on the old code. The harness
  now runs a full 300-second budget clean at 1,911 executions a second — against the
  first instrumented run's 489, itself already up from the uninstrumented harness's 253
  — and its corpus reaches 1,752 edges, against 114 for the uninstrumented harness's own
  translation unit.
- Short E-AC-3 syncframes (`numblkscod` 0–2) were sized at the full six-block byte
  budget, so a short stream measured up to 6x its nominal bit rate. CBR frames now take
  `frame_words`' documented per-block scaling; six-block streams are unchanged.
- **A §E2.3.1.2 legacy-core stream failed to decode, or silently selected the wrong
  programme.** Programme selection parsed an AC-3 core's lead frame as an Annex E
  syncframe — but an AC-3 core carries neither `strmtyp` nor `substreamid`, so the
  selection read a programme id out of the `crc1` checksum: about a quarter of frames
  failed outright, the rest were silently mis-selected. The identity is now asserted
  from `bsid`. FFmpeg's FATE fixture `the_great_wall_7.1.eac3` (an AC-3 core plus an
  Annex E extension to 7.1) now decodes all 157 access units; it had failed on its
  first.
- **Dual mono's output-stage dialnorm normalisation levelled Ch2 by Ch1's dialnorm,
  not its own.** `OutputStage::apply` took one `dialnorm` and scaled every channel by
  it; acmod 0 (1+1) codes two unrelated programmes with independent dialnorm words
  (§5.4.2.16's `dialnorm2` for Ch2), and an encoder sizes Ch2's `compr2` on the
  assumption Ch2 is normalised by `dialnorm2`. A 1+1 stream with dialnorm 27 and
  dialnorm2 20 played Ch2 7 dB too quiet under `kLine`/`kRf`/`apply_dialnorm`.
  `apply()` now takes an optional second dialnorm and levels Ch2 by it alone;
  `FrameDecoder`, `Eac3Decoder` (`decode_access_unit` and `flush()`) and the WASM
  decode demo's own side fold all thread it through.
- **A §E2.3.1.2 legacy core's own output stage ran a second time, ahead of the
  programme it belongs to.** `decode_ac3_core` built the core's `FrameDecoder` from
  the whole `DecoderConfig`, `output` included, so `OperatingMode::kLine` normalised
  the bed's channels once inside that decoder and again over the eight-channel
  programme `apply_output` assembles from it — measured at dialnorm 24, the bed came
  out 14 dB down and the dependent's own channels, which never pass through the
  core, 7. A downmix target folded the bed to two channels before the dependent's
  could be laid over it, failing every access unit with `kInvalidStream`. The core
  now decodes with `output` reset; `drc_scale`, `heavy_compression` and every other
  field are unchanged.
- **A §E2.3.1.2 legacy core folded with the AC-3 defaults instead of its own downmix
  levels.** `decode_ac3_core` copied the core's acmod, `dialnorm`, `compr` and so on onto
  `DecodedSubstream`, but not its `cmixlev`/`surmixlev` or, for a `bsid`-6 core, Annex
  D's `xbsi1` group — a legacy core has no `mixmdate` to carry them in at all, and the
  fields those needed did not exist on `DecodedSubstream`/`DecodedAccessUnit`. So
  `apply_output()` and `flush()` always folded a legacy core's programme with §7.8's
  −4.5 dB centre / −6 dB surround, whatever the core's own bsi or `xbsi1` actually said.
  Both structs now carry `bsid` alongside `cmixlev`/`surmixlev`/`alternate_bsi`, and the
  fold resolves them through the same `ac3::mix_levels(acmod, cmixlev, surmixlev,
  alternate_bsi)` overload `FrameDecoder` already uses for a bare AC-3 stream.
- `ac3cli` reports a decode failure in words (`decode failed: a header field holds a
  value A/52 reserves`) rather than as a bare enumerator — nine call sites across
  `decode`, `analysis` and `live` weren't using the existing `describe()`.
- **The AC-3 encoder's heavy compression measured only bsi's downmix levels, leaving
  no ceiling for a compliant Annex D decoder's own Lo/Ro fold.** §D4.1.1 requires
  overload protection to hold for either kind of decoder, in any downmix mode; a
  compliant decoder folding mono from xbsi1's `lorocmixlev`/`lorosurmixlev` (§D3.1.2)
  instead of bsi's `cmixlev`/`surmixlev` can peak several dB louder, since
  `lorocmixlev` runs up to +3 dB against `cmixlev`'s -6 dB floor. `compr` is now
  driven by whichever of the two folds peaks louder whenever `alternate_bsi->mix` is
  set.
- **A reserved `dmixmod` read back as "not indicated".** A/52:2018 Table D2.2 and ETSI
  TS 102 366 V1.4.1 Table D.1.1 both list `'11'` as reserved, and Annex E gives
  E-AC-3's `mixmdate` field the same table, so neither codec defines a fourth preferred
  downmix. Both decoders, `io::read_frame_header` and `io::read_frame_metadata` used to
  store `'11'` as `'00'`. `meta::DownmixMode::kReserved` now keeps it, and
  `meta::describe()` names it. Both encoders refuse to write it, as they already refuse
  reserved surround levels, and `transcode` carries a reserved source value across as
  not indicated.
- **The renderer played a JOC programme's LFE ahead of its objects.** A reconstructed
  object comes out `oba::joc::reconstruction_delay()` samples after the bed it was pulled
  from: 576 (12 ms) in the QMF domain the decoder uses by default, 256 in the MDCT-band
  one. `ac3::render::LayoutRenderer::render()` played the bed's LFE beside the objects as
  it arrived, so on the ESP32 player and Hearth's test sink the LFE led the objects by
  that much. While objects are placed, the LFE now goes through a delay line of that
  length. `set_joc_domain()` sets the length, and the ESP32 player passes its decoder's
  domain. The line takes 2,304 bytes for a 5.1 bed, allocated when a unit's objects are
  first placed, so a player that only plays the bed pays nothing. Measured end to end on
  a stream this project's encoder writes, with one pulse sent to both an object and the
  LFE (`tests/render/test_object_lfe_timing.cpp`): the LFE feed had it 576 samples before
  the object's speaker, and now both have it at 832. In the MDCT-band domain the LFE was
  256 samples early, and both are now at 512. The QEMU 7.1.4 render run's twelve slot
  levels are unchanged. `set_bed()` stays idempotent for an unchanged bed, as it was
  before: only a genuine change of which coded channels are LFE empties the delay line,
  so a caller that re-announces the same bed every unit (as Hearth's own local decode
  reference does) still agrees with one that calls `set_bed()` only when the bed changes
  (as the players do).

**Robustness and diagnostics**

- The four copies of the IAB `BitWriter::push_plex` test helper could shift by 64 —
  `width` doubles through 4/8/16/32/64, and a value at or above `0xFFFFFFFE` hit
  undefined behaviour. The reader has always had the bound (`width >= 32` returns
  `kBadEscape`); the writers now match it. Unreachable for these fixtures' actual
  values.
- **`bap-census=` was accepted by eight commands that cannot produce one** (`qc`,
  `levels`, `transcode`, `probe`, `normalize`, `cut`, `spdif`, `mkv`) — each parsed the
  key and silently did nothing, exiting 0. The option is now scoped to `decode`, the
  only command that builds a census; every other command refuses it with the parser's
  existing `error: unknown option`.
- **`ac3cli decode … bap-census=` silently wrote nothing for E-AC-3 input**, though the
  trace was wired in and its cost paid — the AC-3 path had always written it. Both paths
  now write at the same point; covered by a CLI test over single- and multi-substream
  E-AC-3.
- **`quiet` crashed `decode` on a multi-programme or richly-annotated stream, and
  crashed `transcode`, `metadata`, `normalize`, `cut` and `cat` on every stream.**
  `quiet` makes the status stream a null `FILE*`; several report lines called
  `fmt::println` on it directly instead of going through `status_println`, which the
  Windows CRT's parameter check turns into a hard crash. All such lines now route
  through `status_println`.
- **`quiet` left three of `monitor`'s status lines on stdout, and `decode` wrote its
  object-signature summary ahead of a `-` output's WAV data.** All four now go to the
  command's status stream, tested with and without `quiet`.
- **`monitor` misdescribed the object layer of every bed programme, and claimed an LFE
  object for streams that carry none** — its own copy of `decode`'s object-count line
  had kept only the shape this project's own encoder writes. Both commands now report
  through one shared function, `print_object_summary`, tested against a 5.1.4 bed
  programme and objects with no LFE.
- **`transcode dialnorm=auto` and `dialnorm2=auto` did not measure anything.**
  `parse_options` marks the option as given, which skipped the carry from the source, but
  nothing in `run_transcode` read the measurement flag it also sets — the encoder was
  built from `plan::Metadata`'s unmeasured default of 31, printed as `(from dialnorm=)` as
  if the operator had typed it. Both now run the same BS.1770 pass `normalize` makes over
  the source and print `(measured)` instead.
- **`transcode` crashed, printing nothing, when the encoder refused the configuration it
  carried from the source.** A `dialnorm` or `dialnorm2` of 0, which §5.4.2.8 reserves and
  a decoder reads as 31, is one such value. The E-AC-3 encoder refuses it when it is built,
  by coding no channels, and `transcode` went on to render the decoded audio into a channel
  list sized for none (`0xC0000005` on Windows). It now stops before decoding and prints the
  encoder's reason. Transcoding the same stream to AC-3 reported
  `bitrate must be a legal AC-3 rate` whatever the refusal was; both codecs now name the
  cause, as in `dialnorm out of range 1..31`.

**Crucible desktop application**

- **A Crucible re-probe requested while one was already running was silently dropped**,
  served by nobody — not queued, not retried — affecting every caller (Re-probe, pinning
  a mode, choosing an endpoint, the device watcher itself). A request that arrives mid-
  enumeration is now kept and starts a fresh probe once the running one finishes.
- **Crucible tapped applications before it had anywhere to play them.** On macOS the
  Core Audio process tap mutes the source app while tapped, so a machine with no usable
  output would have muted every application and delivered its audio nowhere. Taps now
  open only while the output stage has an endpoint, checked every frame.
- **Crucible reported a running engine on a machine with nothing to play into.**
  `Engine::start()` reported success before the worker thread had built the output stage
  or opened a sink; it now waits for the worker under two deadlines and the status strip
  reports why start failed instead of claiming success. A second bug found while testing
  this: a stopped-and-restarted engine never re-enumerated.
- **Crucible listed every PulseAudio application on Linux as one entry, and could tap
  none of them** — PipeWire reports the pid of `pipewire-pulse`, the relay every
  PulseAudio-API app talks through, not the app's own pid. A stream whose `client.api`
  names a relay is now bound for `application.process.id` instead.
- **Crucible could not start on a Linux desktop with a system tray**, crashing on nine
  or ten launches out of ten: a `Qt.labs.platform` submenu nested in the tray icon's
  menu triggers a type-confusion `static_cast` inside Qt's D-Bus tray implementation.
  The tray's menu is now flat on every platform, with a regression test pinning the
  constraint.
- The room page described Windows' application-list behaviour on Linux, where PipeWire
  (unlike Windows' session model) only shows an application while it's actually playing
  sound.
- **Crucible's headphones output played a decoded Atmos programme's dynamic objects
  against an LFE that arrived 576 samples too early** - the same JOC reconstruction
  delay `ac3cli spatial` had (see "Command line and GUI" above). `OutputStage::submit`'s
  spatial-sink branch now holds the LFE back by the same FIFO delay line.

**Tooling, packaging and release engineering**

- **Every Linux and macOS package shipped without the `ac3cli` man page or any of the
  four shell completions.** They were guarded by `if(CMAKE_CROSSCOMPILING)` on the
  mistaken assumption this meant only the arm64 cross legs — it's set whenever a
  toolchain file supplies `CMAKE_SYSTEM_NAME`, which every Linux/macOS preset does. Only
  the Homebrew formula (no toolchain file) got them, which is why the one test asserting
  they exist kept passing. The guard now compares host and target system name/processor.
- **A dispatched release would have published the Linux Crucible package stamped with
  the previous release's version** — its configure step was the one of three that passed
  no `DERIVED_VERSION_OVERRIDE`, so `git describe` saw the previous tag before the new
  one was pushed. Fixed, plus three smaller release-path gaps: missing `.sha512` side-
  cars for the Linux Crucible assets, a `SHA512SUMS` glob missing `*.AppImage`, and a
  release job that only checked some package existed rather than every promised one.
- **The README's decode-accuracy badge disagreed with the page it links to.** Per-
  channel SNR floors taught the docs page to report the tightest per-channel margin, but
  the badge generator kept the old scalar rule (worst absolute dB) — on one commit the
  badge read 18.3 dB (a surround 1.3 dB clear of its floor) while the page read 58.1 dB
  (the front channel genuinely closest to failing), and the badge's colour could stay
  green while the gold-reference gate itself failed. The badge now runs the same
  computation as the page.

**Browser (WASM)**

- The WASM demos' three unlabelled `<input>` elements (the stream picker, the seek
  slider, the WAV picker) now carry an `aria-label`, found by the first SonarCloud scan.
- **The AudioWorklet pipeline's browser test never ran.** `worklet.spec.js` matched no
  Playwright project, so it silently skipped in CI while the docs described that
  pipeline as covered. It now runs in the decode project; wiring it in also found the
  spec navigating to a 404 page that read as a COOP/COEP failure.

**Build system**

- **macOS cross-builds compiled the wrong architecture's SIMD kernels.**
  `AC3FORGE_SIMD`'s `auto` keyed on `CMAKE_SYSTEM_PROCESSOR`, which on Apple platforms
  describes the host, not `CMAKE_OSX_ARCHITECTURES`'s target — building arm64 from an
  Intel Mac handed it SSE2/AVX2 intrinsics and failed outright. Both now follow the
  effective target architecture; a universal configure resolves `generic`.
- **An installed {fmt} older than 11.1.0 was accepted, and the build then failed.**
  `cmake/Fmt.cmake` looked {fmt} up with no version, so Ubuntu 26.04's `libfmt-dev`
  10.1.1 satisfied it and compilation stopped at the first `#include <fmt/base.h>`, a
  header fmt 11 introduced. The lookup now asks for 11.1.0 or newer, the first release
  the tree builds against (11.0.x's `fmt/chrono.h` fails under Clang 22): an older copy
  is skipped and named in the configure output, and the `FetchContent` fallback (or the
  `AC3FORGE_FETCH_FMT=OFF` error) applies. A build directory that had already cached
  the old copy recovers on its next configure.

**Audio backend and object signing**

- **`MonitorSink::start()` could not say a device had refused this shared-mode format,
  rather than something else failing.** Every failure past device resolution returned the
  same `kComFailure` on Windows, ALSA and Core Audio, so a caller could not tell "this
  device will not do the rate or channel count you asked for" from a COM/ALSA/HAL problem —
  diagnosing an HDMI/AVR endpoint locked to a non-48kHz shared-mode rate needed a
  standalone WASAPI probe written outside this codebase to find the `AUDCLNT_E_UNSUPPORTED_FORMAT`
  underneath the generic message. `start()` now reports a new `MonitorError::kFormatRejected`
  for that HRESULT specifically on Windows, for the channel/rate `hw_params` calls on ALSA,
  and for the equivalent channel-count/nominal-rate checks on Core Audio. PipeWire and AAudio
  hand format negotiation to a graph or mixer that converts rather than refuses, so neither
  backend returns it.
- **An output device that went away left the sink saying it was still playing.** A render
  thread that met a device failure - an unplugged endpoint answering
  `AUDCLNT_E_DEVICE_INVALIDATED`, ALSA giving up on `-ENODEV`, an AAudio write refused -
  ended and left `running()` true behind it, so `position()` reported a clock that had
  stopped, `submit()` went on filling a queue nobody read, and `flush()` waited out its
  timeout. Every backend now stops its sink when this happens: `running()` says so,
  `position()` reports nothing, `submit()` refuses, and `flush()`, `pause()` and
  `resume()` answer at once. `start()` opens again with no `stop()` needed first. Hearth's
  player stops playback and says which output went away, rather than waiting for a clock
  that will not move again; before, a lost device left it playing for ever with nothing
  said. Two hidden cases, `ac3tests "[passthrough-unplug]"` and `"[monitor-unplug]"`, take
  a person through unplugging a real output.
- **`SpatialObjectSink` was left out of that same fix, and still reported itself running
  after its render stream had gone.** `BeginUpdatingAudioObjects` failing outright, or the
  endpoint simply going quiet with no other word - a removed one need never signal the
  render-ready event again either - left `running()` true, so `submit()`/`can_submit()` went
  on taking objects into rings nobody drained. The Windows backend now stops itself the same
  way, reading back `GetMaxDynamicObjectCount` on the `ISpatialAudioClient` on a wait that
  times out to catch the quiet case (not the stream's own `GetAvailableDynamicObjectCount`,
  which Microsoft's own reference says not to call once streaming has started), and `start()`
  opens again with no `stop()` needed first, as the other two sinks already do. `ac3tests
  "[spatial-unplug]"` is its own hidden case.
- **The GUI, Crucible and the Shield Android demo still spun forever on a lost output
  device.** `running()` turning false (see above) was not enough on its own:
  `EncoderController`'s file-to-receiver, motion-preview and live-session workers,
  `ObjectDecodeController`'s audition and `StreamPlayerController`'s playback all retried
  `submit()` on nothing but a stop or pause flag, so a lost device left each one waiting
  on audio that would never resume - the file-to-receiver play flag never cleared,
  refusing every later play. `OutputStage`'s own seam (`BurstSink`/`PcmSink`/`ObjectSink`)
  exposed no `running()` at all, so a re-probe that still found the same dead endpoint
  listed read as "nothing changed" and kept the dead sink for good. Shield's
  `live_cursor` encode loop had the same shape, and `MainActivity`'s underrun-based
  recovery stopped working at exactly the point it mattered, because a sink that has
  stopped `running()` refuses every `submit()` without ever reaching the render code
  that counts a real underrun. All now stop (or, once their next reprobe/reconcile
  runs, restart) instead of hanging.
- **Two more callers kept retrying a spatial sink that had already stopped itself.**
  `ac3cli spatial`'s submit loop had no `running()` check at all, so an unplugged or
  disabled endpoint hung the command for ever rather than ending with the reason
  printed, the way a lost device already ends other commands; its final drain-wait
  gets the same check. Crucible's `submit_with_patience()` - shared by the passthrough,
  monitor and spatial legs - still waited out its full ~200 ms patience window on every
  single frame once a sink had stopped itself, rather than counting the one underrun
  and moving on immediately; `OutputStage::apply()`'s own reprobe already restarts a
  sink in this state (see above), so only the per-frame wait needed shortening.
- **`PassthroughSink` crashed the instant a real exclusive-mode bitstream endpoint drove
  it** — surfaced once an Onkyo TX-RZ740 over HDMI locked AC-3, E-AC-3 and signed Atmos
  through it for the first time. `Activate`/`Initialize` ran on the calling thread while
  `Start`/`GetBuffer`/`Stop` ran on a worker thread, fatal inside `AUDIOSES.DLL` for a
  real exclusive-mode client; the whole WASAPI lifecycle now runs on one worker thread.
  The same session found the "bursts rendered" counter truncating to zero almost every
  callback, hanging the CLI's drain-wait loop after playback had already finished.
- **`ac3::signing::decode_signing_key` silently signed with the wrong bytes** when a key
  file held a comma-separated `0xHH` hex-array export — a common disassembler shape, and
  how this project's own reverse-engineered test key was saved — rather than base64 or
  raw binary. Self-consistent against this project's own round-trip but rejected by a
  real licensed decoder, which is how a real AV receiver refusing to unlock a signed
  Atmos object layer surfaced it. Now recognises the format and refuses ambiguous
  hex/array-shaped content instead of silently taking it as raw key bytes.

## [0.10.0-beta.1] - 2026-09-01

Tenth tagged release. The E-AC-3 encoder catches up with the decision quality AC-3 got in 0.7.0,
both decoders gain a consumer output stage, all three containers become readable as well as
writable, and the verification estate extends to E-AC-3. The immersive surface widens well past
Atmos-in-DD+: an IAMF writer, an IAB/MXF reader bridged onto the Atmos encoder, and an AC-4
bitstream inspector. The browser gains in-page encoding and QC beside the existing decode demo,
plus a reusable streaming decoder package. The Shield Atmos demo grows into a real application,
and the library is now reachable from Rust as well as C and Python. The repository also moved to
trunk-based development, and a concrete API-freeze plan for v1.0 now exists.

### Added

**Encoding**

- **Per-channel exponent strategies and short syncframes for E-AC-3.** The encoder wrote one
  exponent set per frame for every channel; it now plans them per channel or per block, and can
  emit 1/2/3-block syncframes with `convsync`. Spectral distance improves about 0.6 dB on
  transient material. See [Encoding E-AC-3](docs/library/encoding-eac3.md).
- **Real bit-allocation parameters for E-AC-3.** `bamode=1` transmits the frame's own parameters
  instead of inheriting Table E1.4's, and `dbpbcod=3` — measured better at every rate and layout
  tried — replaces the pinned default.
- **Content-decided `dithflag`** on both encoders, per channel per block, free in bits because
  the flag is transmitted either way. `dither=off` pins it at 0 for callers needing bit-for-bit
  agreement between two decodes.
- **Delta bit allocation under coupling, and per-channel coupling membership.** Delta is no
  longer skipped whenever coupling is active; `chincpl` is decided per channel rather than
  frame-wide; 2/0 gets a measured phase-restoring `phsflg`; enhanced-coupling angle interpolation
  is encoded and decoded.
- **Content-adaptive tool selection.** E-AC-3's `auto` chooses coupling, spectral extension and
  AHT from the frame's own spectrum rather than the bit rate alone: +0.11 MOS and +0.36 dB on
  real programme material.
- **Average-rate (ABR) E-AC-3 encoding.** `vbr=avg:kbps[,win:frames]` holds a long-run average
  through a sliding bit reservoir. [The rate-control curve](docs/concepts/ac3-eac3.md#e-ac-3-rate-control-what-vbr-and-abr-are-worth)
  shows where CBR, VBR and ABR each win.
- **A per-frame bit-allocation search** (`EncoderConfig::search`, `eac3::FrameConfig::search`),
  judged by a decoded-domain distortion measure and psychoacoustic model in `ac3::quality`.
  `search=distortion` is a measured win on AC-3 from 448 kbit/s up; `search=perceptual` is not
  yet competitive. Both are off by default. See [Quality measures](docs/library/quality.md).
- **`fgaincod` is settable on both codecs** (`fgaincod=` on the command line). On E-AC-3 this
  means writing Table E1.4's per-block `fgaincode` element, which the encoder had never emitted.
  The default is unchanged and writes no element at all.

**Decoding and playback**

- **A decoder output stage.** `ac3::OutputStage` applies dialnorm, the §7.8 Lo/Ro, Lt/Rt and mono
  downmixes using the stream's own levels, LFE mixing, and the line and RF operating modes.
  Reachable as `decode`/`monitor`'s `channels=`, `downmix=`, `drcmode=` and `mix-lfe`. Off by
  default, so existing callers are unaffected. Lo/Ro agrees with FFmpeg's own fold to 119–121 dB.
  See [Decoding](docs/library/decoding.md).
- **Error concealment**, opt-in: a bad frame is repeated-and-faded or muted in the overlap-add
  domain, so the delay state stays coherent. `conceal=repeat|mute`.
- **The full metadata surface**, writable and reportable on both codecs instead of being constants
  on the way out and skipped on the way in: AC-3's Annex D alternate syntax (`bsid` 6), the
  informational BSI fields, and E-AC-3's `mixmdate` and `infomdat` groups — `bsmod`, `dsurmod`,
  separate Lt/Rt and Lo/Ro levels, programme scale factors, mixing and pan information. See
  [Metadata](docs/library/metadata.md).
- **More than one programme per stream.** Access units are grouped by programme; `decode`, `qc`
  and `levels` take `programme=<0..7>`, and `eac3-encode` can author a second with `programme2=`.
- **Third-party Atmos streams decode.** OAMD, JOC and EMDF read the real breadth of the syntax —
  multiple update blocks, object size/zone/snap, sparse JOC matrices, alternate object data,
  several bed instances — rather than only the shapes this encoder produces. A committed Dolby
  Encoding Engine fixture exercises it.
- **QMF-domain JOC.** Object reconstruction runs in the 64-band complex filterbank the format
  calls for. Mean per-object SNR 22.8 → 28.6 dB. `joc-domain=qmf|mdct` selects it on both sides.
- **A consumer-facing diagnostic sink.** `DecoderConfig::diagnostics` reports recoverable,
  informational decode events — a CRC failure (fired the moment the check runs, so it still
  reaches a caller even when `conceal=` turns the same frame into a successful, concealed
  result) and an EMDF payload id neither decoder interprets. A plain function pointer, no
  allocation, off by default, usable from the minimum-footprint decoder profile.

**Containers and streams**

- **Readers for Matroska, MP4 and MPEG-TS**, plus `ac3cli demux`. Each reads real third-party
  shapes this project never writes, and `decode`, `qc`, `levels`, `play` and `monitor` — and the
  GUI's QC and Inspect pickers — now take a container directly, sniffed by content rather than by
  extension. See [Muxing and sinks](docs/library/muxing-and-sinks.md).
- **`ac3cli probe`**: what a stream declares — layout, substream map, tools in use, metadata
  ranges, CRC validity — without decoding audio. `json=1` emits a versioned schema.
  [Command reference](docs/forge/cli/commands.md).
- **`ac3cli probe` reads AC-4 too**, auto-detected. A new standalone `ac4::` library parses the
  sync frame, table of contents, presentation and substream-group framing (ETSI TS 103 190-1/-2)
  — channel-coded, A-JOC-coded, direct-coded-object and OAMD substream groups alike, including
  7.0.4 through 22.2 channel-based immersive layouts — bitstream inspection, not decoding: audio
  content is reported by byte range, never decoded, and `oamd_common_data()` is refused cleanly
  rather than misparsed. Backed by an independent Python transcription, real Dolby Encoding
  Engine fixtures for the channel-coded path, and synthetic hand-built vectors for A-JOC/object/
  OAMD, no real fixture being reachable for that path. See [Validation](docs/verification.md#ac-4).
- **IEC 61937 de-framing.** A burst parser and `ac3cli unspdif`, plus capture-side recognition, so
  a loopback of a bitstreaming player records the elementary stream rather than PCM.
- **A streaming fMP4/CMAF fragmenter** with a rolling HLS playlist and dynamic MPD, the DASH
  object-audio signalling, the `ceao` brand, and the MPEG-TS ATSC profile beside DVB.
- **Object-layer strip without re-encoding**, so a JOC stream yields a bit-identical-bed 5.1
  companion rendition (`strip-objects`, and `fmp4 … fallback-51` writing both).
- **Stream tools that leave the audio alone**: `transcode` (DD+→DD, carrying metadata across),
  in-place metadata rewrite with CRCs re-stamped, and access-unit-aligned `cut`/`cat`.

**Immersive formats**

- **A JOC → ADM BWF writer.** `decode … adm_out` writes a Dolby Atmos Master ADM Profile BW64
  from a decoded stream's own bed LFE and reconstructed objects, positioned by their real OAMD
  timeline. Scoped to dynamic-object-only programmes. Needs `-DAC3FORGE_BUILD_ADM=ON`.
- **`iamf`, a writer for AOM's IAMF (Immersive Audio Model and Formats) v1.1.0.** E-AC-3 can never
  be an IAMF codec, so this decodes a 7.1.4 stream and re-wraps it as a channel-based IAMF Audio
  Element carrying `ipcm` substreams, in IAMF's own ISO-BMFF encapsulation — a direct route to the
  IAMF/Eclipsa Audio ecosystem alongside the indirect one the ADM writer above already opens
  (AOM's `iamf-tools` encoder accepts ADM-BWF input). Object elements and a reader are not
  started. See [IAMF writing](docs/library/iamf.md).
- **`ac3iab`, a reader for SMPTE ST 2098-2's Immersive Audio Bitstream** — the frame framing and
  every element in the format's element tree, with positions, spreads and gains resolved. Its
  lossless coder is read by identity only. Validated against the DTS reference validator's own
  sample corpus. Reads real MXF IAB Track Files too (`ac3iab::parse_mxf_iab`), not just a bare
  elementary `.iab` file — the wrapping is governed by a separate standard, SMPTE ST 2067-201,
  which clip-wraps the whole bitstream as a single KLV.
- **`atmos-iab`: a real Dolby Atmos cinema/IMF master straight to DD+ JOC E-AC-3.** Every Bed
  channel/Object an IAB file (or MXF Track File) names becomes an `AtmosEncoder` object, driven by
  the file's own authored per-frame panning — `ac3::admbridge::build_iab`, the IAB counterpart to
  the existing `atmos-adm`/ADM bridge. Needs `-DAC3FORGE_BUILD_ADM=ON`.
- **One object-scene timeline type** (`ac3::oba::ObjectScene`) shared by `atmos-path`, the GUI and
  the examples, replacing four ad-hoc formats.
- **Object extent, channel lock and zone constraints on encode**, mapped from the ADM bridge.

**Command line and GUI**

- **A documented exit-code scheme**, `help <command>`, `quiet`/`verbose`, and a man page and shell
  completions generated from the same command table and installed by the build.
- **`record` and `live` reach parity with the GUI session**: any layout up to 7.1.4, either codec,
  `container=raw|mkv|ts|spdif|fmp4` written incrementally, a capture-silence watchdog, an object
  slot budget for `mode=atmos`, and a parallel 5.1 leg for an AC-3-only endpoint.
- **Live object positioning over OSC**, replacing the synthetic orbit `live mode=atmos` and the
  GUI's live room used to fake motion with. `ac3cli live ... mode=atmos positions=osc:<port>`
  and a "Drive objects from OSC" toggle on the GUI's Live session card both drive object
  placement from a show-control rig or a DAW in real time (`/object/<n>/xyz|gain|lfe|release`,
  0-based), room markers greying out while a live update owns them. Loopback-only by default;
  `positions=osc:any:<port>` opts into every interface. MIDI and a desktop game controller are
  follow-ons under the same `positions=<scheme>:...` grammar, not implemented yet.
- **`play` follows the sink**: it reads what a chosen receiver actually accepts (EDID short audio
  descriptors on ALSA; a live probe elsewhere) and adapts instead of refusing — a source format the
  sink can't bitstream is transcoded to AC-3 or decoded to PCM automatically, so the "no 5.1 PCM
  over optical" case now takes one command instead of two. `follow=off` restores the old refusal.
- **A GUI stream player** — the twin of `ac3cli monitor` — with transport, live meters, the
  soundfield view, and WAV/object export from the same decode pass. A finished run offers **QC
  this run** and **Inspect objects** directly. See [Open stream](docs/forge/gui/open-stream.md).
- **Desktop integration**: drag-and-drop, `ac3gui <file>`, and `.ac3`/`.ec3` file associations on
  Windows, macOS and Linux, so the app appears in application menus instead of being launch-only.
- **A self-contained Linux AppImage for `ac3gui`**, bundling its own Qt 6 instead of depending on
  the host distro's own `qt6-base-dev`/`qml6-module-*` split, alongside the existing `.deb`/`.rpm`.
  See [Linux](docs/platforms/linux.md#appimage).
- **Loudness of the rendered layout and of objects.** Metering follows BS.1770-5's extended
  algorithm for advanced sound systems, weighting channels by position, and can re-render an
  object programme onto a named layout by its own positions before metering. `qc` gained
  `layout=rendered|bed` and `objects=<layout>`, plus two new delivery presets.
- **GUI localisation.** A Preferences **Language** picker switches the app live between English
  and five real languages (Français, Deutsch, Español, العربية, עברית, יידיש — the same set the
  sibling CountdownSolver project ships), with right-to-left mirroring and bundled Noto Sans
  Arabic/Hebrew faces for the three languages that need them. Coverage is partial today (window
  chrome, tab names, the Guided wizard, all of Preferences) and tracked, not hidden — see
  [Localisation](docs/forge/gui/localisation.md). A pseudo-locale QA fixture proves the extraction/
  compile/load pipeline end to end independent of real-language completeness, and CI now fails if
  a `qsTr()` change isn't reflected in the committed translation catalogue.
- **GUI accessibility.** Every custom control and every control in the main window now reports a
  real `Accessible` name, role and description to screen readers, built from the same live state
  the visuals already read rather than a static copy of a label — channel meters, QC gates, the
  Guided wizard's cards, the object-placement room and timeline views, run-strip chips, all of it.
- **`ac3cli spatial`, a Windows Spatial Sound object sink.** Every JOC-reconstructed object goes
  out as a dynamic object at its real OAMD position, and the bed's LFE as a static one, through
  `ISpatialAudioObjectRenderStream`. This is the one path that lets Dolby's own renderer engage
  with this project's reconstructed objects at all — a licensed decoder otherwise refuses to
  object-decode a stream without a signing key this project doesn't ship. Refuses cleanly, naming
  which Settings toggle to flip, when the chosen endpoint has no spatial sound format enabled;
  `ac3cli outputs` reports each device's spatial capability alongside its passthrough columns.

**Browser (WASM)**

- **An in-browser encode demo**, alongside the existing decode one: drop a `.wav` file and get back
  a real AC-3/E-AC-3 elementary stream, encoded entirely client-side by the same codec compiled to
  WebAssembly, plus a real BS.1770 loudness/true-peak QC verdict against the same five delivery
  presets `ac3cli qc` checks — computed on the same PCM, in the page. A round-trip preview decodes
  the produced stream through the existing decode module to prove it's real. Headless-browser CI
  coverage (Playwright) now spans both demos, not just decode.
- **A reusable browser decoder package**, `ac3forge-wasm-decoder` (source in
  [`js/`](https://github.com/iainchesworthlabs/ac3forge/tree/main/js)), turning the WASM decode
  demo's underlying build into a reusable browser decoder — a real answer to Chrome's continued
  inability to decode EC-3 natively
  ([video.js http-streaming#1297](https://github.com/videojs/http-streaming/issues/1297)). It is
  **not on the npm registry yet**: the publish job is deliberately held to a manual dispatch until
  the one-time npmjs trusted-publisher setup in [docs/releasing.md](docs/releasing.md) is done, so
  consume it from source for now.
- **A push-frame decode API** over the caller-buffer `decode_access_unit_into` form, so decoding a
  live/streaming source allocates nothing on the hot path.
- **A realtime AudioWorklet playback pipeline**: decoding runs in a Worker, off the main thread;
  only a lock-free `SharedArrayBuffer` ring-buffer drain runs on the audio-rendering thread.
  Multichannel output or the library's own §7.8 downmix (never a hand-rolled fold) is selectable
  per stream.
- **An hls.js/MSE bridge** for playing EC-3 audio where the browser cannot decode it natively —
  patches `MediaSource`'s codec-support/`SourceBuffer` surface (a passive event listener alone
  doesn't work: hls.js drops an audio track outright the moment the real `addSourceBuffer` throws
  for an unsupported codec) and extracts access units from the fMP4 segments hls.js's own remuxer
  produces.
- The docs site's WASM demo now consumes the bundled JavaScript bindings and their
  decode/playback logic.

**Shield Atmos Demo (Android)**

- **New: the wire trace.** A second thread parses back the exact access units going out over HDMI
  and draws what a decoder finds in them — the lead object's intended height against the height read
  back off the wire, which is a visible staircase because height is sent in sixteen steps. It
  deliberately computes no reconstruction-quality figure: both ends share the same non-normative QMF
  prototype, so such a number would be unfalsifiable by construction. What it does prove is that the
  object container survives on the wire, and that OBJECTS OFF genuinely removes it.
- **New: five demo scenes and a guided tour.** The app had exactly one thing to show — three
  objects on fixed orbits — from launch until you walked away. It now has Orbit, Flyover, Overhead,
  Elevator and Front/back, each with its own line of what to listen for, blended rather than jumped
  between; and once left idle it walks them itself rather than just inviting the next person.
- **New: record a path and loop it.** Fly the object by hand, press again, and it flies your own
  gesture forever — still pushable, still springing back to itself.
- **New: controller rumble** on the two crossings the ear is least sure of: passing overhead, and
  passing through the listening position.
- **New: a settings panel and a phone remote.** Every control was previously an undocumented
  keypress. The panel is D-pad navigable; the phone remote serves one page so anyone in the room can
  drive the object from their own phone. The remote is **off by default** and has no authentication
  — it starts only when switched on, and stops when the demo leaves the screen.
- **New: OBJECTS OFF** strips the object layer out of the live stream on a keypress, so a licensed
  decoder can be watched dropping from Atmos to DD+ and back with the object layer's byte cost on
  screen. Plus a real BS.1770 loudness readout, a programme meter with PPM ballistics replacing a
  fixed display gain, and a soundfield-energy arrow computed from the encoded bed.

**Library, C API, Python and Rust**

- **A pimpl sweep across the exported surface**, so a private-state change is no longer an ABI
  break for anyone linking the shared libraries. [Library overview](docs/library/index.md) records
  the one deliberate exception and how it is meant to grow.
- **An E-AC-3 encoder in the C API and in Python**, covering plain E-AC-3 and the wide
  dependent-substream layouts with the Annex E tools. See [C API](docs/library/c-api.md) and
  [Python API](docs/library/python-api.md) for what is deliberately not mirrored.
- **Stream scan, caller-buffer decode, and loudness/level/QC metering, all now in the C API.**
  `ac3forge_scan` reports what a stream actually contains — layout, every programme, the
  DVB/ATSC service fields a muxer's descriptors want — without decoding any audio.
  `ac3forge_decoder_decode_frame_into`/`ac3forge_eac3_decoder_decode_access_unit_into` decode
  into caller-owned buffers instead of allocating per call, for the realtime embedder this C
  surface exists for, and preserve the §3.7 transient pre-noise hold-back exactly (a held-back
  frame leaves the caller's spans untouched). `ac3forge_loudness_meter_t`/
  `ac3forge_level_meter_t`/`ac3forge_qc_preset`/`ac3forge_evaluate_qc_gate` mirror the library's
  BS.1770-5 loudness meter, level meter and named delivery-QC gates. See
  [C API](docs/library/c-api.md).
- **A first Rust binding over the C API**: `ac3forge-sys` (raw, `bindgen`-generated against the C
  header at build time) plus a safe `ac3forge` wrapper covering AC-3 and E-AC-3 encode/decode. The
  C API had never crossed a real FFI boundary before — building this found and fixed two real
  header defects (a missing `ac3forge_object_placement_init()`, undocumented pointer lifetimes on
  four decoded-audio accessors). See [Rust bindings](docs/library/rust-api.md).
- **A latency budget** exposed through every binding, and **a minimum-footprint decoder profile**
  (`AC3FORGE_MINIMAL_DECODER`) proven on a cross-compiled bare-metal target.
- **Zero-copy numpy encode/decode in Python**, plus caller-buffer decoding. Every `encode_frame`/
  `encode_access_unit` call accepts a 2-D `(n_channels, n_samples)` array as well as a sequence of
  1-D arrays, and reads directly out of whichever is passed when it is already contiguous
  `float32`; decoded `.channels`/`.object_audio` are read-only views onto the decoded object's own
  memory instead of a fresh copy on every access; `FrameDecoder.decode_frame_into`/
  `Eac3Decoder.decode_access_unit_into` write PCM into caller-supplied buffers for a realtime
  embedder or tight batch loop that wants to reuse them. See [Python API](docs/library/python-api.md)'s
  "Zero-copy numpy and buffer reuse".
- **pkg-config files** for every installed component (`ac3forge`, `ac3signing`, `matroska`,
  `mp4`, `mpegts`, `iamf`, `ac3iab`, `ac3adm`, `admbridge`, `ac3forge_c`), for a non-CMake
  consumer.
  **`ac3adm`/`ac3::admbridge` (the ADM/BW64 reader and its Atmos bridge) are now installable via
  `find_package(ac3forge)`**, shared-only, without re-exporting the third-party libbw64/libadm
  they embed. **A `capi` feature** for the vcpkg port and Conan recipe reaches `ac3::forge_c`
  through either package manager for the first time. See
  [Using ac3::forge](docs/library/index.md).
- **Stream scanning in Python.** `ac3.scan()`/`ac3.read_frame_header()` read an elementary
  stream's shape — channel layout, every programme, every access unit's byte range — without
  decoding any audio, plus timing helpers (`ac3.access_unit_timing`, `stream_duration_seconds`,
  and neighbours) for a muxer computing where to cut. See [Python API](docs/library/python-api.md)'s
  "Scanning a stream".
- **Research trace export, reachable from Python.** The encoder/decoder mirror trace — added for
  the in-repo self-check — now fills in from an ordinary decode too, and
  `ac3::verify::append_trace_csv`/`append_trace_json_lines` (`ac3.verify.trace_to_csv`/
  `trace_to_json_lines` in Python) turn it into one tidy row per (frame, substream, block, stream,
  kind, index, value): per-frame bap, exponent, the §7.2.2.5 masking curve and the composite SNR
  offset, ready for `pandas.read_csv`/`read_json` and `.to_parquet()` from there.
- **`FrameError` gained `describe()`**, matching every other error type. Python's `Ac3EncodeError`
  now carries a real message instead of just the failing enumerator's name.
- **A concrete API-freeze plan for v1.0** ([docs/library/api-stability.md](docs/library/api-stability.md),
  roadmap `AP1`): a Public/Internal/Diagnostic/Experimental tier for every header under `ac3/`, a
  SemVer/deprecation policy, a C config struct growth policy, and release criteria. The C API
  gained a compile-time version alongside its existing runtime-only `ac3forge_version()`:
  `AC3FORGE_C_VERSION_MAJOR`/`MINOR`/`PATCH`/`AC3FORGE_C_VERSION` in `ac3forge_c/ac3forge.h`.
  `SOVERSION` and an ABI-tagging inline namespace are deliberately deferred to the `v1.0.0` cut
  itself — see the page's own reasoning.

**Verification**

- **E-AC-3 gains the coverage AC-3 already had**: an encoder input-space fuzzer, a mirror
  self-check diffing the encoder's model against a real decode per block, and metadata-parser
  fuzzers for the EMDF, OAMD, JOC, signing and ADM paths with a CRC-repairing mutator.
- **Real programme material in the fixture corpus** — two 30 s CC0 speech and music fixtures
  beside the synthetic ones, versioned and hash-enforced — and **a perceptual column that carries
  real numbers in CI** rather than nulls.
- **Third-party decode interop gates** against committed Dolby Encoding Engine and FFmpeg streams,
  plus a nightly run over pinned FATE samples.
- **Published conformance vectors** ([usage](docs/conformance-vectors.md)) and **a threat model
  for untrusted input** ([threat model](docs/threat-model.md)), both shipped with every release.
- **New CI legs**: ThreadSanitizer over the audio layer, script linting, PR-time performance
  comparison, an advisory ABI diff against the last release, CodeQL over the Android app's Kotlin,
  container-command tests, a headless browser test of the WASM demo, and instrumented tests for
  the Android bridge's device-free paths.
- **A Windows ARM64 CI leg** on GitHub's hosted `windows-11-arm` runner, building and testing
  `ac3cli` on real ARM64 hardware and packaging a `win-arm64` release archive — CLI-only for now
  (no resolvable prebuilt Qt6 ARM64 kit yet) and experimental until proven green over real runs.
- **An object-reconstruction quality trend**, and listening-test apparatus (no session has been
  run yet).
- **The block-switch decision's cross-toolchain determinism is proven, not assumed.** The
  transient detector that decides `blksw` — which reshapes MDCT type, coupling/AHT eligibility
  and rematrix bands every block, on both encoders — is verified bit-identical across five
  independent compiler/architecture builds, and its decision is now pinned by tests at all six
  A/52 sample rates instead of just one.

**Release engineering**

- **The packaging manifests bump themselves after a release.** A new post-release job downloads
  the release's own source tarball and platform assets, computes the digests the vcpkg port, the
  Homebrew formula and cask, the winget manifest and the Conan recipe each need, cross-checks the
  ones that are real built packages against the release's own published `SHA512SUMS`, opens a PR
  bumping all four together, and pushes the Homebrew formula/cask straight to the live tap. This
  is what had gone stale two releases in a row before it existed. Testable without cutting a
  release: it is also directly runnable by hand in dry-run mode against any already-shipped tag.
- **GitHub Release notes are drawn from CHANGELOG.md**, not drafted from the commit list — the
  matching dated section becomes the release body directly, since that curation already happens
  in CHANGELOG.md as part of normal development.
- **`check_packaging_versions.sh` gained a latest-tag advisory**: a warning, not a failure, when
  a manifest does not yet match the most recent release.

**Tooling and packaging**

- **macOS release packages are now universal (arm64 + x86_64) binaries.** A new CI leg builds a
  real (not cross-compiled) x86_64 half on GitHub's native-Intel `macos-15-intel` runner, and a
  merge job `lipo`s it together with the existing Apple Silicon build into one `.dmg`. The
  Homebrew Cask no longer restricts itself to `arch: :arm64`.

### Changed

- **JOC defaults to the QMF domain** on both sides. Reconstructed object audio now lags the bed by
  576 samples rather than 256.
- **The fast inverse transform reaches enhanced coupling and JOC, and the FFT core is radix-4.**
  A 30-second 15-object decode drops from 6.5 s to under 3 s. Encoder output is byte-identical.
- **SIMD kernels are selected by CMake per architecture** rather than by `#ifdef`, with
  bit-identical output and no runtime dispatch.
- **Runtime AVX2 dispatch — and the three non-SIMD findings that outweighed it.** A second,
  AVX2-flagged kernel tier is now chosen per process by CPUID (`AC3FORGE_SIMD_TIER=auto|sse2|avx2`
  forces either way for testing), carrying 256-bit windowing and twiddle stages plus batched
  four-transform IMDCT/MDCT kernels. Output is unchanged: real encodes and decodes are
  byte-for-byte identical under `sse2` and `avx2`.
  Profiling by *source line* rather than by symbol then found three costs larger than every
  transform in the codec put together, all of them redundant work rather than missing
  vectorisation, and all with unchanged output:
  `FrameParameters::at()` re-walked an O(objects) offset list on **every** coefficient access,
  making a frame O(objects²) — a 12-object Atmos decode is now **1.82×** faster under
  `joc-domain=mdct` and **2.90×** under the default `joc-domain=qmf`;
  `aht_bin_gaq_bits` fully quantised six mantissas per candidate gain to read one integer width
  off each, where that width follows from a single predicate — **1.70×** on `eac3_51_auto`
  whole-frame encode;
  and §6.6.5's QMF mixing coefficient re-evaluated its shape/timeslot branches once per
  (subband, channel) instead of once per (object, timeslot) — **−8.6%** instructions on a
  12-object QMF-domain decode.
  FMA3 was measured (~1%, and it perturbs results) and declined, so `-ffp-contract=off` stays
  pinned. See [docs/building.md](docs/building.md)'s "Runtime AVX2 dispatch" and
  [docs/performance-trend.md](docs/performance-trend.md)'s "Profile by source line, not by symbol".
- **Floating-point contraction is pinned off project-wide**, and the timing benches run real
  programme material instead of a single tone.
- **The coverage gate covers `apps/cli` and `python/`**, not just `src/`, and the fuzz jobs are no
  longer `continue-on-error`.
- **Enhanced coupling and transient pre-noise are measured and documented but not automatic.**
  Enhanced coupling sounds better on real material at every point tried but is kept out of `auto`
  because FFmpeg misreads its syntax; transient pre-noise measures worse than leaving the audio
  alone at every rate, because block switching gets there first.
- **The repository moved to trunk-based development.** `develop` is retired and `main` is the
  single long-lived branch; topic branches are `feature/*` and `bugfix/*` only. This removes the
  promotion and sync-back pull requests entirely. Branch protection moved across with the same
  parameters. See [CONTRIBUTING.md](CONTRIBUTING.md) and
  [branch protection](.github/branch-protection.md). The trend pages still show two tracks so
  historical data stays visible; reworking them for a single track is separate follow-up work.
- **ROADMAP.md was rebuilt** for the post-0.9.0 state.
- **A pre-freeze naming sweep, source- and ABI-breaking.** JOC's namespace now matches its header
  path: `ac3::joc` is `ac3::oba::joc`. The S/PDIF burst packer's directory now matches its
  namespace, which was already correct: `ac3/sinks/iec61937.hpp` is `ac3/iec61937/iec61937.hpp` —
  `ac3::audio`'s `PassthroughSink`/`MonitorSink` are the library's actual `Sink` types, and this
  header was never one. `ac3::FrameEncoder`/`ac3::eac3::FrameEncoder` keep their shared name across
  namespaces on purpose; [Library overview](docs/library/index.md) now writes down the
  codec-vs-codec-blind namespace split that rule follows.
- Internal: `std::format`/`std::print` replaced with {fmt} throughout, since the NDK's libc++ has
  no usable `<format>`; the WASM demo plays the library's own downmix rather than a hand-rolled
  one.
- Internal: the macOS backend's loopback-capture gap is documented against Apple's real Core Audio
  process/system tap API (`AudioHardwareCreateProcessTap`/`CATapDescription`, macOS 14.2 — the
  in-tree comment previously cited 14.4) and now carries a pure, CI-verified OS-version capability
  check (`ac3::coreaudio::system_audio_tap_api_available()`) a future implementation should refuse
  on. Capture there is still input-only; the tap itself needs real Mac hardware to build and
  verify. See [macOS](docs/platforms/macos.md#per-application-capture-the-core-audio-process-tap).

### Fixed

**Codec correctness**

- **Five E-AC-3 decoder defects in syntax only a third-party encoder produces**, found by pointing
  the decoder at real Dolby Encoding Engine and FFmpeg streams: AHT flags gated wrongly, the
  coupling channel's own gain and offset fields not read at all, band-structure tables not carried
  across blocks, `first*` state tracked wrong, and a missing coupling-state reset.
- **Coupling and delta bit allocation**: the decoder never read `cpldeltbae`; AC-3 coupling
  desynchronised once membership went per channel; `deltbaie=0`'s "retain" meaning was not honoured
  once exponent sets could change mid-frame; `snroffststr 0x2` read the wrong fields; a coupled
  block skipped `cplfgaincod`/`cplfsnroffst` entirely.
- **A framing bug on real disc and broadcast content**: an AC-3 frame's `crc1` bytes were read as
  `strmtyp`/`substreamid`, merging unrelated frames into one access unit — 176 of 480 groups on one
  sample.
- **`dialnorm=auto` and `ac3cli loudness` mis-assigned channel weights** on any layout wider than
  stereo, feeding WAV-order channels to a coded-order meter, so LFE could receive the surround
  boost meant for a surround channel.
- **A coordinate's binade shift was computed through `std::log2`**, whose last-bit behaviour is not
  required to agree across compilers, at exactly the input class where the true result is an
  integer. Replaced with `std::ilogb`, which reads the exponent directly. Byte-identical on real
  material.
- **The QC dialog reported the wrong preset's verdict.** Its preset list was written when there
  were three presets and never updated when two more were inserted into the middle of the shared
  list, so the option labelled "Netflix" applied a different preset's gate under Netflix's name,
  and two presets were unreachable. The control now derives from the same list the selection
  indexes into.
- **`latency_samples()` ignored the syncframe length**, reporting the six-block figure for a short
  syncframe and so overstating a one-block frame by about 27 ms — to exactly the caller sizing
  buffers for low-latency use.

**Robustness**

- **`mp4::Reader` could index far past its input** on a fragmented box using the 64-bit largesize
  escape to declare a size near `UINT64_MAX`, wrapping the parse position behind the streaming
  reader's window.
- **`ac3::io::read_wav` could read past the end of its buffer** on a file whose header sits near
  EOF, plus seven more out-of-bounds and precondition bugs in bit allocation, ADM parsing and
  signing verification. Each has a reproducer under `fuzz/regressions/`.
- **`eac3-encode`/`eac3-sine` crashed instead of erroring** on a bitrate beyond what `frmsiz` can
  signal, and when `auto` chose AHT under a short syncframe.
- **Python's encoders segfaulted instead of raising** when given the wrong *number* of channel or
  object arrays; only the per-array length was checked, and the underlying guard is an `assert()`
  compiled out in release wheels.

**Tooling and packaging**

- **The committed WASM demo fallbacks had gone stale, and the two directories had drifted apart.**
  Every module committed under `docs/assets/` predated the bindings it serves: the decode demo's
  `ac3forge_decode.wasm` was 365 KB and the encode demo's own copy of that same module 372 KB —
  already inconsistent with each other — against the 615 KB a current build produces, and the
  encode module was 389 KB against 640 KB. So a local `mkdocs serve` (and the PR-time docs build)
  embedded much older modules than the checked-in pages expect. The live site was never affected:
  the docs deploy job rebuilds both demos fresh on every publish. All four copies are now taken
  from a single fresh build on the pinned Emscripten (6.0.6), so the two directories agree, and
  verified by running `apps/wasm/tests`' Playwright suite against the committed copies themselves
  rather than the build tree: the decode demo decodes the bundled Atmos-in-DD+ fixture with real
  moving object positions, and the encode demo encodes a known stereo tone, matches its QC verdict
  and round-trip decodes it.
- **Containers hardcoded 1536 samples per frame**, breaking timelines on short E-AC-3 syncframes;
  `atmos bed51` still advertised an object layer it deliberately did not encode.
- **The minimum-footprint image ceiling was stale**, measured before the QMF work landed on the
  branch it was taken from. Re-measured and re-based.
- **Several CI checks false-failed on outcomes they exist to report** rather than on real defects:
  the trend runner aborted on a leg whose infeasible tool variants are the point of the leg; two
  encoder-space fuzzers treated a documented loudness-gate refusal as a hard failure because it
  arrives on a different exit code; a matrix-coverage check compared two spellings of a
  parameterised token that could never match; and the E-AC-3 mirror self-check carried an
  assumption that went stale when a parallel branch gave the coupling channel a delta field.
- **A Python latency test asserted a figure the C++ side never agreed with**, failing the wheel
  workflow on two platforms. The C++ value was correct; the test had drifted.
- **Packaging manifests and the Homebrew tap** were two releases behind, and **several pages
  described shipped work as still pending** — both corrected.
- **`python/pyproject.toml`'s licence identifier drifted to `GPL-3.0-only`** while `vcpkg.json`,
  the Conan recipe, the Homebrew formula and the README's own grant language ("or (at your
  option) any later version") all agreed on `GPL-3.0-or-later` — corrected to match. The ABI
  gate's exported-symbol allowlist and shared-library-diff steps discovered libraries from a
  hardcoded list rather than the actual build output, which had silently left `libac3iab.so`
  uncovered by both since it landed; both now discover dynamically, and a statically-embedded
  third-party dependency (`libadm`, pulled in by the new `ac3adm` export above) was found leaking
  ~16,800 of its own template-instantiation symbols into `libac3adm.so`'s dynamic symbol table
  through this change, fixed with a linker `--exclude-libs` flag rather than shipped. Both the
  licence check and a vcpkg-feature/Conan-option/pkg-config completeness check are now part of
  `tools/checks/check_packaging_versions.sh`.
- **The Windows installer stopped silently degrading to a ZIP-only package.** `cpack`'s NSIS
  generator dropped itself whenever `makensis` was missing with no diagnostic anywhere, so the
  release shipped without an installer for several releases before anyone noticed. CI now
  installs `makensis` and fails the build if a real `.exe` doesn't come out of `cpack`; a local
  build without NSIS installed still falls back to ZIP-only, but now says so. The
  packaging-consistency check also now catches a winget manifest whose `InstallerType` doesn't
  match its own installer URL or nested-installer fields — the same class of drift a manual
  copy-forward release bump can introduce.
- **The ABI gate stopped failing on every pull request.** `abi-gate` compared HEAD against the
  last release tag, so it reported the whole release cycle's accumulated drift — 806 commits'
  worth by 2026-08-28 — on every PR, including ones that touched no source at all. It now
  compares against the PR's own merge base; the last release tag is still the comparison point
  on a push or a tag, where that view is the useful one. `abidiff` also runs under
  `tools/ci/abi-suppressions.ini`, which drops the libstdc++ template instantiations that are
  not part of any ABI this project controls — about 900 of the roughly 1050 entries the gate was
  emitting. Advisory pre-1.0 now means green: the job reports through a single `ABI_ENFORCE`
  switch rather than `continue-on-error: true`, which never made the check green in the first
  place, since GitHub still reports a continue-on-error job's own check run as `failure`. The
  exported-symbol allowlist, six symbols behind `main`, is back in sync.

**Browser (WASM)**

- **The encode demo's round-trip preview 404'd on the published docs site.** The page loaded its
  decode module as `../ac3forge_decode.js` even though the build copies that module in next to the
  page precisely so the directory is servable from anywhere; the parent-relative path only worked
  when the demo directory was the server root, and broke under the docs site's subdirectory embed.
  The Playwright harness now serves both demos from a subdirectory for every run, so the layout
  that failed is the layout that gets tested.

**Shield Atmos Demo (Android)**

- **The encode loop kept streaming to the receiver after the demo left the screen.** It stopped only
  in `onDestroy`, so pressing HOME left a cached process pushing E-AC-3 bursts into the AVR with no
  UI and nothing to stop it. Now stopped in `onStop`, without tearing down the stream for the app's
  own About screen. Both on-screen render loops likewise ran behind other windows.
- **"Waiting for receiver" cleared on a capability probe rather than on audio flowing**, so a failed
  sink open left a fully-drawn dashboard over permanent silence. Readiness now means the encode loop
  is confirmed running, with a distinct "starting" state in between, and the waiting screen reports
  what the HDMI route actually advertises — including whether it claims the Atmos (JOC) profile.
- **A native library load failure crashed instead of showing its own failure screen**, because a
  throwing static initializer marks the class erroneous and the later `NoClassDefFoundError` is not
  what the call sites caught.
- **A partial `AudioTrack` write duplicated bytes into the IEC 61937 stream**, since a short write
  was retried by resubmitting the whole burst. Now resumed from.
- **Precise placement was impossible**: a flat per-axis deadzone with no rescaling meant the
  smallest deflection anyone could hold was about a third of full travel. Now radial and rescaled,
  seeded from the device's own declared flat range. Right-stick height is resolved by probing which
  axis the device declares rather than assuming.
- **The status line now reports whole-frame occupancy**, not just `encode_frame()`. The previously
  quoted figure excluded synthesis, the limiter, both meters, signing, stripping, the packer and the
  JNI submit — most of the frame.
- **The real-time encode thread no longer attaches to and detaches from the JVM once per frame**, and
  both worker threads now have explicit priorities instead of inheriting whatever started them.
- **`isDirectPlaybackSupported` was called unguarded on a minSdk-26 app**, so on any API 26–28
  device — a 2015/2017 Shield on Android 9, for instance — the app's most load-bearing platform
  query threw `NoSuchMethodError` rather than degrading. Guarded.

### Security

- **Build-time key material can no longer reach a published Shield APK.** The EMDF object-signing
  key asset is now deleted after the debug smoke build and before any release step, and the staged
  release APK is asserted to contain no `signing.key` entry before it can be uploaded. The check
  reads the APK's actual entry list rather than trusting step ordering, so a reordering or a Gradle
  asset-merge change fails the release instead of shipping the asset. Worth knowing because it
  changes the shipped artifact: a published release APK therefore carries no object-signing key,
  so it emits the 5.1 bed rather than a signed object stream and a receiver's Atmos indicator will
  not light where a previously published build lit it. Locally built debug APKs, which still have
  the key, are unaffected.

## [0.9.0-beta.1] - 2026-08-22

Ninth tagged release. The headline is the memory-usage optimization programme landing in full:
per-frame codec allocation churn down 54–88%, every CLI command and GUI recording streaming
instead of buffering, and a new memory trend that gates regressions the same way the timing
series always has — alongside a default-on fast inverse transform (4.5–4.7× faster decodes), a
whole-library per-component coverage gate, `ac3::signing` joining the installed/exported library
surface, and continued `apps/cli` command-group extraction.

### Added

- **Performance and reference transform modes.** The decoder's inverse transform joins the
  forward MDCT in having a fast path: §7.9.4 step 3 — the one O(N²) part of the normative
  inverse — now runs through the same radix-2 FFT core the fast forward fold uses, and after
  its evidence was reviewed (worst transform-level relative error 7.8e-14 against the direct
  form; 214.9 dB SNR agreement for AC-3 and 284.7 dB for E-AC-3 over 180 seconds of real 5.1
  material) it became the default: **decodes run 4.5–4.7× faster** (a 180-second decode drops
  from ~3.5 s to ~0.8 s), and the direct form's 320 KiB of tabulated matrices are no longer
  built at all on the default path. The pair is exposed as one intent-level switch:
  `mode=reference` runs every transform in a command on the spec's own direct evaluations —
  the forms the fast paths are validated against, for fixture regeneration or sample-for-sample
  comparison against an external decoder — and `mode=performance` (the default state) names the
  fast paths; `fast-mdct=off` / `fast-imdct=off` still adjust one half at a time. Encoded
  output never depends on the decode-side switch. See
  [Validation → Performance and reference modes](docs/verification.md#performance-and-reference-modes).
- **Span-output decode forms.** `FrameDecoder::decode_frame_into` and
  `Eac3Decoder::decode_access_unit_into` decode into caller-owned planar storage rather than
  allocating a fresh vector per call, with the same results as the value forms, pinned by
  lockstep equivalence tests. The E-AC-3 form keeps
  §3.7's transient-pre-noise hold-back semantics exactly: a held-back frame leaves the caller's
  spans untouched and is copied out at release.
- **Streaming I/O for unbounded sessions.** `ac3::io::WavStreamReader` (block-at-a-time WAV
  reading with the same parsing and sample conversion as the whole-file reader),
  `ac3::io::WavPcm16StreamWriter` (the incremental sibling of the one-shot PCM16 writer, for
  IEC 61937 carriers whose length isn't known up front), and `mpegts::Writer` (incremental
  transport-stream muxing whose output is byte-identical to `mpegts::mux()` — that equality is
  its contract and its test). Matroska already had its incremental `Writer`; MP4 deliberately
  does not get one — `moov`/`stco` need every frame's final offset, and `fragment()` (fMP4) is
  that format's streaming shape.
- **A memory trend beside the timing trends.** `ac3membench` counts heap allocations and
  allocator traffic per frame, live-byte drift and peak RSS across the encoder configurations
  *and* the decode paths the timing benches never covered; every `develop`/`main` push appends
  to the same `quality-history` series the CPU numbers use, rendered on
  [docs/performance-trend.md](docs/performance-trend.md) with the same trailing-baseline gates
  (either churn metric regressing flags the row) plus an absolute leak check that applies
  regardless of the trailing baseline.
- **`ac3::signing` is now an installed, exported library component** (repo-structure review D6),
  restructured into the same OBJECT+STATIC+SHARED shape `ac3::forge` itself uses
  (`ac3::signing_static`/`ac3::signing_shared`, `AC3SIGNING_EXPORT`-annotated) instead of a
  single internal-only `STATIC` target with no `install()` at all. `signing_static`/
  `signing_shared` each publicly link their own matching `forge_static`/`forge_shared`,
  preserving today's `PUBLIC ac3::forge` propagation; a real standalone
  `find_package(ac3forge CONFIG REQUIRED)` consumer linking `ac3::signing_static` now builds and
  runs across the installed-package boundary.

### Fixed

- **The encoder input-space fuzz no longer reports FFmpeg container-probe misses as encoder
  failures.** Case seed 1124127684685913171 (stereo at 512 kbit/s, 48 kHz) produced a fully
  valid stream — every syncframe on its exact 2048-byte boundary, both CRC words of every frame
  good, a clean strict decode under `-f ac3` — that FFmpeg 8.0's auto-detection nonetheless
  handed to its MPEG-PS demuxer: with frames that large, ffmpeg's AC-3 prober cannot clear its
  own accept threshold inside the 8 KiB probe window (it wants seven consecutive syncframes),
  while three start-code-shaped byte patterns inside ordinary quantized mantissas were enough
  for the MPEG-PS prober to win that window outright, and no amount of appended audio can win it
  back. `tools/ci/fuzz_encoder_space.py` now arbitrates any FFmpeg refusal by rerunning with
  `-f ac3` forced and every error check kept — a clean forced decode classifies the case as
  "misprobed" (counted and reported, never failing), a refused one still fails with the real
  decode error. The seed is recorded in the script's new `--regressions` replay list, which CI
  gates on before each unseeded search, and `fuzz/seeds/` gained a 512 kbit/s stereo stream so
  decoder-side fuzzing mutates from the big-frame corner too.
- **Installed packages now actually export `ac3::forge_c_static`/`ac3::forge_c_shared`**, matching
  what [docs/library/c-api.md](docs/library/c-api.md) and the in-tree `ALIAS` targets always
  documented. The raw CMake targets were previously `capi_static`/`capi_shared` under the `ac3::`
  namespace with no matching alias, so an installed package actually provided `ac3::capi_static` —
  a name nothing in the documented consumer surface used, and a `find_package(ac3forge)` consumer
  following the docs could not link the C API at all. Fixing the name surfaced a second, more
  serious bug: the C API's object library always privately links the static codec regardless of
  `BUILD_SHARED_LIBS` (a deliberate self-contained-ABI design), and
  `AC3FORGE_INSTALL_BOTH_LINKAGES=OFF` combined with a shared-only build used to leave that static
  target out of every export set, failing the configure step outright. That combination now
  configures, builds and installs cleanly.
- **The Conan and Winget packaging manifests are back on the real latest release** — both were
  still pinned to `0.8.0-beta.1` after `0.8.0-beta.2` shipped. `tools/checks/check_packaging_versions.sh`
  now runs in CI and fails the build if any packaging manifest's version drifts from the others
  again.
- **The hosted WASM decode demo (`docs/assets/wasm-decode-demo/`) matches the real one again** — it
  had silently fallen out of sync with `apps/wasm/`'s own copy (missing favicon links and the GPL
  footer). `docs.yml`'s docs build now byte-compares the two and fails if they drift apart again.
- **The Debian/Ubuntu package's homepage field is no longer empty** — `PROJECT_HOMEPAGE_URL` is now
  set on the root `project()` call, so `dpkg -s ac3forge` reports the real project URL instead of
  nothing.
- Fixed a stale anchor in `docs/platforms/raspberry-pi.md` pointing at a `linux.md` heading whose
  text no longer matches.
- Fixed `docs/library/index.md` and `docs/releasing.md`'s vcpkg port sections, which still blamed
  `ac3::forge_c`'s absence from the port on the installed-export-set bug fixed above — the port
  has always passed `-DAC3FORGE_BUILD_CAPI=OFF` regardless of that bug and continues to now that
  it's gone, as a deliberate scope decision pending a `capi` feature. Verified with a real
  `vcpkg install ac3forge --overlay-ports=packaging/vcpkg-port` that the port still installs no
  `ac3::forge_c` artifacts today.
- **A stack-overflow-risk PREfast finding (alert #77) is fixed**: `examples/atmos_objects.cpp`
  now heap-allocates its `Eac3Decoder` instead of stack-declaring it, the same fix already
  applied to `atmos_fallback.cpp` and `station_broadcast.cpp` for the identical scratch-state
  growth. Two duplicate false-positive `optional`-access findings (alerts #70/#71, in
  `apps/gui/qc_controller.cpp`'s and `apps/cli/main.cpp`'s `measure_qc`/`measure_eac3`) are
  documented and suppressed — a `have_first`/non-empty-stream guard already proves the meter
  optional is engaged before use, matching a pattern already fixed once elsewhere in `main.cpp`.
- **`misc-include-cleaner` findings that leaked back into `apps/cli/main.cpp` and
  `commands/analysis.cpp`** after the CLI command-group extraction (both predate that move and
  were never revisited for their own include lists) are fixed, keeping the `static-analysis` CI
  leg green.

### Changed

- **The CI coverage gate now measures the whole library, per component.** The `coverage` leg
  previously instrumented and gated the codec core (now `src/forge`) alone; it now instruments
  every library component —
  `ac3::forge`, `ac3::audio`, `ac3::signing`, the Matroska/MP4/MPEG-TS writers, the C API, and
  the opt-in ADM module plus its bridge — and gates statement (line) and branch coverage per
  component via the new `tools/checks/coverage_report.sh`, so a regression in a small module can no
  longer hide inside a blended number. `src/forge`'s own floor rose from 80%/70% line/branch to
  88%/78% to track the suite's growth, and the first whole-library measurement put honest floors
  under two thin spots — `src/audio`'s device I/O paths and the C API's E-AC-3 surface — rather
  than leaving them unmeasured. See the script's floor table for every component's numbers.
- **The C API's E-AC-3 surface is now tested, and its coverage floor raised to match.**
  `tests/test_capi.cpp` gained the E-AC-3 half it was missing: substream and access-unit round
  trips across the Annex E tool combinations, dependent-substream and dual mono metadata,
  transient pre-noise hold-back and flush, the decode/encode error mappings, and the NULL-handle
  defaults across the whole opaque-handle surface — all on real multi-frame audio. `src/capi`'s
  measurement moved from 48.4%/27.1% line/branch to 87.8%/79.2%, and its floor in
  `tools/checks/coverage_report.sh` from 42/22 to 82/72 per the table's own calibration rule.
- **Memory use no longer scales with how long a session runs.** The memory-usage optimization
  programme changed how every front end moves audio: the CLI's encode commands stream their
  input and their output (a 3-minute 5.1 encode peaked at 437.8 MiB before the programme and
  9.3 MiB after; decode 217 → 28.5 MiB; `spdif` — whose IEC 61937 payload runs at the 4×
  carrier rate — 225.7 → 18.0 MiB; an hour of `eac3-silence` 205 → 8.7 MiB), and every
  output-producing command holds keep-partial and error semantics exactly as before, verified
  byte-for-byte against pre-change binaries in every case. GUI recordings now stream to disk as
  they encode for the containers whose format permits it (elementary, Matroska, MPEG-TS, the
  IEC 61937 carrier), so a crash partway through a recording no longer loses the audio already
  captured. The WASM demo gained real memory ceilings and reports an out-of-memory error instead
  of the tab being killed.
- **The codec's own per-frame allocation churn is down 85–88 % on encode and 54–61 % on
  decode.** Working buffers that were freshly allocated every 32 ms frame — the exponent
  strategy plan, the coupling work set, the E-AC-3 encoder's whole per-(stream, block) MDCT
  spectrum set, the decoder's AHT and enhanced-coupling stores among them — are now owned,
  reused storage with an every-field reset discipline, bit-exact by construction and verified
  bit-exact in practice (AC-3 encode: 225,028 → 26,778 bytes and 286 → 86 allocations per
  frame on the measured runner). The E-AC-3 decoder's per-substream state moved from
  `std::map`s onto flat 32-slot arrays — the identity key space is exactly [0, 32) — for O(1)
  lookup and zero setup allocations. Every step is recorded on the new memory trend, which now
  gates regressions the same way the timing series always has.
- **`apps/` now holds every platform-facing target, and internal naming matches it.**
  `platform/{cli,gui,wasm,android}` moved to `apps/{cli,gui,wasm,android}`; `src/lib` (the codec
  core) is now `src/forge`; `src/adm_bridge` is now `src/admbridge`; and `ac3::audio`'s former
  three-way split (`ac3::platform`/`ac3::capture`/`ac3::sinks`) retired in favour of one
  consolidated `ac3::audio` namespace and header tree. None of this is installed/public surface
  except where called out separately below, so it only affects building from source, not an
  existing library consumer.
- **`apps/cli/main.cpp`'s single ~6,100-line file is being broken into one file per command group
  under `apps/cli/commands/`.** The shared parsing/I/O/metering support layer, the `src=`/`map=`
  multi-source subsystem, and the container-wrapping, audio-hardware, synthetic-signal-generator,
  Atmos, and real-material-encode command groups have moved out so far, each verified with a full
  rebuild and the whole test suite; `main.cpp` itself is down to 1,763 lines, with the decode and
  level/loudness/spdif command groups still to move. The command dispatch table
  (`kCommands`) — the thing that keeps an argv index from ever being silently wrong — is untouched
  throughout.
- **Build- and test-tree hygiene**: `scripts/` and `tools/` merged into one
  `tools/{checks,generators,references,ci}/` convention; the six top-level `requirements-*.{in,txt}`
  files moved into `requirements/`; `tests/` regrouped from ~53 flat files into subdirectories
  mirroring `src/forge/include/ac3/<namespace>/`'s own granularity, folding in a stalled
  platform/CRT axis split along the way; `CMakePresets.json`'s test and package presets
  deduplicated behind hidden base presets; the `examples/` target's separate output directory (and
  the DLL-copy machinery it required on Windows) removed by building examples alongside the shared
  libraries like every other target already does.
- **The installed CMake export set is now named `forgeTargets`, not `ac3forgeTargets`**, matching
  the bare-component-name convention every other export set here already uses (`matroskaTargets`,
  `mp4Targets`, `mpegtsTargets`, `capiTargets`) — it was the one export set named after the whole
  package instead of its own component. Anything referencing the old `ac3forgeTargets.cmake`
  filename directly (rather than going through `find_package(ac3forge)`, which needs no change)
  will need updating.
- **[CONTRIBUTING.md](CONTRIBUTING.md) now documents the repository's actual layout rule** — an
  `ac3/<name>/` header prefix means the component depends on `ac3::forge`, a bare `<name>/` prefix
  means it's deliberately codec-blind, and the C API is the one deliberate exception (depends on
  the codec, but isolated as a C surface) — plus the `apps/` vs `src/` split and the per-backend
  directory pattern. The docs site's nav also got a pass: the four data-trend pages now sit
  contiguously, `docs/project/history.md` moved to `docs/history.md` alongside its own nav
  siblings, and `apps/gui/icons/` gained a README marking it as generated output.
- **`static-analysis` now enforces correct header inclusion.** clang-tidy's
  `misc-include-cleaner` check joins the curated set the `static-analysis` CI leg gates: every
  symbol used in `src/forge`, `src/matroska`, and `apps/cli` must have its owning header
  `#include`d directly, not merely reachable through another header's transitive includes —
  closing the gap where a file built only because of what a sibling header happened to pull in,
  and would break the moment that sibling's own includes changed. The first run found 548
  pre-existing findings (538 missing includes, almost all standard-library facades — `<span>`,
  `<vector>`, `<expected>`, `<cstdint>`, and similar — plus a couple of `ac3::` types; 10 unused
  includes); all were fixed mechanically with `clang-tidy -fix` as part of this change and
  verified against a full rebuild plus a clean `ctest` run (615/615) before the check joined the
  enforced baseline. See `.clang-tidy`'s own header comment for the full rationale.

### Known gaps

- The macOS `ac3gui.app` is still not Apple-notarized or code-signed — unchanged from
  0.8.0-beta.2; this release signs artifacts with GPG and attests provenance via Sigstore/OIDC,
  neither of which satisfies Gatekeeper. Expect a "developer cannot be verified" prompt on first
  launch.
- Objects still will not decode as *objects* in Dolby's own decoder or hardware — unchanged from
  0.6.0-beta.1; `verify-objects` checks a stream against its own signature, not Dolby's gate.
- Exclusive-mode S/PDIF/HDMI passthrough has been confirmed against real bitstreaming hardware on
  ALSA only, via a Raspberry Pi 4B to a real Atmos-capable AVR over HDMI (see
  [docs/platforms/raspberry-pi.md](docs/platforms/raspberry-pi.md); this record corrected
  post-release once that validation's own docs were reconciled). WASAPI exclusive mode, PipeWire
  and CoreAudio remain unconfirmed against real bitstreaming hardware on any platform.
- `fscod2` audio content has no external decode oracle at all — verified only by this project's
  own encoder/decoder round trip.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified.

## [0.8.0-beta.2] - 2026-08-19

Eighth tagged release. `ac3gui` builds and packages on macOS for the first time — every
platform's release archive now carries a real GUI, not just Windows/Linux's — plus Python
bindings on PyPI and a C API over the encode/decode core.

### Added

- **Python bindings (`ac3forge` on PyPI)**, roadmap F2: a pybind11 module bound directly onto
  `ac3::FrameEncoder`, `ac3::FrameDecoder`, `ac3::Eac3Decoder` and `ac3::oba::AtmosEncoder` —
  numpy-friendly PCM, Python exceptions in place of `std::expected`. `.github/workflows/wheels.yml`
  builds wheels for Windows, macOS and Linux via `cibuildwheel`; publishing to PyPI itself is
  wired up but stays off until a maintainer provisions PyPI trusted publishing — see
  [docs/releasing.md](docs/releasing.md#publishing-to-pypi). See
  [docs/library/python-api.md](docs/library/python-api.md).
- **A C API over the encode/decode core** (roadmap F1), for consumers that can't link C++23
  directly.
- **`ac3gui` now builds, tests and packages on macOS.** The `macos-llvm` CI leg was CLI-only
  since it was promoted out of experimental; it now installs Homebrew's `qt` formula and builds
  the GUI the same opt-in way the four Linux legs do, `ac3gui_qmltests` and a headless
  `ac3gui --smoke` included, and this release's `ac3forge-0.8.0-Darwin.dmg` carries `ac3gui.app`
  for the first time. Getting there needed two real fixes for hangs under Qt's offscreen platform
  plugin, not just turning the option on — see
  [docs/platforms/macos.md](docs/platforms/macos.md#gui-on-macos).
- **A Homebrew Cask for `ac3gui`** is staged at `packaging/homebrew/Casks/ac3gui.rb`, alongside
  the existing CLI-only Formula — a Cask, not a Formula, being the right shape for a prebuilt
  `.app`. Not yet published to the `homebrew-ac3forge` tap; see
  [docs/releasing.md](docs/releasing.md#homebrew-formula-and-cask).

### Known gaps

- The macOS `ac3gui.app` is not Apple-notarized or code-signed — this release signs artifacts
  with GPG and attests provenance via Sigstore/OIDC, neither of which satisfies Gatekeeper.
  Expect a "developer cannot be verified" prompt on first launch.
- Objects still will not decode as *objects* in Dolby's own decoder or hardware — unchanged from
  0.6.0-beta.1; `verify-objects` checks a stream against its own signature, not Dolby's gate.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming
  hardware on any platform, ALSA, PipeWire or CoreAudio.
- `fscod2` audio content has no external decode oracle at all — verified only by this project's
  own encoder/decoder round trip.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified.

## [0.8.0-beta.1] - 2026-08-17

Seventh tagged release. The repository moved from `iainchesworth/ac3forge` to
`iainchesworthlabs/ac3forge`; this release cuts over to the new location and closes out
everything left stale by that move. No AC-3/E-AC-3/Atmos codec or CLI/GUI behavior changed.

### Added

- **CI can build on a self-hosted runner when one is actually online and idle**, per OS, falling
  back to GitHub-hosted otherwise — never as an all-or-nothing switch, and never for fork PRs,
  which always stay on GitHub-hosted regardless of runner availability. See
  [docs/ci-self-hosted-runners.md](docs/ci-self-hosted-runners.md) for the live-check and
  override design.

### Fixed

- **The published docs site was about to go stale at its own URL.** GitHub's repo-transfer
  redirect covers `github.com/<owner>/<repo>` paths (blob/tree/actions/releases), but the default
  GitHub Pages URL is owner-scoped with no such redirect — `iainchesworth.github.io/ac3forge`
  would 404 once this repo's `gh-pages` branch (now under `iainchesworthlabs`) next deployed.
  Docs now publish to and link from `iainchesworthlabs.github.io/ac3forge`.
- **Dependabot auto-merge silently stopped working after the transfer.**
  `dependabot-auto-merge.yml`'s repository guard hardcoded the pre-transfer
  `iainchesworth/ac3forge` slug; since `github.repository` now reports
  `iainchesworthlabs/ac3forge`, the job's `if` condition never matched, so no Dependabot PR
  auto-merged since the move.
- Roughly 40 hardcoded `iainchesworth/ac3forge` repo-path links across docs,
  README/ROADMAP/CONTRIBUTING/SECURITY, `mkdocs.yml`, and the vcpkg portfile updated to
  `iainchesworthlabs/ac3forge`. PR/issue references that predate the transfer
  (`docs/wasm-demo.md`'s `#168`/`#169` links) were deliberately left as-is — GitHub's redirect
  still serves them, and rewriting would misrepresent when they were filed.

### Known gaps

- Objects still will not decode as *objects* in Dolby's own decoder or hardware — unchanged from
  0.6.0-beta.1; `verify-objects` checks a stream against its own signature, not Dolby's gate.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming
  hardware on any platform, ALSA or PipeWire.
- `fscod2` audio content has no external decode oracle at all — verified only by this project's
  own encoder/decoder round trip.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified.

## [0.7.0-beta.1] - 2026-08-17

Sixth tagged release. The main change is an AC-3 quality push: three independent fixes to the
encoder's bit allocation — weighing delta segments against their own cost at every layout, raising
`dbpbcod` past the spec's own recommendation, and giving the LFE its own fine SNR offset instead
of a shared one — move the 5.1 landscape leg at 448 kbit/s from 2.98 dB behind FFmpeg 8.0.1 to
0.72 dB ahead of it, with perceptual quality unchanged. Finding and fixing those relied on new
verification infrastructure landing alongside them: a fuzz harness over the encoder's own input
space (as opposed to only the decoder's), an opt-in encoder/decoder mirror self-check, and a codec
matrix now driven by real programme material rather than synthetic tones — which is what caught a
stale coupling-channel delta cursor and a frame-ending mid-delta bug that had escaped every
existing gate. Also landing this release: a native PipeWire audio backend for Linux, a shared app
icon and About dialogs across every GUI surface, and an E-AC-3 `auto` tool set that picks
coupling/spectral extension/AHT from the per-channel bitrate instead of taking on/off flags as
given.

### Added

- **An opt-in AC-3 encoder/decoder mirror self-check (`ac3::verify`)**, which decodes every frame
  the encoder just emitted with this project's own decoder and diffs the decoder's model against
  the encoder's own — per-block bit offset, decoded exponents, bit allocation and delta correction.
  Motivated by a bug where `deltbaie == 0` was written to mean "no delta this block" instead of
  §5.4.3.47's "keep the previous block's" — the decoder kept a stale correction, mantissa fields
  were then sized differently on each side, and the failure surfaced two blocks later as an
  exponent walking outside 0..24, misdirecting the investigation into the wrong file entirely. The
  self-check catches that class of bug structurally, at the block where the two models first part
  company, rather than at whatever `§7.10.2` guard the misaligned bits happen to trip first. Off by
  default (`EncoderConfig::trace`/`DecoderConfig::trace` are null pointers, costing one branch per
  block and no allocation); `ac3::verify::MirrorEncoder` drives the encode-decode-compare loop for
  a caller that wants it. AC-3 (`FrameEncoder`/`FrameDecoder`) only for now — E-AC-3 computes its
  delta bit allocation once per frame rather than carrying it block to block, so it is not exposed
  to this specific bug class, and Annex E's dependent-substream/transient-pre-noise holdback
  machinery would need its own instrumentation design rather than reusing this one as-is.
- **A property/fuzz harness over the AC-3 encoder's own input space**
  (`tools/fuzz_encoder_space.py`). Every fuzzing target this project had mutates an
  already-encoded bitstream, which asks whether the *decoder* survives corrupt input; the codec
  matrix walks a hand-enumerated list of command lines against one bootstrap tone. Neither has
  any notion of option *combinations*, and neither varies the input material. This one draws
  random legal encoder configurations crossed with adversarial PCM whose character can change
  part-way through a frame — which is what drives exponent-run splits, block switching and the
  delta bit allocation — then holds every resulting stream against both this project's decoder
  and FFmpeg's strict decode. Motivated by the `deltbaie` defect below, which produced streams
  both decoders reject and escaped every existing gate; reverting that fix, the harness finds
  rejected streams within seconds. Runs bounded on every pull request (in the FFmpeg-oracle
  job) and deeper nightly, mirroring how `fuzz.yml` already splits short from nightly.
- **A new `auto` E-AC-3 tool set, which picks coupling/spectral extension/AHT from the
  per-channel bitrate** instead of taking the on/off flags as given. Every Annex E tool trades
  waveform fidelity for a band it can describe more cheaply than it can code, so each is a win
  below some rate and a loss above it — `auto` applies the measured crossovers (56 kbit/s per
  channel for spectral extension; `12 + 14n` for coupling, whose saving scales with how many
  channels share the band). It still honours an explicit `cpl:N`/`spx:N`/`aht:N` band-edge pin,
  so geometry stays steerable without taking over the decision.
- **A native PipeWire audio backend for Linux** (`src/audio/src/platform/pipewire/`,
  `AC3FORGE_WITH_PIPEWIRE`), selected via pkg-config when ALSA's headers are not present.
  Live capture and monitor playback are genuine `pw_stream` PCM; IEC 61937 bitstream passthrough
  negotiates PipeWire's own compressed-format API for real
  (`SPA_MEDIA_SUBTYPE_iec958`/`spa_format_audio_iec958_build()`/`PW_STREAM_FLAG_EXCLUSIVE`), but
  depends on the target node's `iec958Codecs` having been enabled by the session manager, which
  is outside this library's control — see `src/platform/pipewire/passthrough.cpp` and
  `docs/building.md`'s "Why ALSA still comes first" for the full account, including why ALSA
  keeps precedence over PipeWire when both are present.
- **A shared app icon and About dialogs across every GUI surface.** One procedurally-generated
  mark (`assets/icon/generate_icons.py`, Pillow-based, plus a matching hand-authored SVG) now
  backs `ac3gui`'s window/taskbar icon and packaged `.exe`/`.app` icon, Shield's launcher icon and
  Android-TV Leanback banner, and the WASM demo's favicon. `ac3gui` gained an About dialog and
  Shield an About screen (reached via the TV remote's Info button), both showing real build
  version/git provenance through the existing `ac3::version_details()`, alongside a GPLv3 notice
  and font attribution.

### Changed

- **The AC-3 encoder now gives the LFE its own fine SNR offset instead of copying the one every
  other channel gets.** The bitstream carries a separate `lfefsnroffst`, but this encoder wrote
  the shared value into it, which left the LFE a price-taker in a search it cannot influence: the
  offset search picks the one value at which the frame's *total* mantissa cost fits, and that
  total is set by channels of about 250 bins each. The LFE's 7 bins are rounding error in that
  sum, so its precision was decided entirely by channels 36 times its size — and it lost
  precision at the same rate as them despite costing a fraction as much to serve. Raising only
  its own field by 4 fine steps moves about 12 bits per frame at 448 kbit/s and leaves the
  frame's total mantissa cost unchanged. Measured on two materials (the 5.1 fixture and the
  synthesized full-band decorrelated 5.1) at 192/256/320/384/448/640 kbit/s: LFE SNR up at every
  point, by as much as 5.7 dB, overall SNR never lower, ViSQOL MOS flat.
- **The AC-3 encoder now weighs delta bit allocation against what it costs at every layout, not
  only when coupling is active.** A delta segment is 12 bits of side information taken from the
  same budget that would otherwise buy a higher composite SNR offset, so the encoder already
  re-ran its offset search with delta cleared and kept whichever pass came out higher — but only
  when a coupling channel existed, because that is where a failing test first exposed it. Nothing
  in that reasoning is about coupling, and the layouts that never couple were the ones paying
  most: 5.1 at 448 kbit/s was emitting about ten segments per block, 724 bits per frame, 5% of
  the whole frame. On the 5.1 reference this is worth 0.7 dB.
- **The AC-3 encoder raises `dbpbcod` from the §8.2.12 recommendation of 2 to 3.** `dbpbcod` sets
  the knee below which §7.2.2.5 lifts a band's excitation, so raising it steers bits away from
  bands holding almost no energy and towards the ones that do. Measured on three materials
  (the 5.1 and stereo fixtures and the synthesized full-band decorrelated 5.1) at 192/256/320/
  384/448/640 kbit/s, it improves SNR in every case — by 5.9 dB at 192 kbit/s on the 5.1
  reference, where there are fewest bits to misplace — with ViSQOL MOS flat or better throughout.
  The other four parameters are unchanged: `floorcod` turns out never to bind, and `fgaincod`,
  though worth more still at high rates, regresses at 192 kbit/s.
- Together with the LFE exponent fix below, these move the AC-3 5.1 landscape leg at 448 kbit/s
  from 36.02 dB to 39.71 dB — from 2.98 dB behind FFmpeg 8.0.1 to 0.72 dB ahead of it — with MOS
  unchanged at 3.67. The three are independent and were each measured separately: the delta cost
  check and `dbpbcod` account for 39.13 dB between them, and the LFE fix adds the remaining
  0.58 dB on top.
- **Coupling is now dropped, rather than moved down in frequency, when spectral extension leaves
  it no room.** §E3.3.1 derives the coupling end frequency from `spxbegf`; when that landed below
  the requested `cplbegf` the encoder used to slide `cplbegf` down to meet it, which silently
  coupled from 8.0 kHz where the rate model had asked for 10.2 kHz and made every coefficient
  above 8.0 kHz parametric. On the stereo reference at 192 kbit/s this was worth 6.8 dB of SNR
  (21.6 → 28.5 dB with all tools forced on).
- **The landscape comparison now reports `auto` rather than a forced `all`.** The headline number
  is meant to be what a real user of this encoder gets, the same standard applied to FFmpeg's and
  DEE's own automatic choices; `all` was a configuration this encoder would never itself choose.
  Against FFmpeg 8.0.1 the E-AC-3 stereo leg moves from −11.19 dB to −0.83 dB, and the 5.1 leg is
  unchanged at +0.49 dB.
- **The landscape page shows SNR, LSD and MOS side by side, each with its own vs-FFmpeg/vs-DEE
  delta.** These tools trade waveform fidelity for banded envelope fidelity deliberately, so a
  single-metric headline reported a working tool as a straight loss.
- **The quality landscape page (`docs/landscape.md`) now shows a spectrogram alongside its
  SNR/LSD/MOS numbers** — one stacked original/ac3forge/FFmpeg/DEE image per tracked leg,
  refreshed each release promotion, so there's a visual reference next to the trend numbers, not
  only figures.
- **The CI quality gate now includes an AC-3 5.1 leg.** It was stereo-only, which left the LFE and
  the full channel count with no absolute gate — two separate faults have now shipped through that
  hole. The floor is deliberately loose: the gate decodes with FFmpeg under `-xerror`, so a
  malformed frame fails it as a hard decode error, which is the failure mode both faults had.
- **A new `tools/check_ac3_allocation.py`** reports per-channel and per-band SNR against FFmpeg at
  a matched bitrate, to say *which* part of an allocation gap is worth chasing rather than only
  that one exists. It is what found the LFE fault below.
- **The AC-3 codec matrix (`scripts/run-codec-matrix.sh`) now sweeps real programme material,
  not only synthetic tones.** A stationary sine keeps near-identical exponents in every block, so
  a defect that only appears at a mid-frame exponent-run boundary — exactly the shape of the
  `deltbaie` bug below — was structurally unreachable at any bitrate or layout. The golden
  stereo/5.1 fixtures now run the full encode sweep too, decoded by both this project's decoder
  and FFmpeg's strict decode.

### Fixed

- **AC-3 encoder: a delta bit allocation that ended part-way through a frame produced an
  undecodable stream.** `deltbaie = 0` means "keep the previous block's delta bit allocation",
  not "no delta" (A/52 §5.4.3.47), so a channel whose exponent run stopped wanting a correction
  mid-frame was never told to drop it. The decoder kept applying the stale correction, its bit
  allocation diverged from the encoder's, and every field after that point was read at the wrong
  bit offset — a stream both this project's decoder and FFmpeg reject. Real material hit this at
  several bitrates, 64 and 96 kbit/s stereo among them. E-AC-3 was unaffected.
- **AC-3 encoder: the LFE sent one exponent set per frame however much its level moved.** A
  frame's exponents are the per-bin minimum across the blocks they cover, so a single set for six
  blocks is a set chosen by the loudest of them and every quieter block was then quantized
  against a scale meant for something louder. §5.4.3.15 makes `lfeexpstr` a single bit, and the
  encoder was reading that bit as though it could only ever say "reuse". On the 5.1 reference the
  LFE moves 10–16 dB inside one frame, which cost 12 dB of LFE channel SNR — 56% of the whole
  encode's noise power, on a channel carrying a third of its signal. Worth +0.3 to +3.8 dB
  overall across 192–640 kbit/s (+1.6 at 448), for 18 bits per refresh against a 14336-bit frame.
  Stereo is unaffected, having no LFE.

- **AC-3 coupling channel: delta bit allocation could push corrections past band 50, or land
  them somewhere the decoder never reads.** `choose_delta_segments()` and
  `compute_bit_allocation()` (`src/lib/src/core/bitalloc.cpp`) both started their §7.2.2.6 delta
  band cursor at band 0 regardless of which band a channel's own allocation starts at — harmless
  for fbw/LFE (start band 0), but the coupling channel starts higher, so a literal band-0 cursor
  either overshoot band 50 or wrote corrections into mask bands the coupling channel's own
  allocation never reads. Both FFmpeg and Dolby's own reference decoder require the cursor to
  start at the channel's own start band instead; this project's decoder shared the encoder's
  reading, so the round trip never noticed. Found by the encoder input-space fuzz harness above.

### Known gaps

- Objects still will not decode as *objects* in Dolby's own decoder or hardware — unchanged from
  0.6.0-beta.1; `verify-objects` checks a stream against its own signature, not Dolby's gate.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming
  hardware on any platform, ALSA or PipeWire — the new PipeWire path additionally depends on the
  target node's `iec958Codecs` having been enabled by the session manager, which is outside this
  library's control.
- `fscod2` audio content has no external decode oracle at all — verified only by this project's
  own encoder/decoder round trip.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified.

## [0.6.0-beta.1] - 2026-08-17

Fifth tagged release. The main change is Atmos object *decode*: earlier releases could only
encode object audio, and decoding an Atmos file just played its 5.1 bed. The E-AC-3 decoder now
reads OAMD object positions and reconstructs JOC object audio, surfaced through the CLI's
`decode`/`monitor` commands, a new GUI object inspector, and a browser-based WASM demo that
renders real decoded object motion and solos individual object audio. A companion
`verify-objects` mode checks a stream's own EMDF authenticity tag (not Dolby's proprietary
decoder gate — see Known gaps).

Also landing this release: a standalone BW64/RF64 + Audio Definition Model (ADM) reader that
drives a real professional ADM BWF master straight through to a DD+ JOC E-AC-3 stream; two new
container writers — MP4/ISOBMFF (with fragmented MP4/CMAF segmenting plus HLS/DASH signaling)
and MPEG-2 Transport Stream — alongside the existing Matroska writer; full ITU-R BS.1770/EBU
R128 loudness metering and a bitstream-aware delivery-QC command; Raspberry Pi (arm64 Linux) and
a real macOS CoreAudio backend; and the library is now installable through vcpkg.

### Atmos object decode

- **The E-AC-3 decoder reads OAMD object metadata and reconstructs JOC object audio**, closing
  the gap where only the encoder side supported objects. `ac3cli decode`/`monitor` surface the
  decoded object layer directly (including per-object WAV export via `objects_dir`).
- **A new GUI "Inspect objects…" dialog** plays back a decoded Atmos stream's object positions
  and lets you solo individual objects' audio.
- **A browser-based WASM demo** renders real decoded object motion and solo-plays real isolated
  object audio, entirely in-browser.
- **`ac3::signing` gained stream verification** (`verify_atmos_frame`/`verify_atmos_stream`, CLI
  `verify-objects`): checks a stream's own embedded EMDF authenticity tag. This is opt-in and
  separate from Dolby's own decoder gate — see Known gaps.
- **The E-AC-3 decoder now applies dynamic range control** (`drc=`/`heavy`), matching the legacy
  AC-3 decoder; previously accepted and silently ignored.

### ADM ingest

- **A standalone BW64/RF64 + Audio Definition Model parser** (`ac3adm::ac3adm`) reads a
  professional ADM BWF master's object graph into memory, and a bridging layer maps it onto the
  Atmos object encoder's input shape.
- **`ac3cli atmos-adm`** drives both together end to end: a real ADM BWF master straight to a
  DD+ JOC E-AC-3 stream. This module is opt-in (`-DAC3FORGE_BUILD_ADM=ON`, off by default) since
  it needs several Boost header libraries; see [docs/library/index.md](docs/library/index.md).

### Delivery containers

- **A standalone MP4/ISOBMFF container writer**, with a spec-correct `dec3`/`dac3` box, plus
  fragmented MP4/CMAF segmenting and HLS/DASH manifest signaling.
- **A standalone MPEG-2 Transport Stream container writer.**
- **Live capture sessions can mux straight to Matroska.** The GUI's Format tab and the CLI both
  gained the new container options.

### Loudness & delivery QC

- **Full ITU-R BS.1770-4/EBU R128 metering**: momentary and short-term loudness, loudness
  range, and true peak.
- **`dialnorm=auto` finished for multi-source assignments and dual-mono streams**, in both the
  CLI and GUI (dual-mono measures each channel independently).
- **A new CLI `qc` command and GUI QC dialog** audit an already-encoded stream's bitstream-level
  loudness against its embedded metadata and delivery gates.
- **A perceptual-quality (ViSQOL) column** sits alongside SNR in the quality-comparison tooling.

### Platform & packaging

- **Raspberry Pi (arm64 Linux)** is now a supported platform, Pi 4/5 tier (Pi 3 out of scope on
  real-time budget grounds).
- **A real macOS CoreAudio backend** for live capture/monitor playback.
- **The library is installable via vcpkg** (staged in-tree pending submission to the curated
  registry — see [docs/releasing.md](docs/releasing.md#vcpkg-port)): `ac3::forge` plus
  `matroska`/`mp4`/`mpegts` as opt-in container-writer features.

### Fixes

- **AC-3 decode's reported dynamic-range floor was wrong whenever the true minimum sample was
  exactly 0.0 dB** — an accumulator seeded at 0.0 instead of the first real sample silently
  widened the reported range.
- **A flushed E-AC-3 dependent substream (e.g. a height-only pair at end of stream) could crash
  the CLI decoder** instead of writing correct audio, when its channel layout didn't match the
  program's main substream.
- **`fast-mdct=off` is now honored consistently** across all `eac3-encode`/`eac3-encode-multi`
  commands.
- **Piping CLI output to `-` no longer risks corrupting stdout** when `dialnorm=auto` or a
  multi-source summary is printed.
- **The GUI's auto-monitor preference now actually takes effect** on the input rail's Add
  button.

### Known gaps

- Objects still will not decode as *objects* in Dolby's own decoder or hardware: DD+ JOC gates
  that on an authenticity tag keyed to a secret embedded in Dolby's decoder binaries, which this
  project ships no key for. `verify-objects` checks a stream against its *own* signature, not
  Dolby's gate. The bed still decodes as plain 5.1 anywhere.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming
  hardware on any platform.
- `fscod2` audio content has no external decode oracle at all — verified only by this project's
  own encoder/decoder round trip.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified.

## [0.5.0-beta.1] - 2026-08-15

Fourth tagged release. The main change is a fast-transform performance initiative: an opt-in
FFT-based MDCT was introduced, taken default-on, and then progressively hardware-optimized down
through every transform kernel the encoder touches — the long transform, both block-switched
short transforms, and the opt-in enhanced-coupling tool's DFT — alongside an algorithmic
warm-start for the bit-allocation rate-control search. Measured on the same 5950X release build
throughout, default 5.1 encoding drops from 0.4.0-beta.1's ~3.0 ms/frame to ~0.47 ms/frame (about
6.4×) and 8-object Atmos from ~4.8 ms/frame to ~0.43 ms/frame (about 11×), with SNR held at
+0.000 dB against an independent FFmpeg oracle at every step along the way. Alongside the
performance work: two GUI fixes (object-drag losing its mouse grab mid-gesture, and ambiguous
plan/elevation axis labeling), a quality-trend dashboard fix, and Linux packaging now ships real
`libFOO`/`libFOO-dev`-style system packages instead of one `.deb`/`.rpm` silently bundling the
CLI together with the entire library SDK.

### Performance: fast transforms, default-on and hardware-optimized

- **A new FFT-based fast forward MDCT**, landed opt-in behind `fast_mdct` (off by default): the
  §7.9.4 N/4-FFT structure replaces the direct §8.2.3.2 O(N²) evaluation for the long transform,
  ~25× faster at the kernel level (76.8 µs → 3.1 µs/call) with the direct form kept in-tree as
  the permanent reference/validation oracle. Verified bit-identical-class agreement (peak-relative
  ~3e-15) against the direct form on goldens, random data and real audio, plus **+0.000 dB**
  through an independent FFmpeg oracle at 192–448 kbps.
- **The inverse transform and enhanced coupling's windowing step got the equivalent fix**: `std::cos`/
  `std::sin` calls inside `imdct512_windowed`, `imdct256_pair_windowed` and `ecpl_channel_spectrum`'s
  windowing loop, previously recomputed fresh every call, are now one-time tables. Bit-exact by
  construction (the naive periodic-index shortcut is provably *not* bit-exact for the IMDCT's
  un-reduced angles — documented as a trap so it isn't re-attempted). A real 5.1 E-AC-3 decode
  drops from ~640 ms to ~145 ms (~4.4×).
- **The fast MDCT is now the default everywhere**, with `band_energy` (Atmos's JOC reconstruction
  solve) wired through the same flag — the gap that had capped Atmos's win at ~2.0×. Whole-frame:
  plain 5.1 3.0 → 0.67 ms/frame (~4.5×), 8-object Atmos 4.8 → 0.64 ms/frame (**~7.6×**, up from
  ~2.0× before `band_energy` rode the flag). `fast-mdct=off` (AC-3 commands) / `tools=nofastmdct`
  (E-AC-3) force the direct form back; the old opt-in spellings still parse as no-ops so existing
  run history keeps working.
- **The fast MDCT kernel itself closed to its standalone-prototype speed** (3.09 µs → 903 ns/call,
  a further 3.4×) by moving every angle-dependent value in the §7.9.4 fold — pre/post twiddles and
  the FFT's own butterfly twiddles/bit-reversal — into one-time tables, and switching the FFT to
  split real/imaginary arrays so the auto-vectorizer can see the butterfly's independent
  multiply-add chains.
- **Both block-switched short transforms get their own fast folds**, closing the last kernels still
  running direct-form O(N²) sums under the default `fast_mdct`. Each derives to the same scaled
  DCT-IV core the long transform already uses (877 ns/call vs. 35.8 µs direct — ~41×), removing the
  worst-case real-time hazard on transient-heavy material: a fully block-switched 5.1 frame's
  transform stage drops from ~1.3 ms-class to ~32 µs-class.
- **The opt-in enhanced-coupling tool's `dft512` gets the same FFT treatment** as the long MDCT
  (both now share one `fft_radix2.hpp` core): `ecpl_channel_spectrum`, still the single most
  expensive kernel measured, drops from 277 µs to 47 µs/call (~5.9×). Not run by any default
  encode, but a real-time hazard whenever `ecpl` is enabled.
- **The bit-allocation rate-control search now warm-starts from the previous frame's converged
  offset** instead of a fixed bracket, exploiting that consecutive frames of real material converge
  to the same or a neighbouring value. A stationary frame's ~11 full bit-allocation evaluations
  drop to 2–3; whole-frame time falls a further 18% (5.1) / 11% (Atmos) on top of the kernel work
  above. Brute-force verified against the plain binary search over 4,355 monotone-predicate cases
  with zero mismatches; outputs are byte-identical on every monotone path, and the one path where
  they can legitimately differ (AHT's locally non-monotone cost function) was already
  probe-order-dependent before this change — decoded PCM agrees at 102–115 dB SNR per channel.
- **New performance observability**: Tracy zones across every previously
  unzoned encoder stage, a standalone `ac3kernelbench` micro-benchmark harness timing kernels in
  isolation against real audio, and a per-kernel trend history (non-gating, `::warning::`-only)
  alongside the existing whole-frame performance trend — see
  [docs/performance-trend.md](docs/performance-trend.md).

### Packaging

- **Linux `.deb`/`.rpm` now ship a real `libFOO`/`libFOO-dev` split** instead of one package
  silently bundling `ac3cli` together with the entire library SDK (headers, static archives, the
  CMake package config — confirmed against real `dpkg-deb -c` output, not assumed). `libac3forge0`
  carries just the versioned shared library a linked binary loads at runtime; `libac3forge-dev`/
  `ac3forge-devel` carries everything a builder needs, version-pinned to its exact matching
  `libac3forge0`. Installable with a plain `apt install`/`dnf install` rather than a manual archive
  download — see [docs/releasing.md](docs/releasing.md#what-gets-published). ZIP/TGZ downloads are
  unaffected: `library`+`libruntime` still merge into one `ac3forge-dev-*` archive, exactly as
  before.

### GUI fixes

- **Object-drag no longer loses the mouse grab mid-gesture.** The Objects tab's plan/elevation/
  live-session `MouseArea`s sit inside a `Flickable`-based `ScrollView`, which could steal the grab
  from a child `MouseArea` once movement looked flick-like — most reproducible on the elevation
  view's vertical drag, the same axis `Flickable` watches for scrolling. `preventStealing: true`
  on all five affected `MouseArea`s holds the grab for the whole gesture.
- **The plan and elevation views in the Objects tab are now labeled as what they are** — "(top-down)"
  / "(side-on)" headers, a one-line caption naming which screen axis maps to which room axis, and a
  corrected elevation hint ("drag: depth + height" rather than "drag for height", since the plan
  view's marker moves too during an elevation drag — correct behaviour, previously unexplained).

### Developer tooling

- **The quality-trend dashboard's table no longer conflates unrelated checks.** The chart already
  scoped rows by codec and `isPrimaryCheck`; the table below it rendered the raw, unfiltered record
  list, which let a steady ~25 dB interop fixture read as a crash relative to an unrelated ~68 dB
  series. The table now follows the same Codec scoping as the chart, with a `Check` column and a
  tooltipped `†` marker on non-primary checks.

### Known gaps

- Objects will not decode as *objects* in Dolby's own decoder: DD+ JOC gates that on an
  authenticity tag keyed to a secret embedded in Dolby's decoder binaries, which this project
  ships no key for, so its streams are unsigned unless an operator supplies one. The bed still
  decodes as plain 5.1 anywhere.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming hardware
  on either platform (no such endpoint was available during development).
- `fscod2` audio content has no external decode oracle at all, not even Dolby's own Reference
  Player — verified only by this project's own encoder/decoder round trip.
- The external-encoder landscape comparison's Dolby DEE leg silently drops the Ls channel on
  discrete 5.1 input — a limitation of the installed DEE build used as a comparison oracle, not of
  this project's own encoder; affected rows are marked `unverified` rather than scored.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified, and [docs/history.md](docs/history.md) for how this was built.

## [0.4.0-beta.1] - 2026-08-14

Third tagged release. The GUI is rebuilt to the canon design handoff — a numbered-rail workflow,
a single assignment table driving all channel routing, an audible timeline with per-source
offsets and motion editing, live capture (including two-device parallel capture with software
clock-drift correction), per-source gain/LFE/resample controls, dual-mono independent DRC,
S/PDIF-wrapped WAV output, four selectable colour palettes with a native system-accent theme, and
a full round of dark-mode fixes. Alongside the GUI work: enhanced coupling's encoder now fits
real angle/chaos coordinates instead of sending them as zero, the decoder accepts Annex E's
default coupling band structure, the EMDF object signer is a committed clean-room library, eight
new library examples ship, an external-encoder (FFmpeg/DEE) comparison joins the quality
dashboards, and Android release builds sign with a real keystore.

### E-AC-3 encoding and decoding

- **Enhanced coupling's encoder now fits real angle and chaos coordinates**, closing the last
  known gap from 0.3.0-beta.1's enhanced coupling work — it no longer sends angle/chaos as zero.
  Amplitude and angle are solved as an exact 2-variable linear least squares per band (§3.5.5.4's
  reconstruction is linear in the complex gain a band's amplitude/angle pair expresses); chaos is
  chosen by searching its 8 legal codes directly against the decoder's own deterministic
  de-correlation sequence and keeping whichever reconstructs closest to the source, rather than
  estimated from a statistical proxy. Quality on ordinary material is unchanged (a correlated
  signal's best fit lands near angle zero anyway); the case the amplitude-only fit could not
  represent at all — two channels' different content forced into the same narrow coupling band —
  improves measurably, from a ~3 dB floor to ~6 dB, without threatening the coding tool's own
  structural limit on how much a single coordinate per band can ever separate.
- **E-AC-3 stereo (2/0) rematrixing** — the bitstream syntax and decoder undo path have existed
  since 0.2.0-beta.1; only the encoder's own §7.5.3 minimum-power decision was missing, and it
  turned out to need no new logic at all, just the same rule AC-3's own encoder already makes,
  over the same Table 7.25 bands (Annex E only changes how many of the four are active, not their
  boundaries or the rule itself).
- **The decoder now accepts Annex E's legal default coupling band structure (`cplbndstrce=0`)**
  instead of rejecting it with `DecodeError::kUnsupported`. This project's own encoder always
  transmits an explicit band structure, so the default path had only ever been exercised against
  the encoder's own output — decoding FFmpeg's E-AC-3, which legally chooses the default, failed
  immediately. The root cause was a stale assumption that Table E2.12's array needed
  relative-to-`cplbegf` indexing; cross-checked against FFmpeg's own `decode_band_structure()`,
  the table is indexed absolutely from `cplbegf == 0`. A permanent regression fixture (a real
  FFmpeg 8.0.1 encode with nonzero `cplbegf`) now covers this in the gold-reference gate.

### Atmos object signing

- **The EMDF object signer is now a committed, clean-room library (`ac3::signing`)** rather than a
  gitignored overlay. The HMAC-SHA-256 construction and the layout of what gets signed are in-tree
  and dependency-free; the **key** is the only secret and is supplied by the operator at runtime,
  never embedded and never written to disk. `ac3cli atmos` gains `sign-objects` with
  `signing-key=<path>` (or the `AC3FORGE_SIGNING_KEY_FILE` / `AC3FORGE_SIGNING_KEY` env vars);
  signing engages only when both a request and a key are present. The Shield app reads its key from
  a bundled `signing.key` asset written from the `ATMOS_SIGNING_KEY` CI secret at build
  time. See [docs/concepts/object-signing.md](docs/concepts/object-signing.md).

### GUI: canon workbench redesign

- **The desktop GUI is rebuilt to the canon design handoff**, replacing the earlier workbench that
  had drifted from it — the numbered rail (01 Input / 02 Levels / 03 Soundfield), plan strip,
  two-tier bitrate picker, routing strip and command bar now match the handoff, landed via a
  6-agent conformance sweep against the mockup (~70 fixes across CLI parity, run history, timeline
  editing, live-tab truth and guided copy).
- **A single assignment table now drives channel routing everywhere**, replacing the free-text
  token field that only appeared once a second source was loaded. Each source channel gets one
  destination dropdown (bed position / a new object / programme / nothing); sending a channel to
  an object turns object mode on, fixes the 5.1 bed, and raises the rate to ≥384 kbps atomically.
  In object mode, a channel assigned to a bed position becomes a static object pinned at that
  speaker's azimuth; unassigned channels drop with a named warning, and encoding enforces the
  sixteen-object cap over dynamic + pinned together.
- **Meter and soundfield redraws no longer tear down and rebuild ~30x/second.** The 30 Hz level
  stream previously rebuilt fresh JS arrays (and every delegate) on every tick; meter/soundfield
  models are now layout-keyed and read by index, and encode-progress/object-drag updates are
  coalesced onto the ~30–60 Hz publish cadence instead of flooding the GUI event queue per frame.
- **A real first-run screen, Preferences dialog and honest run history** round out the shell: first
  run synthesizes a bundled 5.1 test signal into a real WAV; Preferences persists via `QSettings`;
  and run history, failure-banner actions, and the live tab now reflect actual encoder/session
  state rather than mockup placeholders.

### GUI: timeline & time model

- **Timeline length is now derived, not fixed** — `max(offset + duration)` over every loaded
  source, rather than a hardcoded 8 s.
- **Each source gets an independent start offset**, settable from a rail numeric field or by
  dragging its clip band, applied as leading silence in both the channel and object encode loops
  and the meter preview — and reproducible on the command line via a new `offset=` CLI token.
- **Keyframes stay programme-absolute when a clip is dragged**; Shift-drag explicitly carries a
  source's object keyframes along by the same delta (clamped at 0), so a plain drag no longer
  silently drags authored motion with it.
- **Zoom (wheel/button, up to 40x) and snap** — ruler-tick and drag-snap tiers at 1 s / 0.1 s / a
  32 ms floor — move together as the view scales.
- **The Preview button is now audible**: it renders every object through the Atmos encoder and
  plays the 5.1 bed back live through the monitor sink, paced in real time with the playhead
  following the audio clock.
- **Object identity is now keyed by (source, channel)** instead of position in the dynamic-object
  list, so reassigning a channel or removing a non-primary source no longer silently migrates or
  destroys motion belonging to a different or surviving channel.
- **`atmos-encode` gains an optional keyframes-file argument**, matching `atmos-path`'s grammar;
  the GUI's "Export paths…" writes that exact format, closing the last gap in object-mode CLI
  reproducibility.

### GUI: live session and two-device capture

- **A live take now streams to disk incrementally** instead of buffering the whole session in RAM:
  an elementary-stream take *is* the growing output file, muxed to Matroska once at a clean stop,
  so a crash still leaves the elementary take behind. An optional raw-WAV safety copy streams the
  untouched captured PCM alongside it.
- **A silence watchdog fails a session ~3 s after a capture device goes quiet**, instead of the
  transport reading "Running" forever against a vanished device, with a "Choose another device"
  recovery action on the resulting failure banner.
- **Live Atmos sessions pre-allocate a fixed object-slot budget** rather than baking the capture
  device's channel count straight into the JOC stream, so objects can be added or reassigned to a
  different capture channel mid-session.
- **Changing the receiver — or toggling passthrough — mid-session now hot-swaps the passthrough
  sink** on the worker thread between frames, without restarting capture or encode.
- **A live session can now pace a second capture device off the first's clock in software.** The
  master device's delivery paces the frame loop as before; the second device is conformed to the
  master's clock via a streaming linear-interpolation fractional resampler and a proportional
  drift-correction servo, since there's no shared hardware clock between two independent capture
  endpoints. Available from the GUI and from `ac3cli`'s new `live capture2=<index>` token, with
  the slave device's measured drift correction visible in the chain's capture cell. A plain
  channel-mode session's bed still comes from the master device alone — there is no principled
  default position to auto-pan a second, independent device's audio into.

### GUI: source gain, metering, and format/output controls

- **Per-assignment gain/trim** on the channel routing table, applied inside the same routing
  matrix that drives encode, meter preview, and fed-channel flags.
- **Source-side metering pips**: a whole-programme, pre-routing peak/RMS reading per loaded file
  source.
- **Resample-on-load**: adding a source at a different sample rate than the primary no longer
  refuses outright — it resamples to the primary's rate via an offline windowed-sinc polyphase
  resampler and labels the row accordingly; the refusal survives only when the primary's own rate
  has no legal AC-3 target at all.
- **LFE low-pass filtering**: a full-bandwidth channel explicitly routed onto LFE through the
  assignment table now runs through a 120 Hz 4th-order Butterworth low-pass in preview and
  channel-encode. Automatic single-source routing (a file's own dedicated LFE channel) stays
  bit-exact.
- **CLIP latches per channel** in the meters — once lit, stays lit until clicked or a new
  transport starts.
- **`objm` fold-to-mono**: the range grammar (`0.1-2:objm`) can now fold a contiguous run of one
  source's channels into a single dynamic object.
- **Dual-mono programmes get independent DRC.** A/52 §7.7.1/§7.7.2.2 give 1+1's two programmes
  independent DRC curves and heavy-compression ceilings, but the encoder was building the second
  programme's controller from the first's own config. CLI gains `drc2=`/`heavy2`/`ceiling2=`/
  `dialogue2=`; GUI gains a Programme 2 DRC combo and a "Heavy compression — programme 2" card.
- **A third container option: S/PDIF-wrapped WAV**, reusing the existing IEC 61937 burst-wrapping
  machinery. Works for both codecs — E-AC-3's carrier runs at 4x rate.
- **An advisory bit-rate floor for wide layouts**: a muted hint under Bit rate when the CBR rate
  works out to fewer than ~77 kbps per full-bandwidth coded channel. A hint, not a gate.
- **Guided now applies measured loudness and film-standard DRC automatically** while it's driving
  and Loudness/Metadata is untouched this session; dual mono gets the DRC-only half of the
  contract on both programmes, since loudness measurement is refused there.

### GUI: guided-mode workflow polish

- **Finished run chips now carry their own Play action**, sending that run's own output to a
  receiver — not whatever the most recent encode happened to produce.
- **Run history now survives a restart.** The last 30 completed runs persist to Settings as JSON;
  clicking a run chip opens a details popover with status, rate, duration, size, frames, failure
  text, and the `ac3cli` command line snapshotted when that run started.
- **Guided's amp destination now auto-picks a bitstream-capable output device** — the first device
  that can carry the prospective encode plan — with a "Choose a different device" override and a
  stated reason when nothing qualifies.
- **Guided's Movement step, once object mode is on, offers two cards**: *Everything moves* (every
  loaded channel becomes an object) and *Keep the bed, add movers* (only claims still-unassigned
  channels).
- **Good/Better/Best now maps to VBR quality, not a fixed bitrate**, when a VBR default or an
  already-selected Variable rate mode applies — Guided's Quality step rate cards set a VBR quality
  target (40/75/90) instead of a CBR number.
- **Preferences defaults apply on Save to untouched fields only**, generalising the existing
  loudness-touched contract to container/rate mode/bit rate/VBR quality.
- **The guided wizard's Back/Next footer no longer disappears off-screen.** It previously shared
  the tab `StackLayout`, whose implicit height is the max over every page — inheriting the Format
  tab's height let the footer stretch a full screen below the visible content. The wizard now owns
  its own surface outside the tab stack: the step bar and footer stay pinned, only the step content
  scrolls between them.
- **The always-on `ac3cli` command bar is now a popover.** Encode runs the encoder in-process, so
  the full command line is reference material, not the primary act: a compact chip opens a popover
  with the complete line, wrapped, with Copy.
- Fixed the runs lane's empty-state text riding the top edge instead of centring in the strip.

### CLI

- **Fixed: a bare `heavy2` token was silently misparsed** as `encode`/`eac3-encode`'s optional
  `in2.wav` positional instead of enabling Ch2 heavy compression — `run_main`'s bare-token
  classifier was missing it alongside `couple`/`heavy`/`mixmeta`/`sign-objects`/`keep-partial`.
- **`keep-partial` token**: a bare trailing-options token that keeps whatever frames
  `encode`/`eac3-encode`/`atmos-encode` already produced before a failure, at
  `<name>.partial.<ext>` — mirrors the GUI's own keep-partial-output preference.

### GUI: theming

- **Four selectable colour palettes, including a native system-accent theme.** *Signal* (the
  design system's red, default), *Ink* (cool greys, cobalt accent), and *Console* (warm greys,
  studio amber) join *System* — a new `SystemTheme` singleton that reads the platform's native
  accent colour and re-announces on OS colour-scheme changes, so changing the OS accent colour
  restyles the running app live. All four are selectable in Preferences → Appearance.
- **Dark mode is now hand-tuned per palette instead of a mechanical inversion of the light ramp.**
  The previous approach turned near-white accent tints into murky red-blacks and left the
  fully-saturated accent glaring against near-black; each palette now defines both modes by hand.

### GUI: dark-mode audit fixes

- **A round of dark-mode fixes found by auditing every tab across all four palettes.** Smoke-mode
  screenshot captures are now hermetic — session restore previously ran at window creation, so a
  screenshot inherited whatever session the last run saved, and closing the smoke binary could
  clobber the user's real saved session with smoke state. The Coding tools tab now explains itself
  instead of rendering a bare void when object mode or plain AC-3 hides its contents. The runs
  lane's hard-capped height had exposed a horizontal scrollbar overlaying the chips and eating
  their clicks — the scrollbar is now off, wheel/drag still pan. The Encode button's `.ac3`/`.ec3`
  suffix no longer goes stale after the codec moves the plan between containers.

### Quality & verification tooling

- **Added an external-encoder landscape comparison against FFmpeg and Dolby DEE**, giving the
  encoder a real point of reference beyond its own gold-reference gate. A new stereo fixture
  exercises coupling, enhanced coupling, spx, AHT, transient pre-noise, and rematrixing together; a
  local-only baseline tool encodes fixed legs through FFmpeg, DEE, and `ac3cli`, while CI itself
  runs a compute-only trend mode scoring against those legs using only this project's own decoder —
  no FFmpeg or DEE invocation at CI time. Results render in two new docs pages,
  `docs/tool-comparison-trend.md` (per-commit, per-variant detail) and `docs/landscape.md`
  (release-over-release headline table). This work directly surfaced the `cplbndstrce=0` decoder
  gap fixed above, and found that the installed DEE build silently drops the Ls channel on discrete
  5.1 input — the affected rows are honestly marked `"status": "unverified"` rather than reporting
  a fabricated score.
- **The gold-reference gate now checks a real Annex E tool-enabled stream (`tools=cpl`)**, not just
  the `tools=none` baseline, at the existing 55 dB SNR floor. `spx`/`aht`/`all` are deliberately
  left off this specific check: those tools are approximate/generative reconstruction where two
  independent spec-correct decoders legitimately diverge much further, so a 55 dB floor would
  false-fail on normal divergence rather than catch a real regression.
- **The quality trend chart and tool-comparison trend chart both gained a per-series breakdown
  view** ("Worst of legs, by branch" / "By platform leg", and "By branch" / "By variant"), so one
  CI leg — or one Annex E tool-set — quietly drifting relative to its siblings is visible as a
  trend line instead of only by scanning table rows.

### Android (Shield)

- **Android release builds now sign with a real release keystore instead of the debug key**, once
  a maintainer has provisioned the `ANDROID_KEYSTORE_*` secrets per
  [docs/releasing.md](docs/releasing.md). Local dev, ordinary CI, and any release run with no
  keystore provisioned all still degrade to the debug keystore exactly as before.

### Bug fixes

- **Windows audio backends no longer list a blank row in the device picker.** A real WASAPI
  endpoint that never fills in its friendly-name property was enumerated with an empty display
  string, and both the capture and passthrough front ends put that straight into a combo box as an
  unlabeled entry. The fix resolves a display name through a fallback chain (friendly name → device
  description → an endpoint-id-carrying stand-in), and an endpoint whose id can't be read is now
  skipped entirely rather than listed.

### Library examples & documentation

- **Eight new `examples/` programs**, each a build target and `ctest` entry like every other
  example: `wav_roundtrip` (real WAV file I/O, not just in-memory PCM), `custom_layout` (a
  channel selection no named `LayoutId` covers, via `Plan::custom_locations`),
  `multi_source_assignment` (combining separate sources via `ac3::plan::Assignment`),
  `scripted_object_motion` (authored `KeyframePath`/`OrbitPath` driving `AtmosEncoder`),
  `object_signing` (`ac3::signing::sign_atmos_stream`, previously undemonstrated),
  `level_metering` (`ac3::analysis::LevelMeter`/`energy_vector`), `decode_robustness`
  (recovering from one damaged frame in an otherwise-good stream via `ac3::split_frames`), and
  `atmos_fallback` (`AtmosConfig::emit_object_metadata`'s objects-or-nothing design decision,
  side by side). Three new library reference pages —
  [Channel plans & routing](docs/library/channel-plans-and-routing.md),
  [File I/O](docs/library/file-io.md) and [Object signing](docs/library/signing.md) — and new
  sections on the existing [Spatial & Atmos objects](docs/library/spatial-and-atmos.md),
  [Decoding](docs/library/decoding.md) and [Muxing & sinks](docs/library/muxing-and-sinks.md)
  pages are written from them.

### Known gaps

- Objects will not decode as *objects* in Dolby's own decoder: DD+ JOC gates that on an
  authenticity tag keyed to a secret embedded in Dolby's decoder binaries, which this project
  ships no key for, so its streams are unsigned unless an operator supplies one. The bed still
  decodes as plain 5.1 anywhere.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming hardware
  on either platform (no such endpoint was available during development).
- `fscod2` audio content has no external decode oracle at all, not even Dolby's own Reference
  Player — verified only by this project's own encoder/decoder round trip.
- The external-encoder landscape comparison's Dolby DEE leg silently drops the Ls channel on
  discrete 5.1 input — a limitation of the installed DEE build used as a comparison oracle, not of
  this project's own encoder; affected rows are marked `unverified` rather than scored.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified, and [docs/history.md](docs/history.md) for how this was built.

## [0.3.0-beta.1] - 2026-08-11

Second tagged release. Adds the two remaining Annex E coding tools (enhanced coupling,
transient pre-noise processing), a native Android app on NVIDIA Shield TV, packaged
`find_package(ac3forge)` libraries for third-party consumers, explicit multi-source channel
assignment, and a GUI tier split for first-time users through experts.

### E-AC-3 encoding and decoding

- **Enhanced coupling (§E3.5)** and **transient pre-noise processing (§3.7)**, the two Annex E
  tools the decoder previously recognised but refused (`DecodeError::kUnsupported`) — now
  implemented end to end, encoder and decoder, each behind its own tool token (`cpl+ecpl`,
  `tpn`). Enhanced coupling round-trips at the same ~20dB near-transparent bar as standard
  coupling for realistic content; transient pre-noise processing follows the spec's own
  time-scaling synthesis pseudocode, reusing the existing block-switch transient detector rather
  than a second one.
- Fixed two real conformance bugs found implementing the above: a missing §3.3.2 `nrematbd`
  formula for `ecplinu` (both encoder and decoder), and a systematic 2:1 gain error in enhanced
  coupling's FFT-based reconstruction pathway.
- `Eac3Decoder::decode_substream` now returns an optional decoded substream plus a new
  `flush()`, since transient pre-noise processing can hold a frame back until the next one
  confirms whether a correction reaches into it. Streams that never use the tool see no
  behavioural change.

### Dolby Atmos objects and multi-source encoding

- **Explicit multi-source channel assignment** alongside automatic routing — `ac3cli`'s encode
  commands take `src=`/`map=` to assign specific input files/channels to specific output
  channels and objects, instead of relying purely on automatic layout inference.
- Object mode now addresses objects by source, not a stale positional index, so multi-source
  sessions keep object identity stable as sources are added or reordered.

### GUI

- **Guided/Advanced/Expert tier split**: a real step-by-step wizard for first-time users, with
  Advanced and Expert tiers exposing the same controls power users had before.
- Multi-source input and an explicit per-channel assignment surface in the GUI, mirroring the
  CLI's `src=`/`map=`.
- **Dual mono (1+1) as a bed**, not a distinct layout — it now feeds the same object/motion
  pipeline as any other bed.
- **Variable bit rate** as a selectable GUI rate mode (a quality target with optional min/max
  kbps bounds), alongside CBR.
- Live sessions no longer clobber a file's authored objects when a live capture starts, and warn
  before silently dropping VBR settings that don't apply live.
- A Qt Quick Test harness drives the real `EncoderController` end-to-end, not a mock, for GUI
  regression coverage.

### Android (Shield) — new platform

- **ac3forge on NVIDIA Shield TV**: a native Android app (`platform/android/`) pairing
  `ac3::forge`/`ac3::audio` via JNI with a live Atmos demo — authored object trajectories,
  deflection, and ambient object motion, encoded and rendered on-device.
- HDMI receiver resilience hardening for the Shield demo, so a receiver renegotiating format
  mid-playback doesn't drop the session.
- Ships as a debug-signed `.apk` this release — see Known gaps.

### Library and packaging

- **`find_package(ac3forge)` support**: `ac3::forge` and `matroska::matroska` now build as
  proper static and shared CMake targets with `install()`/export support, so a third-party
  project can consume them without vendoring the source tree. `ac3::audio` (live capture/
  monitor/passthrough) stays CLI/GUI-internal, not part of what's installed.
- `ac3::forge` split into a platform-independent codec core plus `ac3::audio`, clearing the way
  for the library package above and for platforms — like Android — that only want the codec.

### Quality and packaging infrastructure

- Quality-trend dashboard redesign (readability, tightened gate thresholds) and a fix for CI
  concurrency dropping quality data mid-run.
- A round of security hardening prompted by OpenSSF Scorecard: hash-pinned CI tool installs,
  commit-SHA-pinned GitHub Actions (replacing tag-pinned ones), a `SECURITY.md`
  vulnerability-reporting policy, patched CVEs in docs dependencies, branch-protection scoring
  wired up, and build provenance republished as `.intoto.jsonl` for Scorecard to read.
- Several MSVC `/analyze` and clang-tidy findings fixed for real: heap-allocating large
  encoder/decoder objects out of worker-thread stacks, reusing MDCT scratch buffers instead of
  stack-declaring them per call, and a couple of static-analysis false-positive suppressions.
- macOS packaging now stays a single `.dmg` bundling both the runtime and library components,
  matching the archive packages' intent — CPack's DragNDrop generator defaulted to splitting
  per component the first time this leg actually ran on real macOS CI, caught by this release's
  own packaging dry run.

### Known gaps

- The Shield `.apk` ships debug-signed via Android's default debug keystore — no release
  keystore is provisioned in this repo yet, so it's a sideload-only build, not one suited for
  store distribution.
- Enhanced coupling's encoder always sends angle/chaos as zero (an amplitude-only fit) — quality
  degrades if two channels' content shares one narrow coupling band. Closed in
  [0.4.0-beta.1](#040-beta1---2026-08-14).
- Objects will not decode as *objects* in Dolby's own decoder: DD+ JOC gates that on an
  authenticity tag keyed to a secret embedded in Dolby's decoder binaries, which this project
  does not produce. The bed still decodes as plain 5.1 anywhere.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming
  hardware on either platform (no such endpoint was available during development).
- `fscod2` audio content has no external decode oracle at all, not even Dolby's own Reference
  Player — verified only by this project's own encoder/decoder round trip.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified, and [docs/history.md](docs/history.md) for how this was built.

## [0.2.0-beta.1] - 2026-08-10

First tagged release. ac3forge is a clean-room AC-3 and E-AC-3 encoder and decoder in C++23,
implemented from the published standards — no FFmpeg or other codec library is linked, only
used during development as an independent oracle to check output against.

### AC-3 and E-AC-3 encoding

- Every AC-3 coding mode (1+1 dual mono, 1/0 through 3/2, each with or without LFE) at 48,
  44.1 and 32 kHz, CBR only, across all 19 nominal Table 5.18 bit rates. Exact 44.1 kHz timing
  via Bresenham alternation between the two legal frame lengths.
- E-AC-3, all of the above plus 7.1, 5.1.2, 5.1.4 and 7.1.4 through dependent substreams, and
  either CBR or VBR (a quality target with optional min/max kbps bounds) per substream.
- Per-block, per-channel block switching (§8.2.2 transient detector, long 512-point vs. switched
  256-point transform pairs), automatic delta bit allocation (§7.2.2.6), and 2/0 rematrixing
  (§7.5.3) on AC-3.
- Channel coupling (§7.4 / §E3.3), spectral extension (§E3.6) and the adaptive hybrid transform
  with gain-adaptive quantization (§E3.4) on E-AC-3, each opt-in per stream.
- `fscod2`, Annex E's half sample rates (24, 22.05, 16 kHz).

### Dolby Atmos objects (Joint Object Coding)

- Mono sources placed and moved in 3D space, panned into a 5.1 bed with OAMD + JOC metadata
  carried in an EMDF container (ETSI TS 103 420) — playable as plain 5.1 by any decoder, and
  reconstructible as discrete objects by one that understands the container.
- Authored keyframe paths and closed-form orbits for object motion, both file-driven
  (`ac3cli atmos-path`) and live per-frame (`ac3cli live --atmos`).
- Syntax checked field-for-field against Dolby's own Reference Player and Dolby Media Encoder.

### Decoding

- A single in-repo decoder core shared with the encoder, reading both AC-3 and E-AC-3 —
  dependent substreams, `chanmap`, and the §E3.8.2 render — at float32-precision parity with
  FFmpeg on every layout FFmpeg itself can read.
- All three Annex E coding tools (coupling, spectral extension, AHT) decode individually or all
  stacked together, at every channel layout including 7.1.4 — the one combination FFmpeg cannot
  check at all, since its parser refuses a second dependent substream.
- Block switching and dual mono decode on both formats; decoded switch decisions are reported
  back (`DecodedFrame::blksw`), the same tier of diagnostic as `dynrng`.

### Metadata

- `dynrng` (five DRC profiles: film-standard, film-light, music-standard, music-light, speech),
  `compr` heavy compression, measured `dialnorm` (ITU-R BS.1770-4 gated loudness), and downmix
  levels (`cmixlev`/`surmixlev`, the E-AC-3 `mixmdate` group) — verified against FFmpeg applying
  the metadata, not just against the encoded bits.

### Live audio, capture and passthrough

- WASAPI (Windows) and ALSA (Linux) backends for live input/loopback capture, shared-mode
  monitor playback, and exclusive-mode S/PDIF (IEC 61937) bitstream passthrough — AC-3 and
  E-AC-3/Atmos alike.
- A lock-free SPSC ring carries samples from capture into the encoder; `ac3cli live` wires
  capture → encode → monitor/passthrough continuously.
- `MonitorSink` playback confirmed against real Windows hardware, including a live
  microphone-capture-to-monitor session; ALSA verified headless (WSL2 has no sound devices) plus
  under AddressSanitizer/UndefinedBehaviorSanitizer with leak detection.

### Tools and formats

- `ac3::io::scan`: derives stream format, access-unit boundaries and channel count directly from
  the bitstream.
- `matroska::matroska`: a standalone MKV muxer, independent of the codec library.
- `ac3::sinks::iec61937`: S/PDIF burst packing, byte-exact against FFmpeg's `spdif` muxer for
  AC-3 and independently verified against Microsoft's own IEC 61937 documentation for E-AC-3.
- `ac3::analysis`: peak/RMS/loudness metering with console ballistics and the Gerzon energy
  vector, shared by both front ends.
- `ac3cli`, a 21-command command-line front end, and `ac3gui`, a Qt Quick GUI with file and
  live-capture encoding, an object placement/motion view, and channel-level metering.

### Quality and packaging infrastructure

- CI across Windows (MSVC, clang-cl), Linux (GCC, Clang) and macOS (Homebrew LLVM) — CLI and GUI
  on Windows/Linux, CLI on macOS — plus a dedicated AddressSanitizer+UndefinedBehaviorSanitizer
  leg, clang-tidy static analysis, a coverage gate, a per-platform gold-reference quality gate,
  and an independent FFmpeg-validation leg.
- libFuzzer harnesses over every untrusted-input entry point (stream scanning, both decoders,
  WAV reading), run on every push and nightly with deeper mutation.
- Signed, attested release packages (Windows `.zip`/`.exe`, Linux `.tar.gz`/`.deb`/`.rpm`, macOS
  `.tar.gz`/`.dmg`) with SHA-512 checksums, keyless Sigstore/OIDC build provenance, and an SPDX
  SBOM — see [docs/releasing.md](docs/releasing.md).

### Known gaps

- Objects will not decode as *objects* in Dolby's own decoder: DD+ JOC gates that on an
  authenticity tag keyed to a secret embedded in Dolby's decoder binaries, which this project
  does not produce. The bed still decodes as plain 5.1 anywhere.
- Exclusive-mode S/PDIF/HDMI passthrough has not been confirmed against real bitstreaming
  hardware on either platform (no such endpoint was available during development).
- `fscod2` audio content has no external decode oracle at all, not even Dolby's own Reference
  Player — verified only by this project's own encoder/decoder round trip.

See [Validation](docs/verification.md) for the full account of what is and isn't independently
verified, and [docs/history.md](docs/history.md) for how this was built.
