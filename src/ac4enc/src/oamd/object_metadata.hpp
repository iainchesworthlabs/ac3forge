#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "iclforge/ac4enc/encoder.hpp"
#include "oamd/oamd_syntax.hpp"

// Object audio metadata from the encoder's side: ObjectProperties, the terms
// of ETSI TS 103 190-2 V1.3.1 Annex F, turned into the codes of clause 6.2.8
// (the inverse of the decoder's clause 6.3.9 tables), each object's changes
// kept in time, and each frame's object_info_block()s built from them.
//
// Timing (src/ac4enc/ERRATA.md, "When an object's metadata changes"): an
// update at sample s of the encoder's delayed signal lands in the frame that
// codes s, at block_offset_factor (s - the frame's first sample) / 32 with
// oa_sample_offset 0, where the decoder places it at the sample the input
// sample comes out at.

namespace iclforge::ac4::detail {

// Why `p` cannot be sent, or an empty view where it can: a value off the range
// encoder.hpp gives it, or a depth exponent other than 1 with a screen factor
// of 0. The two are one group of fields (Part 2 clause 6.2.8.7's
// group_other_mask 0b0010), and the factor has no code for 0 (src/ac4enc/
// ERRATA.md, "The screen factor and the depth exponent").
[[nodiscard]] std::string_view properties_refusal(const ObjectProperties& p) noexcept;

// Whether `p` can be sent.
[[nodiscard]] inline bool properties_valid(const ObjectProperties& p) noexcept {
    return properties_refusal(p).empty();
}

// What one object_info_block() sends of an object, in full: whether it is
// active, its object_basic_info(), and for a dynamic object its
// object_render_info()'s three groups and add_per_object_md()'s data.
struct ObjectCodes {
    bool active = true;
    ObjectBasicFields basic{};
    std::array<int, 4> position{31, 0, 1, 0};  // pos3D_X, _Y, _Z_sign, _Z
    bool zone_defaults = true;
    int zone_flags = 0;
    int zone_mask = 0;
    // The other properties' group: ObjectRenderFields' other_defaults to
    // div_code, the rest unused.
    ObjectRenderFields other{};
    std::optional<AddPerObjectFields> add{};

    // The position's standard precision as the decoder keeps it, Z signed.
    [[nodiscard]] std::array<int, 3> standard() const noexcept {
        return {position[0], position[1], (position[2] != 0 ? 1 : -1) * position[3]};
    }
    [[nodiscard]] bool same_other(const ObjectCodes& o) const noexcept;
};

[[nodiscard]] ObjectCodes object_codes(const ObjectProperties& p, bool dynamic) noexcept;

// The timing block at `offset_factor` whose ramp is `ramp_samples`: Table 94's
// codes where one holds it, Table 95's where one does, and ramp_duration
// otherwise, at most 2 047.
[[nodiscard]] OamdTimingFields::Block timing_block(int offset_factor, int ramp_samples) noexcept;

// Each object's metadata over the signal's axis: what it starts with and its
// updates in order, the ones before the frames still to code dropped.
class ObjectTimeline {
   public:
    explicit ObjectTimeline(std::vector<ObjectProperties> initial);

    [[nodiscard]] std::size_t objects() const noexcept { return tracks_.size(); }

    // An update from signal sample `sample` on, after any at the same sample.
    void add(int object, std::int64_t sample, int ramp_samples, const ObjectProperties& p);

    // What is in force at `sample`: the last update at or before it.
    [[nodiscard]] const ObjectProperties& at(int object, std::int64_t sample) const;
    // The position at `sample`, moving linearly over each update's ramp.
    [[nodiscard]] std::array<double, 3> position(int object, std::int64_t sample) const;

    struct Update {
        std::int64_t sample = 0;
        int ramp = 0;
        ObjectProperties properties{};
        int object = 0;
    };
    // Each update of any object in [from, to), in order of sample.
    [[nodiscard]] std::vector<Update> between(std::int64_t from, std::int64_t to) const;

    // Forgets the updates that the one in force at `sample` has replaced.
    void drop_before(std::int64_t sample);

   private:
    // Per object, the updates in order, the first in force from the start.
    std::vector<std::deque<Update>> tracks_;
};

// One block a frame's portion sends: where it starts, its ramp, and each of
// the portion's objects' properties from then.
struct BlockPlan {
    int offset_factor = 0;
    int ramp = 0;
    std::vector<ObjectProperties> properties{};
};

// A frame's metadata of one portion: its timing, n_blocks blocks per object
// object by object, and what the decoder holds after them.
struct PortionFrame {
    OamdTimingFields timing{};
    int n_blocks = 0;
    std::vector<ObjectInfoBlockFields> blocks{};
    std::vector<ObjectCodes> held{};
    std::vector<std::array<int, 3>> standard{};
};

// The blocks of one OAMD portion: an A-JOC substream's downmix or upmix, or
// an OAMD substream's direct-coded objects, with what the decoder holds of
// each object from the blocks sent before.
class PortionWriter {
   public:
    explicit PortionWriter(std::vector<bool> dynamic);

    [[nodiscard]] std::size_t objects() const noexcept { return dynamic_.size(); }
    [[nodiscard]] bool dynamic(std::size_t object) const noexcept { return dynamic_[object]; }

    // The frame's blocks from `plans` (at most 7, the first at 0 in an
    // I-frame, which sends everything), each object's against what the block
    // before left: its basic information and render groups reused where
    // they are the same, its position as a difference where one holds it.
    [[nodiscard]] PortionFrame frame(std::span<const BlockPlan> plans, bool iframe) const;

    // What a frame sends whose bits hold no more: in an I-frame one block at
    // 0 of each object's activity, gain, priority and position, which bound
    // its size; elsewhere no block.
    [[nodiscard]] PortionFrame least(const BlockPlan& now, bool iframe) const;

    // The most bits least() takes, whatever the properties.
    [[nodiscard]] std::size_t least_bits_bound(bool iframe) const;

    void commit(const PortionFrame& sent);

   private:
    std::vector<bool> dynamic_;
    std::vector<ObjectCodes> held_;
    std::vector<std::array<int, 3>> standard_;
    bool have_ = false;
};

// The blocks a frame of `frame_length` samples from signal sample `start`
// sends for `objects` of `timeline` (their indices there, in the portion's
// order): one at each 32-sample step an update of any of them starts in, the
// latest 7 (in an I-frame one at 0 and the latest 6), each object's
// properties those in force at the step's end.
[[nodiscard]] std::vector<BlockPlan> plan_blocks(const ObjectTimeline& timeline,
                                                 std::span<const int> objects,
                                                 std::int64_t start, int frame_length,
                                                 bool iframe);

}  // namespace iclforge::ac4::detail
