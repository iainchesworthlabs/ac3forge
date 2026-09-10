# Recorded `GET /status` payloads

Bodies of `GET /status` exactly as the firmware sent them, recorded on 2026-09-11 from the
streaming example (`esp-idf/ac3forge/examples/stream_player`) under QEMU in CI's HTTP shape
(`sdkconfig.defaults;sdkconfig.ci-http`), with one local change: the stream came from port 18100
on the host rather than 8000, so the locations say `:18100`. `rendering.spec.js` serves each one
to the page as it is.

| File | What the device had just done |
|---|---|
| `finished-eac3.json` | The boot play: the WASM page's demo (`apps/wasm/assets/demo.ec3`, E-AC-3 5.1 with objects), 250 access units to the end of the stream. |
| `finished-ac3.json` | `POST /play` of the example's own sample (`stream/sample.ac3`, AC-3 5.1, no objects), played to its end. |
| `refused.json` | `POST /play` of an `ftp://` location, which the `http` source refuses: `202`, then the state stays `stopped` and the location stays the previous one. |
| `playing-eac3.json` | `POST /play` of the demo concatenated eight times, taken mid-way (`passes` 0, 200 or more frames). |
| `stopped.json` | `POST /stop` during that play: the location is the stopped play's, the stream and the figures the last play that ended by itself. |
| `failed-decode.json` | `POST /play` of the demo with ten access units (100 to 109) overwritten with `0xFF` after their first eight bytes: the decoder stopped with error 2. |
| `failed-open.json` | `POST /play` of a location on a port nothing listens on: the source did not open, so the state is `failed` beside the previous play's figures, whose `failed` is false. |

The payloads come from three runs of the same image. Each run ended when the part panicked in
the HTTP server's task shortly after a `PUT /layout`, the same fault the base branch shows; see the
pull request that added them. What the page cannot get from the emulator - objects placed onto a
height layout, which needs PSRAM for the reconstruction - `rendering.spec.js` derives from these
by changing the fields such a play changes, and says so.
