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
python3 -m http.server 8000 --bind 0.0.0.0 --directory esp-idf/ac3forge/examples/stream_player/www
```

then play `http://<host>:8000/714-walk.ec3` - from a board, the serving machine's LAN address;
from QEMU's user-mode network, `10.0.2.2`. The web page's Play field takes the URL, and so
does `POST /play`.

## What plays where

- A 7.1.4 output needs a sink of twelve slots or more. On the ESP32-S3 only `capture` with
  `CONFIG_AC3FORGE_EXAMPLE_CAPTURE_TDM=1` reaches twelve: it converts and checks with no
  peripheral behind it. The S3's I2S carries at most 128 bits a TDM frame - four 32-bit slots or
  eight 16-bit ones on a data line - and ESP-IDF refuses more, so the `tdm` sink cannot carry
  7.1.4. `sdkconfig.ci-http714` is the capture shape under QEMU, and CI plays the set on it.
- On a two-slot sink every stream plays too, folded to 2.0 by the decoder.
- `streams.json` marks `"psram": true` the streams measured to need more internal RAM than a
  network shape has without PSRAM: 7.1.4 with AHT, enhanced coupling or TPN, and the Dolby
  Encoding Engine's 5.1, which uses AHT. A board with PSRAM (`sdkconfig.psram`) is where they
  play.
- Objects are placed when the layout has height speakers and the firmware reconstructs them
  (`CONFIG_AC3FORGE_EXAMPLE_OBJECTS`). `objects-mdct.ec3` and `height.ec3` are in the
  MDCT-band domain (`CONFIG_AC3FORGE_EXAMPLE_JOC_DOMAIN=1`); `demo.ec3` and
  `514-joc-dee.ec3` are in the QMF domain, which needs PSRAM.
- `ac3-51-44k.ac3` is at 44.1 kHz, and the player refuses it: its sink runs at 48 kHz.

## Making it again

```bash
python tools/generators/gen_device_streams.py --ac3cli <a host build's ac3cli>
```

rewrites every stream here and `streams.json`. The levels in `streams.json` are the host
decoder's when the set was made. CI holds the emulated board to them
(`tools/checks/check_stream_set.py`), so a change to what the decoder outputs fails there
until the set is made again.
