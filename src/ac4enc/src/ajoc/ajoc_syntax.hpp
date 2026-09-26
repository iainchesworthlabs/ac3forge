#pragma once

#include <array>
#include <cstddef>
#include <vector>

#include "bit_writer.hpp"

// The A-JOC syntax, written: ETSI TS 103 190-2 V1.3.1 clause 6.2.5, ajoc() with
// ajoc_ctrl_info(), ajoc_data_point_info(), ajoc_data() and ajoc_huff_data(),
// and clause 6.2.3.5's ajoc_dmx_de_data() and 6.2.3.6's ajoc_bed_info(), which
// audio_data_ajoc() reads around it. Transcribed for writing, separate from the
// decoder's reader (src/ac4dec/src/syntax/ajoc.cpp) and the Python parser; the
// traces agree record for record.
//
// The values are the syntax's: each ajoc_huff_data()'s are what it returns, the
// first band's the F0 codeword's index (huff_decode()) under DIFF_FREQ, and every
// other band's, and every band's under DIFF_TIME, the codeword's index less its
// codebook's cb_off (huff_decode_diff()).

namespace ac4::detail {

// One ajoc_huff_data(), 6.2.5.5.
struct AjocSetFields {
    int diff_type = 0;        // 0 DIFF_FREQ, 1 DIFF_TIME; 0 wherever b_dfonly holds
    std::vector<int> values;  // bands 0 to ajoc_num_bands - 1
};

// One upmix object's ajoc_ctrl_info() and ajoc_data().
struct AjocObjectFields {
    bool present = true;     // ajoc_object_present
    int num_bands_code = 0;  // ajoc_num_bands_code, Table 78
    int quant_select = 0;    // ajoc_quant_select: 0 fine, 1 coarse
    bool sparse = false;     // ajoc_sparse_select
    // With `sparse`: ajoc_mix_mtx_dry_present per downmix signal, and
    // ajoc_mix_mtx_wet_present per decorrelator, sent for an enabled one.
    std::vector<int> dry_present;
    std::vector<int> wet_present;
    // Per data point, a set per downmix signal and per decorrelator; a sparse
    // object's absent ones are skipped (their entries are ignored).
    std::vector<std::vector<AjocSetFields>> dry;
    std::vector<std::vector<AjocSetFields>> wet;
};

// ajoc(num_dmx_signals, num_umx_signals), 6.2.5.1.
struct AjocFields {
    std::vector<int> decorr_enable;     // ajoc_num_decorr of them, 0 to 7
    int num_dpoints = 1;                // 0 to 2
    std::array<int, 2> start_pos{};     // ajoc_start_pos, 0 to 31
    std::array<int, 2> ramp_len{1, 1};  // ajoc_ramp_len_minus1 + 1, 1 to 64
    bool b_nodt = true;
    std::vector<AjocObjectFields> objects;  // num_umx_signals of them
};

// ajoc_dmx_de_data(num_dmx_signals, num_umx_signals), 6.2.3.5.
struct AjocDmxDeFields {
    bool cfg = true;            // b_dmx_de_cfg
    bool keep_coeffs = false;   // b_keep_dmx_de_coeffs
    int max_gain = 0;           // de_max_gain
    std::vector<int> dialogue;  // de_main_dlg_flag[], one per upmix object
    // de_dlg_dmx_coeff_idx[dio][dmxo] as the coefficient in fifteenths, 0 to
    // 15 (Table 82), for the dialogue objects in turn; written where
    // b_keep_dmx_de_coeffs is 0.
    std::vector<int> coeff;
};

// Table 78.
[[nodiscard]] int ajoc_band_count(int num_bands_code) noexcept;

// Whether ajoc_huff_data() can return `value` for a band: of `wet` or dry
// data, at `quant_select`, as the first band of DIFF_FREQ (`first`) or
// another.
[[nodiscard]] bool ajoc_codable(bool wet, int quant_select, int diff_type, bool first,
                                int value) noexcept;

void write_ajoc(BitWriter& w, int num_dmx_signals, const AjocFields& fields);

// The coefficients are written for the dialogue objects `dialogue` names, or
// where this frame sends no configuration, `dialogue_objects` of them.
void write_ajoc_dmx_de_data(BitWriter& w, int num_dmx_signals, const AjocDmxDeFields& fields,
                            int dialogue_objects);

// ajoc_bed_info(), 6.2.3.6: b_obj_without_bed_info_present with
// num_obj_with_bed_render_info where `num_obj_with_bed_render_info` is 0 or more.
void write_ajoc_bed_info(BitWriter& w, int num_obj_with_bed_render_info);

}  // namespace ac4::detail
