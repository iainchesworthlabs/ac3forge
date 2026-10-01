# The stream set

Streams for the streaming example's `http` source to fetch: 7.1.4 streams that reach all
twelve slots of a 7.1.4 output, and beside them a range across the layouts the player renders
onto, both codecs, dependent substreams, two programmes, dual mono, the Annex E coding tools,
short frames, VBR, DRC words, other encoders' streams and object audio.
[planning/esp32-stream-set.md](../../../../../planning/esp32-stream-set.md) has what each one
is, what was measured, and why the set is as it is. `streams.json` has the same for each
stream, with the level each slot of a 7.1.4 output should get from it.

## Serving it

From the repository's root:

```bash
python3 -m http.server 8000 --bind 0.0.0.0 --directory esp-idf/iclforge/examples/hearth_sink/www
```

then play `http://<host>:8000/714-walk.ec3` - from a board, the serving machine's LAN address;
from QEMU's user-mode network, `10.0.2.2`, with `POST /play`.

## What plays where

- A 7.1.4 output needs a sink of twelve slots or more. `capture` with
  `CONFIG_ICLFORGE_EXAMPLE_CAPTURE_TDM=1` reaches twelve with no peripheral behind it: it converts
  and checks. `sdkconfig.ci-http714` is that shape under QEMU, and CI plays the set on it. The
  `i2s` sink on an ESP32-S3 reaches twelve only with 16-bit slots and a second I2S line wired: one
  line's TDM frame holds at most 128 bits - four 32-bit slots or eight 16-bit ones - and ESP-IDF
  refuses more. The ESP32-P4's `i2s_wide` sink has a 512-bit frame, but has not opened TDM on the
  pre-production silicon the example was brought up on.
- On a two-slot sink every stream plays too, folded to 2.0 by the decoder.
- `streams.json` marks `"psram": true` the streams measured to need more internal RAM than a
  network shape has without PSRAM: 7.1.4 with enhanced coupling or TPN. A board with PSRAM
  (`sdkconfig.psram`) is where they play.
- Objects are placed when the layout has height speakers and the firmware reconstructs them
  (`CONFIG_ICLFORGE_EXAMPLE_OBJECTS`). `objects-mdct.ec3` and `height.ec3` are in the
  MDCT-band domain (`CONFIG_ICLFORGE_EXAMPLE_JOC_DOMAIN=1`); `demo.ec3` and
  `514-joc-dee.ec3` are in the QMF domain, which needs PSRAM.
- `ac3-51-44k.ac3` is at 44.1 kHz, and the player refuses it: its sink runs at 48 kHz.

## Making it again

```bash
python tools/generators/gen_device_streams.py --forge <a host build's forge>
```

rewrites every stream here and `streams.json`. The levels in `streams.json` are the host
decoder's when the set was made. CI holds the emulated board to them
(`tools/checks/check_stream_set.py`), so a change to what the decoder outputs fails there
until the set is made again.
