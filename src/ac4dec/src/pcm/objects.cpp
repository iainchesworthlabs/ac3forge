#include "pcm/objects.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ac4::detail {
namespace {

constexpr double kMinusInfinity = -std::numeric_limits<double>::infinity();

[[nodiscard]] std::size_t at(int index) noexcept {
    return static_cast<std::size_t>(index);
}

// Table 102.
[[nodiscard]] double gain_of_value(int value) noexcept {
    return value <= 14 ? static_cast<double>(15 - value) : static_cast<double>(14 - value);
}

// Table 107: the Y position's exponent.
constexpr std::array<double, 4> kDepthExponent = {0.25, 0.5, 1.0, 2.0};

// Table 108.
constexpr std::array<double, 16> kDistance = {1.1, 1.3, 1.6,  2.0,  2.5,  3.2,  4.0,  5.0,
                                              6.3, 7.9, 10.0, 12.6, 15.8, 20.0, 25.1, 50.1};

// Table 110.
constexpr std::array<double, 4> kDivergenceTable = {0.500755, 0.608529, 0.704833, 1.0};

// Table 111; code 0 is reserved.
constexpr std::array<double, 64> kDivergenceCode = {
    0.0,      0.0,      0.004026, 0.00716,  0.012731, 0.020173, 0.028485, 0.04021,
    0.050582, 0.063601, 0.079914, 0.100299, 0.125666, 0.140532, 0.157027, 0.175282,
    0.195417, 0.217536, 0.241718, 0.268002, 0.296377, 0.326766, 0.359017, 0.392895,
    0.428081, 0.464184, 0.500755, 0.537316, 0.573389, 0.608529, 0.642346, 0.674524,
    0.704833, 0.733123, 0.75932,  0.783416, 0.805451, 0.825506, 0.843686, 0.860112,
    0.874914, 0.888222, 0.900168, 0.910875, 0.920461, 0.929035, 0.936698, 0.943544,
    0.949656, 0.955112, 0.95998,  0.964322, 0.968195, 0.974729, 0.979923, 0.98405,
    0.98733,  0.989935, 0.992874, 0.994955, 0.996817, 0.99821,  0.998993, 1.0};

// Tables 123 to 125: ext_prec_pos3D_* 0b00 to 0b11 are 1, 2, -1 and -2; 0
// where the block sends none (6.3.9.12.1).
[[nodiscard]] int ext_value(const std::optional<int>& code) noexcept {
    if (!code) {
        return 0;
    }
    constexpr std::array<int, 4> kValues = {1, 2, -1, -2};
    return kValues[at(*code & 3)];
}

[[nodiscard]] double clip3(double lo, double hi, double v) noexcept {
    return std::clamp(v, lo, hi);
}

[[nodiscard]] int clip3(int lo, int hi, int v) noexcept {
    return std::clamp(v, lo, hi);
}

// Table 99's DEFAULT, for an inactive object.
void default_render(ObjectProperties& p) {
    p.position = {0.5, 0.5, 0.0};
    p.zone_mask = 0;
    p.enable_elevation = false;
    p.width = {};
    p.screen_factor = 0.0;
    p.snap = false;
}

// 6.3.9.8.4: the position the block sends, of standard precision or as a
// difference from the last, refined by extended precision where the block's
// add_per_object_md() sends it.
void apply_position(const ObjectRenderInfo& r, const std::optional<ExtPrecPos>& ext,
                    ObjectMetadataState& s, ObjectProperties& p) {
    const int ex = ext ? ext_value(ext->ext_prec_pos3d_x) : 0;
    const int ey = ext ? ext_value(ext->ext_prec_pos3d_y) : 0;
    const int ez = ext ? ext_value(ext->ext_prec_pos3d_z) : 0;
    if (r.b_diff_pos_coding) {
        const std::array<int, 3> prev = s.standard;
        p.position[0] = clip3(0.0, 1.0,
                              clip3(0, 62, prev[0]) / 62.0 + clip3(-4, 3, r.diff_pos3d_x) / 62.0 +
                                  clip3(-2, 2, ex) / (62.0 * 5.0));
        p.position[1] = clip3(0.0, 1.0,
                              clip3(0, 62, prev[1]) / 62.0 + clip3(-4, 3, r.diff_pos3d_y) / 62.0 +
                                  clip3(-2, 2, ey) / (62.0 * 5.0));
        p.position[2] = clip3(-1.0, 1.0,
                              clip3(-15, 15, prev[2]) / 15.0 + clip3(-4, 3, r.diff_pos3d_z) / 15.0 +
                                  clip3(-2, 2, ez) / (15.0 * 5.0));
        // The standard precision the next difference refers to (src/ac4dec/
        // ERRATA.md, "Object audio metadata").
        s.standard = {clip3(0, 62, prev[0] + r.diff_pos3d_x),
                      clip3(0, 62, prev[1] + r.diff_pos3d_y),
                      clip3(-15, 15, prev[2] + r.diff_pos3d_z)};
        return;
    }
    const double sign = r.pos3d_z_sign != 0 ? 1.0 : -1.0;
    p.position[0] =
        clip3(0.0, 1.0, clip3(0, 62, r.pos3d_x) / 62.0 + clip3(-2, 2, ex) / (62.0 * 5.0));
    p.position[1] =
        clip3(0.0, 1.0, clip3(0, 62, r.pos3d_y) / 62.0 + clip3(-2, 2, ey) / (62.0 * 5.0));
    p.position[2] =
        clip3(-1.0, 1.0, sign * (clip3(0, 15, r.pos3d_z) / 15.0 + clip3(-2, 2, ez) / (15.0 * 5.0)));
    s.standard = {clip3(0, 62, r.pos3d_x), clip3(0, 62, r.pos3d_y),
                  (r.pos3d_z_sign != 0 ? 1 : -1) * clip3(0, 15, r.pos3d_z)};
}

// 6.3.9.8.5 to 6.3.9.8.9: the zone group, its defaults where grouped.
void apply_zone(const ObjectRenderInfo& r, ObjectProperties& p) {
    if (r.b_grouped_zone_defaults) {
        p.zone_mask = 0;
        p.enable_elevation = true;
        p.snap = false;
        return;
    }
    p.zone_mask = (r.group_zone_flag & 0b100) != 0 ? r.zone_mask.value_or(0) : 0;
    p.enable_elevation = (r.group_zone_flag & 0b010) == 0;
    p.snap = (r.group_zone_flag & 0b001) != 0;
}

// 6.3.9.8.10 to 6.3.9.8.23: the other properties' group, each absent one at
// its default (src/ac4dec/ERRATA.md, "Object audio metadata").
void apply_other(const ObjectRenderInfo& r, ObjectProperties& p) {
    const double previous_divergence = p.divergence;
    p.width = {};
    p.screen_factor = 0.0;
    p.depth_exponent = 1.0;
    p.distance.reset();
    p.divergence = 0.0;
    if (r.b_grouped_other_defaults) {
        return;
    }
    if ((r.group_other_mask & 0b0001) != 0) {
        if (r.object_width_mode.value_or(0) == 0) {
            const double w = r.object_width_code.value_or(0) / 31.0;
            p.width = {w, w, w};
        } else {
            p.width = {r.object_width_x_code.value_or(0) / 31.0,
                       r.object_width_y_code.value_or(0) / 31.0,
                       r.object_width_z_code.value_or(0) / 31.0};
        }
    }
    if ((r.group_other_mask & 0b0010) != 0) {
        p.screen_factor = (r.object_screen_factor_code.value_or(0) + 1) / 8.0;
        p.depth_exponent = kDepthExponent[at(r.object_depth_factor.value_or(2) & 3)];
    }
    if ((r.group_other_mask & 0b0100) != 0) {
        p.distance = r.b_obj_at_infinity.value_or(false)
                         ? std::numeric_limits<double>::infinity()
                         : kDistance[at(r.object_distance_factor_code.value_or(0) & 15)];
    }
    if ((r.group_other_mask & 0b1000) != 0) {
        switch (r.object_div_mode.value_or(0)) {
            case 0b00:
                p.divergence = kDivergenceTable[at(r.object_div_table.value_or(0) & 3)];
                break;
            case 0b10: {
                const int code = r.object_div_code.value_or(0) & 63;
                // Code 0 is reserved: the divergence stays as it was.
                p.divergence = code == 0 ? previous_divergence : kDivergenceCode[at(code)];
                break;
            }
            default:
                // 0b01 reuses the last block's; 0b11 is reserved, read alike.
                p.divergence = previous_divergence;
                break;
        }
    }
}

}  // namespace

ObjectProperties apply_block(const ObjectInfoBlock& block, bool dynamic,
                             std::optional<double> previous_gain, ObjectMetadataState& state) {
    ObjectProperties p = state.have ? state.properties : ObjectProperties{};
    p.active = !block.b_object_not_active;

    // Table 98 and 6.3.9.7.
    switch (block.basic_status) {
        case InfoStatus::kDefault:
            p.gain_db = kMinusInfinity;
            p.priority = 0.0;
            break;
        case InfoStatus::kAllNew: {
            const ObjectBasicInfo& b = block.basic;
            p.gain_db = 0.0;
            p.priority = 1.0;
            if (!b.b_default_basic_info_md) {
                if (b.basic_info_md == 0b0 || b.basic_info_md == 0b10) {
                    switch (b.object_gain_code.value_or(0b0)) {
                        case 0b0:
                            p.gain_db = gain_of_value(b.object_gain_value.value_or(0));
                            break;
                        case 0b10:
                            p.gain_db = kMinusInfinity;
                            break;
                        default:
                            p.gain_db = previous_gain.value_or(0.0);
                            break;
                    }
                }
                if (b.basic_info_md == 0b10 || b.basic_info_md == 0b11) {
                    p.priority = b.object_priority_code.value_or(31) / 31.0;
                }
            }
            break;
        }
        default:
            break;  // REUSE
    }

    // Table 99 and 6.3.9.8, for an object with render information.
    const std::optional<ExtPrecPos> ext =
        block.per_object ? block.per_object->ext_prec_pos : std::optional<ExtPrecPos>{};
    if (block.b_object_not_active) {
        default_render(p);
    } else if (dynamic) {
        switch (block.render_status) {
            case InfoStatus::kAllNew:
            case InfoStatus::kPartReuse: {
                const ObjectRenderInfo& r = block.render;
                if (r.b_obj_render_position_present) {
                    apply_position(r, ext, state, p);
                }
                if (r.b_obj_render_zone_present) {
                    apply_zone(r, p);
                }
                if (r.b_obj_render_otherprops_present) {
                    apply_other(r, p);
                }
                break;
            }
            case InfoStatus::kDefault:
                default_render(p);
                break;
            default:
                break;  // REUSE
        }
    }

    // add_per_object_md(), for this block (src/ac4dec/ERRATA.md, "Object
    // audio metadata").
    p.trim_disabled = block.per_object && block.per_object->b_obj_trim_disable;
    p.headphone_render_mode.reset();
    p.head_track_disabled = false;
    if (block.per_object && block.per_object->b_headphone) {
        p.headphone_render_mode = block.per_object->hp_render_mode_obj;
        p.head_track_disabled = block.per_object->b_head_track_disable_obj;
    }
    state.properties = p;
    state.have = true;
    return p;
}

BlockTiming block_timing(const OamdTimingData& timing, int block) noexcept {
    if (block < 0 || block >= timing.num_obj_info_blocks || block >= kMaxObjInfoBlocks) {
        return {};
    }
    return {.sample = timing.sample_offset + 32 * timing.block_offset_factor[at(block)],
            .ramp = timing.ramp_duration[at(block)]};
}

std::optional<Speaker> speaker_of_index(int index) noexcept {
    switch (index) {
        case 0:
            return Speaker::kLeft;
        case 1:
            return Speaker::kRight;
        case 2:
            return Speaker::kCentre;
        case 3:
            return Speaker::kLeftSurround;
        case 4:
            return Speaker::kRightSurround;
        case 5:
            return Speaker::kLeftBack;
        case 6:
            return Speaker::kRightBack;
        case 7:
            return Speaker::kTopFrontLeft;
        case 8:
            return Speaker::kTopFrontRight;
        case 9:
            return Speaker::kTopBackLeft;
        case 10:
            return Speaker::kTopBackRight;
        case 11:
            return Speaker::kLfe;
        case 12:
            return Speaker::kTopSideLeft;
        case 13:
            return Speaker::kTopSideRight;
        case 19:
            return Speaker::kLfe2;
        case 26:
            return Speaker::kLeftWide;
        case 27:
            return Speaker::kRightWide;
        default:
            return std::nullopt;
    }
}

}  // namespace ac4::detail
