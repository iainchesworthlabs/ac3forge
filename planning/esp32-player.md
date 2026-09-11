# The ESP32-S3 player: from two examples to a component and an ESPHome media player

!!! note "Status as of 2026-09-10: Phases 0 and 1 met on a board; Phase 2 owes only the DSP board"
    Written 2026-09-10 for a second `ESP32-S3-DevKitC-1-N16R8`, and run on it the same day - see
    [What the board showed](#what-the-board-showed). Phase 0: both examples measured on silicon,
    with the lines in their READMEs. Phase 1: `ac3forge::Player` and `ac3forge::Control` in the
    component, and its exit criterion met - the E-AC-3 demo over WiFi for ten minutes with no
    underruns, started by `POST /play` and read back by `GET /status`. Phase 2:
    `ac3forge::OutputLayout` and `LayoutRenderer` in the component, the player on the block form,
    32-bit slots, the slave role, a TDM sink and `PUT /layout`, four QEMU shapes in CI, and
    a 7.1.4 render from objects measured through the player on the board at 26 ms of every 32.
    What Phase 2 still owes needs the SigmaDSP board: the slave role against a real master, and
    TDM into a DAC. The board also found five things QEMU could not: the component did not
    compile the decoder's hot sources at `-O2`, the level meter cost twice the decode, ESP-IDF's
    I2S write sends the rest of a partly written DMA buffer as silence, every pass ended in a
    100 ms wait, and the network shape did not fit without PSRAM for the decoder. All five are
    fixed. Nothing from Phase 3 onward exists in the tree.

    Shape follows [the topology](topology.md) and [the appliance plan](player-appliance.md):
    design sections say what changes and why, phases carry exit criteria and how each is
    verified, [Decisions](#decisions) lists what is open with a recommendation and a cost, and
    [What cannot be verified](#what-cannot-be-verified-and-why) says where the evidence stops.

[The topology](topology.md) names the ESP32-S3 node as the sink role on a microcontroller, and
its Phase 5 is "the HLS client on the device". This page is the layer underneath any transport:
how bytes that have already arrived on the part become sound, and how that layer is packaged so
the two consumers that exist today, an ESP-IDF integrator and an ESPHome configuration, and the
one that comes later, the HLS client, all sit on the same code.

## What exists, by path

| Path | What it is | State |
|---|---|---|
| `esp-idf/ac3forge/` | The ESP-IDF component: a wrapper that `add_subdirectory()`s the repo root and links `ac3::forge_minimal`. No sources of its own. | Builds in CI under `espressif/idf:v6.1`; packs and verifies through `tools/packaging/pack_esp_component.py`. |
| `esp-idf/ac3forge/examples/i2s_player/` | Decodes a flash-resident AC-3 fixture to an I2S DAC and prints per-lap timing. | Measured on a board 2026-09-10: 9.9 ms of every 32 for AC-3 5.1 folded to stereo, paced at exactly 32 ms a frame. |
| `esp-idf/ac3forge/examples/stream_player/` | Bytes from a `partition`, `sd`, `fatfs` or `http` source through `ac3::io::AccessUnitAccumulator` to an `i2s`, `tdm`, `capture` or `null` sink. One loop, on the main task. | CI runs `partition` and `fatfs` under QEMU with the `capture` sink. Phase 0 runs `http` to `i2s` on a board. |
| `esphome/components/ac3forge/` | An ESPHome external component: a decoder and the framer, fed bytes by another component. | `esphome config` in CI. Never compiled into firmware by CI. |
| `docs/platforms/esp32.md` | The platform page. | Being restructured by PR #603; player documentation stays in the example READMEs until it lands. |

Two things about that table decide the shape of everything below.

**The decode loop has been written three times.** `stream_player.cpp`, `esphome/components/ac3forge/ac3forge.cpp` and the probe's `decode_eac3()` each drive the accumulator, call a decoder into caller-owned storage and hand the result on. Two of the three used `ac3::FrameDecoder`, which reads AC-3 alone: bsid above 8 returns `DecodeError::kUnsupported`, so neither the streaming example nor the ESPHome component could ever have played an E-AC-3 stream, and CI did not notice because its only sample is AC-3. `ac3::Eac3Decoder::decode_access_unit_into` takes the access units the accumulator produces, decodes Annex E, and accepts a plain AC-3 syncframe as one access unit of one substream. Phase 0 moves the example onto it; Phase 3 moves the ESPHome component.

**Nothing between the source and the decoder buffers.** The `http` source reads from the socket inside the decode loop. The I2S DMA queue holds 20 ms, less than the 32 ms one frame lasts, so from the moment playback is under way the loop has 20 ms to fetch and decode each frame before the DAC runs dry. The decode alone is 11 ms for 5.1 E-AC-3 at 240 MHz. What the network adds is what Phase 0 measures.

## What the board showed

Measured on 2026-09-10 on the second DevKitC-1 (an ESP32-S3 rev v0.2 with 8 MB of octal PSRAM and
16 MB of flash) at 240 MHz, with no DAC wired - the I2S peripheral clocks the audio out regardless,
so the pacing and the underrun counts are real. The READMEs carry the lines themselves.

- [`i2s_player`](../esp-idf/ac3forge/examples/i2s_player/README.md): AC-3 5.1 folded to stereo in
  9.9 ms of every 32, paced at exactly 32.000 ms a frame once its DMA descriptors divided the
  write. With the 240-frame descriptors it had, it paced at 35.000: ESP-IDF v6.1's
  `i2s_channel_write` abandons a partly written buffer whenever two sent ones are waiting, and
  the rest of it goes out as silence. The streaming example's sinks had the same exposure.
- [`stream_player`](../esp-idf/ac3forge/examples/stream_player/README.md#on-the-board), local:
  `partition` to `i2s`, 150 passes, 28.7 s of wall clock for 28.8 s of audio and no underruns. A
  7.1.4 render from objects takes 22.8 ms of decode and 3.2 ms of render in each 32 ms frame - the
  probe's `eac3_atmos_render` row is 25.1 ms for the same work - once the component compiled the
  decoder's hot sources at `-O2` (it had not: 28.4 ms without) and the level meter stopped
  squaring every sample in double (it had cost 60 ms a frame, twice the decode it measured).
- `stream_player` over WiFi: the E-AC-3 demo from a PC on the LAN plays with the host's levels and
  no underruns once the decoder's larger allocations are in PSRAM and the DMA queue is 64 ms. With
  the decoder in internal SRAM, as `sdkconfig.psram` had it, it does not fit beside WiFi:
  `abort()` 31 ms into playback, and a boot loop.
- `stream_player` for ten minutes over WiFi, started by `POST /play` and read back by
  `GET /status`: 18,750 access units, no underrun while it played, and the host's levels -
  Phase 1's exit criterion.

## The layers a player needs

Seven, and five of them exist. The order is the order bytes take.

1. **Framing.** `ac3::io::AccessUnitAccumulator` turns a byte stream into access units over a
   caller-owned buffer, allocating nothing. Exists.
2. **Decoding, both generations.** `ac3::Eac3Decoder::decode_access_unit_into`, into caller-owned
   planar storage. Exists. The integrations used the wrong class; see above.
3. **The output stage.** `ac3::OutputConfig`: the §7.8 fold and §7.7 operating mode, applied in the
   decoder's own storage before it returns. Exists.
4. **Sample format.** Planar float to interleaved 16-bit, or 24-in-32 with slot padding for TDM.
   Exists as [`ac3forge/interleave.hpp`](../esp-idf/ac3forge/include/ac3forge/interleave.hpp),
   moved on 2026-09-10 from inside the streaming example into the component, free of ESP-IDF, and
   tested on the host by `tests/io/test_interleave.cpp`. It is library code with a temporary home.
5. **Bytes to PCM.** The loop over 1 to 4: feed bytes, take frames, with hold-back (§3.7) and
   end-of-stream handled once. Written three times, as above. Library code with no home.
6. **Buffering and tasks.** A fetch task filling a ring buffer, a decode task draining it and
   writing to the sink, each pinned to a core: the fetch beside the WiFi and TCP/IP tasks on
   core 0, the decode alone on core 1. FreeRTOS-specific, so it cannot live in the library. Does
   not exist; the example's single loop is what stands in for it.
7. **Sinks.** Standard I2S at two slots; TDM at up to eight; a null sink for CI. Exist in the
   example. A SigmaDSP such as the ADAU1452 or ADAU1467 wants 32-bit slots and can be the clock
   master, which needs an I2S slave role; neither is configurable yet.

## Where each layer belongs

Three homes, and the rule for choosing is the one the repository already uses: platform-free code
in the library, platform-specific code in the platform's own directory, and an example shows how
to use both.

**The library, `src/forge`.** Layers 4 and 5. `ac3::io::interleave` is a move of code that already
has host tests. A `StreamDecoder` over the accumulator, the E-AC-3 decoder and the output stage,
with `feed()` and `next()` into caller-owned spans, is the loop written three times, written once.
Both are hand-over items for whoever owns `src/forge`, so this page describes them and does not
touch that tree. Until they land, the component carries copies, marked as
such, and the day they land is the day the copies are deleted.

**The component, `esp-idf/ac3forge/`.** Layer 6, and the seams for 7. The component today registers
no sources; it gains `include/ac3forge/player.hpp` and `src/player.cpp`, registered as component
sources beside the interface link it already has. Two abstract seams, mirroring the example's
`byte_source.hpp` and `audio_sink.hpp`, one pipeline:

```cpp
namespace ac3forge {

struct ByteSource {           // an HTTP body, an SD file, a partition, a UART
    virtual std::size_t read(std::span<std::byte> dst) = 0;   // 0 means end of stream
    virtual bool rewind() = 0;                                 // false if this source cannot
};

struct PcmSink {              // an I2S channel, a TDM channel, ESPHome's speaker::Speaker
    // One folded frame of planar float; the sink converts to its own slot
    // format (ac3forge/interleave.hpp). Blocks until taken: this paces the player.
    virtual void write(std::span<const std::span<const float>> channels) = 0;
};

struct PlayerConfig {
    std::size_t ring_bytes;          // between fetch and decode; seconds of stream at its bit rate
    std::size_t framing_bytes;       // the accumulator's buffer; ac3::io::kRecommendedBuffer
    int fetch_core, decode_core;     // 0 and 1
    unsigned fetch_priority, decode_priority;
    ac3::OutputConfig output;        // the fold and operating mode
    bool skip_object_reconstruction; // true for a stereo sink; the bed is the complete mix
};

class Player {                // owns the two tasks and the ring; reports what the sink saw
   public:
    Player(const PlayerConfig&, ByteSource&, PcmSink&);
    bool start();
    void stop();
    struct Stats { std::uint64_t frames, held, underruns, dry_us, decode_us, worst_frame_us; };
    Stats stats() const;
};

}  // namespace ac3forge
```

The sketch is a shape, not a signature freeze. What it fixes is the division of labour: the
player knows about tasks, cores, the ring and the decoder; the sink knows about a peripheral; the
source knows about a transport. The example's four sources and four sinks become implementations
of the two seams, and `stream_player.cpp` becomes the wiring of a configured pair into a
`Player`. **Built 2026-09-10** as `include/ac3forge/player.hpp` and `src/player.cpp`, with the
example's seams adapted rather than rewritten (a `SeamSource` and a `MeteredSink` over the
existing free functions) and the ring's size, placement and both cores in the example's Kconfig.
The Sendspin shape later needs an interleaved 16-bit entry on the I2S sink beside the planar one;
that is that sink's, not the seam's.

Registering sources changes how the component is consumed in one respect: it acquires
`REQUIRES freertos esp_timer`, which every IDF project has. The packing script stages the
component directory whole, so `include/` and `src/` travel with it.

**The examples.** Stay, and get smaller. `i2s_player` is left as it is: it decodes a fixture linked
into the image and is the measurement anyone can run with a board and a DAC, so its loop should
stay visible rather than move behind a class. `stream_player` becomes a consumer of the component
and the place its Kconfig lives: pins, DMA depth, slot width, role, source and sink choice.

## ESPHome

What the ESPHome side is, from its sources at `esphome/components/{speaker,audio,media_player}`
on the `dev` branch as of 2026-09-10, and what that decides.

**The pipeline ESPHome has.** `speaker` is an abstract `Speaker` with `play(const uint8_t*, size_t)`
returning bytes taken, and `i2s_audio` implements it over a ring buffer and a task. Its
`media_player` platform runs an `AudioPipeline` per purpose (media, announcement): a read task,
`audio::AudioReader` over `esp_http_client` into a `ring_buffer::RingBuffer`, and a decode task,
`audio::AudioDecoder` from that ring into the speaker. The decoder dispatches on
`audio::AudioFileType`, a closed enum of `NONE, WAV, MP3, FLAC, OPUS`, each behind a
`USE_AUDIO_*_SUPPORT` define that pulls a `micro-*` IDF component by git. The reader detects the
type from `Content-Type` (`audio/mpeg`, `audio/wav`, `audio/flac`, `audio/ogg` with `opus`) and
falls back to the URL's extension; a stream that matches neither fails to start with
`ESP_ERR_NOT_SUPPORTED`.

**How Home Assistant feeds it.** The device advertises `MediaPlayerSupportedFormat{format,
sample_rate, num_channels, purpose, sample_bytes}` in its traits. Home Assistant picks one by
purpose and builds a proxy URL ending in `.{format}`; its ffmpeg proxy runs
`ffmpeg -i <media> -f <format> [-ar rate] [-ac channels] [-sample_fmt s16]` and streams the pipe.
The format string is passed to ffmpeg unvalidated. ffmpeg has an `eac3` muxer and encoder, so a
device advertising `format: eac3, sample_rate: 48000, num_channels: 6` would receive every piece
of media in the house as E-AC-3 5.1, as a raw elementary stream, which is exactly what
`AccessUnitAccumulator` takes. `sample_bytes` must be omitted for it, as ESPHome already omits it
for MP3: `-sample_fmt s16` is not an option an E-AC-3 encoder accepts.

**So the wire is open and the device is closed.** The closed enum means an external component
cannot add a codec to ESPHome's own pipeline, and the reader refuses the stream before the
decoder would see it. Two routes:

- **(a) An `ac3forge` media player platform in the external component.** Its own pipeline, on the
  component's `Player` from the previous section: a `ByteSource` over `esp_http_client` (the
  streaming example's `http` source already is one), a `PcmSink` over any configured
  `speaker::Speaker`, and a `media_player::MediaPlayer` entity that advertises `eac3` and turns
  `control()` into `start`/`stop`. It reuses ESPHome's speaker and its entity model and copies
  nothing from its pipeline. Cost: a second pipeline in the configuration for anyone who also
  wants FLAC or MP3 on the same speaker, and a component that has to be compiled into firmware
  to be tested at all, which CI does not do today.
- **(b) Upstream.** Add `AudioFileType::EAC3` (and `AC3`) to `esphome/components/audio`, a
  `request_eac3_support()` that adds this repository's component by git the way `micro-flac` is
  added, a `decode_eac3_()` in `AudioDecoder` over the library's `StreamDecoder`, `audio/eac3` and
  `.eac3`/`.ec3` in the type detection, and `"EAC3": "eac3"` in the media player's format map.
  Then every `speaker` media player can advertise E-AC-3. ESPHome's C++ is GPLv3 and this library
  is GPL-3.0-or-later, so the licence question does not arise. Cost: a proposal to another
  project, on their timetable, with a decoder dependency of a size they may not want in the
  default build; and it needs (a) to exist first, as the working implementation the proposal is
  made from.

The recommendation is (a) now, written so that the bytes-to-PCM glue in it is exactly what (b)
would contribute, and (b) after (a) has played on a board. This is the same shape as the
topology's Sendspin decision: build the thing, then propose it.

**The plumbing component stays, and gets the decoder it should have had.** `Ac3ForgeComponent`
is fed bytes by another component and hands back planar float; it is the ESPHome face of layer 5,
and it moves onto `Eac3Decoder` in Phase 3 whatever happens to the media player. Its floor of
4,160 bytes for `buffer_size` stays right for AC-3 and is wrong for any stream with a dependent
substream, which the schema help text should say.

## Output layouts

A player is configured for the speakers it has, not for the stream it is sent. Three classes of
layout, in increasing cost, and the configuration names one of them:

| Layout | How it is made | State |
|---|---|---|
| **2.0** | The §7.8 fold of the bed, in the decoder (`DownmixTarget::kLoRo` or `kLtRt`). | Exists; what both examples play today. |
| **As coded: 5.1, 7.1** | The bed's channels as decoded, one TDM slot each. An Atmos bed is the complete mix, so objects need not be reconstructed. | The `tdm` sink exists and has never run on hardware. |
| **With height: 5.0.4, 5.1.4, 7.1.4, 9.2.4, …** | Objects reconstructed from the bed (`skip_object_reconstruction = false`), then each object panned onto the configured speaker set by `ac3::spatial::pan_direction` over two rings, horizontal and upper, with the bed's own channels placed at their nominal positions and the LFE sends summed. | **Exists on the target as of main's #611, in the probe**: the `eac3_atmos_render` row places a height-object stream onto 7.1.4 through the block form (`decode_access_unit_by_block`, one 256-sample block at a time), every level the host's, the render 5% of the row's instructions, 210,573 bytes of peak heap. `spatial.cpp` is in the profile. **Wired the same day** (Phase 2): a layout in `PlayerConfig`, the block-form decode, `LayoutRenderer`, and a TDM sink (at most four 32-bit slots on one S3 line, a board found on 2026-09-11); the QEMU shape `sdkconfig.ci-render` plays this row's stream through the player onto 7.1.4 at the row's own levels. |

The configuration takes a named layout (`5.1.4`) or a speaker list, each with an azimuth, an
elevation and a slot number, which is what `pan_ring` wants anyway; named layouts are the ITU-R
BS.2051 positions written out. The channel count decides the sink: two slots on standard I2S; on
one TDM line four of 32 bits or eight of 16, because an ESP32-S3 TDM frame holds 128 bits; twice
that across the S3's two I2S peripherals; or a DSP's TDM inputs. Rendering block by block, 256
samples at a time, keeps the output storage at 15 channels × 256 × 4 bytes rather than a frame's
92 KB, which matters on a part with 280 KB.

What is measured and what is not: the bed-only decode and the object reconstruction both have
figures from silicon, and so does a 7.1.4 programme carried as a bed with two dependent
substreams, which decodes at 0.90x real time at 240 MHz, real time with a tenth to spare. The
render through the player has board timing too, as of 2026-09-10: 3.2 ms a frame onto twelve
slots beside 22.8 ms of decode, 26 ms of the frame's 32 (the streaming example's README). The
block form matters
beyond rendering: a player decoding 256 samples at a time holds a block of PCM per channel
rather than a frame, which for twelve channels is 12 KB instead of 73 KB, and that is the
difference between a height layout fitting beside WiFi in internal SRAM or not.

## Control

The device is driven by commands, never fed decoded audio: the same shape [the appliance
plan](player-appliance.md#whole-house-audio-and-why-hearth-is-its-own-zone) settles on for
Hearth, with Home Assistant sending commands to an entity.

- **ESP-IDF.** A REST surface on `esp_http_server`, in the component beside the player so an
  integrator's firmware gets it by linking: `POST /play` with a URL, `POST /stop`,
  `POST /volume`, `GET /status` with the stream description, the decode timing and the sink's
  underrun counters, `GET`/`PUT /layout`. The streaming example mounts it. mDNS advertisement
  follows [the topology's Phase 4](topology.md#phase-4-discovery-and-more-than-one-sink) rather
  than being invented here.
- **ESPHome.** The `media_player` entity is the control surface Home Assistant already speaks:
  play a URL, stop, volume, mute, state. The sink counters and the decode timing become
  sensors, so a stalling network shows up on a dashboard rather than in a serial log. ESPHome's
  own `web_server` component gives the same entity a REST face when a configuration enables it,
  so nothing HTTP-shaped needs writing twice. The output layout is YAML on the component, as
  above, because a speaker set is a property of the installation and the entity should not have
  to carry it.

## The encode direction

The component builds the other profile too (`AC3FORGE_ESP_PROFILE=encoder`): the AC-3 and
E-AC-3 `FrameEncoder`s and the `AccessUnitEncoder`, in an image of 110,900 bytes of DIRAM with a
218,560-byte peak heap on this part, checked by the encode probe against the host's bytes for six
frames of synthesised 5.1. Never in the same image as the decoder: no two of decode, AC-3 encode
and E-AC-3 encode fit internal SRAM together, so one part is a source or a sink, and two boards
make the pair [the topology](topology.md) describes and has never had, our encoder to a network
to our decoder.

What the probe did not report until this page is time. Its lines were bytes, hashes and
allocations; Phase 0 adds `encode_us`, `us_per_frame` and `realtime_permille` per encoder, in the
form the decode side already prints, and the board answers whether a 5.1 E-AC-3 encode fits its
32 ms frame at 240 MHz. Everything below is conditional on that number.

**What the encoder profile already carries**, so the question "can it encode everything the
library can" has a precise answer. In: AC-3 at every layout from 1/0 to 3/2 with LFE and
coupling; E-AC-3 with every Annex E tool the `FrameConfig` exposes, which is channel coupling,
§E3.5 enhanced coupling, AHT, spectral extension, transient pre-noise processing, short frames
(`numblkscod`), VBR, DRC profiles and heavy compression; layouts beyond 5.1 through the
`AccessUnitEncoder`, an independent 5.1 substream with a dependent carrying the rear pair for 7.1
(`chanmap::k71Rear`, the shape the legacy-core test builds); and a second programme. The probe
exercises three of these shapes today (AC-3 5.1, E-AC-3 5.1, E-AC-3 2/0 with enhanced coupling)
and checks each against the host's bytes. Out: `AtmosEncoder` and everything under it, so no
object layer and no height channels. A "7.1.4" encode in this library's terms is a 5.1 bed with
the rears and the four heights carried as objects at fixed positions, JOC-coded; that is the
Atmos encoder, below. What a 7.1 access unit costs this part was measured on 2026-09-10 with a
fixture added to the encode probe (`-DAC3FORGE_PROBE_SEVEN_ONE=ON`): on the host the run's peak
heap goes from 223,020 bytes to 435,263, and under QEMU with the S3's memory map the fixture
dies on a 73,728-byte request, with 303,656 bytes free and a largest block of 241,664 before the
run began. **A 7.1 E-AC-3 encode does not fit this part's internal SRAM.** Two encoders at once
is the cost - the platform page now says the same of every dependent-substream layout, 601,954
bytes for 7.1.4 with three resident - and the question that remains is PSRAM, which the N16R8
has 8 MB of and QEMU cannot emulate; the fixture stays in the probe as an opt-in so the number
can be re-taken on a board with PSRAM enabled, or after the encoder core shrinks.

**Audio in.** The mirror of the sink seams: an I2S or TDM receive channel, the S3's I2S being
full duplex and a SigmaDSP's serial outputs carrying TDM8, filling caller-owned planar float one
frame at a time. A `PcmSource` beside `ByteSource` in the component.

**The encoder.** `ac3::eac3::FrameEncoder` for 5.1, `AccessUnitEncoder` for 7.1, and
`ac3::FrameEncoder` where the sink is S/PDIF: IEC 61937 carries AC-3 at 48 kHz but E-AC-3 only at
four times that, which optical receivers mostly do not accept. Both return a `std::vector` per
frame, there being no `encode_frame_into`, at 249 allocations a frame for E-AC-3, the same PF7
gap the probe already gates at 260. A source tolerates it; a fixed-storage form is a hand-over.

**Bitstream out.** Three sinks, in the order they are useful here: an HTTP server on
`esp_http_server`, one chunked `GET /stream.ec3` that a sink pulls from, so the second board's
`http` source is the consumer and the HLS origin of the topology's Phase 2 is the same bytes in
segments; an SD card; and S/PDIF for AC-3 through the I2S peripheral, which ESPHome's own I2S
speaker already does in its `spdif_mode`, so the technique is proven on this silicon.

**Atmos.** "Audio to Atmos" is `ac3::oba::AtmosEncoder`: a 5.1 bed and mono objects with
positions, JOC-coded into one E-AC-3 access unit. It is not in the minimum-footprint profile.
`src/forge/minimal.cmake`'s encoder list carries neither `oba/atmos.cpp` nor the QMF bank the
JOC solve estimates in, so it has never been built for Xtensa and there is no footprint or timing
for it. On the host the object layer adds 0.80 ms a frame for four objects over the bed's encode;
this part decodes about fifty times slower than a desktop core, so the object layer alone would
be of the order of a whole frame before the bed's encode is counted. It also needs an object
source: positions from the control surface, or a fixed scene. Worth attempting as a measurement,
which means adding those files to the encoder profile, a hand-over, and not something to promise
from here. The realistic first result is audio in, E-AC-3 5.1 out, in real time or a measured
statement of how far short it falls.

## Sendspin

[The topology](topology.md#the-transport-later-a-sendspin-extension) already names Sendspin as
the transport to approach after HLS. Two things have moved since it was written, both read from
the sources on 2026-09-10: the ESPHome component is merged and shipping (a `sendspin:` hub, a media
source that plays through the speaker media player, a group `media_player`, sensors), built on a
standalone Apache-2.0 C++ client, `sendspin-cpp`, published to the ESP component registry
(`sendspin/sendspin-cpp`, 0.7.2, ESP-IDF 5.1 and later); and Music Assistant carries the server
built in and enabled by default, sending 16-bit FLAC or Opus, stereo, with no multichannel and
no compressed passthrough. The Home Assistant this project is developed beside runs Music
Assistant, so a Sendspin server is already on the network a board would join.

**The protocol, as it bears on this page.** A player advertises `supported_formats`, a
priority-ordered list of `{codec, channels, sample_rate, bit_depth}` with `codec` one of `opus`,
`flac` or `pcm`, and a `buffer_capacity` in bytes of compressed audio; the server picks one and
says so in `stream/start`, with an optional `codec_header`. Audio arrives as binary chunks: one
type byte, an int64 microsecond timestamp on the server's clock saying when the chunk's first
sample plays, a `send_ahead`, then the encoded frame; a chunk is between 15 and 150 ms. Clocks
are aligned by a two-state Kalman filter fed by bursts of time messages, and a player must not
report itself available until that filter has converged. The codec list is closed: "Servers MUST
support all audio codecs: `opus`, `flac`, and `pcm`", and nothing wider than stereo is described.

**What `sendspin-cpp` does and leaves to the application.** Everything protocol-shaped: the
WebSocket server the Music Assistant connects to (over `esp_http_server` on ESP-IDF), the Noise
handshake, message dispatch, the time filter and its bursts, decoding, and synchronisation, which
it does in the PCM domain by inserting silence, dropping frames or interpolating. The application
implements one method, `PlayerRoleListener::on_audio_write(uint8_t*, size_t, timeout_ms)`, which
receives decoded interleaved 16-bit PCM from the library's sync task and blocks until the output
has taken it, and calls `notify_audio_played(frames, timestamp_us)` from its output path so the
library knows where the DAC is. The decoded-audio ring defaults to 1,000,000 bytes and the
library expects PSRAM for it. The decoders are FLAC, Opus and PCM, compiled in, with no interface
for a fourth.

### Where the Atmos would come from

The end state, as put on 2026-09-10: a listener's streaming service is the source, Music
Assistant fronts it as it does today, the bitstream reaches an E-AC-3-capable node unchanged
and is rendered into that room, while other rooms take the server's stereo. Read against Music
Assistant's own provider documentation, the services narrow:

- **Apple Music**: AAC at 256 kbit/s only. Lossless and Dolby Atmos are behind Apple's own
  encryption, which Music Assistant cannot open. Not a source for this.
- **Tidal**: FLAC to 24-bit 192 kHz. Tidal carries Atmos as E-AC-3 JOC and its provider does not
  mention it; whether the Atmos rendition is reachable through the API the provider uses is the
  question to answer first, and it is a provider change if so. The one plausible service source.
- **Amazon Music**: no Music Assistant provider.
- **Files**: E-AC-3 JOC the listener already holds, including what this project's own encoder
  makes. Works today over the HTTP source, and is the material every phase here is measured with.

So the source side of the end state is, in order of certainty: local files now; Tidal if its
Atmos rendition can be fetched; Apple Music not at all. The device side is the same whichever
answers, which is why it is built first.

### Sendspin as a source, on this player

On **ESPHome** there is nothing to build for Opus: the `sendspin` component plays through the
same speaker this page's media player would, and a configuration can carry both. What this
project adds there is the E-AC-3 media player of [Phase 3](#phase-3-esphome), beside it.

On **ESP-IDF**, Sendspin is a source unlike the others: it delivers PCM, already decoded and
already timed, and it pushes rather than being read. It does not go through `ByteSource` or the
decoder at all. It goes straight to the `PcmSink` of [Where each layer belongs](#where-each-layer-belongs),
whose `write(std::span<const std::int16_t>)` is `on_audio_write` in different clothes, and the
sink's I2S write path is where `notify_audio_played` is called from, which is also where the
underrun accounting already lives. So a `sendspin_player` shape of the streaming example is the
`sendspin-cpp` client, a `PlayerRoleListener` over the component's sink, an mDNS advertisement
(the library runs the server side of the WebSocket and leaves discovery to the application, as
ESPHome's component does it), and PSRAM on for the library's ring. It costs no decoder memory,
so a build carrying both it and the E-AC-3 decoder for the HTTP source is the first shape where
this page's PSRAM question ([decision 4](#decisions)) answers itself: the Sendspin ring in PSRAM,
the decoder's peak in internal SRAM as now.

### Atmos over Sendspin

Carrying E-AC-3 with JOC over Sendspin needs three changes in three places, and one thing stays
the same:

1. **The spec**: a fourth codec, `eac3`, in `supported_formats` and `stream/start`. One access
   unit per chunk fits the protocol as it stands: 32 ms sits inside the 15 to 150 ms bounds, the
   timestamp means what it means for Opus, `bit_depth` is ignored as it is for Opus, and
   `channels` says 6 or 8. The list is closed today, which is a sentence in a document, not a
   design constraint.
2. **The client**: `sendspin-cpp` needs a decoder interface where it now has three compiled-in
   decoders, so that a fourth can be supplied by the application. With one, this project's
   decoder plugs in at the point the library already decodes a chunk, and the synchronisation
   after it is unchanged: the library syncs decoded PCM, and an E-AC-3 chunk decodes to 1,536
   frames of it exactly as an Opus chunk decodes to 960. The fold to stereo, or the render to a
   layout, is this player's, as in every other source.
3. **The server, which is two repositories.** The wire encoding is not Music Assistant's: it
   lives in `aiosendspin`, the Apache-2.0 Python protocol library Music Assistant's provider
   calls, whose `AudioCodec` enum is `OPUS`, `FLAC`, `PCM` and whose encoders are PyAV over the
   three (`OpusEncoder`, `FlacEncoder`, a `PcmPassthrough` that only chunks), selected by a
   `create_encoder(codec)` factory. A fourth codec there is an `Eac3Encoder` on the same pattern,
   and PyAV already carries ffmpeg's E-AC-3 encoder, so encoding a 5.1 programme to E-AC-3 is
   the smaller part. Music Assistant's provider then needs little: it reads the player's
   `supported_formats` into that enum, offers a `codec:rate:depth:channels` preference, and hands
   PCM from its streams controller to the library, at stereo 48 kHz today. Six channels through
   that controller, and passing an Atmos bitstream through it undecoded so the objects survive,
   are the Music Assistant changes proper, and the larger ask: the second is the
   passthrough-capable endpoint the topology describes, on the source side.

So the repositories touched, in dependency order: `Sendspin/spec` (a codec name) and
`Sendspin/aiosendspin` (the encoder) and `Sendspin/sendspin-cpp` (the decoder interface) have to
agree and can move together; `music-assistant/server` follows for six channels and passthrough;
ESPHome's `sendspin` component follows the library, advertising the codec and taking the decoder
from this project's component. This repository's own work is the decoder behind the interface,
which is the same decoder every other source here already uses.

What does not change is the sink side of this page. Every layer below the transport, the
decoder, the fold or render, the layouts, the I2S and TDM sinks and their instrumentation, is
the same code whether the access units arrived over HTTP, in HLS segments, or in Sendspin
chunks. That is the argument for building the HTTP and HLS paths first and the reason the
topology sequences the Sendspin conversation after them: the proposal is made with a device
that already decodes the codec in real time, and asks the protocol for a way to carry it.

The risk the topology records stands. A synchronised-audio protocol may decline a codec whose
decode latency it cannot see, and it may decline a fourth codec on principle. The
`source` role the spec defines is the other half of "a source that streams it": a Sendspin
source captures local audio for the server, in `opus`, `flac` or `pcm`, so the same extension
would let an encoder on this part feed Atmos into the house.

## What the sink hardware asks for

The examples were written against a MAX98357A and a PCM5102: 16-bit slots, the ESP32-S3 as I2S
master. A SigmaDSP input port, ADAU1452 or ADAU1467, differs in two ways, both configuration
rather than code:

- **Slot width.** SigmaDSP serial ports expect 64 fs bit clocks, 32-bit slots. `i2s_std` separates
  `slot_bit_width` from `data_bit_width`, so 16-bit data left-justified in 32-bit slots is one
  field. Worth making the default: every DAC that accepts 16-bit slots accepts 32-bit ones.
- **Who is master.** The DSP can generate BCLK and LRCLK from its own crystal, in which case the
  ESP32-S3 runs I2S as slave (`I2S_ROLE_SLAVE`) with the same three wires reversed in direction,
  and the DMA still paces the decode because it drains at the master's rate. Or the ESP32-S3 stays
  master and the DSP's ASRC absorbs the drift between the two clocks. Either works; the first has
  no rate conversion in the path.

Both become Kconfig choices on the `i2s` sink in Phase 2 and fields of `PcmSink` implementations
in the component. Neither can be verified without the DSP board; see below.

## Memory, and the question PSRAM raises

The probe keeps PSRAM off so the internal-SRAM budget is enforced on every build, and the
examples inherit that. A player with WiFi up is a different sum: the WiFi and TCP/IP stacks take
their share of the same internal heap the decoder's 210,203-byte peak needs, and the streaming
image's static data is already 163,546 bytes of 341,760 before either runs. Phase 0 reports
`heap_free` beside every timing line so this is a measurement rather than an argument. If the
internal heap does not hold both, the answer is PSRAM for the WiFi and LwIP buffers
(`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`) and the ring buffer, with the decoder's own allocations
kept internal: the probe's policy was about the decoder, and the decoder still fits.

## Phases

### Phase 0: the board

Both examples on a second `ESP32-S3-DevKitC-1-N16R8` over its native USB connector, at 240 MHz.
The streaming example moved onto `Eac3Decoder`, its I2S sink counting underruns against the DAC's
clock, its DMA depth in Kconfig. An E-AC-3 5.1 stream served over HTTP from the host.

**Exit:** each README carries a timing line from silicon; the streaming README carries
`realtime_permille`, the underrun count, the per-channel RMS against the host, and what the run
needed.

**Verified by:** the runs themselves, and the CI leg that still decodes the AC-3 sample under
QEMU through the changed player.

### Phase 1: the player in the component, and its control

`ac3forge::Player`, `ByteSource` and `PcmSink` in `esp-idf/ac3forge/`; the streaming example's
sources and sinks become implementations; `stream_player.cpp` becomes the wiring. The fetch task
on core 0, the decode task on core 1, the ring sized in seconds of stream. The REST surface from
[Control](#control) beside it, mounted by the example.

**Exit:** the same E-AC-3 stream over HTTP plays for ten minutes with zero underruns, started by
`POST /play` and read back by `GET /status`, and the example's own code is shorter than before.

**Verified by:** the board, with the sink's counters; CI's QEMU leg through the `null` sink and
the `partition` source, which exercises the tasks and the ring without a peripheral; the REST
handlers against a fake player on the host.

**The board's half, met 2026-09-10.** The E-AC-3 demo concatenated seventy-five times - ten
minutes - over WiFi from a PC on the LAN, started by `POST /play` (`202`) and read back by
`GET /status` every two minutes: 18,750 access units, 599.9 s of wall clock for 600.0 s of
audio, the host's levels to the digit, and no underrun while it played, with
`sdkconfig.psram` as it now stands. The streaming README has the lines.

### Phase 2: sinks and layouts

32-bit slots as the I2S default and the slave role as a Kconfig choice, carried by the
component's I2S `PcmSink`. The as-coded layouts on the TDM sink. The height layouts by moving
the player onto the block form and the render the probe's `eac3_atmos_render` row already
performs on this target - `decode_access_unit_by_block` into a 256-sample block per channel,
the objects panned by `ac3::spatial::pan_direction` onto the configured speaker set - with a
layout in `PlayerConfig` and a sink of up to sixteen 16-bit slots across the S3's two I2S
peripherals (one line holds four 32-bit slots) or a DSP's TDM inputs.

**Exit:** the slot layout checked by the `capture` sink on the host, as the TDM layout is today;
the slave role played against a SigmaDSP as master; a 7.1.4 stream decoded and rendered through
the player on the board with its per-frame cost recorded beside the probe's 0.90x for the
decode alone.

**Verified by:** `tests/io/test_interleave.cpp` for the layout and the probe's render row for the
levels; hardware for the role and the timing, which have no substitute.

**Built 2026-09-10, everything but what needs a board.** `include/ac3forge/layout.hpp` holds
`OutputLayout` - a name (`7.1.4`) or a speaker list (`L,R,C,LFE,Ls,Rs`, or angles), one speaker
per slot, sixteen at most - and `render.hpp` holds `LayoutRenderer`, which turns a `PcmBlock`
into one block per slot: unit gain to a slot whose location matches, `pan_direction` for one that
does not, the LFE to the LFE feeds, and, when the unit carries objects and the player asked for
them, the objects placed by their positions with the bed's LFE passed through and the bed's other
channels left out. `Player` moved onto `decode_access_unit_by_block` and `decode_frame_by_block`;
its `PlayerConfig` takes the layout, the stereo fold and an objects policy (auto reconstructs when
the layout has heights), and its `PcmSink` takes one block per slot. The coded layout the
renderer places is read from the unit's headers before the decode (the block form delivers
samples before it reports the layout) and confirmed against the decoded layout after; a
disagreement is counted in `layout_mismatches`, and none has been seen. The example's I2S sink
runs 32-bit slots by default and takes the slave role from Kconfig, the TDM sink carries up to
four 32-bit slots (an S3 TDM frame holds 128 bits) with its DMA descriptors sized from the bus
width, and `PUT /layout` changes the
layout for the next play. Host tests: `tests/io/test_layout.cpp`, eleven cases. QEMU: the
stereo, TDM and HTTP shapes unchanged to the digit, and a fourth, `sdkconfig.ci-render`, that
plays the probe's height-object fixture onto 7.1.4 through the twelve-slot TDM conversion with
every slot's RMS equal to `render_fixture.hpp`'s - one lap, as coded, MDCT-band domain, an 8 KB
ring: 31 KB of internal heap left after the reconstruction state is allocated, 22 KB of the
decode task's 32 KB stack used. The QMF domain a real stream needs is 233 KB more and waits for
the board's PSRAM, as does everything in the exit criterion that says "board".

**On the board, 2026-09-10.** The 7.1.4 render ran through the player at 22.8 ms of decode and
3.2 ms of render a frame, MDCT-band domain, through the `capture` sink - inside real time with
6 ms to spare, where the probe's `eac3_atmos_render` row takes 25.1 ms for the same work. The
QMF domain is not measured. The slave role and TDM into a DAC still wait for the SigmaDSP board.

### Phase 3: ESPHome

`Ac3ForgeComponent` onto `Eac3Decoder`; a `media_player` platform over `Player` and a configured
`speaker::Speaker`, advertising `eac3` at 48 kHz, six channels; the layout as YAML on the
component; the sink counters and decode timing as sensors.

**Exit:** a Home Assistant instance plays a local media file to the device as E-AC-3 through its
own ffmpeg proxy, and the device reports the same underrun counters as the example, on a
dashboard.

**Verified by:** `esphome config` in CI as today, over a configuration that includes the media
player; a firmware compile is [decision 6](#decisions); the end-to-end run needs a Home
Assistant instance and a board.

### Phase 4, conditional: upstream

Propose `AudioFileType::EAC3` to ESPHome with Phase 3 as the argument.

**Exit:** a pull request or issue opened, and the answer recorded here whichever way it goes.

**Verified by:** the thread. Not schedulable.

### Phase 5: the encode direction

Conditional on the encode probe's timing from Phase 0. A `PcmSource` over I2S or TDM receive,
`ac3::eac3::FrameEncoder` at 5.1, and an HTTP server serving the stream as it is made; AC-3 over
S/PDIF where a receiver is the sink. The Atmos encoder as a measurement first: added to the
encoder profile (a hand-over), its footprint and per-frame time recorded before anything is built
on it.

**Exit:** audio into one board comes out of another, or the host, as E-AC-3 5.1 with per-channel
levels matching a host encode of the same signal, and the encode's `realtime_permille` is
recorded beside the decode's.

**Verified by:** the encode probe on silicon for the timing; a loopback of two boards, or one
board and the host's decoder, for the levels; a host test of the source seam against a fake
receive channel.

### Phase 6: a Sendspin player shape

On ESP-IDF only; ESPHome has it. `sendspin-cpp` from the registry, a `PlayerRoleListener` over
the component's `PcmSink`, `notify_audio_played` from the sink's write path, an mDNS
advertisement, PSRAM on for the library's ring. Opus and FLAC from the Music Assistant already on
the network.

**Exit:** the board plays as a Sendspin player in a group with another Sendspin player, with the
sink's underrun counters at zero over ten minutes, and its measured offset from the other player
stated as a number rather than "sounds fine", which is [the topology's Phase 4](topology.md#phase-4-discovery-and-more-than-one-sink)
measured on this hardware.

**Verified by:** the board against the Music Assistant instance; the sink counters; a two-channel
capture of both players for the offset.

### Phase 7, conditional: E-AC-3 over Sendspin

The three changes in [Atmos over Sendspin](#atmos-over-sendspin), proposed with Phases 0 to 6 as
the argument: a decoder interface in `sendspin-cpp`, `eac3` in the spec, and a production or
passthrough path in Music Assistant. Not schedulable here; the exit is the answer, recorded.

### Hand-over to the decoder core

Six items for whoever owns `src/forge`; this page describes them and does not touch
that tree.

1. **A defect, found by the streaming example's CI shape on 2026-09-10.**
   `Eac3Decoder::decode_ac3_core` builds its inner `FrameDecoder` from the whole
   `DecoderConfig`, output stage included. Under a fold, a §E2.3.1.2 AC-3 core therefore
   reaches the §E3.8.2 assembly already folded to two channels, and the assembly's channel-count
   check refuses it as `kInvalidStream`. The header's promise that a plain AC-3 syncframe is one
   access unit of one substream holds only with `output` left at its default, which is what every
   existing test does. The fix is to construct the core's decoder from a copy of the config with
   `output` reset, since the assembled programme is folded once by `apply_output`; a test is a
   plain AC-3 stream through `decode_access_unit_into` with `kLoRo` set. Until it lands, the
   streaming example dispatches single-syncframe AC-3 units to `FrameDecoder` itself, and a legacy
   core with dependents under a fold still fails.
2. `ac3::io::interleave`, moved from the example with its host tests.
3. `ac3::io::StreamDecoder` over the accumulator, both decoders and the output stage, with
   `feed()` and `next()` into caller-owned spans, tested over both generations. The component's
   copy goes when it lands.
4. **A flush in the block form, found by [the stream set](esp32-stream-set.md#what-the-set-found)
   on 2026-09-11.** A stream using §3.7's transient pre-noise processing ends with its last
   access unit held back, and `Eac3Decoder::flush()` releases it only as raw per-substream
   results, not assembled and not through a `BlockSink`. The player decodes through
   `decode_access_unit_by_block`, so it cannot release that unit: a TPN stream's play ends one
   access unit short (15 of 16 for the set's `51-tpn.ec3`). A `flush_by_block(BlockSink)` that
   assembles what is held and delivers it as the unit's blocks would close it; a test is a TPN
   stream decoded block by block, whose samples then match `decode_access_unit` followed by
   `flush()`.
5. **The fold of a wide programme, found the same day.** Played to `2.0` in the emulated
   network shape, which has no PSRAM, a stream with a four-channel dependent substream - 7.1,
   5.1.4, 7.1.4 - runs out of internal RAM: `OutputStage::apply` (`output.cpp`, the fold's
   scratch) asks for 6,144 bytes with about 8 KB left and no block that large, where the same
   streams play as coded onto twelve slots with 147 KB to spare. 5.1 and 5.1.2 fold. On the
   board, with PSRAM, the fold fits - each play started with 169 KB of internal RAM free and a
   94 KB block - and what it costs is time. The 2.0 image's decode, which runs the output stage
   with the example's line-mode DRC and dialnorm, takes 4.5 ms more of a 32 ms frame than the
   as-coded image's for 5.1 and 8.6 ms more for 7.1.4, where the player's renderer places the
   same channels on twelve slots in 1.4 to 2.2 ms. So a 7.1.4 stream at `2.0` over WiFi decodes
   in 36 ms a frame and falls behind ([the stream set on a board](esp32-stream-set.md#on-a-board)).
   Folding the assembled programme with scratch sized to it once, or seat by seat without it,
   would close the memory. The time is not profiled: the fold's arithmetic is already float
   (`decode_scalar_t`), `Eac3Decoder::apply_output` times the stage as the `eac3_output` zone,
   and the probe, whose stage timers report it, folds 5.1 (`eac3_fold`) but no 7.1.4 stream. The
   tests are the peak heap of a 7.1.4 fold against a 5.1 one, and the board's time a frame for
   `714-walk.ec3` at `2.0`.
6. **The Annex E tools at 7.1.4, found the same day.** Decoded and rendered onto twelve slots
   over WiFi, a 32 ms frame of 7.1.4 takes 26.7 ms with no coding tools, 30.4 with TPN, 30.7
   with coupling, 31.1 with spectral extension, 35.2 with AHT, 39.1 with all of them and 59.2 with
   enhanced coupling. So a 7.1.4 stream from an encoder that uses AHT or enhanced coupling cannot
   play in real time on this part as the decoder stands, and the rest leave a sink 1 to 2 ms. The
   figures are the board run in [the stream set](esp32-stream-set.md#on-a-board).

## What cannot be verified, and why

- **WiFi.** Everything above the radio in the `http` source runs under QEMU over its emulated
  Ethernet, and CI runs it (`sdkconfig.ci-http`): 250 access units of the E-AC-3 demo fetched
  from the host and decoded with levels matching the host's to the digit, 2026-09-10. The radio
  and the timing have board runs as of the same day ([What the board showed](#what-the-board-showed));
  CI can hold neither, because neither exists under QEMU.
- **TDM on hardware.** No TDM DAC. The layout is tested on the host; the peripheral is not.
- **The I2S slave role.** Needs a bus master, which means the SigmaDSP board. Until it is
  connected, the role compiles and nothing more.
- **ESPHome firmware in CI.** The external component fetches the library by git reference, so a
  CI compile builds whatever the reference points at, not the code under review. A local path
  would fix it and ESPHome's `add_idf_component` does not take one. See decision 6.
- **Home Assistant end to end.** Needs an instance on the same network as the board. The format
  negotiation is read from Home Assistant's source rather than exercised until then.

## Decisions

1. **Where the player layer lives.** (a) **the component**, as sources it registers; (b) the
   library, behind a FreeRTOS abstraction; (c) the example, as it is. **Recommend (a).** The
   library is platform-free and stays so; the example is where an integrator copies from, and a
   player they have to copy is one they will get wrong. Cost: the component stops being a
   three-file wrapper, and the packing script's claim that everything real is in `src/forge`
   becomes "and in the component's own `src/`".

2. **The ESPHome route.** (a) **an `ac3forge` media player platform now, upstream after**; (b)
   upstream first; (c) the plumbing component only, no media player. **Recommend (a).** (b) has
   nothing to argue from until (a) exists; (c) leaves the component unable to do the one thing a
   user of it wants. Cost: a second pipeline for configurations that also want FLAC, until (b).

3. **The advertised format.** (a) **`eac3`, 48 kHz, six channels**, folded on the device; (b) two
   channels, folded by ffmpeg before encoding. **Recommend (a).** The bandwidth argument for
   carrying the codec at all is the 5.1 bed at 448 kbit/s against several megabits of PCM; a
   stereo E-AC-3 stream has nothing over stereo FLAC but size. Cost: the device does the fold,
   which it already does.

4. **PSRAM in the player builds.** (a) **off, until Phase 0's `heap_free` says otherwise**; (b) on
   for WiFi and LwIP from the start. **Recommend (a)**, because the answer is a measurement this
   PR makes. Cost: possibly one rebuild. **Answered by the board on 2026-09-10:** the network
   shape needs PSRAM for the decoder's larger allocations, not only for WiFi and lwIP, and a
   64 ms DMA queue besides - see `sdkconfig.psram`.

5. **Slot width default.** (a) **32-bit slots**; (b) 16-bit as now. **Recommend (a)**: a superset
   of what the two tested DACs accept, and what a DSP requires. Cost: the CI capture check gains a
   case. **Taken, (a), 2026-09-10**: `CONFIG_AC3FORGE_EXAMPLE_I2S_SLOT_BITS` defaults to 32, the
   stereo `capture` sink converts the way the `i2s` sink is configured to, and
   `CONFIG_AC3FORGE_EXAMPLE_I2S_SLAVE` is the role.

6. **A firmware compile in CI for the ESPHome component.** (a) **not yet: `esphome config` as
   today, and a manual compile recorded in the README when Phase 3 lands**; (b) a compile job
   that pins the library reference to the commit under test, which needs the commit pushed
   before the job runs; (c) vendor the library into the ESPHome component directory for CI only.
   **Recommend (a)** with (b) revisited when Phase 3 has something to compile. Cost: a component
   that can break silently between manual compiles.

7. **How a layout is configured.** (a) **named layouts plus an explicit speaker list**, each
   speaker an azimuth, an elevation and a slot; (b) named layouts only; (c) a speaker list only.
   **Recommend (a)**: names for the installations that have a standard name, the list for the
   ones that do not, and the list is what the panner consumes either way. Cost: two schemas that
   must agree, checked by expanding every name through the list form in a test. **Taken, (a),
   2026-09-10**: `ac3forge::OutputLayout` parses both from one string - `7.1.4`, or
   `L,R,C,LFE,Ls,Rs`, or `30/0,-30/0,lfe` - and a name is exactly the list of its Table E2.5
   locations, which `tests/io/test_layout.cpp` checks.

8. **The control surface.** (a) **REST in the component for ESP-IDF, the `media_player` entity
   for ESPHome, sensors for the counters**; (b) REST everywhere, including under ESPHome; (c) the
   entity only, no REST on ESP-IDF. **Recommend (a)**: each platform's users already have the
   surface named, and ESPHome's `web_server` gives the entity a REST face for free. Cost: two
   thin surfaces over one player rather than one, and a `/status` schema to keep stable.

9. **Which encoder first.** (a) **E-AC-3 5.1 over HTTP**, decoded by the second board; (b) AC-3
   over S/PDIF into a receiver. **Recommend (a)**: it is the pair the topology has never had, and
   the sink already exists. Cost: S/PDIF, the one output an ordinary receiver takes from this
   board, waits a phase.

10. **The Atmos encoder on this part.** (a) **attempt it as a measurement**, adding it to the
    encoder profile and recording footprint and time before deciding; (b) decline it, on the
    host-to-board scaling above. **Recommend (a)**, because the scaling is an estimate and the
    measurement is a day's work. Cost: a profile change in the decoder core's tree, and possibly
    a number that closes the question the way the ESP32-P4 was closed.

11. **Sendspin on ESP-IDF.** (a) **`sendspin-cpp` from the registry, over the component's
    sink**; (b) a client of our own; (c) ESPHome only, nothing on ESP-IDF. **Recommend (a)**: the
    library is what ESPHome ships, Apache-2.0, and already an IDF component; a client of our own
    re-implements Noise, the time filter and the sync for no gain, and (c) leaves an ESP-IDF
    integrator without the one transport their house already speaks. Cost: PSRAM on for that
    shape, and a dependency that is in technical preview and will move.

12. **Where E-AC-3 plugs into Sendspin.** (a) **propose a decoder interface upstream in
    `sendspin-cpp`, then supply ours through it**; (b) fork the library and add the decoder;
    (c) do not pursue. **Recommend (a)**: the interface is the smallest change that serves both
    projects, and a fork of a preview-stage library is a maintenance debt from the first day.
    Cost: the proposal's timetable is theirs, and the spec's codec list and the server's
    production path have to move with it.
