#include "oamd/object_metadata.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace iclforge::ac4::detail {
namespace {

[[nodiscard]] std::size_t at(int index) noexcept {
    return static_cast<std::size_t>(index);
}

// Table 107: the Y position's exponent.
constexpr std::array<double, 4> kDepthExponent = {0.25, 0.5, 1.0, 2.0};

// Table 108.
constexpr std::array<double, 16> kDistance = {1.1, 1.3, 1.6,  2.0,  2.5,  3.2,  4.0,  5.0,
                                              6.3, 7.9, 10.0, 12.6, 15.8, 20.0, 25.1, 50.1};

// Table 111; code 0 is reserved and code 1 is 0.
constexpr std::array<double, 64> kDivergenceCode = {
    0.0,      0.0,      0.004026, 0.00716,  0.012731, 0.020173, 0.028485, 0.04021,
    0.050582, 0.063601, 0.079914, 0.100299, 0.125666, 0.140532, 0.157027, 0.175282,
    0.195417, 0.217536, 0.241718, 0.268002, 0.296377, 0.326766, 0.359017, 0.392895,
    0.428081, 0.464184, 0.500755, 0.537316, 0.573389, 0.608529, 0.642346, 0.674524,
    0.704833, 0.733123, 0.75932,  0.783416, 0.805451, 0.825506, 0.843686, 0.860112,
    0.874914, 0.888222, 0.900168, 0.910875, 0.920461, 0.929035, 0.936698, 0.943544,
    0.949656, 0.955112, 0.95998,  0.964322, 0.968195, 0.974729, 0.979923, 0.98405,
    0.98733,  0.989935, 0.992874, 0.994955, 0.996817, 0.99821,  0.998993, 1.0};

// Table 95: ramp_duration_table's durations in samples.
constexpr std::array<int, 16> kRampTable = {32,   64,   128,  256,  320,  480,  1000, 1001,
                                            1024, 1600, 1601, 1602, 1920, 2000, 2002, 2048};

constexpr int kMaxBlocks = 7;
constexpr int kBlockSamples = 32;

[[nodiscard]] bool in(double v, double lo, double hi) noexcept {
    return std::isfinite(v) && v >= lo && v <= hi;
}

[[nodiscard]] int code_of(double v, double scale, int hi) noexcept {
    return std::clamp(static_cast<int>(std::lround(v * scale)), 0, hi);
}

template <std::size_t N>
[[nodiscard]] int nearest(const std::array<double, N>& table, double v, std::size_t first) noexcept {
    std::size_t best = first;
    for (std::size_t i = first; i < N; ++i) {
        if (std::abs(table[i] - v) < std::abs(table[best] - v)) {
            best = i;
        }
    }
    return static_cast<int>(best);
}

[[nodiscard]] bool same_basic(const ObjectBasicFields& a, const ObjectBasicFields& b) noexcept {
    return a.defaults == b.defaults && a.info_md == b.info_md && a.gain_code == b.gain_code &&
           a.gain_value == b.gain_value && a.priority == b.priority;
}

[[nodiscard]] bool same_zone(const ObjectCodes& a, const ObjectCodes& b) noexcept {
    return a.zone_defaults == b.zone_defaults && a.zone_flags == b.zone_flags &&
           a.zone_mask == b.zone_mask;
}

// The render fields a block sends of `codes`.
void fill_render(const ObjectCodes& codes, ObjectRenderFields& r) {
    const bool otherprops = r.otherprops;
    const bool zone = r.zone;
    const bool position = r.position;
    const bool diff = r.diff_pos;
    const std::array<int, 3> d = r.diff;
    r = codes.other;
    r.otherprops = otherprops;
    r.zone = zone;
    r.position = position;
    r.diff_pos = diff;
    r.diff = d;
    r.x = codes.position[0];
    r.y = codes.position[1];
    r.z_sign = codes.position[2];
    r.z = codes.position[3];
    r.zone_defaults = codes.zone_defaults;
    r.zone_flags = codes.zone_flags;
    r.zone_mask = codes.zone_mask;
}

}  // namespace

std::string_view properties_refusal(const ObjectProperties& p) noexcept {
    const bool gain = (std::isinf(p.gain_db) && p.gain_db < 0.0) || in(p.gain_db, -49.0, 15.0);
    const bool depth = std::ranges::find(kDepthExponent, p.depth_exponent) != kDepthExponent.end();
    const bool distance =
        !p.distance || (std::isinf(*p.distance) && *p.distance > 0.0) || in(*p.distance, 1.0, 1e9);
    const bool headphone =
        !p.headphone_render_mode || (*p.headphone_render_mode >= 0 && *p.headphone_render_mode <= 3);
    const bool in_range = gain && in(p.priority, 0.0, 1.0) && in(p.position[0], 0.0, 1.0) &&
                          in(p.position[1], 0.0, 1.0) && in(p.position[2], -1.0, 1.0) &&
                          p.zone_mask >= 0 && p.zone_mask <= 7 && in(p.width[0], 0.0, 1.0) &&
                          in(p.width[1], 0.0, 1.0) && in(p.width[2], 0.0, 1.0) &&
                          in(p.screen_factor, 0.0, 1.0) && depth && distance &&
                          in(p.divergence, 0.0, 1.0) && headphone;
    if (!in_range) {
        return "an object's properties off the ranges ObjectProperties gives them";
    }
    // object_codes() sends the screen factor and the depth exponent as one group,
    // where the factor is above 0 or the exponent is not 1, and the factor's code,
    // (code + 1) / 8, has no value of 0: the decoder would read the factor as 1/8.
    if (p.depth_exponent != 1.0 && p.screen_factor == 0.0) {
        return "an object with a depth exponent other than 1 and a screen factor of 0, which the "
               "group of fields that sends both has no code for";
    }
    return {};
}

bool ObjectCodes::same_other(const ObjectCodes& o) const noexcept {
    const ObjectRenderFields& a = other;
    const ObjectRenderFields& b = o.other;
    return a.other_defaults == b.other_defaults && a.other_mask == b.other_mask &&
           a.width_mode == b.width_mode && a.width == b.width && a.width_xyz == b.width_xyz &&
           a.screen_factor == b.screen_factor && a.depth_factor == b.depth_factor &&
           a.at_infinity == b.at_infinity && a.distance == b.distance && a.div_mode == b.div_mode &&
           a.div_code == b.div_code;
}

ObjectCodes object_codes(const ObjectProperties& p, bool dynamic) noexcept {
    ObjectCodes c;
    c.active = p.active;

    // Table 102's gains, +15 to +1 dB as 0 to 14 and -1 to -49 dB as 15 to 63;
    // 0 dB and a priority of 1 are what b_default_basic_info_md sends.
    const bool silent = std::isinf(p.gain_db) && p.gain_db < 0.0;
    const int gain = silent ? 0 : std::clamp(static_cast<int>(std::lround(p.gain_db)), -49, 15);
    const bool sends_gain = silent || gain != 0;
    const int priority = code_of(p.priority, 31.0, 31);
    ObjectBasicFields& b = c.basic;
    b.defaults = !sends_gain && priority == 31;
    b.info_md = sends_gain ? (priority == 31 ? 0b0 : 0b10) : 0b11;
    b.gain_code = silent ? 0b10 : 0b0;
    b.gain_value = gain > 0 ? 15 - gain : 14 - gain;
    b.priority = priority;

    if (p.trim_disabled || p.headphone_render_mode) {
        AddPerObjectFields add;
        add.trim_disable = p.trim_disabled;
        if (p.headphone_render_mode) {
            add.headphone = std::pair{*p.headphone_render_mode, p.head_track_disabled};
        }
        c.add = add;
    }
    if (!dynamic) {
        return c;
    }

    const int z = code_of(std::abs(p.position[2]), 15.0, 15);
    c.position = {code_of(p.position[0], 62.0, 62), code_of(p.position[1], 62.0, 62),
                  p.position[2] >= 0.0 || z == 0 ? 1 : 0, z};

    c.zone_defaults = p.zone_mask == 0 && p.enable_elevation && !p.snap;
    c.zone_flags = (p.zone_mask != 0 ? 0b100 : 0) | (!p.enable_elevation ? 0b010 : 0) |
                   (p.snap ? 0b001 : 0);
    c.zone_mask = p.zone_mask;

    ObjectRenderFields& r = c.other;
    r.other_mask = 0;
    const std::array<int, 3> width = {code_of(p.width[0], 31.0, 31), code_of(p.width[1], 31.0, 31),
                                      code_of(p.width[2], 31.0, 31)};
    if (width != std::array<int, 3>{}) {
        r.other_mask |= 0b0001;
        if (width[0] == width[1] && width[1] == width[2]) {
            r.width_mode = 0;
            r.width = width[0];
        } else {
            r.width_mode = 1;
            r.width_xyz = width;
        }
    }
    if (p.screen_factor > 0.0 || p.depth_exponent != 1.0) {
        r.other_mask |= 0b0010;
        r.screen_factor = std::clamp(static_cast<int>(std::lround(p.screen_factor * 8.0)) - 1, 0, 7);
        r.depth_factor = nearest(kDepthExponent, p.depth_exponent, 0);
    }
    if (p.distance) {
        r.other_mask |= 0b0100;
        r.at_infinity = std::isinf(*p.distance);
        r.distance = r.at_infinity ? 0 : nearest(kDistance, *p.distance, 0);
    }
    if (p.divergence > 0.0) {
        r.other_mask |= 0b1000;
        r.div_mode = 0b10;
        r.div_code = nearest(kDivergenceCode, p.divergence, 1);
    }
    r.other_defaults = r.other_mask == 0;
    return c;
}

OamdTimingFields::Block timing_block(int offset_factor, int ramp_samples) noexcept {
    OamdTimingFields::Block b;
    b.offset_factor = offset_factor;
    if (ramp_samples <= 0) {
        b.ramp_code = 0b00;
    } else if (ramp_samples == 512) {
        b.ramp_code = 0b01;
    } else if (ramp_samples == 1536) {
        b.ramp_code = 0b10;
    } else {
        b.ramp_code = 0b11;
        const auto listed = std::ranges::find(kRampTable, ramp_samples);
        b.use_table = listed != kRampTable.end();
        b.ramp_table = b.use_table ? static_cast<int>(listed - kRampTable.begin()) : 0;
        b.ramp = std::min(ramp_samples, 2047);
    }
    return b;
}

// --- ObjectTimeline ----------------------------------------------------------

ObjectTimeline::ObjectTimeline(std::vector<ObjectProperties> initial) {
    for (std::size_t o = 0; o < initial.size(); ++o) {
        std::deque<Update>& track = tracks_.emplace_back();
        track.push_back({std::numeric_limits<std::int64_t>::min(), 0, std::move(initial[o]),
                         static_cast<int>(o)});
    }
}

void ObjectTimeline::add(int object, std::int64_t sample, int ramp_samples,
                         const ObjectProperties& p) {
    std::deque<Update>& track = tracks_[detail::at(object)];
    const auto after = std::ranges::upper_bound(track, sample, {}, &Update::sample);
    track.insert(after, Update{sample, ramp_samples, p, object});
}

const ObjectProperties& ObjectTimeline::at(int object, std::int64_t sample) const {
    const std::deque<Update>& track = tracks_[detail::at(object)];
    const auto after = std::ranges::upper_bound(track, sample, {}, &Update::sample);
    return std::prev(after)->properties;
}

std::array<double, 3> ObjectTimeline::position(int object, std::int64_t sample) const {
    const std::deque<Update>& track = tracks_[detail::at(object)];
    const auto after = std::ranges::upper_bound(track, sample, {}, &Update::sample);
    const auto now = std::prev(after);
    if (now == track.begin() || now->ramp <= 0 || sample >= now->sample + now->ramp) {
        return now->properties.position;
    }
    const std::array<double, 3>& from = std::prev(now)->properties.position;
    const double t = static_cast<double>(sample - now->sample) / now->ramp;
    std::array<double, 3> out{};
    for (std::size_t k = 0; k < out.size(); ++k) {
        out[k] = from[k] + t * (now->properties.position[k] - from[k]);
    }
    return out;
}

std::vector<ObjectTimeline::Update> ObjectTimeline::between(std::int64_t from,
                                                            std::int64_t to) const {
    std::vector<Update> out;
    for (const std::deque<Update>& track : tracks_) {
        for (const Update& u : track) {
            if (u.sample >= from && u.sample < to) {
                out.push_back(u);
            }
        }
    }
    std::ranges::stable_sort(out, {}, &Update::sample);
    return out;
}

void ObjectTimeline::drop_before(std::int64_t sample) {
    // The update before the one in force stays, where a ramp starts from.
    for (std::deque<Update>& track : tracks_) {
        while (track.size() >= 3 && track[2].sample <= sample) {
            track.pop_front();
        }
    }
}

std::vector<BlockPlan> plan_blocks(const ObjectTimeline& timeline, std::span<const int> objects,
                                   std::int64_t start, int frame_length, bool iframe) {
    // Each 32-sample step an update starts in, with the first update's ramp.
    std::map<int, int> steps;
    for (const ObjectTimeline::Update& u : timeline.between(start, start + frame_length)) {
        if (std::ranges::find(objects, u.object) == objects.end()) {
            continue;
        }
        steps.try_emplace(static_cast<int>((u.sample - start) / kBlockSamples), u.ramp);
    }
    if (iframe) {
        steps.try_emplace(0, 0);
    }
    std::vector<std::pair<int, int>> kept(steps.begin(), steps.end());
    // The latest kMaxBlocks, and in an I-frame the one at 0 before them: a
    // later block carries what an earlier one dropped would have.
    const std::size_t keep_from = iframe ? 1 : 0;
    while (kept.size() > static_cast<std::size_t>(kMaxBlocks)) {
        kept.erase(kept.begin() + static_cast<std::ptrdiff_t>(keep_from));
    }
    std::vector<BlockPlan> out;
    for (const auto& [step, ramp] : kept) {
        BlockPlan plan;
        plan.offset_factor = step;
        plan.ramp = ramp;
        const std::int64_t end = start + static_cast<std::int64_t>(step + 1) * kBlockSamples - 1;
        for (const int object : objects) {
            plan.properties.push_back(timeline.at(object, end));
        }
        out.push_back(std::move(plan));
    }
    return out;
}

// --- PortionWriter -------------------------------------------------------------

PortionWriter::PortionWriter(std::vector<bool> dynamic)
    : dynamic_(std::move(dynamic)), held_(dynamic_.size()), standard_(dynamic_.size()) {}

PortionFrame PortionWriter::frame(std::span<const BlockPlan> plans, bool iframe) const {
    PortionFrame out{.timing = {},
                     .n_blocks = static_cast<int>(plans.size()),
                     .blocks = {},
                     .held = held_,
                     .standard = standard_};
    for (const BlockPlan& plan : plans) {
        out.timing.blocks.push_back(timing_block(plan.offset_factor, plan.ramp));
    }
    for (std::size_t o = 0; o < dynamic_.size(); ++o) {
        const bool dynamic = dynamic_[o];
        bool known = have_;
        for (std::size_t b = 0; b < plans.size(); ++b) {
            const ObjectCodes codes = object_codes(plans[b].properties[o], dynamic);
            ObjectCodes& held = out.held[o];
            std::array<int, 3>& standard = out.standard[o];
            const bool no_delta = iframe && b == 0;
            ObjectInfoBlockFields block;
            block.not_active = !codes.active;
            block.basic = codes.basic;
            block.basic_reuse = !no_delta && known && same_basic(codes.basic, held.basic);
            block.add_table = codes.add;
            if (codes.active && dynamic) {
                ObjectRenderFields& r = block.render_fields;
                if (no_delta || !known || !held.active) {
                    // Everything, the position as it is.
                    block.render = RenderStatus::kAllNew;
                    r.otherprops = r.zone = r.position = true;
                } else {
                    r.position = codes.position != held.position;
                    r.zone = !same_zone(codes, held);
                    r.otherprops = !codes.same_other(held);
                    const int changed = (r.position ? 1 : 0) + (r.zone ? 1 : 0) + (r.otherprops ? 1 : 0);
                    block.render = changed == 0   ? RenderStatus::kReuse
                                   : changed == 3 ? RenderStatus::kAllNew
                                                  : RenderStatus::kPartReuse;
                    const std::array<int, 3> now = codes.standard();
                    const std::array<int, 3> d = {now[0] - standard[0], now[1] - standard[1],
                                                  now[2] - standard[2]};
                    r.diff_pos = r.position &&
                                 std::ranges::all_of(d, [](int v) { return v >= -4 && v <= 3; });
                    r.diff = d;
                }
                fill_render(codes, r);
                if (r.position) {
                    standard = codes.standard();
                }
            }
            // What the decoder holds after the block: an inactive object's
            // basic information as it was, and its render information
            // replaced by the defaults, which the next active block sends
            // anew.
            if (codes.active) {
                held.basic = codes.basic;
                if (dynamic) {
                    held.position = codes.position;
                    held.zone_defaults = codes.zone_defaults;
                    held.zone_flags = codes.zone_flags;
                    held.zone_mask = codes.zone_mask;
                    held.other = codes.other;
                }
            }
            held.active = codes.active;
            known = true;
            out.blocks.push_back(std::move(block));
        }
    }
    return out;
}

PortionFrame PortionWriter::least(const BlockPlan& now, bool iframe) const {
    if (!iframe) {
        return PortionFrame{.timing = {}, .n_blocks = 0, .blocks = {}, .held = held_, .standard = standard_};
    }
    BlockPlan bounded = now;
    bounded.offset_factor = 0;
    bounded.ramp = 0;
    for (ObjectProperties& p : bounded.properties) {
        ObjectProperties kept;
        kept.active = p.active;
        kept.gain_db = p.gain_db;
        kept.priority = p.priority;
        kept.position = p.position;
        p = kept;
    }
    return frame(std::span<const BlockPlan>(&bounded, 1), true);
}

std::size_t PortionWriter::least_bits_bound(bool iframe) const {
    // oa_sample_offset_type and num_obj_info_blocks; in an I-frame one
    // block's timing, with ramp_duration's 11 bits, and per object
    // b_object_not_active, the basic information's 16 bits at most, the render
    // information's position and two default groups, and b_add_table_data.
    if (!iframe) {
        return 4;
    }
    return 4 + 20 + dynamic_.size() * (1 + 16 + 19 + 1);
}

void PortionWriter::commit(const PortionFrame& sent) {
    held_ = sent.held;
    standard_ = sent.standard;
    have_ = have_ || sent.n_blocks > 0;
}

}  // namespace iclforge::ac4::detail
