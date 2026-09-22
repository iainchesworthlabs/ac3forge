#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "bit_reader.hpp"
#include "syntax/context.hpp"

// The metadata of a channel-coded ac4_substream() - Part 2 clause 6.2.7.1's
// metadata() and the Part 1 clause 4.2.14 elements it calls - and the two
// elements other substreams share with it: drc_frame() (Part 1 clause
// 4.2.14.5), which ac4_presentation_substream() carries too, and
// emdf_payloads_substream() (Part 1 clause 4.2.4.4), which metadata() carries
// inline and which is also a substream of its own.
//
// Part 2 Table 49 amends Part 1's metadata(), basic_metadata(),
// further_loudness_info(), extended_metadata(), dialog_enhancement() and
// de_data(), and the amended syntax covers both substream versions: sus_ver 0
// is the Part 1 layout, sus_ver 1 the extended one every bitstream_version 2
// substream uses (Part 2 clause 6.2.1.6). Where the parts name an element
// differently (b_dmx_coeff and b_stereo_dmx_coeff, b_vhl_active and
// b_tfl_active, extension_bits and extensions_bits, de_config_flag and
// b_de_config_flag) the records carry Part 2's name.
//
// Syntax only. Fields hold the codes the bitstream sends, with only the
// arithmetic the syntax itself does - the differential decoding of DRC gains
// and dialogue enhancement parameters, and the values it carries from one
// frame to the next. Meaning is for the phases that apply them.

namespace ac4::detail {

// --- Part 2 clause 6.2.7.3 further_loudness_info ----------------------------

// Semantics: Part 1 clause 4.3.12.3 and Part 2 clause 6.3.8.2. An absent
// optional is a field whose presence flag was 0 (or that this variant of the
// element does not carry).
struct FurtherLoudnessInfo {
    int loudness_version = 0;  // loudness_version, plus extended_loudness_version when it is 3
    std::optional<int> loud_prac_type;  // not carried by the substream form at sus_ver 1
    bool b_loudcorr_dialgate = false;
    std::optional<int> dialgate_prac_type;  // the one after b_loudcorr_dialgate
    bool b_loudcorr_type = false;
    std::optional<int> loudrelgat;
    std::optional<int> loudspchgat;
    std::optional<int> loudspchgat_dialgate_prac_type;  // the dialgate_prac_type after loudspchgat
    std::optional<int> loudstrm3s;
    std::optional<int> max_loudstrm3s;
    std::optional<int> truepk;
    std::optional<int> max_truepk;
    // b_prgmbndy: prgmbndy is 2 to the number of prgmbndy_bit reads (Part 1
    // clause 4.3.12.3.21). Held as that count so a stream that runs the loop
    // past 63 bits cannot overflow it.
    std::optional<int> prgmbndy_bits;
    bool b_end_or_start = false;
    std::optional<int> prgmbndy_offset;
    std::optional<int> lra;
    int lra_prac_type = 0;
    std::optional<int> loudmntry;
    std::optional<int> max_loudmntry;
    std::optional<int> rtll_comp;
    std::optional<std::uint64_t> e_bits_size;  // b_extension
    std::uint64_t extensions_bits = 0;         // width of extensions_bits, not interpreted
};

// Part 2 clause 6.2.7.3. b_presentation_ldn is 1 for the presentation
// substream's loudness and 0 for a substream's.
[[nodiscard]] ParseResult parse_further_loudness_info(BitReader& r, int sus_ver,
                                                      bool b_presentation_ldn,
                                                      FurtherLoudnessInfo& out);

// --- Part 2 clause 6.2.7.2 basic_metadata ------------------------------------

// The b_stereo_dmx_coeff block. basic_metadata() carries it for sus_ver 0,
// with the two downmix loudness corrections; custom_dmx_data() (Part 2
// clause 6.2.9.2) carries it without them. Semantics: Part 1 clause
// 4.3.12.2.8 to 4.3.12.2.19.
struct StereoDmxCoeff {
    int loro_centre_mixgain = 0;
    int loro_surround_mixgain = 0;
    std::optional<int> loro_dmx_loud_corr;  // basic_metadata only
    bool b_ltrt_mixinfo = false;
    int ltrt_centre_mixgain = 0;
    int ltrt_surround_mixgain = 0;
    std::optional<int> ltrt_dmx_loud_corr;  // basic_metadata only
    std::optional<int> lfe_mixgain;         // b_lfe_mixinfo, for a mode with LFE
    int preferred_dmx_method = 0;
};

struct BasicMetadata {
    std::optional<int> dialnorm_bits;  // sus_ver 0; at sus_ver 1 the presentation substream has it
    bool b_more_basic_metadata = false;
    std::optional<int> substream_loudness_bits;  // sus_ver 1, b_substream_loudness_info
    // b_further_loudness_info (sus_ver 0) or b_further_substream_loudness_info
    // (sus_ver 1).
    std::optional<FurtherLoudnessInfo> further_loudness_info;
    std::optional<int> pre_dmixtyp_2ch;  // b_prev_dmx_info, stereo
    std::optional<int> phase90_info_2ch;
    std::optional<StereoDmxCoeff> stereo_dmx_coeff;  // sus_ver 0, above stereo
    std::optional<int> pre_dmixtyp_5ch;              // 5.X
    std::optional<int> pre_upmixtyp_5ch;
    bool b_upmixtyp_7ch = false;  // 7.X
    std::optional<int> pre_upmixtyp_3_4;
    std::optional<int> pre_upmixtyp_3_2_2;
    std::optional<int> phase90_info_mc;  // above stereo
    bool b_surround_attenuation_known = false;
    bool b_lfe_attenuation_known = false;
    std::optional<bool> dc_block_on;  // b_dc_blocking
};

// --- Part 2 clause 6.2.7.4 extended_metadata ---------------------------------

struct ExtendedMetadata {
    // At sus_ver 0 both are parameters (SubstreamContext::b_associated and
    // b_dialog, Part 1 clauses 4.3.12.4.1 and 4.3.12.4.2); at sus_ver 1
    // b_dialog is read and b_associated is not used.
    bool b_associated = false;
    bool b_dialog = false;
    std::optional<int> scale_main;
    std::optional<int> scale_main_centre;
    std::optional<int> scale_main_front;
    std::optional<int> pan_associated;  // mono
    std::optional<int> dialog_max_gain;
    bool b_pan_dialog_present = false;
    std::array<int, 2> pan_dialog{};  // a mono substream's single pan_dialog is [0]
    std::optional<int> pan_signal_selector;
    bool b_channels_classifier = false;
    bool b_c_active = false;
    bool b_c_has_dialog = false;
    bool b_l_active = false;
    bool b_l_has_dialog = false;
    bool b_r_active = false;
    bool b_r_has_dialog = false;
    bool b_ls_active = false;
    bool b_rs_active = false;
    bool b_lb_active = false;
    bool b_rb_active = false;
    bool b_lw_active = false;
    bool b_rw_active = false;
    bool b_tfl_active = false;
    bool b_tfr_active = false;
    bool b_lfe_active = false;
    std::optional<int> event_probability;
};

// --- Part 1 clauses 4.2.14.5 to 4.2.14.10 drc_frame -------------------------

inline constexpr int kMaxDrcModes = 8;       // drc_decoder_nr_modes is 3 bits
inline constexpr int kMaxDrcChannels = 4;    // Part 1 Table 168, Part 2 Table 69
inline constexpr int kMaxDrcSubframes = 8;   // Part 1 Table 169
inline constexpr int kMaxDrcBands = 4;       // Part 1 Table 163

// Part 1 clause 4.2.14.8; semantics clause 4.3.13.4. A field whose condition
// was false stays 0.
struct DrcCompressionCurve {
    int drc_lev_nullband_low = 0;
    int drc_lev_nullband_high = 0;
    int drc_gain_max_boost = 0;
    int drc_lev_max_boost = 0;
    int drc_nr_boost_sections = 0;
    int drc_gain_section_boost = 0;
    int drc_lev_section_boost = 0;
    int drc_gain_max_cut = 0;
    int drc_lev_max_cut = 0;
    int drc_nr_cut_sections = 0;
    int drc_gain_section_cut = 0;
    int drc_lev_section_cut = 0;
    bool drc_tc_default_flag = false;
    int drc_tc_attack = 0;
    int drc_tc_release = 0;
    int drc_tc_attack_fast = 0;
    int drc_tc_release_fast = 0;
    bool drc_adaptive_smoothing_flag = false;
    int drc_attack_threshold = 0;
    int drc_release_threshold = 0;
};

// Part 1 clause 4.2.14.7, one DRC decoder mode.
struct DrcDecoderModeConfig {
    bool configured = false;         // the drc_config() in force configured this mode id
    int drc_output_level_from = 0;   // mode ids 4 to 7 only
    int drc_output_level_to = 0;
    bool drc_repeat_profile_flag = false;
    int drc_repeat_id = 0;
    // With drc_repeat_profile_flag the three below and `curve` are copies of
    // mode drc_repeat_id's: clause 4.3.13.3.5 says the repeated mode "is
    // defined by" that one, although the syntax only copies the curve flag.
    bool drc_default_profile_flag = false;
    bool drc_compression_curve_flag = false;  // 1 also when drc_default_profile_flag set it
    int drc_gains_config = 0;                 // Table 163, when the curve flag is 0
    std::optional<DrcCompressionCurve> curve;
};

// Part 1 clause 4.2.14.6.
struct DrcConfig {
    int drc_decoder_nr_modes = 0;  // modes carried: this + 1
    std::array<int, kMaxDrcModes> drc_decoder_mode{};  // mode ids in transmission order
    std::array<DrcDecoderModeConfig, kMaxDrcModes> mode{};  // by drc_decoder_mode_id
    int drc_eac3_profile = 0;  // Table 160
};

// What drc_frame() carries from one frame to the next: the drc_config() of the
// last I-frame. An I-frame always replaces it, and an I-frame without DRC
// clears it, because an I-frame is one that "can be decoded independently from
// preceding frames" (Part 2 clause 4.5.2), so no later frame may lean on an
// older configuration.
struct DrcState {
    bool config_valid = false;
    DrcConfig config{};
};

// What drc_frame() needs from outside it.
struct DrcContext {
    // drc_frame(b_iframe): b_iframe or b_audio_ndot for metadata() at
    // sus_ver 0, b_pres_ndot for ac4_presentation_substream().
    bool b_iframe = false;
    // nr_drc_channels (Part 1 clause 4.3.13.7.1 Table 168, Part 2 clause
    // 6.3.3.1.21 Table 69) depends on the channel configuration: the
    // substream's ch_mode, or the presentation's pres_ch_mode. -1 when there
    // is none (an object presentation). add_ch_base changes which channels
    // form each group, not how many groups there are, so the syntax does not
    // need it.
    int ch_mode = -1;
    // nr_drc_subframes (Part 1 clause 4.3.13.7.2 Table 169) depends on the
    // frame length: frame_len_base, Part 1 Tables 83 and 84.
    int frame_len_base = 2048;
};

// Table 168 extended by Table 69; -1 where neither table gives the channel
// configuration (3.0, and no channel mode at all). Table 168 lists only the
// modes with LFE, and Table 69 puts LFE in a group only "in case they are
// present", so a mode without LFE takes its LFE twin's count.
[[nodiscard]] int nr_drc_channels(const DrcContext& ctx) noexcept;

// Table 169; -1 for a frame length the table does not list.
[[nodiscard]] int nr_drc_subframes(int frame_len_base) noexcept;

// One drc_data() entry for a mode whose drc_compression_curve_flag is 0.
struct DrcGainset {
    int drc_decoder_mode_id = 0;
    std::uint64_t drc_gainset_size = 0;
    int drc_version = 0;
    bool gains_present = false;  // drc_version <= 1: drc_gains() was read
    int drc_gains_config = 0;    // the mode's, which sets nr_drc_bands
    // The extents the gain loops ran over; all 1 for drc_gains_config 0, whose
    // single gain applies to every channel, band and subframe.
    int nr_drc_channels = 0;
    int nr_drc_bands = 0;
    int nr_drc_subframes = 0;
    // drc_gain[ch][sf][band] in dB: drc_gain_val - 64 (clause 4.3.13.6.1) and
    // the differential sums of drc_gains().
    std::array<std::int16_t, kMaxDrcChannels * kMaxDrcSubframes * kMaxDrcBands> drc_gain{};
    std::uint64_t drc2_bits = 0;  // width of drc2_bits, read when drc_version >= 1

    [[nodiscard]] int gain(int ch, int sf, int band) const noexcept {
        return drc_gain[static_cast<std::size_t>((ch * kMaxDrcSubframes + sf) * kMaxDrcBands +
                                                 band)];
    }
};

struct DrcFrame {
    bool b_drc_present = false;
    bool drc_config_present = false;  // this frame carried drc_config(); DrcState holds it
    int n_gainsets = 0;
    std::array<DrcGainset, kMaxDrcModes> gainsets{};
    bool curve_present = false;
    bool drc_reset_flag = false;
    int drc_reserved = 0;
};

[[nodiscard]] ParseResult parse_drc_frame(BitReader& r, const DrcContext& ctx, DrcState& state,
                                          DrcFrame& out);

// --- Part 2 clauses 6.2.7.5 and 6.2.7.6 dialog_enhancement ------------------

inline constexpr int kDeNrBands = 8;      // de_nr_bands, Part 1 clause 4.3.14.5.1
inline constexpr int kMaxDeChannels = 3;  // Part 1 Table 171

using DeParameters = std::array<std::array<int, kDeNrBands>, kMaxDeChannels>;

// Part 1 clause 4.2.14.12.
struct DeConfig {
    int de_method = 0;          // Table 170
    int de_max_gain = 0;
    int de_channel_config = 0;  // Table 171
};

// Part 1 Table 171, column de_nr_channels.
[[nodiscard]] int de_nr_channels(int de_channel_config) noexcept;

// One de_data() element, with what it carries over applied: the values here are
// the ones in force for this frame whether transmitted or kept.
struct DeData {
    bool de_keep_pos_flag = false;
    int de_mix_coef1_idx = 0;
    int de_mix_coef2_idx = 0;
    bool de_keep_data_flag = false;
    bool de_ms_proc_flag = false;
    // de_par[ch][band]: quantization indices, after de_abs_huffman() and
    // de_diff_huffman() (Part 1 clauses 4.3.14.5.4 and 4.3.14.5.5).
    DeParameters de_par{};
    int de_signal_contribution = 0;
};

// What one de_data() set carries from one frame to the next.
struct DeParameterState {
    DeParameters de_par_prev{};  // Part 1 clause 4.3.14.5.3: 0 where the last frame defined none
    int de_mix_coef1_idx = 0;
    int de_mix_coef2_idx = 0;
    bool de_ms_proc_flag = false;
    int de_signal_contribution = 0;
};

struct DialogEnhancement {
    bool b_de_data_present = false;
    bool b_de_config_flag = false;
    bool de_config_present = false;  // de_config() was read: an I-frame, or b_de_config_flag
    DeConfig config{};               // in force for this frame
    int de_nr_channels = 0;
    DeData data{};
    // Part 2 clause 6.3.8.3.1: the separate set for core decoding, 9.X.4 only
    // (Part 2 clause 4.8.3.15).
    bool b_de_simulcast = false;
    DeData core_data{};
};

// The last de_config() - with the same I-frame rule as DrcState - and each
// data set's carried values.
struct DeState {
    bool config_valid = false;
    DeConfig config{};
    DeParameterState data{};
    DeParameterState core_data{};
};

// --- Part 1 clauses 4.2.4.4, 4.2.14.14 emdf_payloads_substream --------------

// Part 1 clause 4.2.14.14; semantics clause 4.3.15.2.
struct EmdfPayloadConfig {
    // 64 bits, as variable_bits() hands them back: nothing here bounds what
    // the escape can express, and these are reported rather than indexed with.
    std::optional<std::uint64_t> smpoffst;
    std::optional<std::uint64_t> duration;
    std::optional<std::uint64_t> groupid;
    std::optional<int> codecdata;
    bool b_discard_unknown_payload = false;
    // Read only when neither b_discard_unknown_payload nor b_smpoffst is set.
    bool b_payload_frame_aligned = false;
    bool b_create_duplicate = false;
    bool b_remove_duplicate = false;
    std::optional<int> priority;
    std::optional<int> proc_allowed;
};

struct EmdfPayload {
    // Table 174; never the terminating 0. 64 bits because the escape at 31 adds
    // a variable_bits(5) that spans the whole range that can express.
    std::uint64_t emdf_payload_id = 0;
    EmdfPayloadConfig config{};
    std::vector<std::uint8_t> bytes;  // emdf_payload_size of them
};

struct EmdfPayloads {
    std::vector<EmdfPayload> payloads;
};

// Part 1 clause 4.2.4.4, including its final byte_align. Both an
// emdf_payloads_substream_info() substream and the one metadata() carries.
[[nodiscard]] ParseResult parse_emdf_payloads_substream(BitReader& r, EmdfPayloads& out);

// --- Part 2 clause 6.2.7.1 metadata ------------------------------------------

struct Metadata {
    BasicMetadata basic{};
    ExtendedMetadata extended{};
    std::uint64_t tools_metadata_size = 0;
    // sus_ver 0 only: at sus_ver 1 drc_frame() is in the presentation substream.
    std::optional<DrcFrame> drc;
    DialogEnhancement dialog_enhancement{};
    bool b_emdf_payloads_substream = false;
    EmdfPayloads emdf_payloads{};
};

// Per audio substream, carried between frames.
struct MetadataState {
    DrcState drc{};  // sus_ver 0 only
    DeState de{};
};

// Part 2 clause 6.2.7.1 metadata(b_alternative, b_ajoc, b_iframe, channel_mode,
// sus_ver) for a channel-coded substream, from its first bit to the end of its
// last element; the caller reads the byte_align after it.
//
// Channel-coded only: b_ajoc is 0 there (Part 2 clause 6.2.1.6), and the
// oamd_dyndata_single() that metadata() reads when b_alternative is set
// belongs to object substreams alone - its n_objs, obj_type[] and b_lfe[] exist
// only for them, and Part 2 Table 7 places it only in object audio - so
// ctx.b_alternative reads nothing here.
//
// Checks tools_metadata_size against the bits drc_frame() and
// dialog_enhancement() took.
[[nodiscard]] ParseResult parse_metadata(BitReader& r, const SubstreamContext& ctx,
                                         MetadataState& state, Metadata& out);

// --- Shared by the syntax that carries metadata ------------------------------

// Reads `bits` bits of an element the syntax reads as one run it does not
// interpret (extensions_bits, drc2_bits, add_data), recorded as
// BitReader::read_run() describes. Fails with kTruncated, recording nothing,
// when the run does not fit in what is left of the substream.
[[nodiscard]] ParseResult read_bit_run(BitReader& r, std::uint64_t bits, std::string_view name);

}  // namespace ac4::detail
