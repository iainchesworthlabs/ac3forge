# IAB (SMPTE ST 2098-2) reading: `ac3iab::ac3iab`

`ac3iab/ac3iab.hpp`, `ac3iab/mxf.hpp`, library `ac3iab::ac3iab`. A standalone reader for the
Immersive Audio Bitstream (IAB, SMPTE ST 2098-2:2022) — the format Dolby Atmos cinema masters
carry, and that Netflix's IMF pipeline (SMPTE ST 2067-201) delivers inside MXF track files. Like
`ac3adm::ac3adm`, `matroska::matroska`, `mp4::mp4` and `mpegts::mpegts`, it links nothing from
`ac3::forge` — it has no idea AC-3, E-AC-3 or the JOC/Atmos object layer exist.

Roadmap item IM1: phase 1 is the bitstream reader (`ac3iab.hpp`); phase 2 is MXF Track File
extraction (`mxf.hpp`), both covered here. Mapping the parsed bed/object graph onto
`ac3::oba::AtmosEncoder` (phase 3) is a separate module, `ac3::admbridge`'s `build_iab()` — see
[ADM → Atmos bridging](adm-bridge.md#bridging-iab-roadmap-im1-phase-3) — driven end to end by
`ac3cli atmos-iab` (see [Commands](../cli/commands.md)).

```cpp
const auto frames = ac3iab::parse_iabitstream(path);   // a bare elementary .iab file
// or:
const auto frames = ac3iab::parse_mxf_iab(path);       // a real IAB Track File (MXF)
if (!frames) {
    fmt::printf("parse failed: %.*s\n", static_cast<int>(ac3iab::describe(frames.error()).size()),
                ac3iab::describe(frames.error()).data());
    return 1;
}
for (const auto& entry : *frames) {
    fmt::printf("frame: %u Hz, %u-bit, %zu bed(s), %zu object(s)\n", entry.frame.sample_rate,
                entry.frame.bit_depth, entry.frame.beds.size(), entry.frame.objects.size());
}
```

Full program: [`examples/read_iab.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/read_iab.cpp) —
writes the same small IAB fixture both as a bare elementary file and wrapped in a synthetic MXF
Track File, parses both, and prints that they agree.

**Defaults on**, unlike `ac3adm::ac3adm`. `AC3FORGE_BUILD_IAB` defaults **ON** — IAB's own
Plex(n)-coded bitstream and its MXF/KLV wrapper both need nothing beyond this module's own bit
reader (`src/ac3iab/src/bitreader.hpp`), no third-party dependency at all, so it builds the same
way the three container writers do:

```bash
cmake --preset config-windows-msvc-debug   # AC3FORGE_BUILD_IAB=ON by default
```

`ac3cli atmos-iab` (roadmap IM1 phase 3, needs `-DAC3FORGE_BUILD_ADM=ON` — the same flag
`ac3::admbridge` itself rides, since that is the module with a consumer for this graph) is this
module's own real-world driver; nothing else in this build (`ac3gui`, the other examples) consumes
it yet.

## What gets parsed

- **The IAB element graph** (§9 Table 4's element tree, §10's field definitions): `IaFrame` →
  `BedDefinition` (+ recursive `BedDefinition`/`BedRemap` children) and `ObjectDefinition` (+
  recursive `ObjectDefinition`/`ObjectZoneDefinition19` children), plus `AudioDataPCM`,
  `AuthoringToolInfo` and `UserData`. Positions (§5.4's `DistanceXY`/`DistanceZ` formulas), gains
  and spreads (§5.5) are resolved to their final linear/physical values on the way in, the same
  "plain aggregate, already-resolved" shape [`ac3adm/model.hpp`](adm.md) uses for ADM — see
  [`ac3iab/model.hpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/src/ac3iab/include/ac3iab/model.hpp)
  for the full struct-by-struct citation trail.
- **`AudioDataDLC`** (§9.6/§10.7, Annex B) is read only by identity — its forward-adaptive lattice
  predictor plus entropy-coded residual is left as an opaque `std::vector<std::byte>` rather than
  decoded. This is IM1's one deliberately unfinished piece; see that struct's own comment in
  `model.hpp`.
- **The MXF wrapping** (`mxf.hpp`, phase 2) — SMPTE ST 2098-2 itself has no MXF content at all; the
  wrapping is a separate, much shorter standard, **SMPTE ST 2067-201:2021** ("IMF — Immersive Audio
  Bitstream Level 0 Plug-in"), which in turn references the base MXF standards (ST 377-1 file
  format, ST 379-1/-2 Generic/Constrained Generic Container, ST 336 KLV/BER encoding). All five are
  free from [pub.smpte.org](https://pub.smpte.org).

  The one fact that keeps this "minimal" rather than a general MXF library: ST 2067-201 §5.5
  clip-wraps the whole Immersive Audio Bitstream as **one** Generic Container KLV Value (ST 379-2
  §8.4.2 — "a single CP, containing a single CI, containing a single CE, comprised of a single
  KLV"). That Value is byte-identical to ST 2098-2 Clause 7's `IABitstream` syntax — the same
  `while(true){Preamble;IAFrame;}` run an elementary `.iab` file already has — so `parse_mxf_iab`
  needs no frame-level indexing, Index Tables or System Item at all: it walks top-level KLV
  triplets from the start of the file (no Run-In assumed — ST 377-1 §7.2.1's own "default case"),
  skips everything that is not a match by that KLV's own declared Length, and hands the one KLV
  whose Key matches ST 2067-201 Table 4.2's registered value straight to `parse_iabitstream`'s
  `std::istream` overload, unmodified. See
  [`src/ac3iab/src/mxf_reader.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/src/ac3iab/src/mxf_reader.cpp)'s
  own header comment for the full clause-by-clause trail, including why Header Metadata's
  Preface/ContentStorage/Package object graph is never parsed at all (locating essence is a
  KLV-Key matter, not an object-graph one).

## Consulted, never copied

Every table and algorithm here is transcribed directly from the published standards, with the
section/table/clause number cited at each call site, per CONTRIBUTING.md's clean-room rule.
`DTSProAudio/iab-validator` (MIT) was consulted only as an external oracle to check this reader's
bitstream output against a second, independent implementation — it has no MXF-related code or
sample `.mxf` files at all, so it played no such role for `mxf.hpp`.

## API

```cpp
enum class IabError : std::uint8_t {
    kCannotOpen, kTruncated, kBadEscape, kBadPreambleTag, kBadFrameTag, kReservedVersion,
    kReservedSampleRate, kReservedBitDepth, kReservedFrameRate, kUnterminatedString,
    kMxfBadKlv, kMxfNoIabEssence,
};
std::string_view describe(IabError error);

struct IABitstreamFrame { std::vector<std::byte> preamble; IaFrame frame; };

std::expected<std::vector<IABitstreamFrame>, IabError> parse_iabitstream(const std::string& path);
std::expected<std::vector<IABitstreamFrame>, IabError> parse_iabitstream(std::istream& in);
std::expected<IaFrame, IabError> parse_iaframe(std::span<const std::byte> payload);

std::expected<std::vector<IABitstreamFrame>, IabError> parse_mxf_iab(const std::string& path);
std::expected<std::vector<IABitstreamFrame>, IabError> parse_mxf_iab(std::istream& in);
```

One error enum covers both entry points — `parse_mxf_iab` is still fundamentally "read an IAB
frame sequence", just from a different container, so `kMxfBadKlv`/`kMxfNoIabEssence` join the
bitstream-level codes rather than a second, parallel error type. `parse_iaframe` takes one
already-extracted `IAElement(IA_FRAME)`'s payload directly (no header of its own — see its own doc
comment) — the lower-level entry point both `parse_iabitstream` and `parse_mxf_iab` use internally
once they have stripped their own respective framing away.

## Bridging to Atmos

`ac3::admbridge`'s `build_iab()` maps this module's parsed graph onto `ac3::oba::AtmosEncoder`'s
input shape — one `ac3::oba::ObjectPath` plus one mono PCM buffer per Bed channel or Object, ready
to drive `encode_frame()` in a loop, the same destination shape `ac3::admbridge::build()` produces
for ADM. See [ADM → Atmos bridging](adm-bridge.md#bridging-iab-roadmap-im1-phase-3) for what gets
mapped (Table 19 → `ac3::oba::BedLabel`, position conversion, MetaID-based cross-frame identity)
and what does not (spread, the 9-zone `ObjectZoneControl`).
[`examples/encode_iab.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/encode_iab.cpp)
is the full read → bridge → encode pipeline; `ac3cli atmos-iab` drives the identical pipeline from
the command line.

---

See also: [ADM → Atmos bridging](adm-bridge.md) — `ac3::admbridge`, which maps this graph onto
`ac3::oba::AtmosEncoder`; [ADM / BW64 reading](adm.md) — the sibling codec-blind reader this
module's shape and documentation follow.
