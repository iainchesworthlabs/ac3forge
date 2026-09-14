# Recorded `GET /status` payloads

Bodies of `GET /status` exactly as the firmware sent them, recorded on 2026-09-11 from the
streaming example (`esp-idf/ac3forge/examples/stream_player`) under QEMU in CI's HTTP shape
(`sdkconfig.defaults;sdkconfig.ci-http`), with one local change: the stream came from port 18100
on the host rather than 8000, so the locations say `:18100`. They predate #638, which clears the
last play's figures when a play begins and reports `opening` while the source opens:
`stopped.json` and `failed-open.json` show what came before it, and `rendering.spec.js` renders
the shape the example reports since as well. It serves each payload to the page as it is.

| File | What the device had just done |
|---|---|
| `finished-eac3.json` | The boot play: the WASM page's demo (`apps/wasm/assets/demo.ec3`, E-AC-3 5.1 with objects), 250 access units to the end of the stream. |
| `finished-ac3.json` | `POST /play` of the example's own sample (`stream/sample.ac3`, AC-3 5.1, no objects), played to its end. |
| `refused.json` | `POST /play` of an `ftp://` location, which the `http` source refuses: `202`, then the state stays `stopped` and the location stays the previous one. |
| `playing-eac3.json` | `POST /play` of the demo concatenated eight times, taken mid-way (`passes` 0, 200 or more frames). |
| `stopped.json` | `POST /stop` during that play: the location is the stopped play's, the stream and the figures an earlier play's, as before #638; since it, a play stopped part-way reports none. |
| `failed-decode.json` | `POST /play` of the demo with ten access units (100 to 109) overwritten with `0xFF` after their first eight bytes: the decoder stopped with error 2. |
| `failed-open.json` | `POST /play` of a location on a port nothing listens on: the source did not open, so the state is `failed` with `failed` false, beside the previous play's figures, as before #638; since it, beside none. |

The second set was recorded the same day from the firmware that reports how a play serves its
layout (`sink_slots`, and `stream.layout`, `render`, `coded` and `silent`), in
`sdkconfig.ci-http714`'s twelve-slot shape playing the stream set
(`esp-idf/ac3forge/examples/stream_player/www/`) from port 18300, and one from
`sdkconfig.ci-http`'s two-slot shape.

| File | What the device had just done |
|---|---|
| `finished-714.json` | The twelve-slot shape's boot play: `714-walk.ec3`, 7.1.4 on 7.1.4. |
| `playing-714.json` | The walk again, taken while it played. |
| `finished-51-on-714.json` | `layout-51.ec3` on 7.1.4: the rear surrounds and the heights silent. |
| `next-51.json` | `PUT /layout` of `5.1` after that play: `layout` is the next play's, `stream.layout` still this one's. |
| `finished-714-on-51.json` | `layout-714.ec3` at 5.1: the rears and the heights spread over the room. |
| `finished-51-on-20.json` | `layout-51.ec3` at 2.0: the decoder's Lo/Ro fold. (7.1.4 at 2.0 does not fit this shape's internal RAM; see `planning/esp32-stream-set.md`.) |
| `finished-dualmono-on-714.json` | `eac3-dualmono.ec3` on 7.1.4: two mono programmes on L and R. |
| `finished-objects-as-bed.json` | `demo.ec3` on 7.1.4, in a shape that plays objects as their bed. |
| `failed-sample-rate.json` | `ac3-51-44k.ac3`, which the player refuses: its sink runs at 48 kHz. |
| `finished-20.json` | The two-slot shape's boot play: the WASM page's demo folded to 2.0. |

The first set's payloads come from three runs of the same image. Each run ended when the part panicked in
the HTTP server's task shortly after a `PUT /layout`, the same fault the base branch shows; see the
pull request that added them. What the page cannot get from the emulator - objects placed onto a
height layout, which needs PSRAM for the reconstruction - `rendering.spec.js` derives from these
by changing the fields such a play changes, and says so.
