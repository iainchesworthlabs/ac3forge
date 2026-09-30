# The Hearth plan

This page points to the plan rather than reproducing it. Design proposals live in the
repository's `planning/` directory and are deliberately not republished onto this site — see
[`planning/README.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/README.md)
for why: a proposal read alongside reference documentation is too easily read as a shipped
feature.

!!! note "Status as of 2026-09-30: built, with the user guide and the hardware exits open"
    The plan decided on 2026-09-15 is built apart from the items its own status names: a user
    guide and screenshots of the running app, and the exits that need a DAC wired to a board, a
    multichannel endpoint or Music Assistant itself. The appliance plan of 2026-09-07, which it
    replaced, was not built in that form.

## What's decided

Decided on 2026-09-15. Hearth has two forms that talk to each other:

- **`ac3hearth`**, a desktop reference player for Windows, Linux and macOS. It plays AC-3, E-AC-3
  and E-AC-3 JOC media with every decoder setting the library has, renders to a chosen speaker
  layout, routes each channel to an output with trim, delay and bass management, and shows the
  per-channel levels and the bitstream information. It plays to a local device, passes the
  bitstream through to a receiver, or streams to sinks on the network. The plan designed AC-4 in
  and left its decoder to a later chip (decision 7); the player has since gained it, as
  [What's built](#whats-built) records.
- **`hearth_sink`**, firmware for ESP32-S3 (up to sixteen TDM outputs) and ESP32-C6 (up to
  eight) boards. A sink is a Sendspin player: Music Assistant can play to it, and `ac3hearth`
  sends it the undecoded bitstream through an extension role, which the board decodes and
  renders to its own layout. The ESP32-P4 joined the sink family after the plan was written.

Between them is Sendspin, with `ac3hearth` as a conformant server and the boards as conformant
players, so groups, clock synchronisation, encryption and pairing come from that protocol.

This replaced the plan decided on 2026-09-07 for a headless appliance: a daemon beside a
receiver with a web control page, an optional kiosk window, and an HLS client. The name, Hearth's
place as the family's fourth member, and its build identity carried over.

## What's built

Detail: [Hearth overview](../index.md).

- **ESP32 `hearth_sink`.** A Sendspin player with Improv Wi-Fi setup, pairing, groups and updates
  over the network, on the ESP32-S3, the ESP32-C6 (stereo only) and the ESP32-P4 (silicon
  revision v1.x, with Wi-Fi through the board's onboard C6). Two S3 boards played one programme as
  a group, and a C6 and an S3 played a stereo group. No DAC is wired to any board yet. Setup: [An
  ESP32-S3 sink](../sink-esp32-s3.md).
- **`apps/hearth`.** The `ac3hearth` engine and window (Qt Quick, with Play, Media, Speakers,
  Decoder, Network and Settings pages; it builds and packages on Windows, Linux and macOS when Qt6
  6.8+ is found), `ac3hearth-testsink`, `ac3hearth-testserver` and `ac3hearth-render`. The player
  reads raw `.ac3`, `.ec3` and `.ac4` streams and plays to a local device, to a receiver as a
  bitstream, or to a group of network sinks. CI runs the engine tests and plays to an emulated S3
  from the test server.
- **AC-4 in `ac3hearth`** (phases I2 and I5 of
  [`planning/ac4.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/ac4.md)).
  The engine plays channel-based, immersive and object AC-4 through the decoder's public API,
  decoded for every local output, with the Decoder page's AC-4 controls and the Media page's AC-4
  information. A network group's members on the extension role that list AC-4 are sent the stream
  as bursts, and its PCM members get the decoded audio. No ESP32 sink lists AC-4 yet (phase I6).
- **`src/sendspin`.** Shared by the desktop tools and the board.
- **ESP32-C6.** Decode probe timed on a board. `hearth_sink`'s Sendspin player runs on this board,
  stereo only: a ten-minute group run with an ESP32-S3 had no underruns on either board. Setup:
  [README, "On the ESP32-C6"](https://github.com/iainchesworthlabs/ac3forge/blob/main/esp-idf/iclforge/examples/hearth_sink/README.md#on-the-esp32-c6).
  CI builds the Sendspin player for this part, for the 4 MB and the 16 MB flash layouts, and does
  not run it: ESP-IDF's RISC-V QEMU emulates the ESP32-C3 and no other part.

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
