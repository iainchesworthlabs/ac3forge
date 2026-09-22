#include "ac3/admbridge/iab_bridge.hpp"

#include <cassert>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "ac3/admbridge/coordinates.hpp"
#include "ac3/oba/oamd.hpp"
#include "ac3iab/model.hpp"

// See iab_bridge.hpp's own top comment for the overall two-pass design and what is and is not
// mapped. This file is the implementation of that design.

namespace ac3::admbridge {

namespace {

// Identity per §10.3.1's own MetaID mechanism - see iab_bridge.hpp's own top comment. A Bed
// channel's identity is (its parent BedDefinition's MetaID, its own ChannelID); an Object's is its
// own MetaID alone (`sub_channel_id` unused, left 0).
struct IabChannelKey {
    bool is_bed = false;
    std::uint32_t meta_id = 0;
    std::uint32_t sub_channel_id = 0;

    friend bool operator==(const IabChannelKey&, const IabChannelKey&) = default;
};

struct IabChannelKeyHash {
    std::size_t operator()(const IabChannelKey& k) const noexcept {
        std::size_t h = std::hash<bool>{}(k.is_bed);
        h = h * 31 + std::hash<std::uint32_t>{}(k.meta_id);
        h = h * 31 + std::hash<std::uint32_t>{}(k.sub_channel_id);
        return h;
    }
};

// §10.3.5 Table 19's ChannelID/DestinationChannelID codes with a clean ac3::oba::BedLabel
// equivalent - see iab_bridge.hpp's own top comment for the full reasoning on the surround-zone
// collapse (0x6/0xA -> Ls/Rs, 0x7/0x8 -> Lb/Rb, 0x5/0x9 refused) and why several other codes are
// deliberately left unmapped rather than guessed at. 0x80-0x89 are Table 19's own BS.2051-2-named
// alternative codes for a subset of the same physical positions (heights, wide, dual-LFE); 0xE/0xF
// ("Left/Right Height", unqualified - the same pattern 0x0/0x4 "Left"/"Right" alone use for the
// front row, versus the explicitly-qualified "Surround Height"/"Side Surround Height"/"Rear
// Surround Height" variants below 0x11) are the front-height pair, matching 0x80/0x81's own "Top
// Front" naming for the identical position.
[[nodiscard]] std::optional<ac3::oba::BedLabel> bed_label_for_channel_id(std::uint32_t channel_id) {
    switch (channel_id) {
        case 0x0: return ac3::oba::BedLabel::kL;               // Left
        case 0x2: return ac3::oba::BedLabel::kC;               // Center
        case 0x4: return ac3::oba::BedLabel::kR;               // Right
        case 0x6: return ac3::oba::BedLabel::kLs;              // Left Surround
        case 0xA: return ac3::oba::BedLabel::kRs;              // Right Surround
        case 0x7: return ac3::oba::BedLabel::kLb;              // Left Rear Surround
        case 0x8: return ac3::oba::BedLabel::kRb;              // Right Rear Surround
        case 0xD: return ac3::oba::BedLabel::kLfe;             // LFE
        case 0xE: return ac3::oba::BedLabel::kTfl;             // Left Height
        case 0xF: return ac3::oba::BedLabel::kTfr;             // Right Height
        case 0x80: return ac3::oba::BedLabel::kTfl;            // Left Top Front / J
        case 0x81: return ac3::oba::BedLabel::kTfr;            // Right Top Front / J
        case 0x82: return ac3::oba::BedLabel::kTbl;            // Left Top Back / J
        case 0x83: return ac3::oba::BedLabel::kTbr;            // Right Top Back / J
        case 0x84: return ac3::oba::BedLabel::kTsl;            // Top side left / H
        case 0x85: return ac3::oba::BedLabel::kTsr;            // Top side right / H
        case 0x86: return ac3::oba::BedLabel::kLfe;            // LFE1 / H
        case 0x87: return ac3::oba::BedLabel::kLfe2;           // LFE2 / H
        case 0x88: return ac3::oba::BedLabel::kLw;             // Front Left (Wide) / H
        case 0x89: return ac3::oba::BedLabel::kRw;             // Front Right (Wide) / H
        default: return std::nullopt;
    }
}

[[nodiscard]] bool is_lfe_label(ac3::oba::BedLabel label) {
    return label == ac3::oba::BedLabel::kLfe || label == ac3::oba::BedLabel::kLfe2;
}

struct ChannelIdentity {
    IabChannelKey key;
    std::optional<ac3::oba::BedLabel> bed_label;  // set exactly when key.is_bed
};

// Pass 1: unions every unconditionally-Activated top-level Bed channel / Object across the whole
// sequence, in first-seen order - see iab_bridge.hpp's own top comment for why this is a union
// over the WHOLE sequence (MetaID identity) rather than per-frame, and why conditional elements are
// excluded rather than represented. Only top-level ac3iab::IaFrame::beds/objects are walked -
// nested BedDefinition/BedRemap/ObjectDefinition/ObjectZoneDefinition19 children (§9 Table 4) are
// deliberately not recursed into: Annex C.1 items 3a/3b's own nesting allowance exists for
// alternate/derived submixes, not the primary content this bridge selects one of (the same "pick
// the primary set" scoping this function's Activation handling already applies).
[[nodiscard]] std::expected<std::vector<ChannelIdentity>, BridgeError> collect_channel_identities(
    std::span<const ac3iab::IABitstreamFrame> frames) {
    std::vector<ChannelIdentity> order;
    std::unordered_set<IabChannelKey, IabChannelKeyHash> seen;

    for (const auto& entry : frames) {
        for (const auto& bed : entry.frame.beds) {
            if (bed.activation.conditional) {
                continue;
            }
            for (const auto& channel : bed.channels) {
                const IabChannelKey key{
                    .is_bed = true, .meta_id = bed.meta_id, .sub_channel_id = channel.channel_id};
                if (!seen.insert(key).second) {
                    continue;
                }
                const auto label = bed_label_for_channel_id(channel.channel_id);
                if (!label) {
                    return std::unexpected(BridgeError::kUnsupportedIabChannel);
                }
                order.push_back({.key = key, .bed_label = label});
            }
        }
        for (const auto& object : entry.frame.objects) {
            if (object.activation.conditional) {
                continue;
            }
            const IabChannelKey key{.is_bed = false, .meta_id = object.meta_id};
            if (!seen.insert(key).second) {
                continue;
            }
            order.push_back({.key = key, .bed_label = std::nullopt});
        }
    }
    return order;
}

// §10.3.6/Table 8's own AudioDataID convention, shared by Bed channels and Objects: 0 means
// legitimate silence (zero-filled, not an error); a non-zero value must resolve to an
// AudioDataPCM element in THIS frame or the channel has no audio to place -
// BridgeError::kNoIabEssenceForChannel (an AudioDataDLC-only reference is exactly this case, since
// phase 1 does not decode that element - see model.hpp's own AudioDataDlc comment).
[[nodiscard]] std::expected<std::vector<float>, BridgeError> resolve_essence(
    const ac3iab::IaFrame& frame, std::uint32_t audio_data_id, std::uint32_t samples_per_frame) {
    if (audio_data_id == 0) {
        return std::vector<float>(samples_per_frame, 0.0f);
    }
    for (const auto& pcm : frame.audio_pcm) {
        if (pcm.audio_data_id == audio_data_id) {
            return pcm.samples;
        }
    }
    return std::unexpected(BridgeError::kNoIabEssenceForChannel);
}

// TS 103 420 §8.3.2.2's own 16-channel cap, the same constant and reasoning bridge.cpp's own
// kMaxChannels documents for build() - reused by citation, not by symbol, since that one has
// internal linkage in its own translation unit.
constexpr std::size_t kMaxChannels = 15;

}  // namespace

std::expected<IabBridgeResult, BridgeError> build_iab(std::span<const ac3iab::IABitstreamFrame> frames) {
    if (frames.empty()) {
        return std::unexpected(BridgeError::kEmptyIabStream);
    }

    auto identities = collect_channel_identities(frames);
    if (!identities) {
        return std::unexpected(identities.error());
    }
    if (identities->size() > kMaxChannels) {
        return std::unexpected(BridgeError::kTooManyChannels);
    }

    IabBridgeResult out;
    out.sample_rate = frames.front().frame.sample_rate;
    out.pcm.resize(identities->size());
    std::vector<std::vector<ac3::oba::Keyframe>> keyframes(identities->size());

    // Pass 2: walks every frame once, in order, building each channel's timeline and PCM together
    // - see iab_bridge.hpp's own top comment for why a Bed channel contributes at most one
    // keyframe per frame (no per-block position data exists) while an Object contributes one per
    // active sub block, and why an absent/conditionally-Activated occurrence adds no keyframe at
    // all (the previous real keyframe's interpolation simply continues through the gap - the same
    // carry-forward convention §10.5.4's own PanInfoExists already uses within one frame, extended
    // here across frame boundaries too).
    double time_s = 0.0;
    for (const auto& entry : frames) {
        const auto& frame = entry.frame;
        const auto sub_block_count = ac3iab::num_pan_sub_blocks(frame.frame_rate_code);
        const auto samples_per_frame = ac3iab::sample_count(frame.frame_rate_code, frame.sample_rate == 96000);
        // Unreachable through either public parser (parse_iaframe rejects a Reserved FrameRate
        // code before ever returning an IaFrame - see ac3iab.hpp's own IabError::kReservedFrameRate)
        // - kept as a real assert, not silently trusted, since IaFrame's plain-aggregate shape lets
        // a caller construct one directly without going through that check at all.
        assert(sub_block_count.has_value() && samples_per_frame.has_value());
        const double frame_duration_s =
            static_cast<double>(*samples_per_frame) / static_cast<double>(frame.sample_rate);

        for (std::size_t ch = 0; ch < identities->size(); ++ch) {
            const auto& identity = (*identities)[ch];

            if (identity.key.is_bed) {
                const ac3iab::BedDefinition* bed = nullptr;
                for (const auto& candidate : frame.beds) {
                    if (!candidate.activation.conditional && candidate.meta_id == identity.key.meta_id) {
                        bed = &candidate;
                        break;
                    }
                }
                const ac3iab::BedChannel* channel = nullptr;
                if (bed != nullptr) {
                    for (const auto& candidate : bed->channels) {
                        if (candidate.channel_id == identity.key.sub_channel_id) {
                            channel = &candidate;
                            break;
                        }
                    }
                }

                const bool is_lfe = is_lfe_label(*identity.bed_label);
                if (channel != nullptr) {
                    keyframes[ch].push_back({
                        .time_s = time_s,
                        .position = ac3::oba::bed_label_position(*identity.bed_label),
                        .gain = is_lfe ? 0.0 : channel->gain,
                        .lfe_send = is_lfe ? 1.0 : 0.0,
                    });
                    auto essence = resolve_essence(frame, channel->audio_data_id, *samples_per_frame);
                    if (!essence) {
                        return std::unexpected(essence.error());
                    }
                    out.pcm[ch].insert(out.pcm[ch].end(), essence->begin(), essence->end());
                } else {
                    out.pcm[ch].insert(out.pcm[ch].end(), *samples_per_frame, 0.0f);
                }
            } else {
                const ac3iab::ObjectDefinition* object = nullptr;
                for (const auto& candidate : frame.objects) {
                    if (!candidate.activation.conditional && candidate.meta_id == identity.key.meta_id) {
                        object = &candidate;
                        break;
                    }
                }

                if (object != nullptr) {
                    for (std::size_t sb = 0; sb < object->sub_blocks.size(); ++sb) {
                        const auto& block = object->sub_blocks[sb];
                        if (!block.has_pan_info) {
                            continue;
                        }
                        keyframes[ch].push_back({
                            .time_s = time_s + (static_cast<double>(sb) + 1.0) /
                                                    static_cast<double>(*sub_block_count) * frame_duration_s,
                            .position = iab_position_to_room(block.position),
                            .gain = block.gain,
                            .lfe_send = 0.0,
                            .snap = block.snap,
                        });
                    }
                    auto essence = resolve_essence(frame, object->audio_data_id, *samples_per_frame);
                    if (!essence) {
                        return std::unexpected(essence.error());
                    }
                    out.pcm[ch].insert(out.pcm[ch].end(), essence->begin(), essence->end());
                } else {
                    out.pcm[ch].insert(out.pcm[ch].end(), *samples_per_frame, 0.0f);
                }
            }
        }

        time_s += frame_duration_s;
    }

    for (std::size_t ch = 0; ch < identities->size(); ++ch) {
        const auto& identity = (*identities)[ch];
        // Never empty: collect_channel_identities() only ever records an identity from a frame it
        // was actually, unconditionally present in, and that same occurrence always contributes at
        // least one keyframe above (a Bed channel unconditionally; an Object unconditionally too,
        // since sub_block 0's own has_pan_info is always true per §10.5.4).
        auto path = ac3::oba::KeyframePath::create(std::move(keyframes[ch]));
        if (!path) {
            return std::unexpected(BridgeError::kUnsupportedIabChannel);
        }

        const bool is_lfe = identity.key.is_bed && is_lfe_label(*identity.bed_label);
        out.channel_ids.push_back(identity.key.is_bed
                                       ? "bed:" + std::to_string(identity.key.meta_id) + ":" +
                                             std::to_string(identity.key.sub_channel_id)
                                       : "object:" + std::to_string(identity.key.meta_id));
        out.is_bed.push_back(identity.key.is_bed);
        out.is_lfe.push_back(is_lfe);
        out.paths.push_back(ac3::oba::ObjectPath(std::move(*path)));
    }

    return out;
}

}  // namespace ac3::admbridge
