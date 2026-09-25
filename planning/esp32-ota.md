# Firmware over the network for Hearth sinks

**Status, 2026-09-24:** proposed. Nothing is built. Every `hearth_sink` layout on `main`
(`b49a966c`) is a single `factory` app, and nothing in the tree calls `esp_ota_*`. The ESP-IDF
facts below were read from the v6.1 tree at `D:\esp\esp-idf`, which the board builds use. The
board facts come from the builds and flashes of 2026-09-24. [Decisions](#decisions) lists what is
recommended and what each choice costs. The user took decisions 2, 5 and 9 on 2026-09-24:

- Images are not signed while the boards are in development, so anyone on the network can flash
  a board, as anyone with a USB cable can. Every image is checked for damage from the build to the
  flash and at every boot ([Integrity](#integrity)).
- The board decides when a new image is accepted.
- The P4's co-processor firmware waits for a phase of its own.

On 2026-09-25 the user added two final phases. CI builds the firmware for every board and
publishes it, as it does the desktop packages ([Published images](#published-images), O8). A
guide then tells a user how to use those images ([The user guide](#the-user-guide), O9).

Today a `hearth_sink` board is updated over its USB connector:

- a build on the PC;
- `esptool write-flash @flash_args` on the board's COM port;
- sometimes a BOOT or RESET press, or `--after watchdog-reset` on the board that needs it;
- and nothing at all while the cable or the port is being used for something else.

This plan adds updates over the network the board is already on, usually Wi-Fi, or Ethernet
under QEMU. It also adds a remote restart, and a way back to the previous firmware. Five rules
keep it safe:

- The new image goes into the slot that is not running.
- The board keeps running its current image until the new one has been checked in full: for
  damage, and for being an image for this board.
- A new image stays on trial until it has shown it can do the job, and any reset during the trial
  brings the previous image back.
- The bootloader checks an image before every boot, and boots the other slot if the image has
  been damaged.
- An update never writes the bootloader, the partition table or eFuses, so USB stays the way to
  recover a board. It is still needed when all else fails.

It covers every board the project runs on: the ESP32-S3, the ESP32-C6 and the ESP32-P4.

## What exists

| | ESP32-S3 | ESP32-C6 | ESP32-P4 |
|---|---|---|---|
| Boards | two DevKitC-1 N16R8: COM15 `hearth-eb2c64`, COM16 `hearth-47b39c` | one, QFN40 rev v0.2, COM9 | one DFRobot FireBeetle 2, rev v1.3, COM10 (its USB link is down as of 2026-09-24; it is reachable only over Wi-Fi) |
| Flash on the board | 16 MB | 16 MB (the 2026-09-15 bring-up note; check with `esptool flash-id` before migrating) | 16 MB |
| Flash size the build assumes | 16 MB (`sdkconfig.defaults`) | 4 MB (`sdkconfig.c6`, for any C6 module) | 16 MB |
| Partition table | `partitions.csv`: `factory` 1.5 MiB | `partitions_c6.csv`: `factory` 2 MiB | `partitions_p4.csv`: `factory` 4 MiB |
| Sendspin image, 2026-09-24 | 1,419,104 bytes (90% of its partition) | 1,580,816 bytes (75%) | 1,463,536 bytes (35%) |
| Bootloader | 21,168 of 32,768 bytes (at `0x0`) | 23,152 of 32,768 bytes (at `0x0`) | **23,296 of 24,576 bytes** (at `0x2000`) |
| QEMU machine | yes; CI runs `hearth_sink` over the emulated Ethernet (`net/openeth/`) | no | no |
| Network | Wi-Fi | Wi-Fi, with Wi-Fi's code in flash (`sdkconfig.sendspin-c6`) | Wi-Fi through the onboard ESP32-C6 over SDIO (`esp_hosted`) |

All three tables share one shape: `nvs` at `0x9000` (24 KiB), `phy_init` at `0xF000`, the app at
`0x10000`, then `audio` and `storage` (256 KiB each, for the partition and FAT sources). NVS holds
what a board is:

- its name, network, slot width and wiring (namespace `hearth_sink`, `main/settings.cpp`);
- its Sendspin identity and pairing records (namespace `sendspin`,
  `esp-idf/ac3forge/src/sendspin_store.cpp`).

A USB flash leaves NVS alone, because `flash_args` has no region there. The migration below keeps
it that way.

The control surface ([control.hpp](../esp-idf/ac3forge/include/ac3forge/control.hpp)) is
`esp_http_server` on port 80:

- three sockets, least-recently-used purge, a 6,144-byte task stack;
- no authentication. Whoever can reach the port can drive the board, as
  [the device UI plan](esp32-device-ui.md#security) records. The network is the boundary.

`GET /hardware` already reports the running firmware's project, version and ESP-IDF version.

## The shape

```
 PC                                   board (running ota_0)
 ──                                   ─────────────────────
 idf.py build
 tools/hearth/ota.py push ──────────► GET /hardware, GET /firmware    (pre-flight: chip, revision,
                                                                       layout, not on trial)
                      ──────────────► PUT /firmware  (the app image, with its SHA-256)
                                        enter flash mode: playback, Sendspin, sink stopped
                                        write ota_1; check the image and the SHA-256 of what
                                        was sent against what is now in flash
                                        boot ota_1 next, reply 200, restart
                                      bootloader: ota_1 is NEW → PENDING_VERIFY, boot it
                                      ota_1 on trial: network address + HTTP server
                                        + Sendspin player, 30 s without a break
 poll GET /firmware ────────────────► accepted (valid)          or     any reset / 5 min → ota_0
 report: updated / rolled back (why) / did not come back (what to try)
```

Six pieces:

1. **An A/B flash layout.** Two app slots (`ota_0` and `ota_1`) and `otadata`. Each board gets it
   once, over USB, with NVS kept.
2. **A bootloader with rollback** (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`). A new image boots on
   trial, and a reset before it is accepted boots the previous one.
3. **Integrity checks at every step.** A SHA-256 goes from the build to the flash with each
   image, and the bootloader checks the image before every boot. No eFuses are burned. Signed
   images come later ([Signing, later](#signing-later)).
4. **Flash mode, and the routes around it.** Flash mode is the state the board is in while it
   takes an update. The routes are `GET /firmware`, `PUT /firmware`, `PUT /firmware/mode`,
   `PUT /firmware/rollback` and `POST /restart`.
5. **A host tool.** `tools/hearth/ota.py` pushes a build to one board or to all of them, waits for
   each one to accept the new image, and reports what happened. `idf.py ota` wraps it. The web
   page and `ac3hearth` come later and use the same routes.
6. **Published images and a guide.** CI builds each board's firmware and publishes it with the
   desktop packages, and a user guide covers installing and updating from those images (O8 and
   O9, the last phases).

## Flash layout

`nvs` and `phy_init` stay where they are. `otadata` goes where the app started, and the first
slot at `0x20000`.

**16 MB boards: every board on the desk.** `partitions.csv` becomes:

| Name | Type | SubType | Offset | Size | |
|---|---|---|---|---|---|
| `nvs` | data | nvs | `0x9000` | `0x6000` | unchanged |
| `phy_init` | data | phy | `0xF000` | `0x1000` | unchanged |
| `otadata` | data | ota | `0x10000` | `0x2000` | new |
| `ota_0` | app | ota_0 | `0x20000` | `0x400000` | 4 MiB |
| `ota_1` | app | ota_1 | `0x420000` | `0x400000` | 4 MiB |
| `coredump` | data | coredump | `0x820000` | `0x10000` | new; used from O4 |
| `audio` | data | `0x40` | `0x830000` | `0x40000` | moved, same size |
| `storage` | data | fat | `0x870000` | `0x40000` | moved, same size |
| `reserve` | data | `0x41` | `0x8B0000` | `0x400000` | new; empty ([decision 8](#decisions)) |

That ends at 12.7 MiB. A 4 MiB slot is 2.6 to 3 times the size of each chip's image today. The
P4 already had a 4 MiB app partition for this reason. The P4 and the C6 on COM9 then use this table,
so `partitions_p4.csv` goes. A 16 MB C6 selects it with a new overlay, `sdkconfig.flash16mb`
(`CONFIG_ESPTOOLPY_FLASHSIZE_16MB` and this table), placed after `sdkconfig.c6` in the list
([decision 7](#decisions)).

**4 MB C6 modules.** `partitions_c6.csv` becomes:

| Name | Offset | Size | |
|---|---|---|---|
| `nvs`, `phy_init`, `otadata` | as above | | |
| `ota_0` | `0x20000` | `0x1C0000` | 1.75 MiB |
| `ota_1` | `0x1E0000` | `0x1C0000` | 1.75 MiB |
| `coredump` | `0x3A0000` | `0x10000` | |
| `audio` | `0x3B0000` | `0x10000` | 64 KiB, from 256; the sample is 10,752 bytes |
| `storage` | `0x3C0000` | `0x40000` | ends at `0x400000` |

The C6 image is 86% of a 1.75 MiB slot today. When it outgrows that, 4 MB C6 modules need
something else: a smaller image, or no `audio`/`storage` partitions in the C6's Sendspin build. The
limit is written here so that it does not arrive as a surprise.

**Moving a board to the new layout** is one USB flash of the usual `write-flash @flash_args`. The
build's `flash_args` then writes:

- the bootloader, now with rollback;
- the new partition table;
- `ota_data_initial.bin` (erased, so the bootloader boots `ota_0`);
- the app into `ota_0`;
- `audio` and `storage` at their new offsets.

It still writes nothing at `0x9000`, so the board keeps its name, its network and its pairings.
The app finds `audio` and `storage` by label, not by offset. The first image written this way has
to be able to take the next update over the network, so no board migrates until O1 is merged in
full ([Phases](#phases)).

**A network built into the image.** A board joins the network stored in its NVS, or failing
that, the one compiled into its image from `CONFIG_AC3FORGE_EXAMPLE_WIFI_SSID`
(`net/wifi/network.cpp`). Nothing copies the built-in one into NVS. The board builds on the desk
compile one in from a local fragment, so a board may never have had its network stored. A
published image has no network built in (O8). A board whose network comes only from its image
would boot a published image with no network, fail its trial and roll back. That is safe, but it
would be confusing. So from O1 an image stores its built-in network in NVS the first time it
boots and finds none stored. `GET /firmware` reports where the network came from, and `ota.py`
refuses to push an image with no network to a board whose only network is built in.

**The P4's bootloader.** It sits at `0x2000`, below the partition table at `0x8000`: a
24,576-byte window. The "ESP32P4 firmware flash" session measured it on 2026-09-24 with
bootloader-only builds from `b49a966c` and the board's overlays:

| P4 bootloader | Bytes | Spare |
|---|---|---|
| As flashed today: log level Info, no rollback | 23,296 | 1,280 |
| Rollback, Info | 23,424 | 1,152 |
| Rollback, Warning | 20,896 | 3,680 |
| Rollback, Error | 20,608 | 3,968 |
| Rollback, Warning, signed apps (O7) | 20,992 | 3,584 |

Rollback costs 128 bytes, so it fits and the partition table stays where it is. O1 keeps the Info
level, because O2's board tests read the bootloader's lines about which slot it chose and why.
A board's bootloader changes only with a USB flash, so this margin matters only at build time. If
a later ESP-IDF grows the bootloader past the window, the build fails and says so, and
`CONFIG_BOOTLOADER_LOG_LEVEL_WARN` is the one-line fix. The S3 and C6 bootloaders start at `0x0`
in a 32,768-byte window, with 11,600 and 9,616 bytes spare.

Anything else that might one day need a partition has to be in this table before the boards
migrate, because a table change is a USB flash. That is why `coredump` and `reserve` are there
already.

## Flash mode

The user's direction: flashing is a mode of its own, in which nothing else runs. The board enters
flash mode on `PUT /firmware/mode` with body `flash`, or on the first `PUT /firmware`. Entering
it does this, in order:

1. The command goes through the queue app_main already reads, since app_main's task owns the
   player (`main/hearth_sink.cpp`). That task ends any play started with `POST /play`.
2. The Sendspin player sends `client/goodbye` with reason `restart` to every server connected (as
   `sendspin_host.cpp` already does when the board's configuration changes). It then stops, which
   frees its ring, its decode task and its WebSocket buffers. Clock sync stops with it.
3. The sink closes, and its I2S channels stop. Nothing goes to the DACs until the restart.
4. mDNS withdraws `_sendspin._tcp`, so servers stop dialling, and keeps the host name, so tools
   can still find the board.
5. `/status` reports `"state": "flash"`. That is a new value of an existing key, so the page's
   key-order contract is unaffected.

Three things keep running: the network (the Wi-Fi station, the SDIO link to the P4's
co-processor, or Ethernet), the HTTP server and mDNS's name.

Routes that would start playback or change a setting answer `409`, with a reply that says the
board is in flash mode:

- `POST /play` and `POST /pairing`;
- `PUT /layout`, `/slot-width`, `/wiring`, `/name` and `/network`.

Every `GET` still answers.

**Every way out is a restart.**

- An image written and checked restarts the board into that image.
- `PUT /firmware/mode` with body `normal` restarts it into the running image.
- So do ten minutes in flash mode with no upload in progress.
- A refused image leaves the board in flash mode, so a corrected one can be sent. The ten minutes
  start again from the refusal.

A restart is the only way out because it rebuilds everything in the one order boot already uses
and tests. A resume would have to bring the player, the Sendspin host and the sink back in an
order nothing else runs. The cost is the seconds a board takes to rejoin its network after an
update that was cancelled or refused.

**Memory.** The upload needs a task whose stack is in internal RAM (8 KiB to start with, measured
in O1). The flash cache is off while the task erases and writes, and PSRAM is reached through that
cache, so a stack there would be out of reach. It also needs a 4 KiB receive buffer. The teardown frees far more than that on every chip: the C6's Sendspin ring alone
is 48 KiB and its decode stack 24 KiB. So flash mode changes no memory setting. The C6's internal
low-water mark during an upload confirms it: 114,308 bytes free at the lowest, measured once O4
printed it ([Diagnostics](#diagnostics-without-a-cable-o4)).

**Flash writes and the cache.** An erase or a write disables the flash cache, in windows of up to
one 64 KiB block erase. Nothing time-critical is left running by then. On the C6, Wi-Fi's own code
runs from flash (its IRAM options are off), so Wi-Fi also pauses in those windows. TCP resends
whatever those windows delay.

## An update, on the board

1. `PUT /firmware` arrives on the HTTP server's task. It is refused at once, before any of the
   body is read, when:
   - an update is already running;
   - the running image is on trial;
   - there is no second slot (a board still on the old layout);
   - there is no `Content-Length`, or the length is more than the slot holds;
   - the `Content-Type` is not `application/octet-stream`.
2. The request goes to a firmware task (`httpd_req_async_handler_begin`, in ESP-IDF v6.1). The
   server stays free to answer `GET /firmware` while the upload runs.
3. The firmware task enters flash mode and waits for app_main to confirm the teardown.
4. It reads the first 288 bytes: the image header, the first segment's header and
   `esp_app_desc_t`. Nothing is erased until they pass these checks:
   - the magic numbers;
   - the chip ID is this chip's;
   - this chip's revision is within the image's minimum and maximum;
   - the project name is the running image's (`ac3forge_hearth_sink`);
   - the flash size in the header is the running image's.

   ESP-IDF checks the chip ID and revision again at the end
   (`bootloader_common_check_chip_validity`, called from `esp_image_verify`). Checking here
   refuses a wrong image before 1.5 MB are written, and the reply says which check failed. That
   matters on the P4: an image built without `sdkconfig.p4`'s revision settings needs v3.1 or
   newer, and this board is v1.3.
5. `esp_ota_begin` with the declared length erases what the image needs. Then 4 KiB reads go into
   `esp_ota_write`, and each read also goes into a running SHA-256 of the body. `GET /firmware`
   reports the progress. A stalled connection gives up after 30 s.
6. Three checks follow, the first and third reading the image back from flash
   ([Integrity](#integrity)):
   - `esp_ota_end` runs `esp_image_verify`: the header's checksum, the SHA-256 the build
     appended to the image, the chip ID, the revision range and the segment layout;
   - the SHA-256 of the body has to equal the request's `Content-Digest`, when there is one (the
     tool always sends one);
   - the SHA-256 of the bytes read back from the slot has to equal the SHA-256 of the body.

   Only then does `esp_ota_set_boot_partition` make the new slot the next boot.
7. The board replies `200` with the version written, waits about a second for the reply to leave,
   and calls `esp_restart()`.

Any failure calls `esp_ota_abort`, replies with the reason, records it for `GET /firmware`, and
leaves the board in flash mode. The slot is left with a partial image that nothing boots, because
`otadata` was never changed. The image that was in that slot has gone too, so after a failed
upload the running image carries on with nothing to roll back to. `PUT /firmware/rollback`
answers `409` until an update succeeds.

## The trial

The rollback bootloader marks a newly written slot `PENDING_VERIFY` the first time it boots it. If
the board resets while the slot is still in that state, the bootloader marks it `ABORTED` and
boots the other slot.

On the new image:

- It runs as normal: servers can connect and play. `GET /firmware` reports `"trial"` and the time
  left.
- **It is accepted** (`esp_ota_mark_app_valid_cancel_rollback`) once three things have held for
  30 s without a break:
  - the board holds a network address;
  - the HTTP server is running;
  - on a Sendspin build, the Sendspin player has started.
- **It is rolled back** (`esp_ota_mark_app_invalid_rollback_and_reboot`) if it is not accepted
  within 5 minutes. Before that it stores which condition never held.
- A panic, a watchdog reset, a brownout or a power cut before acceptance also rolls back, through
  the bootloader. A board that hangs during its trial can therefore be unplugged and plugged back
  in, and it comes back on the previous image.
- The trial runs on a task of its own, not in app_main's loop, so a stuck loop still rolls back.
  An `esp_timer` 30 s past the deadline restarts the board if that task has not acted, and a
  restart while on trial is itself a rollback.
- The task watchdog is left as the builds set it: it reports and does not panic
  (`CONFIG_ESP_TASK_WDT_PANIC` is off in every board build). A decode that keeps the idle task
  from running for 5 s makes it fire. That is a problem of load, not a broken image, and a trial
  that panicked on it would roll back a good image because of what a server happened to play.
- While the image is on trial:
  - `PUT /firmware` is refused. ESP-IDF refuses too: `esp_ota_begin` returns
    `ESP_ERR_OTA_ROLLBACK_INVALID_STATE`, because the other slot holds the image to fall back to.
  - `POST /restart` is refused, since a restart now is a rollback, and the reply says so.
  - `PUT /firmware/rollback` rolls back straight away.

The hold time and the deadline are Kconfig values (`AC3FORGE_FIRMWARE_TRIAL_HOLD_S` and
`_DEADLINE_S`), which the QEMU tests shorten.

**After a rollback**, the previous image reports it in `GET /firmware`'s `last_update`: the
version, `"rolled back"`, and why. The reason is what the failed image stored before it gave up,
or else the reset reason the previous image reads on its first boot back (`esp_reset_reason()`:
panic, task watchdog, brownout, power-on).

**What the trial cannot catch:**

- A fault that appears only after acceptance, such as a crash 20 minutes into a play. The
  previous image stays in the other slot until the next update, so `PUT /firmware/rollback` (or
  `ota.py rollback`) brings it back, as long as the network and the HTTP server still work. A
  later option is a crash-loop guard, which would roll back by itself after repeated panics
  shortly after boot. It is not in O1: it must never swap back and forth between two images that
  both fail.
- An image that runs well but cannot take the next update. Roll back to the previous image, which
  can; failing that, USB.
- Settings that a newer image writes in a form the older one cannot read. See
  [Settings survive a rollback](#settings-survive-a-rollback).

## Settings survive a rollback

Both images share one NVS partition. The Sendspin store keeps its pairing records as one blob and
reads it only if its length is exactly what it expects (`sendspin_store.cpp`). If a newer image
changed that blob's layout, a rollback would silently lose every pairing.

The rule from O1 on: a new image never changes the meaning or the layout of a key it did not add.
A new layout goes under a new key, and the old key stays readable. O1 adds a host test that loads
blobs written by the previous layout of each store.

## Integrity

The user's requirement: anyone on the network may flash a board, as anyone with a USB cable can,
but a damaged image must never run, whether it was damaged on disk, on the way to the board, on
the way into flash or while it sat in flash.

Every ESP-IDF app image carries a SHA-256 of itself, which the build appends: `hash_appended` is 1
in the S3, C6 and P4 images of 2026-09-24. The checks build on that:

| Where the damage happens | What catches it | Where |
|---|---|---|
| On disk: a truncated or changed build output | `ota.py` checks the file's own appended SHA-256 before it sends anything | the tool |
| On the network | The image's own SHA-256 (next row). Also, the tool sends a SHA-256 of the whole file (`Content-Digest: sha-256=:…:`, RFC 9530) and the board compares it with its hash of the body as it arrived | `PUT /firmware`, before the new slot can boot |
| On the way into flash | `esp_image_verify`, which `esp_ota_end` runs, reads the image back from flash and checks its checksum, its appended SHA-256, the chip ID, the revision range and the segment layout. The board then hashes the bytes read back from the slot, and they must equal the SHA-256 of the body | `PUT /firmware` |
| In flash, later | The bootloader checks the image's SHA-256 before every boot. If the slot it was going to boot fails, it tries the other slot (`bootloader_utility_load_boot_image`) | every boot |
| The fallback image, before it is needed | Once no trial is left to decide, the board reads both slots through in the background, a little at a time from its timer task, recomputes each image's SHA-256 and compares it with the one appended, as the bootloader does, and `GET /firmware` reports whether each is intact. It stops before an update writes anything. `esp_partition_get_sha256` would not do: it returns the appended digest without checking it | `GET /firmware`, `ota.py status` |

Two build settings would switch the boot-time check off: `CONFIG_BOOTLOADER_SKIP_VALIDATE_ON_POWER_ON`
and `_ALWAYS`. Both default to off. A new check, `tools/checks/check_esp_efuse_free.py`, fails CI
if any `sdkconfig` fragment under `esp-idf/` turns either on. The same check refuses the options
that burn eFuses: hardware secure boot, flash encryption, anti-rollback, and a disabled or secure
ROM download mode. Each of those would take away some way of recovering a board over USB.

`GET /firmware` reports each slot's SHA-256 as the board has it. After an update, `ota.py`
compares the running slot's digest with the file it sent. That proves the board runs exactly that
file, which a version string cannot do when two builds of one commit share it.

A `curl -T` upload carries no `Content-Digest`. The image's own SHA-256, checked from flash,
still catches any damage to the image itself. It does not cover bytes after the image's end, such
as padding or, later, a signature block. The file's digest does, which is why the tool always
sends it.

**What this does not stop.** Anyone who can reach the board can install any image built for its
chip, as they could with a USB cable. They can also restart it, or put it in flash mode (which
ends in a restart), as they can stop it today.

### Signing, later

When the boards move out of development, signed images are a phase of their own (O7). Everything
it needs is in ESP-IDF v6.1:

- `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` with the RSA-3072 scheme, which all three chips
  support (`SOC_SECURE_BOOT_V2_RSA`). No eFuses are burned.
- With hardware secure boot off, `esp_ota_end` takes its trusted keys from the signature blocks
  of the running image (`secure_boot_signatures_app.c`, "Take trusted digest key(s) from running
  app").
- The bootloader checks no signatures in this mode: `SECURE_SIGNED_ON_BOOT_NO_SECURE_BOOT` is
  only for the ESP32's V1 scheme.

**It can be switched on over the network, with no USB flash:**

1. A board running an image built without signing checks only integrity, so it accepts the first
   signed image as it would any other.
2. From then on, the running image's key is the one every update must be signed with.
3. An image can carry three signature blocks, so keys can be changed over the network too: sign
   the changeover image with both keys.

O7 proves the first step under QEMU before any board takes it.

**What it costs:**

- a private key kept outside every worktree, with an offline copy;
- one USB flash per board to replace the key if it is lost;
- a key-path guard in the example's CMakeLists;
- a throwaway key in CI's QEMU job.

It also stops what the table above allows, because a board would then refuse any image not
signed with its key. That includes one sent by a hostile web page ([Routes](#routes)).

## Routes

| Route | What it does | Replies |
|---|---|---|
| `GET /firmware` | Mode; each slot's version, ELF SHA-256, image SHA-256, state, and whether its image is intact; the trial's progress; the last update and how it ended; an upload's progress; slot size, flash size and the partition table as the board has it; the bootloader's version; whether the board's network is stored or built into its image | `200`, JSON |
| `PUT /firmware` | Body: an app image (`ac3forge_hearth_sink.bin`, not the merged image), with an optional `Content-Digest`. Enters flash mode, writes the other slot, checks it, restarts into it | `200` then a restart; `400` not an app image, the wrong chip, a revision this chip does not meet, cut short, or damaged (a SHA-256 does not match, and the reply says which); `403` the `Host` is not one of the board's own names ([decision 13](#decisions)); `409` on trial, or an update already running; `411` no length; `413` larger than the slot; `415` a `Content-Type` other than `application/octet-stream` (none at all is fine: `curl -T` sends none) |
| `PUT /firmware/mode` | Body: `flash` enters flash mode; `normal` leaves it with a restart into the running image, and outside flash mode does nothing | `200`; `409` on trial |
| `PUT /firmware/rollback` | Makes the other slot's image, if it is valid, the next to boot, and restarts into it, on trial as an update's image is. On trial, gives up the trial instead | `200`; `409` nothing valid to roll back to |
| `POST /restart` | Restarts into the running image | `200`; `409` on trial, where a restart would roll back |
| `GET /firmware/coredump` | The core dump the last crash left, as it lies in flash (O4) | `200`, `application/octet-stream`; `404` none, or a build that keeps none |
| `DELETE /firmware/coredump` | Erases it | `200`; `403` the `Host` is not the board's; `409` an update is under way |
| `GET /log` | The console's recent output, oldest first; `?from=N` for what was written since byte N, with `X-Log-From` and `X-Log-Next` saying where the text starts and where to ask next (O4) | `200`, text; `404` a build that keeps no log |

`curl -T build/ac3forge_hearth_sink.bin http://hearth-eb2c64.local/firmware` is a whole update,
since `curl -T` sends a PUT.

Three routes are PUT for the reason [the device UI plan](esp32-device-ui.md#security) gives: a
page on another site cannot send a PUT without a preflight, and the board answers none (Control
sends no CORS headers). A POST with a text body needs no preflight. So `PUT /firmware/rollback`
reads oddly as a verb, but it cannot be sent cross-site. `POST /restart` is a POST like
`POST /stop`: sent cross-site it restarts the board, and nothing persistent changes.

A page on the internet can still get past that by DNS rebinding: it points its own host name at
the board's address, so its PUTs count as same-origin. While images are unsigned, nothing else
would stop such a page installing an image, so the three firmware PUTs check the `Host` header
([decision 13](#decisions)). They accept:

- an IP address;
- the board's own mDNS name, bare or with `.local`.

A rebinding page's requests carry its own host name and are refused with `403`. Browsers
increasingly block these requests themselves (Chrome asks before a public site reaches the local
network), and the check covers the rest. The cost: the page's **Update firmware…** works only
when the page was opened by IP address or `.local` name, not by a name a router hands out. Every
other route is unchanged.

These routes are for the board's own network. A board must never be reachable from the internet;
to update from outside the house, use a VPN into the network.

## The host tool

`tools/hearth/ota.py`, Python 3 standard library only. `zeroconf` is optional and used only by
`--all`.

```
ota.py push   (--build-dir DIR | IMAGE) (--host H [--host H ...] | --all) [--yes] [--force]
ota.py status [--host H ... | --all]
ota.py restart  --host H
ota.py rollback --host H
ota.py cancel   --host H          # leave flash mode: restart into the running image
```

**`push`**, for each board in turn:

1. **Read the image.** From a build directory, `project_description.json` gives the app binary,
   the target and the version, and `partition_table/partition-table.bin` the layout. From the
   image itself: the chip ID, the revision range, the flash size, and `esp_app_desc_t` (project,
   version, ELF SHA-256). The tool checks the image's own appended SHA-256, which refuses a
   damaged file before anything is sent, and takes a SHA-256 of the whole file.
2. **Pre-flight**, from `GET /hardware` and `GET /firmware`. It refuses when:
   - the target or chip differs;
   - the chip's revision is outside the image's range;
   - the layout differs from the build's (an update cannot change it: "this needs one USB
     flash");
   - the image does not fit the slot;
   - the flash size differs;
   - the running image is on trial;
   - the board's only network is built into its image, and the new image has none
     ([Flash layout](#flash-layout));
   - the board already runs this image (skipped unless `--force`).

   A board that is playing is asked about first, and `--yes` answers for it.
3. **Upload.** `PUT /firmware` with the file's `Content-Digest`, and a progress line.
4. **Wait.** Poll `GET /firmware` at the address the board had, for up to 6 minutes, until:
   - the new image is running and accepted, and the running slot's SHA-256 equals the file's:
     **updated**;
   - the old image is running, with `last_update` saying why: **rolled back**;
   - nothing answers: **did not come back**. The tool then says what to try. A board that hangs
     during its trial comes back on the previous image if its power is cycled. After that, USB:
     the same `write-flash @flash_args` as today.

`--all` finds boards by their `_sendspin._tcp` records. It updates one board at a time and stops
at the first board that rolls back or does not come back, before touching the others
([decision 11](#decisions)). A build without the Sendspin player advertises no service, so its
boards have to be named with `--host`.

**`idf.py ota`.** An `idf_ext.py` in the example adds an `ota` action (ESP-IDF v6.1 loads a
project's `idf_ext.py`). `idf.py -C <example> -B <build dir> ... build ota --host hearth-eb2c64.local`
builds and then pushes, as `build flash` does today with a port.

## The web page (O3)

The page's firmware tiles already show `/hardware`'s version. A Firmware section adds:

- the running slot, the other slot's version and state, and the trial's countdown;
- how the last update ended;
- **Update firmware…**, a file input whose upload shows the bytes sent;
- **Restart**, and **Roll back to** the other slot's version, each behind a dialog like the one
  for forgetting servers.

After an upload the page polls `GET /hardware` until the board answers with the new version, then
reloads. Pages are already served `Cache-Control: no-cache`, so the page that reloads is the new
image's. The device-UI suite gains the routes in `stub.js` and in `contract.spec.js`'s route
check. The page budget (45,056 bytes, 42,846 used) is derived again, as each redesign did.

**Built 2026-09-25**, as [the device page's plan](esp32-device-ui.md#firmware) describes, with
three changes to the sketch:

- The page reads `GET /firmware` rather than `GET /hardware` for what the board runs. When the
  board answers again running another image than the page was loaded with, the page loads
  again, whoever made the update.
- It reads the image's head before sending, and keeps back a file the board would refuse on it
  alone. An upload enters flash mode before the board reads a byte, so a wrong file would stop
  what plays for nothing.
- It sends no `Content-Digest`. A page on plain HTTP has no `crypto.subtle`, and the board
  checks the image's own SHA-256 and reads back what it wrote. The device page's decision 29
  has the reasoning.

The budget is 57,344 bytes, against 55,454 used.

## Diagnostics without a cable (O4)

A board updated over its network is usually a board with no cable on it. When one panics, or
does something odd, its console is where the cause is written, and nobody is reading it. O4 keeps
two things the console would have shown, where the network can reach them.

**The last crash.** A panic writes a core dump to the `coredump` partition, which O1 put in both
tables for this ([decision 8](#decisions)). The board keeps it through the restart that follows,
and through a rollback: the image that goes back reads the dump the failed image wrote.

- `GET /firmware` says whether there is one: its size, the task that crashed and where, and
  which image wrote it, by the ELF SHA-256 the dump carries. That image is usually one of the
  two slots.
- `GET /firmware/coredump` sends the dump as it lies in the partition.
- `DELETE /firmware/coredump` erases it, so the next crash is not mistaken for this one.
- `ota.py coredump --host H` saves it to a file. With `--elf`, the ELF of the image that wrote
  it, it runs ESP-IDF's `esp_coredump info_corefile` on it: every task's backtrace, which
  `idf.py coredump-info` would show at the desk.

**Recent console lines.** A ring of the console's last few kilobytes, in RAM:

- `GET /log` sends it as text, oldest first. `GET /log?from=N` sends only what was written since
  byte N, and each reply says where the next read starts, in `X-Log-Next`. So a client can
  follow the console the way a terminal would.
- `ota.py log --host H` prints it, and `--follow` keeps printing what is new.

**How the console is kept.** ESP-IDF has no public way to add an output to its console, and
picolibc, its C library in v6.1, has one `stdout` for every task. So the component reopens
`stdout` and `stderr` on a device of its own, `/dev/ac3log`, whose writes go on to
`/dev/console` as before and into the ring as well ([decision 17](#decisions)). That covers a
`printf` from any task, and the `ESP_LOGx` that reach `stdout`. It does not cover:

- what was printed before `log_start`, first thing in `app_main`;
- what `esp_rom_printf` writes, which includes a panic's registers and backtrace. The core dump is
  the record of a crash.

**What the network does not get.** The console prints the Sendspin pairing token, which pairs a
server with no code. It is printed for whoever holds the board, and the page and `/status` never
carry it. `GET /log` is for anyone on the network, so the token's line is printed inside a
`ConsoleOnly` scope, which keeps that task's writes out of the ring ([decision 18](#decisions)).
Improv's packets are kept out the same way: they are binary, not lines.

A core dump holds what was on each task's stack when the board crashed. That can include key
material a task was working with. The network is already the boundary for everything else this
API does ([Routes](#routes)), and it is for this too. `DELETE /firmware/coredump` takes the same
`Host` check as the firmware PUTs.

**What each board keeps** ([decision 16](#decisions)):

| Build | Core dump | Its static internal SRAM | Console ring |
|---|---|---|---|
| S3 board (`sdkconfig.psram`) | off | 4,016 bytes | 16 KiB, in PSRAM |
| C6 | on | 1,140 bytes | 2 KiB, internal |
| P4 | on | 3,652 bytes, of 418 KiB left | 16 KiB, in PSRAM |
| CI's update test (`sdkconfig.ci-ota`) | on | 2,128 bytes | 2 KiB, internal |
| CI's 7.1.4 stream set (`sdkconfig.ci-http714`) | off | 2,128 bytes | none |

Each cost is `idf.py size`'s, against the same build with `CONFIG_ESP_COREDUMP_ENABLE_TO_NONE`
(2026-09-25). A build whose task stacks may be in PSRAM, the S3 board and the P4, costs more:
ESP-IDF gives the dump a stack of its own in internal SRAM (`ESP_COREDUMP_USE_STACK_SIZE`, 1,792
bytes at the least). A JOC stream has left the S3 board 43 bytes of internal SRAM (hearth_sink's
README). A crash there still says it panicked, in `GET /firmware`'s last update, and the ring
keeps what the console said before it.

The core dump's code adds 16 to 17 KB to an image. The C6's on the 4 MB table, the tightest, is
now 1,665,856 bytes, which leaves 169,152 (9%) of its 1.75 MiB slot.

**The upload's least free heap.** O2 asks for the C6's internal heap low-water mark during an
upload. The upload now measures it from the moment flash mode has stopped the player to the
upload's end, and prints it (`firmware: the upload's least free internal heap was N bytes`). The
line comes just before the restart into the new image, so it reaches the USB console. After an
update that went through, the ring in RAM is gone with the restart. Measured on 2026-09-25, each
board taking a whole image from O4's image:

| Board | Internal heap free as flash mode starts | Least free during the upload |
|---|---|---|
| C6 (COM9) | 135,920 bytes, largest block 86,016 | 114,308 bytes |
| S3 board (COM15) | 94,335 bytes, largest block 31,744 | 89,415 bytes |

Neither comes near running out during an upload. The S3 board is the tighter of the two, and
still keeps 89 KB free.

## ac3hearth (O5)

The desktop app already finds sinks by mDNS and has a settings page for each one. Its firmware
panel:

- shows each sink's version and whether it matches the build the app knows of;
- offers **Update** from a chosen file, using the same routes;
- reports the trial as the tool does.

Shipping sink images inside the app's release packages, once they are signed (O7), would need a
release key held by CI, which means a secret only the user can set. That is its own decision,
taken when it comes up.

## Published images

CI builds the desktop packages for each platform, and `release.yml` publishes them. If boards are
to be updated over the network, their firmware should come the same way, so that updating a board
does not need an ESP-IDF install. That is O8.

**What CI builds today.** In `_build.yml`, the S3's Sendspin board configuration and the C6's are
compiled, then thrown away: the S3's with `rm -rf build`, and the C6's in `$RUNNER_TEMP`. Neither
is uploaded. CI builds the P4's bare-metal probe, not `hearth_sink` for the P4. Every image CI
runs under QEMU is a CI shape (`sdkconfig.ci-*`), not a board's.

**What O8 builds.** One image for each board configuration the guides describe, from the same
overlay lists the board recipes use:

| Image | Overlays after `sdkconfig.defaults` | Table | For |
|---|---|---|---|
| `hearth-sink-esp32s3` | `hw`, `psram`, `sendspin` | 16 MB | an S3 with 16 MB of flash and 8 MB of octal PSRAM, such as the DevKitC-1 N16R8 |
| `hearth-sink-esp32c6` | `hw`, `sendspin`, `c6`, `sendspin-c6` | 4 MB | any C6 module |
| `hearth-sink-esp32c6-16mb` | the same, then `flash16mb` | 16 MB | a C6 with 16 MB of flash, such as the board on COM9 |
| `hearth-sink-esp32p4-rev1` | `hw`, `p4`, `sendspin` | 16 MB | a P4 of silicon revision v1.x, such as the FireBeetle 2 on COM10 |

A P4 of revision v3.x needs a build without `sdkconfig.p4`'s revision settings. It is left out
until there is such a board to run it on ([decision 14](#decisions)).

No published image has a network built in: a board gets its network from Improv or from the page,
and keeps it in NVS. CI refuses to publish an image whose `sdkconfig` sets
`CONFIG_AC3FORGE_EXAMPLE_WIFI_SSID` or `_PASSWORD`.

**What each image publishes:**

- `<image>-<version>.bin`: the app image, for updates over the network.
- `<image>-<version>-factory.bin`: bootloader, partition table, empty `otadata`, app, `audio` and
  `storage` merged into one file, written at `0x0`. It is for a new board: it also overwrites NVS.
- `<image>-<version>-parts.zip`: the same pieces as separate files, with a `flash_args` of
  relative paths. This is how a board already in use moves to this layout with its NVS kept
  ([Flash layout](#flash-layout)). The build directory's own `flash_args` names `audio`'s source
  by absolute path, so it cannot be shipped as it is.
- `<image>-<version>-elf.zip`: the ELF, so a backtrace or a core dump (O4) from a published image
  can be read.
- `hearth-sink-manifest.json`: every image of the release, with its chip, table, revision range
  and SHA-256, which `ota.py` reads to choose an image for each board.

**When.**

- **On every CI run** that builds the ESP lane: the images are uploaded as a workflow artifact
  kept for 14 days. `ota.py push --run <run id>` downloads one with `gh run download`, so a PR's
  firmware can go onto a board with no local build.
- **At a release:** `_build.yml` uploads them as `packages-esp32-firmware`, which the
  `github-release` job already collects (`pattern: packages-*`). The images then get what every
  release asset gets: `SHA512SUMS`, the GPG signature when the key is provisioned, the SBOM and
  a build provenance attestation (`gh attestation verify`). The "Verify every documented package
  was built" step gains a line for each image, and `docs/releasing.md`'s "What gets published"
  lists them.

A check beside the others (`tools/ci/check_firmware_package.py`, after
`check_hearth_package.py`) opens each image before it is uploaded. It checks:

- the chip ID and revision range are the right ones for its name;
- `hash_appended` is set and the image's SHA-256 matches;
- the image fits the smallest slot of its table;
- the `parts.zip` flashes the same bytes as the factory image;
- no network is built in.

**Choosing the image for a board.** `ota.py push --release <tag|latest>` reads the release's
manifest and each board's `/hardware` and `/firmware`: chip, revision, flash size, PSRAM and
partition table. It downloads the image that matches and checks its SHA-256 against the
manifest's and against `SHA512SUMS`. A board that no image fits is named and skipped.
`--release latest --all` updates every board on the network to the newest release, one board at
a time.

**Signing, when O7 comes.** A published image has to be signed with the key the boards trust. That
means a release key held by CI, a secret only the user can set, or signing on the maintainer's
machine before upload. O7 decides which.

## The user guide

O9 writes `docs/hearth/sink-firmware.md`, for someone who has a board and a release, and no
ESP-IDF install:

1. **Which image.** The table above, and how to tell a board apart: `esptool chip-id` and
   `esptool flash-id` over USB, or `/hardware` on a board already on the network.
2. **Checking a download.** `SHA512SUMS`, `gh attestation verify`, and the GPG signature when
   the release has one.
3. **A new board.** The browser installer or `esptool write-flash 0x0 <image>-factory.bin`
   ([decision 15](#decisions)), then joining a network over Improv, as the S3 guide already
   describes.
4. **A board running an older build.** The browser installer with erasing left off, or the
   `parts.zip` and `write-flash @flash_args`. Either keeps the board's name, network and
   pairings.
5. **Updating over the network.** `ota.py push --release latest --all`, the page's **Update
   firmware…**, or `curl -T`.
6. **What the board does** during an update: flash mode, the trial and a rollback, and what
   `ota.py` and the page show for each.
7. **Going back.** **Roll back** on the page, `ota.py rollback`, or an older release pushed as
   any other.
8. **When a board does not come back.** Cycle the power during a trial; failing that, USB, with
   the browser installer or the release's `parts.zip`. If the board does not answer on USB, hold
   BOOT while pressing RESET to put it in the ROM's download mode, which is always there.

O9 also brings the rest of the documentation into line:

- `docs/hearth/sink-esp32-s3.md`'s "Build and flash" offers the published image first and
  building it second;
- `docs/hearth/index.md` links the guide, and `mkdocs.yml` lists it;
- `docs/releasing.md` covers the firmware in its release checklist.

**The browser installer** ([decision 15](#decisions)) is a page on the documentation site built
on ESP Web Tools. Its supported chips include the ESP32-S3, ESP32-C6 and ESP32-P4.

- **How it flashes.** Over Web Serial on the board's USB serial connector, talking to the chip's
  ROM download mode, as `esptool` does. It resets a board into download mode by itself where the
  board allows that. A board that does not can be put into download mode by hand, by holding
  BOOT while pressing RESET, and the installer then talks to the ROM directly. Such a board is
  one whose firmware no longer answers on USB, or one that does not reset cleanly, like COM16.
  The ROM cannot be overwritten, so the same page recovers a board that will not boot.
- **After flashing.** A board put into download mode by hand may need one press of RESET to start
  the new firmware. The page then offers Improv, which the boards already answer, to give the board
  its network. A first install needs a browser and a USB cable, and nothing else.
- **Its manifest lists the pieces, not the merged factory image**: bootloader, partition table,
  `otadata`, app, `audio` and `storage`, each at its offset. It sets
  `new_install_prompt_erase: true`, so the person installing chooses whether to erase. A new board
  is erased; a board already in use is left unerased, so its NVS survives and the same page moves
  it to the new layout.
- **Browsers:** Chrome, Edge and Firefox, which ESP Web Tools lists as having Web Serial; not
  Safari, and nothing on iOS.
- **Hosting.** The page cannot fetch GitHub release assets, which carry no CORS headers, so
  `docs.yml` copies the latest release's images and manifest into the site when it deploys.
- **On the P4,** the installer has to be on the USB-C connector that carries the console. The
  board's other connector is a separate USB peripheral.

## What stays USB-only, and how a board is recovered

**USB only:**

- the migration to the new layout;
- any later change to the partition table, the flash size setting or the bootloader, which
  includes an ESP-IDF upgrade that changes the bootloader;
- the P4's co-processor firmware, until O6 ([decision 9](#decisions)).

Signing does not need USB: [Signing, later](#signing-later) switches it on over the network.

ESP-IDF v6.1 can update the bootloader and the partition table over the network, through a
staging partition and a final copy (`esp_ota_set_final_partition`). This plan does not use that
([decision 6](#decisions)): a power cut during the copy leaves a board with nothing to boot.

**Recovery, from least to most effort:**

1. The trial rolls back by itself.
2. A power cycle during a hung trial rolls back.
3. `PUT /firmware/rollback` for a fault that appears after acceptance.
4. USB: `write-flash @flash_args`, as today, or from O9 the browser installer. Both talk to the
   ROM download mode, which is in mask ROM, and this plan burns no eFuse that could lock it, so a
   board can always be recovered this way. A board that does not reset into it by itself is put
   there by holding BOOT while pressing RESET. The P4 needs its cable working for this, which it
   does not have today.

## Per chip

**ESP32-S3.**

- The 16 MB table.
- The only chip with a QEMU machine, so CI's end-to-end OTA tests run here, over the emulated
  Ethernet.
- COM16 needs `--after watchdog-reset` for its migration flash: `esptool`'s default hard reset
  leaves that board in download mode (the `i2s_player` README records it).

**ESP32-C6.**

- The 16 MB table on COM9, through `sdkconfig.flash16mb`; the 4 MB table for other modules.
- No PSRAM, and Wi-Fi's code in flash. Flash mode's teardown matters most here, and O2 measures
  it: the internal heap's low-water mark during an upload, and whether Wi-Fi keeps its association
  through the erase windows. It did through every upload, and the low-water mark was 114,308
  bytes.
- No QEMU machine, so board-only.

**ESP32-P4.**

- The 16 MB table.
- The tightest bootloader of the three: 1,152 bytes spare with rollback at log level Info
  ([Flash layout](#flash-layout)).
- Rev v1.3: images must come from `sdkconfig.p4` (`ESP32P4_SELECTS_REV_LESS_V3`,
  `REV_MIN_100`). A default P4 image needs v3.1 and is refused before anything is written. O2
  pushes one on purpose to prove it. The other direction holds too: this board's images accept
  revisions v1.0 to v1.99 (`min_rev_full` 100, `max_rev_full` 199 in the image of 2026-09-24), so
  a production v3.x P4 refuses them.
- Wi-Fi through the co-processor, which stays up in flash mode.
- The co-processor's own firmware (boot log: "Version mismatch: Host [2.12.0] > Co-proc [0.0.0]")
  is a separate flash target. `esp_hosted` ships a host-performs-slave-OTA example for it, which
  this plan leaves to decision 9.
- No QEMU machine, and no USB until its cable is fixed; its migration waits for that.

## Tests

**On the host.** The logic that needs no ESP-IDF goes in `esp-idf/ac3forge/include/ac3forge/firmware_image.hpp`,
the way `hardware_info.hpp` is kept free of it:

- parsing an image header;
- every pre-write refusal and its reply text;
- parsing `Content-Digest`, and the reply when a SHA-256 does not match;
- the `Host` check: IP addresses and the board's own names pass, anything else is refused;
- the trial's decision from what has held and for how long, including a network that drops and
  comes back, which restarts the 30 s;
- `GET /firmware`'s JSON.

`tests/io/test_firmware_image.cpp` checks all of it from synthetic headers, including a P4 image
that needs v3.1 on a v1.3 chip. The settings rule has its test of old blobs.
`tools/hearth/test_ota.py` runs the tool against a stand-in board built on `http.server`: the
pre-flight refusals, a damaged file refused before sending, an update that is accepted, one that
rolls back, one that does not come back, and `--all` stopping at the first failure.

**Under QEMU** (S3, in the ESP32 job). The job builds image A and image B from the same tree with
different versions. It boots A, then:

1. pushes B with `ota.py`: B restarts, is accepted, A is in the other slot, and `GET /firmware`
   reports both images intact;
2. pushes B with one byte changed: `400` (its SHA-256), and A keeps running;
3. pushes B with a `Content-Digest` that does not match: `400`, and A keeps running;
4. pushes a C6 image: `400` before anything is written;
5. cuts an upload short: refused, and the next full upload is accepted;
6. pushes an image built with `AC3FORGE_FIRMWARE_TEST=unhealthy`, which never reports healthy:
   it rolls back at the (shortened) deadline, and `last_update` says why;
7. pushes one built with `AC3FORGE_FIRMWARE_TEST=panic`, which panics as its trial starts: the
   bootloader rolls it back;
8. damages the running slot's image in the flash file between two boots: the bootloader boots
   the other slot;
9. sends `PUT /firmware/rollback`, then `POST /restart`;
10. sends a firmware PUT with a `Host` that is not the board's: `403`.

`AC3FORGE_FIRMWARE_TEST` is a CMake variable, not a Kconfig option, and so is `PROJECT_VER` for
image B. Each variant is then a rebuild of A's build directory that recompiles a file or two, so
the step costs one full build.

QEMU writes to the flash image it was given, and a new QEMU started on that file boots what was
written. It does not discard the code it has already translated from flash when the flash is
written, though: an image written over one that ran in the same QEMU runs the old image's code
after an `esp_restart()`, while reporting its own version. The test therefore starts a new QEMU on
the same flash file for each image an update writes, as a power cycle would. Rollbacks, panics
and `POST /restart` stay in one QEMU, which keeps the reset reason a panic leaves.

**On boards** (O2): each exit in [Phases](#phases), on each chip.

## Phases

Nothing reaches a board until O1 has merged in full, because each board gets one USB flash and
that image has to be able to take the next update.

- **O1, in the repository only.** Split into PRs that merge before any board migrates:
  - (a) the layouts, the rollback bootloader, `sdkconfig.flash16mb` and
    `check_esp_efuse_free.py`, with CI's QEMU shapes passing on the new table and the README's
    offsets updated;
  - (b) `ac3forge::Firmware` beside `Control` in the component, flash mode through the example's
    hooks, the integrity checks, the `Host` check, the trial, a built-in network stored in NVS at
    first boot, and the host tests;
  - (c) the QEMU end-to-end tests;
  - (d) `ota.py`, its tests and `idf.py ota`.

  **Exit:** CI green, the QEMU job included; no board touched.
- **O2, boards.** One USB migration flash each: S3 COM15 and COM16, C6 COM9, and the P4 when its
  cable works. **Exit, on each chip:**
  - name, network and pairings survive the migration (for example "1 pairing record(s)" at boot,
    and the name in `/status`);
  - a build pushed over Wi-Fi is accepted, and a Sendspin server reconnects by itself;
  - a damaged image is refused, and the board keeps running what it had;
  - an image that panics at boot comes back on the previous version by itself;
  - a power cut during a trial comes back on the previous version;
  - `PUT /firmware/rollback` works;
  - an update of a playing board stops the play and says goodbye to its server;
  - the time each step takes;
  - the C6's internal heap low-water mark during an upload (114,308 bytes, read with O4's line);
  - on the P4, a v3.1 image is refused before anything is written.
- **O3.** The page's Firmware section ([built](#the-web-page-o3)).
- **O4, diagnostics without a cable.** Core dumps to the `coredump` partition, fetched with
  `GET /firmware/coredump` and read with `idf.py coredump-info`. Also a ring of recent console
  lines at `GET /log`, since flashing without a cable also means reading the console without one.
  [Built](#diagnostics-without-a-cable-o4).
- **O5.** The firmware panel in `ac3hearth`.
- **O6.** The P4's co-processor firmware, as a study first ([decision 9](#decisions)).
- **O7, when the boards leave development.** Signed images, switched on over the network
  ([Signing, later](#signing-later)).

The last two phases come once the ones above are done. O7 has no fixed place: it happens when the
boards leave development, and if that is before O8, O8's images are published signed.

- **O8, published images.** Everything in [Published images](#published-images):
  - the four images built on every run of the ESP lane and kept for 14 days;
  - the same images published with each release;
  - `check_firmware_package.py`;
  - `ota.py push --run` and `--release`.

  **Exit:**
  - `ota.py push --release` puts a release's images on each board on the desk over the network,
    each board getting the image that fits it;
  - a factory image installs a blank board;
  - a dry run of `release.yml` finds every image it is documented to publish.
- **O9, the user guide.** [The user guide](#the-user-guide), the browser installer if decision 15
  takes it, and the other pages brought into line. **Exit:** someone with only the guide takes a
  blank board to one that plays, then updates it over the network to a newer release.

## What cannot be verified

- **Wi-Fi under CI.** QEMU has no Wi-Fi, so CI's uploads go over the emulated Ethernet. Wi-Fi
  uploads are tested on boards only.
- **The C6 and the P4 in CI.** Neither has a QEMU machine, so CI builds them and cannot run them.
- **Power lost at an exact instant**, such as during the write of `otadata`. The protection there
  is ESP-IDF's: `otadata` keeps two sectors, each with a sequence number and a CRC. Power can be
  pulled by hand during an upload and during a trial, but not at a chosen microsecond.
- **Every way an image can fail after it is accepted.** The trial covers 30 s of the board being
  healthy. A fault that takes longer to show is found in use, and rolled back by hand.
- **Boards other than the ones on the desk.** A published image is checked on one board of its
  kind. Another module with the same chip, flash and PSRAM should run it; one that differs in any
  of them is what the guide's "Which image" section and `ota.py`'s choice exist to catch.
- **The browser installer in every browser.** ESP Web Tools lists Chrome, Edge and Firefox as
  having Web Serial; O9 tries it in each. There is no Web Serial in Safari or on iOS, and the
  guide's `esptool` commands are the way in there.

## Decisions

1. **Update scheme.** (a) **two slots and a rollback bootloader**; (b) a small factory recovery
   app plus one update slot; (c) one slot and a staging area. **Recommend (a).** It is the only
   one of the three that always leaves a whole, working image on the board. (b) needs a second
   application to write and keep working, and (c) copies over the only image. Cost: two full
   slots of flash, easy on 16 MB and 1.75 MiB each on a 4 MB C6.
2. **What an update must prove about itself.** (a) **signed with the user's key: RSA-3072, no
   eFuses, the running image's key as the anchor**; (b) a password for the upload route, set when
   the board is provisioned; (c) nothing, the network is the boundary as it is for the rest of
   the API. **Recommend (a).** It is the only one that stops someone else on the network running
   their own code on the board. A password crosses the network in the clear and sits in every
   tool and page that uses it. Cost: a private key to keep safe and back up, since losing it
   means a USB flash per board; every board build needs the key; and images built by CI cannot
   be pushed to the boards on the desk. **Taken 2026-09-24: (c) while the boards are in
   development, with every image checked for damage end to end ([Integrity](#integrity)).** The
   user's words: "anyone on the wifi can flash these boards (since they support usb flashing too)
   but we should have a mechanism to validate the firmware is not corrupted either in flash or on
   transfer to flash". (a) becomes O7, switched on over the network when the boards leave
   development.
3. **Push or pull.** (a) **push: the tool, the page or curl sends the image with a PUT**; (b)
   pull: the board fetches a URL (`esp_https_ota`); (c) both. **Recommend (a).** It needs no
   server, no certificates and no TLS memory on the C6. Pull can come later for a fleet, or for
   updates from a release server. Cost: every update starts from a machine on the same network.
4. **How flash mode ends.** (a) **always with a restart**; (b) a resume in place when an update
   is cancelled or refused. **Recommend (a),** for the reasons in [Flash mode](#flash-mode).
   Cost: a board rejoins its network after a cancelled or refused update.
5. **When a new image is accepted.** (a) **the board decides: an address, the HTTP server and
   the Sendspin player, held for 30 s, within 5 minutes**; (b) the tool must confirm it
   (`PUT /firmware/accept`) within the deadline; (c) at once. **Recommend (a).** Unlike (b), it
   does not roll back a good image because a laptop went to sleep or the page was closed; unlike
   (c), it catches an image that crashes at boot or cannot rejoin the network. Cost: a Wi-Fi
   outage during those 5 minutes rolls back a good image, which then has to be pushed again.
   **Taken 2026-09-24: (a).**
6. **The bootloader and the partition table over the network.** (a) **never**; (b) with
   ESP-IDF's staging copy. **Recommend (a).** Neither has a second copy, so a power cut during
   the copy leaves nothing that boots. Cost: those changes need one USB flash per board.
7. **Which table the 16 MB C6 uses.** (a) **the 16 MB table through `sdkconfig.flash16mb`**; (b)
   the 4 MB table on every C6. **Recommend (a).** Slots of 4 MiB, against a 1.75 MiB slot that is
   86% full today. Cost: one more overlay line in the C6's board recipe.
8. **Partitions reserved before migrating.** (a) **`coredump` (64 KiB) on both tables, and a
   4 MiB `reserve` on the 16 MB table**; (b) only what O1 uses. **Recommend (a).** The table is
   USB-only after the migration. Cost: flash nothing uses yet, on boards with room to spare.
9. **The P4's co-processor firmware.** (a) **a later phase of its own (O6)**; (b) part of this
   work. **Recommend (a).** If its update fails, the P4 has no network at all, and on this board
   the co-processor may have no USB path to recover it. That risk needs its own study of
   `esp_hosted`'s example. Cost: the version mismatch in the P4's boot log stays for now.
   **Taken 2026-09-24: (a).** If O6 finds it needs a partition to stage the co-processor's image
   in, `reserve` is sized for that, so O6 needs no USB flash of its own.
10. **The tool.** (a) **Python under `tools/hearth/`, plus `idf.py ota`**; (b) a command in
    `ac3cli`; (c) only the desktop app. **Recommend (a).** Board builds already run in the
    ESP-IDF Python environment, and `idf.py ota` fits the recipe in use. Cost: a second client
    of the routes to keep in step, beside the page.
11. **Several boards.** (a) **one at a time, stopping at the first rollback or silence**; (b) in
    parallel. **Recommend (a).** The first board is the test of the image for the rest. Cost: a
    minute or so per board, most of it the trial.
12. **Where the code lives.** (a) **the component (`firmware.cpp` beside `control.cpp`), with the
    example supplying flash mode's teardown and the trial's health checks as hooks**; (b) the
    example only. **Recommend (a).** Control's routes are the component's
    ([device UI plan, decision 5](esp32-device-ui.md#decisions)), and an integrator's firmware
    gets the same updates. Cost: hooks in `ControlHandlers` for what only the owner knows.
13. **The `Host` header on the firmware PUTs, while images are unsigned.** (a) **accept only an IP
    address or the board's own mDNS name, bare or with `.local`**; (b) accept any `Host`, as every
    other route does. **Recommend (a).** Without signing, it is what stops a web page on the
    internet from flashing a board through the viewer's browser by DNS rebinding
    ([Routes](#routes)). Cost: the page's firmware upload works only when the page was opened by
    IP address or `.local` name. O7 makes the check unnecessary, and it can then go.
14. **Which images CI publishes.** (a) **the four in [Published images](#published-images): the
    S3, the C6 at 4 MB and at 16 MB, and the P4 of revision v1.x**; (b) those four and a P4 image
    for revision v3.x as well. **Recommend (a).** Each of the four runs on a board on the desk and
    is checked there in O8. A v3.x image would be published before anything had run it. Cost: a
    v3.x P4 has no published image until someone has such a board.
15. **How a new board gets its first image.** (a) **a browser installer on the documentation
    site (ESP Web Tools), with the `esptool` commands beside it**; (b) the `esptool` commands
    only. **Recommend (a).** A first install then needs only a browser and a cable, and it ends
    in Improv, which gives the board its network in the same few minutes. The same page moves a
    board in use to the new layout without erasing it, and recovers one held in download mode.
    Cost: a third-party script on one page of the site; a browser with Web Serial (not Safari or
    iOS); and `docs.yml` copying each release's images into the site, because a page cannot fetch
    release assets directly.
16. **Which boards keep a core dump** ([Diagnostics](#diagnostics-without-a-cable-o4)). (a)
    **every build, except the S3 board and the widest CI shape, where internal SRAM is what runs
    out**; (b) every build; (c) none, and a crash read over USB. **Recommend (a).** The C6 and
    the P4 have internal SRAM to spare, and the S3 board has none: 4,016 bytes, against a JOC
    stream that left 43. Cost: a crash on the S3 board says it panicked and no more; reading
    one takes a USB cable and ESP-IDF's monitor.
17. **How the console reaches the ring.** (a) **`stdout` and `stderr` reopened on a device of
    the component's own, which writes on to `/dev/console`**; (b) `esp_log_set_vprintf`; (c)
    ESP-IDF's ROM output channel (`esp_rom_install_channel_putc`). **Recommend (a).** The
    example's lines, the ones that say what an update or a trial is doing, are `printf`, which
    (b) never sees; (c) runs from interrupts and with the cache off, where the ring's lock
    cannot be taken, and it sees only ROM output. Cost: a device registered with ESP-IDF's VFS,
    one more step on every console write, and nothing kept from before `app_main`.
18. **The pairing token and the log.** (a) **a scope, `ConsoleOnly`, around the few writes
    that are the console's alone**; (b) filter the ring for known secrets; (c) no ring on a
    Sendspin board. **Recommend (a).** The code that prints the token knows it is one; a
    filter would have to recognise every secret that might ever be printed. Cost: a new line
    that ought to stay off the network has to be written inside the scope, and a line some
    other task prints is kept.
