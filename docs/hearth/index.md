# Hearth

Hearth plays AC-3, E-AC-3, E-AC-3 with Atmos objects, and AC-4 through local speakers, an HDMI or
S-PDIF receiver, or synchronised network sinks. The desktop player, `hearth`, decodes a stream
for its local outputs, passes AC-3 and E-AC-3 to a receiver that accepts them, and plays a stream
to a group of network sinks over Sendspin, a protocol for synchronised network audio.
`hearth_sink` is the firmware for the ESP32 boards that join such a group.

## The desktop player

`hearth` runs on Windows, Linux, and macOS with a Qt 6.8 or later kit, and builds as the
`iclforge-hearth` package on each. It reads raw `.ac3`, `.ec3` and `.ac4` elementary streams.
**Add folder…** also lists MP4, Matroska and MPEG-TS files, and any file can be added or dropped.
The queue marks a file it cannot read as not playable, with the reason, when it reaches it, and
skips it or stops there as Settings > Playback > **An item fails** says. Every MP4, Matroska and
MPEG-TS file is one of those today.

The window has six pages, chosen in the header or with `Ctrl+1` to `Ctrl+6`.

| Page | What it holds |
|---|---|
| **Play** | The queue (add files or a folder, or drop them on the page), the transport bar (play and pause, stop, previous and next, a position scrubber, the Gapless toggle, volume), per-channel levels, BS.1770-4 loudness, the metadata of the frame being decoded, the Objects card for E-AC-3 with Atmos objects, and the signal path from decoder to output |
| **Media** | What the decoder reads from a queued item: its stream, bitstream or presentation information and metadata, with **Copy** and **Export JSON…** |
| **Speakers** | The speaker layout, the routing of each speaker to an output of the device, size, trim and delay per speaker, the bass-management crossover, and an identify tone |
| **Decoder** | The decoder's controls on an AC-3 and E-AC-3 tab and an AC-4 tab. The page turns to the tab of the item that is playing, and a change reaches that item at its next access unit or frame |
| **Network** | The Sendspin players found on the network, pairing, groups and their volumes, and each paired Hearth sink's own settings and firmware |
| **Settings** | Playback, the network name and discovery, pairing records, appearance, language and diagnostics |

The header says where playback goes. Clicking it, or **Choose…** in the signal path, opens the
output picker: this computer's outputs for decoded audio, the outputs that take AC-3 and E-AC-3 as
a bitstream, and the network groups. Its **Play here** button moves playback to the row chosen.
A network sink that another server holds, such as one Music Assistant holds, shows as in use by
another server, with a **Take it back** button. From a paired ESP32 sink's settings page the app
updates that sink's firmware. The engine and the Sendspin protocol are tested in CI, including
against `hearth-testsink` and the aiosendspin 9.1.1 server library that Music Assistant uses;
Music Assistant itself has not been tested.

### AC-4

The desktop player plays channel-based, immersive and object AC-4 (A-JOC and direct-coded)
through the library's AC-4 decoder, which decodes it for every local output. No local output takes
AC-4 as a bitstream. A stream none of whose presentations the decoder decodes stays in the queue,
marked as not playable with the decoder's reason, and is skipped.

The AC-4 tab of the Decoder page has six cards:

- **Presentation.** **Automatic**, or one presentation from the stream's table of contents. The
  picker records a choice by the presentation's `presentation_id` where the stream sends one, and
  by its place in the table otherwise. With no choice made, the decoder selects the presentation
  that best meets its preferences: the first in the language of the computer's locale, one that
  carries audio description when **Mix in audio description** is on, and one made for headphones
  when the Dynamic range device is Portable headphones. The table beside the picker shows each
  presentation's language, channel count, content and substream groups, with the one playing in
  bold.
- **Dialogue.** **Enhancement**, 0 to 12 dB, raises dialogue against the rest of the mix where the
  stream carries dialogue enhancement data, up to the stream's own limit. **Dialogue level**, -12
  to +12 dB, sets the dialogue against the music and effects where a presentation carries them
  apart, up to the most the stream allows. **Mix in audio description** plays first a presentation
  that carries an associated programme and mixes it in, at **Its level**, -12 to 0 dB.
- **Dynamic range** (marked AC-4 only). **Device** chooses which of the compression curves the
  stream carries applies: Automatic (the one for the output level: home theatre up to -27 dBFS,
  flat panel TV up to -17, portable above that), Home theatre, Flat panel TV, Portable speakers,
  Portable headphones or No compression. **Output level**, -31 to 0 dBFS, is the level that
  **Dialogue normalisation** brings the stream's dialogue to. With normalisation off, both are
  disabled and the stream plays at its coded level with no compression.
- **Stereo and mono** (shared with AC-3 and E-AC-3). **Downmix** is Lo/Ro or Lt/Rt and applies
  when the speaker layout is 2.0; a 1.0 layout folds to mono, and a wider layout is rendered
  instead of folded. **Follow the stream's preferred downmix** uses the method the stream names in
  place of that choice for a stereo layout, and takes Lt/Rt's Pro Logic II form where the stream
  prefers it; the page has no Pro Logic II control of its own. **Mix the LFE in** is on for AC-4
  until it is set here or on the AC-3 and E-AC-3 tab.
- **Errors** (shared with AC-3 and E-AC-3). **Bad frame** is Stop, Repeat and fade, or Mute; AC-4
  is back to what the stream carries at its next I-frame.
- **Immersive and objects** (marked AC-4 only). **Layout** folds an immersive element to 5.1.2,
  5.1.4, 7.1, 7.1.2 or 7.1.4 when the speaker layout does not itself fold to stereo or mono; As
  coded keeps the layout the stream was coded in. **Core decoding** reconstructs the immersive
  element and its objects in a lighter form for low-complexity playback, and renders the core
  layout alone; off decodes in full. Objects are rendered into the speaker layout beside the
  channels.

The Media page describes an AC-4 item with a Stream card (bitstream version, frame rate, sync
frames and CRC failures, bit rate, I-frames, splices, substreams), the presentations, the
substream groups, an Immersive card when the stream carries A-JOC, and the metadata of the
presentation a decoder with no preferences selects: dialogue level, loudness, dynamic range,
dialogue enhancement and downmix.

A seek in an AC-4 item starts the decoder at the last I-frame at least 6,144 samples before the
point asked for and drops what comes before that point. Gapless playback keeps the output open
from one item to the next when both have the same sample rate and speaker layout, and an AC-4
item can follow an E-AC-3 one on the same output. A network group starts again, with a gap, when
the next item goes to its members in another form: another burst type, or PCM alone. The
decoder's own delay, 1,313 samples at frame rate index 13 (27 ms at 48 kHz), is not trimmed from
the start of an item, so two AC-4 items in a queue play about that far apart.

In a network group, a member on the Hearth extension role takes the stream itself as IEC 61937
bursts, and a member that is a standard Sendspin player takes PCM decoded here. An AC-4 item goes
to an extension-role member as IEC 61937-14 bursts only if the member lists `ac4` among its data
types (no ESP32 build does yet); otherwise that member gets nothing for the item. It also gets
nothing when the presentation chosen on the AC-4 tab is one a sink would not choose with no
preferences: Hearth decodes that presentation here, and only the PCM members hear it.

### Diagnostics

Settings > Diagnostics has three buttons that produce the same text report of what Hearth has
done: outputs opened, items played, sinks found and paired, and every error. **Save
diagnostics…** writes it to a file, **View live…** shows it in a window that refreshes twice a
second, and **Copy diagnostics** puts it on the clipboard. Pairing keys, codes and file paths are
left out of it, and nothing is sent anywhere.

Setting `ICLFORGE_HEARTH_DIAGNOSTICS_PORT` to a port number, before Hearth starts, also serves
the report at `http://127.0.0.1:<port>/diagnostics`. The endpoint listens on the loopback address
only, and stays off when the variable is unset, does not name a port, or names one that cannot be
bound. On Windows every note Hearth records for the report is also sent to the debugger output
(`OutputDebugString`), where DebugView or an attached debugger shows it.

## ESP32 sinks

`hearth_sink` uses Improv Wi-Fi for initial network setup and Sendspin to pair with a server and
play in a group. It decodes AC-3, E-AC-3, and Atmos objects for its configured speaker layout; the
ESP32-C6 build plays stereo only. ESP32-S3 boards and an ESP32-C6 with an ESP32-S3 have played
AC-3 and E-AC-3 in Sendspin groups on hardware, for ten minutes each without an underrun (the
[S3 sink guide](sink-esp32-s3.md) and the [C6 sink guide](sink-esp32-c6.md) have the runs); no DAC
was wired to the boards. Every release from the next one on publishes sink firmware for the
ESP32-S3, the ESP32-C6, and the ESP32-P4; [Sink firmware](sink-firmware.md) covers installing it
and updating a board over its network.

**AC-4 on the sinks.** No ESP32 sink takes AC-4 in a Sendspin group: every `hearth_sink` build
lists `ac3` and `eac3` as its data types and none lists `ac4`, so a group's ESP32 member gets
nothing for an AC-4 item. Phase I6 of [`planning/ac4.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/ac4.md)
would change that and is not built. The desktop player sends AC-4 bursts to a member that lists
`ac4`, which today is the development test sink alone. Separately, a `hearth_sink` built for the
ESP32-P4 with `sdkconfig.ac4` (CI builds it; the published firmware images have the decoder off)
plays AC-4 from an HTTP source: 2.0 in the SIMPLE and A-SPX modes in real time at 360 MHz, and
nothing wider ([AC-4 on the P4](../platforms/bare-metal/esp32-p4.md#ac-4)).

## Where it runs

| Target | What runs there | Strongest evidence |
|---|---|---|
| [ESP32-S3](../platforms/bare-metal/esp32-s3.md) | `hearth_sink`, a Sendspin sink ([setup guide](sink-esp32-s3.md)); `i2s_player`, which loops a fixed test stream | Two boards in a Wi-Fi group for ten minutes without an underrun; all decode fixtures run in real time on a board; the sink pairs and plays under QEMU in CI |
| [ESP32-P4](../platforms/bare-metal/esp32-p4.md) | The AC-3 and E-AC-3 decoder, and `hearth_sink` built for boards of silicon revision v1.x, which reach Wi-Fi through the board's onboard ESP32-C6 ([firmware image](sink-firmware.md#which-image)); AC-4 decode in a build with `CONFIG_ICLFORGE_AC4` (off in the published images; [AC-4 on the P4](../platforms/bare-metal/esp32-p4.md#ac-4)) | All AC-3 and E-AC-3 decode fixtures run in real time on a board at 360 MHz; CI builds the sink firmware |
| [ESP32-C3](../platforms/bare-metal/esp32-c3.md) | The same decoder, in the fixed-point tier | Correct under `qemu-riscv32` emulation. No board has run it |
| [ESP32-C6](../platforms/bare-metal/esp32-c6.md) | Fixed-point decoder; `hearth_sink`'s Sendspin player, stereo only | All decode fixtures run on a board; a stereo Sendspin group with an ESP32-S3 played ten minutes with no underruns on either board ([setup guide](https://github.com/iainchesworthlabs/ac3forge/blob/main/esp-idf/iclforge/examples/hearth_sink/README.md#on-the-esp32-c6)). CI builds the Sendspin player for this part, for the 4 MB and the 16 MB flash layouts, and does not run it: ESP-IDF's RISC-V QEMU emulates the ESP32-C3 and no other part |
| [ESPHome](../platforms/bare-metal/esphome.md) | An external component wrapping the ESP32-S3 decoder | Config-checked in CI against the manifest; not yet a `media_player` or `speaker` source |
| Windows, Linux and macOS | `hearth` (the desktop window, Qt 6.8+) with its engine and Network page; `hearth-testsink`, `hearth-testserver` and `hearth-render` development tools | Engine and Sendspin interoperability tests run in CI, and the window builds there on all three platforms; each committed AC-4 test stream that the decoder accepts plays through the engine sample for sample as the library decodes it |

The desktop player's passthrough design uses the same path as `forge play`. That command has
played the AC-3, E-AC-3 and signed Atmos stream shapes listed there to a receiver through a
Raspberry Pi 4B without an underrun. See
[Raspberry Pi passthrough](../platforms/raspberry-pi.md#live-hdmi-passthrough-to-a-real-receiver).

## What it does not do

- **Send AC-4 to an ESP32 sink in a group.** [ESP32 sinks](#esp32-sinks) has the detail.
- **Play from a container.** Only raw `.ac3`, `.ec3` and `.ac4` streams play.
- **List AC-4 objects on the Play page.** The Objects card shows the positions of E-AC-3 objects.
  An AC-4 item's objects are rendered into the speaker layout, and the Media page says an A-JOC
  stream carries object metadata without listing it.
- **Remove an item from the queue.** The window has no control for it.
- **Trim the AC-4 decoder's delay** at the start of an item (see above).
- **Come with a user guide.** This page and the sink guides are the Hearth documentation. The
  [design record](design/player-appliance.md) explains the decisions behind the app.

## Where to go next

- [An ESP32-S3 sink](sink-esp32-s3.md) — build, flash, configure, and pair a board.
- [An ESP32-C6 sink](sink-esp32-c6.md) — the stereo player on a part with no PSRAM.
- [Sink firmware](sink-firmware.md) — install a published image, update over the network, and go
  back; or [install from the browser](sink-installer.md).
- [ESP32-S3](../platforms/bare-metal/esp32-s3.md) — decoder timing and memory measurements.
- [AC-4](../concepts/ac4.md) and [AC-4 decoding and encoding](../library/ac4.md) — the format and
  the library's decoder.
- [The design record](design/player-appliance.md) — decisions and current implementation status.
- [Hearth QML feature coverage](https://github.com/iainchesworthlabs/ac3forge/blob/main/apps/hearth/ui/tests/FEATURE_COVERAGE.md) — what the headless QML suites exercise.
- [Roadmap](../roadmap.md) — planned work.
