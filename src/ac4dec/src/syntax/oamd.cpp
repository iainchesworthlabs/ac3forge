#include "syntax/oamd.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include "syntax/metadata.hpp"

namespace ac4::detail {

namespace {

[[nodiscard]] int read_int(BitReader& r, int bits, std::string_view name) {
    return static_cast<int>(r.read(bits, name));
}

// A prefix code of one or two bits, 0b0, 0b10 or 0b11 (Tables 92, 93, 100 and
// 101): one record of the bits read, valued at them, as
// immersive_codec_mode_code is recorded.
[[nodiscard]] int read_prefix_1_2(BitReader& r, std::string_view name) {
    const std::size_t start = r.position();
    if (r.peek_raw(1) == 0) {
        r.consume(1);
        r.emit(start, 1, 0, name);
        return 0;
    }
    const auto code = static_cast<int>(r.peek_raw(2));
    r.consume(2);
    r.emit(start, 2, static_cast<std::uint64_t>(code), name);
    return code;
}

// 3 bits of two's complement: diff_pos3D_* (6.3.9.8.3), -4 to 3.
[[nodiscard]] int signed3(std::uint32_t code) noexcept {
    return code >= 4 ? static_cast<int>(code) - 8 : static_cast<int>(code);
}

// Table 94 and Table 95: ramp_duration in samples.
constexpr std::array<int, 16> kRampDurationTable = {32,   64,   128,  256,  320,  480,  1000, 1001,
                                                    1024, 1600, 1601, 1602, 1920, 2000, 2002, 2048};

// A nested element that returns the bits it read (trim(), bed_render_info(),
// headphone(), add_per_object_md(), ext_prec_alt_pos() and ajoc_bed_info()),
// measured as the reader's position after it less its position before
// (src/ac4dec/ERRATA.md, "bits_used from trim()/bed_render_info()/
// headphone() is measured, not returned"). One whose bits run past the
// budget the syntax gives it fails the substream (the same register, "An
// add_data budget a nested element overruns fails the substream").
class Budget {
   public:
    explicit Budget(std::uint64_t bits) noexcept : bits_(bits) {}
    [[nodiscard]] ParseResult spend(std::size_t before, std::size_t after) {
        const std::uint64_t used = after - before;
        if (used > bits_) {
            return fail(DecodeError::kInvalidStream,
                        "an element reads past the byte budget of the data that holds it");
        }
        bits_ -= used;
        return {};
    }
    [[nodiscard]] std::uint64_t left() const noexcept { return bits_; }

   private:
    std::uint64_t bits_ = 0;
};

// --- 6.2.8.8 bed_render_info and the tools it calls ------------------------------

// tool_t2_to_f_s_b() and tool_t2_to_f_s() (6.2.9.9 and 6.2.9.10), and
// tool_tb_to_f_s_b(), tool_tb_to_f_s(), tool_tf_to_f_s_b() and tool_tf_to_f_s()
// (6.2.8.13 to 6.2.8.16): each a "to front" flag and a gain, else - in the _b
// forms - a "to side" flag and one of two other gains. `code_b` takes the 7 the
// syntax assigns where it sends none.
struct ToolNames {
    std::string_view to_front;
    std::string_view to_side;
    std::string_view gain_a;
    std::string_view gain_b;
    std::string_view gain_c;
};

constexpr ToolNames kT2Names{"b_top_to_front", "b_top_to_side", "gain_t2a_code", "gain_t2b_code",
                             "gain_t2c_code"};
constexpr ToolNames kTbNames{"b_top_back_to_front", "b_top_back_to_side", "gain_t2d_code",
                             "gain_t2e_code", "gain_t2f_code"};
constexpr ToolNames kTfNames{"b_top_front_to_front", "b_top_front_to_side", "gain_t2a_code",
                             "gain_t2b_code", "gain_t2c_code"};

[[nodiscard]] ac4::GainTool parse_tool(BitReader& r, const ToolNames& names, bool side_branch) {
    ac4::GainTool tool;
    if (r.read_flag(names.to_front)) {
        tool.code_a = read_int(r, 3, names.gain_a);
        tool.code_b = 7;
        return tool;
    }
    if (!side_branch) {
        tool.code_b = read_int(r, 3, names.gain_b);
        return tool;
    }
    if (r.read_flag(names.to_side)) {
        tool.code_b = read_int(r, 3, names.gain_b);
        return tool;
    }
    tool.code_c = read_int(r, 3, names.gain_c);
    tool.code_b = 7;
    return tool;
}

// 6.2.8.8a stereo_dmx_coeff()
[[nodiscard]] ac4::StereoDmxCoeff parse_stereo_dmx_coeff(BitReader& r) {
    ac4::StereoDmxCoeff c;
    c.loro_centre_mixgain = read_int(r, 3, "loro_centre_mixgain");
    c.loro_surround_mixgain = read_int(r, 3, "loro_surround_mixgain");
    if (r.read_flag("b_ltrt_mixinfo")) {
        c.ltrt_centre_mixgain = read_int(r, 3, "ltrt_centre_mixgain");
        c.ltrt_surround_mixgain = read_int(r, 3, "ltrt_surround_mixgain");
    }
    if (r.read_flag("b_lfe_mixinfo")) {
        c.lfe_mixgain = read_int(r, 5, "lfe_mixgain");
    }
    c.preferred_dmx_method = read_int(r, 2, "preferred_dmx_method");
    return c;
}

[[nodiscard]] std::optional<ac4::BedRenderInfo> parse_bed_render_info(BitReader& r) {
    if (!r.read_flag("b_bed_render_info")) {
        return std::nullopt;
    }
    ac4::BedRenderInfo info;
    if (r.read_flag("b_stereo_dmx_coeff")) {
        info.stereo_dmx_coeff = parse_stereo_dmx_coeff(r);
    }
    if (!r.read_flag("b_cdmx_data_present")) {
        return info;
    }
    if (r.read_flag("b_cdmx_w_to_f")) {
        info.gain_w_to_f_code = read_int(r, 3, "gain_w_to_f_code");
    }
    if (r.read_flag("b_cdmx_b4_to_b2")) {
        info.gain_b4_to_b2_code = read_int(r, 3, "gain_b4_to_b2_code");
    }
    if (r.read_flag("b_tm_ch_present")) {
        if (r.read_flag("b_cdmx_t2_to_f_s_b")) {
            info.t2_to_f_s_b = parse_tool(r, kT2Names, true);
        }
        if (r.read_flag("b_cdmx_t2_to_f_s")) {
            info.t2_to_f_s = parse_tool(r, kT2Names, false);
        }
    }
    const bool b_tb_ch_present = r.read_flag("b_tb_ch_present");
    if (b_tb_ch_present) {
        if (r.read_flag("b_cdmx_tb_to_f_s_b")) {
            info.tb_to_f_s_b = parse_tool(r, kTbNames, true);
        }
        if (r.read_flag("b_cdmx_tb_to_f_s")) {
            info.tb_to_f_s = parse_tool(r, kTbNames, false);
        }
    }
    const bool b_tf_ch_present = r.read_flag("b_tf_ch_present");
    if (b_tf_ch_present) {
        if (r.read_flag("b_cdmx_tf_to_f_s_b")) {
            info.tf_to_f_s_b = parse_tool(r, kTfNames, true);
        }
        if (r.read_flag("b_cdmx_tf_to_f_s")) {
            info.tf_to_f_s = parse_tool(r, kTfNames, false);
        }
    }
    if (b_tb_ch_present || b_tf_ch_present) {
        if (r.read_flag("b_cdmx_tfb_to_tm")) {
            info.gain_tfb_to_tm_code = read_int(r, 3, "gain_tfb_to_tm_code");
        }
    }
    return info;
}

// --- 6.2.8.9 trim and 6.2.8.9a headphone ------------------------------------------

// 6.3.9.10.4: nine trim configurations.
constexpr int kNumTrimConfigs = 9;

// trim_balance_presence[]'s five flags are one field, [4] its first bit
// (src/ac4dec/ERRATA.md, "Arrays read as one field"), as the inspector reads
// the table of contents' trim().
[[nodiscard]] std::optional<ac4::Trim> parse_trim(BitReader& r) {
    if (!r.read_flag("b_trim_present")) {
        return std::nullopt;
    }
    ac4::Trim trim;
    trim.warp_mode = read_int(r, 2, "warp_mode");
    (void)r.read(2, "reserved");
    trim.global_trim_mode = read_int(r, 2, "global_trim_mode");
    if (trim.global_trim_mode == 0b10) {
        for (int cfg = 0; cfg < kNumTrimConfigs; ++cfg) {
            if (r.read_flag("b_default_trim")) {
                trim.configs.emplace_back(std::nullopt);
                continue;
            }
            ac4::TrimConfig config;
            config.disabled = r.read_flag("b_disable_trim");
            if (!config.disabled) {
                config.presence = read_int(r, 5, "trim_balance_presence");
                if ((config.presence & 0b10000) != 0) {
                    config.trim_centre = read_int(r, 4, "trim_centre");
                }
                if ((config.presence & 0b01000) != 0) {
                    config.trim_surround = read_int(r, 4, "trim_surround");
                }
                if ((config.presence & 0b00100) != 0) {
                    config.trim_height = read_int(r, 4, "trim_height");
                }
                if ((config.presence & 0b00010) != 0) {
                    const int sign = read_int(r, 1, "bal3D_Y_sign_tb_code");
                    config.bal3d_y_tb = std::pair{sign, read_int(r, 4, "bal3D_Y_amount_tb")};
                }
                if ((config.presence & 0b00001) != 0) {
                    const int sign = read_int(r, 1, "bal3D_Y_sign_lis_code");
                    config.bal3d_y_lis = std::pair{sign, read_int(r, 4, "bal3D_Y_amount_lis")};
                }
            }
            trim.configs.emplace_back(config);
        }
    }
    return trim;
}

[[nodiscard]] std::optional<ac4::Headphone> parse_headphone(BitReader& r) {
    if (!r.read_flag("b_headphone")) {
        return std::nullopt;
    }
    ac4::Headphone headphone;
    headphone.hp_operation_mode = read_int(r, 3, "hp_operation_mode");
    if (headphone.hp_operation_mode == 0b001 || headphone.hp_operation_mode == 0b010) {
        headphone.b_head_track_disable_all = r.read_flag("b_head_track_disable_all");
    }
    return headphone;
}

// --- 6.2.8.6 object_basic_info and 6.2.8.7 object_render_info -------------------

void parse_object_basic_info(BitReader& r, ObjectBasicInfo& out) {
    out.b_default_basic_info_md = r.read_flag("b_default_basic_info_md");
    if (out.b_default_basic_info_md) {
        return;
    }
    out.basic_info_md = read_prefix_1_2(r, "basic_info_md");
    if (out.basic_info_md == 0b0 || out.basic_info_md == 0b10) {
        out.object_gain_code = read_prefix_1_2(r, "object_gain_code");
        if (*out.object_gain_code == 0b0) {
            out.object_gain_value = read_int(r, 6, "object_gain_value");
        }
    }
    if (out.basic_info_md == 0b10 || out.basic_info_md == 0b11) {
        out.object_priority_code = read_int(r, 5, "object_priority_code");
    }
}

void parse_object_render_info(BitReader& r, InfoStatus status, bool b_no_delta,
                              ObjectRenderInfo& out) {
    if (status == InfoStatus::kAllNew) {
        out.b_obj_render_otherprops_present = true;
        out.b_obj_render_zone_present = true;
        out.b_obj_render_position_present = true;
    } else {
        out.b_obj_render_otherprops_present = r.read_flag("b_obj_render_otherprops_present");
        out.b_obj_render_zone_present = r.read_flag("b_obj_render_zone_present");
        out.b_obj_render_position_present = r.read_flag("b_obj_render_position_present");
    }
    if (out.b_obj_render_position_present) {
        out.b_diff_pos_coding = b_no_delta ? false : r.read_flag("b_diff_pos_coding");
        if (out.b_diff_pos_coding) {
            out.diff_pos3d_x = signed3(r.read(3, "diff_pos3D_X"));
            out.diff_pos3d_y = signed3(r.read(3, "diff_pos3D_Y"));
            out.diff_pos3d_z = signed3(r.read(3, "diff_pos3D_Z"));
        } else {
            out.pos3d_x = read_int(r, 6, "pos3D_X");
            out.pos3d_y = read_int(r, 6, "pos3D_Y");
            out.pos3d_z_sign = read_int(r, 1, "pos3D_Z_sign");
            out.pos3d_z = read_int(r, 4, "pos3D_Z");
        }
    }
    if (out.b_obj_render_zone_present) {
        out.b_grouped_zone_defaults = r.read_flag("b_grouped_zone_defaults");
        if (!out.b_grouped_zone_defaults) {
            // group_zone_flag[] is one field, [2] its first bit (ERRATA, "Arrays
            // read as one field").
            out.group_zone_flag = read_int(r, 3, "group_zone_flag");
            if ((out.group_zone_flag & 0b100) != 0) {
                out.zone_mask = read_int(r, 3, "zone_mask");
            }
        }
    }
    if (out.b_obj_render_otherprops_present) {
        out.b_grouped_other_defaults = r.read_flag("b_grouped_other_defaults");
        if (!out.b_grouped_other_defaults) {
            out.group_other_mask = read_int(r, 4, "group_other_mask");
            if ((out.group_other_mask & 0b0001) != 0) {
                out.object_width_mode = read_int(r, 1, "object_width_mode");
                if (*out.object_width_mode == 0) {
                    out.object_width_code = read_int(r, 5, "object_width_code");
                } else {
                    out.object_width_x_code = read_int(r, 5, "object_width_X_code");
                    out.object_width_y_code = read_int(r, 5, "object_width_Y_code");
                    out.object_width_z_code = read_int(r, 5, "object_width_Z_code");
                }
            }
            if ((out.group_other_mask & 0b0010) != 0) {
                out.object_screen_factor_code = read_int(r, 3, "object_screen_factor_code");
                out.object_depth_factor = read_int(r, 2, "object_depth_factor");
            }
            if ((out.group_other_mask & 0b0100) != 0) {
                out.b_obj_at_infinity = r.read_flag("b_obj_at_infinity");
                if (!*out.b_obj_at_infinity) {
                    out.object_distance_factor_code = read_int(r, 4, "obj_distance_factor_code");
                }
            }
            if ((out.group_other_mask & 0b1000) != 0) {
                out.object_div_mode = read_int(r, 2, "object_div_mode");
                if (*out.object_div_mode == 0b00) {
                    out.object_div_table = read_int(r, 2, "object_div_table");
                } else if ((*out.object_div_mode & 0b10) != 0) {
                    out.object_div_code = read_int(r, 6, "object_div_code");
                }
            }
        }
    }
}

// 6.2.8.10 add_per_object_md(). The object_info_block() calls it with
// (b_dynamic_object, b_object_not_active), the definition names its parameters
// (b_object_not_active, b_dynamic_object): the parameters are read by name
// (src/ac4dec/ERRATA.md, "add_per_object_md()'s parameters").
[[nodiscard]] ParseResult parse_add_per_object_md(BitReader& r, bool b_object_not_active,
                                                  bool b_dynamic_object, AddPerObjectMd& out) {
    out.b_obj_trim_disable = r.read_flag("b_obj_trim_disable");
    if (!b_object_not_active && b_dynamic_object) {
        if (r.read_flag("b_ext_prec_pos")) {
            ExtPrecPos pos;
            if (auto ok = parse_ext_prec_pos(r, pos); !ok) {
                return ok;
            }
            out.ext_prec_pos = pos;
        }
    }
    out.b_headphone = r.read_flag("b_headphone");
    if (out.b_headphone) {
        out.hp_render_mode_obj = read_int(r, 2, "hp_render_mode_obj");
        out.b_head_track_disable_obj = r.read_flag("b_head_track_disable_obj");
    }
    return check(r);
}

// 6.2.8.5 object_info_block(b_no_delta, b_dynamic_object)
[[nodiscard]] ParseResult parse_object_info_block(BitReader& r, bool b_no_delta,
                                                  bool b_dynamic_object, ObjectInfoBlock& out) {
    out = ObjectInfoBlock{};
    out.b_object_not_active = r.read_flag("b_object_not_active");
    if (out.b_object_not_active) {
        out.basic_status = InfoStatus::kDefault;
    } else if (b_no_delta) {
        out.basic_status = InfoStatus::kAllNew;
    } else {
        out.basic_status =
            r.read_flag("b_basic_info_reuse") ? InfoStatus::kReuse : InfoStatus::kAllNew;
    }
    if (out.basic_status == InfoStatus::kAllNew) {
        parse_object_basic_info(r, out.basic);
    }
    if (out.b_object_not_active || !b_dynamic_object) {
        out.render_status = InfoStatus::kDefault;
    } else if (b_no_delta) {
        out.render_status = InfoStatus::kAllNew;
    } else if (r.read_flag("b_render_info_reuse")) {
        out.render_status = InfoStatus::kReuse;
    } else {
        out.render_status = r.read_flag("b_render_info_partial_reuse") ? InfoStatus::kPartReuse
                                                                       : InfoStatus::kAllNew;
    }
    if (out.render_status == InfoStatus::kAllNew || out.render_status == InfoStatus::kPartReuse) {
        parse_object_render_info(r, out.render_status, b_no_delta, out.render);
    }
    out.b_add_table_data = r.read_flag("b_add_table_data");
    if (out.b_add_table_data) {
        out.add_table_data_size = read_int(r, 4, "add_table_data_size_minus1") + 1;
        Budget budget(8U * static_cast<std::uint64_t>(out.add_table_data_size));
        const std::size_t before = r.position();
        AddPerObjectMd per_object;
        if (auto ok =
                parse_add_per_object_md(r, out.b_object_not_active, b_dynamic_object, per_object);
            !ok) {
            return ok;
        }
        if (auto ok = budget.spend(before, r.position()); !ok) {
            return ok;
        }
        out.per_object = per_object;
        out.add_table_data_bits = budget.left();
        if (auto ok = read_bit_run(r, budget.left(), "add_table_data"); !ok) {
            return ok;
        }
    }
    return check(r);
}

// The objects' blocks of oamd_dyndata_single() and oamd_dyndata_multi(); with
// `skip_ajoc`, the multi form's, which passes over A-JOC coded objects.
[[nodiscard]] ParseResult parse_blocks(BitReader& r, const OamdObjectList& objects, int n_blocks,
                                       bool b_iframe, bool skip_ajoc, OamdDynData& out) {
    if (objects.count > kMaxOamdObjects) {
        return fail(DecodeError::kUnsupported, "more objects than the decoder describes");
    }
    if (n_blocks < 0 || n_blocks > kMaxObjInfoBlocks) {
        return fail(DecodeError::kInvalidStream, "num_obj_info_blocks out of range");
    }
    out = OamdDynData{};
    out.n_objs = objects.count;
    out.n_blocks = n_blocks;
    out.blocks.assign(static_cast<std::size_t>(objects.count * n_blocks), ObjectInfoBlock{});
    for (int i = 0; i < objects.count; ++i) {
        const OamdObjectType& object = objects[i];
        if (skip_ajoc && object.ajoc_coded) {
            continue;
        }
        const bool b_dynamic_object = object.type == ObjType::kDyn && !object.lfe;
        for (int b = 0; b < n_blocks; ++b) {
            if (auto ok =
                    parse_object_info_block(r, b_iframe && b == 0, b_dynamic_object,
                                            out.blocks[static_cast<std::size_t>(i * n_blocks + b)]);
                !ok) {
                return ok;
            }
        }
    }
    return check(r);
}

// ext_prec_alt_pos(n_objs, b_keep, obj_type[], b_lfe[]), 6.2.8.12.
[[nodiscard]] ParseResult parse_ext_prec_alt_pos(BitReader& r, const OamdObjectList& objects,
                                                 bool b_keep, AltDataSet& out) {
    out.ext_prec_alt_pos.assign(static_cast<std::size_t>(objects.count), std::nullopt);
    if (b_keep) {
        return {};
    }
    for (int obj = 0; obj < objects.count; ++obj) {
        if (objects[obj].type == ObjType::kDyn && !objects[obj].lfe) {
            if (r.read_flag("b_ext_prec_alt_pos")) {
                ExtPrecPos pos;
                if (auto ok = parse_ext_prec_pos(r, pos); !ok) {
                    return ok;
                }
                out.ext_prec_alt_pos[static_cast<std::size_t>(obj)] = pos;
            }
        }
    }
    return check(r);
}

// The alternative sets of oamd_dyndata_single(), after the blocks.
[[nodiscard]] ParseResult parse_alternative(BitReader& r, const OamdObjectList& objects,
                                            OamdDynData& out) {
    out.alternative = true;
    out.b_ducking_disabled = r.read_flag("b_ducking_disabled");
    out.object_sound_category = r.read(2, "object_sound_category");
    if (out.object_sound_category == 3) {
        out.object_sound_category += r.variable_bits(2, "object_sound_category");
    }
    out.n_alt_data_sets = r.read(2, "n_alt_data_sets");
    if (out.n_alt_data_sets == 3) {
        out.n_alt_data_sets += r.variable_bits(2, "n_alt_data_sets");
    }
    if (auto ok = check(r); !ok) {
        return ok;
    }
    // Each set reads at least two bits, so a count the rest of the substream
    // cannot hold is a truncated substream, found before anything is sized.
    if (out.n_alt_data_sets > r.remaining_bits() / 2) {
        return fail(DecodeError::kTruncated, "n_alt_data_sets runs past the end of the substream");
    }
    for (std::uint64_t s = 0; s < out.n_alt_data_sets; ++s) {
        AltDataSet& set = out.alt_sets.emplace_back();
        set.b_keep = r.read_flag("b_keep");
        if (!set.b_keep) {
            int n_data_points = objects.count;
            if (objects.count > 0 && objects[0].type == ObjType::kIsf) {
                n_data_points = 1;
            } else {
                set.b_common_data = r.read_flag("b_common_data");
                if (set.b_common_data) {
                    n_data_points = 1;
                }
            }
            for (int dp = 0; dp < n_data_points && dp < objects.count; ++dp) {
                AltDataPoint& point = set.points.emplace_back();
                const OamdObjectType& object = objects[dp];
                if (object.type == ObjType::kBed || object.type == ObjType::kIsf) {
                    if (r.read_flag("b_alt_gain")) {
                        point.alt_obj_gain = read_int(r, 6, "alt_obj_gain");
                    }
                } else {
                    if (r.read_flag("b_alt_gain")) {
                        point.alt_obj_gain = read_int(r, 6, "alt_obj_gain");
                    }
                    if (!object.lfe) {
                        point.b_alt_position = r.read_flag("b_alt_position");
                        if (point.b_alt_position) {
                            point.alt_pos3d_x = read_int(r, 6, "alt_pos3D_X");
                            point.alt_pos3d_y = read_int(r, 6, "alt_pos3D_Y");
                            point.alt_pos3d_z_sign = read_int(r, 1, "alt_pos3D_Z_sign");
                            point.alt_pos3d_z = read_int(r, 4, "alt_pos3D_Z");
                        }
                    }
                }
            }
        }
        set.b_additional_data = r.read_flag("b_additional_data");
        if (set.b_additional_data) {
            const std::uint64_t bytes = r.variable_bits(2, "skip_bits") + 1;
            if (auto ok = check(r); !ok) {
                return ok;
            }
            if (bytes > r.remaining_bits() / 8U) {
                return fail(DecodeError::kTruncated, "skip_bits run past the end of the substream");
            }
            Budget budget(bytes * 8U);
            const std::size_t before = r.position();
            if (auto ok = parse_ext_prec_alt_pos(r, objects, set.b_keep, set); !ok) {
                return ok;
            }
            if (auto ok = budget.spend(before, r.position()); !ok) {
                return ok;
            }
            set.skip_bits = budget.left();
            if (auto ok = read_bit_run(r, budget.left(), "skip_data"); !ok) {
                return ok;
            }
        }
        if (auto ok = check(r); !ok) {
            return ok;
        }
    }
    return check(r);
}

}  // namespace

ParseResult parse_ext_prec_pos(BitReader& r, ExtPrecPos& out) {
    // ext_prec_pos_presence[] is one field, [2] its first bit (ERRATA,
    // "Arrays read as one field"); Table 122 gives [2] to X, [1] to Y and [0]
    // to Z.
    out.presence = read_int(r, 3, "ext_prec_pos_presence");
    if ((out.presence & 0b100) != 0) {
        out.ext_prec_pos3d_x = read_int(r, 2, "ext_prec_pos3D_X");
    }
    if ((out.presence & 0b010) != 0) {
        out.ext_prec_pos3d_y = read_int(r, 2, "ext_prec_pos3D_Y");
    }
    if ((out.presence & 0b001) != 0) {
        out.ext_prec_pos3d_z = read_int(r, 2, "ext_prec_pos3D_Z");
    }
    return check(r);
}

ParseResult parse_oamd_timing_data(BitReader& r, OamdTimingData& out) {
    out = OamdTimingData{};
    out.oa_sample_offset_type = read_prefix_1_2(r, "oa_sample_offset_type");
    if (out.oa_sample_offset_type == 0b10) {
        // Table 93: 0b0 is 16, 0b10 is 8, 0b11 is 24.
        const int code = read_prefix_1_2(r, "oa_sample_offset_code");
        out.sample_offset = code == 0b0 ? 16 : code == 0b10 ? 8 : 24;
    } else if (out.oa_sample_offset_type == 0b11) {
        out.sample_offset = read_int(r, 5, "oa_sample_offset");
    }
    out.num_obj_info_blocks = read_int(r, 3, "num_obj_info_blocks");
    for (int blk = 0; blk < out.num_obj_info_blocks; ++blk) {
        const auto b = static_cast<std::size_t>(blk);
        out.block_offset_factor[b] = read_int(r, 6, "block_offset_factor");
        out.ramp_duration_code[b] = read_int(r, 2, "ramp_duration_code");
        switch (out.ramp_duration_code[b]) {
            case 0b00:
                out.ramp_duration[b] = 0;
                break;
            case 0b01:
                out.ramp_duration[b] = 512;
                break;
            case 0b10:
                out.ramp_duration[b] = 1536;
                break;
            default:
                if (r.read_flag("b_use_ramp_table")) {
                    out.ramp_duration[b] = kRampDurationTable[static_cast<std::size_t>(
                        read_int(r, 4, "ramp_duration_table"))];
                } else {
                    out.ramp_duration[b] = read_int(r, 11, "ramp_duration");
                }
                break;
        }
    }
    return check(r);
}

ParseResult parse_oamd_dyndata_single(BitReader& r, const OamdObjectList& objects, int n_blocks,
                                      bool b_iframe, bool b_alternative, OamdDynData& out) {
    if (auto ok = parse_blocks(r, objects, n_blocks, b_iframe, false, out); !ok) {
        return ok;
    }
    if (b_alternative) {
        return parse_alternative(r, objects, out);
    }
    return {};
}

ParseResult parse_oamd_dyndata_multi(BitReader& r, const OamdObjectList& objects, int n_blocks,
                                     bool b_iframe, OamdDynData& out) {
    return parse_blocks(r, objects, n_blocks, b_iframe, true, out);
}

ParseResult parse_oamd_common_data(BitReader& r, OamdCommonData& out) {
    out = OamdCommonData{};
    ac4::OamdCommonData& data = out.data;
    data.b_default_screen_size_ratio = r.read_flag("b_default_screen_size_ratio");
    if (!data.b_default_screen_size_ratio) {
        data.master_screen_size_ratio_code = read_int(r, 5, "master_screen_size_ratio_code");
    }
    data.b_bed_object_chan_distribute = r.read_flag("b_bed_object_chan_distribute");
    if (!r.read_flag("b_additional_data")) {
        return check(r);
    }
    std::uint64_t add_data_bytes = r.read(1, "add_data_bytes_minus1") + 1U;
    if (add_data_bytes == 2) {
        add_data_bytes += r.variable_bits(2, "add_data_bytes");
    }
    if (auto ok = check(r); !ok) {
        return ok;
    }
    if (add_data_bytes > r.remaining_bits() / 8U) {
        return fail(DecodeError::kTruncated, "add_data_bytes run past the end of the substream");
    }
    Budget budget(add_data_bytes * 8U);
    std::size_t before = r.position();
    data.trim = parse_trim(r);
    if (auto ok = budget.spend(before, r.position()); !ok) {
        return ok;
    }
    if (budget.left() != 0) {
        before = r.position();
        data.bed_render_info = parse_bed_render_info(r);
        if (auto ok = budget.spend(before, r.position()); !ok) {
            return ok;
        }
    }
    if (budget.left() != 0) {
        before = r.position();
        data.headphone = parse_headphone(r);
        if (auto ok = budget.spend(before, r.position()); !ok) {
            return ok;
        }
    }
    out.add_data_bits = budget.left();
    return read_bit_run(r, budget.left(), "add_data");
}

ParseResult parse_oamd_substream(BitReader& r, const OamdSubstreamContext& ctx,
                                 OamdSubstream& out) {
    out = OamdSubstream{};
    if (r.read_flag("b_oamd_common_data_present")) {
        OamdCommonData common;
        if (auto ok = parse_oamd_common_data(r, common); !ok) {
            return ok;
        }
        out.common = std::move(common);
    }
    if (r.read_flag("b_oamd_timing_present")) {
        OamdTimingData timing;
        if (auto ok = parse_oamd_timing_data(r, timing); !ok) {
            return ok;
        }
        out.timing = timing;
    }
    if (auto ok = check(r); !ok) {
        return ok;
    }
    if (!ctx.b_alternative) {
        std::optional<int> blocks =
            out.timing ? std::optional<int>{out.timing->num_obj_info_blocks} : ctx.carried_blocks;
        if (!blocks) {
            return fail(DecodeError::kMissingIFrame,
                        "oamd_dyndata_multi() needs an oamd_timing_data() that no frame has sent");
        }
        OamdDynData dyndata;
        if (auto ok = parse_oamd_dyndata_multi(r, ctx.objects, *blocks, ctx.b_oamd_ndot, dyndata);
            !ok) {
            return ok;
        }
        out.dyndata = std::move(dyndata);
    }
    r.align();
    return check(r);
}

}  // namespace ac4::detail
