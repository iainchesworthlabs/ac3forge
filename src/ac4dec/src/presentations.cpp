#include "presentations.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>
#include <tuple>

#include "pcm/routing.hpp"

namespace ac4::detail {
namespace {

// Part 2 Table 53: the substream type of each ac4_sgi_specifier() of
// presentation_configs 0 to 4, where there is one.
constexpr std::array<std::array<Role, 3>, 5> kConfigRoles = {{
    {Role::kMusicAndEffects, Role::kDialogue, Role::kMain},
    {Role::kMain, Role::kDialogueEnhancement, Role::kMain},
    {Role::kMain, Role::kAssociated, Role::kMain},
    {Role::kMusicAndEffects, Role::kDialogue, Role::kAssociated},
    {Role::kMain, Role::kDialogueEnhancement, Role::kAssociated},
}};

void language_of(const std::optional<ContentType>& content, std::string& out) {
    out.clear();
    if (content && content->language_tag) {
        for (const std::byte b : *content->language_tag) {
            out.push_back(static_cast<char>(std::to_integer<unsigned char>(b)));
        }
    }
}

// Whether decode() turns a channel-coded substream into PCM: a channel mode
// it renders, at 48 or 44.1 kHz (a 96 or 192 kHz substream's HSF extension is
// refused).
[[nodiscard]] bool decodable_substream(const ChannelSubstreamInfo& chan) noexcept {
    return chan.ch_mode.has_value() && !speakers_of(*chan.ch_mode).empty() && !chan.sf_multiplier.has_value() &&
           chan.substream_index.has_value();
}

// Appends a member to `plan`, reusing the storage a member at that place had.
void add_member(PresentationPlan& plan, std::size_t& count, const ChannelSubstreamInfo& chan, Role role, int group,
                std::optional<std::size_t> gain_slot, const std::optional<ContentType>& content) {
    if (count == plan.members.size()) {
        plan.members.emplace_back();
    }
    Member& m = plan.members[count++];
    m.substream = chan.substream_index.value_or(-1);
    m.role = role;
    m.group = group;
    m.gain_slot = gain_slot;
    m.content_classifier = content ? content->content_classifier : -1;
    language_of(content, m.language);
    m.ch_mode = chan.ch_mode.value_or(-1);
    m.iframe = !chan.b_iframe.empty() && chan.b_iframe.front();
}

// Which sg_gain a group at `position` takes (Part 2 clause 6.2.2.3 reads
// n_substream_groups of them): one per ac4_sgi_specifier() in order, but for
// configurations 1 and 4, whose dialogue enhancement group n_substream_groups
// leaves out, and a single group, which is sent none (src/ac4dec/ERRATA.md,
// "Substream group gains").
[[nodiscard]] std::optional<std::size_t> gain_slot_v1(const PresentationInfoV1& p, std::size_t position) noexcept {
    if (!p.presentation_config) {
        return std::nullopt;
    }
    switch (*p.presentation_config) {
        case 1:
            return std::nullopt;
        case 4:
            return position == 0 ? std::optional<std::size_t>{0}
                                 : (position == 2 ? std::optional<std::size_t>{1} : std::nullopt);
        default:
            return position;
    }
}

void plan_v1(const Toc& toc, std::size_t index, PresentationPlan& plan) {
    const PresentationInfoV1& p = toc.presentations_v1[index];
    plan.index = index;
    plan.v1 = true;
    plan.presentation_version = p.presentation_version;
    plan.presentation_config = p.presentation_config;
    plan.presentation_id = p.presentation_id;
    plan.md_compat = p.md_compat;
    plan.enabled = p.enable_presentation.value_or(true);
    plan.pre_virtualized = p.b_pre_virtualized;
    plan.presentation_substream = p.presentation_substream_index;
    bool decodable = p.frame_rate_fraction == 1 && !p.group_refs.empty();
    std::size_t count = 0;
    for (std::size_t position = 0; position < p.group_refs.size(); ++position) {
        const int group_index = p.group_refs[position];
        if (group_index < 0 || static_cast<std::size_t>(group_index) >= toc.substream_groups.size()) {
            decodable = false;
            continue;
        }
        // A group named twice holds the same substreams both times (ERRATA,
        // "A substream group named twice by one presentation").
        if (std::ranges::find(p.group_refs.begin(), p.group_refs.begin() + static_cast<std::ptrdiff_t>(position),
                              group_index) != p.group_refs.begin() + static_cast<std::ptrdiff_t>(position)) {
            continue;
        }
        const SubstreamGroupInfo& group = toc.substream_groups[static_cast<std::size_t>(group_index)];
        decodable = decodable && group.b_substreams_present && group.b_channel_coded && !group.substreams.empty();
        const Role role = role_v1(p, position, group);
        for (const GroupSubstream& sub : group.substreams) {
            if (sub.kind != GroupSubstream::Kind::kChan || !sub.chan) {
                decodable = false;
                continue;
            }
            decodable = decodable && decodable_substream(*sub.chan) && !sub.hsf_ext_substream_index;
            add_member(plan, count, *sub.chan, role, group_index, gain_slot_v1(p, position), group.content_type);
        }
    }
    plan.members.resize(count);
    plan.decodable = decodable && count > 0;
}

void plan_v0(const Toc& toc, std::size_t index, PresentationPlan& plan) {
    const PresentationInfoV0& p = toc.presentations_v0[index];
    plan.index = index;
    plan.v1 = false;
    plan.presentation_version = p.presentation_version;
    plan.presentation_config = p.presentation_config;
    plan.presentation_id = p.presentation_id;
    plan.md_compat = p.md_compat;
    plan.enabled = true;
    plan.pre_virtualized = p.b_pre_virtualized;
    plan.presentation_substream.reset();
    bool decodable = !p.substreams.empty();
    std::size_t count = 0;
    for (const auto& [name, chan] : p.substreams) {
        decodable = decodable && decodable_substream(chan) && !chan.hsf_ext_substream_index;
        add_member(plan, count, chan, role_v0(name), -1, std::nullopt, chan.content_type);
    }
    plan.members.resize(count);
    plan.decodable = decodable;
}

[[nodiscard]] char lower(char c) noexcept {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

[[nodiscard]] bool same_text(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() && std::ranges::equal(a, b, [](char x, char y) { return lower(x) == lower(y); });
}

// A BCP 47 tag's primary language subtag, the part before the first hyphen.
[[nodiscard]] std::string_view primary_subtag(std::string_view tag) noexcept {
    return tag.substr(0, tag.find('-'));
}

[[nodiscard]] int language_rank(const PresentationPlan& plan, std::string_view wanted) noexcept {
    const std::string_view language = presentation_language(plan);
    if (wanted.empty() || language.empty()) {
        return 0;
    }
    if (same_text(language, wanted)) {
        return 2;
    }
    return same_text(primary_subtag(language), primary_subtag(wanted)) ? 1 : 0;
}

// Part 1 Table 92's codes for each refinement, decoder mix then premix.
[[nodiscard]] bool type_matches(std::string_view tag, AssociatedType type) noexcept {
    const auto either = [tag](std::string_view mix, std::string_view premix) {
        return same_text(tag, mix) || same_text(tag, premix);
    };
    switch (type) {
        case AssociatedType::kAny:
            return true;
        case AssociatedType::kAudioDescription:
            return either("qad", "qax");
        case AssociatedType::kAudioDescriptionSubtitles:
            return either("qas", "qtx");
        case AssociatedType::kSpokenSubtitles:
            return either("qss", "qsx");
        case AssociatedType::kEmergencyInformation:
            return either("qei", "qex");
    }
    return false;
}

[[nodiscard]] bool table_92_code(std::string_view tag) noexcept {
    return std::ranges::any_of(std::array<AssociatedType, 4>{AssociatedType::kAudioDescription,
                                                              AssociatedType::kAudioDescriptionSubtitles,
                                                              AssociatedType::kSpokenSubtitles,
                                                              AssociatedType::kEmergencyInformation},
                               [tag](AssociatedType type) { return type_matches(tag, type); });
}

// The associated service a presentation carries: its associated substream's
// content_classifier and tag, or, for a service mixed into the main audio
// before encoding (Table 92's premix), its main substream's where that
// substream is classified as associated audio (Part 2 Table 54) or carries a
// Table 92 code.
struct Service {
    bool present = false;
    int content_classifier = -1;
    std::string_view tag;
};

[[nodiscard]] Service service_of(const PresentationPlan& plan) noexcept {
    for (const Member& m : plan.members) {
        if (m.role == Role::kAssociated) {
            return {.present = true, .content_classifier = m.content_classifier, .tag = m.language};
        }
    }
    for (const Member& m : plan.members) {
        if (m.role == Role::kMain || m.role == Role::kMusicAndEffects) {
            if ((m.content_classifier >= 0 && role_from_classifier(m.content_classifier) == Role::kAssociated) ||
                table_92_code(m.language)) {
                return {.present = true, .content_classifier = m.content_classifier, .tag = m.language};
            }
            break;
        }
    }
    return {};
}

[[nodiscard]] int associated_rank(const PresentationPlan& plan, const PresentationChoice& choice) noexcept {
    const Service service = service_of(plan);
    if (!choice.associated) {
        return service.present ? 0 : 1;
    }
    return service.present && service.content_classifier == *choice.associated &&
                   type_matches(service.tag, choice.associated_type)
               ? 1
               : 0;
}

}  // namespace

// The language of the presentation (Part 1 clause 4.3.3.8.8's NOTE: its main
// or dialogue substream's, never its associated audio's).
std::string_view presentation_language(const PresentationPlan& plan) noexcept {
    for (const Member& m : plan.members) {
        if (m.role == Role::kDialogue && !m.language.empty()) {
            return m.language;
        }
    }
    for (const Member& m : plan.members) {
        if ((m.role == Role::kMain || m.role == Role::kMusicAndEffects) && !m.language.empty()) {
            return m.language;
        }
    }
    return {};
}

SubstreamRole public_role(Role role) noexcept {
    switch (role) {
        case Role::kMain:
            return SubstreamRole::kMain;
        case Role::kMusicAndEffects:
            return SubstreamRole::kMusicAndEffects;
        case Role::kDialogue:
            return SubstreamRole::kDialogue;
        case Role::kDialogueEnhancement:
            return SubstreamRole::kDialogueEnhancement;
        case Role::kAssociated:
            return SubstreamRole::kAssociated;
    }
    return SubstreamRole::kMain;
}

void PresentationName::add(std::span<const std::uint8_t> chunk) {
    const auto append = [](std::string& out, std::span<const std::uint8_t> bytes) {
        for (const std::uint8_t b : bytes) {
            out.push_back(static_cast<char>(b));
        }
    };
    // The name ends at its first zero byte: a fixed 32-byte field pads with
    // them.
    const auto take = [this](const std::string& text) { name_.assign(text, 0, text.find('\0')); };
    const std::size_t n = chunk.size();
    if (n == 0) {
        none();
        return;
    }
    // byte[name_len - 1] = 0: the whole name, byte[0] to byte[name_len - 2].
    if (chunk[n - 1] == 0) {
        pending_.clear();
        append(pending_, chunk.first(n - 1));
        take(pending_);
        none();
        return;
    }
    // byte[name_len - 2] = 0: the last chunk, byte[name_len - 1] the number
    // of chunks. The name is whole when the chunks gathered in the frames
    // before it are that many less one; a decoder that joined in the middle
    // waits for the next repetition.
    if (n >= 2 && chunk[n - 2] == 0) {
        const int total = chunk[n - 1];
        append(pending_, chunk.first(n - 2));
        if (++chunks_ == total) {
            take(pending_);
        }
        none();
        return;
    }
    // A chunk before the last: all name_len bytes are the name's. No count
    // reaches past 255 chunks.
    if (chunks_ == 255) {
        none();
    }
    append(pending_, chunk);
    ++chunks_;
}

void PresentationName::none() noexcept {
    pending_.clear();
    chunks_ = 0;
}

void PresentationName::clear() noexcept {
    name_.clear();
    none();
}

Role role_from_classifier(int content_classifier) noexcept {
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

Role role_v1(const PresentationInfoV1& presentation, std::size_t position, const SubstreamGroupInfo& group) noexcept {
    if (!presentation.presentation_config) {
        return Role::kMain;  // 6.3.2.2.1: a single substream group is Main
    }
    const int config = *presentation.presentation_config;
    if (config >= 0 && config <= 4 && position < 3) {
        return kConfigRoles[static_cast<std::size_t>(config)][position];
    }
    if (config == 5 && group.content_type) {
        return role_from_classifier(group.content_type->content_classifier);
    }
    return Role::kMain;
}

Role role_v0(std::string_view name) noexcept {
    if (name == "M+E") {
        return Role::kMusicAndEffects;
    }
    if (name == "Dialog") {
        return Role::kDialogue;
    }
    if (name == "DE") {
        return Role::kDialogueEnhancement;
    }
    if (name == "Associate") {
        return Role::kAssociated;
    }
    return Role::kMain;
}

bool plan_presentation(const Toc& toc, std::size_t index, PresentationPlan& plan) {
    if (toc.bitstream_version >= 2) {
        if (index >= toc.presentations_v1.size()) {
            return false;
        }
        plan_v1(toc, index, plan);
        return true;
    }
    if (index >= toc.presentations_v0.size()) {
        return false;
    }
    plan_v0(toc, index, plan);
    return true;
}

bool selectable(const PresentationPlan& plan, int level) noexcept {
    // Part 2 clause 6.3.2.3.1: presentation versions 0, 1 and 2 are decoded,
    // the rest skipped. Configurations 0 to 5 carry audio; 6 is EMDF alone and
    // the rest reserved or read as bytes.
    if (!plan.decodable || !plan.enabled || plan.presentation_version < 0 || plan.presentation_version > 2 ||
        (plan.presentation_config && (*plan.presentation_config < 0 || *plan.presentation_config > 5))) {
        return false;
    }
    // Part 1 Table 86 defines md_compat 0 to 4 and Part 2 Table 55 0 to 3,
    // each with 7 unrestricted; a reserved value has no meaning to be within.
    const int reserved_from = plan.v1 ? 4 : 5;
    if (!plan.md_compat || (*plan.md_compat >= reserved_from && *plan.md_compat < 7)) {
        return false;
    }
    return *plan.md_compat <= level;
}

std::optional<std::size_t> anchor_member(const PresentationPlan& plan) noexcept {
    for (std::size_t m = 0; m < plan.members.size(); ++m) {
        if (plan.members[m].role == Role::kMain || plan.members[m].role == Role::kMusicAndEffects) {
            return m;
        }
    }
    if (plan.members.empty()) {
        return std::nullopt;
    }
    return 0;
}

std::optional<std::size_t> select(const Toc& toc, const PresentationChoice& choice, int level,
                                  std::vector<PresentationPlan>& plans) {
    const std::size_t count = toc.bitstream_version >= 2 ? toc.presentations_v1.size() : toc.presentations_v0.size();
    if (plans.size() < count) {
        plans.resize(count);
    }
    std::optional<std::size_t> by_id;
    std::optional<std::size_t> by_index;
    std::optional<std::size_t> best;
    // Part 2 clause 4.8.2's order: language, then associated audio, then the
    // kind of audio; the first in the table of contents among equals.
    const auto rank = [&choice](const PresentationPlan& plan) {
        return std::tuple{language_rank(plan, choice.language), associated_rank(plan, choice),
                          plan.pre_virtualized == choice.headphones ? 1 : 0};
    };
    for (std::size_t i = 0; i < count; ++i) {
        PresentationPlan& plan = plans[i];
        if (!plan_presentation(toc, i, plan) || !selectable(plan, level)) {
            continue;
        }
        if (!by_id && choice.presentation_id && plan.presentation_id == choice.presentation_id) {
            by_id = i;
        }
        if (choice.index && *choice.index == i) {
            by_index = i;
        }
        if (!best || rank(plan) > rank(plans[*best])) {
            best = i;
        }
    }
    return by_id ? by_id : (by_index ? by_index : best);
}

}  // namespace ac4::detail

namespace ac4 {

std::optional<std::size_t> select_presentation(const Toc& toc, const PresentationChoice& choice, int level) {
    std::vector<detail::PresentationPlan> plans;
    return detail::select(toc, choice, level, plans);
}

}  // namespace ac4
