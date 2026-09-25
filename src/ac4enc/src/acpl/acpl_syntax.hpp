#pragma once

#include <array>
#include <cstddef>
#include <vector>

#include "bit_writer.hpp"

// The A-CPL syntax, written: ETSI TS 103 190-1 V1.4.1 Tables 59 to 65
// (acpl_config_1ch, acpl_config_2ch, acpl_data_1ch, acpl_data_2ch and the
// elements they call), transcribed for writing. The decoder's reader
// (src/ac4dec/src/syntax/acpl.cpp) and the Python parser are transcriptions
// of their own; the three traces agree record for record.
//
// The values here are the syntax's: each parameter set's values are what
// huff_decode_diff() returns for them (4.3.10.8.3), the codeword's index less
// its codebook's cb_off, from acpl_param_band up.

namespace ac4::detail {

// acpl_config_1ch(acpl_1ch_mode), Table 59: PARTIAL in ASPX_ACPL_1, FULL in
// ASPX_ACPL_2.
struct AcplConfig1chFields {
    bool partial = false;
    int num_param_bands_id = 0;  // Table 143: 15, 12, 9 or 7 bands
    int quant_mode = 0;          // Table 144: 0 fine, 1 coarse
    int qmf_band = 1;            // acpl_qmf_band, 1 to 8, sent in PARTIAL mode
};

// acpl_config_2ch(), Table 60.
struct AcplConfig2chFields {
    int num_param_bands_id = 0;
    int quant_mode_0 = 0;  // alpha, beta and beta3
    int quant_mode_1 = 0;  // gamma
};

// acpl_framing_data(), Table 63.
struct AcplFramingFields {
    int interpolation_type = 0;  // 0 smooth, 1 steep
    int num_param_sets = 1;      // 1 or 2
    std::array<int, 2> param_timeslot{};  // steep only, one per set
};

// One acpl_huff_data(), Table 65.
struct AcplSetFields {
    int diff_type = 0;        // 0 DIFF_FREQ, 1 DIFF_TIME
    std::vector<int> values;  // bands acpl_param_band to acpl_num_param_bands - 1
};

// One acpl_ec_data(), Table 64: a set per parameter set.
using AcplParamFields = std::vector<AcplSetFields>;

// acpl_data_1ch(), Table 61.
struct AcplData1chFields {
    AcplFramingFields framing;
    AcplParamFields alpha1;
    AcplParamFields beta1;
};

// acpl_data_2ch(), Table 62.
struct AcplData2chFields {
    AcplFramingFields framing;
    std::array<AcplParamFields, 2> alpha;  // acpl_alpha1, acpl_alpha2
    std::array<AcplParamFields, 2> beta;   // acpl_beta1, acpl_beta2
    AcplParamFields beta3;
    std::array<AcplParamFields, 6> gamma;  // acpl_gamma1 to acpl_gamma6
};

// The parameters' kinds, which choose their codebooks (Pseudocode 8).
enum class AcplKind : std::uint8_t { kAlpha, kBeta, kBeta3, kGamma };

// Table 143 and acpl_param_band (Table 59): the bands a data element sends.
[[nodiscard]] int acpl_num_param_bands(int num_param_bands_id) noexcept;
[[nodiscard]] int acpl_param_band(const AcplConfig1chFields& config) noexcept;

void write_acpl_config_1ch(BitWriter& w, const AcplConfig1chFields& config);
void write_acpl_config_2ch(BitWriter& w, const AcplConfig2chFields& config);
void write_acpl_data_1ch(BitWriter& w, const AcplConfig1chFields& config, const AcplData1chFields& data);
void write_acpl_data_2ch(BitWriter& w, const AcplConfig2chFields& config, const AcplData2chFields& data);

// Whether huff_decode_diff() can return `value` for band `band` of a set of
// `kind` at `quant_mode`: the F0 codebook's for the first band along
// frequency, the DF or DT codebook's otherwise.
[[nodiscard]] bool acpl_codable(AcplKind kind, int quant_mode, int diff_type, bool first_band, int value) noexcept;

// The bits one acpl_huff_data() takes: diff_type and its codewords. Every
// value must be codable.
[[nodiscard]] std::size_t acpl_set_bits(AcplKind kind, int quant_mode, const AcplSetFields& set) noexcept;

}  // namespace ac4::detail
