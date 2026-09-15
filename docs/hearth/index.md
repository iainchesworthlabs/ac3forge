# AC3Forge Hearth

Hearth is the project's playback member: the thing that takes an AC-3, E-AC-3 or Atmos/JOC
stream and turns it into sound in a room, on a machine that sits next to a receiver and stays
there. Where [Forge](../forge/index.md) is for a person at a workstation and
[Crucible](../crucible/index.md) captures what a desktop is already playing, Hearth plays a
stream through to a speaker or an HDMI/S-PDIF receiver — bit-exact, with no re-encode where the
sink will take it.

!!! note "Status: hardware-verified as an embedded player; a desktop player and network sinks are planned, not built"
    **Today, Hearth is the bare-metal player.** `esp-idf/ac3forge/examples/` decodes AC-3 and
    E-AC-3 — including Atmos objects, placed onto 7.1.4 — in real time on an ESP32-S3 board, and
    drives real I2S and TDM hardware. The streaming example already exposes a small HTTP control
    surface (`GET /status`, `POST /volume`, `POST /play`, `POST /stop`), tested through a port
    forward under QEMU and with its requests measured on a board.

    **A desktop reference player for Windows, Linux and macOS, `ac3hearth`, and firmware that
    turns ESP32-S3 and ESP32-C6 boards into Sendspin network sinks, `hearth_sink`, are a decided
    plan ([the design record](design/player-appliance.md)); no code for either exists yet, and
    there is no `apps/hearth` in the tree.** That plan replaced an earlier one, for a headless
    appliance with a web control page, on 2026-09-15.

## Where it runs

| Target | What runs there | Strongest evidence |
|---|---|---|
| [ESP32-S3](../platforms/bare-metal/esp32-s3.md) | Two example players — `i2s_player` (a fixed fixture, looped) and `stream_player` (flash, SD, FAT or HTTP source; I2S, TDM, capture or null sink) | **Real time on a board**, every fixture, Atmos objects placed onto 7.1.4; `stream_player`'s control surface exercised through QEMU with a port forward |
| [ESP32-C3](../platforms/bare-metal/esp32-c3.md) | The same decoder, in the fixed-point tier | Correct under `qemu-riscv32` emulation. No board has run it |
| [ESPHome](../platforms/bare-metal/esphome.md) | An external component wrapping the ESP32-S3 decoder | Config-checked in CI against the manifest; not yet a `media_player` or `speaker` source |
| Windows, Linux and macOS | `ac3hearth`, a desktop reference player: planned, not built | Nothing yet — see [the design record](design/player-appliance.md) |

The passthrough path itself — decode-or-pass-through, following what the sink will accept — is
proven outside Hearth too: `ac3cli play` ([Forge](../forge/index.md)) has locked every stream
shape, including signed Atmos, against a real Atmos-capable receiver on a Raspberry Pi 4B at zero
underruns ([Raspberry Pi](../platforms/raspberry-pi.md#live-hdmi-passthrough-to-a-real-receiver)).
That is the evidence the planned desktop player's passthrough mode builds on.

## What it does not do (yet)

No install guide, no settings page, no troubleshooting page exists here, because the application
they would document has not been built. What exists today is the embedded player above,
documented on its own platform pages rather than as a product guide, since it ships as example
code and a component, not an installable application.

## Where to go next

- [The design record](design/player-appliance.md) — what's decided about the desktop player and
  the sinks, and where the full plan is.
- [ESP32-S3](../platforms/bare-metal/esp32-s3.md) — the real, hardware-verified player today.
- [Forge](../forge/index.md) and [Crucible](../crucible/index.md) — the family's other two
  members.
- [Roadmap](../roadmap.md) — where the appliance sits against everything else planned.
