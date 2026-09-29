#pragma once

#include <array>
#include <cstddef>
#include <vector>

#include "bit_writer.hpp"

// The A-JCC syntax, written: ETSI TS 103 190-2 V1.3.1 clause 6.2.6,
// ajcc_data() with b_5fronts 0 (the 7.X.4 channel modes) and the elements it
// calls, transcribed for writing. The decoder's reader
// (src/ac4dec/src/syntax/ajcc.cpp) and the Python parser are transcriptions of
// their own; the traces agree record for record.
//
// The values here are the syntax's: each parameter set's values are what
// huff_decode_diff() returns for them, the codeword's index less its
// codebook's cb_off, for every band.

namespace ac4::detail {

// ajcc_framing_data(), 6.2.6.2.
struct AjccFramingFields {
    int interpolation_type = 0;           // 0 smooth, 1 steep
    int num_param_sets = 1;               // 1 or 2
    std::array<int, 2> param_timeslot{};  // steep only, one per set
};

// One ajcc_huff_data(), 6.2.6.4.
struct AjccSetFields {
    int diff_type = 0;        // 0 DIFF_FREQ, 1 DIFF_TIME; always 0 under b_no_dt
    std::vector<int> values;  // bands 0 to ajcc_num_bands - 1
};

// One ajced(), 6.2.6.3: a set per parameter set of its side's framing.
using AjccParamFields = std::vector<AjccSetFields>;

// ajcc_data(0), 6.2.6.1. `params` in syntax order: alpha1, alpha2, beta1,
// beta2, dry1 to dry4, wet1 to wet6; the first of each pair, and dry1, dry2 and
// wet1 to wet3, the left module's, at the left framing's parameter sets.
struct AjccDataFields {
    bool no_dt = false;
    int num_param_bands_id = 0;  // Table 83: 15, 12, 9 or 7 bands
    int core_mode = 0;           // Table 84
    int qm_ab = 0;               // Table 87: 0 fine, 1 coarse
    int qm_dw = 0;               // Table 88
    std::array<AjccFramingFields, 2> framing{};
    std::array<AjccParamFields, 14> params{};
};

// Table 83.
[[nodiscard]] int ajcc_num_param_bands(int num_param_bands_id) noexcept;

void write_ajcc_data(BitWriter& w, const AjccDataFields& data);

// Whether huff_decode_diff() can return `value` for band `band` of parameter
// `param` (AjccDataFields::params' index) at `quant_mode`.
[[nodiscard]] bool ajcc_codable(std::size_t param, int quant_mode, int diff_type, bool first_band,
                                int value) noexcept;

// The bits one ajcc_huff_data() of parameter `param` takes at `quant_mode`:
// its diff_type where b_no_dt is 0, and its codewords. Every value must be
// codable (ajcc_codable()).
[[nodiscard]] std::size_t ajcc_set_bits(std::size_t param, int quant_mode, bool no_dt,
                                        const AjccSetFields& set) noexcept;

}  // namespace ac4::detail
