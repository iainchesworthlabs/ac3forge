# ac3forge for ESPHome

An ESPHome external component that pulls ac3forge into an ESP-IDF build and
exposes a decoder plus the streaming framer.

## What this is, and is not

**Is:** the plumbing. `Ac3ForgeComponent` owns an `ac3::FrameDecoder` and an
`ac3::io::AccessUnitAccumulator`; you feed it bytes and take planar float PCM
back.

**Is not:** a `media_player` or a `speaker` source. ESPHome's `speaker` platform
is ESP-IDF-only, so the frameworks are compatible and that is the obvious next
step — but it is a component in its own right, and shipping the plumbing first
lets it be built against something that already works.

## Using it

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

Two refs are in play and they are not the same thing. The `external_components`
`ref` picks the version of *this ESPHome component*; `ac3forge:`'s `version:`
picks the version of *the library* it fetches. Pin both to tags for anything you
intend to keep working.

`buffer_size` is the framer's working buffer. The floor of 4,160 bytes is one
syncframe plus the header of the next, which is what deciding where an access
unit ends requires; below that no access unit can ever be assembled. 16 KB is
the size to use when the stream's shape is not known in advance — it holds an
independent substream plus three dependents, which covers Atmos.

## Which chips

**ESP32-S3 only**, and that is measured rather than cautious. The S3 has a
single-precision FPU, which the decode path needs; the original ESP32 and the S2
would software-emulate every floating-point operation, and the ESP32-P4's vector
unit is integer-only so it inherits nothing (see
[`docs/platforms/esp32.md`](../docs/platforms/esp32.md)). The C3 and C6 are
RISC-V and a different port.

## Memory

The decode peaks at about 233 KB of internal SRAM against roughly 280 KB free,
before ESPHome's own components take their share. That is tight, and it is the
reason PSRAM is worth having on a board that will also run WiFi — the library
does not require it, but ESPHome is not the only thing on the part.

Real-time decode on this chip is **still unmeasured**. Everything CI knows comes
from QEMU, which is not cycle-accurate.

## Why a git dependency and not the registry

ac3forge is not published to the ESP Component Registry yet — see
[`.github/workflows/esp-component.yml`](../.github/workflows/esp-component.yml)
for why that publish job is deliberately not armed. ESPHome's
`add_idf_component` writes `git:`, `version:` and `path:` straight into the
generated `idf_component.yml`, which is the form the IDF component manager wants
for a component living in a subdirectory of a repository, so nothing is blocked
on publishing.

## Validation

`esphome config` runs over
[`tests/ac3forge-test.yaml`](tests/ac3forge-test.yaml) in CI, against a **local**
source pointing at the working tree. That exercises the schema and `to_code` —
including the `add_idf_component` call, whose signature is not covered by any
stability promise. CI also asserts that a `buffer_size` no access unit fits in
is *rejected*, because a bound that never rejects is not a bound.

CI does not compile the firmware: that would clone ac3forge at the configured
ref and build the whole IDF project, which says nothing about the code under
review because the ref it fetched is not that code.
