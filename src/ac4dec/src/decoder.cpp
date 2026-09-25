#include "ac4dec/decoder.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include "ac4/ac4.hpp"
#include "bit_reader.hpp"
#include "pcm/objects.hpp"
#include "pcm/substream_pcm.hpp"
#include "presentations.hpp"
#include "syntax/context.hpp"
#include "syntax/metadata.hpp"
#include "syntax/oamd.hpp"
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
        case Speaker::kLfe2:
            return "LFE2";
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
using detail::Role;
using detail::role_from_classifier;
using detail::role_v1;
using detail::SubstreamContext;

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
    // An object audio substream's objects (SubstreamContext::coding kAjoc or
    // kObjects), and a kOamd substream's context.
    std::optional<detail::ObjectAudioContext> objects;
    std::optional<detail::OamdSubstreamContext> oamd;
    // The index of the OAMD substream of the group an object audio substream
    // or an OAMD substream belongs to, whose carried state (the timing) the
    // group's substreams share; unset for a group without one.
    std::optional<int> oamd_key;
    // An object audio substream's objects as decode() puts them out, in full
    // and in core decoding, the LFE first (SubstreamPcm's object order); and
    // where its objects start in its group's list, oamd_dyndata_multi()'s.
    std::vector<ObjectEntry> essences_full;
    std::vector<ObjectEntry> essences_core;
    int group_offset = 0;
    // An A-JOC substream's oamd_common_data() from the table of contents.
    std::optional<OamdCommonData> object_common;
};

[[nodiscard]] detail::ObjType obj_type_of(ObjectKind kind) noexcept {
    switch (kind) {
        case ObjectKind::kBed:
            return detail::ObjType::kBed;
        case ObjectKind::kIsf:
            return detail::ObjType::kIsf;
        default:
            return detail::ObjType::kDyn;
    }
}

// The objects of an A-JOC substream's OAMD portion (Part 2 clause 6.2.3.4): the
// LFE first where b_lfe is set (is_lfe[0] = 1), then the bed or intermediate
// spatial format objects bed_dyn_obj_assignment() lists, then dynamic objects
// up to the portion's fullband count (src/ac4dec/ERRATA.md, "The objects of an
// A-JOC substream"). False where the assignment lists more than the count,
// which 6.3.2.8.1 leaves undefined.
[[nodiscard]] bool ajoc_portion(const std::vector<ObjectEntry>& assigned, int n_fullband,
                                bool b_lfe, detail::OamdObjectList& out) {
    out = detail::OamdObjectList{};
    if (static_cast<int>(assigned.size()) > n_fullband) {
        return false;
    }
    if (b_lfe) {
        out.push({.type = detail::ObjType::kDyn, .lfe = true, .ajoc_coded = true});
    }
    for (const ObjectEntry& entry : assigned) {
        out.push({.type = obj_type_of(entry.kind), .lfe = false, .ajoc_coded = true});
    }
    for (int i = static_cast<int>(assigned.size());
         i < n_fullband && out.count <= detail::kMaxOamdObjects; ++i) {
        out.push({.type = detail::ObjType::kDyn, .lfe = false, .ajoc_coded = true});
    }
    return true;
}

// A bed's or intermediate spatial format's objects, as the direct-coded
// substream that starts them lists them, and how many of them the group's
// substreams have taken so far (src/ac4dec/ERRATA.md, "The objects of a
// direct-coded substream").
struct StaticRun {
    std::vector<ObjectEntry> objects;
    bool isf = false;
    std::size_t next = 0;  // the next fullband object to take
    int lfes_given = 0;    // the LFEs given to the run's substreams so far
};

// What a direct-coded substream codes: its objects in the order its audio
// carries them (an LFE, then its element's channels), and the element's
// fullband count and LFE. A refusal where the table of contents gives no such
// share.
struct ObjectShare {
    detail::OamdObjectList objects{};
    std::vector<ObjectEntry> entries;  // the same objects, as the table of contents lists them
    int n_objects = 0;
    bool b_lfe = false;
    std::optional<detail::SyntaxError> refusal;
};

[[nodiscard]] ObjectShare object_share(const ObjSubstreamInfo& info,
                                       std::optional<StaticRun>& run) {
    ObjectShare share;
    if (!info.num_objects) {
        share.refusal =
            detail::SyntaxError{DecodeError::kInvalidStream, "a reserved n_objects_code"};
        return share;
    }
    share.n_objects = *info.num_objects;
    const auto add = [&share](const ObjectEntry& entry) {
        share.objects.push(
            {.type = obj_type_of(entry.kind), .lfe = entry.lfe, .ajoc_coded = false});
        share.entries.push_back(entry);
    };
    if (info.b_dynamic_objects) {
        share.b_lfe = info.b_lfe;
        for (const ObjectEntry& entry : info.objects) {
            add(entry);
        }
        return share;
    }
    if (info.static_kind == ObjSubstreamInfo::Static::kReserved) {
        if (share.n_objects != 0) {
            share.refusal = detail::SyntaxError{DecodeError::kUnsupported,
                                                "objects a substream of reserved data describes"};
        }
        return share;
    }
    const bool isf = info.static_kind == ObjSubstreamInfo::Static::kIsf;
    if (info.static_start) {
        run = StaticRun{.objects = info.objects, .isf = isf, .next = 0, .lfes_given = 0};
    } else if (!run || run->isf != isf) {
        share.refusal =
            detail::SyntaxError{DecodeError::kInvalidStream,
                                "a substream that extends a bed or intermediate spatial format "
                                "no substream before it started"};
        return share;
    }
    // The run's LFEs go one to each of its first substreams, LFE to the first
    // and LFE2 to the second (the NOTE after Part 2 clause 6.3.2.10.6); its
    // fullband objects go to its substreams in order, n_objects each.
    int lfe_seen = 0;
    for (const ObjectEntry& entry : run->objects) {
        if (!entry.lfe) {
            continue;
        }
        if (lfe_seen++ == run->lfes_given) {
            add(entry);
            share.b_lfe = true;
            ++run->lfes_given;
            break;
        }
    }
    int taken = 0;
    while (taken < share.n_objects && run->next < run->objects.size()) {
        const ObjectEntry& entry = run->objects[run->next++];
        if (!entry.lfe) {
            add(entry);
            ++taken;
        }
    }
    if (taken < share.n_objects) {
        share.refusal =
            detail::SyntaxError{DecodeError::kInvalidStream,
                                "a substream that codes more objects than its bed assigns"};
    }
    return share;
}

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

// The context of an object audio substream - A-JOC coded or direct coded - of
// `coding`, at `index`, instance of a series starting at `first` of
// `b_iframe.size()` instances (Part 1 4.3.3.7.9, as assign_instances() reads
// it for channel-coded substreams). `fill` completes the context's object
// fields. Its channel_mode is negative (Part 2 6.2.2.2's NOTE 2).
// An object audio substream's objects as decode() puts them out, full and
// core, and where they start in the group's list.
struct Essences {
    std::vector<ObjectEntry> full;
    std::vector<ObjectEntry> core;
    int group_offset = 0;
    std::optional<OamdCommonData> object_common;
};

template <typename Fill>
void assign_object_instances(const Toc& toc, std::optional<int> first_index,
                             const std::vector<bool>& b_iframe, std::optional<int> sf_multiplier,
                             int presentation_version, bool b_associated, bool b_dialog,
                             bool b_alternative, const detail::ObjectAudioContext& objects,
                             std::optional<int> oamd_key, const Essences& essences, Fill fill,
                             std::map<int, Assignment>& out) {
    if (!first_index) {
        return;
    }
    const std::size_t instances = b_iframe.empty() ? 1 : b_iframe.size();
    const std::int64_t first = *first_index;
    for (std::size_t i = 0; i < instances; ++i) {
        const std::int64_t wide = first + static_cast<std::int64_t>(i);
        if (wide > std::numeric_limits<int>::max()) {
            break;
        }
        const int index = static_cast<int>(wide);
        if (out.contains(index)) {
            continue;
        }
        const int base = detail::frame_len_base(toc.frame_rate_index, toc.sample_rate_hz);
        if (base == 0) {
            refuse(out, index, DecodeError::kInvalidStream, "a reserved frame_rate_index");
            continue;
        }
        const auto factor = static_cast<int>(instances);
        if (base % factor != 0) {
            refuse(out, index, DecodeError::kInvalidStream,
                   "a frame rate factor the frame length does not divide by");
            continue;
        }
        Assignment a;
        a.kind = SubstreamReport::Kind::kAudio;
        a.state_key = static_cast<int>(first);
        SubstreamContext& ctx = a.audio;
        ctx.bitstream_version = toc.bitstream_version;
        ctx.presentation_version = presentation_version;
        ctx.fs_index = toc.sample_rate_hz == 44100 ? 0 : 1;
        ctx.frame_rate_index = toc.frame_rate_index;
        ctx.frame_len_base = base / factor;
        ctx.b_iframe = !b_iframe.empty() && b_iframe[i];
        ctx.sus_ver = 1;
        ctx.ch_mode = -1;
        ctx.sf_multiplier = sf_multiplier;
        ctx.b_associated = b_associated;
        ctx.b_dialog = b_dialog;
        ctx.b_alternative = b_alternative;
        fill(ctx);
        a.objects = objects;
        a.oamd_key = oamd_key;
        a.essences_full = essences.full;
        a.essences_core = essences.core;
        a.group_offset = essences.group_offset;
        a.object_common = essences.object_common;
        out.emplace(index, std::move(a));
    }
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
            // The group's OAMD substream is named before its substreams
            // (6.2.1.6), so its claim comes first; the objects its
            // oamd_dyndata_multi() lists are the group's substreams' in order,
            // filled in below.
            std::optional<int> oamd_key;
            bool oamd_claimed = false;
            if (group.oamd && group.oamd->substream_index) {
                const int index = *group.oamd->substream_index;
                oamd_key = index;
                if (!out.contains(index)) {
                    Assignment a;
                    a.kind = SubstreamReport::Kind::kOamd;
                    a.oamd = detail::OamdSubstreamContext{};
                    a.oamd->b_oamd_ndot = group.oamd->b_oamd_ndot;
                    a.oamd->b_alternative = p.b_alternative;
                    a.oamd_key = index;
                    out.emplace(index, std::move(a));
                    oamd_claimed = true;
                }
            }
            const int classifier = group.content_type ? group.content_type->content_classifier : 0;
            const bool b_associated = role == Role::kAssociated || role_from_classifier(classifier) == Role::kAssociated;
            const bool b_dialog = role == Role::kDialogue || classifier == 0b100;
            detail::OamdObjectList group_objects;
            std::optional<StaticRun> run;
            const auto refuse_series = [&out](std::optional<int> first, std::size_t instances,
                                              DecodeError error, std::string_view reason) {
                if (!first) {
                    return;
                }
                for (std::size_t i = 0; i < std::max<std::size_t>(instances, 1); ++i) {
                    const std::int64_t index = std::int64_t{*first} + static_cast<std::int64_t>(i);
                    if (index > std::numeric_limits<int>::max()) {
                        break;
                    }
                    refuse(out, static_cast<int>(index), error, reason);
                }
            };
            for (const GroupSubstream& sub : group.substreams) {
                // The substream's own claim on its own index goes first,
                // matching Python's substream_roles() (audio() before the
                // hsf_ext put()) - a fuzzed stream can send an
                // hsf_ext_substream_index equal to the substream's own
                // index, and out.contains()'s first-claim-wins means the
                // two transcriptions would otherwise disagree on which
                // claim that self-reference resolves to.
                if (sub.kind == GroupSubstream::Kind::kAjoc && sub.ajoc) {
                    const AjocSubstreamInfo& info = *sub.ajoc;
                    detail::ObjectAudioContext objects;
                    const bool dmx_ok =
                        info.b_static_dmx ||
                        ajoc_portion(info.static_objects, info.n_fullband_dmx_signals, info.b_lfe,
                                     objects.dmx);
                    const bool umx_ok = ajoc_portion(
                        info.upmix_objects, info.n_fullband_upmix_signals, info.b_lfe, objects.umx);
                    // Its objects: in full decoding the upmix's, in core
                    // decoding the downmix's, or a static downmix's bed (L, R,
                    // C, Ls and Rs, Table A.27's 0 to 4); the LFE first.
                    Essences essences;
                    essences.group_offset = group_objects.count;
                    const auto portion = [&info](const std::vector<ObjectEntry>& assigned,
                                                 int count, std::vector<ObjectEntry>& out_list) {
                        if (info.b_lfe) {
                            out_list.push_back({.kind = ObjectKind::kBed,
                                                .lfe = true,
                                                .ajoc_coded = true,
                                                .speaker = 11});
                        }
                        for (const ObjectEntry& entry : assigned) {
                            out_list.push_back(entry);
                        }
                        for (int i = static_cast<int>(assigned.size()); i < count; ++i) {
                            out_list.push_back({.kind = ObjectKind::kDyn,
                                                .lfe = false,
                                                .ajoc_coded = true,
                                                .speaker = {}});
                        }
                    };
                    portion(info.upmix_objects, info.n_fullband_upmix_signals, essences.full);
                    essences.object_common = info.oamd_common_data;
                    if (info.b_static_dmx) {
                        if (info.b_lfe) {
                            essences.core.push_back({.kind = ObjectKind::kBed,
                                                     .lfe = true,
                                                     .ajoc_coded = true,
                                                     .speaker = 11});
                        }
                        for (int s = 0; s < 5; ++s) {
                            essences.core.push_back({.kind = ObjectKind::kBed,
                                                     .lfe = false,
                                                     .ajoc_coded = true,
                                                     .speaker = s});
                        }
                    } else {
                        portion(info.static_objects, info.n_fullband_dmx_signals, essences.core);
                    }
                    // Every object counts, past the list's capacity too, so a
                    // group of more than it holds is refused where it is read.
                    for (int i = 0; i < objects.umx.count; ++i) {
                        group_objects.push(i < detail::kMaxOamdObjects ? objects.umx[i]
                                                                       : detail::OamdObjectType{});
                    }
                    if (!dmx_ok || !umx_ok) {
                        refuse_series(
                            info.substream_index, info.b_iframe.size(), DecodeError::kInvalidStream,
                            "bed_dyn_obj_assignment() assigns more objects than the signals");
                    } else if (objects.dmx.count > detail::kMaxOamdObjects ||
                               objects.umx.count > detail::kMaxOamdObjects) {
                        refuse_series(info.substream_index, info.b_iframe.size(),
                                      DecodeError::kUnsupported,
                                      "more A-JOC objects than the decoder describes");
                    } else {
                        assign_object_instances(
                            toc, info.substream_index, info.b_iframe, info.sf_multiplier,
                            p.presentation_version, b_associated, b_dialog, p.b_alternative,
                            objects, oamd_key, essences,
                            [&info](SubstreamContext& ctx) {
                                ctx.coding = detail::AudioCoding::kAjoc;
                                ctx.b_lfe = info.b_lfe;
                                ctx.b_static_dmx = info.b_static_dmx;
                                ctx.n_fullband_dmx = info.n_fullband_dmx_signals;
                                ctx.n_fullband_umx = info.n_fullband_upmix_signals;
                            },
                            out);
                    }
                } else if (sub.kind == GroupSubstream::Kind::kObj && sub.obj) {
                    const ObjSubstreamInfo& info = *sub.obj;
                    const ObjectShare share = object_share(info, run);
                    Essences essences;
                    essences.group_offset = group_objects.count;
                    essences.full = share.entries;
                    essences.core = share.entries;
                    for (int i = 0; i < share.objects.count; ++i) {
                        group_objects.push(i < detail::kMaxOamdObjects ? share.objects[i]
                                                                       : detail::OamdObjectType{});
                    }
                    if (share.refusal) {
                        refuse_series(info.substream_index, info.b_iframe.size(),
                                      share.refusal->error, share.refusal->reason);
                    } else {
                        detail::ObjectAudioContext objects;
                        objects.objects = share.objects;
                        assign_object_instances(
                            toc, info.substream_index, info.b_iframe, info.sf_multiplier,
                            p.presentation_version, b_associated, b_dialog, p.b_alternative,
                            objects, oamd_key, essences,
                            [&share](SubstreamContext& ctx) {
                                ctx.coding = detail::AudioCoding::kObjects;
                                ctx.b_lfe = share.b_lfe;
                                ctx.n_objects = share.n_objects;
                            },
                            out);
                    }
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
            if (oamd_claimed) {
                // 6.3.9.5: oamd_dyndata_multi() lists "all object essences
                // present over all audio substreams of the according substream
                // group in the order of bitstream presence" (src/ac4dec/
                // ERRATA.md, "The objects oamd_dyndata_multi() lists").
                out.at(*oamd_key).oamd->objects = group_objects;
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
// One audio substream of the presentation decode() decodes, as the walk
// parse() makes of the whole frame read it.
struct CapturedAudio {
    int index = -1;  // its substream_index
    int state_key = 0;
    SubstreamContext context{};
    AudioSubstream content{};
    bool read = false;  // read to its end, with no refusal
    // An object audio substream's objects and its group's OAMD substream
    // (Assignment's).
    std::vector<ObjectEntry> essences_full;
    std::vector<ObjectEntry> essences_core;
    int group_offset = 0;
    std::optional<int> oamd_key;
    std::optional<OamdCommonData> object_common;
};

// A group's OAMD substream as the frame carried it.
struct CapturedOamd {
    int key = -1;
    detail::OamdSubstream content{};
};

// What decode() keeps of the frame: the presentation select_presentation()
// gave, its audio substreams (one per member of its plan, in the plan's
// order) and its presentation substream.
struct Capture {
    const detail::PresentationPlan* plan = nullptr;
    std::vector<CapturedAudio> audio;
    PresentationSubstream presentation{};
    bool presentation_read = false;
    std::vector<CapturedOamd> oamd;

    [[nodiscard]] CapturedAudio* wants(int index) noexcept {
        for (CapturedAudio& a : audio) {
            if (a.index == index) {
                return &a;
            }
        }
        return nullptr;
    }
};

// Mixing values a stream need not send in every frame, which a decoder keeps
// until new ones come or the stream is spliced (Part 1 clause 6.2.16.0),
// field by field: the associated audio's gains on the main audio and its pan,
// and a dialogue substream's maximum gain and pans.
struct AssociatedMixState {
    std::optional<int> scale_main;
    std::optional<int> scale_main_centre;
    std::optional<int> scale_main_front;
    std::optional<int> pan_associated;
};

struct DialogueMixState {
    std::optional<int> dialog_max_gain;  // unset: g_dialog_max is 0 dB
    std::optional<std::array<int, 2>> pan_dialog;
};

// Part 2 Table 70: -0.25 dB a step, 63 silence.
[[nodiscard]] double group_gain(int code) noexcept {
    return code >= 63 ? 0.0 : std::pow(10.0, -0.25 * static_cast<double>(code) / 20.0);
}

// Part 1 clauses 4.3.12.4.4 to 4.3.12.4.8: -0.3 dB a step, 255 silence; 0 dB
// where none has been sent.
[[nodiscard]] double scale_gain(const std::optional<int>& code) noexcept {
    if (!code) {
        return 1.0;
    }
    return *code >= 255 ? 0.0 : std::pow(10.0, -0.3 * static_cast<double>(*code) / 20.0);
}

// Part 1 clause 4.3.12.4.9: 1.5 degrees a step, clockwise from the front.
[[nodiscard]] double pan_degrees(int code) noexcept {
    return 1.5 * static_cast<double>(code);
}

// A listener's gain in dB; below -120 dB, silence.
[[nodiscard]] double listener_gain(double db) noexcept {
    return db < -120.0 ? 0.0 : std::pow(10.0, db / 20.0);
}

// Part 1 Table 92's premix codes: the associated audio was mixed into the
// main audio before encoding, so a listener's g_assoc has nothing to act on
// (clause 4.3.3.8.8).
[[nodiscard]] bool premixed(std::string_view tag) noexcept {
    const auto is = [tag](std::string_view code) {
        return tag.size() == code.size() && std::ranges::equal(tag, code, [](char a, char b) {
                   return std::tolower(static_cast<unsigned char>(a)) == static_cast<unsigned char>(b);
               });
    };
    return is("qax") || is("qtx") || is("qsx") || is("qex");
}

// The member whose dialnorm and DRC a version 0 presentation takes (Part 2
// clause 4.8.5.2 Table 16 and clause 4.8.6): the dialogue substream's in
// configurations 0 and 3, the main one's otherwise.
[[nodiscard]] std::size_t dialnorm_member(const detail::PresentationPlan& plan, std::size_t anchor) noexcept {
    if (plan.presentation_config == 0 || plan.presentation_config == 3) {
        for (std::size_t m = 0; m < plan.members.size(); ++m) {
            if (plan.members[m].role == Role::kDialogue) {
                return m;
            }
        }
    }
    return anchor;
}

}  // namespace

struct Decoder::Impl {
    DecoderConfig config{};
    std::map<int, AudioSubstreamState> audio;
    std::map<int, PresentationSubstreamState> presentation;
    // What a substream group's OAMD substream sent last, keyed by its index:
    // the timing its substreams take where they send none (src/ac4dec/
    // ERRATA.md, "Which oamd_timing_data() applies"), and the common data.
    struct OamdGroupState {
        std::optional<detail::OamdTimingData> timing;
        std::optional<detail::OamdCommonData> common;
    };
    std::map<int, OamdGroupState> oamd;
    // decode()'s reconstruction state, keyed as `audio` is.
    std::map<int, detail::SubstreamPcm> pcm;
    std::optional<int> previous_sequence_counter;
    // Part 2 clause 5.11's phi_t of the last frame decode() read, which a
    // change of source does not forget: the 0 a splicer writes continues it.
    std::optional<int> converter_phase;
    std::string_view refusal;
    // The keys in `pcm` of the substreams decode() last output: the one the
    // others were mixed into, and the others with what they are to the
    // presentation; its rate, and the presentation.
    struct LastMember {
        int key = 0;
        Role role = Role::kMain;
    };
    std::optional<int> last_key;
    std::vector<LastMember> last_members;
    int last_rate = 0;
    std::size_t last_presentation = 0;
    std::optional<int> last_presentation_id;
    // Set by a change of source until a frame decodes.
    bool new_source = false;
    // The plans of the frame's presentations, what decode() keeps of the
    // frame, and the members' matrices for the mix: kept from frame to frame
    // so that a frame allocates none of them once the first has sized them.
    std::vector<detail::PresentationPlan> plans;
    Capture frame_capture;
    std::vector<detail::MixSource> sources;
    std::vector<std::vector<float>> scratch_channels;  // what a QMF-only decode puts out: nothing
    std::vector<Speaker> scratch_speakers;
    // The mixing values in force: by presentation substream in a version 1
    // presentation and by associated audio substream in a version 0 one, and
    // by dialogue substream.
    std::map<int, AssociatedMixState> associated_mix;
    std::map<int, DialogueMixState> dialogue_mix;
    // Object audio: each object's metadata and its updates that wait for
    // their output sample, by substream (its state key) and object; an A-JOC
    // substream's portions' own timings, the last each sent; the object
    // substreams decode() last output; and the output samples so far.
    struct ObjectTrack {
        detail::ObjectMetadataState state;
        ObjectProperties current;  // in force at the next output sample
        std::deque<std::pair<std::int64_t, ObjectUpdate>> pending;
    };
    std::map<std::pair<int, int>, ObjectTrack> object_tracks;
    struct AjocTimings {
        std::optional<detail::OamdTimingData> dmx;
        std::optional<detail::OamdTimingData> umx;
    };
    std::map<int, AjocTimings> ajoc_timings;
    std::vector<int> last_objects;
    std::int64_t output_samples = 0;
    // An object substream's objects, kept from frame to frame, and the
    // frame's length.
    std::vector<std::vector<float>> object_pcm;
    std::size_t object_samples = 0;

    [[nodiscard]] bool keeps(int key) const noexcept {
        return key == last_key ||
               std::ranges::any_of(last_members,
                                   [key](const LastMember& m) { return m.key == key; }) ||
               std::ranges::find(last_objects, key) != last_objects.end();
    }

    // The objects of one object audio member of the presentation, decoded,
    // their metadata applied, into `frame`.
    [[nodiscard]] detail::ParseResult decode_objects(const CapturedAudio& member,
                                                     const detail::FrameInputs& base,
                                                     DecodedFrame& frame);

    // A change of source (Part 1 clause 4.3.3.2.2): what was read from the
    // stream goes, the mixing values with it, and the signal of the
    // substreams that output last carries on, so that their audio comes out
    // to its end and overlaps the new source's first frame
    // (src/ac4dec/ERRATA.md, "A change of source").
    void forget_stream() {
        audio.clear();
        presentation.clear();
        oamd.clear();
        associated_mix.clear();
        dialogue_mix.clear();
        ajoc_timings.clear();
        for (auto& [key, track] : object_tracks) {
            track.state = {};
        }
        std::erase_if(pcm, [this](const auto& entry) { return !keeps(entry.first); });
        new_source = true;
    }

    // Drops the signal too, so that the next frame decoded starts from
    // silence.
    void forget_signal() {
        pcm.clear();
        last_key.reset();
        last_members.clear();
        last_objects.clear();
        object_tracks.clear();
    }

    // This frame's mixing of `plan` (Part 1 clause 6.2.16, Part 2 clauses
    // 4.8.3.17 to 4.8.4), the substream at `anchor` taking the others, from
    // the captured frame and the values in force; `dialnorm` is the one the
    // DRC takes, which a version 0 presentation levels its associated audio
    // to.
    [[nodiscard]] detail::MixValues mix_values(const detail::PresentationPlan& plan, std::size_t anchor,
                                               std::optional<double> dialnorm);

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
    frame.presentation = last_presentation;
    frame.presentation_id = last_presentation_id;
    detail::FrameInputs inputs{.sequence_counter = frame.sequence_counter,
                               .converter_phase = converter_phase.value_or(0),
                               .new_source = false,
                               .output = config.output,
                               .drc = {},
                               .de = {},
                               .downmix = {},
                               .decoding = config.decoding};
    // The presentation's other substreams are concealed the same way, as far
    // as the QMF domain, and mixed in as the last good frame mixed them.
    detail::FrameInputs member_inputs = inputs;
    member_inputs.qmf_only = true;
    sources.clear();
    std::optional<detail::MixSource> dialogue;
    for (const LastMember& member : last_members) {
        const auto it = pcm.find(member.key);
        if (it == pcm.end() || !it->second.can_conceal() ||
            !it->second.conceal(config.concealment, member_inputs, scratch_channels, scratch_speakers)) {
            continue;
        }
        const detail::MixSource out = it->second.qmf_output(member.key);
        if (member.role == Role::kDialogueEnhancement) {
            dialogue = dialogue.value_or(out);
        } else {
            sources.push_back(out);
        }
    }
    inputs.sources = sources;
    inputs.dialogue = dialogue;
    if (!source->conceal(config.concealment, inputs, frame.channels, frame.speakers)) {
        return std::unexpected(error);
    }
    frame.concealed = Concealment{.error = error,
                                  .action = config.concealment == ConcealmentPolicy::kRepeatFade
                                                ? ConcealmentAction::kRepeatFade
                                                : ConcealmentAction::kMute};
    return std::optional<DecodedFrame>{std::move(frame)};
}

detail::ParseResult Decoder::Impl::decode_objects(const CapturedAudio& member,
                                                  const detail::FrameInputs& base,
                                                  DecodedFrame& frame) {
    // The objects' PCM: the substream's element, then A-JOC in full
    // decoding, with the output level gain alone (SubstreamPcm).
    detail::FrameInputs inputs = base;
    inputs.de = {};
    inputs.downmix = {};
    inputs.mix = {};
    inputs.sources = {};
    inputs.dialogue.reset();
    inputs.qmf_only = false;
    inputs.objects = true;
    detail::SubstreamPcm& substream = pcm[member.state_key];
    if (auto ok =
            substream.decode(member.context, member.content, inputs, object_pcm, scratch_speakers);
        !ok) {
        return ok;
    }
    const bool full = config.decoding == DecodingMode::kFull;
    const std::vector<ObjectEntry>& essences = full ? member.essences_full : member.essences_core;
    if (object_pcm.size() != essences.size()) {
        return detail::fail(
            DecodeError::kInvalidStream,
            "an object audio substream whose objects its table of contents does not list");
    }
    const std::size_t length = object_pcm.empty() ? 0 : object_pcm.front().size();
    object_samples = length;
    // Part 2 clause 5.8.2.5: a direct-coded dialogue substream's objects take
    // the dialogue enhancement gain, up to the cap its dialog_max_gain sets
    // (0 dB without one), which is kept as a dialogue substream's is for the
    // mix (Part 1 clause 4.3.12.4.11).
    const detail::ExtendedMetadata& sent = member.content.metadata.extended;
    if (member.context.coding == detail::AudioCoding::kObjects && sent.b_dialog) {
        DialogueMixState& state = dialogue_mix[member.state_key];
        if (sent.dialog_max_gain) {
            state.dialog_max_gain = sent.dialog_max_gain;
        } else if (member.context.b_iframe) {
            state.dialog_max_gain.reset();
        }
        const double max_db =
            state.dialog_max_gain ? 3.0 * static_cast<double>(1 + *state.dialog_max_gain) : 0.0;
        if (const double db = std::min(config.output.dialogue_enhancement_db, max_db); db != 0.0) {
            const auto de_gain = static_cast<float>(std::pow(10.0, db / 20.0));
            for (std::vector<float>& samples : object_pcm) {
                for (float& sample : samples) {
                    sample *= de_gain;
                }
            }
        }
    }

    // The metadata of this frame's objects and its timing (Part 2 Table 7):
    // an A-JOC substream's portion, the upmix's in full decoding and the
    // downmix's in core decoding; a direct-coded substream's in its group's
    // OAMD substream, or in its own metadata() in an alternative
    // presentation. A portion that sends no timing takes the downmix's
    // (b_derive_timing_from_dmx), the group's, or its own last (src/ac4dec/
    // ERRATA.md, "Which oamd_timing_data() applies").
    // The common data: an A-JOC substream's own in the table of contents,
    // else its group's OAMD substream's.
    std::optional<detail::OamdTimingData> group_timing;
    if (member.object_common) {
        frame.object_common = member.object_common;
    }
    if (member.oamd_key) {
        if (const auto group = oamd.find(*member.oamd_key); group != oamd.end()) {
            group_timing = group->second.timing;
            if (group->second.common && !member.object_common) {
                frame.object_common = group->second.common->data;
            }
        }
    }
    const detail::OamdDynData* dyn = nullptr;
    int offset = 0;
    std::optional<detail::OamdTimingData> timing;
    if (member.context.coding == detail::AudioCoding::kAjoc && member.content.ajoc) {
        const detail::AjocSubstream& a = *member.content.ajoc;
        AjocTimings& own = ajoc_timings[member.state_key];
        const std::optional<detail::OamdTimingData> dmx_timing =
            a.dmx_timing ? a.dmx_timing : (group_timing ? group_timing : own.dmx);
        if (a.dmx_timing) {
            own.dmx = a.dmx_timing;
        }
        if (a.umx_timing) {
            own.umx = a.umx_timing;
        }
        if (full) {
            dyn = &a.umx;
            timing = a.umx_timing
                         ? a.umx_timing
                         : (a.b_derive_timing_from_dmx ? dmx_timing
                                                       : (group_timing ? group_timing : own.umx));
        } else if (!member.context.b_static_dmx) {
            dyn = a.dmx ? &*a.dmx : nullptr;
            timing = dmx_timing;
        }
    } else {
        if (member.content.metadata.oamd) {
            dyn = &*member.content.metadata.oamd;
        } else if (member.oamd_key) {
            for (const CapturedOamd& captured : frame_capture.oamd) {
                if (captured.key == *member.oamd_key && captured.content.dyndata) {
                    dyn = &*captured.content.dyndata;
                    offset = member.group_offset;
                }
            }
        }
        timing = group_timing;
    }

    // Each block's update, at its sample in the output: the codec frame's
    // first sample comes out the decoder's delay after this frame's first
    // output sample (clause 5.9.2).
    const std::int64_t start = output_samples;
    const auto origin =
        start + static_cast<std::int64_t>(std::lround(substream.output_delay_samples()));
    const int n = static_cast<int>(essences.size());
    if (dyn != nullptr && timing && dyn->n_blocks > 0 && offset + n <= dyn->n_objs) {
        for (int b = 0; b < dyn->n_blocks; ++b) {
            const detail::BlockTiming when = detail::block_timing(*timing, b);
            std::optional<double> previous_gain;
            for (int k = 0; k < n; ++k) {
                const ObjectEntry& e = essences[static_cast<std::size_t>(k)];
                const bool dynamic = e.kind == ObjectKind::kDyn && !e.lfe;
                ObjectTrack& track = object_tracks[{member.state_key, k}];
                const ObjectProperties p = detail::apply_block(dyn->block(offset + k, b), dynamic,
                                                               previous_gain, track.state);
                previous_gain = p.gain_db;
                track.pending.emplace_back(
                    origin + when.sample,
                    ObjectUpdate{.sample = 0, .ramp_samples = when.ramp, .properties = p});
            }
        }
    }

    // The objects, each with the updates that fall in this frame.
    const auto end = start + static_cast<std::int64_t>(length);
    for (int k = 0; k < n; ++k) {
        const ObjectEntry& e = essences[static_cast<std::size_t>(k)];
        ObjectTrack& track = object_tracks[{member.state_key, k}];
        DecodedObject object;
        object.kind = e.kind;
        object.lfe = e.lfe;
        if (e.speaker) {
            object.speaker = detail::speaker_of_index(*e.speaker);
        }
        object.samples = std::move(object_pcm[static_cast<std::size_t>(k)]);
        object.properties = track.current;
        while (!track.pending.empty() && track.pending.front().first < end) {
            auto [at_sample, update] = track.pending.front();
            track.pending.pop_front();
            update.sample = at_sample < start ? 0 : static_cast<std::size_t>(at_sample - start);
            track.current = update.properties;
            object.updates.push_back(std::move(update));
        }
        frame.objects.push_back(std::move(object));
    }
    return {};
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
    impl_->last_presentation = 0;
    impl_->last_presentation_id.reset();
}

std::expected<FrameReport, DecodeError> Decoder::parse(std::span<const std::byte> raw_ac4_frame) {
    return impl_->read(raw_ac4_frame, nullptr);
}

std::string_view Decoder::refusal_reason() const noexcept {
    return impl_->refusal;
}

detail::MixValues Decoder::Impl::mix_values(const detail::PresentationPlan& plan, std::size_t anchor,
                                            std::optional<double> dialnorm) {
    detail::MixValues mix;
    // Part 2 clause 4.8.4, Table 70: each group's gain, which a version 1
    // presentation of several groups may send, 0 dB where it sends none.
    const auto group = [&](const detail::Member& member) {
        if (!plan.v1 || !member.gain_slot || !frame_capture.presentation_read ||
            !frame_capture.presentation.b_substream_group_gains_present ||
            *member.gain_slot >= frame_capture.presentation.sg_gain.size()) {
            return 1.0;
        }
        return group_gain(frame_capture.presentation.sg_gain[*member.gain_slot]);
    };
    // The associated audio's gains on the main audio and its pan: from the
    // presentation substream in version 1 (Part 2 clause 4.8.3.17), from the
    // associated substream's extended_metadata() in version 0.
    const AssociatedMixState* associated = nullptr;
    for (std::size_t m = 0; m < plan.members.size(); ++m) {
        if (plan.members[m].role != Role::kAssociated) {
            continue;
        }
        const auto keep = [](std::optional<int>& into, const std::optional<int>& sent) {
            if (sent) {
                into = sent;
            }
        };
        if (plan.v1 && plan.presentation_substream) {
            AssociatedMixState& state = associated_mix[*plan.presentation_substream];
            if (frame_capture.presentation_read && frame_capture.presentation.b_associated) {
                const PresentationSubstream& p = frame_capture.presentation;
                keep(state.scale_main, p.scale_main);
                keep(state.scale_main_centre, p.scale_main_centre);
                keep(state.scale_main_front, p.scale_main_front);
                keep(state.pan_associated, p.pan_associated);
            }
            associated = &state;
        } else if (!plan.v1) {
            const detail::ExtendedMetadata& sent = frame_capture.audio[m].content.metadata.extended;
            AssociatedMixState& state = associated_mix[frame_capture.audio[m].state_key];
            keep(state.scale_main, sent.scale_main);
            keep(state.scale_main_centre, sent.scale_main_centre);
            keep(state.scale_main_front, sent.scale_main_front);
            keep(state.pan_associated, sent.pan_associated);
            associated = &state;
        }
        break;
    }
    mix.main_gain = group(plan.members[anchor]);
    if (associated != nullptr) {
        mix.scale_all = scale_gain(associated->scale_main);
        mix.scale_front = scale_gain(associated->scale_main_front);
        mix.scale_centre = scale_gain(associated->scale_main_centre);
    }
    for (std::size_t m = 0; m < plan.members.size() && mix.count < mix.members.size(); ++m) {
        const detail::Member& member = plan.members[m];
        if (m == anchor || (member.role != Role::kDialogue && member.role != Role::kAssociated)) {
            continue;
        }
        const CapturedAudio& captured = frame_capture.audio[m];
        detail::MixMember out;
        out.key = captured.state_key;
        out.gain = group(member);
        if (member.role == Role::kDialogue) {
            // Part 1 clause 4.3.12.4.11: g_dialog_max, which an I-frame that
            // does not send it sets to 0 dB; and the pans (6.2.16.1).
            const detail::ExtendedMetadata& sent = captured.content.metadata.extended;
            DialogueMixState& state = dialogue_mix[captured.state_key];
            if (sent.b_dialog && sent.dialog_max_gain) {
                state.dialog_max_gain = sent.dialog_max_gain;
            } else if (member.iframe) {
                state.dialog_max_gain.reset();
            }
            if (sent.b_dialog && sent.b_pan_dialog_present) {
                state.pan_dialog = sent.pan_dialog;
            }
            const double max_db = state.dialog_max_gain ? 3.0 * static_cast<double>(1 + *state.dialog_max_gain) : 0.0;
            out.gain *= listener_gain(std::min(config.output.dialogue_gain_db, max_db));
            // Part 2 clause 4.8.3.17: with associated audio, the dialogue is
            // scaled as the main audio is.
            if (associated != nullptr) {
                out.scale_all = mix.scale_all;
                out.scale_front = mix.scale_front;
                out.scale_centre = mix.scale_centre;
            }
            // A mono or two-channel dialogue substream takes a pan a channel;
            // a 3.0 one keeps its channels (ERRATA, "The dialogue's gain and pans").
            if (state.pan_dialog && (member.ch_mode == 0 || member.ch_mode == 1)) {
                out.pan[0] = pan_degrees((*state.pan_dialog)[0]);
                if (member.ch_mode == 1) {
                    out.pan[1] = pan_degrees((*state.pan_dialog)[1]);
                }
            }
        } else {
            // Part 1 clause 6.2.16.2: g_assoc, which a premixed service does
            // not take; a mono substream panned by pan_associated, 0 degrees
            // where none has been sent.
            if (!premixed(member.language)) {
                out.gain *= listener_gain(std::min(config.output.associated_gain_db, 0.0));
            }
            if (member.ch_mode == 0) {
                out.pan[0] = pan_degrees(associated != nullptr ? associated->pan_associated.value_or(0) : 0);
            }
            // Clause 6.2.16's levelling in a version 0 presentation, whose
            // associated substream carries a dialnorm of its own (Part 2
            // clause 4.8.5.2): to the dialnorm the DRC takes.
            if (!plan.v1 && dialnorm && captured.content.metadata.basic.dialnorm_bits) {
                const double own = -0.25 * static_cast<double>(*captured.content.metadata.basic.dialnorm_bits);
                out.gain *= std::pow(2.0, (*dialnorm - own) / 6.0);
            }
        }
        mix.members[mix.count++] = out;
    }
    mix.active = mix.count > 0;
    return mix;
}

std::expected<std::optional<DecodedFrame>, DecodeError> Decoder::decode(std::span<const std::byte> raw_ac4_frame) {
    Impl& d = *impl_;
    d.refusal = {};
    auto report = d.read(raw_ac4_frame, &d.frame_capture);
    if (!report) {
        d.refusal = describe(report.error());
        // read() took the frame to be the one the stream expected; its phase
        // follows the last frame's.
        if (d.converter_phase) {
            d.converter_phase = (*d.converter_phase + 1) % 5;
        }
        return d.conceal_or(report.error());
    }
    // Part 2 clause 5.11: phi_t is sequence_counter modulo 5, but where a
    // splicer wrote 0 it goes on from the frame before, and it is 0 for a
    // first frame of 0.
    const int counter = report->sequence_counter;
    const int phase = counter != 0 ? counter % 5 : (d.converter_phase ? (*d.converter_phase + 1) % 5 : 0);
    d.converter_phase = phase;
    const Capture& capture = d.frame_capture;
    if (capture.plan == nullptr) {
        // Where a substream is one this decoder does not decode yet (an
        // immersive element or objects, say), that is why no presentation can
        // be selected, and its reason says so.
        const auto unsupported = std::ranges::find(report->substreams, std::optional{DecodeError::kUnsupported},
                                                   &SubstreamReport::refused);
        d.refusal = unsupported != report->substreams.end() ? unsupported->refused_reason
                                                            : std::string_view{"no presentation this decoder can select"};
        return d.conceal_or(DecodeError::kUnsupported);
    }
    const detail::PresentationPlan& plan = *capture.plan;
    const std::optional<std::size_t> channel_anchor = detail::anchor_member(plan);
    const std::size_t anchor = channel_anchor.value_or(0);
    // The presentation needs every one of its substreams: one refused, or
    // missing, is the frame's failure.
    for (const CapturedAudio& member : capture.audio) {
        const auto it = std::ranges::find(report->substreams, member.index, &SubstreamReport::index);
        if (it != report->substreams.end() && it->refused) {
            d.refusal = it->refused_reason;
            if (*it->refused == DecodeError::kMissingIFrame && d.concealment_source() == nullptr) {
                // Nothing comes out for this frame, so the signal before it
                // is dropped rather than resumed a gap later.
                d.forget_signal();
                return std::optional<DecodedFrame>{};
            }
            return d.conceal_or(*it->refused);
        }
        if (!member.read) {
            d.refusal = "the substream to decode is not in the frame";
            return d.conceal_or(DecodeError::kInvalidStream);
        }
    }
    const CapturedAudio& main = capture.audio[anchor];
    DecodedFrame frame;
    frame.sample_rate_hz = main.context.fs_index == 0 ? 44100 : 48000;
    const auto is_object = [&plan](std::size_t m) {
        return plan.members[m].coding != detail::Coding::kChannel;
    };
    frame.sequence_counter = report->sequence_counter;
    frame.presentation = plan.index;
    frame.presentation_id = plan.presentation_id;
    // Dialnorm and DRC come from the presentation substream where the
    // presentation has one, and otherwise from the metadata() of the
    // substream Part 2 Table 16 names (clauses 4.8.5.2 and 4.8.6).
    detail::FrameInputs inputs{.sequence_counter = report->sequence_counter,
                               .converter_phase = phase,
                               .new_source = d.new_source,
                               .output = d.config.output,
                               .drc = {},
                               .de = {},
                               .downmix = {},
                               .decoding = d.config.decoding};
    std::optional<double> dialnorm;
    const detail::DrcState* drc_state = nullptr;
    const detail::DrcFrame* drc_frame = nullptr;
    if (capture.presentation_read && plan.presentation_substream) {
        dialnorm = -0.25 * static_cast<double>(capture.presentation.dialnorm_bits);
        drc_state = &d.presentation[*plan.presentation_substream].drc;
        drc_frame = &capture.presentation.drc;
    } else {
        const CapturedAudio& levels = capture.audio[dialnorm_member(plan, anchor)];
        const detail::Metadata& metadata = levels.content.metadata;
        if (metadata.basic.dialnorm_bits) {
            dialnorm = -0.25 * static_cast<double>(*metadata.basic.dialnorm_bits);
        }
        if (metadata.drc) {
            drc_state = &d.audio[levels.state_key].metadata.drc;
            drc_frame = &*metadata.drc;
        }
    }
    inputs.drc = detail::drc_frame_values(d.config.output, dialnorm, drc_state, drc_frame);
    // The presentation's object audio substreams, each decoded apart.
    d.last_objects.clear();
    d.object_samples = 0;
    for (std::size_t m = 0; m < capture.audio.size(); ++m) {
        if (!is_object(m)) {
            continue;
        }
        if (const detail::ParseResult decoded = d.decode_objects(capture.audio[m], inputs, frame);
            !decoded) {
            d.refusal = decoded.error().reason;
            return d.conceal_or(decoded.error().error);
        }
        d.last_objects.push_back(capture.audio[m].state_key);
    }
    if (!channel_anchor) {
        // A presentation of object audio alone: no channels.
        d.last_key.reset();
        d.last_members.clear();
        d.last_rate = frame.sample_rate_hz;
        d.last_presentation = plan.index;
        d.last_presentation_id = plan.presentation_id;
        d.new_source = false;
        d.output_samples += static_cast<std::int64_t>(d.object_samples);
        std::erase_if(d.pcm, [&d](const auto& entry) { return !d.keeps(entry.first); });
        return std::optional<DecodedFrame>{std::move(frame)};
    }
    inputs.de = detail::de_frame_values(main.content.metadata.dialog_enhancement);
    inputs.downmix =
        detail::downmix_values(capture.presentation_read ? &capture.presentation : nullptr, main.content.metadata);
    inputs.mix = d.mix_values(plan, anchor, dialnorm);
    // The presentation's other substreams, each as far as the QMF domain,
    // after its own dialogue enhancement; the dialogue enhancement substream
    // is the waveform of the main substream's hybrid method.
    d.sources.clear();
    std::optional<detail::MixSource> dialogue;
    for (std::size_t m = 0; m < capture.audio.size(); ++m) {
        if (m == anchor || is_object(m)) {
            continue;
        }
        const CapturedAudio& member = capture.audio[m];
        detail::FrameInputs member_inputs{.sequence_counter = report->sequence_counter,
                                          .converter_phase = phase,
                                          .new_source = d.new_source,
                                          .output = d.config.output,
                                          .drc = {}};
        member_inputs.de = detail::de_frame_values(member.content.metadata.dialog_enhancement);
        member_inputs.decoding = d.config.decoding;
        member_inputs.qmf_only = true;
        detail::SubstreamPcm& pcm = d.pcm[member.state_key];
        if (const detail::ParseResult decoded =
                pcm.decode(member.context, member.content, member_inputs, d.scratch_channels, d.scratch_speakers);
            !decoded) {
            d.refusal = decoded.error().reason;
            return d.conceal_or(decoded.error().error);
        }
        const detail::MixSource out = pcm.qmf_output(member.state_key);
        if (plan.members[m].role == Role::kDialogueEnhancement) {
            dialogue = dialogue.value_or(out);
        } else {
            d.sources.push_back(out);
        }
    }
    inputs.sources = d.sources;
    inputs.dialogue = dialogue;
    const detail::ParseResult decoded =
        d.pcm[main.state_key].decode(main.context, main.content, inputs, frame.channels, frame.speakers);
    if (!decoded) {
        d.refusal = decoded.error().reason;
        return d.conceal_or(decoded.error().error);
    }
    d.last_key = main.state_key;
    d.last_members.clear();
    for (std::size_t m = 0; m < capture.audio.size(); ++m) {
        if (m != anchor && !is_object(m)) {
            d.last_members.push_back({.key = capture.audio[m].state_key, .role = plan.members[m].role});
        }
    }
    d.output_samples +=
        frame.channels.empty() ? 0 : static_cast<std::int64_t>(frame.channels.front().size());
    d.last_rate = frame.sample_rate_hz;
    d.last_presentation = plan.index;
    d.last_presentation_id = plan.presentation_id;
    d.new_source = false;
    // A substream the presentation no longer takes drops its signal.
    std::erase_if(d.pcm, [&d](const auto& entry) { return !d.keeps(entry.first); });
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
        capture->plan = nullptr;
        capture->audio.clear();
        capture->oamd.clear();
        capture->presentation_read = false;
        if (const std::optional<std::size_t> selected =
                detail::select(toc, config.presentation, config.level, plans)) {
            capture->plan = &plans[*selected];
            for (const detail::Member& member : capture->plan->members) {
                capture->audio.emplace_back().index = member.substream;
            }
        }
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
        if (!state.carries(assignment.audio)) {
            state = AudioSubstreamState{};
            state.carry(assignment.audio);
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
            } else if (CapturedAudio* const captured = capture != nullptr ? capture->wants(index) : nullptr) {
                captured->state_key = assignment.state_key;
                captured->context = assignment.audio;
                captured->content = std::move(parsed);
                captured->read = true;
            }
        }
        report.substreams.push_back(owner_report);
        report.substreams.push_back(ext_report);
    }
    for (const int index : resolved) {
        assignments.erase(index);
    }

    // Two passes: the OAMD substreams first, whose timing the object audio
    // substreams of their groups take (src/ac4dec/ERRATA.md, "Which
    // oamd_timing_data() applies"), then the rest in index order.
    for (const bool oamd_pass : {true, false}) {
        for (auto& [index, assignment] : assignments) {
            if ((assignment.kind == SubstreamReport::Kind::kOamd) != oamd_pass) {
                continue;
            }
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
            BitReader reader(raw_ac4_frame.subspan(located.offset, located.size), index,
                             config.syntax);
            ParseResult result;
            switch (assignment.kind) {
                case SubstreamReport::Kind::kAudio: {
                    // What one substream carries from frame to frame belongs to
                    // its channel mode and substream syntax version; a change of
                    // either starts it afresh. The slot is the series' first
                    // index, which is this substream's own outside a frame-rate-
                    // multiplied series (assign_instances()).
                    AudioSubstreamState& state = audio[assignment.state_key];
                    if (!state.carries(assignment.audio)) {
                        state = AudioSubstreamState{};
                        state.carry(assignment.audio);
                    }
                    // An object audio substream's objects, and the num_obj_info_blocks
                    // its group's OAMD substream sent last, which it takes where it
                    // sends no timing of its own.
                    std::optional<detail::ObjectAudioContext> objects = assignment.objects;
                    if (objects && assignment.oamd_key) {
                        if (const auto group = oamd.find(*assignment.oamd_key);
                            group != oamd.end() && group->second.timing) {
                            objects->group_blocks = group->second.timing->num_obj_info_blocks;
                        }
                    }
                    AudioSubstream parsed;
                    result = detail::parse_audio_substream(reader, assignment.audio, state, parsed,
                                                           nullptr, objects ? &*objects : nullptr);
                    if (CapturedAudio* const captured =
                            result && capture != nullptr ? capture->wants(index) : nullptr) {
                        captured->state_key = assignment.state_key;
                        captured->context = assignment.audio;
                        captured->content = std::move(parsed);
                        captured->read = true;
                        captured->essences_full = assignment.essences_full;
                        captured->essences_core = assignment.essences_core;
                        captured->object_common = assignment.object_common;
                        captured->group_offset = assignment.group_offset;
                        captured->oamd_key = assignment.oamd_key;
                    }
                    break;
                }
                case SubstreamReport::Kind::kOamd: {
                    detail::OamdSubstreamContext ctx = *assignment.oamd;
                    OamdGroupState& group = oamd[*assignment.oamd_key];
                    if (group.timing) {
                        ctx.carried_blocks = group.timing->num_obj_info_blocks;
                    }
                    detail::OamdSubstream parsed;
                    result = detail::parse_oamd_substream(reader, ctx, parsed);
                    if (result) {
                        // What the substream sent holds until it sends another.
                        if (parsed.timing) {
                            group.timing = parsed.timing;
                        }
                        if (parsed.common) {
                            group.common = parsed.common;
                        }
                        if (capture != nullptr) {
                            capture->oamd.push_back({.key = index, .content = std::move(parsed)});
                        }
                    }
                    break;
                }
                case SubstreamReport::Kind::kPresentation: {
                    PresentationSubstream parsed;
                    result = detail::parse_presentation_substream(reader, *assignment.presentation,
                                                                  presentation[index], parsed);
                    if (result && capture != nullptr && capture->plan != nullptr &&
                        capture->plan->presentation_substream == index) {
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
                    result = detail::fail(
                        DecodeError::kUnsupported,
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
    }
    std::ranges::sort(report.substreams, {}, &SubstreamReport::index);
    return report;
}

}  // namespace ac4
