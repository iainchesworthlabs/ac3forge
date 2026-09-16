#include "ac4dec/decoder.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "ac4/ac4.hpp"
#include "bit_reader.hpp"
#include "syntax/context.hpp"
#include "syntax/metadata.hpp"
#include "syntax/presentation.hpp"
#include "syntax/substream.hpp"

namespace ac4 {

std::string_view describe(DecodeError error) {
    switch (error) {
        case DecodeError::kTruncated:
            return "a syntax element runs past the end of its substream";
        case DecodeError::kInvalidToc:
            return "the table of contents could not be read";
        case DecodeError::kInvalidStream:
            return "the substream holds a value its syntax cannot follow";
        case DecodeError::kUnsupported:
            return "the substream uses syntax this decoder does not read yet";
        case DecodeError::kMissingIFrame:
            return "the frame needs configuration that no I-frame has sent";
    }
    return "unknown error";
}

namespace {

using detail::AudioSubstream;
using detail::AudioSubstreamState;
using detail::BitReader;
using detail::ParseResult;
using detail::PresentationContext;
using detail::PresentationSubstream;
using detail::PresentationSubstreamState;
using detail::SubstreamContext;

enum class Role : std::uint8_t { kMain, kMusicAndEffects, kDialogue, kDialogueEnhancement, kAssociated };

// Part 2 Table 54, for presentation_config 5 and for a single group's
// content_classifier.
[[nodiscard]] Role role_from_classifier(int content_classifier) noexcept {
    switch (content_classifier) {
        case 0b010:
        case 0b011:
        case 0b101:
            return Role::kAssociated;
        case 0b100:
            return Role::kDialogue;
        default:
            return Role::kMain;
    }
}

// Part 2 Table 53: the substream type of the group at `position` in a
// presentation_version 1 presentation.
[[nodiscard]] Role role_v1(const PresentationInfoV1& p, std::size_t position, const SubstreamGroupInfo& group) {
    if (!p.presentation_config) {
        return Role::kMain;  // 6.3.2.2.1: a single substream group is Main
    }
    static constexpr std::array<std::array<Role, 3>, 5> kRoles = {{
        {Role::kMusicAndEffects, Role::kDialogue, Role::kMain},
        {Role::kMain, Role::kDialogueEnhancement, Role::kMain},
        {Role::kMain, Role::kAssociated, Role::kMain},
        {Role::kMusicAndEffects, Role::kDialogue, Role::kAssociated},
        {Role::kMain, Role::kDialogueEnhancement, Role::kAssociated},
    }};
    const int config = *p.presentation_config;
    if (config >= 0 && config <= 4 && position < 3) {
        return kRoles[static_cast<std::size_t>(config)][position];
    }
    if (config == 5 && group.content_type) {
        return role_from_classifier(group.content_type->content_classifier);
    }
    return Role::kMain;
}

// DEE's immersive stereo: presentation_version 2, which Part 2 V1.3.1 names
// without defining, over a channel-coded substream carrying the channel_mode
// code Table 56 gives to 7.0 (3/4/0). Every such stream parses to the exact
// end of every substream only when that substream is read as stereo wherever
// its channel mode is used: its channel element, its metadata() and the
// presentation's pres_ch_mode. The Python transcription found the same. It is
// an observation of one encoder, not the text; the errata register records it.
void apply_observed_stereo_rule(Toc& toc) {
    // Each group is patched once however many presentations name it, and
    // however many times one names it: a presentation may repeat a group
    // reference, and walking a group per reference costs a walk per reference.
    std::vector<bool> named(toc.substream_groups.size(), false);
    for (const PresentationInfoV1& p : toc.presentations_v1) {
        if (p.presentation_version != 2) {
            continue;
        }
        for (const int group_index : p.group_refs) {
            if (group_index >= 0 && static_cast<std::size_t>(group_index) < toc.substream_groups.size()) {
                named[static_cast<std::size_t>(group_index)] = true;
            }
        }
    }
    for (std::size_t group_index = 0; group_index < named.size(); ++group_index) {
        if (!named[group_index]) {
            continue;
        }
        for (GroupSubstream& sub : toc.substream_groups[group_index].substreams) {
            if (sub.kind == GroupSubstream::Kind::kChan && sub.chan && sub.chan->channel_mode == 0b1111000) {
                sub.chan->ch_mode = detail::ch_mode::kStereo;
            }
        }
    }
}

// What the decoder decided a substream is, and what its syntax needs.
struct Assignment {
    SubstreamReport::Kind kind = SubstreamReport::Kind::kOther;
    SubstreamContext audio{};
    std::optional<PresentationContext> presentation;
    std::optional<detail::SyntaxError> refusal;  // set to refuse without reading
    // Which slot of carried state this substream reads and writes. It is the
    // substream's own index except in a frame-rate-multiplied series, where
    // every instance shares the first one's - see assign_instances().
    int state_key = 0;
};

void refuse(std::map<int, Assignment>& out, int index, DecodeError error, std::string_view reason) {
    if (out.contains(index)) {
        return;
    }
    Assignment a;
    a.kind = SubstreamReport::Kind::kOther;
    a.refusal = detail::SyntaxError{error, reason};
    out.emplace(index, std::move(a));
}

// The context of the channel-coded substream instance `index` of `chan`.
// Refuses without reading what the syntax cannot follow: a reserved
// frame_rate_index or channel_mode.
void assign_audio(const Toc& toc, const ChannelSubstreamInfo& chan, int index, int state_key, int frame_rate_factor,
                  bool b_iframe, int presentation_version, int sus_ver, bool b_associated, bool b_dialog,
                  bool b_alternative, std::map<int, Assignment>& out) {
    if (out.contains(index)) {
        return;
    }
    const int base = detail::frame_len_base(toc.frame_rate_index, toc.sample_rate_hz);
    if (base == 0) {
        refuse(out, index, DecodeError::kInvalidStream, "a reserved frame_rate_index");
        return;
    }
    // Part 1 Tables 83 and 87: an instance of a frame-rate-multiplied series
    // covers its share of the base frame, and every (frame_rate_index, factor)
    // pair Table 87 permits lands on another index's listed length - 2048 at 25
    // fps doubled is 1024, the 50 fps entry. The length sets transform lengths
    // and the widths derived from them, so an instance read at the base length
    // is misread, not merely mis-scaled.
    if (frame_rate_factor <= 0 || base % frame_rate_factor != 0) {
        refuse(out, index, DecodeError::kInvalidStream,
               "a frame rate factor the frame length does not divide by");
        return;
    }
    if (!chan.ch_mode) {
        refuse(out, index, DecodeError::kInvalidStream, "a reserved channel_mode");
        return;
    }
    Assignment a;
    a.kind = SubstreamReport::Kind::kAudio;
    a.state_key = state_key;
    SubstreamContext& ctx = a.audio;
    ctx.bitstream_version = toc.bitstream_version;
    ctx.presentation_version = presentation_version;
    ctx.fs_index = toc.sample_rate_hz == 44100 ? 0 : 1;
    ctx.frame_rate_index = toc.frame_rate_index;
    ctx.frame_len_base = base / frame_rate_factor;
    ctx.b_iframe = b_iframe;
    ctx.sus_ver = sus_ver;
    ctx.ch_mode = *chan.ch_mode;
    ctx.sf_multiplier = chan.sf_multiplier.has_value();
    ctx.add_ch_base = chan.add_ch_base.value_or(false);
    if (chan.original_content) {
        ctx.b_4_back_channels_present = chan.original_content->b_4_back_channels_present;
        ctx.b_centre_present = chan.original_content->b_centre_present;
        ctx.top_channels_present = chan.original_content->top_channels_present;
    }
    ctx.b_associated = b_associated;
    ctx.b_dialog = b_dialog;
    ctx.b_alternative = b_alternative;
    out.emplace(index, std::move(a));
}

// Part 1 4.3.3.7.9: with a frame_rate_factor above 1, substream_index names
// the first of that many consecutive substreams, one per instance, each with
// its own b_iframe or b_audio_ndot.
//
// The series is one audio signal cut into consecutive codec frames - 4.3.3.5.3
// says each of those substreams is decoded consecutively, and 4.3.3.2.7 makes
// b_iframe_global true when the FIRST b_iframe of a series is - so what an
// I-frame of the series configures serves the instances after it, and each
// instance predicts from the one before. They therefore share one slot of
// carried state, the first instance's. A slot per instance leaves instance 1
// with no configuration any I-frame ever sent, so every frame of a legal
// stream whose I-frames set only the first flag fails as missing its I-frame.
void assign_instances(const Toc& toc, const ChannelSubstreamInfo& chan, int presentation_version, int sus_ver,
                      bool b_associated, bool b_dialog, bool b_alternative, std::map<int, Assignment>& out) {
    if (!chan.substream_index) {
        return;
    }
    const std::size_t instances = chan.b_iframe.empty() ? 1 : chan.b_iframe.size();
    // substream_index carries a variable_bits() escape, so it reaches INT_MAX;
    // the sum is taken in 64 bits because instance INT_MAX + 1 would overflow.
    // An index past INT_MAX is past every substream the index table can hold,
    // so it and the instances after it name nothing.
    const std::int64_t first = *chan.substream_index;
    for (std::size_t i = 0; i < instances; ++i) {
        const std::int64_t index = first + static_cast<std::int64_t>(i);
        if (index > std::numeric_limits<int>::max()) {
            break;
        }
        const bool b_iframe = !chan.b_iframe.empty() && chan.b_iframe[i];
        assign_audio(toc, chan, static_cast<int>(index), static_cast<int>(first),
                     static_cast<int>(instances), b_iframe, presentation_version, sus_ver, b_associated, b_dialog,
                     b_alternative, out);
    }
}

// Which syntax each substream is read with (Part 1 Table 15, Part 2 Table 50).
// A substream named by several elements is read once, as the first names it:
// every presentation's EMDF payload and presentation substreams before any
// substream group's, the order the Python transcription takes too.
void assign_v1(const Toc& toc, std::map<int, Assignment>& out) {
    for (const PresentationInfoV1& p : toc.presentations_v1) {
        for (const int index : p.emdf_payloads_substream_indices) {
            if (!out.contains(index)) {
                Assignment a;
                a.kind = SubstreamReport::Kind::kEmdfPayloads;
                out.emplace(index, std::move(a));
            }
        }
        if (p.presentation_substream_index && !out.contains(*p.presentation_substream_index) &&
            detail::frame_len_base(toc.frame_rate_index, toc.sample_rate_hz) == 0) {
            // No frame length, so nothing in the substream that derives from
            // one can be read - the same refusal assign_audio() makes, rather
            // than reading on until channel-dependent DRC gains need it.
            refuse(out, *p.presentation_substream_index, DecodeError::kInvalidStream,
                   "a reserved frame_rate_index");
        }
        if (p.presentation_substream_index && !out.contains(*p.presentation_substream_index)) {
            Assignment a;
            a.kind = SubstreamReport::Kind::kPresentation;
            a.presentation = detail::presentation_context_v1(toc, p);
            out.emplace(*p.presentation_substream_index, std::move(a));
        }
    }
    std::vector<bool> seen(toc.substream_groups.size(), false);
    for (const PresentationInfoV1& p : toc.presentations_v1) {
        for (std::size_t position = 0; position < p.group_refs.size(); ++position) {
            const int group_index = p.group_refs[position];
            if (group_index < 0 || static_cast<std::size_t>(group_index) >= toc.substream_groups.size() ||
                seen[static_cast<std::size_t>(group_index)]) {
                continue;
            }
            seen[static_cast<std::size_t>(group_index)] = true;
            const SubstreamGroupInfo& group = toc.substream_groups[static_cast<std::size_t>(group_index)];
            if (!group.b_substreams_present) {
                continue;
            }
            const Role role = role_v1(p, position, group);
            if (group.oamd && group.oamd->substream_index) {
                refuse(out, *group.oamd->substream_index, DecodeError::kUnsupported,
                       "object audio metadata substreams are not decoded yet");
            }
            const int classifier = group.content_type ? group.content_type->content_classifier : 0;
            const bool b_associated = role == Role::kAssociated || role_from_classifier(classifier) == Role::kAssociated;
            const bool b_dialog = role == Role::kDialogue || classifier == 0b100;
            for (const GroupSubstream& sub : group.substreams) {
                // The substream's own claim on its own index goes first,
                // matching Python's substream_roles() (audio() before the
                // hsf_ext put()) - a fuzzed stream can send an
                // hsf_ext_substream_index equal to the substream's own
                // index, and out.contains()'s first-claim-wins means the
                // two transcriptions would otherwise disagree on which
                // claim that self-reference resolves to.
                if (sub.kind == GroupSubstream::Kind::kAjoc && sub.ajoc && sub.ajoc->substream_index) {
                    refuse(out, *sub.ajoc->substream_index, DecodeError::kUnsupported,
                           "A-JOC substreams are not decoded yet");
                } else if (sub.kind == GroupSubstream::Kind::kObj && sub.obj && sub.obj->substream_index) {
                    refuse(out, *sub.obj->substream_index, DecodeError::kUnsupported,
                           "object substreams are not decoded yet");
                } else if (sub.kind == GroupSubstream::Kind::kChan && sub.chan) {
                    // sus_ver is 1 for bitstream_version 2 (Part 2 6.2.1.6).
                    assign_instances(toc, *sub.chan, p.presentation_version, 1, b_associated, b_dialog,
                                     p.b_alternative, out);
                }
                if (sub.hsf_ext_substream_index) {
                    refuse(out, *sub.hsf_ext_substream_index, DecodeError::kUnsupported,
                           "HSF extension substreams are not decoded yet");
                }
            }
        }
    }
}

void assign_v0(const Toc& toc, std::map<int, Assignment>& out) {
    for (const PresentationInfoV0& p : toc.presentations_v0) {
        for (const int index : p.emdf_payloads_substream_indices) {
            if (!out.contains(index)) {
                Assignment a;
                a.kind = SubstreamReport::Kind::kEmdfPayloads;
                out.emplace(index, std::move(a));
            }
        }
        for (const auto& [role, chan] : p.substreams) {
            // Own claim before hsf_ext, matching assign_v1() - see its own
            // comment.
            const int classifier = chan.content_type ? chan.content_type->content_classifier : 0;
            const bool b_associated = role == "Associate" || role_from_classifier(classifier) == Role::kAssociated;
            const bool b_dialog = role == "Dialog" || classifier == 0b100;
            assign_instances(toc, chan, p.presentation_version, 0, b_associated, b_dialog, false, out);
            if (chan.hsf_ext_substream_index) {
                refuse(out, *chan.hsf_ext_substream_index, DecodeError::kUnsupported,
                       "HSF extension substreams are not decoded yet");
            }
        }
    }
}
}  // namespace

struct Decoder::Impl {
    DecoderConfig config{};
    std::map<int, AudioSubstreamState> audio;
    std::map<int, PresentationSubstreamState> presentation;
    std::optional<int> previous_sequence_counter;
};

Decoder::Decoder() : Decoder(DecoderConfig{}) {}

Decoder::Decoder(const DecoderConfig& config) : impl_(std::make_unique<Impl>()) {
    impl_->config = config;
}

Decoder::~Decoder() = default;
Decoder::Decoder(Decoder&&) noexcept = default;
Decoder& Decoder::operator=(Decoder&&) noexcept = default;

void Decoder::reset() {
    impl_->audio.clear();
    impl_->presentation.clear();
}

std::expected<FrameReport, DecodeError> Decoder::parse(std::span<const std::byte> raw_ac4_frame) {
    auto frame = ac4::parse_raw_frame(raw_ac4_frame);
    if (!frame) {
        return std::unexpected(DecodeError::kInvalidToc);
    }
    apply_observed_stereo_rule(frame->toc);
    const Toc& toc = frame->toc;

    // Part 1 4.3.3.2.2: a frame continues the stream when its sequence_counter
    // is the previous one plus 1, wraps from 1020 to 1, or follows a 0 (the
    // splice mark). Anything else is a change of source, and nothing carried
    // from before it may be used; frames that need configuration wait for
    // the next I-frame.
    if (impl_->previous_sequence_counter) {
        const int previous = *impl_->previous_sequence_counter;
        const int counter = toc.sequence_counter;
        const bool continues = counter == previous + 1 || (counter == 1 && previous == 1020) ||
                               (counter != 0 && previous == 0);
        if (!continues) {
            reset();
        }
    }
    impl_->previous_sequence_counter = toc.sequence_counter;

    FrameReport report;
    report.sequence_counter = toc.sequence_counter;
    report.b_iframe_global = toc.b_iframe_global;

    std::map<int, Assignment> assignments;
    // Part 2 clause 5.1.3: above a frame rate of 30 fps a presentation can
    // spread one coded frame over 2 or 4 transmission frames, each carrying
    // fragments of its substreams rather than whole ones. Assembling them
    // needs a queue of partial frames this phase does not keep, so every
    // substream of such a frame is refused by name, before anything claims it
    // - reading a fragment as a whole substream reports a legal stream as a
    // damaged one.
    const bool fragmented =
        std::ranges::any_of(toc.presentations_v1, [](const PresentationInfoV1& presentation) {
            return presentation.frame_rate_fraction != 1;
        });
    if (fragmented) {
        for (std::size_t index = 0; index < frame->substreams.size(); ++index) {
            refuse(assignments, static_cast<int>(index), DecodeError::kUnsupported,
                   "a frame of the efficient high frame rate mode, whose substreams are fragments");
        }
    }
    if (toc.bitstream_version >= 2) {
        assign_v1(toc, assignments);
    } else {
        assign_v0(toc, assignments);
    }

    for (auto& [index, assignment] : assignments) {
        SubstreamReport substream;
        substream.index = index;
        substream.kind = assignment.kind;
        if (index < 0 || static_cast<std::size_t>(index) >= frame->substreams.size()) {
            substream.refused = DecodeError::kInvalidStream;
            substream.refused_reason = "a substream index outside the substream index table";
            report.substreams.push_back(substream);
            continue;
        }
        if (assignment.refusal) {
            substream.refused = assignment.refusal->error;
            substream.refused_reason = assignment.refusal->reason;
            report.substreams.push_back(substream);
            continue;
        }
        const Substream& located = frame->substreams[static_cast<std::size_t>(index)];
        substream.size_bits = located.size * 8U;
        if (located.offset + located.size > raw_ac4_frame.size()) {
            substream.refused = DecodeError::kTruncated;
            substream.refused_reason = "the substream runs past the end of the frame";
            report.substreams.push_back(substream);
            continue;
        }
        BitReader reader(raw_ac4_frame.subspan(located.offset, located.size), index, impl_->config.syntax);
        ParseResult result;
        switch (assignment.kind) {
            case SubstreamReport::Kind::kAudio: {
                // What one substream carries from frame to frame belongs to
                // its channel mode and substream syntax version; a change of
                // either starts it afresh. The slot is the series' first
                // index, which is this substream's own outside a frame-rate-
                // multiplied series (assign_instances()).
                AudioSubstreamState& state = impl_->audio[assignment.state_key];
                if (state.ch_mode != assignment.audio.ch_mode || state.sus_ver != assignment.audio.sus_ver) {
                    state = AudioSubstreamState{};
                    state.ch_mode = assignment.audio.ch_mode;
                    state.sus_ver = assignment.audio.sus_ver;
                }
                AudioSubstream parsed;
                result = detail::parse_audio_substream(reader, assignment.audio, state, parsed);
                break;
            }
            case SubstreamReport::Kind::kPresentation: {
                PresentationSubstream parsed;
                result = detail::parse_presentation_substream(reader, *assignment.presentation,
                                                              impl_->presentation[index], parsed);
                break;
            }
            case SubstreamReport::Kind::kEmdfPayloads: {
                detail::EmdfPayloads parsed;
                result = detail::parse_emdf_payloads_substream(reader, parsed);
                break;
            }
            default:
                break;
        }
        substream.bits_read = reader.position();
        if (!result) {
            substream.refused = result.error().error;
            substream.refused_reason = result.error().reason;
        }
        report.substreams.push_back(substream);
    }
    return report;
}

}  // namespace ac4
