# ADM ↔ Atmos bridging: `ac3::admbridge`

`ac3/admbridge/bridge.hpp`, `ac3/admbridge/coordinates.hpp`, library `ac3::admbridge`. Two
directions live here now:

- **Read** (roadmap item B1 phase 2 of 3, see [ROADMAP.md](https://github.com/iainchesworthlabs/ac3forge/blob/main/ROADMAP.md)):
  maps the ADM object graph [`ac3adm::ac3adm`](adm.md) parses from a BW64/ADM master onto
  [`ac3::oba::AtmosEncoder`](spatial-and-atmos.md)'s input shape — one `ac3::oba::ObjectPath` plus
  one mono PCM span per bed speaker feed or dynamic object, ready to drive `encode_frame()` in a
  loop. Driven end to end by `ac3cli atmos-adm` and
  [`examples/encode_adm.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/encode_adm.cpp).
- **Write** (roadmap item IM2, "JOC → ADM BWF writer"): the mirror image — maps a decoded
  `ac3::Eac3Decoder` programme's own bed/object PCM and OAMD automation onto an
  `ac3adm::AdmDocument`, ready for `ac3adm::write_bw64()`. Driven end to end by
  `ac3cli decode ... adm_out`.

Both directions are the same "one place `ac3adm` and `ac3::forge`/`ac3::oba` are allowed to meet"
seam this module has always been, see [Commands](../cli/commands.md) for both commands.

**Opt-in, gated by the same flag as `ac3adm::ac3adm`.** `ac3::admbridge` depends on both
`ac3adm::ac3adm` and `ac3::forge`, so it is meaningless without `AC3FORGE_BUILD_ADM=ON` and is
built as part of the same `add_subdirectory` block — no separate `AC3FORGE_BUILD_ADMBRIDGE` option
exists. See [ADM / BW64 reading](adm.md) for the exact CMake invocation.

```cpp
const auto document = ac3adm::parse_bw64(path);
if (!document) { /* ... */ }

const auto bridged = ac3::admbridge::build(*document);
if (!bridged) {
    fmt::printf("build failed: %.*s\n",
                static_cast<int>(ac3::admbridge::describe(bridged.error()).size()),
                ac3::admbridge::describe(bridged.error()).data());
    return 1;
}

ac3::oba::AtmosEncoder encoder{{.bitrate_kbps = 448}, static_cast<int>(bridged->channel_count())};
// Per frame: slice bridged->pcm[i] to the frame's sample range, evaluate
// ac3::oba::evaluate_placements(bridged->paths, t), call encoder.encode_frame(...).
```

## Why a new, standalone module

Two hard constraints rule out folding this into either side it bridges:

- `ac3adm::ac3adm` is documentedly codec-blind — its own header comments and this project's design
  keep it with zero dependency on `ac3::forge`/`ac3::oba`, and that does not change here.
- `src/forge` (`ac3::forge`, `ac3::oba::AtmosEncoder`) is always built, unconditionally, by every
  configuration of this project. It cannot gain a dependency on the opt-in, Boost-requiring
  `ac3adm` without breaking every default build.

`ac3::admbridge` is therefore its own module (`src/admbridge/`), PUBLIC-linking both — the same
shape `ac3::signing` uses for its own `ac3::forge` dependency. It is not part of the installed
`find_package(ac3forge)` package, for the same reason `ac3adm::ac3adm` itself is not (see
[ADM / BW64 reading](adm.md)'s own "Why opt-in" section): consume it via `add_subdirectory`
in-tree. ROADMAP.md's IM1 (an IAB / SMPTE ST 2098-2 reader; the DAMF reader it replaced is now
in the roadmap's "not on the list" section for want of a public specification) named this module
as the "mapping layer" it intended to share, and phase 3 has now landed: `build_iab()`
(`ac3/admbridge/iab_bridge.hpp`) maps a whole parsed `ac3iab::IABitstreamFrame` sequence — from
either of `ac3iab::ac3iab`'s two readers (`src/ac3iab`, phases 1-2: a bare elementary `.iab` file
or a real MXF Track File) — onto this same `ObjectPath` layer, driven end to end by `ac3cli
atmos-iab` (see [Commands](../cli/commands.md)). `ac3adm::AdmDocument` and `ac3iab::
IABitstreamFrame` are therefore both input shapes here, sharing the coordinate-conversion and
`ObjectPath`-construction logic this module exists to keep independent of either container's own
parsing — see "Bridging IAB" below for exactly what differs between the two.

## What gets mapped

- **Bed vs. dynamic object** is decided by the `TypeDefinition` of the `audioObject`'s own resolved
  `audioPackFormat`(s) — `AudioObject` itself carries no type of its own. `kDirectSpeakers` becomes
  a bed speaker feed; `kObjects` becomes a dynamic object. `kMatrix`, `kHoa`, `kBinaural`,
  `kUserCustom`, `kUnknown`, a nested `audioPackFormat`, or an object whose several packs disagree
  with each other all fail clearly with `BridgeError::kUnsupportedType` rather than being silently
  mishandled — none of them map onto `AtmosEncoder`'s plain position+gain+lfe_send object model
  without a design of their own this phase does not attempt.
- **`AtmosEncoder` has no separate bed-feeding method** — its constructor takes a plain object
  count and `encode_frame()` takes one flat span of objects plus one flat span of placements,
  nothing in that signature distinguishing a bed channel from a dynamic object — so a bed channel
  is represented as an object with an unmoving, pinned placement, the only way the API allows it,
  the same convention every existing caller (`ac3cli`'s `run_atmos_encode`, the GUI's
  `encodeObjects`) already uses. A bed channel whose `speakerLabel` identifies it as the LFE
  (BS.2076-2 Table 12: `LFE`, `LFE1`, `LFE2`) is routed at gain 0 / `lfe_send` 1 instead of panned
  — objects never reach the LFE by panning.
- **Position/gain automation** — `build_channel_path()` walks a channel's `audioBlockFormat`
  sequence into one `ac3::oba::ObjectPath`, implementing BS.2076-2 §10.3's `jumpPosition`/
  `interpolationLength` state machine: `jumpPosition = 0` interpolates continuously across a
  block's *entire* duration; `jumpPosition = 1` jumps (or ramps over `interpolationLength`, when
  given) at the block's *start* and then holds for the rest of it; the first block in a sequence
  always holds across its own span regardless of its own `jumpPosition`. This was checked directly
  against the standard's own text and worked Figs. 7–10 — an earlier reading of just this
  behaviour's *name* ("hold vs. glide") had the two cases backwards relative to what §10.3 actually
  says, corrected once the real clause text was read rather than assumed.
- **Coordinate conversion** (`coordinates.hpp`) — BS.2076-2 §8's polar (azimuth/elevation/distance,
  positive azimuth to the left, positive elevation up) and Cartesian (X right-positive, Y
  front-positive, Z top-positive, `[-1, 1]` unit cube) conventions, both converted to
  `ac3::oba::Position`'s room-anchored `[0, 1]`/`[0, 1]`/`[-1, 1]` one. Checked against the
  standard's own axis-direction text at the cardinal points, and empirically against this
  project's own existing ring-position constants (`tests/oba/test_atmos_motion.cpp`'s `kL`/`kR`/`kSR`)
  — the BS.2076-2 `M+030`/`M-030`/`M-110` speaker-label azimuths reproduce those exact values
  through this conversion.
- **Absolute timeline time** for a channel's automation is `object.start_s + block.rtime_s` — two
  levels, not three. BS.2076-2 Table 24 defines `audioObject`'s own `start` as relative to the
  *programme's* start directly, and §5.6.7 confirms this holds through nesting (a nested object's
  own `start` is never added to its parent's). `audioProgramme`'s own optional `start`/`end`
  (Table 37) is a separate video-alignment concept, not a third term in this sum — and
  `ac3adm::AudioProgramme` does not even carry a `start_s` field to add.
- **Channel resolution** walks `audioProgramme` → `audioContent` → `audioObject` (recursing through
  nested `audioObjects`, with a cycle guard per §5.6.7's own prohibition on reference loops) →
  `audioPackFormat` → `audioChannelFormat`, then resolves each channel's `audioTrackUIDRef`
  through `<chna>` to its physical PCM channel in `AdmDocument::audio`.

## Object extent and channel lock

`width`/`height`/`depth` map straight through to `ac3::oba::ObjectSize` on every keyframe this
bridge produces. BS.2076-2 Table 15/16/17's extents and TS 103 420 §5.6.1.2's
`object_width`/`object_depth`/`object_height` are the same normalized `[0, 1]` quantity on the
same three axes, so this is a rename rather than a conversion, and §10.3's interpolation of them
between blocks is exactly what `KeyframePath` already does with position and gain.

`channelLock` maps to `ObjectPlacement::snap` — BS.2076-2 §10.2 and TS 103 420 §5.6.1.5.1
("Channel lock") describe the same instruction to a renderer: place the object at its nearest
speaker instead of panning between speakers. `maxDistance` has no image: `b_object_snap` is one
bit with no distance to condition it on, so a conditioned `channelLock` becomes an unconditioned
snap, which is the closest thing the syntax can say.

Both reach the bitstream and stop there. `AtmosEncoder` still folds every object into its 5.1 bed
as a point source, for the reason
[Spatial & Atmos objects](spatial-and-atmos.md#extent-and-rendering-constraints) gives: spreading
an object in the downmix would have the receiving renderer spread it a second time.

## What does not get mapped

- **`diffuse`** (§10.1) is parsed by `ac3adm` and dropped. It is a direct-versus-diffuse balance,
  not an extent; OAMD has no field for it, and folding it into `object_size` would misreport a
  decorrelation instruction as a physical size.
- **`zoneExclusion`** (§10.4) and **`objectDivergence`** (§10.5) are not parsed by `ac3adm` at all
  — `ac3adm::AudioBlockFormat` has no field for either, so nothing is dropped so much as never
  read. Both have a partial image in OAMD (`zone_constraints_idx`, and `obj_div_block` in the
  `extended_object_element`), but neither is a clean mapping: BS.2076-2 excludes arbitrary
  axis-aligned cuboids, while TS 103 420 Table 20 offers six named presets and no way to express
  anything else, and `obj_div_block` rides in an element `oba::build_payload` does not write. The
  decode side reads both (`DynamicObject::zone`, `DynamicObject::divergence`); the ADM-to-OAMD
  direction is left unmapped rather than approximated.
- **`screenRef`** and the Matrix/Binaural-specific sub-elements are likewise unparsed, per
  `ac3adm::model.hpp`'s own scope note.

`ObjectPlacement::zone` and `ObjectPlacement::enable_elevation` exist and are transmitted — a
caller constructing paths directly can set them; it is only the ADM-derived route that leaves them
at their defaults.

## Bridging IAB (roadmap IM1 phase 3)

`build_iab(std::span<const ac3iab::IABitstreamFrame> frames)` maps a whole parsed Immersive Audio
Bitstream sequence — `ac3iab::parse_iabitstream()` or `ac3iab::parse_mxf_iab()`'s own return value,
unmodified — onto the identical destination shape `build()` produces for ADM, in a new header,
`ac3/admbridge/iab_bridge.hpp`.

IAB has no whole-file object graph the way ADM's `audioProgramme → audioContent → audioObject` tree
does — it is a flat sequence of self-contained `IaFrame`s, each carrying its own Bed/Object metadata
scoped to that frame alone (§9.2/§9.4). `build_iab()` therefore does its own two-pass walk: an
identity pass unions every unconditionally-Activated top-level Bed channel/Object across every
frame, keyed by §10.3.1's own MetaID ("the ID that allows the system to track metadata information
between audio frames" — for a Bed channel, combined with its own ChannelID per §10.3.5's note on
ST 2098-1's Channel Identifier function) — fixing `AtmosEncoder`'s channel count and order once, the
same role `collect_leaf_objects()` plays for `build()`'s own ADM walk. A timeline pass then builds
one `ObjectPath` per identity spanning the whole sequence: a frame where that identity is absent, or
only conditionally Activated (an alternate-target-environment mix per §10.3.2/§10.5.1's own
Activation/UseCase fields — the same "pick the one primary set" choice `build()` already makes among
several `audioProgramme`s), contributes no new keyframe — silence-filled PCM, the timeline simply
holds/interpolates through the gap — rather than shrinking the channel count.

Only top-level `IaFrame::beds`/`objects` are walked — nested `BedDefinition`/`BedRemap`/
`ObjectDefinition`/`ObjectZoneDefinition19` children (§9 Table 4) are deliberately not recursed
into: Annex C.1's own nesting allowance exists for alternate/derived submixes, not the primary
content this bridge selects one of.

**Bed channels have no per-block position data at all** (unlike ADM's `audioBlockFormat` sequence)
— a Bed channel's own ChannelID (§10.3.5 Table 19) is a closed, physical-position vocabulary
resolved once via `ac3::oba::bed_label_position()`, the same pinned-placement convention bed
channels already get from ADM's `speakerLabel`; only `ChannelGain` (§10.3.8) can legitimately vary
frame to frame, so a Bed channel's timeline is one keyframe per frame it is present in. An LFE Bed
channel (ChannelID `0xD`, or `0x86`/`0x87`'s BS.2051-2 `LFE1`/`LFE2` aliases) routes at gain 0 /
`lfe_send` 1, the same convention `build_channel_path()` documents for ADM.

**Table 19's cinema channel vocabulary is richer than `ac3::oba::BedLabel`'s own consumer-layout
one in exactly one place**: it names three distinct surround zones per side (Side Surround,
Surround, Rear Surround) where `BedLabel` has only two slots (`kLs`/`kRs`, `kLb`/`kRb`).
"Surround" (`0x6`/`0xA`) maps to `kLs`/`kRs` (the canonical 5.1 pair) and "Rear Surround"
(`0x7`/`0x8`) to `kLb`/`kRb` (7.1's additional back pair, the closest conceptual match); "Side
Surround" (`0x5`/`0x9`) has no equivalent and is refused — `BridgeError::kUnsupportedIabChannel`,
not silently collapsed onto an existing slot. Several other Table 19 codes (Left/Right Center,
Center Height, the `*Height` variants of Side/Rear Surround, Left/Right Top Surround, Top
Surround, and the whole `0x18`-`0x7F` D-Cinema-reserved range) are refused the same way,
deliberately, rather than guessed at without the external documents Table 19 itself defers to
(SMPTE ST 428-12/ST 2098-5) for their exact geometry.

**Position conversion needs no formula at all.** `iab_position_to_room()` (`coordinates.hpp`) is a
direct passthrough: §11.1's `x`/`y` (0 left/front wall to 1 right/back wall) already match
`ac3::oba::Position`'s own convention exactly, and its `z` — "0 corresponds to a horizontal plane
at... the height of the main screen Loudspeakers... 1 corresponds to the ceiling" — anchors its
zero at the same screen/ear-height reference `oba::Position`'s own `z = 0` does, just never
expressing anything below it (IAB's `[0, 1]` is the upper half of `oba::Position::z`'s own
`[-1 floor, +1 ceiling]` range). This is this bridge's own judgement call — no clause states the
two specs share a reference height — the same status the ADM conversion's own one undocumented gap
has above.

`ObjectSpread` (§10.5.15-17) and the 9-zone `ObjectZoneControl` (§10.5.11-14) are not mapped, for
the identical reason ADM's own `width`/`height`/`depth`/`zoneExclusion` are not (see "What does not
get mapped" above): spreading an object in the downmix would have the receiving renderer spread it
a second time, and `ZoneConstraint`'s six named presets have no clean image for either IAB shape.

`IabBridgeResult` is a **new** struct, not a reuse of `BridgeResult`: PCM is concatenated across
many independently-parsed frames, so `IabBridgeResult::pcm` is **owned**
(`std::vector<std::vector<float>>`), not borrowed the way `BridgeResult::pcm` is from a single
caller-owned `AdmDocument` — there is no equivalent single upstream object here to borrow spans
from once `build_iab()` returns.

## Write direction (roadmap IM2)

`write()` takes a `WriteInput` — a sample rate plus one `WriteChannel` per channel to place in the
master, in any order — and returns an `ac3adm::AdmDocument` ready for `ac3adm::write_bw64()`. A
channel is either a bed channel (`bed_label` set — written as a static `DirectSpeakers` channel
pinned at `ac3::oba::bed_label_position()`, `updates` unused) or a dynamic object (`bed_label`
empty — written as an `Objects` channel whose `audioBlockFormat` sequence comes from `updates`).

Scoped to exactly what this project's own decoder ever produces: a dynamic-object-only-or-single-
bed-instance programme (`Eac3Decoder` never emits ISF objects, several bed instances, or
non-standard Table 13 assignments — see `oamd.hpp`'s own `Program` comment), no nested
`audioObject`s, cartesian positions only. `ac3cli decode`'s own `--adm` wiring (`decode.cpp`)
additionally only attempts this for a `dynamic_only` programme — a genuine bed program (channel-
based-immersive third-party content) is warned about and skipped, not written incorrectly.

**`WriteObjectUpdate` is the write-direction input for one OAMD update**, timestamped in absolute
samples from the start of the whole decode (not the access unit it arrived in) — a caller
assembles the list by walking every decoded access unit's own
`DecodedAccessUnit::object_metadata->blocks` in file order and adding each block's own
`sample_offset` to a running total of samples already emitted. `build_block_formats()` (internal
to `bridge.cpp`) turns this into one `audioBlockFormat` per update: TS 103 420's own per-block
model — a value takes effect at `sample_offset`, reached over `ramp_duration` samples, then held —
is already, block for block, BS.2076-2 §10.3's `jumpPosition = 1` + `interpolationLength` case, so
this direction needs none of `build_channel_path()`'s own read-direction case analysis; the first
update in a channel's sequence becomes a plain hold (§10.3's "the first block covers its entire
length regardless of `jumpPosition`" rule), every later one an explicit jump/ramp.

`room_to_adm_cartesian()` (`coordinates.hpp`) is the algebraic inverse of `adm_cartesian_to_room()`
above (`x_adm = 2·x_room - 1`, `y_adm = 1 - 2·y_room`, `z_adm = z_room`) — this writer only ever
emits cartesian ADM, matching the Dolby Atmos Master ADM Profile's own shape, so there is no
matching polar inverse.

## API

```cpp
enum class BridgeError : std::uint8_t {
    kNoProgramme, kProgrammeNotFound, kUnresolvedReference, kObjectReferenceCycle,
    kUnsupportedType, kChannelTrackMismatch, kNoAudioForTrack, kEmptyBlockSequence,
    kTooManyChannels, kEmptyInput,
    kEmptyIabStream, kUnsupportedIabChannel, kNoIabEssenceForChannel,  // build_iab() only
};
std::string_view describe(BridgeError error);

std::expected<ac3::oba::ObjectPath, BridgeError> build_channel_path(
    const ac3adm::AudioChannelFormat& channel, double object_start_s, bool force_lfe);

struct BridgeResult {
    std::vector<std::string> channel_ids;
    std::vector<bool> is_bed;
    std::vector<bool> is_lfe;
    std::vector<ac3::oba::ObjectPath> paths;   // pass directly to evaluate_placements
    std::vector<std::span<const float>> pcm;   // borrowed from the AdmDocument passed to build()
    std::uint32_t sample_rate = 0;
};
std::expected<BridgeResult, BridgeError> build(const ac3adm::AdmDocument& document,
                                               std::string_view programme_id = {});

ac3adm::CartesianPosition polar_to_adm_cartesian(const ac3adm::PolarPosition& polar);
ac3::oba::Position adm_cartesian_to_room(const ac3adm::CartesianPosition& cartesian);
ac3::oba::Position adm_position_to_room(const ac3adm::Position& position);
ac3adm::CartesianPosition room_to_adm_cartesian(const ac3::oba::Position& room);
ac3::oba::Position iab_position_to_room(const ac3iab::Position& position);  // direct passthrough

struct IabBridgeResult {
    std::vector<std::string> channel_ids;
    std::vector<bool> is_bed;
    std::vector<bool> is_lfe;
    std::vector<ac3::oba::ObjectPath> paths;
    std::vector<std::vector<float>> pcm;       // OWNED - see "Bridging IAB" above
    std::uint32_t sample_rate = 0;
};
std::expected<IabBridgeResult, BridgeError> build_iab(
    std::span<const ac3iab::IABitstreamFrame> frames);

struct WriteObjectUpdate {
    std::uint64_t sample_offset = 0;
    int ramp_duration_samples = 0;             // ac3::oba::UpdateBlock::ramp_duration verbatim
    ac3::oba::DynamicObject state;
};
struct WriteChannel {
    std::string name;
    std::span<const float> pcm;
    std::optional<ac3::oba::BedLabel> bed_label{};    // set: bed/LFE; empty: dynamic object
    std::span<const WriteObjectUpdate> updates{};     // dynamic objects only
};
struct WriteInput {
    std::uint32_t sample_rate = 0;
    std::vector<WriteChannel> channels;
};
std::expected<ac3adm::AdmDocument, BridgeError> write(const WriteInput& input);
```

`BridgeResult` is deliberately struct-of-arrays, not one struct per channel — `paths` is directly
usable as the `std::span<const ac3::oba::ObjectPath>` `ac3::oba::evaluate_placements` wants, with
no projection step. `channel_count() <= 15`: `AtmosEncoder`'s own constructor `objects` parameter
is dynamic objects only, with the bed's own LFE bookkeeping as an implicit, always-present 16th
(TS 103 420 §8.3.2.2 caps the total at 16) — the same cap `ac3cli`'s `run_atmos_encode`/
`run_atmos_path` already enforce, reused here rather than re-derived. `sample_rate` is the raw
`ac3adm::PcmAudio::sample_rate`, unconverted — mapping it to `ac3::SampleRate` (and rejecting an
unsupported rate) is left to the caller, the same way every existing WAV-reading entry point
already does that itself.

`bitrate_kbps`/`dialnorm` and every other `AtmosConfig` field are encoding choices ADM data does
not carry at all — `build()` deliberately does not invent defaults for them; constructing
`AtmosConfig` is the caller's job.

## Tests

`tests/admbridge/test_adm_bridge.cpp` covers coordinate conversion (against BS.2076-2 §8's cardinal points
and this project's own existing ring constants), `build_channel_path`'s full §10.3 state machine
(single block, continuous-glide blocks, instant-jump blocks, ramp-then-hold blocks, the
first-block-always-holds override, the LFE override), `build()`'s graph-walking error paths
(unresolved references, a reference cycle, an unsupported pack type, a channel/track-count
mismatch, the 15-channel cap, default programme selection), and one flagship test that builds a
real byte-level BW64 fixture (two DirectSpeakers bed channels plus one Objects channel that holds
at one ring position and then jumps to another), parses it with the real `ac3adm::parse_bw64()`,
bridges it, and drives a real `ac3::oba::AtmosEncoder`/`ac3::Eac3Decoder` round trip — confirming
the decoded bitstream's channel energy actually lands where the authored ADM positions and hold/
jump timing say it should, the same standard `tests/oba/test_atmos_motion.cpp`'s own flagship test
holds itself to.

`tests/cli/test_cli_atmos_adm.cpp` (phase 3) covers the same fixture shape one level up: it runs the
real, built `ac3cli` binary's `atmos-adm` command as a subprocess against a real ADM BWF file on
disk, then decodes what that binary actually wrote and checks the same channel-energy assertions —
proving the CLI's own argument parsing and its `parse_bw64` → `build` → `AtmosEncoder` wiring, not
just the library API in isolation — plus two error-path cases (`BridgeError::kNoProgramme`, a
malformed/non-RIFF file) confirming `describe()` reaches the terminal rather than an opaque crash
or exit code.

`tests/admbridge/test_iab_bridge.cpp` covers `iab_position_to_room`'s cardinal points, the Table 19
→ `BedLabel` mapping (both the codes that resolve and the ones `build_iab()` refuses), MetaID
cross-frame identity (the same MetaID+ChannelID across several frames is one channel, not several;
an absent frame silence-fills rather than shrinking the channel count), sub-block keyframe timing
(a dedicated fixture proving each active sub block's keyframe lands at its own *end* time, not its
start), and one flagship test with the identical rigor `test_adm_bridge.cpp`'s own holds itself to
— a real byte-level IAB fixture (one Bed Center channel, one Object holding hard right then
jumping hard left), parsed with the real `ac3iab::parse_iabitstream()`, bridged, and driven through
a real `AtmosEncoder`/`Eac3Decoder` round trip confirming decoded channel energy lands where
authored. `tests/cli/test_cli_atmos_iab.cpp` (phase 3's own CLI wiring) covers the same fixture
shape one level up, the same way `test_cli_atmos_adm.cpp` does for `atmos-adm`.

---

See also: [ADM / BW64 reading](adm.md) — the phase-1 parser this module consumes; [IAB
reading](iab.md) — `ac3iab::ac3iab`, the other parser this module consumes (phases 1-2); [Spatial &
Atmos objects](spatial-and-atmos.md) — `ac3::oba::AtmosEncoder`, `ac3::oba::motion`, and
`ac3::oba::Position`'s own room-anchored coordinate convention this module's `coordinates.hpp`
converts into.
