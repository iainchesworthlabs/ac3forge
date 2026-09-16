#pragma once

#include <array>
#include <cstdint>

#include "bit_reader.hpp"
#include "huffman.hpp"
#include "syntax/context.hpp"

// Advanced spectral extension (A-SPX) syntax: ETSI TS 103 190-1 V1.4.1 clause
// 4.2.12, semantics 4.3.10. Part 2 (TS 103 190-2 V1.3.1) Table 48 uses every
// one of these elements unchanged for bitstream_version 2.
//
// How many values the syntax reads depends on quantities clause 5.7.6.3
// derives, so the parsers compute them:
//   - num_sbg_sig_highres, num_sbg_sig_lowres and num_sbg_noise from the
//     aspx_config() and the element's aspx_xover_subband_offset
//     (Pseudocodes 67 to 70);
//   - num_aspx_timeslots from the frame length (Pseudocode 75a, Tables 189
//     and 192);
//   - each signal envelope's frequency resolution, which picks the high or
//     low resolution group count for that envelope and, when
//     aspx_freq_res_mode is 2, depends on the envelope borders the framing
//     sets (Pseudocodes 76 and 77).
// Nothing is dequantised: envelope values stay Huffman codebook indices.

namespace ac4::detail {

inline constexpr int kAspxMaxSignalEnvelopes = 5;  // Table 128
inline constexpr int kAspxMaxNoiseEnvelopes = 2;   // Table 53: aspx_num_noise is 1 or 2
inline constexpr int kAspxMaxSbgSignal = 22;       // sbg_template_highres spans 22 groups
inline constexpr int kAspxMaxSbgNoise = 5;         // 5.7.6.3.1.3
inline constexpr int kAspxMaxTimeslots = 16;       // Tables 189 and 192
inline constexpr int kAspxMaxRelBorders = 3;       // aspx_num_rel_left/right: at most 2 bits

// Table 126.
enum class AspxIntClass : std::uint8_t { kFixFix, kFixVar, kVarFix, kVarVar };

// The arguments of get_aspx_hcb() (Pseudocode 79).
enum class AspxDataType : std::uint8_t { kSignal, kNoise };
enum class AspxStereoMode : std::uint8_t { kLevel, kBalance };
enum class AspxHcbType : std::uint8_t { kF0, kDf, kDt };

// aspx_config(), 4.2.12.1. Sent in I-frames; valid until the next one
// (5.7.6.3.1.0).
struct AspxConfig {
    bool valid = false;  // set by parse_aspx_config(); false: no I-frame has sent one
    std::uint8_t quant_mode_env = 0;
    std::uint8_t start_freq = 0;
    std::uint8_t stop_freq = 0;
    std::uint8_t master_freq_scale = 0;
    bool interpolation = false;
    bool preflat = false;
    bool limiter = false;
    std::uint8_t noise_sbg = 0;
    std::uint8_t num_env_bits_fixfix = 0;
    std::uint8_t freq_res_mode = 0;
};

// Clause 5.7.6.3.1's subband group counts for one aspx_data_1ch() or
// aspx_data_2ch(), with the borders they come from.
struct AspxSubbandGroups {
    std::uint8_t num_sbg_master = 0;       // Pseudocode 67
    std::uint8_t sba = 0;                  // sbg_master[0]
    std::uint8_t sbz = 0;                  // sbg_master[num_sbg_master]
    std::uint8_t sbx = 0;                  // Pseudocode 68: the crossover subband
    std::uint8_t num_sb_aspx = 0;
    std::uint8_t num_sbg_sig_highres = 0;  // Pseudocode 68
    std::uint8_t num_sbg_sig_lowres = 0;   // Pseudocode 69
    std::uint8_t num_sbg_noise = 0;        // Pseudocode 70
};

// aspx_framing(ch), 4.2.12.4, and the time slot group borders Pseudocode 76
// derives from it.
struct AspxFraming {
    AspxIntClass int_class = AspxIntClass::kFixFix;
    // As sent; a field the interval class does not send stays 0.
    std::uint8_t tmp_num_env = 0;     // FIXFIX
    std::uint8_t var_bord_left = 0;   // VARFIX and VARVAR, I-frames only
    std::uint8_t var_bord_right = 0;  // FIXVAR and VARVAR
    std::uint8_t num_rel_left = 0;
    std::uint8_t num_rel_right = 0;
    std::array<std::uint8_t, kAspxMaxRelBorders> rel_bord_left{};   // 2*tmp + 2
    std::array<std::uint8_t, kAspxMaxRelBorders> rel_bord_right{};  // 2*tmp + 2
    // tmp - 1, so -1 upwards. FIXFIX sends none and it stays -1, the value
    // tmp = 0 gives, which points at no border. The syntax never uses it for
    // FIXFIX (Pseudocode 76 hands freq_res() a literal 0 there), but
    // Pseudocodes 92 and 95 read aspx_tsg_ptr for every interval class.
    std::int8_t tsg_ptr = -1;
    // aspx_freq_res as sent when aspx_freq_res_mode is 0 (FIXFIX sends [0]).
    std::array<std::uint8_t, kAspxMaxSignalEnvelopes> freq_res{};

    // 4.3.10.4.11: aspx_num_env and aspx_num_noise, which Pseudocode 76 calls
    // num_atsg_sig and num_atsg_noise.
    std::uint8_t num_env = 1;
    std::uint8_t num_noise = 1;
    std::array<std::int8_t, kAspxMaxSignalEnvelopes + 1> atsg_sig{};
    std::array<std::int8_t, kAspxMaxNoiseEnvelopes + 1> atsg_noise{};
    // Pseudocode 77 for every envelope: 0 low, 1 high resolution. This, not
    // the aspx_freq_res array, is what aspx_ec_data() reads envelopes by.
    std::array<std::uint8_t, kAspxMaxSignalEnvelopes> atsg_freqres{};
};

// One aspx_huff_data(), 4.2.12.9: the values of one signal or noise envelope.
struct AspxEnvelope {
    // aspx_sig_delta_dir or aspx_noise_delta_dir: 0 codes along frequency
    // (huff_index[0] from the F0 codebook, the rest from DF), 1 along time
    // (all from DT).
    std::uint8_t delta_dir = 0;
    std::uint8_t num_sbg = 0;
    // Codebook indices before cb_off (4.3.10.8.3), num_sbg of them.
    std::array<std::uint16_t, kAspxMaxSbgSignal> huff_index{};
};

// Everything aspx_data_1ch(), or one channel of aspx_data_2ch(), carries.
struct AspxChannel {
    AspxFraming framing;
    // aspx_qmode_env[ch] (Tables 51 and 52): aspx_quant_mode_env, or 0 for a
    // FIXFIX interval of one envelope. Picks the _15 or _30 signal codebooks.
    std::uint8_t qmode_env = 0;
    // The stereo_mode aspx_ec_data() gets: kBalance only for the second
    // channel of a pair sent with aspx_balance set.
    AspxStereoMode stereo_mode = AspxStereoMode::kLevel;
    // aspx_hfgen_iwc_1ch() or aspx_hfgen_iwc_2ch() for this channel; zero
    // where not sent, as the syntax initialises them.
    std::array<std::uint8_t, kAspxMaxSbgNoise> tna_mode{};
    std::array<bool, kAspxMaxSbgSignal> add_harmonic{};
    std::array<bool, kAspxMaxSbgSignal> fic_used_in_sfb{};
    std::array<bool, kAspxMaxTimeslots> tic_used_in_slot{};
    // aspx_data_sig (framing.num_env envelopes) and aspx_data_noise
    // (framing.num_noise envelopes).
    std::array<AspxEnvelope, kAspxMaxSignalEnvelopes> sig{};
    std::array<AspxEnvelope, kAspxMaxNoiseEnvelopes> noise{};
};

// aspx_data_1ch(), 4.2.12.2, with aspx_hfgen_iwc_1ch().
struct AspxData1ch {
    std::uint8_t xover_subband_offset = 0;  // this I-frame's, or the last I-frame's
    std::uint8_t num_aspx_timeslots = 0;
    AspxSubbandGroups groups;
    AspxChannel channel;
    bool ah_present = false;
    bool fic_present = false;
    bool tic_present = false;
};

// aspx_data_2ch(), 4.2.12.3, with aspx_hfgen_iwc_2ch().
struct AspxData2ch {
    std::uint8_t xover_subband_offset = 0;
    std::uint8_t num_aspx_timeslots = 0;
    AspxSubbandGroups groups;
    // aspx_balance: when set, channel 1 has channel 0's framing (5.7.6.3.5)
    // and its tna_mode.
    bool balance = false;
    std::array<AspxChannel, 2> channels{};
    bool ah_left = false;
    bool ah_right = false;
    bool fic_present = false;
    bool fic_left = false;
    bool fic_right = false;
    bool tic_present = false;
    bool tic_copy = false;
    bool tic_left = false;
    bool tic_right = false;
};

// What one aspx_data_1ch() or aspx_data_2ch() position of a channel element
// needs from earlier frames to be read. Assign {} to forget it: at a splice,
// and whenever the channel element or its codec mode changes.
struct AspxElementState {
    // aspx_xover_subband_offset is sent only in I-frames (Tables 51 and 52).
    bool have_xover_subband_offset = false;
    std::uint8_t xover_subband_offset = 0;
    // Pseudocode 76's previous_stop_pos per channel, held as previous_stop_pos
    // - num_aspx_timeslots so that the initial value clause 5.7.6.3.3.1 gives
    // it (num_aspx_timeslots) is 0. A VARFIX or VARVAR interval in a
    // non-I-frame starts there, and under aspx_freq_res_mode 2 the length of
    // its first envelope decides how many values that envelope sends.
    std::array<std::int8_t, 2> previous_stop_offset{};
};

// aspx_config(). On a truncated read `out.valid` is false.
[[nodiscard]] ParseResult parse_aspx_config(BitReader& r, AspxConfig& out);

// aspx_data_1ch(b_iframe) and aspx_data_2ch(b_iframe), with b_iframe taken
// from ctx. kMissingIFrame when `config` is not valid, or when a non-I-frame
// finds no aspx_xover_subband_offset in `state`.
[[nodiscard]] ParseResult parse_aspx_data_1ch(BitReader& r, const SubstreamContext& ctx,
                                              const AspxConfig& config, AspxElementState& state,
                                              AspxData1ch& out);
[[nodiscard]] ParseResult parse_aspx_data_2ch(BitReader& r, const SubstreamContext& ctx,
                                              const AspxConfig& config, AspxElementState& state,
                                              AspxData2ch& out);

// Clause 5.7.6.3.1's counts. kInvalidStream when aspx_xover_subband_offset
// leaves no subband group (Pseudocode 68 would index past sbg_master) or
// num_sbg_noise exceeds 5.
[[nodiscard]] ParseResult derive_aspx_subband_groups(const AspxConfig& config,
                                                     int xover_subband_offset,
                                                     AspxSubbandGroups& out);

// Pseudocode 75a: num_qmf_timeslots / num_ts_in_ats for a frame_len_base
// (Tables 189 and 192). 0 for a length those tables do not list.
[[nodiscard]] int aspx_num_timeslots(int frame_len_base) noexcept;

// get_aspx_hcb(), Pseudocode 79. quant_mode 0 selects the _15 signal
// codebooks, 1 the _30 ones; noise codebooks ignore it.
[[nodiscard]] const Codebook& aspx_codebook(AspxDataType data_type, int quant_mode,
                                            AspxStereoMode stereo_mode,
                                            AspxHcbType hcb_type) noexcept;

}  // namespace ac4::detail
