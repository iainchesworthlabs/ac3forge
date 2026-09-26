#include "oamd/oamd_syntax.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ac4::detail {
namespace {

[[nodiscard]] std::uint64_t u(int value) noexcept {
    return static_cast<std::uint64_t>(value);
}

// A prefix code of Tables 92, 93, 100 and 101: 0b0 in one bit, 0b10 and 0b11
// in two.
void write_prefix(BitWriter& w, int code, std::string_view name) {
    if (code == 0b0) {
        w.write(1, 0b0, name);
    } else {
        w.write(2, u(code), name);
    }
}

// trim(), 6.2.8.9.
void write_trim(BitWriter& w, const std::optional<TrimFields>& trim) {
    w.write(1, trim ? 1U : 0U, "b_trim_present");
    if (!trim) {
        return;
    }
    w.write(2, u(trim->warp_mode), "warp_mode");
    w.write(2, 0, "reserved");
    w.write(2, u(trim->global_trim_mode), "global_trim_mode");
    if (trim->global_trim_mode != 0b10) {
        return;
    }
    for (const TrimConfigFields& c : trim->configs) {
        w.write(1, c.default_trim ? 1U : 0U, "b_default_trim");
        if (c.default_trim) {
            continue;
        }
        w.write(1, c.disable ? 1U : 0U, "b_disable_trim");
        if (c.disable) {
            continue;
        }
        w.write(5, u(c.presence), "trim_balance_presence");
        if ((c.presence & 0b10000) != 0) {
            w.write(4, u(c.centre), "trim_centre");
        }
        if ((c.presence & 0b01000) != 0) {
            w.write(4, u(c.surround), "trim_surround");
        }
        if ((c.presence & 0b00100) != 0) {
            w.write(4, u(c.height), "trim_height");
        }
        if ((c.presence & 0b00010) != 0) {
            w.write(1, u(c.tb_sign), "bal3D_Y_sign_tb_code");
            w.write(4, u(c.tb_amount), "bal3D_Y_amount_tb");
        }
        if ((c.presence & 0b00001) != 0) {
            w.write(1, u(c.lis_sign), "bal3D_Y_sign_lis_code");
            w.write(4, u(c.lis_amount), "bal3D_Y_amount_lis");
        }
    }
}

// The gain tools: tool_t2_to_f_s_b() and tool_t2_to_f_s() (6.2.9.9 and
// 6.2.9.10), tool_tb_to_f_s[_b]() (6.2.8.13 and 6.2.8.14) and
// tool_tf_to_f_s[_b]() (6.2.8.15 and 6.2.8.16).
struct ToolNames {
    std::string_view to_front;
    std::string_view to_side;
    std::string_view gain_a;
    std::string_view gain_b;
    std::string_view gain_c;
};

void write_tool(BitWriter& w, const ToolNames& names, bool side_branch,
                const GainToolFields& tool) {
    w.write(1, tool.to_front ? 1U : 0U, names.to_front);
    if (tool.to_front) {
        w.write(3, u(tool.gain), names.gain_a);
        return;
    }
    if (!side_branch) {
        w.write(3, u(tool.gain), names.gain_b);
        return;
    }
    w.write(1, tool.to_side ? 1U : 0U, names.to_side);
    w.write(3, u(tool.gain), tool.to_side ? names.gain_b : names.gain_c);
}

void write_optional_tool(BitWriter& w, std::string_view flag, const ToolNames& names,
                         bool side_branch, const std::optional<GainToolFields>& tool) {
    w.write(1, tool ? 1U : 0U, flag);
    if (tool) {
        write_tool(w, names, side_branch, *tool);
    }
}

constexpr ToolNames kT2{"b_top_to_front", "b_top_to_side", "gain_t2a_code", "gain_t2b_code",
                        "gain_t2c_code"};
constexpr ToolNames kTb{"b_top_back_to_front", "b_top_back_to_side", "gain_t2d_code",
                        "gain_t2e_code", "gain_t2f_code"};
constexpr ToolNames kTf{"b_top_front_to_front", "b_top_front_to_side", "gain_t2a_code",
                        "gain_t2b_code", "gain_t2c_code"};

// bed_render_info(), 6.2.8.8.
void write_bed_render_info(BitWriter& w, const std::optional<BedRenderFields>& bed) {
    w.write(1, bed ? 1U : 0U, "b_bed_render_info");
    if (!bed) {
        return;
    }
    w.write(1, bed->stereo_dmx ? 1U : 0U, "b_stereo_dmx_coeff");
    if (bed->stereo_dmx) {
        // stereo_dmx_coeff(), 6.2.8.8a.
        const BedRenderFields::StereoDmx& s = *bed->stereo_dmx;
        w.write(3, u(s.loro_centre), "loro_centre_mixgain");
        w.write(3, u(s.loro_surround), "loro_surround_mixgain");
        w.write(1, s.ltrt ? 1U : 0U, "b_ltrt_mixinfo");
        if (s.ltrt) {
            w.write(3, u((*s.ltrt)[0]), "ltrt_centre_mixgain");
            w.write(3, u((*s.ltrt)[1]), "ltrt_surround_mixgain");
        }
        w.write(1, s.lfe ? 1U : 0U, "b_lfe_mixinfo");
        if (s.lfe) {
            w.write(5, u(*s.lfe), "lfe_mixgain");
        }
        w.write(2, u(s.preferred_dmx_method), "preferred_dmx_method");
    }
    w.write(1, bed->cdmx ? 1U : 0U, "b_cdmx_data_present");
    if (!bed->cdmx) {
        return;
    }
    w.write(1, bed->gain_w_to_f ? 1U : 0U, "b_cdmx_w_to_f");
    if (bed->gain_w_to_f) {
        w.write(3, u(*bed->gain_w_to_f), "gain_w_to_f_code");
    }
    w.write(1, bed->gain_b4_to_b2 ? 1U : 0U, "b_cdmx_b4_to_b2");
    if (bed->gain_b4_to_b2) {
        w.write(3, u(*bed->gain_b4_to_b2), "gain_b4_to_b2_code");
    }
    w.write(1, bed->tm_ch_present ? 1U : 0U, "b_tm_ch_present");
    if (bed->tm_ch_present) {
        write_optional_tool(w, "b_cdmx_t2_to_f_s_b", kT2, true, bed->t2_to_f_s_b);
        write_optional_tool(w, "b_cdmx_t2_to_f_s", kT2, false, bed->t2_to_f_s);
    }
    w.write(1, bed->tb_ch_present ? 1U : 0U, "b_tb_ch_present");
    if (bed->tb_ch_present) {
        write_optional_tool(w, "b_cdmx_tb_to_f_s_b", kTb, true, bed->tb_to_f_s_b);
        write_optional_tool(w, "b_cdmx_tb_to_f_s", kTb, false, bed->tb_to_f_s);
    }
    w.write(1, bed->tf_ch_present ? 1U : 0U, "b_tf_ch_present");
    if (bed->tf_ch_present) {
        write_optional_tool(w, "b_cdmx_tf_to_f_s_b", kTf, true, bed->tf_to_f_s_b);
        write_optional_tool(w, "b_cdmx_tf_to_f_s", kTf, false, bed->tf_to_f_s);
    }
    if (bed->tb_ch_present || bed->tf_ch_present) {
        w.write(1, bed->gain_tfb_to_tm ? 1U : 0U, "b_cdmx_tfb_to_tm");
        if (bed->gain_tfb_to_tm) {
            w.write(3, u(*bed->gain_tfb_to_tm), "gain_tfb_to_tm_code");
        }
    }
}

// headphone(), 6.2.8.9a.
void write_headphone(BitWriter& w, const std::optional<HeadphoneFields>& hp) {
    w.write(1, hp ? 1U : 0U, "b_headphone");
    if (!hp) {
        return;
    }
    w.write(3, u(hp->operation_mode), "hp_operation_mode");
    if (hp->operation_mode == 0b001 || hp->operation_mode == 0b010) {
        w.write(1, hp->head_track_disable_all ? 1U : 0U, "b_head_track_disable_all");
    }
}

// object_basic_info(), 6.2.8.6.
void write_basic_info(BitWriter& w, const ObjectBasicFields& b) {
    w.write(1, b.defaults ? 1U : 0U, "b_default_basic_info_md");
    if (b.defaults) {
        return;
    }
    write_prefix(w, b.info_md, "basic_info_md");
    if (b.info_md == 0b0 || b.info_md == 0b10) {
        write_prefix(w, b.gain_code, "object_gain_code");
        if (b.gain_code == 0b0) {
            w.write(6, u(b.gain_value), "object_gain_value");
        }
    }
    if (b.info_md == 0b10 || b.info_md == 0b11) {
        w.write(5, u(b.priority), "object_priority_code");
    }
}

// object_render_info(object_render_info_status, b_no_delta), 6.2.8.7.
void write_render_info(BitWriter& w, bool all_new, bool no_delta, const ObjectRenderFields& r) {
    const bool otherprops = all_new || r.otherprops;
    const bool zone = all_new || r.zone;
    const bool position = all_new || r.position;
    if (!all_new) {
        w.write(1, r.otherprops ? 1U : 0U, "b_obj_render_otherprops_present");
        w.write(1, r.zone ? 1U : 0U, "b_obj_render_zone_present");
        w.write(1, r.position ? 1U : 0U, "b_obj_render_position_present");
    }
    if (position) {
        const bool diff = !no_delta && r.diff_pos;
        if (!no_delta) {
            w.write(1, diff ? 1U : 0U, "b_diff_pos_coding");
        }
        if (diff) {
            // 3 bits of two's complement each.
            w.write(3, u(r.diff[0] & 7), "diff_pos3D_X");
            w.write(3, u(r.diff[1] & 7), "diff_pos3D_Y");
            w.write(3, u(r.diff[2] & 7), "diff_pos3D_Z");
        } else {
            w.write(6, u(r.x), "pos3D_X");
            w.write(6, u(r.y), "pos3D_Y");
            w.write(1, u(r.z_sign), "pos3D_Z_sign");
            w.write(4, u(r.z), "pos3D_Z");
        }
    }
    if (zone) {
        w.write(1, r.zone_defaults ? 1U : 0U, "b_grouped_zone_defaults");
        if (!r.zone_defaults) {
            w.write(3, u(r.zone_flags), "group_zone_flag");
            if ((r.zone_flags & 0b100) != 0) {
                w.write(3, u(r.zone_mask), "zone_mask");
            }
        }
    }
    if (otherprops) {
        w.write(1, r.other_defaults ? 1U : 0U, "b_grouped_other_defaults");
        if (r.other_defaults) {
            return;
        }
        w.write(4, u(r.other_mask), "group_other_mask");
        if ((r.other_mask & 0b0001) != 0) {
            w.write(1, u(r.width_mode), "object_width_mode");
            if (r.width_mode == 0) {
                w.write(5, u(r.width), "object_width_code");
            } else {
                w.write(5, u(r.width_xyz[0]), "object_width_X_code");
                w.write(5, u(r.width_xyz[1]), "object_width_Y_code");
                w.write(5, u(r.width_xyz[2]), "object_width_Z_code");
            }
        }
        if ((r.other_mask & 0b0010) != 0) {
            w.write(3, u(r.screen_factor), "object_screen_factor_code");
            w.write(2, u(r.depth_factor), "object_depth_factor");
        }
        if ((r.other_mask & 0b0100) != 0) {
            w.write(1, r.at_infinity ? 1U : 0U, "b_obj_at_infinity");
            if (!r.at_infinity) {
                w.write(4, u(r.distance), "obj_distance_factor_code");
            }
        }
        if ((r.other_mask & 0b1000) != 0) {
            w.write(2, u(r.div_mode), "object_div_mode");
            if (r.div_mode == 0b00) {
                w.write(2, u(r.div_table), "object_div_table");
            } else if ((r.div_mode & 0b10) != 0) {
                w.write(6, u(r.div_code), "object_div_code");
            }
        }
    }
}

// ext_prec_pos(), 6.2.8.11.
void write_ext_prec_pos(BitWriter& w, const ExtPrecPosFields& p) {
    w.write(3, u(p.presence), "ext_prec_pos_presence");
    if ((p.presence & 0b100) != 0) {
        w.write(2, u(p.xyz[0]), "ext_prec_pos3D_X");
    }
    if ((p.presence & 0b010) != 0) {
        w.write(2, u(p.xyz[1]), "ext_prec_pos3D_Y");
    }
    if ((p.presence & 0b001) != 0) {
        w.write(2, u(p.xyz[2]), "ext_prec_pos3D_Z");
    }
}

// add_per_object_md(b_object_not_active, b_dynamic_object), 6.2.8.10.
void write_add_per_object_md(BitWriter& w, bool not_active, bool dynamic,
                             const AddPerObjectFields& a) {
    w.write(1, a.trim_disable ? 1U : 0U, "b_obj_trim_disable");
    if (!not_active && dynamic) {
        w.write(1, a.ext_prec_pos ? 1U : 0U, "b_ext_prec_pos");
        if (a.ext_prec_pos) {
            write_ext_prec_pos(w, *a.ext_prec_pos);
        }
    }
    w.write(1, a.headphone ? 1U : 0U, "b_headphone");
    if (a.headphone) {
        w.write(2, u(a.headphone->first), "hp_render_mode_obj");
        w.write(1, a.headphone->second ? 1U : 0U, "b_head_track_disable_obj");
    }
}

[[nodiscard]] std::uint64_t bytes_for(std::size_t bits) noexcept {
    return std::max<std::uint64_t>(1, (bits + 7) / 8);
}

}  // namespace

void write_oamd_common_data(BitWriter& w, const OamdCommonFields& f) {
    w.write(1, f.screen_size_ratio_code ? 0U : 1U, "b_default_screen_size_ratio");
    if (f.screen_size_ratio_code) {
        w.write(5, u(*f.screen_size_ratio_code), "master_screen_size_ratio_code");
    }
    w.write(1, f.bed_object_chan_distribute ? 1U : 0U, "b_bed_object_chan_distribute");
    const bool additional =
        f.additional || f.trim || f.bed_render || f.headphone || f.add_data_bytes > 0;
    w.write(1, additional ? 1U : 0U, "b_additional_data");
    if (!additional) {
        return;
    }
    // The bits each part takes, to size add_data_bytes; bed_render_info() is
    // sent wherever headphone() is, which follows it.
    BitWriter trim = BitWriter::buffered();
    write_trim(trim, f.trim);
    BitWriter bed = BitWriter::buffered();
    write_bed_render_info(bed, f.bed_render);
    BitWriter hp = BitWriter::buffered();
    write_headphone(hp, f.headphone);
    std::size_t needed = trim.bit_position();
    if (f.bed_render || f.headphone) {
        needed += bed.bit_position();
    }
    if (f.headphone) {
        needed += hp.bit_position();
    }
    const std::uint64_t bytes = std::max(bytes_for(needed), u(std::max(f.add_data_bytes, 1)));
    w.write(1, bytes >= 2 ? 1U : 0U, "add_data_bytes_minus1");
    if (bytes >= 2) {
        w.write_variable_bits(2, bytes - 2, "add_data_bytes");
    }
    // Each element the decoder reads while bits of the budget are left.
    std::uint64_t left = bytes * 8;
    w.append(trim);
    left -= trim.bit_position();
    if (left > 0) {
        w.append(bed);
        left -= bed.bit_position();
    }
    if (left > 0) {
        w.append(hp);
        left -= hp.bit_position();
    }
    w.write_zero_run(left, "add_data");
}

void write_oamd_timing_data(BitWriter& w, const OamdTimingFields& f) {
    write_prefix(w, f.sample_offset_type, "oa_sample_offset_type");
    if (f.sample_offset_type == 0b10) {
        write_prefix(w, f.sample_offset_code, "oa_sample_offset_code");
    } else if (f.sample_offset_type == 0b11) {
        w.write(5, u(f.sample_offset), "oa_sample_offset");
    }
    w.write(3, f.blocks.size(), "num_obj_info_blocks");
    for (const OamdTimingFields::Block& b : f.blocks) {
        w.write(6, u(b.offset_factor), "block_offset_factor");
        w.write(2, u(b.ramp_code), "ramp_duration_code");
        if (b.ramp_code == 0b11) {
            w.write(1, b.use_table ? 1U : 0U, "b_use_ramp_table");
            if (b.use_table) {
                w.write(4, u(b.ramp_table), "ramp_duration_table");
            } else {
                w.write(11, u(b.ramp), "ramp_duration");
            }
        }
    }
}

void write_object_info_block(BitWriter& w, bool no_delta, bool dynamic,
                             const ObjectInfoBlockFields& b) {
    w.write(1, b.not_active ? 1U : 0U, "b_object_not_active");
    bool basic = false;
    if (!b.not_active) {
        if (no_delta) {
            basic = true;
        } else {
            w.write(1, b.basic_reuse ? 1U : 0U, "b_basic_info_reuse");
            basic = !b.basic_reuse;
        }
    }
    if (basic) {
        write_basic_info(w, b.basic);
    }
    if (!b.not_active && dynamic) {
        if (no_delta) {
            write_render_info(w, true, true, b.render_fields);
        } else {
            w.write(1, b.render == RenderStatus::kReuse ? 1U : 0U, "b_render_info_reuse");
            if (b.render != RenderStatus::kReuse) {
                const bool partial = b.render == RenderStatus::kPartReuse;
                w.write(1, partial ? 1U : 0U, "b_render_info_partial_reuse");
                write_render_info(w, !partial, false, b.render_fields);
            }
        }
    }
    w.write(1, b.add_table ? 1U : 0U, "b_add_table_data");
    if (b.add_table) {
        BitWriter md = BitWriter::buffered();
        write_add_per_object_md(md, b.not_active, dynamic, *b.add_table);
        const std::uint64_t bytes = std::min<std::uint64_t>(
            16, std::max(bytes_for(md.bit_position()), u(b.add_table->size_bytes)));
        w.write(4, bytes - 1, "add_table_data_size_minus1");
        w.append(md);
        w.write_zero_run(bytes * 8 - md.bit_position(), "add_table_data");
    }
}

void write_oamd_dyndata(BitWriter& w, std::span<const OamdObject> objects, int n_blocks,
                        bool iframe, std::span<const ObjectInfoBlockFields> blocks, bool multi,
                        const AltFields* alternative) {
    const auto per = static_cast<std::size_t>(n_blocks);
    for (std::size_t i = 0; i < objects.size(); ++i) {
        const OamdObject& object = objects[i];
        if (multi && object.ajoc_coded) {
            continue;
        }
        const bool dynamic = object.kind == OamdObjectKind::kDynamic && !object.lfe;
        for (std::size_t b = 0; b < per; ++b) {
            write_object_info_block(w, iframe && b == 0, dynamic, blocks[i * per + b]);
        }
    }
    if (multi || alternative == nullptr) {
        return;
    }
    const AltFields& alt = *alternative;
    w.write(1, alt.ducking_disabled ? 1U : 0U, "b_ducking_disabled");
    if (alt.sound_category < 3) {
        w.write(2, u(alt.sound_category), "object_sound_category");
    } else {
        w.write(2, 3, "object_sound_category");
        w.write_variable_bits(2, u(alt.sound_category - 3), "object_sound_category");
    }
    const std::size_t sets = alt.sets.size();
    if (sets < 3) {
        w.write(2, sets, "n_alt_data_sets");
    } else {
        w.write(2, 3, "n_alt_data_sets");
        w.write_variable_bits(2, sets - 3, "n_alt_data_sets");
    }
    for (const AltDataSetFields& set : alt.sets) {
        w.write(1, set.keep ? 1U : 0U, "b_keep");
        if (!set.keep) {
            std::size_t n_data_points = objects.size();
            if (!objects.empty() && objects[0].kind == OamdObjectKind::kIsf) {
                n_data_points = 1;
            } else {
                w.write(1, set.common ? 1U : 0U, "b_common_data");
                if (set.common) {
                    n_data_points = 1;
                }
            }
            for (std::size_t dp = 0; dp < n_data_points && dp < objects.size(); ++dp) {
                const OamdObject& object = objects[dp];
                const AltDataSetFields::Point& point = set.points.at(dp);
                w.write(1, point.gain ? 1U : 0U, "b_alt_gain");
                if (point.gain) {
                    w.write(6, u(*point.gain), "alt_obj_gain");
                }
                if (object.kind == OamdObjectKind::kDynamic && !object.lfe) {
                    w.write(1, point.position ? 1U : 0U, "b_alt_position");
                    if (point.position) {
                        w.write(6, u((*point.position)[0]), "alt_pos3D_X");
                        w.write(6, u((*point.position)[1]), "alt_pos3D_Y");
                        w.write(1, u((*point.position)[2]), "alt_pos3D_Z_sign");
                        w.write(4, u((*point.position)[3]), "alt_pos3D_Z");
                    }
                }
            }
        }
        w.write(1, set.additional ? 1U : 0U, "b_additional_data");
        if (set.additional) {
            // ext_prec_alt_pos(n_objs, b_keep, obj_type, b_lfe), 6.2.8.12.
            BitWriter ext = BitWriter::buffered();
            if (!set.keep) {
                for (std::size_t obj = 0; obj < objects.size(); ++obj) {
                    if (objects[obj].kind != OamdObjectKind::kDynamic || objects[obj].lfe) {
                        continue;
                    }
                    const bool present =
                        obj < set.ext_prec_alt_pos.size() && set.ext_prec_alt_pos[obj];
                    ext.write(1, present ? 1U : 0U, "b_ext_prec_alt_pos");
                    if (present) {
                        write_ext_prec_pos(ext, *set.ext_prec_alt_pos[obj]);
                    }
                }
            }
            const std::uint64_t bytes = bytes_for(ext.bit_position());
            w.write_variable_bits(2, bytes - 1, "skip_bits");
            w.append(ext);
            w.write_zero_run(bytes * 8 - ext.bit_position(), "skip_data");
        }
    }
}

void write_oamd_substream(BitWriter& w, const std::optional<OamdCommonFields>& common,
                          const std::optional<OamdTimingFields>& timing,
                          std::span<const OamdObject> objects, int n_blocks, bool oamd_ndot,
                          bool alternative, std::span<const ObjectInfoBlockFields> blocks) {
    w.write(1, common ? 1U : 0U, "b_oamd_common_data_present");
    if (common) {
        write_oamd_common_data(w, *common);
    }
    w.write(1, timing ? 1U : 0U, "b_oamd_timing_present");
    if (timing) {
        write_oamd_timing_data(w, *timing);
    }
    if (!alternative) {
        write_oamd_dyndata(w, objects, n_blocks, oamd_ndot, blocks, true, nullptr);
    }
    w.align();
}

}  // namespace ac4::detail
