#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "ac4/ac4.hpp"
#include "bit_reader.hpp"
#include "syntax/context.hpp"
#include "syntax/metadata.hpp"

// Part 2 clause 6.2.2.3 ac4_presentation_substream() and the presentation data
// it calls: advanced_de_data() (6.2.2.5), custom_dmx_data(), cdmx_parameters(),
// the tool_*() elements and loud_corr() (6.2.9). Semantics: Part 2 clauses
// 6.3.3.1 and 6.3.10.
//
// Syntax only, as in metadata.hpp: codes as sent, plus what the syntax itself
// assigns (gain_t2b_code = 7 and the like) and carries between frames.

namespace ac4::detail {

// Everything ac4_presentation_substream() needs from the table of contents.
// presentation_context_v1() below fills it; each field says how.
struct PresentationContext {
    // Part 2 clause 6.2.1.12 ac4_presentation_substream_info() of the
    // presentation: PresentationInfoV1::b_alternative and b_pres_ndot. A
    // presentation substream several presentations reference is read once
    // per frame (the trace contract), in the context of the first.
    bool b_alternative = false;
    bool b_pres_ndot = false;  // drc_frame(b_pres_ndot); Part 2 clause 4.5.2

    // Part 2 clause 6.3.3.1.13: the audio substreams of the presentation, in
    // the order of its ac4_sgi_specifier()s and then of each group's
    // substream infos - the sum over PresentationInfoV1::group_refs of
    // Toc::substream_groups[group_index].substreams.size(). One per
    // ac4_substream_info_chan/_ajoc/_obj(), however many frame_rate_factor
    // instances the info covers; HSF extension substreams are not audio
    // substreams of their own.
    int n_substreams_in_presentation = 0;

    // Part 2 clause 6.2.1.3 assigns it inside ac4_presentation_v1_info(): 1
    // for b_single_substream_group (PresentationInfoV1::presentation_config
    // unset); for presentation_config 0 to 4 the constants 2, 1, 2, 3, 2 -
    // one fewer than the ac4_sgi_specifier()s for 1 and 4, which carry a
    // dialogue enhancement group; for 5 the count read, which is
    // group_refs.size(); and 0 for 7 and above, where
    // presentation_config_ext_info() assigns nothing.
    int n_substream_groups = 0;

    // Part 2 clause 6.3.3.1.27 Pseudocode 25: superset_ch_mode() over the
    // ch_mode of every channel-coded substream of the presentation, -1 when
    // any is A-JOC or object coded (GroupSubstream::Kind kAjoc or kObj).
    // Pseudocode 25 loops sg < n_substream_groups; the groups taken are all
    // of group_refs, since n_substream_groups leaves out the dialogue
    // enhancement group of configs 1 and 4 while 6.3.3.1.29 to 6.3.3.1.31
    // define their helpers over "all substreams in the presentation".
    //
    // presentation_version 2 is not handled: V1.3.1 says only that a decoder
    // shall decode it. DEE's immersive-stereo encodes are version 2 with a
    // channel_mode of 0b1111000 (7.0: 3/4/0), yet their metadata() and
    // presentation substreams parse to the exact end of every frame only as
    // stereo (ch_mode 1). Whether to map or refuse is the caller's decision.
    int pres_ch_mode = -1;

    // Part 2 clause 6.3.3.1.28 Table 71 and Pseudocode 26, over the same
    // substreams: ch_mode_core 5 for a channel-coded ch_mode 11 or 13, 6 for
    // 12 or 14, 3 or 4 (by b_lfe) for an A-JOC substream with b_static_dmx, -1
    // otherwise, combined by superset_ch_mode_core(); -1 when any substream is
    // an A-JOC one without b_static_dmx or an object one; then -1 if equal to
    // pres_ch_mode.
    int pres_ch_mode_core = -1;

    // Part 2 clause 6.3.3.1.29: OR of b_4_back_channels_present over the
    // substreams that carry it (ChannelSubstreamInfo::original_content).
    bool b_pres_4_back_channels_present = false;

    // Part 2 Table 72 over the same substreams' top_channels_present: 2 when
    // any is 3, else 1 when any is 1 or 2, else 0.
    int pres_top_channel_pairs = 0;

    // Part 2 clause 6.3.3.1.31: channel_mode_contains_Lfe() of pres_ch_mode
    // when it is 0 or more, else pres_ch_mode_core 4 or 6.
    bool b_pres_has_lfe = false;

    // For drc_frame()'s nr_drc_subframes: Part 1 Table 83 by
    // Toc::frame_rate_index at 48 kHz, Table 84 at 44.1 kHz (index 13 only);
    // 0 for a reserved index. drc_frame()'s nr_drc_channels comes from
    // pres_ch_mode.
    int frame_len_base = 2048;
};

[[nodiscard]] PresentationContext presentation_context_v1(
    const ac4::Toc& toc, const ac4::PresentationInfoV1& presentation);

// Part 2 clause 6.3.3.1.27: the lowest ch_mode holding every channel of both
// arguments; -1 is the identity, and superset(0, 1) is 1 as the clause says. -1
// for two modes no single ch_mode holds (9.X.4 with 22.2).
[[nodiscard]] int superset_ch_mode(int a, int b) noexcept;

// Part 2 clause 6.3.3.1.28, for ch_mode_core values (Table 71: 3 = 5.0, 4 =
// 5.1, 5 = 5.0.2, 6 = 5.1.2).
[[nodiscard]] int superset_ch_mode_core(int a, int b) noexcept;

// --- Part 2 clause 6.2.2.3 ---------------------------------------------------

struct PresentationTarget {
    int target_level = 0;
    int target_device_category = 0;  // Table 67; the first bit read (index 0) is the MSB
    std::optional<int> reserved_bits;  // b_tdc_extension
    std::optional<int> max_ducking_depth;
    std::optional<int> loud_corr_target;
    // One per substream of the presentation (PresentationContext
    // n_substreams_in_presentation order): alt_data_set_index when b_active,
    // unset when not.
    std::vector<std::optional<std::uint64_t>> alt_data_set_index;
};

// Part 2 clause 6.2.2.5's configuration fields.
struct AdvancedDeConfig {
    int advanced_de_compr_tc_attack = 0;
    int advanced_de_compr_tc_release = 0;
    int advanced_de_compr_ratio = 0;
};

struct AdvancedDeData {
    bool b_advanced_de_config_present = false;
    // The configuration in force: the one read, or the last one read since the
    // last I-frame. Unset for the defaults - the syntax comment says fields
    // not sent in an I-frame "will be reset to their defaults", and no clause
    // gives them.
    std::optional<AdvancedDeConfig> config;
    // Part 2 clause 6.3.3.1.17g: an "integer" ranging -32 to 31, read as 6-bit
    // two's complement.
    int advanced_de_compr_thresh = 0;
    int advanced_de_compr_gain = 0;
};

// One cdmx_parameters() (Part 2 clause 6.2.9.3). Each field is set when the
// tool that carries it was read, with what the bitstream sent or what the
// syntax assigned (the gain_t2b_code = 7 and gain_t2e_code = 7 lines); unset
// fields take Table 130's defaults, a later phase's business, as is the bs 1
// to out 4 exception of clause 6.3.10.3.10.
struct CdmxParameters {
    int out_ch_config = 0;  // Table 127
    std::optional<bool> b_put_screen_to_c;
    std::optional<int> gain_f1_code;
    std::optional<int> gain_f2_code;
    std::optional<int> gain_b_code;
    std::optional<int> gain_t1_code;
    std::optional<bool> b_top_front_to_front;
    std::optional<bool> b_top_front_to_side;
    std::optional<bool> b_top_back_to_front;
    std::optional<bool> b_top_back_to_side;
    std::optional<bool> b_top_to_front;
    std::optional<bool> b_top_to_side;
    std::optional<int> gain_t2a_code;
    std::optional<int> gain_t2b_code;
    std::optional<int> gain_t2c_code;
    std::optional<int> gain_t2d_code;
    std::optional<int> gain_t2e_code;
    std::optional<int> gain_t2f_code;
};

// Part 2 clause 6.2.9.2.
struct CustomDmxData {
    int bs_ch_config = -1;  // Table 126, as the syntax computes it; -1 when none applies
    bool b_cdmx_data_present = false;
    int n_cdmx_configs = 0;
    std::array<CdmxParameters, 4> cdmx{};  // n_cdmx_configs_minus1 is 2 bits
    std::optional<StereoDmxCoeff> stereo_dmx_coeff;  // without the two dmx loudness corrections
};

// Part 2 clause 6.2.9.1: each correction present when its b_loud_comp (or
// b_loro_loud_comp, b_ltrt_loud_comp) was 1.
struct LoudCorr {
    bool b_obj_loud_corr = false;
    bool b_corr_for_immersive_out = false;
    std::optional<int> loro_dmx_loud_corr;
    std::optional<int> ltrt_dmx_loud_corr;
    std::optional<int> loud_corr_5_X;
    std::optional<int> loud_corr_5_X_2;
    std::optional<int> loud_corr_7_X;
    std::optional<int> loud_corr_7_X_4;
    std::optional<int> loud_corr_7_X_2;
    std::optional<int> loud_corr_5_X_4;
    std::optional<int> loud_corr_core_5_X_2;
    std::optional<int> loud_corr_core_5_X;
    std::optional<int> loud_corr_core_loro;  // sent as a pair with loud_corr_core_ltrt
    std::optional<int> loud_corr_core_ltrt;
    std::optional<int> loud_corr_9_X_4;
};

struct PresentationSubstream {
    // b_alternative only.
    bool b_name_present = false;
    // name_len bytes of presentation_name: one chunk of a name that may span
    // frames (Part 2 clause 6.3.3.1.4); assembling chunks is not syntax.
    std::vector<std::uint8_t> presentation_name;
    std::vector<PresentationTarget> targets;

    bool b_additional_data = false;
    std::uint64_t add_data_bytes = 0;
    bool immersive_audio_indicator = false;
    std::optional<bool> b_oamd_common_timing;  // pres_ch_mode -1
    std::optional<AdvancedDeData> advanced_de_data;  // b_advanced_de_data_present
    std::uint64_t add_data_bits = 0;  // width of the add_data run, not interpreted

    int dialnorm_bits = 0;
    std::optional<FurtherLoudnessInfo> further_loudness_info;
    std::uint64_t drc_metadata_size = 0;
    DrcFrame drc{};

    // n_substream_groups > 1 only.
    bool b_substream_group_gains_present = false;
    bool b_keep = false;
    // With b_substream_group_gains_present: the n_substream_groups sg_gain
    // codes in force - read, or with b_keep the last ones read (0, which is
    // 0 dB, before any were: Part 2 clause 6.3.3.1.23).
    std::vector<int> sg_gain;

    bool b_associated = false;
    std::optional<int> scale_main;
    std::optional<int> scale_main_centre;
    std::optional<int> scale_main_front;
    bool b_associate_is_mono = false;
    std::optional<int> pan_associated;

    CustomDmxData custom_dmx_data{};
    LoudCorr loud_corr{};
};

// Per presentation substream, carried between frames.
struct PresentationSubstreamState {
    DrcState drc{};
    std::vector<int> sg_gain;  // the last sg_gain codes read; empty before any
    // The last advanced DE configuration since the last I-frame. An I-frame
    // without advanced_de_data() disables A-DE and one without the
    // configuration resets it (the two syntax comments of 6.2.2.3 and
    // 6.2.2.5), so either clears it.
    std::optional<AdvancedDeConfig> advanced_de_config;
};

// From the first bit of the substream through its final byte_align. Checks
// drc_metadata_size against the bits drc_frame() took and that add_data_bytes
// holds the fields read inside it.
[[nodiscard]] ParseResult parse_presentation_substream(BitReader& r,
                                                       const PresentationContext& ctx,
                                                       PresentationSubstreamState& state,
                                                       PresentationSubstream& out);

}  // namespace ac4::detail
