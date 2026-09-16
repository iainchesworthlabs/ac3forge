# AC3Forge Hearth

Hearth is the project's playback member: the thing that takes an AC-3, E-AC-3 or Atmos/JOC
stream and turns it into sound in a room, on a machine that sits next to a receiver and stays
there. Where [Forge](../forge/index.md) is for a person at a workstation and
[Crucible](../crucible/index.md) captures what a desktop is already playing, Hearth plays a
stream through to a speaker or an HDMI/S-PDIF receiver — bit-exact, with no re-encode where the
sink will take it.

!!! note "Status as of 2026-09-16: an ESP32-S3 sink plays in groups; the desktop player is being built"
    **An ESP32-S3 board is a Hearth sink.** `hearth_sink` joins a network over Improv Wi-Fi, pairs
    with a server, and plays as a Sendspin player: stereo PCM from Music Assistant, and AC-3 or
    E-AC-3 with Atmos objects through Hearth's own role, decoded and rendered to the board's
    speakers. Two boards have played one programme as a group for ten minutes with no underrun.
    No DAC has been wired to one yet. [An ESP32-S3 sink](sink-esp32-s3.md) sets one up.

    **The desktop reference player for Windows, Linux and macOS, `ac3hearth`, is being built**
    ([the design record](design/player-appliance.md)). Its engine is in `apps/hearth` and has no
    window yet, and it cannot play to a sink yet. `ac3hearth-testserver`, a developer tool, plays
    to sinks in the meantime. An ESP32-C6 sink follows the S3's. That plan replaced an earlier
    one, for a headless appliance with a web control page, on 2026-09-15.

## Where it runs

| Target | What runs there | Strongest evidence |
|---|---|---|
| [ESP32-S3](../platforms/bare-metal/esp32-s3.md) | `hearth_sink`, a Sendspin sink ([set one up](sink-esp32-s3.md)), which also plays from flash, SD, FAT or HTTP; and `i2s_player` (a fixed fixture, looped) | **Two boards in a group on Wi-Fi**, ten minutes, no underrun, play times within 549 µs; real time on a board for every fixture, Atmos objects placed onto 7.1.4; under QEMU in CI, paired and played to from the host with levels held to a test sink's |
| [ESP32-C3](../platforms/bare-metal/esp32-c3.md) | The same decoder, in the fixed-point tier | Correct under `qemu-riscv32` emulation. No board has run it |
| [ESPHome](../platforms/bare-metal/esphome.md) | An external component wrapping the ESP32-S3 decoder | Config-checked in CI against the manifest; not yet a `media_player` or `speaker` source |
| Windows, Linux and macOS | `ac3hearth`, a desktop reference player: its engine, with no window yet; and `ac3hearth-testsink` and `ac3hearth-testserver`, the test tools | The engine's tests, and the test tools against aiosendspin 9.1.1 in CI — see [the design record](design/player-appliance.md) |

The passthrough path itself — decode-or-pass-through, following what the sink will accept — is
proven outside Hearth too: `ac3cli play` ([Forge](../forge/index.md)) has locked every stream
shape, including signed Atmos, against a real Atmos-capable receiver on a Raspberry Pi 4B at zero
underruns ([Raspberry Pi](../platforms/raspberry-pi.md#live-hdmi-passthrough-to-a-real-receiver)).
That is the evidence the planned desktop player's passthrough mode builds on.

## What it does not do (yet)

The desktop application has no install guide, settings page or troubleshooting page, because it
has not been built. The sink has a guide, [An ESP32-S3 sink](sink-esp32-s3.md), which covers
flashing, joining a network, pairing, groups, wiring and slot widths. It ships as example code
and a component, which you build yourself: there is no firmware download.

## Where to go next

- [The design record](design/player-appliance.md) — what's decided about the desktop player and
  the sinks, and where the full plan is.
- [An ESP32-S3 sink](sink-esp32-s3.md) — a board from a checkout to playing in a group.
- [ESP32-S3](../platforms/bare-metal/esp32-s3.md) — the decoder on the part, and its timing and
  memory.
- [Forge](../forge/index.md) and [Crucible](../crucible/index.md) — the family's other two
  members.
- [Roadmap](../roadmap.md) — where the appliance sits against everything else planned.
