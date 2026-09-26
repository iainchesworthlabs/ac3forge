# Sink firmware: install, update, go back

Each release of this project publishes `hearth_sink` built for four boards, so a board can be set
up and kept up to date without ESP-IDF. This guide is for someone with a board and a release. It
covers:

- which image a board takes, and checking a download;
- installing a new board, and moving a board that runs an older build;
- updating over the network, what the board does during an update, and going back;
- what to do when a board does not come back.

To build the firmware yourself instead, see [An ESP32-S3 sink](sink-esp32-s3.md#build-and-flash).
[planning/esp32-ota.md](https://github.com/iainchesworthlabs/ac3forge/blob/main/planning/esp32-ota.md)
has the design and the reasons for it.

!!! note "Status as of 2026-09-25: unsigned development images"
    The images are checked for damage at every step, but not signed: while the boards are in
    development, anyone on a board's network can update it, as anyone with a USB cable can.
    Signed images come when the boards leave development. No release has published sink
    firmware yet; the first one to do so will carry the files this guide names.

## Which image

| Image | For |
|---|---|
| `hearth-sink-esp32s3` | An ESP32-S3 with 16 MB of flash and 8 MB of octal PSRAM, such as the ESP32-S3-DevKitC-1-N16R8 |
| `hearth-sink-esp32c6` | Any ESP32-C6 module; it uses a 4 MB partition table |
| `hearth-sink-esp32c6-16mb` | An ESP32-C6 with 16 MB of flash |
| `hearth-sink-esp32p4-rev1` | An ESP32-P4 of silicon revision v1.x, such as the DFRobot FireBeetle 2 |

A P4 of revision v3.x needs another build, which releases do not publish yet. The P4 reaches
Wi-Fi through its onboard ESP32-C6, whose own firmware these images do not change.

**Telling boards apart.** Over USB, `esptool chip-id` names the chip and its revision, and
`esptool flash-id` gives the flash size. A board already on the network reports both:
`http://<board>/hardware` gives its chip, revision and PSRAM, and `http://<board>/firmware` its
flash size and partition table. [`ota.py`](#updating-over-the-network) chooses the image for each
board from these, so a board updated over the network never needs this table.

Each image is published as four files, with `<version>` the release's tag:

| File | What it is for |
|---|---|
| `<image>-<version>.bin` | The app image, for an update over the network |
| `<image>-<version>-factory.bin` | The whole flash from address 0, for a new board. It also erases the board's settings |
| `<image>-<version>-parts.zip` | The same pieces as separate files with a `flash_args`, for a board already in use. Its settings are kept |
| `<image>-<version>-elf.zip` | The ELF, for reading a crash's backtrace or core dump |

`hearth-sink-manifest.json` describes all four images for the tools.

## Checking a download

Every release has a `SHA512SUMS` file. In the directory you downloaded into:

```bash
sha512sum --check --ignore-missing SHA512SUMS
```

Each file also has a build provenance attestation, which says which repository, workflow and
commit built it:

```bash
gh attestation verify hearth-sink-esp32s3-v0.11.0-factory.bin --repo iainchesworthlabs/ac3forge
```

When a release is GPG-signed, each file has a `.asc` signature beside it, and the release carries
`ac3forge-signing-key.asc`. [Releasing](../releasing.md) says how the signing works.

`ota.py push --release` and the [browser installer](sink-installer.md) check what they download
against the release's manifest and `SHA512SUMS` before they write anything.

## A new board

**In the browser:** [the installer](sink-installer.md) writes the image over USB from Chrome, Edge
or Firefox, and then gives the board its network over Improv. Choose to erase a new board.

**With esptool:** write the factory image at address 0, from any computer with Python:

```bash
pip install esptool
esptool --port PORT write-flash 0x0 hearth-sink-esp32s3-v0.11.0-factory.bin
```

`PORT` is the board's serial port, such as `COM5` or `/dev/ttyACM0`. Then give the board its
network over Improv, as [Join a network](sink-esp32-s3.md#join-a-network) describes. A new board
is called `hearth-` followed by the last six hex digits of its MAC address.

## A board running an older build

A board that runs a build from before the two-slot flash layout, or any other build, moves to the
release over USB once. Its name, network and pairings are kept, since neither way erases the
board's settings (its NVS partition):

- **In the browser:** [the installer](sink-installer.md), choosing not to erase;
- **with esptool:** unpack the image's `-parts.zip` and, in that directory, run
  `esptool --port PORT write-flash @flash_args`.

After that, updates go over the network.

## Updating over the network

Any one of these sends a board a new app image. The board checks it and restarts into it on trial
([below](#what-the-board-does-during-an-update)).

- **`ota.py`**, from this repository's `tools/hearth/`, is one Python file with nothing to install.
  It finds the boards on the network, gives each the image that fits it, checks every download
  against the release's manifest and `SHA512SUMS`, and waits for each board to accept its image
  or go back:

    ```bash
    python ota.py push --release latest --all
    python ota.py push --release v0.11.0 --host hearth-eb2c64.local
    python ota.py status --all
    ```

    `--all` finds boards by mDNS and needs the `zeroconf` Python package; `--host` names a board
    by its address or `.local` name. Boards are updated one at a time, and a board that goes back
    stops the rest.
- **The board's page,** `http://<board>/`: **Update firmware…** under Firmware, with the image's
  `.bin`. The page reads the image's head first and refuses a file for another chip or project.
- **ac3hearth,** the desktop app: the **Firmware** tab of a paired sink's settings page.
- **curl:** `curl -T hearth-sink-esp32s3-v0.11.0.bin http://hearth-eb2c64.local/firmware`.
  This does no checks of its own; the board still makes all of its own.

Name a board by its IP address or its own `.local` name: the board refuses firmware addressed to
any other name.

## What the board does during an update

1. **Flash mode.** The board stops what it plays, says goodbye to its Sendspin server and stops
   answering settings changes. It writes the image into the slot it is not running, and checks
   what it wrote before it answers.
2. **The trial.** It restarts into the new image, which is on trial. The image is accepted once
   the board has held a network address, its web server and its Sendspin player together for 30
   seconds, within 5 minutes of starting.
3. **Or it goes back.** If the image does not get there, panics or resets before it is accepted,
   the board boots the image it ran before, and reports why. A power cut during the trial also
   brings the previous image back.

`ota.py` prints each stage. It then says the board was **updated**, **rolled back** (with the
board's reason), or **did not come back** (with what to try). The page shows the same in its
Firmware section, and ac3hearth in its Firmware tab. Every board keeps its recent console output,
and the C6 and the P4 also keep a crash's core dump, both readable over the network; the board's
[README](https://github.com/iainchesworthlabs/ac3forge/blob/main/esp-idf/ac3forge/examples/hearth_sink/README.md)
covers them.

## Going back

- **Roll back** on the board's page or in ac3hearth, or `python ota.py rollback --host <board>`.
  The board restarts into the image in its other slot, which then has a trial of its own.
- **An older release,** pushed as any other: `python ota.py push --release <tag> --host <board>`.

## When a board does not come back

1. **Cycle its power while the new image is on trial.** A reset before the image is accepted
   boots the previous one. Then `python ota.py status --host <board>` says what it runs.
2. **Failing that, USB:** [the installer](sink-installer.md) without erasing, or the release's
   `-parts.zip` with `esptool write-flash @flash_args`.
3. **If the board does not answer on USB either,** put it into the chip's download mode: hold
   BOOT, press and release RESET, then release BOOT. That mode is in the chip's ROM, which nothing
   can overwrite, so it is always there. After flashing, press RESET once.
