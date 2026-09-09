# ac3forge as an ESP-IDF component

Dolby Digital (AC-3) and Dolby Digital Plus (E-AC-3) decoding, or encoding, for the ESP32-S3, in
the library's minimum-footprint profile: a static archive built without exceptions or RTTI,
sized to run out of internal SRAM with no PSRAM.

This directory is the component. It registers no sources of its own: `CMakeLists.txt` pre-seeds
the repository's options, `add_subdirectory()`s the repository root and links
`ac3::forge_minimal`, so the library is built from the same target definitions every other
platform uses and nothing here can drift from `src/forge/minimal.cmake`. `idf_component.yml` is
the registry manifest, and it is not published yet — see
[the CI workflow](../../.github/workflows/esp-component.yml) for why the publish job is gated.

## Using it

Two lines in a project's top-level `CMakeLists.txt`, both **before** the include of
`project.cmake`, because the component is read during IDF's component scan:

```cmake
set(EXTRA_COMPONENT_DIRS "/path/to/ac3forge/esp-idf")   # the directory CONTAINING components
set(AC3FORGE_ESP_PROFILE "decoder")                      # or "encoder"
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
```

The two profiles are mutually exclusive: no two of decode, AC-3 encode and E-AC-3 encode fit in
this part's internal SRAM at once. Switching needs `idf.py fullclean` first, since the choice
reaches the library as CMake cache variables a warm build directory has already resolved.

Packed for the component registry, the archive carries `src/forge` and `cmake` inside this
directory, staged by [`tools/packaging/pack_esp_component.py`](../../tools/packaging/pack_esp_component.py);
the component's own CMake finds the library either way.

## The examples

| Example | What it shows |
|---|---|
| [`examples/i2s_player`](examples/i2s_player/README.md) | Decodes a fixture linked into the image and plays it out of an I2S DAC, printing per-lap timing from the DAC's own clock. The measurement anyone with a board can repeat. |
| [`examples/stream_player`](examples/stream_player/README.md) | Bytes from a flash partition, an SD card or an HTTP body over WiFi, through the incremental framer, to an I2S or TDM DAC; a `capture` sink for CI. How a real player gets its audio. |

Both are built by CI under `espressif/idf:v6.1`, and `stream_player` runs under QEMU there.
Timing figures come only from a board: QEMU is not cycle-accurate.

## On a board

The examples ask for 240 MHz in their `sdkconfig.defaults`; ESP-IDF's own default is 160, and a
decode that has to finish inside a 32 ms frame should name the clock it was measured at. On an
`ESP32-S3-DevKitC-1` reached through its native USB connector, add each example's
`sdkconfig.hw` overlay (`SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.hw"`) so the console
comes out of the same cable, and expect the USB-Serial-JTAG reset trap: every host-initiated
reset lands in `boot:0x0 (DOWNLOAD)` and the application never starts, so flash, press the
board's RESET button, then attach with `idf.py monitor --no-reset`. Building several shapes on
one machine, give each its own build directory and its own `-DSDKCONFIG=<build dir>/sdkconfig`.

## Where this is going

[`planning/esp32-player.md`](../../planning/esp32-player.md) is the plan for what the component
should carry beyond the library for a player — buffering and tasks, the source and sink seams,
a control surface — and for the ESPHome component that sits on it. The platform page,
[`docs/platforms/esp32.md`](../../docs/platforms/esp32.md), has the footprint and timing figures
and what the port required from the library.
