# An ESP32-C6 sink

`hearth_sink` on the ESP32-C6 is a **stereo** Sendspin player: it receives synchronised audio from
Hearth or another compatible server and decodes AC-3 and E-AC-3 up to 2.0. It does not decode
AC-4, 5.1 or immersive layouts — the part's memory budget and fixed-point tier limit the network
player to stereo PCM output.

For pairing, groups, firmware updates and the shared Sendspin workflow, see
[An ESP32-S3 sink](sink-esp32-s3.md) and [Sink firmware](sink-firmware.md). This page covers
what differs on the C6.

!!! note "Status as of 2026-09-26"
    CI builds and packages `hearth-sink-esp32c6` (4 MB flash table) and
    `hearth-sink-esp32c6-16mb`. Hearth plays to C6 sinks in a group with S3 boards. Decode
    probes and timing tables live on the [ESP32-C6 platform page](../platforms/bare-metal/esp32-c6.md).

## Which image

| Image | For |
|---|---|
| `hearth-sink-esp32c6` | Any ESP32-C6 module with a **4 MB** flash partition table |
| `hearth-sink-esp32c6-16mb` | An ESP32-C6 module with **16 MB** of flash |

See [Sink firmware — Which image](sink-firmware.md#which-image) for checking a board and
installing from a release.

## Build and flash

From `esp-idf/ac3forge/examples/hearth_sink` in an ESP-IDF v6.1 terminal:

```bash
export SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.hw;sdkconfig.sendspin;sdkconfig.c6;sdkconfig.sendspin-c6"
idf.py set-target esp32c6
idf.py build
idf.py -p PORT flash monitor
```

For a board with **16 MB** of flash (`esptool --chip esp32c6 flash-id`), append `;sdkconfig.flash16mb`
to `SDKCONFIG_DEFAULTS` so the larger partition table is used.

The example [README](https://github.com/iainchesworthlabs/ac3forge/blob/main/esp-idf/ac3forge/examples/hearth_sink/README.md#on-the-esp32-c6)
has C6-specific measurements (Wi-Fi load, ring size, clock sync).

## Limits compared with the ESP32-S3

| | ESP32-C6 sink | ESP32-S3 sink |
|---|---|---|
| Output | Stereo (2.0) | Up to 7.1.4 (16 TDM slots) |
| PSRAM | None — internal SRAM only | 8 MB octal PSRAM required |
| Codec arithmetic | Fixed-point tier | Float tier |
| AC-4 | Not supported | Not supported (library excluded from firmware) |
| QEMU CI | Not emulated — board or CI build only | Sendspin and Improv under QEMU |

5.1 and 7.1.4 streams **decode** on the bare-metal probe but do not fit the Sendspin player's
memory budget once Wi-Fi and the ring buffer are resident; the firmware refuses layouts wider than
stereo for network playback.

## Related pages

- [Hearth index](index.md) — desktop app and network output
- [ESP32-C6 platform](../platforms/bare-metal/esp32-c6.md) — decode timing and memory tables
- [ESP-IDF component README](https://github.com/iainchesworthlabs/ac3forge/blob/main/esp-idf/ac3forge/README.md)
