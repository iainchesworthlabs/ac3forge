#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "ac3/admbridge/bridge.hpp"
#include "ac3/admbridge/export.hpp"
#include "ac3/oba/motion.hpp"
#include "ac3iab/ac3iab.hpp"

// Roadmap item IM1 phase 3 of 3 ("IAB (SMPTE ST 2098-2) reader", see ROADMAP.md): maps the parsed
// IAB bed/object graph (ac3iab::ac3iab, phases 1-2) onto ac3::oba::AtmosEncoder's input shape - one
// ac3::oba::ObjectPath plus one mono PCM buffer per bed speaker feed or dynamic object, the same
// destination shape build() (bridge.hpp) already produces for ADM. This is still the one place
// ac3iab::ac3iab and ac3::forge/ac3::oba are allowed to meet - see bridge.hpp's own top comment on
// why that boundary exists - just a second source feeding it. Gated by the same AC3FORGE_BUILD_ADM
// flag as the rest of ac3::admbridge (this module's own CMakeLists.txt has the full reasoning);
// AC3FORGE_BUILD_IAB (default ON) is a separate, always-satisfied prerequisite this module's own
// CMakeLists.txt now enforces with a FATAL_ERROR guard.
//
// What is structurally different from build()'s own ADM mapping, and why:
//
//   - IAB has no whole-file object graph the way ADM's audioProgramme->audioContent->audioObject
//     tree does - it is a flat SEQUENCE of self-contained IAFrames, each carrying its own
//     Bed/Object metadata scoped to that frame alone (§9.2/§9.4). build_iab() therefore takes the
//     WHOLE parsed frame sequence (std::span<const ac3iab::IABitstreamFrame>, exactly what
//     ac3iab::parse_iabitstream()/parse_mxf_iab() already return) rather than one already-resolved
//     document, and does its own two-pass walk: an identity pass unions every unconditionally-
//     Activated top-level Bed channel / Object across every frame, keyed by §10.3.1's own MetaID
//     ("the ID that allows the system to track metadata information between audio
//     frames"/"Elements with the same ElementID and MetaID in contiguous IAFrames typically
//     represent continuous audio" - for a Bed channel, combined with its own ChannelID per
//     §10.3.5's note on ST 2098-1's Channel Identifier function) - fixing AtmosEncoder's channel
//     count and order once, the same role collect_leaf_objects() plays for build()'s own ADM walk.
//     A timeline pass then builds one ac3::oba::ObjectPath per identity spanning the WHOLE
//     sequence: a frame where that identity is absent, or only conditionally Activated (an
//     alternate-target-environment mix per §10.3.2/§10.5.1's own Activation/UseCase fields - the
//     same "pick the one primary set" choice build() already makes among several audioProgrammes),
//     contributes no new keyframe (silence-filled PCM, the timeline simply holds/interpolates
//     through the gap) rather than shrinking the channel count.
//   - Bed channels have no per-block position data at all (unlike ADM's audioBlockFormat
//     sequence) - a Bed's own ChannelID (Table 19) is a closed, physical-position vocabulary
//     resolved once via ac3::oba::bed_label_position(), the same "pinned, unmoving placement" bed
//     channels already get from ADM's speakerLabel; only BedChannel::gain (§10.3.8) can legitimately
//     vary frame to frame, so a bed channel's timeline is one keyframe per frame it is present in.
//     An LFE bed channel (ChannelID 0xD, or 0x86/0x87's BS.2051-2 LFE1/LFE2 aliases) is routed at
//     gain 0 / lfe_send 1, the exact convention build_channel_path's own doc comment states for
//     ADM ("Objects never reach the LFE by panning").
//   - Table 19's cinema channel vocabulary is richer than ac3::oba::BedLabel's own consumer-layout
//     one in exactly one place: it names THREE distinct surround zones per side (Side Surround,
//     Surround, Rear Surround) where BedLabel has only two slots (kLs/kRs, kLb/kRb). "Surround"
//     (0x6/0xA) maps to kLs/kRs (the canonical 5.1 pair) and "Rear Surround" (0x7/0x8) to kLb/kRb
//     (7.1's additional back pair, the closest conceptual match); "Side Surround" (0x5/0x9) has no
//     equivalent and is refused - BridgeError::kUnsupportedIabChannel, not silently collapsed onto
//     an existing slot, the same "refuse clearly" precedent bridge.hpp's own top comment states for
//     ADM's unsupported pack types. Several other Table 19 codes (Left/Right Center, Center Height,
//     the *Height variants of Side/Rear Surround, Left/Right Top Surround, Top Surround, and the
//     whole 0x18-0x7F D-Cinema-reserved range) are refused the same way, deliberately, rather than
//     guessed at without the external documents Table 19 itself defers to (SMPTE ST 428-12/
//     ST 2098-5) for their exact geometry - see iab_bridge.cpp's own mapping table for the full,
//     cited list of what IS mapped.
//   - ObjectSpread (§10.5.15-17) and the 9-zone ObjectZoneControl (§10.5.11-14) are not mapped, for
//     the identical reason spatial-and-atmos.md's own "extent and rendering constraints" section
//     already gives ADM's own width/height/depth and zoneExclusion: spreading an object in the
//     downmix would have the receiving renderer spread it a second time, and TS 103 420's own
//     6-preset ZoneConstraint has no clean image for either IAB shape (a 9-independent-gain vector,
//     or a 3-way spread). Applied consistently rather than inventing a different policy for this
//     ingest path than the one ADM's own bridge already committed to.
//   - PCM is concatenated across many independently-parsed frames, so IabBridgeResult::pcm is
//     OWNED (std::vector<std::vector<float>>), not borrowed the way BridgeResult::pcm is from a
//     single caller-owned AdmDocument - there is no equivalent single upstream object here to
//     borrow spans from once this function returns. This is the one reason IabBridgeResult is a
//     new struct rather than a reuse of BridgeResult, even though every other field lines up.
namespace ac3::admbridge {

// The result of bridging a whole parsed IAB frame sequence - everything needed to construct and
// drive an ac3::oba::AtmosEncoder, one entry per channel, all vectors indexed identically. See this
// header's own top comment for exactly how each field differs from BridgeResult's own ADM shape.
struct IabBridgeResult {
    std::vector<std::string> channel_ids;      // "bed:<MetaID>:<ChannelID>" / "object:<MetaID>",
                                                // for diagnostics - IAB has no free-text channel
                                                // name the way ADM's audioChannelFormat::id is
    std::vector<bool> is_bed;
    std::vector<bool> is_lfe;
    std::vector<ac3::oba::ObjectPath> paths;   // pass directly to ac3::oba::evaluate_placements
    std::vector<std::vector<float>> pcm;       // owned - see this header's own top comment
    std::uint32_t sample_rate = 0;             // the first frame's own IaFrame::sample_rate,
                                                // unconverted - same convention build()'s own
                                                // BridgeResult::sample_rate documents

    [[nodiscard]] std::size_t channel_count() const { return paths.size(); }
};

// Bridges a whole parsed IAB frame sequence - ac3iab::parse_iabitstream() or
// ac3iab::parse_mxf_iab()'s own return value, unmodified - onto AtmosEncoder's input shape. Channel
// count is capped at the same 15 build() itself enforces, for the identical reason (see bridge.cpp's
// own kMaxChannels comment).
[[nodiscard]] AC3ADMBRIDGE_EXPORT std::expected<IabBridgeResult, BridgeError> build_iab(
    std::span<const ac3iab::IABitstreamFrame> frames);

}  // namespace ac3::admbridge
