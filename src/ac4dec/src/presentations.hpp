#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ac4/ac4.hpp"
#include "ac4dec/decoder.hpp"

// The presentations of a table of contents as decode() takes them (ETSI TS 103
// 190-2 V1.3.1 clause 4.8.2): the substreams each is made of and what each
// substream is to it (Part 2 clause 4.8.3.2: main, music and effects,
// dialogue, dialogue enhancement or associated audio), which of them this
// decoder can decode, and the one it selects for a PresentationChoice.

namespace ac4::detail {

// Part 2 clause 4.8.3.2's substream types, with the dialogue enhancement
// substream of Tables 53 and 85 beside them.
enum class Role : std::uint8_t { kMain, kMusicAndEffects, kDialogue, kDialogueEnhancement, kAssociated };

// Part 2 Table 54: the substream type of a presentation_config 5 group, or
// of a presentation_version 0 substream, by its content_classifier.
[[nodiscard]] Role role_from_classifier(int content_classifier) noexcept;

// Part 2 Table 53: the substream type of the group at `position` of a version
// 1 presentation's ac4_sgi_specifier()s.
[[nodiscard]] Role role_v1(const PresentationInfoV1& presentation, std::size_t position,
                           const SubstreamGroupInfo& group) noexcept;

// Part 1 Table 85, by the role the inspector names a version 0 substream
// with ("M+E", "Dialog", "DE", "Associate", "Main" or "main").
[[nodiscard]] Role role_v0(std::string_view name) noexcept;

// How a member codes its audio (Part 2 Table 50): channel-coded, an A-JOC
// substream, or a direct-coded object substream.
enum class Coding : std::uint8_t { kChannel, kAjoc, kObjects };

// One substream of a presentation, in the order the presentation lists them:
// its ac4_sgi_specifier()s and then each group's substreams, or Table 85's
// order.
struct Member {
    int substream = 0;  // substream_index: the first of a frame-rate-multiplied series
    Role role = Role::kMain;
    int group = -1;      // the substream group (version 1)
    // Which of the presentation substream's sg_gain values the group takes;
    // none for a group that takes none (version 0, a single group, the
    // dialogue enhancement group).
    std::optional<std::size_t> gain_slot;
    int content_classifier = -1;  // Part 1 Table 91; -1 without content_type()
    std::string language;         // language_tag_bytes; empty without them
    int ch_mode = -1;             // -1 for a reserved channel mode, and for object audio
    bool iframe = false;          // b_iframe or b_audio_ndot of the substream (the first instance)
    Coding coding = Coding::kChannel;
};

// What decode() needs of a presentation.
struct PresentationPlan {
    std::size_t index = 0;  // in Toc::presentations_v1, or presentations_v0 below bitstream_version 2
    bool v1 = true;         // ac4_presentation_v1_info()
    int presentation_version = 0;
    std::optional<int> presentation_config;
    std::optional<int> presentation_id;
    std::optional<int> md_compat;
    bool enabled = true;
    bool pre_virtualized = false;
    std::optional<int> presentation_substream;  // version 1
    std::vector<Member> members;
    // Whether every member is a substream in this stream that decode() turns
    // into PCM: channel-coded in a channel mode it renders, A-JOC coded, or
    // direct-coded objects in an element of 1, 2, 3 or 5 or with their LFE
    // alone; at 48 or 44.1 kHz.
    bool decodable = false;
};

// Fills `plan` with presentation `index` of `toc`, reusing its storage; false
// where there is no such presentation.
bool plan_presentation(const Toc& toc, std::size_t index, PresentationPlan& plan);

// Part 2 clause 4.8.2 and the readings of src/ac4dec/ERRATA.md ("Selecting a
// presentation"): whether a decoder of compatibility level `level` may select
// the presentation - one it can decode, of a presentation_version it decodes,
// carrying audio, within its level and enabled.
[[nodiscard]] bool selectable(const PresentationPlan& plan, int level) noexcept;

// select_presentation(), with `plans` holding every presentation's plan
// afterwards (by index), their storage kept from call to call.
[[nodiscard]] std::optional<std::size_t> select(const Toc& toc, const PresentationChoice& choice, int level,
                                                std::vector<PresentationPlan>& plans);

// The member decode() renders the other channel-coded members into: the first
// channel-coded main or music and effects substream, else the first
// channel-coded member; nothing for a plan without one. Object audio members
// are decoded apart (DecodedFrame::objects).
[[nodiscard]] std::optional<std::size_t> anchor_member(const PresentationPlan& plan) noexcept;

// The language selection compares (Part 1 clause 4.3.3.8.8's NOTE): the first
// dialogue substream's tag, else the first main or music and effects
// substream's; empty for none.
[[nodiscard]] std::string_view presentation_language(const PresentationPlan& plan) noexcept;

// A member's role as the public API names it.
[[nodiscard]] SubstreamRole public_role(Role role) noexcept;

// A presentation's name as ac4_presentation_substream() sends it (Part 2 clause
// 6.3.3.1.4): whole in one frame, or in chunks, one a frame, the last of which
// says how many there were (src/ac4dec/ERRATA.md, "A presentation name in
// chunks").
class PresentationName {
   public:
    // One frame's presentation_name, name_len bytes of it.
    void add(std::span<const std::uint8_t> chunk);
    // A frame of the substream that carries no name: the chunks gathered so
    // far are not consecutive with the next.
    void none() noexcept;
    void clear() noexcept;
    // The last name received whole, UTF-8; empty before one.
    [[nodiscard]] const std::string& name() const noexcept { return name_; }

   private:
    std::string name_;
    std::string pending_;  // the chunks since the last whole name
    int chunks_ = 0;
};

}  // namespace ac4::detail
