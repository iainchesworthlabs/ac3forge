#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "bit_writer.hpp"

// Object audio metadata, written: ETSI TS 103 190-2 V1.3.1 clause 6.2.8 -
// oamd_common_data() with trim(), bed_render_info() and headphone(),
// oamd_timing_data(), oamd_dyndata_single() with its alternative sets,
// oamd_dyndata_multi(), object_info_block() with object_basic_info(),
// object_render_info() and add_per_object_md() - and clause 6.2.2.4's
// oamd_substream(). Transcribed for writing, separate from the decoder's reader
// (src/ac4dec/src/syntax/oamd.cpp) and the Python parser; the traces agree
// record for record.
//
// The fields are the syntax's codes. A prefix code (oa_sample_offset_type and
// oa_sample_offset_code, basic_info_md, object_gain_code) is one record of the
// bits written, valued at them, as the decoder records it (src/ac4dec/
// ERRATA.md, "Prefix codes in the trace"); a flag array (group_zone_flag[],
// trim_balance_presence[], ext_prec_pos_presence[]) is one field, its highest
// index first.

namespace ac4::detail {

// Part 2 clause 6.2.1.10's object types.
enum class OamdObjectKind : std::uint8_t { kBed, kDynamic, kIsf };

// One object of a substream or a group, as the table of contents lists it:
// obj_type[], b_lfe[] and b_ajoc_coded[].
struct OamdObject {
    OamdObjectKind kind = OamdObjectKind::kDynamic;
    bool lfe = false;
    bool ajoc_coded = false;
};

// trim(), 6.2.8.9: global_trim_mode 0b10 sends a configuration per trim
// configuration, each default, disabled or with the values
// trim_balance_presence[] names ([4] its first bit).
struct TrimConfigFields {
    bool default_trim = true;
    bool disable = false;
    int presence = 0;  // trim_balance_presence[], [4] first
    int centre = 0;
    int surround = 0;
    int height = 0;
    int tb_sign = 0;
    int tb_amount = 0;
    int lis_sign = 0;
    int lis_amount = 0;
};

struct TrimFields {
    int warp_mode = 0;
    int global_trim_mode = 0;
    std::array<TrimConfigFields, 9> configs{};  // with global_trim_mode 0b10
};

// A gain tool of bed_render_info(), 6.2.9.9 and 6.2.9.10 and 6.2.8.13 to
// 6.2.8.16: to front with gain a, else (the _b forms) to side with gain b or
// neither with gain c; the forms without _b send gain b where not to front.
struct GainToolFields {
    bool to_front = true;
    bool to_side = false;
    int gain = 0;
};

// bed_render_info(), 6.2.8.8, with stereo_dmx_coeff() (6.2.8.8a).
struct BedRenderFields {
    struct StereoDmx {
        int loro_centre = 0;
        int loro_surround = 0;
        std::optional<std::array<int, 2>> ltrt;  // ltrt_centre_mixgain, ltrt_surround_mixgain
        std::optional<int> lfe;                  // lfe_mixgain
        int preferred_dmx_method = 0;
    };
    std::optional<StereoDmx> stereo_dmx;
    // b_cdmx_data_present with these.
    bool cdmx = false;
    std::optional<int> gain_w_to_f;
    std::optional<int> gain_b4_to_b2;
    bool tm_ch_present = false;
    std::optional<GainToolFields> t2_to_f_s_b;
    std::optional<GainToolFields> t2_to_f_s;
    bool tb_ch_present = false;
    std::optional<GainToolFields> tb_to_f_s_b;
    std::optional<GainToolFields> tb_to_f_s;
    bool tf_ch_present = false;
    std::optional<GainToolFields> tf_to_f_s_b;
    std::optional<GainToolFields> tf_to_f_s;
    std::optional<int> gain_tfb_to_tm;  // with tb or tf present
};

struct HeadphoneFields {
    int operation_mode = 1;               // hp_operation_mode
    bool head_track_disable_all = false;  // with modes 0b001 and 0b010
};

// oamd_common_data(), 6.2.8.1. With any of trim, bed_render and headphone
// (or add_data_bytes above what they need), b_additional_data: its bytes are
// the fewest that hold what is sent, or add_data_bytes where that is more,
// and each element the decoder then reads with bits left of the budget is
// sent, absent where it is not given.
struct OamdCommonFields {
    std::optional<int> screen_size_ratio_code;  // unset: b_default_screen_size_ratio
    bool bed_object_chan_distribute = false;
    bool additional = false;
    std::optional<TrimFields> trim;
    std::optional<BedRenderFields> bed_render;
    std::optional<HeadphoneFields> headphone;
    int add_data_bytes = 0;
};

// oamd_timing_data(), 6.2.8.2.
struct OamdTimingFields {
    int sample_offset_type = 0;  // 0b0, 0b10 or 0b11 (Table 92)
    int sample_offset_code = 0;  // 0b0, 0b10 or 0b11 (Table 93), with type 0b10
    int sample_offset = 0;       // 0 to 31, with type 0b11
    struct Block {
        int offset_factor = 0;  // block_offset_factor, 0 to 63
        int ramp_code = 0;      // ramp_duration_code
        bool use_table = true;  // b_use_ramp_table, with ramp_code 0b11
        int ramp_table = 0;     // ramp_duration_table
        int ramp = 0;           // ramp_duration, 0 to 2 047
    };
    std::vector<Block> blocks;  // num_obj_info_blocks of them, 0 to 7
};

// object_basic_info(), 6.2.8.6.
struct ObjectBasicFields {
    bool defaults = true;  // b_default_basic_info_md
    int info_md = 0b0;     // basic_info_md: 0b0, 0b10 or 0b11
    int gain_code = 0b0;   // object_gain_code: 0b0, 0b10 or 0b11
    int gain_value = 0;    // object_gain_value
    int priority = 31;     // object_priority_code
};

// object_render_info(), 6.2.8.7.
struct ObjectRenderFields {
    // With PART_REUSE, which of the three groups are sent; ALL_NEW sends all.
    bool otherprops = true;
    bool zone = true;
    bool position = true;
    bool diff_pos = false;      // b_diff_pos_coding, not with b_no_delta
    std::array<int, 3> diff{};  // diff_pos3D_X, Y and Z, -4 to 3
    int x = 31;                 // pos3D_X
    int y = 0;                  // pos3D_Y
    int z_sign = 1;             // pos3D_Z_sign
    int z = 0;                  // pos3D_Z
    bool zone_defaults = true;  // b_grouped_zone_defaults
    int zone_flags = 0;         // group_zone_flag[], [2] first
    int zone_mask = 0;
    bool other_defaults = true;  // b_grouped_other_defaults
    int other_mask = 0;          // group_other_mask
    int width_mode = 0;
    int width = 0;                   // object_width_code
    std::array<int, 3> width_xyz{};  // object_width_X_code, _Y_ and _Z_
    int screen_factor = 0;           // object_screen_factor_code
    int depth_factor = 0;            // object_depth_factor
    bool at_infinity = false;        // b_obj_at_infinity
    int distance = 0;                // obj_distance_factor_code
    int div_mode = 0;                // object_div_mode
    int div_table = 0;
    int div_code = 0;
};

// ext_prec_pos(), 6.2.8.11.
struct ExtPrecPosFields {
    int presence = 0;  // ext_prec_pos_presence[], [2] (X) first
    std::array<int, 3> xyz{};
};

// add_per_object_md(), 6.2.8.10, in add_table_data_size_minus1 + 1 bytes: the
// fewest that hold it, or `size_bytes` where that is more (1 to 16).
struct AddPerObjectFields {
    bool trim_disable = false;
    std::optional<ExtPrecPosFields> ext_prec_pos;  // for an active dynamic object
    // hp_render_mode_obj and b_head_track_disable_obj.
    std::optional<std::pair<int, bool>> headphone;
    int size_bytes = 0;
};

// The status an object_info_block() codes its render information with where
// it is neither b_no_delta nor for an object without render information.
enum class RenderStatus : std::uint8_t { kAllNew, kReuse, kPartReuse };

// object_info_block(b_no_delta, b_dynamic_object), 6.2.8.5.
struct ObjectInfoBlockFields {
    bool not_active = false;
    bool basic_reuse = false;  // b_basic_info_reuse, not with b_no_delta
    ObjectBasicFields basic;
    RenderStatus render = RenderStatus::kAllNew;
    ObjectRenderFields render_fields;
    std::optional<AddPerObjectFields> add_table;  // b_add_table_data
};

// One alternative data set of oamd_dyndata_single(), 6.2.8.3.
struct AltDataSetFields {
    bool keep = false;
    bool common = false;  // b_common_data, for objects other than ISF
    struct Point {
        std::optional<int> gain;                     // alt_obj_gain
        std::optional<std::array<int, 4>> position;  // alt_pos3D_X, _Y_, _Z_sign, _Z
    };
    std::vector<Point> points;
    // b_additional_data with ext_prec_alt_pos() per dynamic object (nothing
    // for another), in skip_bits bytes: the fewest that hold it.
    bool additional = false;
    std::vector<std::optional<ExtPrecPosFields>> ext_prec_alt_pos;
};

struct AltFields {
    bool ducking_disabled = false;
    int sound_category = 0;
    std::vector<AltDataSetFields> sets;
};

void write_oamd_common_data(BitWriter& w, const OamdCommonFields& fields);

void write_oamd_timing_data(BitWriter& w, const OamdTimingFields& fields);

void write_object_info_block(BitWriter& w, bool no_delta, bool dynamic,
                             const ObjectInfoBlockFields& block);

// The object_info_blocks of oamd_dyndata_single() (`multi` false) or
// oamd_dyndata_multi() (true, which passes over A-JOC coded objects): `blocks`
// holds n_blocks per object of `objects`, object by object; then, with
// `alternative`, oamd_dyndata_single()'s alternative sets.
void write_oamd_dyndata(BitWriter& w, std::span<const OamdObject> objects, int n_blocks,
                        bool iframe, std::span<const ObjectInfoBlockFields> blocks, bool multi,
                        const AltFields* alternative);

// oamd_substream(), 6.2.2.4, ending byte-aligned.
void write_oamd_substream(BitWriter& w, const std::optional<OamdCommonFields>& common,
                          const std::optional<OamdTimingFields>& timing,
                          std::span<const OamdObject> objects, int n_blocks, bool oamd_ndot,
                          bool alternative, std::span<const ObjectInfoBlockFields> blocks);

}  // namespace ac4::detail
