# ICL Forge for ESPHome

An ESPHome external component that pulls ICL Forge into an ESP-IDF build and
exposes a decoder plus the streaming framer.

## What this is, and is not

**Is:** the plumbing. `IclForgeComponent` owns an `iclforge::ac3::FrameDecoder` and an
`iclforge::ac3::io::AccessUnitAccumulator`; you feed it bytes and take planar float PCM
back. `FrameDecoder` reads AC-3 alone: an E-AC-3 stream (bsid above 8), Atmos
included, is framed but not decoded, and `decode()` then returns null with
`failed()` set. There is no AC-4 here either.

**Is not:** a `media_player` or a `speaker` source. ESPHome's `speaker` platform
is ESP-IDF-only, so the frameworks are compatible and that is the obvious next
step — but it is a component in its own right, and shipping the plumbing first
lets it be built against something that already works.

## Using it

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

Two refs are in play and they are not the same thing. The `external_components`
`ref` picks the version of *this ESPHome component*; `iclforge:`'s `version:`
picks the version of *the library* it fetches. Pin both to tags for anything you
intend to keep working.

`buffer_size` is the framer's working buffer. The floor of 4,160 bytes is one
syncframe plus the header of the next, which is what deciding where an access
unit ends requires; below that no access unit can ever be assembled. 16 KB is
the size to use when the stream's shape is not known in advance — it holds an
independent substream plus three dependents, the shape of an Atmos access unit,
which the framer can hold and `FrameDecoder` cannot decode.

## Which chips

**Tested on the ESP32-S3 only.** CI validates the configuration for an S3 board
(`esp32-s3-devkitc-1`) and compiles nothing, and no ESPHome build of the
component has been run on another part. What it wraps is the ESP-IDF
component, whose manifest lists the ESP32-S3, ESP32-C3, ESP32-C6 and ESP32-P4:
the decode arithmetic follows the part, `float` on the S3 and the P4 and fixed
point on the C3 and C6
([`docs/platforms/bare-metal/esp32-s3.md`](../docs/platforms/bare-metal/esp32-s3.md#the-esp-idf-component)).
The original ESP32 and the S2 are not in the manifest.

## Memory

The probe's AC-3 rows peak at 47,772 to 58,645 bytes of heap, in the block form
of the decoder; this component uses the whole-frame form and has not been
measured. The part has 304,680 bytes of internal SRAM free under QEMU before
ESPHome's own components and WiFi take their share, which is why PSRAM is worth
having on a board that will also run WiFi — the library does not require it,
but ESPHome is not the only thing on the part.

The S3 decodes AC-3 5.1 in real time on a board (0.31x of a frame in the probe's
timing runs of 2026-09-09 to 2026-09-11; [the S3 page](../docs/platforms/bare-metal/esp32-s3.md#timing)
has the table). It has not been timed through this component.

## Why a git dependency and not the registry

ICL Forge is not published to the ESP Component Registry yet — see
[`.github/workflows/esp-component.yml`](../.github/workflows/esp-component.yml)
for why that publish job is deliberately not armed. ESPHome's
`add_idf_component` writes `git:`, `version:` and `path:` straight into the
generated `idf_component.yml`, which is the form the IDF component manager wants
for a component living in a subdirectory of a repository, so nothing is blocked
on publishing.

## Validation

`esphome config` runs over
[`tests/iclforge-test.yaml`](tests/iclforge-test.yaml) in CI, against a **local**
source pointing at the working tree. That exercises the schema and `to_code` —
including the `add_idf_component` call, whose signature is not covered by any
stability promise. CI also asserts that a `buffer_size` no access unit fits in
is *rejected*, because a bound that never rejects is not a bound.

CI does not compile the firmware: that would clone ICL Forge at the configured
ref and build the whole IDF project, which says nothing about the code under
review because the ref it fetched is not that code.
