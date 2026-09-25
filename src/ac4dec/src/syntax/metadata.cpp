#include "syntax/metadata.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string_view>
#include <utility>

#include "huffman.hpp"
#include "tables/huffman_tables.hpp"

namespace ac4::detail {

namespace {

[[nodiscard]] int read_int(BitReader& r, int bits, std::string_view name) {
    return static_cast<int>(r.read(bits, name));
}

// Part 2 Pseudocodes 30 to 36. For ch_mode 0 to 10 they agree with Part 1's
// Pseudocodes 9 to 15, so sus_ver 0 substreams need no second set.
[[nodiscard]] bool contains_lfe(int mode) noexcept {
    return mode == ch_mode::k5_1 || mode == ch_mode::k7_1_340 || mode == ch_mode::k7_1_520 ||
           mode == ch_mode::k7_1_322 || mode == ch_mode::k7_1_4 || mode == ch_mode::k9_1_4 ||
           mode == ch_mode::k22_2;
}

[[nodiscard]] bool contains_c(int mode) noexcept {
    return mode == ch_mode::kMono || (mode >= ch_mode::k3_0 && mode <= ch_mode::k22_2);
}

[[nodiscard]] bool contains_lr(int mode) noexcept {
    return mode >= ch_mode::kStereo && mode <= ch_mode::k22_2;
}

[[nodiscard]] bool contains_ls_rs(int mode) noexcept {
    return mode >= ch_mode::k5_0 && mode <= ch_mode::k22_2;
}

[[nodiscard]] bool contains_lb_rb(int mode) noexcept {
    return mode == ch_mode::k7_0_340 || mode == ch_mode::k7_1_340 ||
           (mode >= ch_mode::k7_0_4 && mode <= ch_mode::k22_2);
}

[[nodiscard]] bool contains_lw_rw(int mode) noexcept {
    return mode == ch_mode::k7_0_520 || mode == ch_mode::k7_1_520 || mode == ch_mode::k22_2;
}

// Only the 3/2/2 modes, although 7.X.4 and 9.X.4 have top front channels too:
// Pseudocode 36 lists 9 and 10 and nothing else.
[[nodiscard]] bool contains_tfl_tfr(int mode) noexcept {
    return mode == ch_mode::k7_0_322 || mode == ch_mode::k7_1_322;
}

// huff_decode() (Part 1 clause 4.3.6.4.2), a miss reported as every tool
// reports one (huff_codeword()).
[[nodiscard]] std::expected<int, SyntaxError> read_codeword(BitReader& r, const Codebook& codebook,
                                                            std::string_view element) {
    return huff_codeword(r, codebook, element,
                         {.truncated = "a Huffman codeword runs past the end of the substream",
                          .invalid = "no Huffman codeword of the codebook matches"});
}

// --- Part 2 clause 6.2.7.2 basic_metadata ------------------------------------

[[nodiscard]] ParseResult parse_basic_metadata(BitReader& r, const SubstreamContext& ctx,
                                               BasicMetadata& out) {
    const int mode = ctx.ch_mode;
    if (ctx.sus_ver == 0) {
        out.dialnorm_bits = read_int(r, 7, "dialnorm_bits");
    }
    out.b_more_basic_metadata = r.read_flag("b_more_basic_metadata");
    if (!out.b_more_basic_metadata) {
        return check(r);
    }
    if (ctx.sus_ver == 0) {
        if (r.read_flag("b_further_loudness_info")) {
            FurtherLoudnessInfo info;
            if (const ParseResult result =
                    parse_further_loudness_info(r, ctx.sus_ver, false, info);
                !result) {
                return result;
            }
            out.further_loudness_info = info;
        }
    } else {
        if (r.read_flag("b_substream_loudness_info")) {
            out.substream_loudness_bits = read_int(r, 8, "substream_loudness_bits");
            if (r.read_flag("b_further_substream_loudness_info")) {
                FurtherLoudnessInfo info;
                if (const ParseResult result =
                        parse_further_loudness_info(r, ctx.sus_ver, false, info);
                    !result) {
                    return result;
                }
                out.further_loudness_info = info;
            }
        }
    }
    if (mode == ch_mode::kStereo) {
        if (r.read_flag("b_prev_dmx_info")) {
            out.pre_dmixtyp_2ch = read_int(r, 3, "pre_dmixtyp_2ch");
            out.phase90_info_2ch = read_int(r, 2, "phase90_info_2ch");
        }
    }
    if (mode > ch_mode::kStereo) {
        // Only the stereo downmix block depends on sus_ver: at sus_ver 1 the
        // presentation substream's custom_dmx_data() carries it instead. The
        // 5.X and 7.X blocks and the three fields after them are read at both
        // versions (verified on the rendered page 136, where the text's
        // indentation across the page break suggests otherwise).
        if (ctx.sus_ver == 0) {
            if (r.read_flag("b_stereo_dmx_coeff")) {
                StereoDmxCoeff coeff;
                coeff.loro_centre_mixgain = read_int(r, 3, "loro_centre_mixgain");
                coeff.loro_surround_mixgain = read_int(r, 3, "loro_surround_mixgain");
                if (r.read_flag("b_loro_dmx_loud_corr")) {
                    coeff.loro_dmx_loud_corr = read_int(r, 5, "loro_dmx_loud_corr");
                }
                coeff.b_ltrt_mixinfo = r.read_flag("b_ltrt_mixinfo");
                if (coeff.b_ltrt_mixinfo) {
                    coeff.ltrt_centre_mixgain = read_int(r, 3, "ltrt_centre_mixgain");
                    coeff.ltrt_surround_mixgain = read_int(r, 3, "ltrt_surround_mixgain");
                }
                if (r.read_flag("b_ltrt_dmx_loud_corr")) {
                    coeff.ltrt_dmx_loud_corr = read_int(r, 5, "ltrt_dmx_loud_corr");
                }
                if (contains_lfe(mode)) {
                    if (r.read_flag("b_lfe_mixinfo")) {
                        coeff.lfe_mixgain = read_int(r, 5, "lfe_mixgain");
                    }
                }
                coeff.preferred_dmx_method = read_int(r, 2, "preferred_dmx_method");
                out.stereo_dmx_coeff = coeff;
            }
        }
        // channel_mode == 5_X: 5.0 and 5.1, the two modes Table 56 names 5.X.
        if (mode == ch_mode::k5_0 || mode == ch_mode::k5_1) {
            if (r.read_flag("b_predmixtyp_5ch")) {
                out.pre_dmixtyp_5ch = read_int(r, 3, "pre_dmixtyp_5ch");
            }
            if (r.read_flag("b_preupmixtyp_5ch")) {
                out.pre_upmixtyp_5ch = read_int(r, 4, "pre_upmixtyp_5ch");
            }
        }
        // channel_mode == 7_X, 3/4/0 and 3/2/2: Part 1 Table 67 spells them
        // 5 <= ch_mode <= 10, 5 <= ch_mode <= 6 and 9 <= ch_mode <= 10. 5/2/0
        // sets b_upmixtyp_7ch and reads nothing after it.
        if (mode >= ch_mode::k7_0_340 && mode <= ch_mode::k7_1_322) {
            out.b_upmixtyp_7ch = r.read_flag("b_upmixtyp_7ch");
            if (out.b_upmixtyp_7ch) {
                if (mode == ch_mode::k7_0_340 || mode == ch_mode::k7_1_340) {
                    out.pre_upmixtyp_3_4 = read_int(r, 2, "pre_upmixtyp_3_4");
                } else if (mode == ch_mode::k7_0_322 || mode == ch_mode::k7_1_322) {
                    out.pre_upmixtyp_3_2_2 = read_int(r, 1, "pre_upmixtyp_3_2_2");
                }
            }
        }
        out.phase90_info_mc = read_int(r, 2, "phase90_info_mc");
        out.b_surround_attenuation_known = r.read_flag("b_surround_attenuation_known");
        out.b_lfe_attenuation_known = r.read_flag("b_lfe_attenuation_known");
    }
    if (r.read_flag("b_dc_blocking")) {
        out.dc_block_on = r.read_flag("dc_block_on");
    }
    return check(r);
}

// --- Part 2 clause 6.2.7.4 extended_metadata ---------------------------------

[[nodiscard]] ParseResult parse_extended_metadata(BitReader& r, const SubstreamContext& ctx,
                                                  ExtendedMetadata& out) {
    const int mode = ctx.ch_mode;
    if (ctx.sus_ver >= 1) {
        out.b_dialog = r.read_flag("b_dialog");
    } else {
        // The NOTE under the table: at sus_ver 0 both flags are parameters
        // the caller derives from the table of contents.
        out.b_associated = ctx.b_associated;
        out.b_dialog = ctx.b_dialog;
        if (out.b_associated) {
            if (r.read_flag("b_scale_main")) {
                out.scale_main = read_int(r, 8, "scale_main");
            }
            if (r.read_flag("b_scale_main_centre")) {
                out.scale_main_centre = read_int(r, 8, "scale_main_centre");
            }
            if (r.read_flag("b_scale_main_front")) {
                out.scale_main_front = read_int(r, 8, "scale_main_front");
            }
            if (mode == ch_mode::kMono) {
                out.pan_associated = read_int(r, 8, "pan_associated");
            }
        }
    }
    if (out.b_dialog) {
        if (r.read_flag("b_dialog_max_gain")) {
            out.dialog_max_gain = read_int(r, 2, "dialog_max_gain");
        }
        out.b_pan_dialog_present = r.read_flag("b_pan_dialog_present");
        if (out.b_pan_dialog_present) {
            if (mode == ch_mode::kMono) {
                out.pan_dialog[0] = read_int(r, 8, "pan_dialog");
            } else {
                out.pan_dialog[0] = read_int(r, 8, "pan_dialog[0]");
                out.pan_dialog[1] = read_int(r, 8, "pan_dialog[1]");
                out.pan_signal_selector = read_int(r, 2, "pan_signal_selector");
            }
        }
    }
    out.b_channels_classifier = r.read_flag("b_channels_classifier");
    if (out.b_channels_classifier) {
        if (contains_c(mode)) {
            out.b_c_active = r.read_flag("b_c_active");
            if (out.b_c_active) {
                out.b_c_has_dialog = r.read_flag("b_c_has_dialog");
            }
        }
        if (contains_lr(mode)) {
            out.b_l_active = r.read_flag("b_l_active");
            if (out.b_l_active) {
                out.b_l_has_dialog = r.read_flag("b_l_has_dialog");
            }
            out.b_r_active = r.read_flag("b_r_active");
            if (out.b_r_active) {
                out.b_r_has_dialog = r.read_flag("b_r_has_dialog");
            }
        }
        if (contains_ls_rs(mode)) {
            out.b_ls_active = r.read_flag("b_ls_active");
            out.b_rs_active = r.read_flag("b_rs_active");
        }
        if (contains_lb_rb(mode)) {
            out.b_lb_active = r.read_flag("b_lb_active");
            out.b_rb_active = r.read_flag("b_rb_active");
        }
        if (contains_lw_rw(mode)) {
            out.b_lw_active = r.read_flag("b_lw_active");
            out.b_rw_active = r.read_flag("b_rw_active");
        }
        if (contains_tfl_tfr(mode)) {
            out.b_tfl_active = r.read_flag("b_tfl_active");
            out.b_tfr_active = r.read_flag("b_tfr_active");
        }
        if (contains_lfe(mode)) {
            out.b_lfe_active = r.read_flag("b_lfe_active");
        }
    }
    if (r.read_flag("b_event_probability")) {
        out.event_probability = read_int(r, 4, "event_probability");
    }
    return check(r);
}

// --- Part 1 clauses 4.2.14.6 to 4.2.14.10 DRC -------------------------------

[[nodiscard]] ParseResult parse_drc_compression_curve(BitReader& r, DrcCompressionCurve& out) {
    out.drc_lev_nullband_low = read_int(r, 4, "drc_lev_nullband_low");
    out.drc_lev_nullband_high = read_int(r, 4, "drc_lev_nullband_high");
    out.drc_gain_max_boost = read_int(r, 4, "drc_gain_max_boost");
    if (out.drc_gain_max_boost > 0) {
        out.drc_lev_max_boost = read_int(r, 5, "drc_lev_max_boost");
        out.drc_nr_boost_sections = read_int(r, 1, "drc_nr_boost_sections");
        if (out.drc_nr_boost_sections > 0) {
            out.drc_gain_section_boost = read_int(r, 4, "drc_gain_section_boost");
            out.drc_lev_section_boost = read_int(r, 5, "drc_lev_section_boost");
        }
    }
    out.drc_gain_max_cut = read_int(r, 5, "drc_gain_max_cut");
    if (out.drc_gain_max_cut > 0) {
        out.drc_lev_max_cut = read_int(r, 6, "drc_lev_max_cut");
        out.drc_nr_cut_sections = read_int(r, 1, "drc_nr_cut_sections");
        if (out.drc_nr_cut_sections > 0) {
            out.drc_gain_section_cut = read_int(r, 5, "drc_gain_section_cut");
            out.drc_lev_section_cut = read_int(r, 5, "drc_lev_section_cut");
        }
    }
    out.drc_tc_default_flag = r.read_flag("drc_tc_default_flag");
    if (!out.drc_tc_default_flag) {
        out.drc_tc_attack = read_int(r, 8, "drc_tc_attack");
        out.drc_tc_release = read_int(r, 8, "drc_tc_release");
        out.drc_tc_attack_fast = read_int(r, 8, "drc_tc_attack_fast");
        out.drc_tc_release_fast = read_int(r, 8, "drc_tc_release_fast");
        out.drc_adaptive_smoothing_flag = r.read_flag("drc_adaptive_smoothing_flag");
        if (out.drc_adaptive_smoothing_flag) {
            out.drc_attack_threshold = read_int(r, 5, "drc_attack_threshold");
            out.drc_release_threshold = read_int(r, 5, "drc_release_threshold");
        }
    }
    return check(r);
}

// Part 1 clauses 4.2.14.6 and 4.2.14.7.
[[nodiscard]] ParseResult parse_drc_config(BitReader& r, DrcConfig& config) {
    config = DrcConfig{};
    config.drc_decoder_nr_modes = read_int(r, 3, "drc_decoder_nr_modes");
    for (int m = 0; m <= config.drc_decoder_nr_modes; ++m) {
        const int id = read_int(r, 3, "drc_decoder_mode_id");
        DrcDecoderModeConfig mode;
        if (id > 3) {
            mode.drc_output_level_from = read_int(r, 5, "drc_output_level_from");
            mode.drc_output_level_to = read_int(r, 5, "drc_output_level_to");
        }
        mode.drc_repeat_profile_flag = r.read_flag("drc_repeat_profile_flag");
        if (mode.drc_repeat_profile_flag) {
            mode.drc_repeat_id = read_int(r, 3, "drc_repeat_id");
            if (const ParseResult result = check(r); !result) {
                return result;
            }
            // The syntax copies drc_compression_curve_flag[drc_repeat_id] at
            // this point, so the mode repeated has to be one this drc_config()
            // configured earlier: anything else has no flag to copy.
            const DrcDecoderModeConfig& source =
                config.mode[static_cast<std::size_t>(mode.drc_repeat_id)];
            if (!source.configured) {
                return fail(DecodeError::kInvalidStream,
                            "drc_repeat_id names a DRC decoder mode drc_config() has not yet "
                            "configured");
            }
            mode.drc_default_profile_flag = source.drc_default_profile_flag;
            mode.drc_compression_curve_flag = source.drc_compression_curve_flag;
            mode.drc_gains_config = source.drc_gains_config;
            mode.curve = source.curve;
        } else {
            mode.drc_default_profile_flag = r.read_flag("drc_default_profile_flag");
            if (!mode.drc_default_profile_flag) {
                mode.drc_compression_curve_flag = r.read_flag("drc_compression_curve_flag");
                if (mode.drc_compression_curve_flag) {
                    DrcCompressionCurve curve;
                    if (const ParseResult result = parse_drc_compression_curve(r, curve);
                        !result) {
                        return result;
                    }
                    mode.curve = curve;
                } else {
                    mode.drc_gains_config = read_int(r, 2, "drc_gains_config");
                }
            } else {
                mode.drc_compression_curve_flag = true;
            }
        }
        mode.configured = true;
        config.mode[static_cast<std::size_t>(id)] = mode;
        config.drc_decoder_mode[static_cast<std::size_t>(m)] = id;
        if (const ParseResult result = check(r); !result) {
            return result;
        }
    }
    config.drc_eac3_profile = read_int(r, 3, "drc_eac3_profile");
    return check(r);
}

// Part 1 Table 163.
[[nodiscard]] int nr_drc_bands(int drc_gains_config) noexcept {
    switch (drc_gains_config) {
        case 2:
            return 2;
        case 3:
            return 4;
        default:
            return 1;
    }
}

// Part 1 clause 4.2.14.10.
[[nodiscard]] ParseResult parse_drc_gains(BitReader& r, const DrcContext& ctx,
                                          const DrcDecoderModeConfig& mode, DrcGainset& out) {
    out.gains_present = true;
    out.drc_gains_config = mode.drc_gains_config;
    auto at = [&out](int ch, int sf, int band) -> std::int16_t& {
        return out.drc_gain[static_cast<std::size_t>((ch * kMaxDrcSubframes + sf) * kMaxDrcBands +
                                                     band)];
    };
    // Clause 4.3.13.6.1 gives drc_gain[0][0][0] as drc_gain_val - 64 dB.
    at(0, 0, 0) = static_cast<std::int16_t>(read_int(r, 7, "drc_gain_val") - 64);
    if (mode.drc_gains_config == 0) {
        out.nr_drc_channels = 1;
        out.nr_drc_bands = 1;
        out.nr_drc_subframes = 1;
        return check(r);
    }
    const int channels = nr_drc_channels(ctx);
    if (channels < 0) {
        return fail(DecodeError::kUnsupported,
                    "channel-dependent DRC gains for a channel configuration Tables 168 and 69 do "
                    "not give nr_drc_channels for");
    }
    const int subframes = nr_drc_subframes(ctx.frame_len_base);
    if (subframes < 0) {
        return fail(DecodeError::kInvalidStream,
                    "channel-dependent DRC gains at a frame length Table 169 does not list");
    }
    const int bands = nr_drc_bands(mode.drc_gains_config);
    out.nr_drc_channels = channels;
    out.nr_drc_bands = bands;
    out.nr_drc_subframes = subframes;
    // Table 75 is one opening brace short: its band loop has none, yet a
    // closing brace follows the reference reset after the subframe loop. The
    // indentation puts that reset inside the band loop, which is what gives
    // a frequency-differential first subframe and time-differential others.
    // The number of drc_gain_code reads does not depend on the reading.
    int ref = at(0, 0, 0);
    for (int ch = 0; ch < channels; ++ch) {
        for (int band = 0; band < bands; ++band) {
            for (int sf = 0; sf < subframes; ++sf) {
                if (sf != 0 || band != 0 || ch != 0) {
                    const auto index = read_codeword(r, tables::kDrcHcb, "drc_gain_code");
                    if (!index) {
                        return std::unexpected(index.error());
                    }
                    at(ch, sf, band) =
                        static_cast<std::int16_t>(ref + (*index - tables::kDrcHcb.cb_off));
                }
                ref = at(ch, sf, band);
            }
            ref = at(ch, 0, band);
        }
        ref = at(ch, 0, 0);
    }
    return check(r);
}

// Part 1 clause 4.2.14.9.
[[nodiscard]] ParseResult parse_drc_data(BitReader& r, const DrcContext& ctx,
                                         const DrcConfig& config, DrcFrame& out) {
    for (int m = 0; m <= config.drc_decoder_nr_modes; ++m) {
        const int mode_id = config.drc_decoder_mode[static_cast<std::size_t>(m)];
        const DrcDecoderModeConfig& mode = config.mode[static_cast<std::size_t>(mode_id)];
        if (mode.drc_compression_curve_flag) {
            out.curve_present = true;
            continue;
        }
        DrcGainset& gainset = out.gainsets[static_cast<std::size_t>(out.n_gainsets)];
        ++out.n_gainsets;
        gainset.drc_decoder_mode_id = mode_id;
        gainset.drc_gains_config = mode.drc_gains_config;
        std::uint64_t size = r.read(6, "drc_gainset_size_value");
        if (r.read_flag("b_more_bits")) {
            size += std::uint64_t{r.variable_bits(2, "drc_gainset_size")} << 6U;
        }
        gainset.drc_gainset_size = size;
        gainset.drc_version = read_int(r, 2, "drc_version");
        std::uint64_t used_bits = 0;
        if (gainset.drc_version <= 1) {
            const std::size_t start = r.position();
            if (const ParseResult result = parse_drc_gains(r, ctx, mode, gainset); !result) {
                return result;
            }
            used_bits = r.position() - start;
        }
        if (const ParseResult result = check(r); !result) {
            return result;
        }
        if (gainset.drc_version >= 1) {
            // bits_left = drc_gainset_size - 2 - used_bits: the syntax counts
            // drc_version inside drc_gainset_size.
            if (size < 2 + used_bits) {
                return fail(DecodeError::kInvalidStream,
                            "drc_gainset_size is smaller than the drc_version and drc_gains() it "
                            "covers");
            }
            gainset.drc2_bits = size - 2 - used_bits;
            if (const ParseResult result = read_bit_run(r, gainset.drc2_bits, "drc2_bits");
                !result) {
                return result;
            }
        } else if (size != used_bits + 2 && size != used_bits) {
            // Clause 4.3.13.5.1 makes the size that of drc_gains() alone,
            // while the bits_left formula counts drc_version in it. With
            // drc_version 0 nothing is read by the size, so either reading
            // is accepted and anything else is not.
            return fail(DecodeError::kInvalidStream,
                        "drc_gainset_size does not match the drc_gains() read");
        }
    }
    if (out.curve_present) {
        out.drc_reset_flag = r.read_flag("drc_reset_flag");
        out.drc_reserved = read_int(r, 2, "drc_reserved");
    }
    return check(r);
}

// --- Part 2 clauses 6.2.7.5 and 6.2.7.6 dialogue enhancement ----------------

[[nodiscard]] ParseResult parse_de_config(BitReader& r, DeConfig& out) {
    out.de_method = read_int(r, 2, "de_method");
    out.de_max_gain = read_int(r, 2, "de_max_gain");
    out.de_channel_config = read_int(r, 3, "de_channel_config");
    return check(r);
}

// Part 2 clause 6.2.7.6 de_data(de_method, de_nr_channels, b_iframe,
// b_de_simulcast). `state` is the set's carried values: de_par_prev, and the
// panning, M/S and contribution values a kept frame repeats.
[[nodiscard]] ParseResult parse_de_data(BitReader& r, const DeConfig& config, int nr_channels,
                                        bool b_iframe, bool b_de_simulcast,
                                        DeParameterState& state, DeData& out) {
    if (nr_channels <= 0) {
        // No parameters are defined this frame, so the next frame's
        // de_par_prev is 0 (Part 1 clause 4.3.14.5.3).
        state.de_par_prev = {};
        return {};
    }
    const int method = config.de_method;
    if ((method == 1 || method == 3) && nr_channels > 1) {
        // The simulcast set sends no panning: the rendered page 140 closes
        // this block after de_mix_coef2_idx, inside b_de_simulcast == 0.
        if (!b_de_simulcast) {
            if (!b_iframe) {
                out.de_keep_pos_flag = r.read_flag("de_keep_pos_flag");
            }
            if (!out.de_keep_pos_flag) {
                state.de_mix_coef1_idx = read_int(r, 5, "de_mix_coef1_idx");
                if (nr_channels == 3) {
                    state.de_mix_coef2_idx = read_int(r, 5, "de_mix_coef2_idx");
                }
            }
        }
    }
    if (!b_iframe) {
        out.de_keep_data_flag = r.read_flag("de_keep_data_flag");
    }
    if (!out.de_keep_data_flag) {
        bool ms = false;
        if ((method == 0 || method == 2) && nr_channels == 2) {
            ms = r.read_flag("de_ms_proc_flag");
        }
        state.de_ms_proc_flag = ms;
        const bool table_1 = method % 2 != 0;
        const Codebook& abs_codebook = table_1 ? tables::kDeHcbAbs1 : tables::kDeHcbAbs0;
        const Codebook& diff_codebook = table_1 ? tables::kDeHcbDiff1 : tables::kDeHcbDiff0;
        DeParameters& par = state.de_par_prev;  // updated in place, as the syntax does
        const int coded_channels = nr_channels - (ms ? 1 : 0);
        int ref_val = 0;
        for (int ch = 0; ch < coded_channels; ++ch) {
            auto& row = par[static_cast<std::size_t>(ch)];
            if (b_iframe && ch == 0) {
                const auto first = read_codeword(r, abs_codebook, "de_par_code");
                if (!first) {
                    return std::unexpected(first.error());
                }
                row[0] = *first - abs_codebook.cb_off;
                ref_val = row[0];
                for (std::size_t band = 1; band < kDeNrBands; ++band) {
                    const auto diff = read_codeword(r, diff_codebook, "de_par_code");
                    if (!diff) {
                        return std::unexpected(diff.error());
                    }
                    row[band] = ref_val + (*diff - diff_codebook.cb_off);
                    ref_val = row[band];
                }
            } else {
                for (std::size_t band = 0; band < kDeNrBands; ++band) {
                    const auto diff = read_codeword(r, diff_codebook, "de_par_code");
                    if (!diff) {
                        return std::unexpected(diff.error());
                    }
                    if (b_iframe) {
                        // Part 2 writes ref_val = de_par[0][band] here, which
                        // would predict band b of channel ch from band b-1 of
                        // channel 0. Part 1 Table 78 writes de_par[ch][band],
                        // the frequency-differential chain the first channel
                        // uses; that is the reading taken.
                        row[band] = ref_val + (*diff - diff_codebook.cb_off);
                        ref_val = row[band];
                    } else {
                        row[band] = row[band] + (*diff - diff_codebook.cb_off);
                    }
                }
            }
            ref_val = row[0];
        }
        // Channels this frame does not code define no parameters.
        for (int ch = coded_channels; ch < kMaxDeChannels; ++ch) {
            par[static_cast<std::size_t>(ch)] = {};
        }
        if (method >= 2) {
            state.de_signal_contribution = read_int(r, 5, "de_signal_contribution");
        }
    }
    out.de_mix_coef1_idx = state.de_mix_coef1_idx;
    out.de_mix_coef2_idx = state.de_mix_coef2_idx;
    out.de_ms_proc_flag = state.de_ms_proc_flag;
    out.de_par = state.de_par_prev;
    out.de_signal_contribution = state.de_signal_contribution;
    return check(r);
}

[[nodiscard]] ParseResult parse_dialog_enhancement(BitReader& r, int mode, bool b_iframe,
                                                   DeState& state, DialogEnhancement& out) {
    out.b_de_data_present = r.read_flag("b_de_data_present");
    if (!out.b_de_data_present) {
        state.data.de_par_prev = {};
        state.core_data.de_par_prev = {};
        if (b_iframe) {
            state.config_valid = false;  // the same I-frame rule as DrcState
        }
        return check(r);
    }
    if (b_iframe) {
        state.config_valid = false;
        if (const ParseResult result = parse_de_config(r, state.config); !result) {
            return result;
        }
        state.config_valid = true;
        out.de_config_present = true;
    } else {
        out.b_de_config_flag = r.read_flag("b_de_config_flag");
        if (out.b_de_config_flag) {
            state.config_valid = false;
            if (const ParseResult result = parse_de_config(r, state.config); !result) {
                return result;
            }
            state.config_valid = true;
            out.de_config_present = true;
        } else if (!state.config_valid) {
            return fail(DecodeError::kMissingIFrame,
                        "dialog_enhancement() in a non-I-frame needs a de_config() and no I-frame "
                        "has supplied one");
        }
    }
    out.config = state.config;
    out.de_nr_channels = de_nr_channels(state.config.de_channel_config);
    if (const ParseResult result = parse_de_data(r, out.config, out.de_nr_channels, b_iframe,
                                                 false, state.data, out.data);
        !result) {
        return result;
    }
    if (mode == ch_mode::k9_0_4 || mode == ch_mode::k9_1_4) {
        out.b_de_simulcast = r.read_flag("b_de_simulcast");
    }
    if (out.b_de_simulcast) {
        // The core set is its own time-differential chain: decoding it
        // against the full set's de_par_prev would mix two parameter sets.
        // It sends no panning of its own, so it shows the full set's.
        if (const ParseResult result = parse_de_data(r, out.config, out.de_nr_channels, b_iframe,
                                                     true, state.core_data, out.core_data);
            !result) {
            return result;
        }
        out.core_data.de_mix_coef1_idx = out.data.de_mix_coef1_idx;
        out.core_data.de_mix_coef2_idx = out.data.de_mix_coef2_idx;
    } else {
        state.core_data.de_par_prev = {};
    }
    return check(r);
}

// --- Part 1 clause 4.2.14.14 emdf_payload_config -----------------------------

[[nodiscard]] ParseResult parse_emdf_payload_config(BitReader& r, EmdfPayloadConfig& out) {
    const bool b_smpoffst = r.read_flag("b_smpoffst");
    if (b_smpoffst) {
        out.smpoffst = r.variable_bits(11, "smpoffst");
    }
    if (r.read_flag("b_duration")) {
        out.duration = r.variable_bits(11, "duration");
    }
    if (r.read_flag("b_groupid")) {
        out.groupid = r.variable_bits(2, "groupid");
    }
    if (r.read_flag("b_codecdata")) {
        out.codecdata = read_int(r, 8, "codecdata");
    }
    out.b_discard_unknown_payload = r.read_flag("b_discard_unknown_payload");
    if (!out.b_discard_unknown_payload) {
        if (!b_smpoffst) {
            out.b_payload_frame_aligned = r.read_flag("b_payload_frame_aligned");
            if (out.b_payload_frame_aligned) {
                out.b_create_duplicate = r.read_flag("b_create_duplicate");
                out.b_remove_duplicate = r.read_flag("b_remove_duplicate");
            }
        }
        if (b_smpoffst || out.b_payload_frame_aligned) {
            out.priority = read_int(r, 5, "priority");
            out.proc_allowed = read_int(r, 2, "proc_allowed");
        }
    }
    return check(r);
}

}  // namespace

// --- Shared ------------------------------------------------------------------

ParseResult read_bit_run(BitReader& r, std::uint64_t bits, std::string_view name) {
    if (bits > r.remaining_bits()) {
        return fail(DecodeError::kTruncated, "a run of data runs past the end of the substream");
    }
    r.read_run(bits, name);
    return {};
}

// --- Part 2 clause 6.2.7.3 further_loudness_info ----------------------------

ParseResult parse_further_loudness_info(BitReader& r, int sus_ver, bool b_presentation_ldn,
                                        FurtherLoudnessInfo& out) {
    out = FurtherLoudnessInfo{};
    const bool full = b_presentation_ldn || sus_ver == 0;
    if (full) {
        out.loudness_version = read_int(r, 2, "loudness_version");
        if (out.loudness_version == 3) {
            out.loudness_version += read_int(r, 4, "extended_loudness_version");
        }
        out.loud_prac_type = read_int(r, 4, "loud_prac_type");
        if (*out.loud_prac_type != 0) {
            out.b_loudcorr_dialgate = r.read_flag("b_loudcorr_dialgate");
            if (out.b_loudcorr_dialgate) {
                out.dialgate_prac_type = read_int(r, 3, "dialgate_prac_type");
            }
            out.b_loudcorr_type = r.read_flag("b_loudcorr_type");
        }
    } else {
        out.b_loudcorr_dialgate = r.read_flag("b_loudcorr_dialgate");
    }
    if (r.read_flag("b_loudrelgat")) {
        out.loudrelgat = read_int(r, 11, "loudrelgat");
    }
    if (r.read_flag("b_loudspchgat")) {
        out.loudspchgat = read_int(r, 11, "loudspchgat");
        out.loudspchgat_dialgate_prac_type = read_int(r, 3, "dialgate_prac_type");
    }
    if (r.read_flag("b_loudstrm3s")) {
        out.loudstrm3s = read_int(r, 11, "loudstrm3s");
    }
    if (r.read_flag("b_max_loudstrm3s")) {
        out.max_loudstrm3s = read_int(r, 11, "max_loudstrm3s");
    }
    if (r.read_flag("b_truepk")) {
        out.truepk = read_int(r, 11, "truepk");
    }
    if (r.read_flag("b_max_truepk")) {
        out.max_truepk = read_int(r, 11, "max_truepk");
    }
    if (full) {
        if (r.read_flag("b_prgmbndy")) {
            // One prgmbndy_bit record per read, until a 1. Past the end of the
            // data the reader returns zeros, so the overflow check is what
            // ends a run of zeros no 1 closes.
            int reads = 0;
            bool bit = false;
            while (!bit) {
                bit = r.read_flag("prgmbndy_bit");
                ++reads;
                if (r.overflow()) {
                    return check(r);
                }
            }
            out.prgmbndy_bits = reads;
            out.b_end_or_start = r.read_flag("b_end_or_start");
            if (r.read_flag("b_prgmbndy_offset")) {
                out.prgmbndy_offset = read_int(r, 11, "prgmbndy_offset");
            }
        }
    }
    if (r.read_flag("b_lra")) {
        out.lra = read_int(r, 10, "lra");
        out.lra_prac_type = read_int(r, 3, "lra_prac_type");
    }
    if (r.read_flag("b_loudmntry")) {
        out.loudmntry = read_int(r, 11, "loudmntry");
    }
    if (r.read_flag("b_max_loudmntry")) {
        out.max_loudmntry = read_int(r, 11, "max_loudmntry");
    }
    if (sus_ver >= 1) {
        if (r.read_flag("b_rtllcomp")) {
            out.rtll_comp = read_int(r, 8, "rtll_comp");
        }
        if (r.read_flag("b_extension")) {
            std::uint64_t e_bits_size = r.read(5, "e_bits_size");
            if (e_bits_size == 31) {
                e_bits_size += r.variable_bits(4, "e_bits_size");
            }
            out.e_bits_size = e_bits_size;
            if (const ParseResult result = check(r); !result) {
                return result;
            }
            out.extensions_bits = e_bits_size;
            if (const ParseResult result = read_bit_run(r, e_bits_size, "extensions_bits");
                !result) {
                return result;
            }
        }
    } else {
        // sus_ver 0 keeps Part 1's extension and finds b_rtllcomp and
        // rtll_comp at the front of its extension bits, so e_bits_size counts
        // them.
        if (r.read_flag("b_extension")) {
            std::uint64_t e_bits_size = r.read(5, "e_bits_size");
            if (e_bits_size == 31) {
                e_bits_size += r.variable_bits(4, "e_bits_size");
            }
            out.e_bits_size = e_bits_size;
            std::uint64_t carried = 1;
            if (r.read_flag("b_rtllcomp")) {
                out.rtll_comp = read_int(r, 8, "rtll_comp");
                carried = 9;
            }
            if (const ParseResult result = check(r); !result) {
                return result;
            }
            if (e_bits_size < carried) {
                return fail(DecodeError::kInvalidStream,
                            "e_bits_size is smaller than the b_rtllcomp and rtll_comp it carries");
            }
            out.extensions_bits = e_bits_size - carried;
            if (const ParseResult result = read_bit_run(r, out.extensions_bits, "extensions_bits");
                !result) {
                return result;
            }
        }
    }
    return check(r);
}

// --- DRC helpers and drc_frame ---------------------------------------------

int nr_drc_channels(const DrcContext& ctx) noexcept {
    switch (ctx.ch_mode) {
        case ch_mode::kMono:
        case ch_mode::kStereo:
            return 1;
        case ch_mode::k5_0:
        case ch_mode::k5_1:
        case ch_mode::k7_0_340:
        case ch_mode::k7_1_340:
        case ch_mode::k7_0_520:
        case ch_mode::k7_1_520:
        case ch_mode::k7_0_322:
        case ch_mode::k7_1_322:
            return 3;
        case ch_mode::k7_0_4:
        case ch_mode::k7_1_4:
        case ch_mode::k9_0_4:
        case ch_mode::k9_1_4:
        case ch_mode::k22_2:
            return 4;
        default:
            return -1;
    }
}

int nr_drc_subframes(int frame_len_base) noexcept {
    switch (frame_len_base) {
        case 384:
            return 1;
        case 512:
            return 2;
        case 768:
        case 960:
            return 3;
        case 1024:
            return 4;
        case 1536:
        case 1920:
            return 6;
        case 2048:
            return 8;
        default:
            return -1;
    }
}

ParseResult parse_drc_frame(BitReader& r, const DrcContext& ctx, DrcState& state, DrcFrame& out) {
    out = DrcFrame{};
    out.b_drc_present = r.read_flag("b_drc_present");
    if (!out.b_drc_present) {
        if (ctx.b_iframe) {
            state.config_valid = false;
        }
        return check(r);
    }
    if (ctx.b_iframe) {
        state.config_valid = false;
        if (const ParseResult result = parse_drc_config(r, state.config); !result) {
            return result;
        }
        state.config_valid = true;
        out.drc_config_present = true;
    } else if (!state.config_valid) {
        return fail(DecodeError::kMissingIFrame,
                    "drc_data() in a non-I-frame needs a drc_config() and no I-frame has supplied "
                    "one");
    }
    return parse_drc_data(r, ctx, state.config, out);
}

// --- Dialogue enhancement helper ---------------------------------------------

int de_nr_channels(int de_channel_config) noexcept {
    switch (de_channel_config) {
        case 0b001:  // Centre
        case 0b010:  // Right
        case 0b100:  // Left
            return 1;
        case 0b011:  // Right, Centre
        case 0b101:  // Left, Centre
        case 0b110:  // Left, Right
            return 2;
        case 0b111:  // Left, Right, Centre
            return 3;
        default:  // 'No parameters'
            return 0;
    }
}

// --- Part 1 clause 4.2.4.4 emdf_payloads_substream ---------------------------

ParseResult parse_emdf_payloads_substream(BitReader& r, EmdfPayloads& out) {
    out.payloads.clear();
    // while (emdf_payload_id != 0) reads the id as part of the test: an id of
    // 0 ends the loop with nothing after it. Past the end of the data the
    // reader returns zeros, so a truncated substream ends the loop too.
    while (true) {
        std::uint64_t id = r.read(5, "emdf_payload_id");
        if (id == 0) {
            break;
        }
        if (id == 31) {
            // The escape carries the whole range variable_bits() can express,
            // so the sum is kept in 64 bits; the payload id is only compared,
            // never used as an index.
            id += r.variable_bits(5, "emdf_payload_id");
        }
        EmdfPayload payload;
        payload.emdf_payload_id = id;
        if (const ParseResult result = parse_emdf_payload_config(r, payload.config); !result) {
            return result;
        }
        const std::uint64_t size = r.variable_bits(8, "emdf_payload_size");
        if (const ParseResult result = check(r); !result) {
            return result;
        }
        if (size * 8U > r.remaining_bits()) {
            return fail(DecodeError::kTruncated,
                        "emdf_payload_size runs past the end of the substream");
        }
        payload.bytes.reserve(static_cast<std::size_t>(size));
        for (std::uint64_t i = 0; i < size; ++i) {
            payload.bytes.push_back(static_cast<std::uint8_t>(r.read(8, "emdf_payload_byte")));
        }
        out.payloads.push_back(std::move(payload));
    }
    if (const ParseResult result = check(r); !result) {
        return result;
    }
    r.align();
    return check(r);
}

// --- Part 2 clause 6.2.7.1 metadata ------------------------------------------

ParseResult parse_metadata(BitReader& r, const SubstreamContext& ctx, MetadataState& state,
                           Metadata& out) {
    out = Metadata{};
    if (const ParseResult result = parse_basic_metadata(r, ctx, out.basic); !result) {
        return result;
    }
    if (const ParseResult result = parse_extended_metadata(r, ctx, out.extended); !result) {
        return result;
    }
    std::uint64_t tools_metadata_size = r.read(7, "tools_metadata_size_value");
    if (r.read_flag("b_more_bits")) {
        tools_metadata_size += std::uint64_t{r.variable_bits(3, "tools_metadata_size")} << 7U;
    }
    out.tools_metadata_size = tools_metadata_size;
    if (const ParseResult result = check(r); !result) {
        return result;
    }
    const std::size_t tools_start = r.position();
    if (ctx.sus_ver == 0) {
        const DrcContext drc_ctx{
            .b_iframe = ctx.b_iframe,
            .ch_mode = ctx.ch_mode,
            .frame_len_base = ctx.frame_len_base,
        };
        DrcFrame drc;
        if (const ParseResult result = parse_drc_frame(r, drc_ctx, state.drc, drc); !result) {
            return result;
        }
        out.drc = drc;
    }
    if (const ParseResult result = parse_dialog_enhancement(r, ctx.ch_mode, ctx.b_iframe,
                                                            state.de, out.dialog_enhancement);
        !result) {
        return result;
    }
    // Part 1 clause 4.3.12.1.1: the size in bits of the DRC and dialogue
    // enhancement metadata, which is exactly what lies between here and the
    // size field.
    if (r.position() - tools_start != tools_metadata_size) {
        return fail(DecodeError::kInvalidStream,
                    "tools_metadata_size does not match the drc_frame() and dialog_enhancement() "
                    "read");
    }
    out.b_emdf_payloads_substream = r.read_flag("b_emdf_payloads_substream");
    if (out.b_emdf_payloads_substream) {
        if (const ParseResult result = parse_emdf_payloads_substream(r, out.emdf_payloads);
            !result) {
            return result;
        }
    }
    return check(r);
}

}  // namespace ac4::detail
