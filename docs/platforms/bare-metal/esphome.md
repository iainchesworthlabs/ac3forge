# ESPHome

`esphome/components/ac3forge/` is an ESPHome external component. It is the plumbing:
`Ac3ForgeComponent` owns an `ac3::FrameDecoder` and an `ac3::io::AccessUnitAccumulator`, takes
bytes and hands back planar float PCM. It is **not** a `media_player` or a `speaker` source —
ESPHome's `speaker` platform is ESP-IDF-only, so that is the obvious next step rather than a
blocked one.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/iainchesworthlabs/ac3forge
      ref: main
      path: esphome/components
    components: [ac3forge]

esp32:
  board: esp32-s3-devkitc-1
  framework:
    type: esp-idf

ac3forge:
  version: v0.10.0-beta.1   # a git ref of ac3forge itself
  buffer_size: 16384
```

Two refs are in play: `external_components`' `ref` picks the version of the ESPHome component,
and `ac3forge:`'s `version:` picks the version of the library it fetches. Pin both for anything
meant to keep working.

`buffer_size` is the framer's working buffer, floored at 4,160 bytes — one syncframe plus the
next header, which is what deciding where an access unit ends requires. 16 KB holds an independent
substream plus three dependents, which covers Atmos.

The component reaches the library by git reference rather than the registry:
`add_idf_component` writes `git:`, `version:` and `path:` into the generated
`idf_component.yml`, which is the form the IDF component manager wants for a component in a
subdirectory. Nothing here is blocked on [publishing](esp32-s3.md#the-esp-idf-component).

CI runs `esphome config` over `esphome/tests/ac3forge-test.yaml` against a local source pointing
at the working tree, which exercises the schema and `to_code` including the `add_idf_component`
call, and asserts that a `buffer_size` no access unit fits in is rejected. It does **not** compile
the firmware: that would clone ac3forge at the configured ref and build the whole IDF project,
which says nothing about the code under review, since the ref it fetched is not that code.

[`esphome/README.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/esphome/README.md)
has the rest, including why PSRAM is worth having on a board that also runs WiFi.

## Where to go next

- [ESP32-S3](esp32-s3.md) — the component this wraps, and the board it targets by default.
- [Bare metal overview](index.md) — how the pages in this section relate.
