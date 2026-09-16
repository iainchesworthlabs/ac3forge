#pragma once

#include <array>
#include <cstdint>

#include "bit_reader.hpp"
#include "huffman.hpp"
#include "syntax/context.hpp"

// Advanced coupling (A-CPL) syntax: ETSI TS 103 190-1 V1.4.1 clause 4.2.13,
// semantics 4.3.11, with the parameter band mapping of clause 5.7.7.2 (Table
// 197) that acpl_config_1ch() needs. Part 2 (TS 103 190-2 V1.3.1) Table 48
// uses these elements unchanged for bitstream_version 2.
//
// Nothing is dequantised: parameter values stay Huffman codebook indices,
// before cb_off, with the diff_type that says how clause 5.7.7.7 decodes them.

namespace ac4::detail {

inline constexpr int kAcplMaxParamBands = 15;  // Table 143
inline constexpr int kAcplMaxParamSets = 2;    // Table 146

// acpl_1ch_mode (Table 142), which the channel element hands
// acpl_config_1ch(). The helper is never read from the bitstream, so these
// enumerators do not follow the table's numbering (0 full, 1 partial).
enum class AcplConfigKind : std::uint8_t { kPartial, kFull };

// The arguments of get_acpl_hcb() (Pseudocode 8).
enum class AcplDataType : std::uint8_t { kAlpha, kBeta, kBeta3, kGamma };
enum class AcplHcbType : std::uint8_t { kF0, kDf, kDt };

// acpl_config_1ch(acpl_1ch_mode), 4.2.13.1. Sent in I-frames.
struct AcplConfig1ch {
    bool valid = false;  // set by parse_acpl_config_1ch(); false: no I-frame has sent one
    AcplConfigKind kind = AcplConfigKind::kFull;
    std::uint8_t num_param_bands_id = 0;
    std::uint8_t num_param_bands = 15;  // Table 143
    std::uint8_t quant_mode = 0;        // Table 144: 0 fine, 1 coarse
    // acpl_qmf_band: acpl_qmf_band_minus1 + 1 for PARTIAL, else 0.
    std::uint8_t qmf_band = 0;
    // acpl_param_band = sb_to_pb(acpl_qmf_band): the first parameter band the
    // data elements send.
    std::uint8_t param_band = 0;
};

// acpl_config_2ch(), 4.2.13.2. Sent in I-frames. acpl_qmf_band and
// acpl_param_band are 0.
struct AcplConfig2ch {
    bool valid = false;  // set by parse_acpl_config_2ch()
    std::uint8_t num_param_bands_id = 0;
    std::uint8_t num_param_bands = 15;  // Table 143
    std::uint8_t quant_mode_0 = 0;      // alpha and beta (4.3.11.2.2)
    std::uint8_t quant_mode_1 = 0;      // gamma (4.3.11.2.3)
};

// acpl_framing_data(), 4.2.13.5.
struct AcplFraming {
    std::uint8_t interpolation_type = 0;  // Table 145: 0 smooth, 1 steep
    std::uint8_t num_param_sets_cod = 0;
    std::uint8_t num_param_sets = 1;      // Table 146: acpl_num_param_sets_cod + 1
    // Sent for steep interpolation only, one per parameter set.
    std::array<std::uint8_t, kAcplMaxParamSets> param_timeslot{};
};

// One acpl_huff_data(), 4.2.13.7.
struct AcplParamSet {
    // 0 DIFF_FREQ: huff_index[start_band] from the F0 codebook, the bands
    // above it from DF. 1 DIFF_TIME: every band from DT.
    std::uint8_t diff_type = 0;
    // a_huff_data[i] for start_band <= i < data_bands, as codebook indices
    // before cb_off. The bands below start_band are not sent and stay 0.
    std::array<std::uint16_t, kAcplMaxParamBands> huff_index{};
};

// One acpl_ec_data(), 4.2.13.6: framing.num_param_sets parameter sets.
struct AcplParams {
    AcplDataType data_type = AcplDataType::kAlpha;
    std::uint8_t quant_mode = 0;  // the codebook's quantisation: 0 fine, 1 coarse
    std::array<AcplParamSet, kAcplMaxParamSets> sets{};
};

// acpl_data_1ch(), 4.2.13.3.
struct AcplData1ch {
    AcplFraming framing;
    std::uint8_t num_bands = 0;   // acpl_num_param_bands
    std::uint8_t start_band = 0;  // acpl_param_band
    AcplParams alpha1;
    AcplParams beta1;
};

// acpl_data_2ch(), 4.2.13.4, in the syntax's order.
struct AcplData2ch {
    AcplFraming framing;
    std::uint8_t num_bands = 0;   // acpl_num_param_bands
    std::uint8_t start_band = 0;  // acpl_param_band, 0 for acpl_config_2ch()
    std::array<AcplParams, 2> alpha{};  // acpl_alpha1, acpl_alpha2
    std::array<AcplParams, 2> beta{};   // acpl_beta1, acpl_beta2
    AcplParams beta3;
    std::array<AcplParams, 6> gamma{};  // acpl_gamma1 to acpl_gamma6
};

// acpl_config_1ch(acpl_1ch_mode) and acpl_config_2ch(). On a truncated read
// `out.valid` is false.
[[nodiscard]] ParseResult parse_acpl_config_1ch(BitReader& r, AcplConfigKind kind,
                                                AcplConfig1ch& out);
[[nodiscard]] ParseResult parse_acpl_config_2ch(BitReader& r, AcplConfig2ch& out);

// acpl_data_1ch() and acpl_data_2ch(). kMissingIFrame when `config` is not
// valid. The syntax needs nothing from `ctx`; it is taken for symmetry with
// the other element parsers.
[[nodiscard]] ParseResult parse_acpl_data_1ch(BitReader& r, const SubstreamContext& ctx,
                                              const AcplConfig1ch& config, AcplData1ch& out);
[[nodiscard]] ParseResult parse_acpl_data_2ch(BitReader& r, const SubstreamContext& ctx,
                                              const AcplConfig2ch& config, AcplData2ch& out);

// sb_to_pb(), Table 197: the parameter band of QMF subband `qmf_subband` (0 to
// 63) when there are `num_param_bands` (15, 12, 9 or 7) of them. -1 for
// anything else.
[[nodiscard]] int acpl_sb_to_pb(int num_param_bands, int qmf_subband) noexcept;

// get_acpl_hcb(), Pseudocode 8. quant_mode is the acpl_quant_mode value: 0
// selects the FINE codebooks, 1 the COARSE ones (Table 144).
[[nodiscard]] const Codebook& acpl_codebook(AcplDataType data_type, int quant_mode,
                                            AcplHcbType hcb_type) noexcept;

}  // namespace ac4::detail
