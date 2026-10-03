# An ESP32-C6 sink

`hearth_sink` runs on the ESP32-C6 as a stereo Sendspin player. It plays AC-3 and E-AC-3 streams
of up to two coded channels. A wider stream is refused before a decoder opens for it, and the
console says why once per stream.

The limit is memory, not decode speed. The C6 has no PSRAM, so the player's buffers and WiFi's
receive buffers share the internal RAM. On a board on 2026-09-22, AC-3 and E-AC-3 5.1 each stopped
within about 12 seconds when the decoder could not allocate its working memory. The part does
decode 5.1 in real time with WiFi running: see the
[ESP32-C6 platform page](../platforms/bare-metal/esp32-c6.md). The E-AC-3 decoder has used less
memory since (its 5.1 peak in the fixed tier is 109,806 bytes now, from 164,066), and the sink
has not been run again on a board to see whether the limit has moved.

The C6 build does not decode AC-4. `CONFIG_ICLFORGE_AC4` builds the decoder for this part in the
fixed-point tier, and the decoder does not fit beside WiFi: 2.0 peaks at 286,365 bytes, where the
part has about 236,000 free with WiFi up ([ESP32-C6](../platforms/bare-metal/esp32-c6.md#ac-4)).
A C6 sink takes AC-4 programmes from Hearth as PCM. No ESP32 sink takes AC-4 in a Sendspin
group; an ESP32-P4 built with `sdkconfig.ac4` decodes it from an HTTP source
([ESP32-P4](../platforms/bare-metal/esp32-p4.md#ac-4)).

A stereo group of a C6 and an S3 has played ten minutes with no underrun on either board
([example README](https://github.com/iainchesworthlabs/iclforge/blob/main/esp-idf/iclforge/examples/hearth_sink/README.md#on-the-esp32-c6)).
`idf.py qemu` does not support the C6, so CI builds its image but only a board runs it.

Pairing, groups and firmware updates work the same as on the S3; see
[An ESP32-S3 sink](sink-esp32-s3.md) and [Sink firmware](sink-firmware.md). This page covers what
differs on the C6.

## Which image

CI builds two C6 images, and a release carries both:

| Image | For |
|---|---|
| `hearth-sink-esp32c6` | A C6 module with 4 MB of flash, the least any C6 module has |
| `hearth-sink-esp32c6-16mb` | A C6 module with 16 MB of flash |

[Sink firmware — Which image](sink-firmware.md#which-image) explains how to check a board's flash
size and install from a release.

## Build and flash

From `esp-idf/iclforge/examples/hearth_sink` in an ESP-IDF terminal:

```bash
idf.py set-target esp32c6
idf.py "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.hw;sdkconfig.sendspin;sdkconfig.c6;sdkconfig.sendspin-c6" build
idf.py -p PORT flash monitor
```

For a module with 16 MB of flash, add `;sdkconfig.flash16mb` at the end of the list so the build
uses the larger partition table.

`sdkconfig.c6` and `sdkconfig.sendspin-c6` record why each setting has the value it has, from
measurements on a board: the ring size, the DMA queue and WiFi's code kept in flash. The example's
[README](https://github.com/iainchesworthlabs/iclforge/blob/main/esp-idf/iclforge/examples/hearth_sink/README.md#on-the-esp32-c6)
covers running it on a C6.

## Related pages

- [ESP32-C6 platform](../platforms/bare-metal/esp32-c6.md) — decode timing and memory, with and
  without WiFi
- [ESP-IDF component README](https://github.com/iainchesworthlabs/iclforge/blob/main/esp-idf/iclforge/README.md)
