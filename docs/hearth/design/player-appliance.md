# The Hearth plan

This page points to the plan rather than reproducing it. Design proposals live in the
repository's `planning/` directory and are deliberately not republished onto this site — see
[`planning/README.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/README.md)
for why: a proposal read alongside reference documentation is too easily read as a shipped
feature.

## What's decided

Decided on 2026-09-15. Hearth has two forms that talk to each other:

- **`ac3hearth`**, a desktop reference player for Windows, Linux and macOS. It plays AC-3, E-AC-3
  and E-AC-3 JOC media with every decoder setting the library has, renders to a chosen speaker
  layout, routes each channel to an output with trim, delay and bass management, and shows the
  per-channel levels and the bitstream information. It plays to a local device, passes the
  bitstream through to a receiver, or streams to sinks on the network. AC-4 is designed in and
  waits for a decoder.
- **`hearth_sink`**, firmware for ESP32-S3 (up to sixteen TDM outputs) and ESP32-C6 (up to
  eight) boards. A sink is a Sendspin player: Music Assistant can play to it, and `ac3hearth`
  sends it the undecoded bitstream through an extension role, which the board decodes and
  renders to its own layout.

Between them is Sendspin, with `ac3hearth` as a conformant server and the boards as conformant
players, so groups, clock synchronisation, encryption and pairing come from that protocol.

This replaced the plan decided on 2026-09-07 for a headless appliance: a daemon beside a
receiver with a web control page, an optional kiosk window, and an HLS client. The name, Hearth's
place as the family's fourth member, and its build identity carried over.

## What's built

As of 2026-09-16. Detail: [Hearth overview](../index.md).

- **ESP32-S3 `hearth_sink`.** Network player (Sendspin). Two boards played one programme as a group. Setup: [An ESP32-S3 sink](../sink-esp32-s3.md). No DAC wired yet.
- **`apps/hearth`.** `ac3hearth` engine (no window; cannot play to a sink), `ac3hearth-testsink`, `ac3hearth-testserver`. CI runs the engine tests and plays to an emulated S3 from the test server.
- **`src/sendspin`.** Shared by the desktop tools and the board.
- **ESP32-C6.** Decode probe timed on a board. `hearth_sink`'s Sendspin player runs on this board, stereo only: a ten-minute group run with an ESP32-S3 had no underruns on either board. Setup: [README, "On the ESP32-C6"](https://github.com/iainchesworthlabs/ac3forge/blob/main/esp-idf/ac3forge/examples/hearth_sink/README.md#on-the-esp32-c6). Still no C6 Sendspin CI job.

## The full record

[`planning/hearth-reference-player.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/hearth-reference-player.md)
has the complete plan: the application's features mapped to the library, the architecture, the
Sendspin extension role, the sinks' memory and output limits per chip, the phases of its four
parts with their exit criteria, what cannot be verified, and the twenty decisions with their
reasoning.

[`planning/player-appliance.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/player-appliance.md)
keeps the 2026-09-07 appliance plan as a record, including the sink-following gaps in
`ac3cli play` that the desktop player's passthrough mode closes.

## Where to go next

- [Hearth overview](../index.md) — what exists today.
- [Crucible's own design record](../../crucible/design/promotion.md) — the closest precedent: a
  demo promoted to a product, with the same phase-by-phase shape.
- [Roadmap](../../roadmap.md) — where this sits against everything else planned.
