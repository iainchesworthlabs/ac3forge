# AC3Forge Hearth

Hearth plays AC-3, E-AC-3, and E-AC-3 with Atmos objects through speakers or an HDMI/S-PDIF
receiver. It can decode a stream for local speakers or pass the encoded stream to a compatible
receiver.

!!! note "Status as of 2026-09-16: an ESP32-S3 sink plays in groups; the desktop player is being built"
    **The ESP32-S3 sink works on a network.** `hearth_sink` uses Improv Wi-Fi for initial network
    setup. It uses Sendspin, a protocol for synchronised network audio, to pair with a server and
    play in a group. Compatibility with the aiosendspin 9.1.1 server library used by Music
    Assistant is validated in CI; Music Assistant itself has not been tested. The sink decodes
    AC-3, E-AC-3, and Atmos objects for its configured speaker layout. Two boards have played one
    programme from the AC3Forge test server as a group for ten minutes without an underrun. No
    DAC has been connected yet.
    [An ESP32-S3 sink](sink-esp32-s3.md) explains setup.

    **The desktop player `ac3hearth` is being built for Windows, Linux, and macOS.** Its engine
    and tests are in `apps/hearth`. It has no window and cannot send audio to a sink.
    `ac3hearth-testserver` provides that function during development. See the
    [design record](design/player-appliance.md).

## Where it runs

| Target | What runs there | Strongest evidence |
|---|---|---|
| [ESP32-S3](../platforms/bare-metal/esp32-s3.md) | `hearth_sink`, a Sendspin sink ([setup guide](sink-esp32-s3.md)); `i2s_player`, which loops a fixed test stream | Two boards in a Wi-Fi group for ten minutes without an underrun; all decode fixtures run in real time on a board; the sink pairs and plays under QEMU in CI |
| [ESP32-C3](../platforms/bare-metal/esp32-c3.md) | The same decoder, in the fixed-point tier | Correct under `qemu-riscv32` emulation. No board has run it |
| [ESP32-C6](../platforms/bare-metal/esp32-c6.md) | Fixed-point decoder; the example includes a `hearth_sink` build overlay | All decode fixtures run on a board. There is no C6 sink guide or Sendspin CI job |
| [ESPHome](../platforms/bare-metal/esphome.md) | An external component wrapping the ESP32-S3 decoder | Config-checked in CI against the manifest; not yet a `media_player` or `speaker` source |
| Windows, Linux and macOS | `ac3hearth` engine; `ac3hearth-testsink` and `ac3hearth-testserver` development tools | Engine and Sendspin interoperability tests run in CI. There is no desktop window |

The desktop player's passthrough design uses the same path as `ac3cli play`. That command has
played every supported stream shape, including signed Atmos, to a receiver through a Raspberry
Pi 4B without an underrun. See [Raspberry Pi passthrough](../platforms/raspberry-pi.md#live-hdmi-passthrough-to-a-real-receiver).

## What it does not do (yet)

The desktop application has no window, package, or user guide. The ESP32-S3 sink is source code
that you build and flash; there is no firmware download. Its [setup guide](sink-esp32-s3.md)
covers network setup, pairing, groups, wiring, and slot widths.

## Where to go next

- [An ESP32-S3 sink](sink-esp32-s3.md) — build, flash, configure, and pair a board.
- [ESP32-S3](../platforms/bare-metal/esp32-s3.md) — decoder timing and memory measurements.
- [The design record](design/player-appliance.md) — decisions and current implementation status.
- [Roadmap](../roadmap.md) — planned work.
