#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "bit_writer.hpp"

// The companding and A-SPX syntax, written: ETSI TS 103 190-1 V1.4.1 Table 49
// (companding_control) and Tables 50 to 58 (aspx_config, aspx_data_1ch,
// aspx_data_2ch and the elements they call), transcribed for writing. The
// decoder's reader (src/ac4dec/src/syntax/aspx.cpp) and the Python parser
// are transcriptions of their own; the three traces agree record for record.
//
// The values here are the syntax's: what the encoder decided is turned into
// them by the encoder (src/ac4enc/src/aspx/aspx_encoder.hpp), which also
// derives the counts the syntax reads with.

namespace ac4::detail {

// aspx_config(), Table 50.
struct AspxConfigFields {
    int quant_mode_env = 1;
    int start_freq = 0;
    int stop_freq = 0;
    int master_freq_scale = 0;
    bool interpolation = true;
    bool preflat = true;
    bool limiter = true;
    int noise_sbg = 3;
    int num_env_bits_fixfix = 0;
    int freq_res_mode = 2;
};

// Table 126's aspx_int_class, with the code each is written as.
enum class AspxIntervalClass : std::uint8_t { kFixFix, kFixVar, kVarFix, kVarVar };

// aspx_framing(ch), Table 53, as its fields are sent.
struct AspxFramingFields {
    AspxIntervalClass int_class = AspxIntervalClass::kFixFix;
    int tmp_num_env = 0;       // FIXFIX: aspx_num_env is 1 << tmp_num_env
    int var_bord_left = 0;     // VARFIX and VARVAR, sent in I-frames only
    int var_bord_right = 0;    // FIXVAR and VARVAR
    std::vector<int> rel_bord_left;   // each 2 * tmp + 2: 2, 4, 6 or 8 (2 or 4 at 8 slots or fewer)
    std::vector<int> rel_bord_right;
    int tsg_ptr = -1;          // sent as tmp = aspx_tsg_ptr + 1 unless FIXFIX
    std::vector<int> freq_res;  // sent when aspx_freq_res_mode is 0: FIXFIX one, else one per envelope

    [[nodiscard]] int num_env() const noexcept;
    [[nodiscard]] int num_noise() const noexcept { return num_env() > 1 ? 2 : 1; }
};

// One envelope's aspx_huff_data(): its direction and the values
// huff_decode() and huff_decode_diff() return for it, which are the codeword
// index for the first value along frequency and the index less cb_off
// otherwise (4.3.10.8.3).
struct AspxEnvelopeFields {
    int delta_dir = 0;  // 0 along frequency, 1 along time
    std::vector<int> values;
};

// Everything of one channel an aspx_data element sends.
struct AspxChannelFields {
    AspxFramingFields framing;
    std::vector<int> tna_mode;            // num_sbg_noise values, 0 to 3
    std::vector<bool> add_harmonic;       // num_sbg_sig_highres, or empty when none is sent
    std::vector<bool> fic_used_in_sfb;    // num_sbg_sig_highres, or empty when none is sent
    std::vector<bool> tic_used_in_slot;   // num_aspx_timeslots, or empty when none is sent
    std::vector<AspxEnvelopeFields> sig;  // num_env
    std::vector<AspxEnvelopeFields> noise;  // num_noise
    // aspx_qmode_env for this channel, and the frequency resolution of each
    // envelope, which choose the codebooks and the value counts.
    int qmode_env = 0;
    std::vector<int> envelope_freq_res;
};

// The counts the syntax reads with (clause 5.7.6.3), for one element.
struct AspxCounts {
    int num_sbg_sig_highres = 0;
    int num_sbg_sig_lowres = 0;
    int num_sbg_noise = 0;
    int num_aspx_timeslots = 0;
};

// companding_control(num_chan), Table 49.
struct CompandingFields {
    int num_chan = 1;
    bool sync_flag = false;
    std::array<bool, 5> compand_on{};
    bool compand_avg = false;
};

void write_companding_control(BitWriter& w, const CompandingFields& fields);
void write_aspx_config(BitWriter& w, const AspxConfigFields& config);

// aspx_data_1ch(b_iframe), Table 51, with aspx_hfgen_iwc_1ch() (Table 55).
void write_aspx_data_1ch(BitWriter& w, bool b_iframe, int xover_subband_offset,
                         const AspxConfigFields& config, const AspxCounts& counts,
                         const AspxChannelFields& channel);

// aspx_data_2ch(b_iframe), Table 52, with aspx_hfgen_iwc_2ch() (Table 56).
// With `balance` the second channel's framing is the first's and is not sent,
// its tna_mode is the first's, and its data are balance values.
void write_aspx_data_2ch(BitWriter& w, bool b_iframe, int xover_subband_offset,
                         const AspxConfigFields& config, const AspxCounts& counts, bool balance,
                         const std::array<AspxChannelFields, 2>& channels);

// The bits an envelope's values cost in the codebook its direction and
// position select: what choosing a direction compares.
[[nodiscard]] std::size_t aspx_envelope_bits(bool signal, int quant_mode, bool balance,
                                             const AspxEnvelopeFields& envelope);

// Whether every value of an envelope has a codeword: F0's range, and the
// differential codebooks' +-cb_off.
[[nodiscard]] bool aspx_envelope_codable(bool signal, int quant_mode, bool balance,
                                         const AspxEnvelopeFields& envelope);

}  // namespace ac4::detail
