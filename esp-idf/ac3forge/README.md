# ac3forge as an ESP-IDF component

Dolby Digital (AC-3) and Dolby Digital Plus (E-AC-3) decoding, or encoding, for the ESP32-S3, in
the library's minimum-footprint profile: a static archive built without exceptions or RTTI,
sized to run out of internal SRAM with no PSRAM.

This directory is the component. For the codec it is a wrapper: `CMakeLists.txt` pre-seeds the
repository's options, `add_subdirectory()`s the repository root and links `ac3::forge_minimal`,
so the library is built from the same target definitions every other platform uses and nothing
here can drift from `src/forge/minimal.cmake`. What it adds of its own is the layer that cannot
live in the library because it is made of FreeRTOS:

- **`ac3forge::Player`** ([`include/ac3forge/player.hpp`](include/ac3forge/player.hpp)): a fetch
  task reading a `ByteSource` into a ring buffer, a decode task draining it through the
  incremental framer and both decoders a block at a time, rendering each block onto the
  configured speaker layout, and a `PcmSink` taking one block of planar float per output slot.
  The layout, the fold, whether to reconstruct objects, cores, priorities, the ring's size and
  whether it sits in PSRAM are `PlayerConfig`. It reports frames, decode time, the worst frame,
  how low the ring ran, and why a run ended. An integrator implements the two seams for their
  transport and their DAC and gets the rest.
- **`ac3forge::OutputLayout`** ([`include/ac3forge/layout.hpp`](include/ac3forge/layout.hpp)):
  the speakers a player has, one per slot, from a name (`2.0`, `5.1`, `7.1.4`, `9.2.4`) or a
  speaker list (`L,R,C,LFE,Ls,Rs`, or angles). **`ac3forge::LayoutRenderer`**
  ([`include/ac3forge/render.hpp`](include/ac3forge/render.hpp)) turns the decoder's block - the
  coded channels and, when the stream has them, the objects with their positions - into one
  block per slot: a stereo or mono layout is the decoder's own §7.8 fold; anything else has the
  bed placed channel by channel through `ac3::spatial::pan_direction`, and a layout with height
  speakers has the objects placed by their own positions instead. Both headers are free of
  ESP-IDF and tested on the host (`tests/io/test_layout.cpp`).
- **`ac3forge::Control`** ([`include/ac3forge/control.hpp`](include/ac3forge/control.hpp)): a REST
  surface over whatever owns a player - `GET /status`, `POST /play` with a location, `POST /stop`,
  `POST /volume`, `GET`/`PUT /layout` - on `esp_http_server`, with callbacks the owner supplies so
  the server's task never touches the player itself. `GET /` is a web page for the same routes,
  and the only client they need: the state, the stream, the layout, the volume and the decode's
  timing, and the four actions, from two files in [`ui/`](ui) sent from flash as they are
  ([`planning/esp32-device-ui.md`](../../planning/esp32-device-ui.md)). `GET /api` lists the
  routes.
- **`ac3forge/interleave.hpp`**: planar float to interleaved 16-bit or 24-in-32 with slot padding,
  free of ESP-IDF and tested on the host. Library code with a temporary home; see the plan below.
- **`ac3forge::DacQueueModel`**
  ([`include/ac3forge/dac_queue_model.hpp`](include/ac3forge/dac_queue_model.hpp)): what an I2S
  DAC heard, worked out from the one fact the hardware guarantees - its DMA drains at exactly
  the sample rate. A sink tells it when each block arrives and when its write has returned, by a
  clock the sink passes in, and it counts, per play, the blocks that arrived to an empty queue,
  how long the queue was dry, and the least that was left. The streaming example's `i2s` and
  `tdm` sinks keep one each for their `sink.*` line. Free of ESP-IDF and tested on the host
  against a simulated DMA (`tests/io/test_dac_queue_model.cpp`).

`idf_component.yml` is the registry manifest, and it is not published yet — see
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
| [`examples/stream_player`](examples/stream_player/README.md) | Bytes from a flash partition, an SD card or an HTTP body over WiFi, through the incremental framer, rendered onto a configured layout - stereo, 5.1, 7.1.4 with the objects placed - to an I2S or TDM DAC; a `capture` sink for CI. How a real player gets its audio. |

Both are built by CI under `espressif/idf:v6.1`, and `stream_player` runs under QEMU there in four
shapes, one of which renders a height-object stream onto 7.1.4 and checks every slot's level
against the footprint probe's. Timing figures come only from a board: QEMU is not cycle-accurate.

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
