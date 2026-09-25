#include "syntax/aspx.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>

#include "aspx/frequency_tables.hpp"
#include "tables/huffman_tables.hpp"

namespace ac4::detail {

namespace {

// Table 194 (tab_border): FIXFIX time slot group borders for two and four
// groups; one group is always {0, num_aspx_timeslots}.
struct TabBorderRow {
    int num_aspx_timeslots = 0;
    std::array<std::int8_t, 3> two{};
    std::array<std::int8_t, 5> four{};
};

constexpr std::array<TabBorderRow, 5> kTabBorder = {{
    {6, {{0, 3, 6}}, {{0, 2, 3, 4, 6}}},
    {8, {{0, 4, 8}}, {{0, 2, 4, 6, 8}}},
    {12, {{0, 6, 12}}, {{0, 3, 6, 9, 12}}},
    {15, {{0, 8, 15}}, {{0, 4, 8, 12, 15}}},
    {16, {{0, 8, 16}}, {{0, 4, 8, 12, 16}}},
}};

// Pseudocode 79's codebooks, each as its F0, DF and DT tables.
using CodebookSet = std::array<const Codebook*, 3>;

constexpr CodebookSet kEnvLevel15 = {&tables::kAspxHcbEnvLevel15F0,
                                     &tables::kAspxHcbEnvLevel15Df,
                                     &tables::kAspxHcbEnvLevel15Dt};
constexpr CodebookSet kEnvBalance15 = {&tables::kAspxHcbEnvBalance15F0,
                                       &tables::kAspxHcbEnvBalance15Df,
                                       &tables::kAspxHcbEnvBalance15Dt};
constexpr CodebookSet kEnvLevel30 = {&tables::kAspxHcbEnvLevel30F0,
                                     &tables::kAspxHcbEnvLevel30Df,
                                     &tables::kAspxHcbEnvLevel30Dt};
constexpr CodebookSet kEnvBalance30 = {&tables::kAspxHcbEnvBalance30F0,
                                       &tables::kAspxHcbEnvBalance30Df,
                                       &tables::kAspxHcbEnvBalance30Dt};
constexpr CodebookSet kNoiseLevel = {&tables::kAspxHcbNoiseLevelF0,
                                     &tables::kAspxHcbNoiseLevelDf,
                                     &tables::kAspxHcbNoiseLevelDt};
constexpr CodebookSet kNoiseBalance = {&tables::kAspxHcbNoiseBalanceF0,
                                       &tables::kAspxHcbNoiseBalanceDf,
                                       &tables::kAspxHcbNoiseBalanceDt};

[[nodiscard]] std::uint8_t u8(std::uint32_t value) noexcept {
    return static_cast<std::uint8_t>(value);
}

// One aspx_hcw, a miss reported as every tool reports one (huff_codeword()).
[[nodiscard]] ParseResult read_hcw(BitReader& r, const Codebook& codebook, std::uint16_t& out) {
    const auto index = huff_codeword(r, codebook, "aspx_hcw",
                                     {.truncated = "an aspx_hcw runs past the end of the substream",
                                      .invalid = "no A-SPX Huffman codeword matches"});
    if (!index) {
        return std::unexpected(index.error());
    }
    out = static_cast<std::uint16_t>(*index);
    return {};
}

// aspx_int_class (Table 126) is the prefix code 0, 10, 110, 111, which Table
// 53 lists as one element of 1...3 bits: peek to find the code's length, then
// read the whole code as that element.
[[nodiscard]] AspxIntClass read_int_class(BitReader& r) {
    if (r.peek_raw(1) == 0) {
        r.read(1, "aspx_int_class");
        return AspxIntClass::kFixFix;
    }
    if (r.peek_raw(2) == 0b10U) {
        r.read(2, "aspx_int_class");
        return AspxIntClass::kFixVar;
    }
    return r.read(3, "aspx_int_class") == 0b110U ? AspxIntClass::kVarFix : AspxIntClass::kVarVar;
}

// aspx_rel_bord_left[ch][rel] or aspx_rel_bord_right[ch][rel] = 2*tmp + 2.
void read_rel_borders(BitReader& r, int bits, std::size_t count,
                      std::array<std::uint8_t, kAspxMaxRelBorders>& borders,
                      std::string_view name) {
    for (std::size_t rel = 0; rel < count; ++rel) {
        borders[rel] = u8(2U * r.read(bits, name) + 2U);
    }
}

// tab_border[num_aspx_timeslots][num_groups] into borders[0..num_groups].
[[nodiscard]] bool tab_border(int num_aspx_timeslots, std::size_t num_groups,
                              std::span<std::int8_t> borders) noexcept {
    for (const TabBorderRow& row : kTabBorder) {
        if (row.num_aspx_timeslots != num_aspx_timeslots) {
            continue;
        }
        switch (num_groups) {
            case 1:
                borders[0] = 0;
                borders[1] = static_cast<std::int8_t>(num_aspx_timeslots);
                return true;
            case 2:
                std::ranges::copy(row.two, borders.begin());
                return true;
            case 4:
                std::ranges::copy(row.four, borders.begin());
                return true;
            default:
                return false;
        }
    }
    return false;
}

// Table 193: which signal border is the middle noise border.
[[nodiscard]] std::size_t noise_mid_border(AspxIntClass int_class, int tsg_ptr,
                                           int num_atsg_sig) noexcept {
    int border = 0;
    if (int_class == AspxIntClass::kVarFix) {
        border = tsg_ptr < 0 ? 1 : num_atsg_sig - 1;
    } else {  // FIXVAR, VARVAR
        border = tsg_ptr < 0 ? num_atsg_sig - 1 : std::max(1, std::min(num_atsg_sig - 1, tsg_ptr));
    }
    return static_cast<std::size_t>(border);
}

// freq_res(), Pseudocode 77, for envelope `atsg` once atsg_sig is set: true
// for FREQ_RES_HIGH.
[[nodiscard]] bool freq_res_high(const AspxFraming& f, std::size_t atsg, int tsg_ptr,
                                 int num_aspx_timeslots, int freq_res_mode) noexcept {
    switch (freq_res_mode) {
        case 0:
            return f.freq_res[atsg] != 0;
        case 1:
            return false;
        case 2: {
            // (atsg_sig[atsg+1] - atsg_sig[atsg]) > num_aspx_timeslots/6.0 + 3.25,
            // multiplied through by 12 so that it is decided in integers.
            const int length = f.atsg_sig[atsg + 1] - f.atsg_sig[atsg];
            const bool before_ptr = static_cast<int>(atsg) < tsg_ptr && num_aspx_timeslots > 8;
            return before_ptr || 12 * length > 2 * num_aspx_timeslots + 39;
        }
        default:
            return true;
    }
}

// Pseudocode 76: the signal and noise borders, each envelope's frequency
// resolution, and previous_stop_pos for the next interval.
[[nodiscard]] ParseResult derive_framing(AspxFraming& f, const AspxConfig& config, bool b_iframe,
                                         int num_aspx_timeslots,
                                         std::int8_t& previous_stop_offset) {
    const std::size_t num_env = f.num_env;
    const int mode = config.freq_res_mode;
    if (f.int_class == AspxIntClass::kFixFix) {
        if (!tab_border(num_aspx_timeslots, num_env, f.atsg_sig) ||
            !tab_border(num_aspx_timeslots, f.num_noise, f.atsg_noise)) {
            return fail(DecodeError::kInvalidStream,
                        "aspx_framing: no FIXFIX borders (Table 194)");
        }
        const auto first =
            static_cast<std::uint8_t>(freq_res_high(f, 0, 0, num_aspx_timeslots, mode));
        for (std::size_t atsg = 0; atsg < num_env; ++atsg) {
            f.atsg_freqres[atsg] = first;
        }
    } else {
        // previous_stop_pos - num_aspx_timeslots, for a VAR start in a non-I-frame.
        const int var_start = b_iframe ? f.var_bord_left : previous_stop_offset;
        std::array<int, kAspxMaxSignalEnvelopes + 1> sig{};
        const std::size_t num_left = f.num_rel_left;
        const std::size_t num_right = f.num_rel_right;
        switch (f.int_class) {
            case AspxIntClass::kFixVar:
                sig[0] = 0;
                sig[num_env] = f.var_bord_right + num_aspx_timeslots;
                break;
            case AspxIntClass::kVarFix:
                sig[0] = var_start;
                sig[num_env] = num_aspx_timeslots;
                break;
            case AspxIntClass::kVarVar:
                sig[0] = var_start;
                sig[num_env] = f.var_bord_right + num_aspx_timeslots;
                break;
            case AspxIntClass::kFixFix:
                break;
        }
        // FIXVAR sends no left borders and VARFIX no right ones, so one pair
        // of loops serves all three classes.
        for (std::size_t tsg = 0; tsg < num_left; ++tsg) {
            sig[tsg + 1] = sig[tsg] + f.rel_bord_left[tsg];
        }
        for (std::size_t tsg = 0; tsg < num_right; ++tsg) {
            sig[num_env - tsg - 1] = sig[num_env - tsg] - f.rel_bord_right[tsg];
        }
        for (std::size_t atsg = 0; atsg <= num_env; ++atsg) {
            f.atsg_sig[atsg] = static_cast<std::int8_t>(sig[atsg]);
        }
        const std::size_t num_noise = f.num_noise;
        f.atsg_noise[0] = f.atsg_sig[0];
        f.atsg_noise[num_noise] = f.atsg_sig[num_env];
        if (num_noise > 1) {
            f.atsg_noise[1] =
                f.atsg_sig[noise_mid_border(f.int_class, f.tsg_ptr, static_cast<int>(num_env))];
        }
        for (std::size_t atsg = 0; atsg < num_env; ++atsg) {
            f.atsg_freqres[atsg] = static_cast<std::uint8_t>(
                freq_res_high(f, atsg, f.tsg_ptr, num_aspx_timeslots, mode));
        }
    }
    previous_stop_offset = static_cast<std::int8_t>(f.atsg_sig[num_env] - num_aspx_timeslots);
    return {};
}

// aspx_framing(ch), Table 53.
[[nodiscard]] ParseResult parse_aspx_framing(BitReader& r, const AspxConfig& config, bool b_iframe,
                                             int num_aspx_timeslots,
                                             std::int8_t& previous_stop_offset, AspxFraming& f) {
    f = AspxFraming{};
    f.int_class = read_int_class(r);
    // Table 53 Note 1: counts and relative borders take 1 bit when
    // num_aspx_timeslots <= 8, otherwise 2 (4.3.10.4.6 to 4.3.10.4.9).
    const int rel_bits = num_aspx_timeslots > 8 ? 2 : 1;
    switch (f.int_class) {
        case AspxIntClass::kFixFix: {
            const int envbits = config.num_env_bits_fixfix + 1;
            f.tmp_num_env = u8(r.read(envbits, "tmp_num_env"));
            // 4.3.10.1.9 and Table 128: FIXFIX has at most four envelopes.
            if (f.tmp_num_env > 2) {
                return fail(DecodeError::kInvalidStream,
                            "aspx_framing: eight FIXFIX signal envelopes (Table 128)");
            }
            f.num_env = u8(1U << f.tmp_num_env);
            if (config.freq_res_mode == 0) {
                f.freq_res[0] = u8(r.read(1, "aspx_freq_res"));
            }
            break;
        }
        case AspxIntClass::kFixVar:
            f.var_bord_right = u8(r.read(2, "aspx_var_bord_right"));
            f.num_rel_right = u8(r.read(rel_bits, "aspx_num_rel_right"));
            read_rel_borders(r, rel_bits, f.num_rel_right, f.rel_bord_right,
                             "aspx_rel_bord_right");
            break;
        case AspxIntClass::kVarVar:
            if (b_iframe) {
                f.var_bord_left = u8(r.read(2, "aspx_var_bord_left"));
            }
            f.num_rel_left = u8(r.read(rel_bits, "aspx_num_rel_left"));
            read_rel_borders(r, rel_bits, f.num_rel_left, f.rel_bord_left, "aspx_rel_bord_left");
            f.var_bord_right = u8(r.read(2, "aspx_var_bord_right"));
            // Table 53 marks this one "Note 2"; its width column, "2 (1)", and
            // 4.3.10.4.7 give it Note 1's rule like the other counts.
            f.num_rel_right = u8(r.read(rel_bits, "aspx_num_rel_right"));
            read_rel_borders(r, rel_bits, f.num_rel_right, f.rel_bord_right,
                             "aspx_rel_bord_right");
            break;
        case AspxIntClass::kVarFix:
            if (b_iframe) {
                f.var_bord_left = u8(r.read(2, "aspx_var_bord_left"));
            }
            f.num_rel_left = u8(r.read(rel_bits, "aspx_num_rel_left"));
            read_rel_borders(r, rel_bits, f.num_rel_left, f.rel_bord_left, "aspx_rel_bord_left");
            break;
    }
    if (f.int_class != AspxIntClass::kFixFix) {
        const int num_env = f.num_rel_left + f.num_rel_right + 1;
        if (num_env > kAspxMaxSignalEnvelopes) {
            return fail(DecodeError::kInvalidStream,
                        "aspx_framing: more than five signal envelopes (Table 128)");
        }
        f.num_env = u8(static_cast<unsigned>(num_env));
        // ptr_bits = ceil(log2(aspx_num_env + 2)), with Note 2's exact
        // division: the bit width of aspx_num_env + 1.
        const int ptr_bits = static_cast<int>(std::bit_width(static_cast<unsigned>(num_env + 1)));
        const auto tmp = static_cast<int>(r.read(ptr_bits, "aspx_tsg_ptr"));
        f.tsg_ptr = static_cast<std::int8_t>(tmp - 1);
        if (config.freq_res_mode == 0) {
            const auto count = static_cast<std::size_t>(num_env);
            for (std::size_t env = 0; env < count; ++env) {
                f.freq_res[env] = u8(r.read(1, "aspx_freq_res"));
            }
        }
    }
    f.num_noise = f.num_env > 1 ? 2 : 1;
    if (auto result = check(r); !result) {
        return result;
    }
    return derive_framing(f, config, b_iframe, num_aspx_timeslots, previous_stop_offset);
}

// aspx_delta_dir(ch), Table 54.
void parse_aspx_delta_dir(BitReader& r, AspxChannel& c) {
    const std::size_t num_env = c.framing.num_env;
    const std::size_t num_noise = c.framing.num_noise;
    for (std::size_t env = 0; env < num_env; ++env) {
        c.sig[env].delta_dir = u8(r.read(1, "aspx_sig_delta_dir"));
    }
    for (std::size_t env = 0; env < num_noise; ++env) {
        c.noise[env].delta_dir = u8(r.read(1, "aspx_noise_delta_dir"));
    }
}

// aspx_ec_data() and aspx_huff_data(), Tables 57 and 58, for one channel's
// signal or noise envelopes. The syntax hands aspx_ec_data() the aspx_freq_res
// array, but that array is incomplete for FIXFIX and absent unless
// aspx_freq_res_mode is 0; the envelope resolution Pseudocodes 76 and 77
// derive is what it stands for.
[[nodiscard]] ParseResult parse_aspx_ec_data(BitReader& r, AspxDataType data_type,
                                             const AspxSubbandGroups& groups, AspxChannel& c) {
    const bool signal = data_type == AspxDataType::kSignal;
    const std::size_t num_env = signal ? c.framing.num_env : c.framing.num_noise;
    // Tables 51 and 52 pass quant_mode 0 for noise, which Pseudocode 79 does
    // not use for the noise codebooks.
    const int quant_mode = signal ? c.qmode_env : 0;
    for (std::size_t env = 0; env < num_env; ++env) {
        AspxEnvelope& e = signal ? c.sig[env] : c.noise[env];
        std::size_t num_sbg = groups.num_sbg_noise;
        if (signal) {
            num_sbg = c.framing.atsg_freqres[env] != 0 ? groups.num_sbg_sig_highres
                                                       : groups.num_sbg_sig_lowres;
        }
        e.num_sbg = u8(static_cast<std::uint32_t>(num_sbg));
        std::size_t first = 0;
        if (e.delta_dir == 0) {
            const Codebook& f0 =
                aspx_codebook(data_type, quant_mode, c.stereo_mode, AspxHcbType::kF0);
            if (auto result = read_hcw(r, f0, e.huff_index[0]); !result) {
                return result;
            }
            first = 1;
        }
        const AspxHcbType rest_type = e.delta_dir == 0 ? AspxHcbType::kDf : AspxHcbType::kDt;
        const Codebook& rest = aspx_codebook(data_type, quant_mode, c.stereo_mode, rest_type);
        for (std::size_t i = first; i < num_sbg; ++i) {
            if (auto result = read_hcw(r, rest, e.huff_index[i]); !result) {
                return result;
            }
        }
        if (auto result = check(r); !result) {
            return result;
        }
    }
    return {};
}

// aspx_hfgen_iwc_1ch(), Table 55.
[[nodiscard]] ParseResult parse_aspx_hfgen_iwc_1ch(BitReader& r, const AspxSubbandGroups& groups,
                                                   std::size_t num_aspx_timeslots,
                                                   AspxData1ch& out) {
    AspxChannel& c = out.channel;
    const std::size_t num_noise = groups.num_sbg_noise;
    const std::size_t num_highres = groups.num_sbg_sig_highres;
    for (std::size_t n = 0; n < num_noise; ++n) {
        c.tna_mode[n] = u8(r.read(2, "aspx_tna_mode"));
    }
    out.ah_present = r.read_flag("aspx_ah_present");
    if (out.ah_present) {
        for (std::size_t n = 0; n < num_highres; ++n) {
            c.add_harmonic[n] = r.read_flag("aspx_add_harmonic");
        }
    }
    out.fic_present = r.read_flag("aspx_fic_present");
    if (out.fic_present) {
        for (std::size_t n = 0; n < num_highres; ++n) {
            c.fic_used_in_sfb[n] = r.read_flag("aspx_fic_used_in_sfb");
        }
    }
    out.tic_present = r.read_flag("aspx_tic_present");
    if (out.tic_present) {
        for (std::size_t n = 0; n < num_aspx_timeslots; ++n) {
            c.tic_used_in_slot[n] = r.read_flag("aspx_tic_used_in_slot");
        }
    }
    return check(r);
}

// aspx_hfgen_iwc_2ch(aspx_balance), Table 56.
[[nodiscard]] ParseResult parse_aspx_hfgen_iwc_2ch(BitReader& r, const AspxSubbandGroups& groups,
                                                   std::size_t num_aspx_timeslots,
                                                   AspxData2ch& out) {
    AspxChannel& left = out.channels[0];
    AspxChannel& right = out.channels[1];
    const std::size_t num_noise = groups.num_sbg_noise;
    const std::size_t num_highres = groups.num_sbg_sig_highres;
    for (std::size_t n = 0; n < num_noise; ++n) {
        left.tna_mode[n] = u8(r.read(2, "aspx_tna_mode"));
    }
    if (!out.balance) {
        for (std::size_t n = 0; n < num_noise; ++n) {
            right.tna_mode[n] = u8(r.read(2, "aspx_tna_mode"));
        }
    } else {
        right.tna_mode = left.tna_mode;
    }
    out.ah_left = r.read_flag("aspx_ah_left");
    if (out.ah_left) {
        for (std::size_t n = 0; n < num_highres; ++n) {
            left.add_harmonic[n] = r.read_flag("aspx_add_harmonic");
        }
    }
    out.ah_right = r.read_flag("aspx_ah_right");
    if (out.ah_right) {
        for (std::size_t n = 0; n < num_highres; ++n) {
            right.add_harmonic[n] = r.read_flag("aspx_add_harmonic");
        }
    }
    out.fic_present = r.read_flag("aspx_fic_present");
    if (out.fic_present) {
        out.fic_left = r.read_flag("aspx_fic_left");
        if (out.fic_left) {
            for (std::size_t n = 0; n < num_highres; ++n) {
                left.fic_used_in_sfb[n] = r.read_flag("aspx_fic_used_in_sfb");
            }
        }
        // Table 56 opens a brace on this for loop that it never closes. Its
        // indentation closes the aspx_fic_right and aspx_fic_present blocks
        // here, which leaves the time interleaved flags below outside
        // aspx_fic_present, as they are in aspx_hfgen_iwc_1ch().
        out.fic_right = r.read_flag("aspx_fic_right");
        if (out.fic_right) {
            for (std::size_t n = 0; n < num_highres; ++n) {
                right.fic_used_in_sfb[n] = r.read_flag("aspx_fic_used_in_sfb");
            }
        }
    }
    out.tic_present = r.read_flag("aspx_tic_present");
    if (out.tic_present) {
        out.tic_copy = r.read_flag("aspx_tic_copy");
        if (!out.tic_copy) {
            out.tic_left = r.read_flag("aspx_tic_left");
            out.tic_right = r.read_flag("aspx_tic_right");
        }
        if (out.tic_copy || out.tic_left) {
            for (std::size_t n = 0; n < num_aspx_timeslots; ++n) {
                left.tic_used_in_slot[n] = r.read_flag("aspx_tic_used_in_slot");
            }
        }
        if (out.tic_right) {
            for (std::size_t n = 0; n < num_aspx_timeslots; ++n) {
                right.tic_used_in_slot[n] = r.read_flag("aspx_tic_used_in_slot");
            }
        }
        if (out.tic_copy) {
            right.tic_used_in_slot = left.tic_used_in_slot;
        }
    }
    return check(r);
}

// What both aspx_data_1ch() and aspx_data_2ch() do before aspx_framing(0):
// the configuration check, aspx_xover_subband_offset, and the counts.
[[nodiscard]] ParseResult begin_aspx_data(BitReader& r, const SubstreamContext& ctx,
                                          const AspxConfig& config, AspxElementState& state,
                                          int& num_aspx_timeslots, AspxSubbandGroups& groups) {
    if (!config.valid) {
        return fail(DecodeError::kMissingIFrame, "A-SPX data without an I-frame's aspx_config()");
    }
    if (ctx.b_iframe) {
        state.xover_subband_offset = u8(r.read(3, "aspx_xover_subband_offset"));
        state.have_xover_subband_offset = true;
        if (auto result = check(r); !result) {
            return result;
        }
    } else if (!state.have_xover_subband_offset) {
        return fail(DecodeError::kMissingIFrame,
                    "A-SPX data without an I-frame's aspx_xover_subband_offset");
    }
    num_aspx_timeslots = aspx_num_timeslots(ctx.frame_len_base);
    if (num_aspx_timeslots == 0) {
        return fail(DecodeError::kInvalidStream,
                    "A-SPX: the frame length has no QMF time slot count (Table 189)");
    }
    return derive_aspx_subband_groups(config, state.xover_subband_offset, groups);
}

}  // namespace

ParseResult parse_aspx_config(BitReader& r, AspxConfig& out) {
    AspxConfig config;
    config.quant_mode_env = u8(r.read(1, "aspx_quant_mode_env"));
    config.start_freq = u8(r.read(3, "aspx_start_freq"));
    config.stop_freq = u8(r.read(2, "aspx_stop_freq"));
    config.master_freq_scale = u8(r.read(1, "aspx_master_freq_scale"));
    config.interpolation = r.read_flag("aspx_interpolation");
    config.preflat = r.read_flag("aspx_preflat");
    config.limiter = r.read_flag("aspx_limiter");
    config.noise_sbg = u8(r.read(2, "aspx_noise_sbg"));
    config.num_env_bits_fixfix = u8(r.read(1, "aspx_num_env_bits_fixfix"));
    config.freq_res_mode = u8(r.read(2, "aspx_freq_res_mode"));
    config.valid = !r.overflow();
    out = config;
    return check(r);
}

ParseResult derive_aspx_subband_groups(const AspxConfig& config, int xover_subband_offset,
                                       AspxSubbandGroups& out) {
    aspx::SubbandGroups tables;
    const aspx::FrequencyConfig frequency{.master_freq_scale = config.master_freq_scale,
                                          .start_freq = config.start_freq,
                                          .stop_freq = config.stop_freq,
                                          .noise_sbg = config.noise_sbg,
                                          .xover_subband_offset = xover_subband_offset};
    switch (aspx::derive_subband_groups(frequency, tables)) {
        case aspx::GroupsError::kNone:
            break;
        case aspx::GroupsError::kXoverOffset:
            // Pseudocode 68 indexes sbg_master[aspx_xover_subband_offset] up
            // to sbg_master[num_sbg_master]; an offset at or past the end
            // leaves no A-SPX range to send envelopes for.
            return fail(DecodeError::kInvalidStream,
                        "aspx_xover_subband_offset is past the master subband groups (Pseudocode 68)");
        case aspx::GroupsError::kNoiseGroups:
            return fail(DecodeError::kInvalidStream, "num_sbg_noise exceeds 5 (5.7.6.3.1.3)");
    }
    AspxSubbandGroups groups;
    groups.num_sbg_master = u8(static_cast<std::uint32_t>(tables.num_sbg_master));
    groups.sba = u8(static_cast<std::uint32_t>(tables.sba));
    groups.sbz = u8(static_cast<std::uint32_t>(tables.sbz));
    groups.sbx = u8(static_cast<std::uint32_t>(tables.sbx));
    groups.num_sb_aspx = u8(static_cast<std::uint32_t>(tables.num_sb_aspx));
    groups.num_sbg_sig_highres = u8(static_cast<std::uint32_t>(tables.num_sbg_sig_highres));
    groups.num_sbg_sig_lowres = u8(static_cast<std::uint32_t>(tables.num_sbg_sig_lowres));
    groups.num_sbg_noise = u8(static_cast<std::uint32_t>(tables.num_sbg_noise));
    out = groups;
    return {};
}

int aspx_num_timeslots(int frame_len_base) noexcept {
    // Table 189: num_qmf_timeslots = frame_length / 64. Table 192:
    // num_ts_in_ats is 2 for frame lengths of 1536 and up, 1 below.
    switch (frame_len_base) {
        case 2048:
        case 1920:
        case 1536:
            return frame_len_base / 128;
        case 1024:
        case 960:
        case 768:
        case 512:
        case 384:
            return frame_len_base / 64;
        default:
            return 0;
    }
}

const Codebook& aspx_codebook(AspxDataType data_type, int quant_mode, AspxStereoMode stereo_mode,
                              AspxHcbType hcb_type) noexcept {
    const bool balance = stereo_mode == AspxStereoMode::kBalance;
    const CodebookSet* set = nullptr;
    if (data_type == AspxDataType::kNoise) {
        set = balance ? &kNoiseBalance : &kNoiseLevel;
    } else if (quant_mode == 0) {
        set = balance ? &kEnvBalance15 : &kEnvLevel15;
    } else {
        set = balance ? &kEnvBalance30 : &kEnvLevel30;
    }
    return *(*set)[static_cast<std::size_t>(hcb_type)];
}

ParseResult parse_aspx_data_1ch(BitReader& r, const SubstreamContext& ctx,
                                const AspxConfig& config, AspxElementState& state,
                                AspxData1ch& out) {
    out = AspxData1ch{};
    int num_aspx_timeslots = 0;
    if (auto result = begin_aspx_data(r, ctx, config, state, num_aspx_timeslots, out.groups);
        !result) {
        return result;
    }
    out.xover_subband_offset = state.xover_subband_offset;
    out.num_aspx_timeslots = u8(static_cast<std::uint32_t>(num_aspx_timeslots));
    AspxChannel& c = out.channel;
    if (auto result = parse_aspx_framing(r, config, ctx.b_iframe, num_aspx_timeslots,
                                         state.previous_stop_offset[0], c.framing);
        !result) {
        return result;
    }
    c.qmode_env = config.quant_mode_env;
    if (c.framing.int_class == AspxIntClass::kFixFix && c.framing.num_env == 1) {
        c.qmode_env = 0;
    }
    parse_aspx_delta_dir(r, c);
    if (auto result = parse_aspx_hfgen_iwc_1ch(r, out.groups, out.num_aspx_timeslots, out);
        !result) {
        return result;
    }
    if (auto result = parse_aspx_ec_data(r, AspxDataType::kSignal, out.groups, c); !result) {
        return result;
    }
    if (auto result = parse_aspx_ec_data(r, AspxDataType::kNoise, out.groups, c); !result) {
        return result;
    }
    return check(r);
}

ParseResult parse_aspx_data_2ch(BitReader& r, const SubstreamContext& ctx,
                                const AspxConfig& config, AspxElementState& state,
                                AspxData2ch& out) {
    out = AspxData2ch{};
    int num_aspx_timeslots = 0;
    if (auto result = begin_aspx_data(r, ctx, config, state, num_aspx_timeslots, out.groups);
        !result) {
        return result;
    }
    out.xover_subband_offset = state.xover_subband_offset;
    out.num_aspx_timeslots = u8(static_cast<std::uint32_t>(num_aspx_timeslots));
    AspxChannel& left = out.channels[0];
    AspxChannel& right = out.channels[1];
    if (auto result = parse_aspx_framing(r, config, ctx.b_iframe, num_aspx_timeslots,
                                         state.previous_stop_offset[0], left.framing);
        !result) {
        return result;
    }
    left.qmode_env = config.quant_mode_env;
    right.qmode_env = config.quant_mode_env;
    if (left.framing.int_class == AspxIntClass::kFixFix && left.framing.num_env == 1) {
        left.qmode_env = 0;
        right.qmode_env = 0;
    }
    out.balance = r.read_flag("aspx_balance");
    if (!out.balance) {
        if (auto result = parse_aspx_framing(r, config, ctx.b_iframe, num_aspx_timeslots,
                                             state.previous_stop_offset[1], right.framing);
            !result) {
            return result;
        }
        right.qmode_env = config.quant_mode_env;
        if (right.framing.int_class == AspxIntClass::kFixFix && right.framing.num_env == 1) {
            right.qmode_env = 0;
        }
    } else {
        // Table 52 reads no aspx_framing(1) here yet goes on to loop over
        // aspx_num_env[1] and aspx_num_noise[1]; 5.7.6.3.5 says the pair's
        // time envelopes are identical, so channel 1 takes channel 0's
        // framing, and with it channel 0's stop border for the next interval.
        right.framing = left.framing;
        state.previous_stop_offset[1] = state.previous_stop_offset[0];
        right.stereo_mode = AspxStereoMode::kBalance;
    }
    parse_aspx_delta_dir(r, left);
    parse_aspx_delta_dir(r, right);
    if (auto result = parse_aspx_hfgen_iwc_2ch(r, out.groups, out.num_aspx_timeslots, out);
        !result) {
        return result;
    }
    // Table 52's order: both signal envelopes, then both noise envelopes.
    for (const AspxDataType data_type : {AspxDataType::kSignal, AspxDataType::kNoise}) {
        for (AspxChannel& c : out.channels) {
            if (auto result = parse_aspx_ec_data(r, data_type, out.groups, c); !result) {
                return result;
            }
        }
    }
    return check(r);
}

}  // namespace ac4::detail
