#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac3/admbridge/export.hpp"
#include "ac3/oba/motion.hpp"
#include "ac3/oba/oamd.hpp"
#include "ac3adm/model.hpp"

// Roadmap item B1 phase 2 of 3 ("ADM BWF reader feeding the JOC encoder", see ROADMAP.md): maps
// the object graph ac3adm::ac3adm (phase 1) parses from a BW64/ADM master onto
// ac3::oba::AtmosEncoder's input shape - one ac3::oba::ObjectPath plus one mono PCM span per
// channel, ready to drive encode_frame() in a loop. Phase 3 (a CLI/GUI-facing end-to-end command)
// is a separate, later task; this module is the mapping/bridge library only.
//
// ac3::admbridge sits between two modules that otherwise know nothing about each other:
// ac3adm::ac3adm (src/ac3adm, codec-blind by design - see its own header comments) and
// ac3::forge/ac3::oba (src/forge, always built, no dependency on the opt-in, Boost-requiring
// ac3adm). This module is the one place both are allowed to meet, and - like ac3adm::ac3adm
// itself - it is gated behind AC3FORGE_BUILD_ADM: it is meaningless without ac3adm, and
// ac3::forge is unconditionally available regardless of whether this module is built at all. See
// src/admbridge/CMakeLists.txt's own header comment for the full reasoning, including why this
// is a new standalone module rather than folded into either side.
//
// ROADMAP.md's B2 entry (a future DAMF `.atmos`/`.atmos.metadata`/`.atmos.audio` reader) names
// this module as the "mapping layer" it plans to share - reason enough to keep the bed/object
// classification, coordinate conversion (coordinates.hpp) and keyframe-timeline construction
// below independent of ac3adm's own BW64/ADM-XML-specific parsing, even though ac3adm::AdmDocument
// is still this module's only current input shape.
//
// What gets mapped, and what does not:
//
//   - Classification (DirectSpeakers "bed" vs. Objects "dynamic object") is via the
//     TypeDefinition of the audioObject's own resolved audioPackFormat(s) - never via any
//     property of AudioObject itself, which carries no type of its own (see ac3adm/model.hpp's
//     own AudioPackFormat/AudioObject comments). Matrix, HOA, Binaural, User Custom and Unknown
//     pack types are out of this phase's scope and rejected with BridgeError::kUnsupportedType
//     rather than silently mishandled - none of them map onto AtmosEncoder's plain
//     position+gain+lfe_send object model without a design of their own this phase does not
//     attempt (HOA in particular has no "position" at all; Matrix's audioMatrixFormat encodes an
//     entirely different, coefficient-based routing this bridge does not interpret).
//   - AtmosEncoder has no distinct bed-feeding method: its constructor just takes an object
//     count, and encode_frame() takes one flat span of objects plus one flat span of placements
//     (ac3/oba/atmos.hpp) - nothing in that signature distinguishes "a bed channel" from "a
//     dynamic object". A bed channel is therefore represented the only way the API allows: as an
//     object with an unmoving, pinned placement, the same convention every existing caller
//     (apps/cli/main.cpp's run_atmos_encode, apps/gui/encoder_controller.cpp's encodeObjects)
//     already uses. This module follows suit - a bed channel becomes one more entry in the same
//     flat channel list, with a static (or, rarely, dynamic - see build_channel_path()'s own
//     comment) ObjectPath pinned at its speakerLabel's room position, at unity gain; a bed
//     channel whose speakerLabel identifies it as the LFE (Table 12: "LFE", "LFE1", "LFE2") is
//     instead routed at gain 0 / lfe_send 1, since (per atmos.hpp's own ObjectPlacement comment)
//     "Objects never reach the LFE by panning".
//   - Position/gain automation (ITU-R BS.2076-2 Clause 10.3's jumpPosition/interpolationLength
//     hold-vs-glide state machine) is implemented in build_channel_path() below - see that
//     function's own comment for the full walkthrough, verified against the standard's own
//     Figs 7-10, not assumed from a paraphrase.
//   - width/height/depth/diffuse/objectDivergence (parsed by ac3adm, per Clause 10.3 also
//     nominally interpolatable) have no equivalent in ac3::oba::Keyframe/ObjectPlacement at all -
//     AtmosEncoder's object model is a pure point source. This bridge silently drops them; every
//     channel it produces is a point source regardless of what the source ADM data's spread
//     parameters said. Documented here and in docs/library/adm-bridge.md rather than left for a
//     caller to discover by reading source.
//   - channelLock/zoneExclusion (Clause 10.2/10.4) are parsed by ac3adm but have no AtmosEncoder
//     equivalent either (no notion of "the nearest bed speaker" or "a masked zone" downstream of
//     a fixed 5.1 VBAP ring) and are likewise dropped.
namespace ac3::admbridge {

enum class BridgeError : std::uint8_t {
    kNoProgramme,           // the document's ADM model has no audioProgramme at all
    kProgrammeNotFound,     // an explicit programme_id was given but matches no audioProgramme
    kUnresolvedReference,   // a content/object/pack/channel/track-UID/chna ID reference did not
                            // resolve to an element that ac3adm actually parsed
    kObjectReferenceCycle,  // nested audioObject references (object_refs) formed a loop -
                            // BS.2076-2 §5.6.7: "An audioObject element should not reference
                            // itself, nor can a loop of references be used"
    kUnsupportedType,       // a resolved audioPackFormat's TypeDefinition is not DirectSpeakers or
                            // Objects, an audioObject's own resolved packs disagree with each
                            // other, or a pack itself nests further audioPackFormats (Matrix/HOA-
                            // style pack nesting, out of this phase's scope) - see this header's
                            // own top comment
    kChannelTrackMismatch,  // an audioObject's audioTrackUIDRef count did not match the channel
                            // count its resolved audioPackFormat(s) describe
    kNoAudioForTrack,       // a resolved <chna> track_index has no corresponding PCM channel in
                            // the document (out of range, or index 0 - BS.2088-1 §8.2's "unused"
                            // marker)
    kEmptyBlockSequence,    // an audioChannelFormat had zero audioBlockFormats - illegal per
                            // BS.2076-2 §5.3.2's "1..*", but ac3adm's own parser does not itself
                            // enforce this, so it is checked here rather than assumed
    kTooManyChannels,       // more bed + dynamic-object channels than AtmosEncoder supports - see
                            // build()'s own comment for the exact cap and its citation
    kEmptyInput,            // write() only: WriteInput::channels was empty, or a dynamic-object
                            // channel's `updates` was empty (every channel needs at least one
                            // DynamicObject state to place it, even a static, never-moving one)
    kEmptyIabStream,        // build_iab() only: the frame span passed to it was empty
    kUnsupportedIabChannel, // build_iab() only: a BedDefinition used a Table 19 ChannelID with no
                            // ac3::oba::BedLabel equivalent - see iab_bridge.cpp's own comment on
                            // exactly which codes map and which are refused
    kNoIabEssenceForChannel, // build_iab() only: a channel's non-zero AudioDataID (§10.3.6/Table 8's
                            // own field) never resolved to an AudioDataPCM element in any frame it
                            // was active in - missing, or only ever present as an (undecoded)
                            // AudioDataDLC asset. AudioDataID == 0 is legitimate silence (§10.3.6)
                            // and is not this error.
};

[[nodiscard]] AC3ADMBRIDGE_EXPORT std::string_view describe(BridgeError error);

// Builds one channel's ac3::oba::ObjectPath from its audioBlockFormat sequence.
//
// BS.2076-2 §5.4.1: a channel with exactly one audioBlockFormat is static - one keyframe, held
// everywhere (ac3::oba::KeyframePath's own "a single keyframe holds its placement everywhere"
// behaviour is exactly this).
//
// For more than one block, §10.3's own state machine (verified directly against the standard's
// text and its Figs 7-10, not assumed from a paraphrase - an earlier draft of this bridge had it
// backwards, see docs/library/adm-bridge.md's own note on this):
//
//   - jumpPosition = 0 (or absent): "the renderer will interpolate a moving object between
//     positions over the full duration of the block" - a continuous ramp spanning the block's
//     ENTIRE [rtime, rtime+duration), reaching the block's own value exactly at its end, and
//     continuous with whatever value the timeline already held at the block's start (in practice
//     the previous block's own final value, since audioBlockFormat sequences are contiguous).
//     Mapped to a single keyframe at the block's END time - the natural KeyframePath linear
//     interpolation from the previous block's own already-placed keyframe reproduces the ramp.
//   - jumpPosition = 1: "it will jump to the new position instantly" and then holds - "The value
//     of x is set at the beginning of the block and maintains that value throughout its
//     duration." If interpolationLength is also given, the jump becomes a ramp of that length at
//     the block's start instead of an instant one ("the interpolation period is set to the
//     interpolationLength value"), still followed by a hold for the remainder of the block.
//     Mapped to a keyframe at the ramp's end (start + interpolationLength, or start again if
//     interpolationLength is absent/zero - see kInstantJumpEpsilon's own comment in bridge.cpp
//     for why an exact zero cannot be represented literally), plus a second keyframe at the
//     block's own end holding the same value, unless the ramp already reaches exactly that time.
//   - The FIRST block in a sequence always holds across its own entire span regardless of ITS
//     OWN jumpPosition/interpolationLength: "To ensure undefined behaviour of the first block is
//     avoided, then the position specified in the first block covers the entire length of the
//     block (regardless of the jumpPosition and interpolationLength properties)."
//
// `object_start_s` is the channel's parent audioObject's own start_s (see build()'s own comment
// on why this, plus the block's own rtime_s, is the whole absolute-time computation - no third,
// programme-level term). `force_lfe` overrides every resulting keyframe to gain 0.0/lfe_send 1.0,
// discarding the block's own real position/gain data entirely (see this header's own top comment
// on why) - pass true only for a bed channel whose speakerLabel identifies it as the LFE.
//
// Exposed (not file-local) specifically so this state machine can be tested directly against
// hand-built ac3adm::AudioChannelFormat fixtures, independent of a full BW64 file/<chna>/pack
// resolution round trip.
[[nodiscard]] AC3ADMBRIDGE_EXPORT std::expected<ac3::oba::ObjectPath, BridgeError>
build_channel_path(const ac3adm::AudioChannelFormat& channel, double object_start_s,
                    bool force_lfe);

// The result of bridging one ac3adm::AdmDocument: everything needed to construct and drive an
// ac3::oba::AtmosEncoder, one entry per channel (bed speaker feed or dynamic object), all vectors
// indexed identically - entry i of every vector describes the same channel. Channels appear in
// programme -> audioContent -> audioObject (including nested audioObjects, depth-first)
// traversal order; that order has no significance to AtmosEncoder itself (see this header's own
// top comment - every channel becomes one of its `objects_` slots identically, bed-pinned or
// not), it exists only so BridgeResult is deterministic and its diagnostics read sensibly.
struct BridgeResult {
    std::vector<std::string> channel_ids;     // ac3adm::AudioChannelFormat::id, for diagnostics
    std::vector<bool> is_bed;                 // true: a DirectSpeakers bed channel
    std::vector<bool> is_lfe;                 // true only for a bed channel routed via lfe_send
                                               // (see this header's own top comment)
    std::vector<ac3::oba::ObjectPath> paths;  // pass directly to ac3::oba::evaluate_placements
    std::vector<std::span<const float>> pcm;  // one mono span per channel, borrowed from the
                                               // AdmDocument passed to build() - the caller must
                                               // keep that document (and its ac3adm::PcmAudio)
                                               // alive for as long as these spans are used
    std::uint32_t sample_rate = 0;            // ac3adm::PcmAudio::sample_rate, unconverted - the
                                               // caller maps this to ac3::SampleRate (and rejects
                                               // an unsupported rate) the same way every existing
                                               // WAV-reading entry point already does; not
                                               // duplicated here

    [[nodiscard]] std::size_t channel_count() const { return paths.size(); }
};

// Resolves `programme_id` (or, if empty, the lowest-ID audioProgramme - BS.2076-2 §5.8: "When
// more than one audioProgramme is included in a file, and there is no other information to
// decide which one to choose for playback, then the default audioProgramme is the one with the
// lowest ID value") down through its audioContent(s)/audioObject(s) (recursing through nested
// audioObjects - §5.6: "AudioObjects can be nested and so they can refer to other audioObjects" -
// with a cycle guard per §5.6.7's own prohibition), classifies each leaf audioObject as a bed or
// a dynamic object via its resolved audioPackFormat's TypeDefinition, builds one
// ac3::oba::ObjectPath per channel from its audioBlockFormat sequence, and resolves its audio via
// <chna>.
//
// Absolute program-timeline time for a channel's automation is `object.start_s + block.rtime_s` -
// TWO levels, not three. BS.2076-2 Table 24 defines audioObject's own `start` as "relative to the
// start of the audioProgramme", and §5.6.7 confirms this holds even through nesting ("the start
// time of the audioObject is still relative to the start of the programme, not... relative to the
// audioObject that refers to it") - so a nested audioObject's own start_s is never added to its
// parent's. audioProgramme's own optional `start`/`end` attributes (Table 37) are a SEPARATE,
// video-alignment concept ("used for alignment with video times"), not a third offset the object
// timeline is measured against - confirmed both by that clause's own wording and by
// ac3adm::AudioProgramme (ac3adm/model.hpp) carrying no start_s field of its own at all to add.
//
// Channel count is capped at 15: AtmosEncoder's own constructor `objects` parameter is dynamic
// objects only, with the bed's own LFE bookkeeping as an implicit, always-present 16th (TS 103
// 420 §8.3.2.2 caps the total at 16) - the exact cap apps/cli/main.cpp's run_atmos_encode/
// run_atmos_path already enforce for the same reason, reused here rather than re-derived.
[[nodiscard]] AC3ADMBRIDGE_EXPORT std::expected<BridgeResult, BridgeError> build(
    const ac3adm::AdmDocument& document, std::string_view programme_id = {});

// --- Write direction: roadmap item IM2 ("JOC -> ADM BWF writer") ---------------------------
//
// The mirror image of build() above: instead of mapping an already-parsed ac3adm::AdmDocument
// onto AtmosEncoder's input shape, this maps a DECODED E-AC-3/Atmos programme's own bed/object
// PCM and OAMD automation onto an ac3adm::AdmDocument, ready for ac3adm::write_bw64(). Still the
// one place ac3adm and ac3::forge/ac3::oba are allowed to meet - see this header's own top
// comment - just travelling the other way.
//
// Scope is deliberately narrower than build()'s own read-side generality, matching what this
// project's OWN decoder (the only source this writer has) ever actually produces: one dynamic-
// object-only-or-single-bed-instance programme (Eac3Decoder never emits ISF objects, several bed
// instances or non-standard Table 13 assignments - oamd.hpp's own Program comment), no nested
// audioObjects, cartesian positions only (this writer never emits polar). A programme this
// restrictive to READ would refuse real third-party content; a programme this restrictive to
// WRITE only ever has to describe what THIS decoder decoded, which is a much smaller shape.

// One OAMD update to a dynamic object's DynamicObject state, timestamped in absolute samples from
// the start of the whole decode (not the access unit it arrived in) - the flattened form of
// ac3::oba::DecodedProgram::UpdateBlock (oamd.hpp) a caller assembles by walking every decoded
// access unit's own object_metadata->blocks in file order and adding each block's own
// sample_offset to a running total of samples already emitted.
struct AC3ADMBRIDGE_EXPORT WriteObjectUpdate {
    std::uint64_t sample_offset = 0;
    // ac3::oba::UpdateBlock::ramp_duration verbatim - samples, or -1 for the one
    // ramp_duration_bits codeword TS 103 420's own table does not name (oamd.hpp's own comment);
    // build_block_formats() (bridge.cpp) treats a negative value as an instant jump (ramp 0).
    int ramp_duration_samples = 0;
    ac3::oba::DynamicObject state;
};

// One channel to write into the master. A bed channel (`bed_label` set) is written as a static
// DirectSpeakers channel pinned at its own room position (ac3::oba::bed_label_position) - `updates`
// is ignored for these, the same "a bed channel has no direction to pin, `force_lfe` discards it
// entirely" convention build_channel_path's own doc comment states for the read direction. A
// dynamic object (`bed_label` empty) is written as an Objects channel whose audioBlockFormat
// sequence comes from `updates`, which must be non-empty and in strictly increasing
// `sample_offset` order (a caller emitting them in decode order already satisfies this; see
// build_block_formats()'s own comment on why a non-increasing entry is folded into its
// predecessor rather than rejected).
struct AC3ADMBRIDGE_EXPORT WriteChannel {
    std::string name;
    std::span<const float> pcm;                       // this channel's whole-file mono audio
    std::optional<ac3::oba::BedLabel> bed_label{};     // set: bed/LFE channel; empty: dynamic object
    std::span<const WriteObjectUpdate> updates{};      // dynamic objects only
};

struct AC3ADMBRIDGE_EXPORT WriteInput {
    std::uint32_t sample_rate = 0;
    std::vector<WriteChannel> channels;
};

// Builds one ac3adm::AdmDocument programme -> content -> {one audioObject per channel}, cartesian
// positions throughout, ready for ac3adm::write_bw64(). `input.channels[i].pcm` is copied into the
// returned document's own `audio.channels[i]` (unlike build()'s own BridgeResult::pcm, which
// borrows - there is no caller-owned buffer here for the result to borrow from once this function
// returns, since the document is the thing about to be written to disk).
[[nodiscard]] AC3ADMBRIDGE_EXPORT std::expected<ac3adm::AdmDocument, BridgeError> write(
    const WriteInput& input);

}  // namespace ac3::admbridge
