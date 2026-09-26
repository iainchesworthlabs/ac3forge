#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "ac4/ac4.hpp"
#include "bit_reader.hpp"
#include "syntax/context.hpp"

// Object audio metadata, ETSI TS 103 190-2 V1.3.1 clause 6.2.8:
// oamd_common_data(), oamd_timing_data(), oamd_dyndata_single(),
// oamd_dyndata_multi() and the object_info_block() they repeat, with
// object_basic_info(), object_render_info() and add_per_object_md(); and the
// OAMD substream, oamd_substream() (6.2.2.4), which carries a second
// oamd_common_data() of its own. The table of contents' oamd_common_data()
// (in ac4_substream_info_ajoc()) is the inspector's (src/ac4); this is the
// decoder's own transcription, for the substreams, with a record per element.
//
// Syntax only: fields hold the codes the bitstream sends. What they mean - a
// position, a gain, when an update takes effect - is src/ac4dec/src/pcm/
// objects.cpp's, by clause 6.3.9 and Annex F.

namespace ac4::detail {

// The most object_info_block()s an object carries in one frame:
// num_obj_info_blocks is 3 bits (6.2.8.2).
inline constexpr int kMaxObjInfoBlocks = 7;

// The most objects one OAMD portion describes. Part 2 Table 55 allows 17
// reconstructed A-JOC objects and an LFE at md_compat 3, and leaves md_compat
// 7 unrestricted; a substream or group with more than this is refused as
// unsupported rather than read into storage without a bound.
inline constexpr int kMaxOamdObjects = 64;

// obj_type (6.2.1.10 and 6.2.1.11): a bed object, a dynamic object, or one of
// the intermediate spatial format's.
enum class ObjType : std::uint8_t { kBed, kDyn, kIsf };

// What the table of contents says of one object an OAMD portion describes, in
// the order the portion lists them (src/ac4dec/ERRATA.md, "The objects of an
// A-JOC substream" and "The objects of a direct-coded substream").
struct OamdObjectType {
    ObjType type = ObjType::kDyn;
    bool lfe = false;
    bool ajoc_coded = false;
};

// A fixed-capacity list of them, which a SubstreamContext carries by value.
struct OamdObjectList {
    std::array<OamdObjectType, kMaxOamdObjects> objects{};
    int count = 0;

    [[nodiscard]] const OamdObjectType& operator[](int i) const noexcept {
        return objects[static_cast<std::size_t>(i)];
    }
    void push(OamdObjectType type) noexcept {
        if (count < kMaxOamdObjects) {
            objects[static_cast<std::size_t>(count)] = type;
        }
        ++count;  // counted past the capacity, so the caller can refuse
    }
};

// --- 6.2.8.2 oamd_timing_data -------------------------------------------------

struct OamdTimingData {
    // oa_sample_offset_type's prefix code as read, 0b0, 0b10 or 0b11 (Table
    // 92), and sample_offset from it (Tables 92 and 93, or oa_sample_offset).
    int oa_sample_offset_type = 0;
    int sample_offset = 0;
    int num_obj_info_blocks = 0;
    std::array<int, kMaxObjInfoBlocks> block_offset_factor{};
    // ramp_duration_code, and the ramp_duration it gives in samples (Tables
    // 94 and 95, or ramp_duration).
    std::array<int, kMaxObjInfoBlocks> ramp_duration_code{};
    std::array<int, kMaxObjInfoBlocks> ramp_duration{};
};

[[nodiscard]] ParseResult parse_oamd_timing_data(BitReader& r, OamdTimingData& out);

// --- 6.2.8.6 object_basic_info ------------------------------------------------

struct ObjectBasicInfo {
    bool b_default_basic_info_md = true;
    // basic_info_md's prefix code (Table 100): 0b0, 0b10 or 0b11.
    int basic_info_md = 0;
    // object_gain_code's prefix code (Table 101), where it was read.
    std::optional<int> object_gain_code;
    std::optional<int> object_gain_value;
    std::optional<int> object_priority_code;
};

// --- 6.2.8.7 object_render_info -----------------------------------------------

struct ObjectRenderInfo {
    bool b_obj_render_position_present = false;
    bool b_obj_render_zone_present = false;
    bool b_obj_render_otherprops_present = false;
    bool b_diff_pos_coding = false;
    // diff_pos3D_*: 3 bits of two's complement (6.3.9.8.3), -4 to 3.
    int diff_pos3d_x = 0;
    int diff_pos3d_y = 0;
    int diff_pos3d_z = 0;
    int pos3d_x = 0;
    int pos3d_y = 0;
    int pos3d_z_sign = 0;
    int pos3d_z = 0;
    bool b_grouped_zone_defaults = true;
    int group_zone_flag = 0;  // group_zone_flag[2] is its most significant bit
    std::optional<int> zone_mask;
    bool b_grouped_other_defaults = true;
    int group_other_mask = 0;
    std::optional<int> object_width_mode;
    std::optional<int> object_width_code;
    std::optional<int> object_width_x_code;
    std::optional<int> object_width_y_code;
    std::optional<int> object_width_z_code;
    std::optional<int> object_screen_factor_code;
    std::optional<int> object_depth_factor;
    std::optional<bool> b_obj_at_infinity;
    std::optional<int> object_distance_factor_code;
    std::optional<int> object_div_mode;
    std::optional<int> object_div_table;
    std::optional<int> object_div_code;
};

// --- 6.2.8.10 and 6.2.8.11 add_per_object_md and ext_prec_pos ------------------

struct ExtPrecPos {
    int presence = 0;  // ext_prec_pos_presence[], [2] its most significant bit
    std::optional<int> ext_prec_pos3d_x;
    std::optional<int> ext_prec_pos3d_y;
    std::optional<int> ext_prec_pos3d_z;
};

struct AddPerObjectMd {
    bool b_obj_trim_disable = false;
    std::optional<ExtPrecPos> ext_prec_pos;  // b_ext_prec_pos
    bool b_headphone = false;
    int hp_render_mode_obj = 0;
    bool b_head_track_disable_obj = false;
};

// --- 6.2.8.5 object_info_block ------------------------------------------------

// object_basic_info_status and object_render_info_status (Tables 98 and 99).
enum class InfoStatus : std::uint8_t { kDefault, kAllNew, kReuse, kPartReuse };

struct ObjectInfoBlock {
    bool b_object_not_active = false;
    InfoStatus basic_status = InfoStatus::kDefault;
    InfoStatus render_status = InfoStatus::kDefault;
    ObjectBasicInfo basic{};    // read where basic_status is kAllNew
    ObjectRenderInfo render{};  // read where render_status is kAllNew or kPartReuse
    bool b_add_table_data = false;
    int add_table_data_size = 0;  // add_table_data_size_minus1 + 1, bytes
    std::optional<AddPerObjectMd> per_object;
    std::uint64_t add_table_data_bits = 0;  // the width of add_table_data after it
};

// --- 6.2.8.3 oamd_dyndata_single's alternative sets ----------------------------

// One data point of an alternative set: alt_obj_gain and alt_pos3D_*, where
// sent.
struct AltDataPoint {
    std::optional<int> alt_obj_gain;
    bool b_alt_position = false;
    int alt_pos3d_x = 0;
    int alt_pos3d_y = 0;
    int alt_pos3d_z_sign = 0;
    int alt_pos3d_z = 0;
};

struct AltDataSet {
    bool b_keep = false;
    bool b_common_data = false;
    std::vector<AltDataPoint> points;  // n_data_points of them
    bool b_additional_data = false;
    // ext_prec_alt_pos(): per object, where b_ext_prec_alt_pos set it.
    std::vector<std::optional<ExtPrecPos>> ext_prec_alt_pos;
    std::uint64_t skip_bits = 0;  // the skip_data after it
};

// oamd_dyndata_single() and oamd_dyndata_multi(): each object's blocks, and
// for an alternative presentation the alternative sets.
struct OamdDynData {
    int n_objs = 0;
    int n_blocks = 0;
    // blocks[obj * n_blocks + b]; an object an oamd_dyndata_multi() passes
    // over (b_ajoc_coded) has default-constructed blocks.
    std::vector<ObjectInfoBlock> blocks;
    // b_alternative (oamd_dyndata_single() only).
    bool alternative = false;
    bool b_ducking_disabled = false;
    std::uint64_t object_sound_category = 0;
    std::uint64_t n_alt_data_sets = 0;
    std::vector<AltDataSet> alt_sets;

    [[nodiscard]] const ObjectInfoBlock& block(int obj, int b) const noexcept {
        return blocks[static_cast<std::size_t>(obj * n_blocks + b)];
    }
};

// oamd_dyndata_single(n_objs, n_blocks, b_iframe, b_alternative, obj_type[],
// b_lfe[]) over `objects`.
[[nodiscard]] ParseResult parse_oamd_dyndata_single(BitReader& r, const OamdObjectList& objects,
                                                    int n_blocks, bool b_iframe, bool b_alternative,
                                                    OamdDynData& out);

// oamd_dyndata_multi(n_objs, n_blocks, b_iframe, obj_type[], b_lfe[],
// b_ajoc_coded[]) over `objects`, a whole substream group's.
[[nodiscard]] ParseResult parse_oamd_dyndata_multi(BitReader& r, const OamdObjectList& objects,
                                                   int n_blocks, bool b_iframe, OamdDynData& out);

// --- 6.2.8.1 oamd_common_data -------------------------------------------------

// The decoder's reading of the element, in the inspector's types (ac4.hpp),
// with the add_data after trim(), bed_render_info() and headphone(): where
// the budget add_data_bytes sets runs out, the elements it has no room for
// are not read, as the syntax says.
struct OamdCommonData {
    ac4::OamdCommonData data{};
    std::uint64_t add_data_bits = 0;  // the width of add_data, read and not interpreted
};

[[nodiscard]] ParseResult parse_oamd_common_data(BitReader& r, OamdCommonData& out);

// --- 6.2.2.4 oamd_substream ---------------------------------------------------

struct OamdSubstream {
    std::optional<OamdCommonData> common;  // b_oamd_common_data_present
    std::optional<OamdTimingData> timing;  // b_oamd_timing_present
    // Where b_alternative is 0: the group's objects' dynamic data.
    std::optional<OamdDynData> dyndata;
};

// What an oamd_substream() needs from outside it: the objects of the
// substream groups that reference it, in the order oamd_dyndata_multi() lists
// them (6.3.9.5, "all object essences present over all audio substreams of
// the according substream group in the order of bitstream presence"), and
// the timing that applies where this frame's substream sends none.
struct OamdSubstreamContext {
    OamdObjectList objects{};
    bool b_oamd_ndot = false;  // the b_iframe of oamd_dyndata_multi()
    bool b_alternative = false;
    // num_obj_info_blocks of the last oamd_timing_data() the group sent,
    // where it has sent one.
    std::optional<int> carried_blocks;
};

// The whole substream, its final byte_align included.
[[nodiscard]] ParseResult parse_oamd_substream(BitReader& r, const OamdSubstreamContext& ctx,
                                               OamdSubstream& out);

// --- Shared with the audio data ------------------------------------------------

// ext_prec_pos(), 6.2.8.11.
[[nodiscard]] ParseResult parse_ext_prec_pos(BitReader& r, ExtPrecPos& out);

// What an object substream's OAMD needs besides its SubstreamContext: the
// objects of each OAMD portion, as the table of contents describes them, and
// the timing the substream group's OAMD substream sent, which applies where
// the substream sends none (src/ac4dec/ERRATA.md, "Which oamd_timing_data()
// applies").
struct ObjectAudioContext {
    OamdObjectList dmx;      // an A-JOC substream's first portion (b_static_dmx 0)
    OamdObjectList umx;      // its second
    OamdObjectList objects;  // a direct-coded substream's
    std::optional<int> group_blocks;
};

}  // namespace ac4::detail
