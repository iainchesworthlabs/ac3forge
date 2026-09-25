#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "bit_reader.hpp"
#include "huffman_codebook.hpp"
#include "syntax/context.hpp"

// Advanced joint object coding's syntax, ETSI TS 103 190-2 V1.3.1 clause
// 6.2.5 - ajoc(), ajoc_ctrl_info(), ajoc_data(), ajoc_data_point_info() and
// ajoc_huff_data() - and the two elements audio_data_ajoc() reads beside it,
// ajoc_dmx_de_data() (6.2.3.5) and ajoc_bed_info() (6.2.3.6). Semantics: clause
// 6.3.6.
//
// Syntax only: the Huffman indices as read. Differential decoding,
// dequantisation and the reconstruction are src/ac4core/src/ajoc's and
// src/ac4dec/src/pcm/objects.cpp's.

namespace ac4::detail {

inline constexpr int kMaxAjocDecorr = 7;       // ajoc_num_decorr is 3 bits
inline constexpr int kMaxAjocBands = 23;       // Table 78
inline constexpr int kMaxAjocDataPoints = 2;   // 5.7.3.4: "0, 1 or 2"
inline constexpr int kMaxAjocDmxSignals = 16;  // n_fullband_dmx_signals_minus1 is 4 bits

// ajoc_huff_data()'s data_type, quant_select (Table 79: 0 fine, 1 coarse) and
// hcb_type.
enum class AjocDataType : std::uint8_t { kDry, kWet };
enum class AjocHcbType : std::uint8_t { kF0, kDf, kDt };

// Pseudocode 27, get_ajoc_hcb(): AJOC_HCB_<data_type>_<quant>_<hcb_type>.
[[nodiscard]] const Codebook& ajoc_codebook(AjocDataType data_type, int quant_select,
                                            AjocHcbType hcb_type) noexcept;

// Table 78: ajoc_num_bands by ajoc_num_bands_code.
[[nodiscard]] int ajoc_num_bands(int code) noexcept;

// One ajoc_huff_data(): diff_type (0 frequency, 1 time differential) and the
// codebook index of each band, as huff_decode() returns it.
struct AjocHuffData {
    std::uint8_t diff_type = 0;
    std::array<std::uint16_t, kMaxAjocBands> index{};
};

// ajoc_ctrl_info()'s per-object part.
struct AjocObjectConfig {
    bool present = false;                                // ajoc_object_present[o]
    int num_bands_code = 0;                              // ajoc_num_bands_code[o]
    int num_bands = 0;                                   // Table 78
    int quant_select = 0;                                // ajoc_quant_select[o]
    int sparse_select = 0;                               // ajoc_sparse_select[o]
    std::array<bool, kMaxAjocDmxSignals> dry_present{};  // ajoc_mix_mtx_dry_present[o][ch]
    std::array<bool, kMaxAjocDecorr> wet_present{};      // ajoc_mix_mtx_wet_present[o][de]
};

// ajoc(num_dmx_signals, num_umx_signals).
struct AjocData {
    int num_dmx = 0;
    int num_umx = 0;
    int num_decorr = 0;
    std::array<bool, kMaxAjocDecorr> decorr_enable{};
    int num_dpoints = 0;
    std::array<int, kMaxAjocDataPoints> start_pos{};
    std::array<int, kMaxAjocDataPoints> ramp_len{};  // ajoc_ramp_len_minus1 + 1
    bool b_nodt = false;
    std::vector<AjocObjectConfig> objects;  // num_umx of them
    // mix_mtx_dry[o][dp][ch] and mix_mtx_wet[o][dp][de], where sent; an entry
    // not sent (an absent object, or a sparse one's element not present) has
    // `sent` false.
    struct Entry {
        bool sent = false;
        AjocHuffData data{};
    };
    std::vector<Entry> dry;  // [(o * kMaxAjocDataPoints + dp) * num_dmx + ch]
    std::vector<Entry> wet;  // [(o * kMaxAjocDataPoints + dp) * kMaxAjocDecorr + de]

    [[nodiscard]] const Entry& dry_at(int o, int dp, int ch) const noexcept {
        return dry[static_cast<std::size_t>((o * kMaxAjocDataPoints + dp) * num_dmx + ch)];
    }
    [[nodiscard]] const Entry& wet_at(int o, int dp, int de) const noexcept {
        return wet[static_cast<std::size_t>((o * kMaxAjocDataPoints + dp) * kMaxAjocDecorr + de)];
    }
};

[[nodiscard]] ParseResult parse_ajoc(BitReader& r, int num_dmx_signals, int num_umx_signals,
                                     AjocData& out);

// ajoc_dmx_de_data(num_dmx_signals, num_umx_signals) and what it carries from
// frame to frame: the configuration, which gives num_dlg_obj, and the
// coefficients b_keep_dmx_de_coeffs keeps.
struct AjocDmxDeConfig {
    int de_max_gain = 0;
    std::vector<std::uint8_t> de_main_dlg_flag;  // num_umx of them
    int num_dlg_obj = 0;                         // Pseudocode 28
};

struct AjocDmxDeState {
    std::optional<AjocDmxDeConfig> config;
    // de_dlg_dmx_coeff in fifteenths (Table 82), [dlg * num_dmx + dmxo].
    std::vector<std::uint8_t> coeff;
    bool coeff_valid = false;
};

struct AjocDmxDeData {
    bool b_dmx_de_cfg = false;
    bool b_keep_dmx_de_coeffs = false;
    bool coeffs_read = false;
    // What is in force for this frame, sent or kept; unset where no
    // configuration is.
    std::optional<AjocDmxDeConfig> config;
    std::vector<std::uint8_t> coeff;  // as AjocDmxDeState::coeff
};

// `b_iframe` is the substream's: an I-frame without b_dmx_de_cfg clears the
// configuration, and b_keep_dmx_de_coeffs is ignored in an I-frame and with
// b_dmx_de_cfg (6.3.6.6.1 and 6.3.6.6.2; src/ac4dec/ERRATA.md, "A-JOC's
// dialogue enhancement data across frames").
[[nodiscard]] ParseResult parse_ajoc_dmx_de_data(BitReader& r, int num_dmx_signals,
                                                 int num_umx_signals, bool b_iframe,
                                                 AjocDmxDeState& state, AjocDmxDeData& out);

// ajoc_bed_info(), 6.2.3.6.
struct AjocBedInfo {
    bool b_obj_without_bed_info_present = false;
    int num_obj_with_bed_render_info = 0;
};

[[nodiscard]] ParseResult parse_ajoc_bed_info(BitReader& r, AjocBedInfo& out);

}  // namespace ac4::detail
