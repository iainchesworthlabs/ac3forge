# ESPHome

`esphome/components/iclforge/` is an ESPHome external component. It is the plumbing:
`IclForgeComponent` owns an `iclforge::ac3::FrameDecoder` and an `iclforge::ac3::io::AccessUnitAccumulator`, takes
bytes and hands back planar float PCM. It is **not** a `media_player` or a `speaker` source —
ESPHome's `speaker` platform is ESP-IDF-only, so that is the obvious next step rather than a
blocked one.

`FrameDecoder` reads AC-3 alone. An E-AC-3 stream (bsid above 8), Atmos included, is framed but
not decoded: `decode()` returns null and `failed()` is set. The component has no AC-4 either, and
it has been validated on an ESP32-S3 board configuration only.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/iainchesworthlabs/iclforge
      ref: main
      path: esphome/components
    components: [iclforge]

esp32:
  board: esp32-s3-devkitc-1
  framework:
    type: esp-idf

iclforge:
  version: v0.10.0-beta.1   # a git ref of iclforge itself
  buffer_size: 16384
```

Two refs are in play: `external_components`' `ref` picks the version of the ESPHome component,
and `iclforge:`'s `version:` picks the version of the library it fetches. Pin both for anything
meant to keep working.

`buffer_size` is the framer's working buffer, floored at 4,160 bytes — one syncframe plus the
next header, which is what deciding where an access unit ends requires. 16 KB holds an independent
substream plus three dependents, the shape of an Atmos access unit, which the framer can hold and
`FrameDecoder` cannot decode.

The component reaches the library by git reference rather than the registry:
`add_idf_component` writes `git:`, `version:` and `path:` into the generated
`idf_component.yml`, which is the form the IDF component manager wants for a component in a
subdirectory. Nothing here is blocked on [publishing](esp32-s3.md#the-esp-idf-component).

CI runs `esphome config` over `esphome/tests/iclforge-test.yaml` against a local source pointing
at the working tree, which exercises the schema and `to_code` including the `add_idf_component`
call, and asserts that a `buffer_size` no access unit fits in is rejected. It does **not** compile
the firmware: that would clone ICL Forge at the configured ref and build the whole IDF project,
which says nothing about the code under review, since the ref it fetched is not that code. The
job is the `esp-component` call in the `esp` lane of `ci.yml`, which runs after a merge to main that
changes the ESP32 trees or a tree its component ships, and nightly ([the lane table](../../ci-lanes.md#lane-table),
[CI for many agents](../../ci-agentic.md#the-tiers)).

[`esphome/README.md`](https://github.com/iainchesworthlabs/iclforge/blob/main/esphome/README.md)
has the rest, including why PSRAM is worth having on a board that also runs WiFi.

## Where to go next

- [ESP32-S3](esp32-s3.md) — the component this wraps, and the board it targets by default.
- [Bare metal overview](index.md) — how the pages in this section relate.
