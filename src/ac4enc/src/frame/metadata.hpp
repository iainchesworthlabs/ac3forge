#pragma once

#include <array>
#include <optional>
#include <vector>

#include "ac4enc/encoder.hpp"
#include "bit_writer.hpp"

// The metadata a frame carries beside the audio, as the codes the syntax
// sends, and the writers for it: further_loudness_info() (ETSI TS 103 190-2
// V1.3.1 clause 6.2.7.3), drc_frame() (TS 103 190-1 V1.4.1 Tables 70 to 75),
// custom_dmx_data() and loud_corr() (Part 2 clauses 6.2.9.2 and 6.2.9.1) and
// dialog_enhancement() (Part 2 clause 6.2.7.5, with Part 1 Table 78's
// de_data()). A transcription of the syntax tables separate from the
// decoder's reader.

namespace ac4::detail {

// Part 1 clause 4.3.14: dialogue enhancement's parameter bands.
inline constexpr std::size_t kDeBands = 8;

// further_loudness_info()'s fields (Part 1 clause 4.3.12.3): the 11-bit
// loudness values are floor(value x 10 + 1/2) + 1 024, lra floor(LU x 10 + 1/2).
struct LoudnessCodes {
    int loud_prac_type = 0;
    std::optional<int> dialgate_prac_type;  // b_loudcorr_dialgate, where there is a practice
    bool loudcorr_type = false;
    std::optional<int> loudrelgat;
    std::optional<int> loudspchgat;
    int speech_dialgate_prac_type = 0;
    std::optional<int> max_loudstrm3s;
    std::optional<int> max_truepk;
    std::optional<int> lra;
    int lra_prac_type = 1;
    std::optional<int> max_loudmntry;
};

// drc_compression_curve()'s fields (Table 73), which Table 166 turns into a
// curve's control points and Table 167's formulas into time constants.
struct CurveCodes {
    int lev_nullband_low = 0;
    int lev_nullband_high = 0;
    int gain_max_boost = 0;
    int lev_max_boost = 0;
    int nr_boost_sections = 0;
    int gain_section_boost = 0;
    int lev_section_boost = 0;
    int gain_max_cut = 0;
    int lev_max_cut = 0;
    int nr_cut_sections = 0;
    int gain_section_cut = 0;
    int lev_section_cut = 0;
    bool tc_default = true;
    int tc_attack = 0;
    int tc_release = 0;
    int tc_attack_fast = 0;
    int tc_release_fast = 0;
    bool adaptive_smoothing = false;
    int attack_threshold = 0;
    int release_threshold = 0;
};

// One drc_decoder_mode_config() (Table 72).
struct DrcModeCodes {
    int id = 0;
    int output_level_from = 0;  // for ids 4 to 7: -L_out,min
    int output_level_to = 0;    // -L_out,max
    std::optional<int> repeat_id;
    bool default_profile = true;
    std::optional<CurveCodes> curve;  // where the mode takes neither of the above
};

// drc_config() (Table 71).
struct DrcCodes {
    std::vector<DrcModeCodes> modes;
    int eac3_profile = 2;
};

// custom_dmx_data()'s stereo coefficients and loud_corr()'s corrections for
// them (Part 2 clauses 6.2.9.2 and 6.2.9.1; Part 1 Tables 149, 149a and 150).
struct DownmixCodes {
    int loro_centre_mixgain = 4;
    int loro_surround_mixgain = 4;
    std::optional<std::array<int, 2>> ltrt_mixgain;  // b_ltrt_mixinfo: centre, surround
    std::optional<int> lfe_mixgain;
    int preferred_dmx_method = 1;
    std::optional<int> loro_dmx_loud_corr;
    std::optional<int> ltrt_dmx_loud_corr;
};

// de_config() (Table 77), and de_ms_proc_flag, which the channel-independent
// method sets for L and R's Mid.
struct DeConfigCodes {
    int method = 0;
    int max_gain = 2;
    int channel_config = 1;  // Table 171: L, R and C as bits 4, 2 and 1
    bool mid = false;
};

// One frame's dialogue enhancement parameters: Table 209's indices (Table
// 210's in the cross-channel method), per processed channel in
// de_channel_config's order, the Mid's alone with de_ms_proc_flag, and per
// band; and in the cross-channel method de_mix_coef1_idx and
// de_mix_coef2_idx (Table 172).
struct DeFrameParameters {
    std::array<std::array<int, kDeBands>, 3> par{};
    std::array<int, 2> mix{};
};

// What the stream's frames carry beside dialnorm: each where it is configured.
struct StreamMetadata {
    std::optional<LoudnessCodes> loudness;
    std::optional<DrcCodes> drc;
    std::optional<DownmixCodes> downmix;
    std::optional<DeConfigCodes> de;
};

// The channels de_channel_config names (Table 171).
[[nodiscard]] int de_channel_count(int channel_config) noexcept;

// Table 162's default profile as Table 166's curve parameters, as DEE sends a
// mode's profile that is not the stream's default one.
[[nodiscard]] CurveCodes curve_codes(DrcProfile profile) noexcept;

// The stream's metadata as codes, or nothing where a value is not one the
// syntax can send. `ch_mode` is Part 1 Table 88's; the downmix's values are
// sent for 5.X and 7.X alone, and dialogue enhancement's channels must be
// ones the channel mode has.
[[nodiscard]] std::optional<StreamMetadata> resolve_metadata(const EncoderConfig& config,
                                                             int ch_mode);

// further_loudness_info(1, 1) in a presentation substream: the header in
// every frame, the values in I-frames, as DEE's streams have it.
void write_further_loudness_info(BitWriter& w, const LoudnessCodes& codes, bool iframe);

// drc_frame(b_iframe): drc_config() and drc_data() in I-frames, nothing
// (b_drc_present 0) in the others, which keep the configuration.
void write_drc_frame(BitWriter& w, const DrcCodes* codes, bool iframe);

// custom_dmx_data() and loud_corr() for a channel-based presentation of
// `ch_mode`: the stereo coefficients and their corrections in I-frames.
void write_downmix(BitWriter& w, int ch_mode, bool has_lfe, const DownmixCodes* codes, bool iframe);

// dialog_enhancement(b_iframe): de_config() in I-frames and each frame's
// parameters, differential in frequency in I-frames and in time against
// `previous` in the others, or kept where they are the same.
void write_dialog_enhancement(BitWriter& w, const DeConfigCodes* config,
                              const DeFrameParameters* parameters,
                              const DeFrameParameters* previous, bool iframe);

}  // namespace ac4::detail
