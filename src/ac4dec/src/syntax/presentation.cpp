#include "syntax/presentation.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace ac4::detail {

namespace {

[[nodiscard]] int read_int(BitReader& r, int bits, std::string_view name) {
    return static_cast<int>(r.read(bits, name));
}

// --- Channel sets for superset() ---------------------------------------------

// One bit per loudspeaker position, enough to say which channel modes hold
// which. The 3/2/2 modes' top pair is taken as 7.X.4's Tfl and Tfr: Part 2
// renamed Part 1's b_vhl_active and b_vhr_active to b_tfl_active and
// b_tfr_active.
constexpr std::uint32_t kL = 1U << 0U;
constexpr std::uint32_t kR = 1U << 1U;
constexpr std::uint32_t kC = 1U << 2U;
constexpr std::uint32_t kLfe = 1U << 3U;
constexpr std::uint32_t kLs = 1U << 4U;
constexpr std::uint32_t kRs = 1U << 5U;
constexpr std::uint32_t kLb = 1U << 6U;
constexpr std::uint32_t kRb = 1U << 7U;
constexpr std::uint32_t kLw = 1U << 8U;
constexpr std::uint32_t kRw = 1U << 9U;
constexpr std::uint32_t kTfl = 1U << 10U;
constexpr std::uint32_t kTfr = 1U << 11U;
constexpr std::uint32_t kTbl = 1U << 12U;
constexpr std::uint32_t kTbr = 1U << 13U;
constexpr std::uint32_t kLscr = 1U << 14U;
constexpr std::uint32_t kRscr = 1U << 15U;
constexpr std::uint32_t kLfe2 = 1U << 16U;
constexpr std::uint32_t kTsl = 1U << 17U;
constexpr std::uint32_t kTsr = 1U << 18U;
constexpr std::uint32_t kTfc = 1U << 19U;
constexpr std::uint32_t kTbc = 1U << 20U;
constexpr std::uint32_t kTc = 1U << 21U;
constexpr std::uint32_t kBfl = 1U << 22U;
constexpr std::uint32_t kBfr = 1U << 23U;
constexpr std::uint32_t kBfc = 1U << 24U;
constexpr std::uint32_t kCb = 1U << 25U;

constexpr std::uint32_t k5_0 = kL | kR | kC | kLs | kRs;
constexpr std::uint32_t k7_0_4 = k5_0 | kLb | kRb | kTfl | kTfr | kTbl | kTbr;

// By ch_mode: Part 2 Table 56 and, for 9.X.4 and 22.2, the channel groups of
// Table 69.
constexpr std::array<std::uint32_t, 16> kChModeChannels = {
    kC,                                    // 0 mono
    kL | kR,                               // 1 stereo
    kL | kR | kC,                          // 2 3.0
    k5_0,                                  // 3 5.0
    k5_0 | kLfe,                           // 4 5.1
    k5_0 | kLb | kRb,                      // 5 7.0: 3/4/0
    k5_0 | kLb | kRb | kLfe,               // 6 7.1: 3/4/0.1
    k5_0 | kLw | kRw,                      // 7 7.0: 5/2/0
    k5_0 | kLw | kRw | kLfe,               // 8 7.1: 5/2/0.1
    k5_0 | kTfl | kTfr,                    // 9 7.0: 3/2/2
    k5_0 | kTfl | kTfr | kLfe,             // 10 7.1: 3/2/2.1
    k7_0_4,                                // 11 7.0.4
    k7_0_4 | kLfe,                         // 12 7.1.4
    k7_0_4 | kLscr | kRscr,                // 13 9.0.4
    k7_0_4 | kLscr | kRscr | kLfe,         // 14 9.1.4
    k7_0_4 | kLfe | kLfe2 | kLw | kRw | kBfl | kBfr | kBfc | kCb | kTsl | kTsr | kTfc | kTbc |
        kTc,                               // 15 22.2
};

// By ch_mode_core, Table 71; 0 to 2 as the matching ch_modes so the
// superset(0, 1) rule reads the same.
constexpr std::array<std::uint32_t, 7> kChModeCoreChannels = {
    kC,
    kL | kR,
    kL | kR | kC,
    k5_0,                        // 3 5.0
    k5_0 | kLfe,                 // 4 5.1
    k5_0 | kTfl | kTfr,          // 5 5.0.2 core
    k5_0 | kTfl | kTfr | kLfe,   // 6 5.1.2 core
};

template <std::size_t N>
[[nodiscard]] int superset_in(const std::array<std::uint32_t, N>& channels, int a,
                              int b) noexcept {
    if (a < 0) {
        return b;
    }
    if (b < 0) {
        return a;
    }
    if ((a == 0 && b == 1) || (a == 1 && b == 0)) {
        return 1;  // "The result of superset(0,1) shall be 1."
    }
    if (static_cast<std::size_t>(a) >= N || static_cast<std::size_t>(b) >= N) {
        return -1;
    }
    const std::uint32_t wanted =
        channels[static_cast<std::size_t>(a)] | channels[static_cast<std::size_t>(b)];
    for (std::size_t mode = 0; mode < N; ++mode) {
        if ((channels[mode] & wanted) == wanted) {
            return static_cast<int>(mode);
        }
    }
    return -1;
}

// Part 2 Pseudocode 30.
[[nodiscard]] bool contains_lfe(int mode) noexcept {
    return mode == ch_mode::k5_1 || mode == ch_mode::k7_1_340 || mode == ch_mode::k7_1_520 ||
           mode == ch_mode::k7_1_322 || mode == ch_mode::k7_1_4 || mode == ch_mode::k9_1_4 ||
           mode == ch_mode::k22_2;
}

// Part 1 Table 83 by frame_rate_index (the column "for external 48 kHz =
// frame_len_base").
constexpr std::array<int, 14> kFrameLenBase48 = {1920, 1920, 2048, 1536, 1536, 960, 960,
                                                 1024, 768,  768,  512,  384,  384, 2048};

[[nodiscard]] int frame_len_base_of(const ac4::Toc& toc) noexcept {
    if (toc.sample_rate_hz == 44100) {
        return toc.frame_rate_index == 13 ? 2048 : 0;  // Table 84
    }
    if (toc.frame_rate_index < 0 ||
        static_cast<std::size_t>(toc.frame_rate_index) >= kFrameLenBase48.size()) {
        return 0;
    }
    return kFrameLenBase48[static_cast<std::size_t>(toc.frame_rate_index)];
}

// --- Part 2 clause 6.2.2.5 advanced_de_data ----------------------------------

[[nodiscard]] ParseResult parse_advanced_de_data(BitReader& r, bool b_pres_ndot,
                                                 PresentationSubstreamState& state,
                                                 AdvancedDeData& out) {
    out.b_advanced_de_config_present = r.read_flag("b_advanced_de_config_present");
    if (out.b_advanced_de_config_present) {
        AdvancedDeConfig config;
        config.advanced_de_compr_tc_attack = read_int(r, 6, "advanced_de_compr_tc_attack");
        config.advanced_de_compr_tc_release = read_int(r, 6, "advanced_de_compr_tc_release");
        config.advanced_de_compr_ratio = read_int(r, 4, "advanced_de_compr_ratio");
        state.advanced_de_config = config;
    } else if (b_pres_ndot) {
        state.advanced_de_config.reset();
    }
    out.config = state.advanced_de_config;
    const int thresh = read_int(r, 6, "advanced_de_compr_thresh");
    out.advanced_de_compr_thresh = thresh >= 32 ? thresh - 64 : thresh;
    out.advanced_de_compr_gain = read_int(r, 5, "advanced_de_compr_gain");
    return check(r);
}

// --- Part 2 clauses 6.2.9.4 to 6.2.9.10 the downmix tools -------------------

void tool_scr_to_c_l(BitReader& r, CdmxParameters& out) {
    out.b_put_screen_to_c = r.read_flag("b_put_screen_to_c");
    if (*out.b_put_screen_to_c) {
        out.gain_f1_code = read_int(r, 3, "gain_f1_code");
    } else {
        out.gain_f2_code = read_int(r, 3, "gain_f2_code");
    }
}

void tool_b4_to_b2(BitReader& r, CdmxParameters& out) {
    out.gain_b_code = read_int(r, 3, "gain_b_code");
}

void tool_t4_to_t2(BitReader& r, CdmxParameters& out) {
    out.gain_t1_code = read_int(r, 3, "gain_t1_code");
}

void tool_t4_to_f_s_b(BitReader& r, CdmxParameters& out) {
    out.b_top_front_to_front = r.read_flag("b_top_front_to_front");
    if (*out.b_top_front_to_front) {
        out.gain_t2a_code = read_int(r, 3, "gain_t2a_code");
        out.gain_t2b_code = 7;
    } else {
        out.b_top_front_to_side = r.read_flag("b_top_front_to_side");
        if (*out.b_top_front_to_side) {
            out.gain_t2b_code = read_int(r, 3, "gain_t2b_code");
        } else {
            out.gain_t2c_code = read_int(r, 3, "gain_t2c_code");
            out.gain_t2b_code = 7;
        }
    }
    out.b_top_back_to_front = r.read_flag("b_top_back_to_front");
    if (*out.b_top_back_to_front) {
        out.gain_t2d_code = read_int(r, 3, "gain_t2d_code");
        out.gain_t2e_code = 7;
    } else {
        out.b_top_back_to_side = r.read_flag("b_top_back_to_side");
        if (*out.b_top_back_to_side) {
            out.gain_t2e_code = read_int(r, 3, "gain_t2e_code");
        } else {
            out.gain_t2f_code = read_int(r, 3, "gain_t2f_code");
            out.gain_t2e_code = 7;
        }
    }
}

void tool_t4_to_f_s(BitReader& r, CdmxParameters& out) {
    out.b_top_front_to_front = r.read_flag("b_top_front_to_front");
    if (*out.b_top_front_to_front) {
        out.gain_t2a_code = read_int(r, 3, "gain_t2a_code");
        out.gain_t2b_code = 7;
    } else {
        out.gain_t2b_code = read_int(r, 3, "gain_t2b_code");
    }
    out.b_top_back_to_front = r.read_flag("b_top_back_to_front");
    if (*out.b_top_back_to_front) {
        out.gain_t2d_code = read_int(r, 3, "gain_t2d_code");
        out.gain_t2e_code = 7;
    } else {
        out.gain_t2e_code = read_int(r, 3, "gain_t2e_code");
    }
}

void tool_t2_to_f_s_b(BitReader& r, CdmxParameters& out) {
    out.b_top_to_front = r.read_flag("b_top_to_front");
    if (*out.b_top_to_front) {
        out.gain_t2a_code = read_int(r, 3, "gain_t2a_code");
        out.gain_t2b_code = 7;
    } else {
        out.b_top_to_side = r.read_flag("b_top_to_side");
        if (*out.b_top_to_side) {
            out.gain_t2b_code = read_int(r, 3, "gain_t2b_code");
        } else {
            out.gain_t2c_code = read_int(r, 3, "gain_t2c_code");
            out.gain_t2b_code = 7;
        }
    }
}

void tool_t2_to_f_s(BitReader& r, CdmxParameters& out) {
    out.b_top_to_front = r.read_flag("b_top_to_front");
    if (*out.b_top_to_front) {
        out.gain_t2a_code = read_int(r, 3, "gain_t2a_code");
        out.gain_t2b_code = 7;
    } else {
        out.gain_t2b_code = read_int(r, 3, "gain_t2b_code");
    }
}

// --- Part 2 clause 6.2.9.3 cdmx_parameters -----------------------------------

void parse_cdmx_parameters(BitReader& r, int bs_ch_config, CdmxParameters& out) {
    if (bs_ch_config == 0 || bs_ch_config == 3) {
        tool_scr_to_c_l(r, out);
    }
    // Out_ch_config values the switches below do not list (Table 127's
    // unused 5 to 7, and the ones needing no downmix tool) read nothing.
    if (bs_ch_config < 2) {
        switch (out.out_ch_config) {
            case 0:
                tool_t4_to_f_s(r, out);
                tool_b4_to_b2(r, out);
                break;
            case 1:
                tool_t4_to_t2(r, out);
                tool_b4_to_b2(r, out);
                break;
            case 2:
                tool_b4_to_b2(r, out);
                break;
            case 3:
                tool_t4_to_f_s_b(r, out);
                break;
            case 4:
                tool_t4_to_t2(r, out);
                break;
            default:
                break;
        }
    }
    if (bs_ch_config == 2) {
        switch (out.out_ch_config) {
            case 0:
                tool_t4_to_f_s(r, out);
                break;
            case 1:
                tool_t4_to_t2(r, out);
                break;
            default:
                break;
        }
    }
    // The table writes 3 <= bs_ch_config <= 4, a range: read literally as C
    // it would hold for every bs_ch_config.
    if (bs_ch_config >= 3 && bs_ch_config <= 4) {
        switch (out.out_ch_config) {
            case 0:
                tool_t2_to_f_s(r, out);
                tool_b4_to_b2(r, out);
                break;
            case 1:
            case 2:
                tool_b4_to_b2(r, out);
                break;
            case 3:
                tool_t2_to_f_s_b(r, out);
                break;
            default:
                break;
        }
    }
    if (bs_ch_config == 5) {
        if (out.out_ch_config == 0) {
            tool_t2_to_f_s(r, out);
        }
    }
}

// --- Part 2 clause 6.2.9.2 custom_dmx_data -----------------------------------

[[nodiscard]] ParseResult parse_custom_dmx_data(BitReader& r, const PresentationContext& ctx,
                                                CustomDmxData& out) {
    const int mode = ctx.pres_ch_mode;
    const bool back = ctx.b_pres_4_back_channels_present;
    int bs_ch_config = -1;
    if (mode >= ch_mode::k7_0_4 && mode <= ch_mode::k9_1_4) {
        if (ctx.pres_top_channel_pairs == 2) {
            if (mode >= ch_mode::k9_0_4 && back) {
                bs_ch_config = 0;
            }
            if (mode <= ch_mode::k7_1_4) {
                bs_ch_config = back ? 1 : 2;
            }
        }
        if (ctx.pres_top_channel_pairs == 1) {
            if (mode >= ch_mode::k9_0_4 && back) {
                bs_ch_config = 3;
            }
            if (mode <= ch_mode::k7_1_4) {
                bs_ch_config = back ? 4 : 5;
            }
        }
    }
    out.bs_ch_config = bs_ch_config;
    if (bs_ch_config >= 0) {
        out.b_cdmx_data_present = r.read_flag("b_cdmx_data_present");
        if (out.b_cdmx_data_present) {
            out.n_cdmx_configs = read_int(r, 2, "n_cdmx_configs_minus1") + 1;
            const int out_ch_config_bits = (bs_ch_config == 2 || bs_ch_config == 5) ? 1 : 3;
            for (int dc = 0; dc < out.n_cdmx_configs; ++dc) {
                CdmxParameters& parameters = out.cdmx[static_cast<std::size_t>(dc)];
                parameters.out_ch_config = read_int(r, out_ch_config_bits, "out_ch_config");
                parse_cdmx_parameters(r, bs_ch_config, parameters);
            }
        }
    }
    if (mode >= ch_mode::k5_0 || ctx.pres_ch_mode_core >= 3) {
        if (r.read_flag("b_stereo_dmx_coeff")) {
            StereoDmxCoeff coeff;
            coeff.loro_centre_mixgain = read_int(r, 3, "loro_centre_mixgain");
            coeff.loro_surround_mixgain = read_int(r, 3, "loro_surround_mixgain");
            coeff.b_ltrt_mixinfo = r.read_flag("b_ltrt_mixinfo");
            if (coeff.b_ltrt_mixinfo) {
                coeff.ltrt_centre_mixgain = read_int(r, 3, "ltrt_centre_mixgain");
                coeff.ltrt_surround_mixgain = read_int(r, 3, "ltrt_surround_mixgain");
            }
            if (ctx.b_pres_has_lfe) {
                if (r.read_flag("b_lfe_mixinfo")) {
                    coeff.lfe_mixgain = read_int(r, 5, "lfe_mixgain");
                }
            }
            coeff.preferred_dmx_method = read_int(r, 2, "preferred_dmx_method");
            out.stereo_dmx_coeff = coeff;
        }
    }
    return check(r);
}

// --- Part 2 clause 6.2.9.1 loud_corr -----------------------------------------

[[nodiscard]] ParseResult parse_loud_corr(BitReader& r, const PresentationContext& ctx,
                                          LoudCorr& out) {
    const int mode = ctx.pres_ch_mode;
    const int core = ctx.pres_ch_mode_core;
    const bool b_objects = mode == -1;  // set by ac4_presentation_substream() before the call
    if (b_objects) {
        out.b_obj_loud_corr = r.read_flag("b_obj_loud_corr");
    }
    const bool obj = out.b_obj_loud_corr;
    auto corr = [&r](std::optional<int>& field, std::string_view name) {
        if (r.read_flag("b_loud_comp")) {
            field = read_int(r, 5, name);
        }
    };
    if (mode > 4 || obj) {
        out.b_corr_for_immersive_out = r.read_flag("b_corr_for_immersive_out");
    }
    if (mode > 1 || obj) {
        if (r.read_flag("b_loro_loud_comp")) {
            out.loro_dmx_loud_corr = read_int(r, 5, "loro_dmx_loud_corr");
        }
        if (r.read_flag("b_ltrt_loud_comp")) {
            out.ltrt_dmx_loud_corr = read_int(r, 5, "ltrt_dmx_loud_corr");
        }
    }
    if (mode > 4 || obj) {
        corr(out.loud_corr_5_X, "loud_corr_5_X");
        if (out.b_corr_for_immersive_out) {
            corr(out.loud_corr_5_X_2, "loud_corr_5_X_2");
            corr(out.loud_corr_7_X, "loud_corr_7_X");
        }
    }
    if (mode > 10 || obj) {
        if (out.b_corr_for_immersive_out) {
            corr(out.loud_corr_7_X_4, "loud_corr_7_X_4");
            corr(out.loud_corr_7_X_2, "loud_corr_7_X_2");
            corr(out.loud_corr_5_X_4, "loud_corr_5_X_4");
        }
    }
    if (core >= 5) {
        corr(out.loud_corr_core_5_X_2, "loud_corr_core_5_X_2");
    }
    if (core >= 3) {
        corr(out.loud_corr_core_5_X, "loud_corr_core_5_X");
        if (r.read_flag("b_loud_comp")) {
            out.loud_corr_core_loro = read_int(r, 5, "loud_corr_core_loro");
            out.loud_corr_core_ltrt = read_int(r, 5, "loud_corr_core_ltrt");
        }
    }
    if (obj) {
        corr(out.loud_corr_9_X_4, "loud_corr_9_X_4");
    }
    return check(r);
}

}  // namespace

// --- Table of contents to PresentationContext ---------------------------------

int superset_ch_mode(int a, int b) noexcept {
    return superset_in(kChModeChannels, a, b);
}

int superset_ch_mode_core(int a, int b) noexcept {
    return superset_in(kChModeCoreChannels, a, b);
}

PresentationContext presentation_context_v1(const ac4::Toc& toc,
                                            const ac4::PresentationInfoV1& presentation) {
    PresentationContext ctx;
    ctx.b_alternative = presentation.b_alternative;
    ctx.b_pres_ndot = presentation.b_pres_ndot;
    if (!presentation.presentation_config) {
        ctx.n_substream_groups = 1;
    } else {
        switch (*presentation.presentation_config) {
            case 0:
            case 2:
            case 4:
                ctx.n_substream_groups = 2;
                break;
            case 1:
                ctx.n_substream_groups = 1;
                break;
            case 3:
                ctx.n_substream_groups = 3;
                break;
            case 5:
                ctx.n_substream_groups = static_cast<int>(presentation.group_refs.size());
                break;
            default:
                ctx.n_substream_groups = 0;
                break;
        }
    }

    int pres_ch_mode = -1;
    bool b_obj_or_ajoc = false;
    int pres_ch_mode_core = -1;
    bool b_obj_or_ajoc_adaptive = false;
    for (const int group_index : presentation.group_refs) {
        // A group the table of contents does not hold (b_multi_pid puts it in
        // another elementary stream) contributes nothing.
        if (group_index < 0 ||
            static_cast<std::size_t>(group_index) >= toc.substream_groups.size()) {
            continue;
        }
        const ac4::SubstreamGroupInfo& group =
            toc.substream_groups[static_cast<std::size_t>(group_index)];
        for (const ac4::GroupSubstream& substream : group.substreams) {
            ++ctx.n_substreams_in_presentation;
            switch (substream.kind) {
                case ac4::GroupSubstream::Kind::kChan:
                    if (substream.chan) {
                        const ac4::ChannelSubstreamInfo& chan = *substream.chan;
                        if (chan.ch_mode) {
                            const int mode = *chan.ch_mode;
                            pres_ch_mode = superset_ch_mode(pres_ch_mode, mode);
                            // Table 71: the channel-coded rows.
                            int mode_core = -1;
                            if (mode == ch_mode::k7_0_4 || mode == ch_mode::k9_0_4) {
                                mode_core = 5;
                            } else if (mode == ch_mode::k7_1_4 || mode == ch_mode::k9_1_4) {
                                mode_core = 6;
                            }
                            pres_ch_mode_core =
                                superset_ch_mode_core(pres_ch_mode_core, mode_core);
                        }
                        if (chan.original_content) {
                            const ac4::OriginalContent& content = *chan.original_content;
                            ctx.b_pres_4_back_channels_present =
                                ctx.b_pres_4_back_channels_present ||
                                content.b_4_back_channels_present;
                            const int pairs = content.top_channels_present == 3   ? 2
                                              : content.top_channels_present > 0 ? 1
                                                                                 : 0;
                            ctx.pres_top_channel_pairs =
                                std::max(ctx.pres_top_channel_pairs, pairs);
                        }
                    }
                    break;
                case ac4::GroupSubstream::Kind::kAjoc:
                    b_obj_or_ajoc = true;
                    if (substream.ajoc && substream.ajoc->b_static_dmx) {
                        pres_ch_mode_core = superset_ch_mode_core(
                            pres_ch_mode_core, substream.ajoc->b_lfe ? 4 : 3);
                    } else {
                        b_obj_or_ajoc_adaptive = true;
                    }
                    break;
                case ac4::GroupSubstream::Kind::kObj:
                    b_obj_or_ajoc = true;
                    b_obj_or_ajoc_adaptive = true;
                    break;
            }
        }
    }
    if (b_obj_or_ajoc) {
        pres_ch_mode = -1;
    }
    if (b_obj_or_ajoc_adaptive) {
        pres_ch_mode_core = -1;
    }
    if (pres_ch_mode_core == pres_ch_mode) {
        pres_ch_mode_core = -1;
    }
    ctx.pres_ch_mode = pres_ch_mode;
    ctx.pres_ch_mode_core = pres_ch_mode_core;
    ctx.b_pres_has_lfe = pres_ch_mode >= 0 ? contains_lfe(pres_ch_mode)
                                           : (pres_ch_mode_core == 4 || pres_ch_mode_core == 6);
    ctx.frame_len_base = frame_len_base_of(toc);
    return ctx;
}

// --- Part 2 clause 6.2.2.3 ac4_presentation_substream -------------------------

ParseResult parse_presentation_substream(BitReader& r, const PresentationContext& ctx,
                                         PresentationSubstreamState& state,
                                         PresentationSubstream& out) {
    out = PresentationSubstream{};
    if (ctx.b_alternative) {
        out.b_name_present = r.read_flag("b_name_present");
        if (out.b_name_present) {
            int name_len = 32;
            if (r.read_flag("b_length")) {
                name_len = read_int(r, 5, "name_len");
            }
            // presentation_name is name_len * 8 bits the syntax reads as a
            // run of bytes: one record per byte.
            for (int i = 0; i < name_len; ++i) {
                out.presentation_name.push_back(
                    static_cast<std::uint8_t>(r.read(8, "presentation_name")));
            }
        }
        std::uint64_t n_targets = std::uint64_t{r.read(2, "n_targets_minus1")} + 1;
        if (n_targets == 4) {
            n_targets += r.variable_bits(2, "n_targets");
        }
        for (std::uint64_t t = 0; t < n_targets; ++t) {
            PresentationTarget target;
            target.target_level = read_int(r, 3, "target_level");
            target.target_device_category = read_int(r, 4, "target_device_category");
            if (r.read_flag("b_tdc_extension")) {
                target.reserved_bits = read_int(r, 4, "reserved_bits");
            }
            if (r.read_flag("b_ducking_depth_present")) {
                target.max_ducking_depth = read_int(r, 6, "max_ducking_depth");
            }
            if (r.read_flag("b_loud_corr_target")) {
                target.loud_corr_target = read_int(r, 5, "loud_corr_target");
            }
            for (int sus = 0; sus < ctx.n_substreams_in_presentation; ++sus) {
                if (r.read_flag("b_active")) {
                    std::uint32_t alt_data_set_index = r.read(1, "alt_data_set_index");
                    if (alt_data_set_index == 1) {
                        alt_data_set_index += r.variable_bits(2, "alt_data_set_index");
                    }
                    target.alt_data_set_index.emplace_back(alt_data_set_index);
                } else {
                    target.alt_data_set_index.emplace_back(std::nullopt);
                }
                if (r.overflow()) {
                    break;
                }
            }
            // n_targets can come from variable_bits(); the data running out
            // is what ends a count no data backs.
            if (const ParseResult result = check(r); !result) {
                return result;
            }
            out.targets.push_back(std::move(target));
        }
    }

    out.b_additional_data = r.read_flag("b_additional_data");
    if (out.b_additional_data) {
        std::uint64_t add_data_bytes = std::uint64_t{r.read(4, "add_data_bytes_minus1")} + 1;
        if (add_data_bytes == 16) {
            add_data_bytes += r.variable_bits(2, "add_data_bytes");
        }
        out.add_data_bytes = add_data_bytes;
        r.align();
        if (const ParseResult result = check(r); !result) {
            return result;
        }
        // Everything down to add_data fills the add_data_bytes that start at
        // this byte boundary.
        const std::uint64_t add_data_bits = add_data_bytes * 8U;
        const std::size_t start = r.position();
        out.immersive_audio_indicator = r.read_flag("immersive_audio_indicator");
        if (ctx.pres_ch_mode == -1) {
            out.b_oamd_common_timing = r.read_flag("b_oamd_common_timing");
        }
        const bool b_advanced_de_data_present = r.read_flag("b_advanced_de_data_present");
        if (b_advanced_de_data_present) {
            AdvancedDeData advanced;
            if (const ParseResult result =
                    parse_advanced_de_data(r, ctx.b_pres_ndot, state, advanced);
                !result) {
                return result;
            }
            out.advanced_de_data = advanced;
        }
        if (const ParseResult result = check(r); !result) {
            return result;
        }
        const std::uint64_t used = r.position() - start;
        if (used > add_data_bits) {
            return fail(DecodeError::kInvalidStream,
                        "add_data_bytes is too small for the fields read inside it");
        }
        out.add_data_bits = add_data_bits - used;
        if (const ParseResult result = read_bit_run(r, out.add_data_bits, "add_data"); !result) {
            return result;
        }
    }
    // "If the advanced DE data element is not present in an I-frame, then
    // A-DE is disabled": no configuration survives such a frame.
    if (ctx.b_pres_ndot && !out.advanced_de_data) {
        state.advanced_de_config.reset();
    }

    out.dialnorm_bits = read_int(r, 7, "dialnorm_bits");
    if (r.read_flag("b_further_loudness_info")) {
        FurtherLoudnessInfo info;
        if (const ParseResult result = parse_further_loudness_info(r, 1, true, info); !result) {
            return result;
        }
        out.further_loudness_info = info;
    }

    std::uint64_t drc_metadata_size = r.read(5, "drc_metadata_size_value");
    if (r.read_flag("b_more_bits")) {
        drc_metadata_size += std::uint64_t{r.variable_bits(3, "drc_metadata_size")} << 5U;
    }
    out.drc_metadata_size = drc_metadata_size;
    if (const ParseResult result = check(r); !result) {
        return result;
    }
    const std::size_t drc_start = r.position();
    const DrcContext drc_ctx{
        .b_iframe = ctx.b_pres_ndot,
        .ch_mode = ctx.pres_ch_mode,
        .frame_len_base = ctx.frame_len_base,
    };
    if (const ParseResult result = parse_drc_frame(r, drc_ctx, state.drc, out.drc); !result) {
        return result;
    }
    // Part 2 clause 6.3.3.1.19: "the size of the drc_frame() element, in bits".
    if (r.position() - drc_start != drc_metadata_size) {
        return fail(DecodeError::kInvalidStream,
                    "drc_metadata_size does not match the drc_frame() read");
    }

    if (ctx.n_substream_groups > 1) {
        out.b_substream_group_gains_present = r.read_flag("b_substream_group_gains_present");
        if (out.b_substream_group_gains_present) {
            const auto groups = static_cast<std::size_t>(ctx.n_substream_groups);
            out.b_keep = r.read_flag("b_keep");
            if (!out.b_keep) {
                state.sg_gain.assign(groups, 0);
                for (std::size_t sg = 0; sg < groups; ++sg) {
                    state.sg_gain[sg] = read_int(r, 6, "sg_gain");
                    if (r.overflow()) {
                        break;
                    }
                }
            } else if (state.sg_gain.size() != groups) {
                // Nothing kept to repeat for this many groups: 0 dB, as until
                // the first transmission.
                state.sg_gain.assign(groups, 0);
            }
            if (const ParseResult result = check(r); !result) {
                return result;
            }
            out.sg_gain = state.sg_gain;
        }
    }

    out.b_associated = r.read_flag("b_associated");
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
        out.b_associate_is_mono = r.read_flag("b_associate_is_mono");
        if (out.b_associate_is_mono) {
            out.pan_associated = read_int(r, 8, "pan_associated");
        }
    }

    if (const ParseResult result = parse_custom_dmx_data(r, ctx, out.custom_dmx_data); !result) {
        return result;
    }
    if (const ParseResult result = parse_loud_corr(r, ctx, out.loud_corr); !result) {
        return result;
    }
    r.align();
    return check(r);
}

}  // namespace ac4::detail
