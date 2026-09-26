#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
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
    // drc_compression_curve_flag 0: the gains sent frame by frame, Table
    // 163's drc_gains_config, and the curve they are computed from.
    std::optional<int> gains_config;
    CurveCodes gains_curve{};
};

// drc_config() (Table 71); `gains` where a mode sends gains, which puts a
// drc_frame() with its drc_data() in every frame.
struct DrcCodes {
    std::vector<DrcModeCodes> modes;
    int eac3_profile = 2;
    bool gains = false;
};

// One frame's gains for a mode that sends them (Table 75), in dB2:
// drc_gain[group][subframe][band], as many as the mode's drc_gains_config
// gives (Tables 163, 168 and 169); with drc_gains_config 0 one gain.
struct DrcModeGains {
    int groups = 1;
    int subframes = 1;
    int bands = 1;
    std::vector<int> gain;  // [(group * subframes + subframe) * bands + band]

    [[nodiscard]] int at(int group, int subframe, int band) const noexcept {
        return gain[static_cast<std::size_t>((group * subframes + subframe) * bands + band)];
    }
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
// de_mix_coef2_idx (Table 172). The hybrid methods (Table 170's 2 and 3) add
// de_signal_contribution, 0 to 31, the waveform's share alpha_c x 31 (Part 1
// clause 4.3.14.4.6).
struct DeFrameParameters {
    std::array<std::array<int, kDeBands>, 3> par{};
    std::array<int, 2> mix{};
    int signal_contribution = 0;
};

// The presentation substream's mixing values (Part 2 clause 6.2.2.3, from
// b_substream_group_gains_present to pan_associated): each substream group's
// gain (Table 70: -0.25 dB a step, 63 silence), and the associated audio's
// gains on the main audio (Part 1 clauses 4.3.12.4.3 to 4.3.12.4.9: -0.3 dB a
// step, 255 silence) and its pan (1.5 degrees a step clockwise from the
// front).
struct AssociatedMixCodes {
    std::optional<int> scale_main;
    std::optional<int> scale_main_centre;
    std::optional<int> scale_main_front;
    std::optional<int> pan_associated;  // b_associate_is_mono
};

struct PresentationMixCodes {
    // As clause 6.2.1.3 assigns it: the group gains are sent above 1.
    int n_substream_groups = 1;
    std::optional<std::vector<int>> sg_gain;       // b_substream_group_gains_present
    bool keep = false;                             // b_keep: the last frame's gains again
    std::optional<AssociatedMixCodes> associated;  // b_associated
};

// extended_metadata()'s dialogue fields at sus_ver 1 (Part 2 clause 6.2.7.4;
// Part 1 clauses 4.3.12.4.10 to 4.3.12.4.14): g_dialog_max as (1 + x) x 3 dB,
// and the dialogue's pan, one angle for a mono substream ([0]) and two with
// pan_signal_selector for the others.
struct DialogueMixCodes {
    std::optional<int> dialog_max_gain;
    std::optional<std::array<int, 2>> pan_dialog;  // b_pan_dialog_present
    int pan_signal_selector = 0;
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

// drc_frame(b_iframe): drc_config() in I-frames, and drc_data(), with
// `gains` for each mode in drc_config()'s order that sends them (a repeat
// of one included), in I-frames and, where a mode sends gains, every frame;
// otherwise nothing (b_drc_present 0), and the configuration is kept.
void write_drc_frame(BitWriter& w, const DrcCodes* codes, bool iframe,
                     std::span<const DrcModeGains> gains = {});

// custom_dmx_data() and loud_corr() for a channel-based presentation of
// `ch_mode`: the stereo coefficients and their corrections in I-frames.
void write_downmix(BitWriter& w, int ch_mode, bool has_lfe, const DownmixCodes* codes, bool iframe);

// dialog_enhancement(b_iframe): de_config() in I-frames and each frame's
// parameters, differential in frequency in I-frames and in time against
// `previous` in the others, or kept where they are the same.
void write_dialog_enhancement(BitWriter& w, const DeConfigCodes* config,
                              const DeFrameParameters* parameters,
                              const DeFrameParameters* previous, bool iframe);

// The presentation substream's fields from b_substream_group_gains_present
// (where n_substream_groups is above 1) to pan_associated; with nothing
// configured, b_associated 0 alone.
void write_presentation_mix(BitWriter& w, const PresentationMixCodes& codes);

// extended_metadata(channel_mode, 1): b_dialog, and with `dialogue` the
// dialogue fields; no channel classification or event probability.
void write_extended_metadata(BitWriter& w, int ch_mode, const DialogueMixCodes* dialogue);

}  // namespace ac4::detail
