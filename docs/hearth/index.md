# AC3Forge Hearth

Hearth plays AC-3, E-AC-3, and E-AC-3 with Atmos objects through local speakers, an HDMI or S-PDIF
receiver, or synchronised network sinks. It can decode a stream for local speakers, pass the encoded
stream to a compatible receiver, or send it to ESP32 sinks over Sendspin, a protocol for
synchronised network audio. The desktop player also plays channel-based AC-4, which it decodes for
every output.

!!! note "Status as of 2026-09-26: the desktop player plays to network sinks and plays AC-4"
    **The desktop player `ac3hearth` plays to local outputs and to network sinks.** It runs on
    Windows, Linux, and macOS with a Qt 6.8+ kit, and builds as the `ac3forge-hearth` package on
    each. Its Network page lists the Sendspin players it finds on the local network, pairs with one
    from the code the sink shows, makes groups, and sets their volumes. The output picker sends
    playback to a group. A sink that another server holds, such as one Music Assistant holds,
    shows as in use by another server, with a **Take it back** button. From a paired ESP32 sink's
    settings page the app updates that sink's firmware. The engine and the Sendspin protocol are
    tested in CI, including against `ac3hearth-testsink` and the aiosendspin 9.1.1 server library
    that Music Assistant uses; Music Assistant itself has not been tested. It plays channel-based
    AC-4 through the library's AC-4 decoder, with the presentation, language, dialogue, and
    output-level controls on the Decoder page's AC-4 tab. A network group gets the AC-4 stream as
    IEC 61937-14 bursts, for the members that list AC-4.

    **ESP32 sinks play in groups.** `hearth_sink` uses Improv Wi-Fi for initial network setup and
    Sendspin to pair with a server and play in a group. It decodes AC-3, E-AC-3, and Atmos objects
    for its configured speaker layout. The ten-minute group run in the
    [S3 sink guide](sink-esp32-s3.md) had no DAC wired to the boards. Every release from the next
    one on publishes sink firmware for the ESP32-S3, the ESP32-C6, and the ESP32-P4;
    [Sink firmware](sink-firmware.md) covers installing it and updating a board over its network.

## Where it runs

| Target | What runs there | Strongest evidence |
|---|---|---|
| [ESP32-S3](../platforms/bare-metal/esp32-s3.md) | `hearth_sink`, a Sendspin sink ([setup guide](sink-esp32-s3.md)); `i2s_player`, which loops a fixed test stream | Two boards in a Wi-Fi group for ten minutes without an underrun; all decode fixtures run in real time on a board; the sink pairs and plays under QEMU in CI |
| [ESP32-P4](../platforms/bare-metal/esp32-p4.md) | The decoder, and `hearth_sink` built for boards of silicon revision v1.x, which reach Wi-Fi through the board's onboard ESP32-C6 ([firmware image](sink-firmware.md#which-image)) | All decode fixtures run in real time on a board at 360 MHz; CI builds the sink firmware |
| [ESP32-C3](../platforms/bare-metal/esp32-c3.md) | The same decoder, in the fixed-point tier | Correct under `qemu-riscv32` emulation. No board has run it |
| [ESP32-C6](../platforms/bare-metal/esp32-c6.md) | Fixed-point decoder; `hearth_sink`'s Sendspin player, stereo only | All decode fixtures run on a board; a stereo Sendspin group with an ESP32-S3 played ten minutes with no underruns on either board ([setup guide](https://github.com/iainchesworthlabs/ac3forge/blob/main/esp-idf/ac3forge/examples/hearth_sink/README.md#on-the-esp32-c6)). CI builds the Sendspin player for this part now too - `idf.py qemu` refuses `esp32c6` outright, so nothing shorter than a board runs it |
| [ESPHome](../platforms/bare-metal/esphome.md) | An external component wrapping the ESP32-S3 decoder | Config-checked in CI against the manifest; not yet a `media_player` or `speaker` source |
| Windows, Linux and macOS | `ac3hearth` (the desktop window, Qt 6.8+) with its engine and Network page; `ac3hearth-testsink`, `ac3hearth-testserver` and `ac3hearth-render` development tools | Engine and Sendspin interoperability tests run in CI, and the window builds there on all three platforms; every committed AC-4 stream plays through the engine sample for sample as the library decodes it |

The desktop player's passthrough design uses the same path as `ac3cli play`. That command has
played every supported stream shape, including signed Atmos, to a receiver through a Raspberry
Pi 4B without an underrun. See [Raspberry Pi passthrough](../platforms/raspberry-pi.md#live-hdmi-passthrough-to-a-real-receiver).

## What it does not do

- **Play immersive or object AC-4.** The app plays channel-based AC-4 only. The
  [library decodes](../library/ac4.md) immersive and object streams too; the app does not use
  that yet. No local output takes AC-4 as a bitstream, and no ESP32 sink decodes AC-4: only the
  development test sink takes the AC-4 bursts a group is sent.
- **Come with a user guide.** This page and the sink guides are the Hearth documentation. The
  [design record](design/player-appliance.md) explains the decisions behind the app.

## Where to go next

- [An ESP32-S3 sink](sink-esp32-s3.md) — build, flash, configure, and pair a board.
- [Sink firmware](sink-firmware.md) — install a published image, update over the network, and go
  back; or [install from the browser](sink-installer.md).
- [ESP32-S3](../platforms/bare-metal/esp32-s3.md) — decoder timing and memory measurements.
- [The design record](design/player-appliance.md) — decisions and current implementation status.
- [Roadmap](../roadmap.md) — planned work.
