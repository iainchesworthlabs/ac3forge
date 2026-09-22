#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The Immersive Audio Bitstream (IAB) element graph, per SMPTE ST 2098-2:2022, "Immersive
// Audio Bitstream Specification" (the bitstream Netflix carries inside IMF via ST 2067-201,
// and the format Dolby Atmos cinema masters ship in). Section numbers throughout this header
// and ac3iab.hpp/src/*.cpp refer to that document.
//
// This header is deliberately just data, the same "plain aggregate, no behaviour" shape
// src/ac3adm/include/ac3adm/model.hpp uses for the Audio Definition Model: every type here
// mirrors one bitstream element or one field group, with prefix/escape codes already resolved
// into their final linear/physical values (gains, positions, spreads) rather than left as the
// raw bitstream code - matching how ac3adm::AudioBlockFormat::gain is always linear regardless
// of the source's own gainUnit. Roadmap item IM1 phase 1 (see ROADMAP.md): a standalone
// `ac3iab::` reader in the `ac3adm::` mould - it knows nothing about AC-3, E-AC-3 or JOC, and a
// later phase 3 (mapping onto ac3::admbridge's ObjectPath layer) is what will finally make this
// module's output useful to the codec.
//
// AudioDataDLC (§9.6/§10.7, Annex B) is the one element this phase reads only the identity of:
// its lossless entropy-coded residual is left as an opaque byte span (AudioDataDlc::coded)
// rather than decoded - see that struct's own comment. AudioDataPCM (§9.7/§10.8) is this
// phase's actual audio-essence target and is fully decoded.

namespace ac3iab {

// §5.3 Table 1: an 8-bit code naming the Target Environment an element is Activated for. Kept
// as a raw code rather than a closed enum: per §10.3.3/§10.4.1/§10.5.2, "UseCase codes other
// than those listed in Table 1 shall be ignored" - a reader has no reason to reject one, so
// there is no invalid representable value to guard against.
using UseCaseCode = std::uint8_t;
inline constexpr UseCaseCode kUseCaseAlwaysUse = 0xFF;

// §10.3.2/§10.5.1: the ConditionalBed/ConditionalObject bit plus, when set, the UseCase code
// that follows it (BedUseCase/ObjectUseCase). `conditional == false` means the element is
// Activated unconditionally and `use_case` was never present in the bitstream (left at 0,
// not meaningful).
struct Activation {
    bool conditional = false;
    UseCaseCode use_case = 0;
};

// §11.1: a position on the unit cube - x: 0 left wall to 1 right wall, y: 0 front wall to 1
// back wall, z: 0 screen/surround height to 1 ceiling. Each axis is independently decoded per
// §5.4 (ObjectPosX/Y via the DistanceXY formula, ObjectPosZ via DistanceZ, §10.5.7); values
// are stored already resolved into [0, 1] (or, for input outside the formula's own valid
// domain, whatever that affine map produces - see the reader's own comment on the fields it
// does not clamp).
struct Position {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

// §10.5.15 Table 26: which of the three ObjectSpread encodings a sub block used. kNone (the
// bitstream's OBJECT_SPREAD_NONE) carries no ObjectSpread value at all - a true point source.
enum class ObjectSpreadMode : std::uint8_t {
    kLowRez = 0x0,  // OBJECT_SPREAD_LOWREZ - one 8-bit code, applied isotropically
    kNone = 0x1,    // OBJECT_SPREAD_NONE - point source, no value follows in the bitstream
    kOneD = 0x2,    // OBJECT_SPREAD_1D - one 12-bit code, applied isotropically
    kThreeD = 0x3,  // OBJECT_SPREAD_3D - three independent 12-bit codes, one per axis
};

// §10.5.16-17: the resolved spread extent(s), always via the DistanceZ formula (§5.4)
// regardless of which axis a value applies to (the field descriptions say so explicitly,
// unlike ObjectPos's per-axis DistanceXY/DistanceZ split). For kLowRez/kOneD, `x == y == z`
// (the single decoded value, replicated so callers do not need to special-case the mode to
// read an isotropic spread); kThreeD carries three independently-decoded values; kNone
// leaves all three at 0.
struct ObjectSpread {
    ObjectSpreadMode mode = ObjectSpreadMode::kNone;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

// §10.3.12-13 / §10.5's own copy of the same field (Table 22): what kind of audio content an
// element carries. Bit 0 (`not_indicated`) collapses the whole byte per Table 22's own note
// ("bits 1-6 shall be ignored" when it is set) - this reader still reports whichever of
// dialog/music/effects/foley/ambience bits happened to also be set, leaving that
// interpretation to the caller rather than silently dropping them. `text` is populated from
// the NUL-terminated ASCII string that follows in the bitstream when bit 7 is set (Annex C.1
// item 2 caps it at 64 bytes including the terminator; not enforced by this reader).
struct AudioDescription {
    bool not_indicated = false;
    bool dialog = false;
    bool music = false;
    bool effects = false;
    bool foley = false;
    bool ambience = false;
    std::optional<std::string> text;
};

// §10.3.5-11: one channel of a Bed. `gain` and `decorrelation` are already resolved from
// their prefix codes (§5.5 Table 20 / Table 21) to a linear [0, 1] value; `decorrelation` is
// nullopt when ChannelDecorInfoExists was 0 ("no decorrelation shall be applied").
struct BedChannel {
    std::uint32_t channel_id = 0;      // §10.3.5, Table 19, Plex(4)
    std::uint32_t audio_data_id = 0;   // §10.3.6, Plex(8); 0 means no/silent asset
    double gain = 1.0;
    std::optional<double> decorrelation;
};

struct BedRemap;

// §9.2/§10.3: metadata plus pointers to audio essence for one frame of one Bed. Recursive per
// Table 4's Allowed-Children rule (a BedDefinition's Children are BedDefinition and
// BedRemap); Annex C.1 item 3a caps real nesting at two levels (a Child BedDefinition may not
// itself have a Child BedDefinition), which this reader does not enforce - a third level, if
// present, still parses.
struct BedDefinition {
    std::uint32_t meta_id = 0;  // §10.3.1, Plex(8)
    Activation activation;       // ConditionalBed/BedUseCase, §10.3.2-3
    std::vector<BedChannel> channels;
    AudioDescription description;
    std::vector<BedDefinition> beds;    // Child BedDefinition elements
    std::vector<BedRemap> remaps;       // Child BedRemap elements
};

// §9.3/§10.4: one sub block's remap matrix. `has_remap_info` mirrors RemapInfoExists (always
// true for sub block 0, which carries no such bit at all); when false, `destination_channel_ids`
// and `gains` are left empty and the caller is expected to carry the previous sub block's
// values forward - this reader stores only what the bitstream actually carried, the same
// "PanInfoExists" convention ObjectPanSubBlock below uses.
struct BedRemapSubBlock {
    bool has_remap_info = true;
    std::vector<std::uint32_t> destination_channel_ids;  // size DestinationChannels, Plex(4), Table 19
    std::vector<std::vector<double>> gains;               // [oChan][iChan], size DestinationChannels x SourceChannels
};

// §9.3/§10.4: describes how to remap a Bed mixed for one Loudspeaker configuration to
// another. Always a Child of exactly one BedDefinition (Table 4); `sub_blocks` has one entry
// per NumPanSubBlocks (§9.3's own note: "NumRemapSubBlocks... is the same as NumPanSubBlocks",
// see IaFrame::num_pan_sub_blocks() below).
struct BedRemap {
    std::uint32_t meta_id = 0;                // §10.4's own MetaID (defined identically to §10.3.1)
    UseCaseCode use_case = 0;                  // §10.4.1 RemapUseCase - unconditional, always present
    std::uint32_t source_channels = 0;         // §10.4.2, Plex(4)
    std::uint32_t destination_channels = 0;    // §10.4.3, Plex(4)
    std::vector<BedRemapSubBlock> sub_blocks;
};

// §10.5.11 Table 24 / §10.6 Table 28: the two fixed zone layouts a decoder can weigh Object
// rendering by. Neither this reader nor the bitstream itself names the zones beyond bitstream
// order (Table 24/28 list a Description column only, no symbolic code) - callers that need the
// zone names look them up by index against the published table.
inline constexpr std::size_t kZoneCount = 9;     // Table 24, §10.5.11-14
inline constexpr std::size_t kZone19Count = 19;  // Table 28, §10.6

// §9.5/§10.6: an optional, more detailed 19-zone alternative to ObjectDefinition's own 9-zone
// control, updated on the same per-sub-block cadence. `has_zone_info` mirrors ZoneInfoExists
// (always true for sub block 0); when false `zone_gains` is left default (all zero) and the
// caller carries the previous sub block's values forward, mirroring ObjectPanSubBlock's own
// PanInfoExists convention.
struct Zone19SubBlock {
    bool has_zone_info = true;
    std::array<double, kZone19Count> zone_gains{};
};

// §9.5: an optional Child of an ObjectDefinition (Table 4: 0..1) that, when present, replaces
// that ObjectDefinition's own 9-zone ObjectZoneControl gains with a finer 19-zone set for every
// sub block.
struct ObjectZoneDefinition19 {
    std::vector<Zone19SubBlock> sub_blocks;
};

// §10.5.4-19: one Object sub block's panning metadata. `has_pan_info` mirrors PanInfoExists
// (always true for sub block 0, which carries no such bit). When false, every field below is
// left default-constructed and the caller is expected to carry the previous sub block's
// values forward - this struct stores only what the bitstream actually carried for that sub
// block, not an interpolated/resolved timeline (that is rendering behaviour, out of scope for
// a "codec-blind" reader - see this header's own top comment).
struct ObjectPanSubBlock {
    bool has_pan_info = true;

    double gain = 1.0;                              // §10.5.5-6, Table 20
    Position position;                                // §10.5.7, §11.1
    bool snap = false;                                // §10.5.8
    // §10.5.9-10: nullopt exactly when ObjectSnapTolExists was 0, in which case the tolerance
    // is DEFAULT_OBJ_SNAP_TOL (§11.2 Table 32 = 1.0, i.e. "always snap") - left as nullopt
    // rather than pre-filled with that constant so a caller can tell "not encoded, use the
    // spec default" apart from "encoded, and 1.0 was the explicit tolerance".
    std::optional<double> snap_tolerance;
    // §10.5.11-14, Table 24/25: nullopt exactly when ObjectZoneControl was 0 ("zone control is
    // not used"), otherwise one already-resolved [0, 1] gain per zone in Table 24's order.
    std::optional<std::array<double, kZoneCount>> zone_gains;
    ObjectSpread spread;                               // §10.5.15-17
    double decorrelation = 0.0;                        // §10.5.18-19, Table 27 - always present, unlike a Bed channel's
};

// §9.4/§10.5: metadata, most importantly position, plus a pointer to audio essence for one
// frame of one Audio Object. Recursive per Table 4 (an ObjectDefinition's Children are
// ObjectDefinition and ObjectZoneDefinition19); Annex C.1 item 3b caps real nesting at two
// ObjectDefinition levels, not enforced by this reader. `sub_blocks` has one entry per
// NumPanSubBlocks (§10.5.3 Table 23, see IaFrame::num_pan_sub_blocks() below).
struct ObjectDefinition {
    std::uint32_t meta_id = 0;               // Table 8's own MetaID (defined identically to §10.3.1)
    std::uint32_t audio_data_id = 0;          // Table 8's own AudioDataID (defined identically to §10.3.6)
    Activation activation;                     // ConditionalObject/ObjectUseCase, §10.5.1-2
    std::vector<ObjectPanSubBlock> sub_blocks;
    AudioDescription description;
    std::vector<ObjectDefinition> objects;     // Child ObjectDefinition elements
    std::optional<ObjectZoneDefinition19> zone19;
};

// §9.6/§10.7, Annex B: losslessly-coded mono audio essence. This phase reads only the
// element's own identity - the forward-adaptive lattice predictor plus Rice/Golomb or
// direct-PCM entropy-coded residual, including its sample-rate-scalable 48/96 kHz base and
// extension layering, is Annex B's own separate piece of work and is left here as an opaque,
// unparsed byte span rather than decoded (see ROADMAP.md's IM1 entry). `coded` is exactly
// DLCSize bytes (§10.7.2) - the element's own declared size, not independently re-derived.
struct AudioDataDlc {
    std::uint32_t audio_data_id = 0;  // §10.7.1, Plex(8); never 0 for a real asset
    std::vector<std::byte> coded;
};

// §9.7/§10.8: one frame of one monaural PCM waveform, decoded to normalized [-1, 1) samples -
// the same convention ac3adm::PcmAudio and ac3::io::WavData use elsewhere in this project.
// §10.8.1: PCMData is little-endian per sample (16 or 24 bits, per the parent IAFrame's own
// BitDepth, §10.2.3), each byte transmitted MSB-first - ordinary little-endian PCM, decoded
// here rather than left as raw bytes since (unlike AudioDataDLC) there is no entropy coding
// step whose opacity is worth preserving.
struct AudioDataPcm {
    std::uint32_t audio_data_id = 0;  // Plex(8)
    std::vector<float> samples;        // SampleCount per Table 18, normalized [-1, 1)
};

// §9.8/§10.9: identifies the vendor and tool that created the frame. Decoders may skip this
// element entirely (§9.8); this reader still parses it since it costs nothing extra once the
// element is being visited at all.
struct AuthoringToolInfo {
    std::string uri;  // AuthoringToolURI - NUL-terminated ASCII, an RFC 3986 URI with a
                       // vendor-registered authority per §10.9.1; not validated here
};

// §9.9/§10.10: undefined user data identified by a SMPTE Universal Label. Decoders may skip
// this element (§9.9); `data` is opaque, per §10.10.2 ("interpretation... outside the scope
// of this specification").
struct UserData {
    std::array<std::byte, 16> user_id{};  // §10.10.1 UserID - a SMPTE ST 298:2009 Universal Label, 128 bits
    std::vector<std::byte> data;           // UserDataBytes
};

// §10.5.3 Table 23 - not itself a bitstream symbol; derived from FrameRate (§10.2.4) alone.
// Governs the sub-block loop trip count for ObjectDefinition pan info, ObjectZoneDefinition19
// zone info, and BedRemap ("NumRemapSubBlocks... is the same as NumPanSubBlocks", §9.3).
// nullopt for FrameRate codes 0xA-0xF (Reserved, Table 17).
[[nodiscard]] constexpr std::optional<unsigned> num_pan_sub_blocks(std::uint8_t frame_rate_code) {
    switch (frame_rate_code) {
        case 0x0: case 0x1: case 0x2: case 0x9: return 8;  // 24, 25, 30, 24000/1001 fps
        case 0x3: case 0x4: case 0x5: return 4;             // 48, 50, 60 fps
        case 0x6: case 0x7: case 0x8: return 2;              // 96, 100, 120 fps
        default: return std::nullopt;
    }
}

// §10.2.4 Table 18 - the AudioDataPCM/AudioDataDLC per-frame sample count, keyed by FrameRate
// and by which of IAFrame's own SampleRate the element carries (§10.2.2: 0x0 = 48 kHz, 0x1 =
// 96 kHz). nullopt for FrameRate codes 0xA-0xF.
[[nodiscard]] constexpr std::optional<std::uint32_t> sample_count(std::uint8_t frame_rate_code, bool is_96k) {
    static constexpr std::uint32_t k48[10] = {2000, 1920, 1600, 1000, 960, 800, 500, 480, 400, 2002};
    static constexpr std::uint32_t k96[10] = {4000, 3840, 3200, 2000, 1920, 1600, 1000, 960, 800, 4004};
    if (frame_rate_code > 0x9) return std::nullopt;
    return is_96k ? k96[frame_rate_code] : k48[frame_rate_code];
}

// §9.1/§10.2: the top-level element - everything needed to decode one frame of audio.
// `sample_rate`/`bit_depth` are already resolved from their 2-bit codes (Table 15/16) to Hz
// and bits; `frame_rate_code` is kept raw (Table 17) since one of its ten values (24000/1001)
// is not a whole-number fps and num_pan_sub_blocks()/sample_count() above take the code
// directly rather than a resolved double.
struct IaFrame {
    std::uint8_t version = 1;            // §10.2.1; this reader accepts only 1 (0 and 2 are forbidden)
    std::uint32_t sample_rate = 48000;    // §10.2.2, Table 15, resolved to Hz
    std::uint32_t bit_depth = 16;         // §10.2.3, Table 16, resolved to bits
    std::uint8_t frame_rate_code = 0;     // §10.2.4, Table 17
    std::uint32_t max_rendered = 0;       // §10.2.5, Plex(8)

    std::vector<BedDefinition> beds;
    std::vector<ObjectDefinition> objects;
    std::vector<AudioDataDlc> audio_dlc;
    std::vector<AudioDataPcm> audio_pcm;
    std::optional<AuthoringToolInfo> authoring_tool;
    std::vector<UserData> user_data;
};

}  // namespace ac3iab
