#include "aspx/aspx_syntax.hpp"

#include <bit>
#include <span>

#include "tables/huffman_codes.hpp"
#include "tables/huffman_tables.hpp"

namespace ac4::detail {
namespace {

// get_aspx_hcb(), Pseudocode 79: the codebook's codewords, for writing, and
// its cb_off, from Annex A.
struct CodebookRef {
    std::span<const HuffCode> codes;
    int cb_off = 0;
};

enum class HcbType : std::uint8_t { kF0, kDf, kDt };

[[nodiscard]] CodebookRef codebook(bool signal, int quant_mode, bool balance, HcbType type) {
    using namespace tables;
    struct Set {
        std::span<const HuffCode> f0, df, dt;
        const Codebook *f0_cb, *df_cb, *dt_cb;
    };
    const auto pick = [&]() -> Set {
        if (!signal) {
            return balance ? Set{kAspxHcbNoiseBalanceF0Codes, kAspxHcbNoiseBalanceDfCodes,
                                 kAspxHcbNoiseBalanceDtCodes, &kAspxHcbNoiseBalanceF0,
                                 &kAspxHcbNoiseBalanceDf, &kAspxHcbNoiseBalanceDt}
                           : Set{kAspxHcbNoiseLevelF0Codes, kAspxHcbNoiseLevelDfCodes,
                                 kAspxHcbNoiseLevelDtCodes, &kAspxHcbNoiseLevelF0,
                                 &kAspxHcbNoiseLevelDf, &kAspxHcbNoiseLevelDt};
        }
        if (quant_mode == 0) {
            return balance ? Set{kAspxHcbEnvBalance15F0Codes, kAspxHcbEnvBalance15DfCodes,
                                 kAspxHcbEnvBalance15DtCodes, &kAspxHcbEnvBalance15F0,
                                 &kAspxHcbEnvBalance15Df, &kAspxHcbEnvBalance15Dt}
                           : Set{kAspxHcbEnvLevel15F0Codes, kAspxHcbEnvLevel15DfCodes,
                                 kAspxHcbEnvLevel15DtCodes, &kAspxHcbEnvLevel15F0,
                                 &kAspxHcbEnvLevel15Df, &kAspxHcbEnvLevel15Dt};
        }
        return balance ? Set{kAspxHcbEnvBalance30F0Codes, kAspxHcbEnvBalance30DfCodes,
                             kAspxHcbEnvBalance30DtCodes, &kAspxHcbEnvBalance30F0,
                             &kAspxHcbEnvBalance30Df, &kAspxHcbEnvBalance30Dt}
                       : Set{kAspxHcbEnvLevel30F0Codes, kAspxHcbEnvLevel30DfCodes,
                             kAspxHcbEnvLevel30DtCodes, &kAspxHcbEnvLevel30F0,
                             &kAspxHcbEnvLevel30Df, &kAspxHcbEnvLevel30Dt};
    };
    const Set set = pick();
    switch (type) {
        case HcbType::kF0:
            return {set.f0, set.f0_cb->cb_off};
        case HcbType::kDf:
            return {set.df, set.df_cb->cb_off};
        case HcbType::kDt:
            return {set.dt, set.dt_cb->cb_off};
    }
    return {};
}

// The codebook for value i of an envelope (Table 58): F0 then DF along
// frequency, DT along time.
[[nodiscard]] CodebookRef value_codebook(bool signal, int quant_mode, bool balance, int delta_dir,
                                         std::size_t i) {
    if (delta_dir == 0) {
        return codebook(signal, quant_mode, balance, i == 0 ? HcbType::kF0 : HcbType::kDf);
    }
    return codebook(signal, quant_mode, balance, HcbType::kDt);
}

[[nodiscard]] bool fits(const CodebookRef& cb, int value) noexcept {
    const int index = value + cb.cb_off;
    return index >= 0 && static_cast<std::size_t>(index) < cb.codes.size();
}

// aspx_huff_data(), Table 58.
void write_huff_data(BitWriter& w, bool signal, int quant_mode, bool balance,
                     const AspxEnvelopeFields& envelope) {
    for (std::size_t i = 0; i < envelope.values.size(); ++i) {
        const CodebookRef cb = value_codebook(signal, quant_mode, balance, envelope.delta_dir, i);
        w.write_codeword(cb.codes, static_cast<std::size_t>(envelope.values[i] + cb.cb_off),
                         "aspx_hcw");
    }
}

// The count and relative border fields: 1 bit when num_aspx_timeslots is 8 or
// fewer, else 2 (Table 53, Note 1).
void write_rel_borders(BitWriter& w, int rel_bits, const std::vector<int>& borders,
                       const char* count_name, const char* border_name) {
    w.write(static_cast<unsigned>(rel_bits), borders.size(), count_name);
    for (const int border : borders) {
        w.write(static_cast<unsigned>(rel_bits), static_cast<std::uint64_t>((border - 2) / 2),
                border_name);
    }
}

// aspx_framing(ch), Table 53. aspx_int_class is a prefix code, 0, 10, 110
// or 111, recorded as one element with the code as its value.
void write_framing(BitWriter& w, bool b_iframe, const AspxConfigFields& config,
                   const AspxCounts& counts, const AspxFramingFields& f) {
    const int rel_bits = counts.num_aspx_timeslots > 8 ? 2 : 1;
    switch (f.int_class) {
        case AspxIntervalClass::kFixFix:
            w.write(1, 0, "aspx_int_class");
            w.write(static_cast<unsigned>(config.num_env_bits_fixfix + 1),
                    static_cast<std::uint64_t>(f.tmp_num_env), "tmp_num_env");
            if (config.freq_res_mode == 0) {
                w.write(1, static_cast<std::uint64_t>(f.freq_res.at(0)), "aspx_freq_res");
            }
            return;
        case AspxIntervalClass::kFixVar:
            w.write(2, 0b10, "aspx_int_class");
            w.write(2, static_cast<std::uint64_t>(f.var_bord_right), "aspx_var_bord_right");
            write_rel_borders(w, rel_bits, f.rel_bord_right, "aspx_num_rel_right",
                              "aspx_rel_bord_right");
            break;
        case AspxIntervalClass::kVarVar:
            w.write(3, 0b111, "aspx_int_class");
            if (b_iframe) {
                w.write(2, static_cast<std::uint64_t>(f.var_bord_left), "aspx_var_bord_left");
            }
            write_rel_borders(w, rel_bits, f.rel_bord_left, "aspx_num_rel_left",
                              "aspx_rel_bord_left");
            w.write(2, static_cast<std::uint64_t>(f.var_bord_right), "aspx_var_bord_right");
            write_rel_borders(w, rel_bits, f.rel_bord_right, "aspx_num_rel_right",
                              "aspx_rel_bord_right");
            break;
        case AspxIntervalClass::kVarFix:
            w.write(3, 0b110, "aspx_int_class");
            if (b_iframe) {
                w.write(2, static_cast<std::uint64_t>(f.var_bord_left), "aspx_var_bord_left");
            }
            write_rel_borders(w, rel_bits, f.rel_bord_left, "aspx_num_rel_left",
                              "aspx_rel_bord_left");
            break;
    }
    // ptr_bits = ceil(log(aspx_num_env + 2) / log(2)), the bit width of
    // aspx_num_env + 1.
    const int num_env = f.num_env();
    const auto ptr_bits = static_cast<unsigned>(std::bit_width(static_cast<unsigned>(num_env + 1)));
    w.write(ptr_bits, static_cast<std::uint64_t>(f.tsg_ptr + 1), "aspx_tsg_ptr");
    if (config.freq_res_mode == 0) {
        for (int env = 0; env < num_env; ++env) {
            w.write(1, static_cast<std::uint64_t>(f.freq_res.at(static_cast<std::size_t>(env))),
                    "aspx_freq_res");
        }
    }
}

// aspx_delta_dir(ch), Table 54.
void write_delta_dir(BitWriter& w, const AspxChannelFields& c) {
    for (const AspxEnvelopeFields& e : c.sig) {
        w.write(1, static_cast<std::uint64_t>(e.delta_dir), "aspx_sig_delta_dir");
    }
    for (const AspxEnvelopeFields& e : c.noise) {
        w.write(1, static_cast<std::uint64_t>(e.delta_dir), "aspx_noise_delta_dir");
    }
}

void write_flags(BitWriter& w, const std::vector<bool>& flags, const char* name) {
    for (const bool flag : flags) {
        w.write(1, flag ? 1 : 0, name);
    }
}

// aspx_ec_data() for one channel's signal or noise envelopes (Table 57).
void write_ec_data(BitWriter& w, bool signal, const AspxChannelFields& c, bool balance) {
    const std::vector<AspxEnvelopeFields>& envelopes = signal ? c.sig : c.noise;
    for (const AspxEnvelopeFields& e : envelopes) {
        write_huff_data(w, signal, signal ? c.qmode_env : 0, balance, e);
    }
}

}  // namespace

int AspxFramingFields::num_env() const noexcept {
    if (int_class == AspxIntervalClass::kFixFix) {
        return 1 << tmp_num_env;
    }
    return static_cast<int>(rel_bord_left.size() + rel_bord_right.size()) + 1;
}

void write_companding_control(BitWriter& w, const CompandingFields& fields) {
    if (fields.num_chan > 1) {
        w.write(1, fields.sync_flag ? 1 : 0, "sync_flag");
    }
    const int nc = fields.sync_flag ? 1 : fields.num_chan;
    bool need_avg = false;
    for (int ch = 0; ch < nc; ++ch) {
        const bool on = fields.compand_on[static_cast<std::size_t>(ch)];
        w.write(1, on ? 1 : 0, "b_compand_on");
        need_avg = need_avg || !on;
    }
    if (need_avg) {
        w.write(1, fields.compand_avg ? 1 : 0, "b_compand_avg");
    }
}

void write_aspx_config(BitWriter& w, const AspxConfigFields& c) {
    w.write(1, static_cast<std::uint64_t>(c.quant_mode_env), "aspx_quant_mode_env");
    w.write(3, static_cast<std::uint64_t>(c.start_freq), "aspx_start_freq");
    w.write(2, static_cast<std::uint64_t>(c.stop_freq), "aspx_stop_freq");
    w.write(1, static_cast<std::uint64_t>(c.master_freq_scale), "aspx_master_freq_scale");
    w.write(1, c.interpolation ? 1 : 0, "aspx_interpolation");
    w.write(1, c.preflat ? 1 : 0, "aspx_preflat");
    w.write(1, c.limiter ? 1 : 0, "aspx_limiter");
    w.write(2, static_cast<std::uint64_t>(c.noise_sbg), "aspx_noise_sbg");
    w.write(1, static_cast<std::uint64_t>(c.num_env_bits_fixfix), "aspx_num_env_bits_fixfix");
    w.write(2, static_cast<std::uint64_t>(c.freq_res_mode), "aspx_freq_res_mode");
}

void write_aspx_data_1ch(BitWriter& w, bool b_iframe, int xover_subband_offset,
                         const AspxConfigFields& config, const AspxCounts& counts,
                         const AspxChannelFields& c) {
    if (b_iframe) {
        w.write(3, static_cast<std::uint64_t>(xover_subband_offset), "aspx_xover_subband_offset");
    }
    write_framing(w, b_iframe, config, counts, c.framing);
    write_delta_dir(w, c);
    // aspx_hfgen_iwc_1ch(), Table 55.
    for (const int mode : c.tna_mode) {
        w.write(2, static_cast<std::uint64_t>(mode), "aspx_tna_mode");
    }
    w.write(1, c.add_harmonic.empty() ? 0 : 1, "aspx_ah_present");
    write_flags(w, c.add_harmonic, "aspx_add_harmonic");
    w.write(1, c.fic_used_in_sfb.empty() ? 0 : 1, "aspx_fic_present");
    write_flags(w, c.fic_used_in_sfb, "aspx_fic_used_in_sfb");
    w.write(1, c.tic_used_in_slot.empty() ? 0 : 1, "aspx_tic_present");
    write_flags(w, c.tic_used_in_slot, "aspx_tic_used_in_slot");
    write_ec_data(w, true, c, false);
    write_ec_data(w, false, c, false);
}

void write_aspx_data_2ch(BitWriter& w, bool b_iframe, int xover_subband_offset,
                         const AspxConfigFields& config, const AspxCounts& counts, bool balance,
                         const std::array<AspxChannelFields, 2>& channels) {
    const AspxChannelFields& left = channels[0];
    const AspxChannelFields& right = channels[1];
    if (b_iframe) {
        w.write(3, static_cast<std::uint64_t>(xover_subband_offset), "aspx_xover_subband_offset");
    }
    write_framing(w, b_iframe, config, counts, left.framing);
    w.write(1, balance ? 1 : 0, "aspx_balance");
    if (!balance) {
        write_framing(w, b_iframe, config, counts, right.framing);
    }
    write_delta_dir(w, left);
    write_delta_dir(w, right);
    // aspx_hfgen_iwc_2ch(aspx_balance), Table 56.
    for (const int mode : left.tna_mode) {
        w.write(2, static_cast<std::uint64_t>(mode), "aspx_tna_mode");
    }
    if (!balance) {
        for (const int mode : right.tna_mode) {
            w.write(2, static_cast<std::uint64_t>(mode), "aspx_tna_mode");
        }
    }
    w.write(1, left.add_harmonic.empty() ? 0 : 1, "aspx_ah_left");
    write_flags(w, left.add_harmonic, "aspx_add_harmonic");
    w.write(1, right.add_harmonic.empty() ? 0 : 1, "aspx_ah_right");
    write_flags(w, right.add_harmonic, "aspx_add_harmonic");
    const bool fic = !left.fic_used_in_sfb.empty() || !right.fic_used_in_sfb.empty();
    w.write(1, fic ? 1 : 0, "aspx_fic_present");
    if (fic) {
        w.write(1, left.fic_used_in_sfb.empty() ? 0 : 1, "aspx_fic_left");
        write_flags(w, left.fic_used_in_sfb, "aspx_fic_used_in_sfb");
        w.write(1, right.fic_used_in_sfb.empty() ? 0 : 1, "aspx_fic_right");
        write_flags(w, right.fic_used_in_sfb, "aspx_fic_used_in_sfb");
    }
    const bool tic = !left.tic_used_in_slot.empty() || !right.tic_used_in_slot.empty();
    w.write(1, tic ? 1 : 0, "aspx_tic_present");
    if (tic) {
        // aspx_tic_copy when both channels interleave the same slots.
        const bool copy = left.tic_used_in_slot == right.tic_used_in_slot;
        w.write(1, copy ? 1 : 0, "aspx_tic_copy");
        if (!copy) {
            w.write(1, left.tic_used_in_slot.empty() ? 0 : 1, "aspx_tic_left");
            w.write(1, right.tic_used_in_slot.empty() ? 0 : 1, "aspx_tic_right");
        }
        write_flags(w, left.tic_used_in_slot, "aspx_tic_used_in_slot");
        if (!copy) {
            write_flags(w, right.tic_used_in_slot, "aspx_tic_used_in_slot");
        }
    }
    // Table 52's order: both signal envelopes, then both noise envelopes.
    write_ec_data(w, true, left, false);
    write_ec_data(w, true, right, balance);
    write_ec_data(w, false, left, false);
    write_ec_data(w, false, right, balance);
}

std::size_t aspx_envelope_bits(bool signal, int quant_mode, bool balance,
                               const AspxEnvelopeFields& envelope) {
    std::size_t bits = 0;
    for (std::size_t i = 0; i < envelope.values.size(); ++i) {
        const CodebookRef cb = value_codebook(signal, quant_mode, balance, envelope.delta_dir, i);
        bits += cb.codes[static_cast<std::size_t>(envelope.values[i] + cb.cb_off)].bits;
    }
    return bits;
}

bool aspx_envelope_codable(bool signal, int quant_mode, bool balance,
                           const AspxEnvelopeFields& envelope) {
    for (std::size_t i = 0; i < envelope.values.size(); ++i) {
        if (!fits(value_codebook(signal, quant_mode, balance, envelope.delta_dir, i),
                  envelope.values[i])) {
            return false;
        }
    }
    return true;
}

}  // namespace ac4::detail
