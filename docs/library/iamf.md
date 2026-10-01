# IAMF writing: `iclforge::iamf`

`iclforge/iamf/iamf.hpp`, library `iclforge::iamf`. A standalone writer for AOM's IAMF (Immersive Audio Model
and Formats) v1.1.0 — the bitstream format behind Eclipsa Audio. Like `iclforge::matroska`,
`iclforge::mp4` and `iclforge::iab`, it links nothing from `iclforge::ac3`: it knows nothing about
AC-3, E-AC-3 or the JOC/Atmos object layer, and takes already-rendered PCM in, the same way
`iclforge::mp4::AudioTrack::codec_config` takes an opaque caller-built box payload.

**Why a writer exists at all.** IAMF's codec list is Opus, AAC-LC, FLAC and LPCM — E-AC-3 can
never be carried inside it. This is therefore a decode → rewrap bridge rather than a new encoder
output. A caller decoding a stream that is
already coded as a 7.1.4 channel layout (`iclforge::ac3::plan::LayoutId::k714` — an independent substream
plus two E-AC-3 dependents) gets the 12 discrete channels straight off
`iclforge::ac3::Eac3Decoder::decode_access_unit`; this module needs them permuted into its own channel
order and handed over as PCM.

**Two routes to the same ecosystem.** [ADM / BW64 writing](adm.md)'s `write_bw64()` already opens
an *indirect* one: AOM's own `iamf-tools` encoder accepts ADM-BWF input, so a decoded programme
written as an ADM master already reaches IAMF via a second, external encoder — but
only for IM2's own scope (dynamic-object-only programmes, cartesian positions). `iclforge::iamf`
writes the IAMF bitstream directly, for any 7.1.4-coded programme this decoder can render, with
nothing else in the chain.

Default-on (`ICLFORGE_BUILD_IAMF`), installed/exported the same way as the container writers —
unlike `iclforge::adm`, it has no third-party dependency to opt in around. The vcpkg port and
the Conan recipe install it where asked for, off by default: `vcpkg install iclforge[iamf]`, or
`-o "iclforge/*:iamf=True"` (see [Using the libraries](index.md)).

```cpp
iclforge::iamf::AudioTrack track{.samples_per_frame = iclforge::ac3::kSamplesPerFrame};
std::vector<iclforge::iamf::Frame> frames;
// ... frames.push_back(...) for each temporal unit ...

const auto file = iclforge::iamf::mux(track, frames);
if (!file) {
    fmt::printf("iclforge::iamf::mux failed: %.*s\n", static_cast<int>(iclforge::iamf::describe(file.error()).size()),
                iclforge::iamf::describe(file.error()).data());
    return 1;
}
```

Full program: [`examples/mux_iamf.cpp`](https://github.com/iainchesworthlabs/iclforge/blob/main/examples/mux_iamf.cpp) — the whole
round trip: encode a synthetic 7.1.4 E-AC-3 access unit, decode it with `iclforge::ac3::Eac3Decoder`,
permute the result, and mux it.

## The permutation a caller does

`iclforge::iamf::Frame::channels` is planar, ordered exactly as this module's Audio Element OBU declares
(IAMF §3.6.2, `loudspeaker_layout` = 7, "7.1.4ch"): **L, C, R, Lss, Rss, Lrs, Rrs, Ltf, Rtf, Ltb,
Rtb, LFE**. A decoded `iclforge::ac3::DecodedAccessUnit::channels` is ordered by Table E2.5 *bit* order
instead (`DecodedAccessUnit::layout`), which is neither this order nor WAV's — so
`examples/mux_iamf.cpp` builds the permutation itself, one `layout.index_of(Location::kX)` call
per IAMF channel:

```cpp
constexpr std::array<Location, 12> kIamf714Order{
    Location::kLeft,   Location::kCentre, Location::kRight,
    Location::kLeftSurround, Location::kRightSurround,
    Location::kLrs,    Location::kRrs,
    Location::kVhl,    Location::kVhr,   // Table E2.5's front-height pair = IAMF's Ltf/Rtf
    Location::kLts,    Location::kRts,   // Table E2.5's rear-height pair  = IAMF's Ltb/Rtb
    Location::kLfe,
};
```

This permutation is not part of `iclforge::iamf` itself — the module stays codec-blind, the same
reason `iclforge::mp4::AudioTrack::codec_config`'s ETSI TS 102 366 payload is built by the *caller*
(`iclforge::ac3::io::build_codec_config_box`), not by `iclforge::mp4` — so it lives in the example, not a bridge
library. There is no `iamfbridge` module mirroring `iclforge::admbridge`; the mapping is small,
one-directional, and this is what it looks like.

## What gets written

Every OBU/box field is transcribed directly from the published IAMF v1.1.0 specification
(section numbers cited throughout [`src/iamf/src/obu_detail.hpp`](https://github.com/iainchesworthlabs/iclforge/blob/main/src/iamf/src/obu_detail.hpp) and
[`isobmff_detail.hpp`](https://github.com/iainchesworthlabs/iclforge/blob/main/src/iamf/src/isobmff_detail.hpp)), per this project's clean-room rule — `libiamf` and AOM's Open
Audio Renderer are oracles used to validate the output, never sources this code was transcribed
from.

- **IA Sequence Header OBU**: `primary_profile`/`additional_profile` = Simple Profile — one
  Audio Element, 12 channels, comfortably inside Simple's own ceiling.
- **Codec Config OBU**: `codec_id` = `ipcm`, at `AudioTrack::sample_rate`/`bit_depth`
  (§3.11.4's own allowed sets: `{44100,16000,32000,48000,96000}` Hz, `{16,24,32}`-bit,
  little-endian).
- **Audio Element OBU**: `CHANNEL_BASED`, one layer, `loudspeaker_layout` = 7 (7.1.4ch) — 7 Audio
  Substreams (5 coupled stereo pairs + Centre + LFE), ordered per §3.6.3.3 (coupled before
  non-coupled; surround pairs before top pairs; front before side/rear within each).
- **Mix Presentation OBU**: one sub-mix, the one Audio Element, and two loudness layouts — the
  Stereo one §3.7 makes mandatory for every sub-mix, plus the programme's own 7.1.4 layout.
  `AudioTrack::stereo_loudness`/`layout_714_loudness` supply the numbers; this module measures
  nothing itself (DSP-free, like every other container module here) — see `iclforge::iamf::LoudnessInfo`'s
  own doc comment on why the caller's own placeholder is a structurally valid default.
- **Audio Frame OBUs**: one per substream per frame, packed using IAMF's own compact
  `Audio_Frame_ID0`..`ID17` OBU types (no separate substream-id field needed for the first 18
  substreams), concatenated into one IA Sample per `iclforge::iamf::Frame`.
- **ISO-BMFF encapsulation** (IAMF §6): `iamf`-branded `ftyp`, one `trak` whose `stsd` carries an
  `iamf` `IASampleEntry` wrapping an `iacb` `IAConfigurationBox` (the four Descriptor OBUs above),
  and `mdat` holding one IA Sample per frame.

## What it does not cover

- **Object elements.** IAMF v1.1.0 has no object-based audio element type at all; v2.0's
  working-group-approved draft (2026-07-27) adds one, but is not final.
- **An OBU reader.** Nothing in this repository can read an IAMF file back.
- **The standalone raw-OBU representation** (IAMF §5) — only the ISO-BMFF encapsulation (§6) is
  implemented; the underlying OBU bytes are identical either way, so adding the bare form later
  needs no change to `obu_detail.hpp`.
- **No Parameter Block OBUs, no Temporal Delimiter OBU, no trimming** — none are mandatory for a
  single static layer (see `iclforge/iamf/iamf.hpp`'s own header comment for the citations), and this
  writer does not need them.
- **A fragmented or live writer.** `iclforge::iamf::mux()` is batch-only, matching where
  `iclforge::mp4::mux`/`iclforge::matroska::mux` themselves started.

---

See also: [ADM / BW64 reading and writing](adm.md) — the indirect route to the same ecosystem;
[Muxing & sinks](muxing-and-sinks.md) — `iclforge::mp4`/`iclforge::matroska`, the container modules
this one's shape is modeled on; [Decoding](decoding.md) — `iclforge::ac3::Eac3Decoder`, this module's own
source of PCM.
