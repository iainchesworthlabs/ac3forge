#include "ac4dec/decoder.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "ac4/ac4.hpp"
#include "bit_reader.hpp"
#include "pcm/substream_pcm.hpp"
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

std::string_view describe(Speaker speaker) {
    switch (speaker) {
        case Speaker::kLeft:
            return "L";
        case Speaker::kRight:
            return "R";
        case Speaker::kCentre:
            return "C";
        case Speaker::kLfe:
            return "LFE";
        case Speaker::kLeftSurround:
            return "Ls";
        case Speaker::kRightSurround:
            return "Rs";
        case Speaker::kLeftBack:
            return "Lb";
        case Speaker::kRightBack:
            return "Rb";
        case Speaker::kLeftWide:
            return "Lw";
        case Speaker::kRightWide:
            return "Rw";
        case Speaker::kTopFrontLeft:
            return "Tfl";
        case Speaker::kTopFrontRight:
            return "Tfr";
        case Speaker::kTopBackLeft:
            return "Tbl";
        case Speaker::kTopBackRight:
            return "Tbr";
        case Speaker::kTopSideLeft:
            return "Tsl";
        case Speaker::kTopSideRight:
            return "Tsr";
    }
    return "?";
}

std::string_view describe(DecodingMode mode) {
    switch (mode) {
        case DecodingMode::kFull:
            return "full";
        case DecodingMode::kCore:
            return "core";
    }
    return "?";
}

std::string_view describe(DownmixTarget target) {
    switch (target) {
        case DownmixTarget::kAsCoded:
            return "as coded";
        case DownmixTarget::k5X:
            return "5.X";
        case DownmixTarget::kStereo:
            return "stereo";
        case DownmixTarget::kLoRo:
            return "Lo/Ro";
        case DownmixTarget::kLtRt:
            return "Lt/Rt";
        case DownmixTarget::kMono:
            return "mono";
        case DownmixTarget::k7X4:
            return "7.X.4";
        case DownmixTarget::k7X2:
            return "7.X.2";
        case DownmixTarget::k7X0:
            return "7.X.0";
        case DownmixTarget::k5X4:
            return "5.X.4";
        case DownmixTarget::k5X2:
            return "5.X.2";
    }
    return "?";
}

std::string_view describe(DrcMode mode) {
    switch (mode) {
        case DrcMode::kOff:
            return "off";
        case DrcMode::kDefault:
            return "default";
        case DrcMode::kHomeTheatre:
            return "home theatre";
        case DrcMode::kFlatPanelTv:
            return "flat panel TV";
        case DrcMode::kPortableSpeakers:
            return "portable speakers";
        case DrcMode::kPortableHeadphones:
            return "portable headphones";
    }
    return "?";
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
    // For a kAudio assignment: the raw ac4_hsf_ext_substream_info() index its
    // group/channel carried, if any - resolved against the map in parse()
    // once every substream's own claim has been made, since the two are read
    // from different elements (see assign_v1()/assign_v0()).
    std::optional<int> hsf_ext_index;
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

// Claims `index` as an ac4_hsf_ext_substream(), the way refuse() claims one
// as kOther - a substream named twice (Part 1 Table 15) is read as the first
// element names it, so a collision (including a self-reference, where the
// owning channel's own claim on `index` already stands) leaves this call a
// no-op, exactly as it would if the second claim were a refusal instead.
void claim_hsf_ext(std::map<int, Assignment>& out, int index) {
    if (out.contains(index)) {
        return;
    }
    Assignment a;
    a.kind = SubstreamReport::Kind::kHsfExt;
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
    ctx.sf_multiplier = chan.sf_multiplier;
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
                    if (sub.hsf_ext_substream_index && sub.chan->substream_index) {
                        if (auto it = out.find(*sub.chan->substream_index);
                            it != out.end() && it->second.kind == SubstreamReport::Kind::kAudio) {
                            it->second.hsf_ext_index = *sub.hsf_ext_substream_index;
                        }
                    }
                }
                if (sub.hsf_ext_substream_index) {
                    claim_hsf_ext(out, *sub.hsf_ext_substream_index);
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
                if (chan.substream_index) {
                    if (auto it = out.find(*chan.substream_index);
                        it != out.end() && it->second.kind == SubstreamReport::Kind::kAudio) {
                        it->second.hsf_ext_index = *chan.hsf_ext_substream_index;
                    }
                }
                claim_hsf_ext(out, *chan.hsf_ext_substream_index);
            }
        }
    }
}
// What decode() turns into PCM: the first channel-coded substream of the first
// presentation that has one, in the order the table of contents lists
// presentations and, within one, its substream groups; and that
// presentation's presentation substream, where it has one.
struct Target {
    std::optional<int> audio;
    std::optional<int> presentation;
};

[[nodiscard]] Target decode_target(const Toc& toc) {
    if (toc.bitstream_version >= 2) {
        for (const PresentationInfoV1& p : toc.presentations_v1) {
            for (const int group_index : p.group_refs) {
                if (group_index < 0 || static_cast<std::size_t>(group_index) >= toc.substream_groups.size()) {
                    continue;
                }
                const SubstreamGroupInfo& group = toc.substream_groups[static_cast<std::size_t>(group_index)];
                if (!group.b_substreams_present) {
                    continue;
                }
                for (const GroupSubstream& sub : group.substreams) {
                    if (sub.kind == GroupSubstream::Kind::kChan && sub.chan && sub.chan->substream_index) {
                        return {.audio = *sub.chan->substream_index,
                                .presentation = p.presentation_substream_index};
                    }
                }
            }
        }
        return {};
    }
    for (const PresentationInfoV0& p : toc.presentations_v0) {
        for (const auto& [role, chan] : p.substreams) {
            if (chan.substream_index) {
                return {.audio = *chan.substream_index, .presentation = std::nullopt};
            }
        }
    }
    return {};
}

// What decode() keeps of the substreams it decodes, from the walk parse()
// makes of the whole frame.
struct Capture {
    std::optional<int> index;
    int state_key = 0;
    SubstreamContext context{};
    AudioSubstream content{};
    bool read = false;  // read to its end, with no refusal
    // The presentation substream of the presentation decoded.
    std::optional<int> presentation_index;
    PresentationSubstream presentation{};
    bool presentation_read = false;
};

}  // namespace

struct Decoder::Impl {
    DecoderConfig config{};
    std::map<int, AudioSubstreamState> audio;
    std::map<int, PresentationSubstreamState> presentation;
    // decode()'s reconstruction state, keyed as `audio` is.
    std::map<int, detail::SubstreamPcm> pcm;
    std::optional<int> previous_sequence_counter;
    // Part 2 clause 5.11's phi_t of the last frame decode() read, which a
    // change of source does not forget: the 0 a splicer writes continues it.
    std::optional<int> converter_phase;
    std::string_view refusal;
    // The key in `pcm` of the substream decode() last output, and its rate.
    std::optional<int> last_key;
    int last_rate = 0;
    // Set by a change of source until a frame decodes.
    bool new_source = false;

    // A change of source (Part 1 clause 4.3.3.2.2): what was read from the
    // stream goes, and the signal of the substream that output last carries
    // on, so that its audio comes out to its end and overlaps the new
    // source's first frame (src/ac4dec/ERRATA.md, "A change of source").
    void forget_stream() {
        audio.clear();
        presentation.clear();
        std::erase_if(pcm, [this](const auto& entry) { return entry.first != last_key; });
        new_source = true;
    }

    // Drops the signal too, so that the next frame decoded starts from
    // silence.
    void forget_signal() {
        pcm.clear();
        last_key.reset();
    }

    // The substream a concealed frame comes from, the one that output last;
    // null without a concealment policy or a frame decoded to conceal from.
    [[nodiscard]] detail::SubstreamPcm* concealment_source() {
        if (config.concealment == ConcealmentPolicy::kNone || !last_key) {
            return nullptr;
        }
        const auto it = pcm.find(*last_key);
        return it != pcm.end() && it->second.can_conceal() ? &it->second : nullptr;
    }

    // A frame of concealed output in place of the frame that failed with
    // `error`, at the sequence_counter and phase decode() took it to have; the
    // error where there is no concealment source.
    [[nodiscard]] std::expected<std::optional<DecodedFrame>, DecodeError> conceal_or(
        DecodeError error);

    // Reads every substream of the frame; with `capture`, keeps the content of
    // decode()'s substream as well.
    [[nodiscard]] std::expected<FrameReport, DecodeError> read(std::span<const std::byte> raw_ac4_frame,
                                                               Capture* capture);
};

std::expected<std::optional<DecodedFrame>, DecodeError> Decoder::Impl::conceal_or(
    DecodeError error) {
    detail::SubstreamPcm* const source = concealment_source();
    if (source == nullptr) {
        return std::unexpected(error);
    }
    DecodedFrame frame;
    frame.sample_rate_hz = last_rate;
    frame.sequence_counter = previous_sequence_counter.value_or(0);
    const detail::FrameInputs inputs{.sequence_counter = frame.sequence_counter,
                                     .converter_phase = converter_phase.value_or(0),
                                     .new_source = false,
                                     .output = config.output,
                                     .drc = {},
                                     .de = {},
                                     .downmix = {},
                                     .decoding = config.decoding};
    if (!source->conceal(config.concealment, inputs, frame.channels, frame.speakers)) {
        return std::unexpected(error);
    }
    frame.concealed = Concealment{.error = error,
                                  .action = config.concealment == ConcealmentPolicy::kRepeatFade
                                                ? ConcealmentAction::kRepeatFade
                                                : ConcealmentAction::kMute};
    return std::optional<DecodedFrame>{std::move(frame)};
}

Decoder::Decoder() : Decoder(DecoderConfig{}) {}

Decoder::Decoder(const DecoderConfig& config) : impl_(std::make_unique<Impl>()) {
    impl_->config = config;
}

Decoder::~Decoder() = default;
Decoder::Decoder(Decoder&&) noexcept = default;
Decoder& Decoder::operator=(Decoder&&) noexcept = default;

void Decoder::reset() {
    impl_->forget_signal();
    impl_->forget_stream();
    impl_->new_source = false;
    impl_->converter_phase.reset();
    impl_->previous_sequence_counter.reset();
    impl_->last_rate = 0;
}

std::expected<FrameReport, DecodeError> Decoder::parse(std::span<const std::byte> raw_ac4_frame) {
    return impl_->read(raw_ac4_frame, nullptr);
}

std::string_view Decoder::refusal_reason() const noexcept {
    return impl_->refusal;
}

std::expected<std::optional<DecodedFrame>, DecodeError> Decoder::decode(std::span<const std::byte> raw_ac4_frame) {
    impl_->refusal = {};
    Capture capture;
    auto report = impl_->read(raw_ac4_frame, &capture);
    if (!report) {
        impl_->refusal = describe(report.error());
        // read() took the frame to be the one the stream expected; its phase
        // follows the last frame's.
        if (impl_->converter_phase) {
            impl_->converter_phase = (*impl_->converter_phase + 1) % 5;
        }
        return impl_->conceal_or(report.error());
    }
    // Part 2 clause 5.11: phi_t is sequence_counter modulo 5, but where a
    // splicer wrote 0 it goes on from the frame before, and it is 0 for a
    // first frame of 0.
    const int counter = report->sequence_counter;
    const int phase = counter != 0
                          ? counter % 5
                          : (impl_->converter_phase ? (*impl_->converter_phase + 1) % 5 : 0);
    impl_->converter_phase = phase;
    if (!capture.index) {
        impl_->refusal = "no presentation has a channel-coded substream";
        return impl_->conceal_or(DecodeError::kUnsupported);
    }
    const auto it = std::ranges::find(report->substreams, *capture.index, &SubstreamReport::index);
    if (it != report->substreams.end() && it->refused) {
        impl_->refusal = it->refused_reason;
        if (*it->refused == DecodeError::kMissingIFrame && impl_->concealment_source() == nullptr) {
            // Nothing comes out for this frame, so the signal before it is
            // dropped rather than resumed a gap later.
            impl_->forget_signal();
            return std::optional<DecodedFrame>{};
        }
        return impl_->conceal_or(*it->refused);
    }
    if (!capture.read) {
        impl_->refusal = "the substream to decode is not in the frame";
        return impl_->conceal_or(DecodeError::kInvalidStream);
    }
    DecodedFrame frame;
    frame.sample_rate_hz = capture.context.fs_index == 0 ? 44100 : 48000;
    frame.sequence_counter = report->sequence_counter;
    // Dialnorm and DRC come from the presentation substream where the
    // presentation has one, and otherwise from the audio substream's
    // metadata() (Part 2 clauses 4.8.5.2 and 4.8.6).
    detail::FrameInputs inputs{.sequence_counter = report->sequence_counter,
                               .converter_phase = phase,
                               .new_source = impl_->new_source,
                               .output = impl_->config.output,
                               .drc = {},
                               .de = {},
                               .downmix = {},
                               .decoding = impl_->config.decoding};
    std::optional<double> dialnorm;
    const detail::DrcState* drc_state = nullptr;
    const detail::DrcFrame* drc_frame = nullptr;
    if (capture.presentation_read && capture.presentation_index) {
        dialnorm = -0.25 * static_cast<double>(capture.presentation.dialnorm_bits);
        drc_state = &impl_->presentation[*capture.presentation_index].drc;
        drc_frame = &capture.presentation.drc;
    } else {
        const detail::Metadata& metadata = capture.content.metadata;
        if (metadata.basic.dialnorm_bits) {
            dialnorm = -0.25 * static_cast<double>(*metadata.basic.dialnorm_bits);
        }
        if (metadata.drc) {
            drc_state = &impl_->audio[capture.state_key].metadata.drc;
            drc_frame = &*metadata.drc;
        }
    }
    inputs.drc = detail::drc_frame_values(impl_->config.output, dialnorm, drc_state, drc_frame);
    inputs.de = detail::de_frame_values(capture.content.metadata.dialog_enhancement);
    inputs.downmix = detail::downmix_values(
        capture.presentation_read ? &capture.presentation : nullptr, capture.content.metadata);
    const detail::ParseResult decoded = impl_->pcm[capture.state_key].decode(
        capture.context, capture.content, inputs, frame.channels, frame.speakers);
    if (!decoded) {
        impl_->refusal = decoded.error().reason;
        return impl_->conceal_or(decoded.error().error);
    }
    impl_->last_key = capture.state_key;
    impl_->last_rate = frame.sample_rate_hz;
    impl_->new_source = false;
    return std::optional<DecodedFrame>{std::move(frame)};
}

std::expected<FrameReport, DecodeError> Decoder::Impl::read(std::span<const std::byte> raw_ac4_frame,
                                                            Capture* capture) {
    auto frame = ac4::parse_raw_frame(raw_ac4_frame);
    if (!frame) {
        // The frame is taken to be the one the stream expected next, so that
        // one damaged frame is not a change of source. After a splice mark
        // any counter but 0 continues, and still does.
        if (previous_sequence_counter && *previous_sequence_counter != 0) {
            previous_sequence_counter =
                *previous_sequence_counter == 1020 ? 1 : *previous_sequence_counter + 1;
        }
        return std::unexpected(DecodeError::kInvalidToc);
    }
    apply_observed_stereo_rule(frame->toc);
    const Toc& toc = frame->toc;
    if (capture != nullptr) {
        const Target target = decode_target(toc);
        capture->index = target.audio;
        capture->presentation_index = target.presentation;
    }

    // Part 1 4.3.3.2.2: a frame continues the stream when its sequence_counter
    // is the previous one plus 1, wraps from 1020 to 1, or follows a 0 (the
    // splice mark). Anything else is a change of source, and nothing read
    // from before it may be used; frames that need configuration wait for
    // the next I-frame.
    if (previous_sequence_counter) {
        const int previous = *previous_sequence_counter;
        const int counter = toc.sequence_counter;
        const bool continues = counter == previous + 1 || (counter == 1 && previous == 1020) ||
                               (counter != 0 && previous == 0);
        if (!continues) {
            forget_stream();
        }
    }
    previous_sequence_counter = toc.sequence_counter;

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
        std::ranges::any_of(toc.presentations_v1, [](const PresentationInfoV1& info) {
            return info.frame_rate_fraction != 1;
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

    // Owner substreams whose ac4_hsf_ext_substream_info() names a distinct,
    // still-unclaimed substream, on a channel that actually reports
    // sf_multiplier (Table 89 gives no HSF extension table for plain
    // 48 kHz, so a link without it - and a self-reference, since
    // assign_v1()/assign_v0() never set hsf_ext_index for one - is left for
    // the general handling below, which reads the channel plainly and
    // refuses the orphaned extension). Resolved together, in whichever order
    // this map holds them: the owner's own asf_section_data() needs
    // max_sfb_ext_hsf, a value only the extension's own bits carry, before
    // either can be fully read (ERRATA.md).
    std::vector<int> resolved;
    std::set<int> claimed_ext;
    for (auto& [index, assignment] : assignments) {
        if (assignment.kind != SubstreamReport::Kind::kAudio || !assignment.hsf_ext_index ||
            !assignment.audio.sf_multiplier || *assignment.hsf_ext_index == index) {
            continue;
        }
        const int ext_index = *assignment.hsf_ext_index;
        const auto ext_it = assignments.find(ext_index);
        if (ext_it == assignments.end() || ext_it->second.kind != SubstreamReport::Kind::kHsfExt ||
            !claimed_ext.insert(ext_index).second) {
            continue;
        }

        SubstreamReport owner_report;
        owner_report.index = index;
        owner_report.kind = SubstreamReport::Kind::kAudio;
        SubstreamReport ext_report;
        ext_report.index = ext_index;
        ext_report.kind = SubstreamReport::Kind::kHsfExt;
        resolved.push_back(index);
        resolved.push_back(ext_index);

        const bool owner_in_range = index >= 0 && static_cast<std::size_t>(index) < frame->substreams.size();
        const bool ext_in_range = ext_index >= 0 && static_cast<std::size_t>(ext_index) < frame->substreams.size();
        if (!owner_in_range || !ext_in_range) {
            owner_report.refused = DecodeError::kInvalidStream;
            owner_report.refused_reason = "a substream index outside the substream index table";
            ext_report.refused = DecodeError::kInvalidStream;
            ext_report.refused_reason = "a substream index outside the substream index table";
            report.substreams.push_back(owner_report);
            report.substreams.push_back(ext_report);
            continue;
        }
        const Substream& owner_loc = frame->substreams[static_cast<std::size_t>(index)];
        const Substream& ext_loc = frame->substreams[static_cast<std::size_t>(ext_index)];
        owner_report.size_bits = owner_loc.size * 8U;
        ext_report.size_bits = ext_loc.size * 8U;
        if (owner_loc.offset + owner_loc.size > raw_ac4_frame.size() ||
            ext_loc.offset + ext_loc.size > raw_ac4_frame.size()) {
            owner_report.refused = DecodeError::kTruncated;
            owner_report.refused_reason = "the substream runs past the end of the frame";
            ext_report.refused = DecodeError::kTruncated;
            ext_report.refused_reason = "the substream runs past the end of the frame";
            report.substreams.push_back(owner_report);
            report.substreams.push_back(ext_report);
            continue;
        }

        BitReader owner_reader(raw_ac4_frame.subspan(owner_loc.offset, owner_loc.size), index, config.syntax);
        BitReader ext_reader(raw_ac4_frame.subspan(ext_loc.offset, ext_loc.size), ext_index, config.syntax);
        AudioSubstreamState& state = audio[assignment.state_key];
        if (state.ch_mode != assignment.audio.ch_mode || state.sus_ver != assignment.audio.sus_ver) {
            state = AudioSubstreamState{};
            state.ch_mode = assignment.audio.ch_mode;
            state.sus_ver = assignment.audio.sus_ver;
        }
        AudioSubstream parsed;
        const ParseResult owner_result =
            detail::parse_audio_substream(owner_reader, assignment.audio, state, parsed, &ext_reader);
        owner_report.bits_read = owner_reader.position();
        if (!owner_result) {
            owner_report.refused = owner_result.error().error;
            owner_report.refused_reason = owner_result.error().reason;
            ext_report.refused = DecodeError::kUnsupported;
            ext_report.refused_reason = "its owning channel substream could not be read";
            ext_report.bits_read = ext_reader.position();
        } else {
            ParseResult ext_result;
            for (detail::Track& track : parsed.element.tracks) {
                const int groups =
                    parsed.element.infos[static_cast<std::size_t>(track.info)].psy.num_window_groups;
                ext_result = detail::parse_sf_hsf_data(ext_reader, groups, track.data, track.hsf);
                if (!ext_result) {
                    break;
                }
            }
            if (ext_result) {
                ext_reader.align();
                ext_result = detail::check(ext_reader);
            }
            ext_report.bits_read = ext_reader.position();
            if (!ext_result) {
                ext_report.refused = ext_result.error().error;
                ext_report.refused_reason = ext_result.error().reason;
            } else if (capture != nullptr && capture->index == index) {
                capture->state_key = assignment.state_key;
                capture->context = assignment.audio;
                capture->content = std::move(parsed);
                capture->read = true;
            }
        }
        report.substreams.push_back(owner_report);
        report.substreams.push_back(ext_report);
    }
    for (const int index : resolved) {
        assignments.erase(index);
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
        BitReader reader(raw_ac4_frame.subspan(located.offset, located.size), index, config.syntax);
        ParseResult result;
        switch (assignment.kind) {
            case SubstreamReport::Kind::kAudio: {
                // What one substream carries from frame to frame belongs to
                // its channel mode and substream syntax version; a change of
                // either starts it afresh. The slot is the series' first
                // index, which is this substream's own outside a frame-rate-
                // multiplied series (assign_instances()).
                AudioSubstreamState& state = audio[assignment.state_key];
                if (state.ch_mode != assignment.audio.ch_mode || state.sus_ver != assignment.audio.sus_ver) {
                    state = AudioSubstreamState{};
                    state.ch_mode = assignment.audio.ch_mode;
                    state.sus_ver = assignment.audio.sus_ver;
                }
                AudioSubstream parsed;
                result = detail::parse_audio_substream(reader, assignment.audio, state, parsed);
                if (result && capture != nullptr && capture->index == index) {
                    capture->state_key = assignment.state_key;
                    capture->context = assignment.audio;
                    capture->content = std::move(parsed);
                    capture->read = true;
                }
                break;
            }
            case SubstreamReport::Kind::kPresentation: {
                PresentationSubstream parsed;
                result = detail::parse_presentation_substream(reader, *assignment.presentation,
                                                              presentation[index], parsed);
                if (result && capture != nullptr && capture->presentation_index == index) {
                    capture->presentation = std::move(parsed);
                    capture->presentation_read = true;
                }
                break;
            }
            case SubstreamReport::Kind::kEmdfPayloads: {
                detail::EmdfPayloads parsed;
                result = detail::parse_emdf_payloads_substream(reader, parsed);
                break;
            }
            case SubstreamReport::Kind::kHsfExt:
                // Reaching this case rather than the combined handling above
                // means its owning channel substream either does not exist,
                // has no sf_multiplier, or is this same substream (a
                // self-reference) - see the loop above.
                result = detail::fail(DecodeError::kUnsupported,
                                      "no active HSF extension was read alongside its owning channel substream");
                break;
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
    std::ranges::sort(report.substreams, {}, &SubstreamReport::index);
    return report;
}

}  // namespace ac4
